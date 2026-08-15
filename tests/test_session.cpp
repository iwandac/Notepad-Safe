// 会话序列化单元测试
// 测试 SessionManager 的 JSON 序列化/反序列化逻辑
#include "SessionManager.h"
#include "TextEncoding.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>

#include <cstdio>

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
    CHECK(sd.version == 1, "默认 version 应为 1");
    CHECK(sd.tabs.isEmpty(), "默认 tabs 应为空");
    CHECK(sd.activeTab == 0, "默认 activeTab 应为 0");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    fprintf(stderr, "\n=== SessionManager Tests ===\n\n");

    test_load_empty_session();
    test_load_valid_session();
    test_load_corrupted_session();
    test_clear_session();
    test_autosave_dir_exists();
    test_session_tab_default_values();
    test_session_data_default_values();

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
