// 会话序列化单元测试
// 测试 SessionManager 的 JSON 序列化/反序列化逻辑
#include "SessionManager.h"
#include "DraftStore.h"
#include "TextEncoding.h"
#include "EditorTab.h"
#include "VirtualFileView.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>
#include <QElapsedTimer>
#include <QIODevice>

#include <cstdio>
#include <memory>

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) fprintf(stderr, "  [RUN] %s\n", name)

#define CHECK(cond, msg) \
    do { \
        ++test_count; \
        if (cond) { ++test_passed; fprintf(stderr, "    [PASS] %s\n", msg); } \
        else { fprintf(stderr, "    [FAIL] %s\n", msg); } \
    } while (0)

// 测试用的临时数据目录
static QString testDataDir()
{
    QString dir = QDir::tempPath() + "/test_credential_session";
    QDir().mkpath(dir);
    return dir;
}

// 清理临时目录
static void cleanupTestData(const QString &dir)
{
    QDir d(dir);
    d.removeRecursively();
}

// 创建测试用 session.json 文件
static bool writeTestSessionFile(const QString &path, const QJsonObject &root)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QJsonDocument doc(root);
    f.write(doc.toJson());
    f.close();
    return true;
}

static bool waitForMappedView(EditorTab *tab, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (tab && tab->l2View() && tab->l2View()->isOpen())
            return true;
    }
    return tab && tab->l2View() && tab->l2View()->isOpen();
}

void test_load_empty_session()
{
    TEST("加载不存在的会话文件");
    QString dir = testDataDir() + "_empty";
    SessionManager sm(dir);

    SessionData data = sm.loadSession();
    CHECK(data.tabs.isEmpty(), "不存在的会话应返回空 tabs");
    CHECK(data.activeTab == 0, "activeTab 应为默认值 0");

    cleanupTestData(dir);
}

void test_load_valid_session()
{
    TEST("加载有效的会话文件");
    QString dir = testDataDir() + "_valid";
    SessionManager sm(dir);

    // 构造测试 JSON
    QJsonArray tabsArray;
    QJsonObject tab1;
    tab1["filePath"] = "/test/file1.txt";
    tab1["encoding"] = "UTF-8";
    tab1["syntax"] = 0;
    tab1["cursorLine"] = 10;
    tab1["cursorIndex"] = 5;
    tab1["modified"] = true;
    tab1["autosavePath"] = "/tmp/autosave_0.tmp";
    tabsArray.append(tab1);

    QJsonObject tab2;
    tab2["filePath"] = "";
    tab2["encoding"] = "UTF-8";
    tab2["syntax"] = 1;
    tab2["cursorLine"] = 0;
    tab2["cursorIndex"] = 0;
    tab2["modified"] = false;
    tab2["autosavePath"] = "";
    tabsArray.append(tab2);

    QJsonObject root;
    root["version"] = 1;
    root["tabs"] = tabsArray;
    root["activeTab"] = 1;

    writeTestSessionFile(sm.sessionFilePath(), root);

    SessionData data = sm.loadSession();
    CHECK(data.tabs.size() == 2, "应有 2 个标签");
    CHECK(data.activeTab == 1, "activeTab 应为 1");

    if (data.tabs.size() >= 2) {
        CHECK(data.tabs[0].filePath == "/test/file1.txt", "标签0 路径");
        CHECK(data.tabs[0].cursorLine == 10, "标签0 光标行");
        CHECK(data.tabs[0].modified == true, "标签0 修改状态");
        CHECK(data.tabs[1].filePath.isEmpty(), "标签1 路径应为空");
        CHECK(data.tabs[1].syntax == 1, "标签1 语法类型");
    }

    cleanupTestData(dir);
}

void test_load_corrupted_session()
{
    TEST("加载损坏的会话文件");
    QString dir = testDataDir() + "_corrupt";
    SessionManager sm(dir);

    // 写入无效 JSON
    QFile f(sm.sessionFilePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write("{ invalid json !!! }");
        f.close();
    }

    SessionData data = sm.loadSession();
    CHECK(data.tabs.isEmpty(), "损坏的 JSON 应返回空 tabs");

    cleanupTestData(dir);
}

void test_clear_session()
{
    TEST("清理会话文件");
    QString dir = testDataDir() + "_clear";
    SessionManager sm(dir);

    // 先写入一个有效会话
    QJsonObject root;
    root["version"] = 1;
    QJsonArray tabs;
    QJsonObject tab;
    tab["filePath"] = "/test/clear.txt";
    tabs.append(tab);
    root["tabs"] = tabs;
    root["activeTab"] = 0;
    writeTestSessionFile(sm.sessionFilePath(), root);

    CHECK(QFile::exists(sm.sessionFilePath()), "会话文件应存在");

    sm.clearSession();
    CHECK(!QFile::exists(sm.sessionFilePath()), "清理后会话文件应不存在");

    cleanupTestData(dir);
}

void test_autosave_dir_exists()
{
    TEST("autosave 目录存在");
    QString dir = testDataDir() + "_dir";
    SessionManager sm(dir);
    QDir autosaveDir(sm.autosaveDir());
    CHECK(autosaveDir.exists(), "autosave 目录应存在");

    cleanupTestData(dir);
}

void test_session_tab_default_values()
{
    TEST("SessionTab 默认值");
    SessionTab st;
    CHECK(st.filePath.isEmpty(), "默认 filePath 应为空");
    CHECK(st.syntax == 0, "默认 syntax 应为 0");
    CHECK(st.cursorLine == 0, "默认 cursorLine 应为 0");
    CHECK(st.cursorIndex == 0, "默认 cursorIndex 应为 0");
    CHECK(st.modified == false, "默认 modified 应为 false");
    CHECK(st.autosavePath.isEmpty(), "默认 autosavePath 应为空");
}

void test_session_data_default_values()
{
    TEST("SessionData 默认值");
    SessionData sd;
    CHECK(sd.version == 2, "默认 version 应为 2");
    CHECK(sd.tabs.isEmpty(), "默认 tabs 应为空");
    CHECK(sd.activeTab == 0, "默认 activeTab 应为 0");
}

void test_secure_note_never_enters_plaintext_session()
{
    TEST("加密笔记不进入明文 session/autosave");
    QString dir = testDataDir() + "_secure_boundary";
    SessionManager sm(dir);
    auto secureTab = std::make_unique<EditorTab>();
    const QString marker = QStringLiteral("SECRET-MARKER-MUST-NOT-TOUCH-DISK");
    secureTab->initializeSecureDocument(QUuid::createUuid(), QStringLiteral("机密标题"), marker);
    secureTab->editor()->append(QStringLiteral("\nmodified"));
    secureTab->setModified(true);

    QVector<EditorTab *> tabs{secureTab.get()};
    CHECK(sm.saveSession(tabs, 0), "保存混合会话操作应成功");
    const SessionData loaded = sm.loadSession();
    CHECK(loaded.tabs.size() == 1 && loaded.tabs.first().secureDocument,
          "session.json 仅记录安全标签的非秘密恢复引用");
    if (loaded.tabs.size() == 1) {
        CHECK(!loaded.tabs.first().documentId.isEmpty(), "安全标签保留不透明 DocumentId");
        CHECK(loaded.tabs.first().filePath.isEmpty(), "未命名安全标签不伪造用户文件路径");
    }

    QFile sessionFile(sm.sessionFilePath());
    CHECK(sessionFile.open(QIODevice::ReadOnly), "session.json 可读取验证");
    const QByteArray sessionBytes = sessionFile.readAll();
    CHECK(!sessionBytes.contains(marker.toUtf8()), "session.json 不包含解密正文");
    CHECK(!sessionBytes.contains(QStringLiteral("机密标题").toUtf8()), "session.json 不包含加密标题");
    QDir autosave(sm.autosaveDir());
    CHECK(autosave.entryList(QDir::Files).isEmpty(), "不生成任何加密笔记明文 autosave");

    secureTab.reset();

    cleanupTestData(dir);
}

void test_plain_s1_recovery_is_encrypted_and_restorable()
{
    TEST("普通 S1 恢复快照加密落盘并可恢复");
    QString dir = testDataDir() + "_encrypted_draft";
    SessionManager sm(dir);
    auto source = std::make_unique<EditorTab>();
    const QString marker = QStringLiteral("LOCAL-RECOVERY-SECRET-中文-😀");
    source->editor()->setText(marker);
    source->setModified(true);

    QVector<EditorTab *> tabs{source.get()};
    CHECK(sm.saveSession(tabs, 0, true), "保存 S1 加密恢复会话成功");
    const SessionData session = sm.loadSession();
    CHECK(session.tabs.size() == 1, "会话记录一个普通恢复标签");
    if (session.tabs.size() == 1) {
        const QString path = session.tabs.first().autosavePath;
        CHECK(path.endsWith(".ndraft") && DraftStore::isEncryptedDraft(path),
              "恢复文件使用版本化 .ndraft 格式");
        QFile file(path);
        CHECK(file.open(QIODevice::ReadOnly), "加密恢复文件可读取验证");
        const QByteArray diskBytes = file.readAll();
        CHECK(!diskBytes.contains(marker.toUtf8()), "磁盘字节不包含正文标记");

        auto restored = std::make_unique<EditorTab>();
        CHECK(sm.restoreAutosave(restored.get(), path), "当前 Windows 用户可无提示恢复");
        CHECK(restored->editor()->text() == marker, "恢复后的正文与原文一致");

        if (!diskBytes.isEmpty()) {
            QByteArray damaged = diskBytes;
            damaged[damaged.size() - 1] ^= 0x5a;
            QFile corrupt(path);
            if (corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                corrupt.write(damaged);
                corrupt.close();
            }
            auto rejected = std::make_unique<EditorTab>();
            CHECK(!sm.restoreAutosave(rejected.get(), path), "篡改后的恢复快照被拒绝");
        }
    }
    cleanupTestData(dir);
}

void test_chunked_checkpoint_integrity()
{
    TEST("分块加密检查点往返与篡改拒绝");
    QString dir = testDataDir() + "_chunked_checkpoint";
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath("checkpoint.ndraft");
    QByteArray original(2 * 1024 * 1024 + 137, 'q');
    original.replace(1024 * 1024 - 4, 12, QByteArrayLiteral("BOUNDARY-OK!"));
    QString error;
    CHECK(DraftStore::writeCheckpoint(
              path, int(TextEncoding::Utf8),
              [&original](QIODevice *output, QString *) {
                  return output->write(original) == original.size();
              }, &error), "跨越多个 1 MiB 块的检查点写入成功");
    DraftStore::SnapshotInfo info;
    CHECK(DraftStore::inspect(path, &info, &error)
              && info.kind == DraftStore::PayloadKind::FullCheckpoint
              && info.plaintextBytes == original.size(),
          "检查点头记录正确类型和明文总长度");
    QByteArray restored;
    CHECK(DraftStore::readChunks(
              path, [&restored](const QByteArray &chunk, QString *) {
                  restored += chunk;
                  return true;
              }, &error), "分块检查点可逐块认证读取");
    CHECK(restored == original, "跨块边界恢复内容完全一致");

    QFile file(path);
    QByteArray damaged;
    if (file.open(QIODevice::ReadOnly))
        damaged = file.readAll();
    if (damaged.size() > 256) {
        damaged[damaged.size() / 2] ^= 0x33;
        QFile rewrite(path);
        if (rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            rewrite.write(damaged);
            rewrite.close();
        }
    }
    CHECK(!DraftStore::readChunks(
              path, [](const QByteArray &, QString *) { return true; }, &error),
          "任意密文块被篡改后整个检查点被拒绝");
    original.fill(0);
    restored.fill(0);
    cleanupTestData(dir);
}

void test_binary_hex_patch_recovery_is_encrypted_and_restorable()
{
    TEST("Binary/Hex patch 恢复日志加密落盘并可精确重放");
    QString dir = testDataDir() + "_binary_patch";
    QDir().mkpath(dir);
    const QString sourcePath = QDir(dir).filePath("source.bin");
    QByteArray original(96, '\0');
    original[0] = 'B';
    QFile sourceFile(sourcePath);
    CHECK(sourceFile.open(QIODevice::WriteOnly), "创建 Binary fixture");
    sourceFile.write(original);
    sourceFile.close();

    SessionManager sm(dir);
    auto source = std::make_unique<EditorTab>();
    QString error;
    CHECK(source->openLargeFileL2(sourcePath, &error,
                                  L2Presentation::Hex,
                                  L2ContentKind::Binary),
          "Binary fixture 进入 Hex provider");
    const bool mapped = waitForMappedView(source.get());
    CHECK(mapped, "Binary fixture 异步 mmap 已就绪");
    if (!mapped) {
        source->shutdownL2BeforeClose();
        cleanupTestData(dir);
        return;
    }
    const QByteArray replacement = QByteArray::fromHex("DEADBEEF");
    CHECK(source->l2View()->replaceBytes(20, replacement.size(), replacement, &error),
          "修改 4 个原始字节");
    source->gotoByteOffset(20, replacement.size());
    QVector<EditorTab *> tabs{source.get()};
    CHECK(sm.saveSession(tabs, 0, true), "保存 Binary/Hex 加密恢复会话");

    const SessionData state = sm.loadSession();
    CHECK(state.version == 2 && state.tabs.size() == 1, "读取 v2 会话元数据");
    if (state.tabs.size() == 1) {
        const SessionTab &tabState = state.tabs.first();
        CHECK(tabState.presentation == "hex", "会话保留 Hex 呈现方式");
        CHECK(tabState.cursorByteOffset == 24
                  && tabState.selectionStartByte == 20
                  && tabState.selectionEndByte == 24,
              "会话以 64 位字节范围保存光标和选择");
        DraftStore::SnapshotInfo info;
        CHECK(DraftStore::inspect(tabState.autosavePath, &info, &error)
                  && info.kind == DraftStore::PayloadKind::BinaryPatchJournal,
              "恢复文件声明为 Binary patch journal");
        QFile encrypted(tabState.autosavePath);
        CHECK(encrypted.open(QIODevice::ReadOnly), "Binary 恢复密文可读取验证");
        const QByteArray ciphertext = encrypted.readAll();
        CHECK(!ciphertext.contains(replacement), "恢复密文不包含编辑后的原始字节串");

        auto restored = std::make_unique<EditorTab>();
        const bool restoreAccepted = sm.restoreAutosave(
            restored.get(), tabState.autosavePath, sourcePath);
        CHECK(restoreAccepted,
              "恢复日志可在原文件未变化时重放");
        CHECK(restoreAccepted && waitForMappedView(restored.get()),
              "恢复后的 Binary backing mmap 已就绪");
        CHECK(restored->isBinaryDocument() && restored->viewModeL2() == ViewMode::Hex,
              "恢复后仍是 Binary/Hex 文档");
        CHECK(restored->l2View()->readByteRange(20, 24) == replacement,
              "恢复后的虚拟字节与退出前一致");
        restored->shutdownL2BeforeClose();
    }
    QFile unchanged(sourcePath);
    CHECK(unchanged.open(QIODevice::ReadOnly) && unchanged.readAll() == original,
          "恢复点不改写用户原文件");
    source->shutdownL2BeforeClose();
    cleanupTestData(dir);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    fprintf(stderr, "\n=== SessionManager Tests ===\n\n");

    if (app.arguments().contains(QStringLiteral("--binary-hex-recovery"))) {
        test_binary_hex_patch_recovery_is_encrypted_and_restorable();
        fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
        return (test_passed == test_count) ? 0 : 1;
    }

    test_load_empty_session();
    test_load_valid_session();
    test_load_corrupted_session();
    test_clear_session();
    test_autosave_dir_exists();
    test_session_tab_default_values();
    test_session_data_default_values();
    test_secure_note_never_enters_plaintext_session();
    test_plain_s1_recovery_is_encrypted_and_restorable();
    test_chunked_checkpoint_integrity();
    test_binary_hex_patch_recovery_is_encrypted_and_restorable();

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
