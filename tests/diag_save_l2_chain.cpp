// diag_save_l2_chain.cpp
// 直接模拟 L2 保存完整链路（不含 QWidget，100% 控制台）：
//   1) QFile::map(smalltestfile) → 2) LineIndex 构建 → 3) 做几轮编辑
//   4) 模拟 EditorTab::saveL2As sameFile:
//      a) PatchList::applyAndSave 写 tempPath
//      b) 模拟 unmap（手动 unmap file）
//      c) remove+rename temp -> target
//      d) 重新 map + applyPatch 重开 + 内容校验
#include "PatchList.h"
#include "LineIndex.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QString>
#include <QTextStream>

#include <atomic>
#include <cstdio>
#include <cstdlib>

static QFile* g_logFile = nullptr;
static QTextStream* g_logStream = nullptr;

static void ensureLog() {
    if (g_logFile) return;
    static QFile f(QDir::tempPath() + QStringLiteral("/diag_save_l2_chain.log"));
    f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    g_logFile = &f;
    static QTextStream ts(&f);
    g_logStream = &ts;
}
static void flushLogs() { ensureLog(); g_logStream->flush(); g_logFile->flush(); }

static int g_pass = 0, g_fail = 0;
static void report(bool cond, const char* name, const QString& detail = QString()) {
    ensureLog();
    if (cond) { ++g_pass; *g_logStream << "  [PASS] " << name << Qt::endl; }
    else       { ++g_fail; *g_logStream << "  [FAIL] " << name;
        if (!detail.isEmpty()) *g_logStream << " :: " << detail;
        *g_logStream << Qt::endl; }
    flushLogs();
}
#define DIAG_LOG(...) do { ensureLog(); char _buf[2048]; std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); *g_logStream << QString::fromLocal8Bit(_buf) << Qt::endl; flushLogs(); } while(0)

static QString findSmallTestFile() {
    const QStringList candidates = {
        QStringLiteral("F:/projects/smalltestfile"),
        QStringLiteral("F:/projects/credentials/smalltestfile"),
        QStringLiteral("smalltestfile"),
        QStringLiteral("../smalltestfile"),
    };
    for (const QString& p : candidates) {
        if (QFileInfo::exists(p)) return QFileInfo(p).absoluteFilePath();
    }
    QString env = QProcessEnvironment::systemEnvironment().value("SMALLTESTFILE");
    if (!env.isEmpty() && QFileInfo::exists(env)) return QFileInfo(env).absoluteFilePath();
    return {};
}
static QString tempDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe-savediag-") +
                QString::number(QDateTime::currentSecsSinceEpoch());
    QDir().mkpath(d);
    return d;
}
static QByteArray readWhole(const QString& path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ensureLog();
    DIAG_LOG("=== diag_save_l2_chain 保存链路诊断 (纯控制台，不含 QWidget) ===");

    QString inPath = findSmallTestFile();
    if (inPath.isEmpty()) {
        DIAG_LOG("  [SKIP] 找不到 smalltestfile");
        return 2;
    }
    QFileInfo fi(inPath);
    DIAG_LOG("  input: %s", qUtf8Printable(inPath));
    DIAG_LOG("  size : %lld bytes", fi.size());

    QString tmp = tempDir();
    DIAG_LOG("  tmpDir: %s", qUtf8Printable(tmp));

    // ============================================================
    // Step 1: 同步 mmap + LineIndex 构建
    // ============================================================
    DIAG_LOG("");
    DIAG_LOG("[Step 1] mmap + LineIndex::buildFromMmap (与 L2 打开后状态一致)");
    QFile origFile(inPath);
    if (!origFile.open(QIODevice::ReadOnly)) { report(false, "origFile.open"); return 1; }
    qint64 size = origFile.size();
    uchar* base = origFile.map(0, size);
    if (!base) { report(false, "origFile.map"); return 1; }
    LineIndex idx;
    std::atomic<bool> cancel{false};
    int progCalls = 0;
    auto prog = [&](qint64 d, qint64 t){ ++progCalls; };
    bool buildOk = idx.buildFromMmap(base, size, prog, &cancel, 0);
    report(buildOk, "buildFromMmap");
    DIAG_LOG("    totalLines=%lld", idx.totalLines());
    report(idx.totalLines() > 1500000, "totalLines > 1.5M",
           QStringLiteral("actual=%1").arg(idx.totalLines()));

    // ============================================================
    // Step 2: PatchList 做 3 次编辑（模拟真实用户 GUI 操作）
    //   insert @ pos 0 + insert @ pos 50 + remove [20..24]
    // ============================================================
    DIAG_LOG("");
    DIAG_LOG("[Step 2] PatchList 做 3 次改动 (insert/insert/remove)");
    PatchList pl;
    QString perr;
    bool ok = true;
    ok = ok && pl.insert(0, QByteArrayLiteral("__HEAD_TAG__"), &perr);
    ok = ok && pl.insert(50, QByteArrayLiteral("<mid>"), &perr);
    // remove @20, len 5
    QByteArray old20_24(size_t(5), Qt::Uninitialized);
    // 读真实 bytes
    for (int i = 0; i < 5 && (quint64(20 + i) < quint64(size)); ++i)
        old20_24[i] = char(base[20 + i]);
    ok = ok && pl.removeWithSnapshot(20, 5, old20_24, &perr);
    report(ok, "3 次 PatchList 编辑均成功", perr);

    qint64 virtualSize = pl.virtualSize(size);
    DIAG_LOG("    原始 size=%lld  →  虚拟 size=%lld", size, virtualSize);

    // ============================================================
    // Step 3: applyAndSave(tempPath) — 捕获 errMsg ！
    // ============================================================
    DIAG_LOG("");
    DIAG_LOG("[Step 3] applyAndSave 写 tempPath（流式 4MB 块，含进度 + cancel 原子）");
    QString savedTmp = tmp + QStringLiteral("/saved.bin");
    cancel.store(false);
    QString saveErr;
    int progSave = 0;
    auto saveProg = [&](qint64 d, qint64 t) {
        ++progSave;
        if (progSave == 1 || progSave % 20 == 0 || d == t)
            DIAG_LOG("    [progress] %lld/%lld  %.1f%%", d, t, t>0?100.0*double(d)/double(t):0.0);
    };
    bool sOk = pl.applyAndSave(base, size, savedTmp, saveProg, &cancel, &saveErr);
    report(sOk, "applyAndSave -> savedTmp", saveErr);
    report(QFileInfo::exists(savedTmp), "savedTmp 存在于磁盘");
    if (sOk) {
        qint64 dSize = QFileInfo(savedTmp).size();
        report(dSize == virtualSize, "savedTmp.size() == virtualSize",
               QStringLiteral("disk=%1 vSize=%2").arg(dSize).arg(virtualSize));
    }
    report(progSave > 0, "进度回调调用次数 > 0",
           QStringLiteral("called=%1").arg(progSave));

    // 内容比对：读取磁盘 vs 直接 patches.renderRange(0, virtualSize)
    if (sOk) {
        DIAG_LOG("    磁盘全量读 vs renderRange 全量渲染 byte-by-byte 对比...");
        QByteArray diskAll = readWhole(savedTmp);
        QByteArray virtAll = pl.renderRange(base, size, 0, virtualSize);
        report(diskAll.size() == virtualSize, "diskAll.size() == virtualSize",
               QStringLiteral("%1 vs %2").arg(diskAll.size()).arg(virtualSize));
        report(diskAll == virtAll, "diskAll bytes == renderRange all bytes");
        QByteArray dHead = diskAll.left(64);
        QByteArray vHead = virtAll.left(64);
        DIAG_LOG("    磁盘前64B: %s", qUtf8Printable(QString::fromUtf8(dHead).toHtmlEscaped()));
        DIAG_LOG("    虚拟前64B: %s", qUtf8Printable(QString::fromUtf8(vHead).toHtmlEscaped()));

        // head 精确比对
        report(dHead == vHead, "前64字节一致",
               QStringLiteral("disk:%1 | virt:%2")
                   .arg(QString::fromUtf8(dHead))
                   .arg(QString::fromUtf8(vHead)));
    }

    // ============================================================
    // Step 4: 模拟 EditorTab::saveL2As 的 sameFile 流程
    //   temp = target.notepad-safe-save-tmp → unmap → remove target
    //   → rename temp → target → 重新 mmap + LineIndex + 内容校验
    // ============================================================
    DIAG_LOG("");
    DIAG_LOG("[Step 4] 模拟 EditorTab::saveL2As sameFile 重命名流程");
    // 先做一个"假原文件 seed"在 targetReal（模拟原 mmap 的目标文件存在）
    QString targetReal = tmp + QStringLiteral("/target-real.bin");
    {
        QFile seed(targetReal);
        seed.open(QIODevice::WriteOnly);
        seed.write("dummy\n", 6);
    }
    QString tempReal = targetReal + QStringLiteral(".notepad-safe-save-tmp");
    cancel.store(false);
    QString s2err;
    bool s2ok = pl.applyAndSave(base, size, tempReal, {}, &cancel, &s2err);
    report(s2ok, "applyAndSave 到 tempReal (= targetReal + .notepad-safe-save-tmp)", s2err);

    // unmap (模拟 m_l2View->closeFile)
    origFile.unmap(base); base = nullptr;
    origFile.close();
    DIAG_LOG("    已 unmap + 关闭原 QFile。现在 remove+rename: %s -> %s",
           qUtf8Printable(QFileInfo(tempReal).fileName()),
           qUtf8Printable(QFileInfo(targetReal).fileName()));

    bool removed = QFile::remove(targetReal);
    bool renamed = QFile::rename(tempReal, targetReal);
    if (!renamed && removed) {
        // 失败 fallback (与 EditorTab::saveL2As 同样策略)
        bool copied = QFile::copy(tempReal, targetReal);
        if (copied) { QFile::remove(tempReal); renamed = true; }
    }
    report((removed || !QFileInfo::exists(targetReal)) && renamed,
           "remove+rename (或 fallback copy) 成功",
           QStringLiteral("removed=%1 renamed=%2 exists_target=%3")
               .arg(removed?1:0).arg(renamed?1:0).arg(QFileInfo::exists(targetReal)?1:0));
    report(QFileInfo::exists(targetReal), "rename 后 targetReal 存在");

    // 重新 map targetReal → 再构造新 LineIndex → 再 renderRange 对比
    if (renamed) {
        DIAG_LOG("    重新 mmap targetReal，做 round-trip 内容一致性校验...");
        QFile reFile(targetReal);
        if (reFile.open(QIODevice::ReadOnly)) {
            qint64 reSize = reFile.size();
            report(reSize == virtualSize, "重打开文件大小 == virtualSize",
                   QStringLiteral("reSize=%1 vSize=%2").arg(reSize).arg(virtualSize));
            if (reSize == virtualSize) {
                uchar* reBase = reFile.map(0, reSize);
                if (reBase) {
                    LineIndex reIdx;
                    std::atomic<bool> c2{false};
                    reIdx.buildFromMmap(reBase, reSize, {}, &c2, 0);

                    // byte-by-byte 和 virtAll 比对
                    bool eq = true;
                    qint64 firstDiff = -1;
                    for (qint64 i = 0; i < reSize && i < virtualSize; ++i) {
                        uchar cb = reBase[i];
                        QByteArray vb = pl.renderRange(base, 0, i, i+1);  // 此时 base=nullptr, size=0 没关系,下面改用 virtAll
                        Q_UNUSED(vb);
                    }
                    // 简单版：直接与 virtAll 对比
                    QByteArray virtAll2 = readWhole(savedTmp);  // 上一步保存成功的那一份（真值）
                    QByteArray reAll(reinterpret_cast<const char*>(reBase), int(reSize));
                    eq = (reAll == virtAll2);
                    if (!eq) {
                        for (qint64 i = 0; i < reSize; ++i) {
                            if (reAll[int(i)] != virtAll2[int(i)]) { firstDiff = i; break; }
                        }
                    }
                    report(eq, "重打开文件 byte-by-byte == applyAndSave 产物",
                           firstDiff < 0 ? QString() :
                               QStringLiteral("第一个不同字节 @ offset=%1  disk=%2 virt=%3")
                                   .arg(firstDiff)
                                   .arg(quint8(uchar(reAll[int(firstDiff)])), 0, 16)
                                   .arg(quint8(uchar(virtAll2[int(firstDiff)])), 0, 16));
                    DIAG_LOG("    重打开 totalLines = %lld (应与磁盘真实行数一致)",
                           reIdx.totalLines());
                    reFile.unmap(reBase);
                } else {
                    report(false, "重 map targetReal 失败", reFile.errorString());
                }
            }
        } else {
            report(false, "重 open targetReal 失败", reFile.errorString());
        }
    }

    DIAG_LOG("");
    DIAG_LOG("========================================");
    DIAG_LOG("  PASS = %d    FAIL = %d", g_pass, g_fail);
    DIAG_LOG("========================================");
    return g_fail == 0 ? 0 : 1;
}
