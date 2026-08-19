// diag_gui_l2_editops.cpp
// 端到端 GUI 层诊断：真实 new VirtualFileView + open smalltestfile
// 顺序测试：openFile → 行数 → selectAll → 拷贝 bytes → cut → undo → paste →
//            saveAs(tempPath) → 读回比对 → insertAtCursor 验证 errMsg 输出
//
// 需要 QApplication（Qt Widgets）运行时；不实际 show() 窗口但会完整初始化 QWidget。
#include "VirtualFileView.h"
#include "PatchList.h"
#include "LineIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QProcessEnvironment>
#include <QString>
#include <QVector>
#include <QTextStream>

#include <atomic>
#include <cstdio>
#include <cstdlib>

static QFile* g_logFile = nullptr;
static QTextStream* g_logStream = nullptr;

static void ensureLog() {
    if (g_logFile) return;
    static QFile f(QDir::tempPath() + QStringLiteral("/diag_gui_l2_editops.log"));
    f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    g_logFile = &f;
    static QTextStream ts(&f);
    g_logStream = &ts;
}

static void flushLogs() {
    ensureLog();
    g_logStream->flush();
    g_logFile->flush();
}

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
    // 环境变量 fallback
    QString env = QProcessEnvironment::systemEnvironment().value("SMALLTESTFILE");
    if (!env.isEmpty() && QFileInfo::exists(env)) return QFileInfo(env).absoluteFilePath();
    return {};
}

static QString tempDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe-gui-diag-") +
                QString::number(QDateTime::currentSecsSinceEpoch());
    QDir().mkpath(d);
    return d;
}

static QByteArray readWhole(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);  // 必须：QGuiApplication::clipboard(), QWidget 构造

    DIAG_LOG("=== diag_gui_l2_editops 端到端诊断 (L2 VirtualFileView GUI 层) ===");

    QString inPath = findSmallTestFile();
    if (inPath.isEmpty()) {
        DIAG_LOG("  [SKIP] 找不到 smalltestfile (设置 SMALLTESTFILE 环境变量或放到 F:/projects/smalltestfile)");
        return 2;
    }
    QFileInfo fi(inPath);
    DIAG_LOG("  input: %s", qUtf8Printable(inPath));
    DIAG_LOG("  size : %lld bytes", fi.size());

    QString tmp = tempDir();
    DIAG_LOG("  tmpDir: %s", qUtf8Printable(tmp));

    // -------------------------------------------------------------
    // 1. openFile
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 1] openFile smalltestfile (同步 mmap + 全量 LineIndex build)");
    VirtualFileView* v = new VirtualFileView();
    QString openErr;
    bool ok = v->openFileSync(inPath, &openErr);
    report(ok, "openFileSync", openErr);
    if (!ok) {
        delete v; return 1;
    }
    report(v->isOpen(), "isOpen() == true");
    report(v->virtualFileSize() >= fi.size() - 16, "virtualSize ≈ fileSize",
           QStringLiteral("vSize=%1  fileSize=%2").arg(v->virtualFileSize()).arg(fi.size()));
    DIAG_LOG("    lineIndex.totalLines() = %lld", v->totalLines());
    report(v->totalLines() > 1500000, "totalLines > 1.5M (修复CR/CRLF后)",
           QStringLiteral("actual=%1").arg(v->totalLines()));

    // -------------------------------------------------------------
    // 2. selectAll
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 2] selectAll");
    v->selectAll();
    report(v->hasSelection(), "hasSelection() == true");
    qint64 sSel = v->selectionStart();
    qint64 eSel = v->selectionEnd();
    DIAG_LOG("    selection=[%lld, %lld)", sSel, eSel);
    report(sSel == 0 && eSel == v->virtualFileSize(), "sel=[0, virtualSize)",
           QStringLiteral("actual=[%1,%2) vSize=%3")
               .arg(sSel).arg(eSel).arg(v->virtualFileSize()));

    // -------------------------------------------------------------
    // 3. copy (读 selectedBytes -> 剪贴板)
    //    注意：200MB+ 的全选 copy 太费内存和时间，copy 之前先把选区缩到前 1KB
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 3] copy(前1KB) -> QClipboard");
    v->selectBytes(0, 1024);
    QByteArray selBefore = v->selectedBytes();
    DIAG_LOG("    selectedBytes([0,1024)).size() = %lld", qint64(selBefore.size()));
    QGuiApplication::clipboard()->clear(QClipboard::Clipboard);
    app.processEvents();
    v->copy();
    app.processEvents();
    QString cbText = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    QByteArray cbBytes = cbText.toUtf8();
    report(!cbText.isEmpty(), "copy() 后剪贴板非空",
           QStringLiteral("cb size=%1").arg(cbBytes.size()));
    report(qint64(cbBytes.size()) >= qMin(qint64(1024), v->virtualFileSize()) - 64,
           "剪贴板大小与选区一致 (UTF-8 round-trip 可能略小)",
           QStringLiteral("cb=%1 expected<=1024").arg(cbBytes.size()));

    // -------------------------------------------------------------
    // 4. 编辑区真实操作：gotoLine 前几行 + insertAtCursor + undo/redo
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 4] insertAtCursor + undo + redo (真实 GUI API)");
    v->gotoLine(0);
    qint64 before = v->virtualFileSize();
    QString err;
    bool insOk = v->insertAtCursor(QByteArrayLiteral("@@DIAG_TAG@@"), &err);
    report(insOk, "insertAtCursor('@@DIAG_TAG@@')", err);
    report(v->virtualFileSize() == before + 11, "vSize += 11",
           QStringLiteral("before=%1 after=%2").arg(before).arg(v->virtualFileSize()));
    report(v->isModified(), "isModified() == true");

    QString undoErr;
    bool uok = v->undo(&undoErr);
    report(uok, "undo()", undoErr);
    report(v->virtualFileSize() == before, "vSize back to before after undo",
           QStringLiteral("cur=%1 expected=%2").arg(v->virtualFileSize()).arg(before));

    QString redoErr;
    bool rok = v->redo(&redoErr);
    report(rok, "redo()", redoErr);
    report(v->virtualFileSize() == before + 11, "vSize = before+11 after redo");

    // -------------------------------------------------------------
    // 5. cut
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 5] cut() (前1000字节)");
    v->gotoLine(0);
    qint64 fullSize = v->virtualFileSize();
    v->selectBytes(0, 1000);
    QByteArray cut1000 = v->selectedBytes();
    DIAG_LOG("    cut前1000字节: size=%lld", qint64(cut1000.size()));
    QString cutErr;
    bool cutOk = v->cut(&cutErr);
    report(cutOk, "cut()前1000字节", cutErr);
    report(v->virtualFileSize() == fullSize - 1000,
           "vSize 减少 1000",
           QStringLiteral("before=%1 after=%2").arg(fullSize).arg(v->virtualFileSize()));
    app.processEvents();
    QClipboard* cb = QGuiApplication::clipboard();
    QByteArray cutCB = cb->text(QClipboard::Clipboard).toUtf8();
    report(!cutCB.isEmpty(), "cut() 后剪贴板非空",
           QStringLiteral("size=%1").arg(cutCB.size()));
    QString uerr;
    v->undo(&uerr);
    report(v->virtualFileSize() == fullSize, "undo cut 后 vSize 还原",
           QStringLiteral("cur=%1 expected=%2").arg(v->virtualFileSize()).arg(fullSize));

    // -------------------------------------------------------------
    // 6. paste
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 6] paste()");
    v->gotoLine(0);
    qint64 sizeBef = v->virtualFileSize();
    cb->setText(QStringLiteral("[[PASTE_DIAG_1234567890]]"), QClipboard::Clipboard);
    app.processEvents();
    QString pasteErr;
    bool pasteOk = v->paste(&pasteErr);
    report(pasteOk, "paste('[[PASTE_DIAG_...]]')", pasteErr);
    report(v->virtualFileSize() == sizeBef + 23,
           "paste 后 vSize += 23",
           QStringLiteral("before=%1 after=%2").arg(sizeBef).arg(v->virtualFileSize()));
    QByteArray head = v->readByteRange(0, 23);
    report(head == QByteArrayLiteral("[[PASTE_DIAG_1234567890]]"),
           "前23字节 == 粘贴内容",
           QStringLiteral("actual=%1").arg(QString::fromUtf8(head.left(40))));
    v->undo(&uerr);
    report(v->virtualFileSize() == sizeBef, "undo paste 还原 vSize");

    // -------------------------------------------------------------
    // 7. saveAs (GUI 层) —— 捕获 errMsg 打印！
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 7] saveAs(tempPath) [关键：捕获并打印 applyAndSave 内部 errMsg]");
    QString savePath = tmp + QStringLiteral("/smalltestfile-edited.out");
    v->gotoLine(0);
    v->insertAtCursor(QByteArrayLiteral("\n"));
    qint64 vSize = v->virtualFileSize();

    int progressCalled = 0;
    auto progressCb = [&](qint64 done, qint64 total) {
        ++progressCalled;
        if (progressCalled == 1 || progressCalled % 20 == 0 || done == total) {
            DIAG_LOG("    [progress] %lld / %lld  (%.1f%%)",
                   done, total, total>0 ? 100.0*double(done)/double(total) : 0.0);
        }
    };
    std::atomic<bool> cancel{false};
    QString saveErr;
    bool saveOk = v->saveAs(savePath, progressCb, &cancel, &saveErr);
    report(saveOk, "saveAs() 返回 true", saveErr);
    report(QFileInfo::exists(savePath), "out file exists on disk");
    qint64 diskSize = QFileInfo(savePath).size();
    report(diskSize == vSize,
           "out.size() == virtualSize",
           QStringLiteral("disk=%1 vSize=%2").arg(diskSize).arg(vSize));
    report(progressCalled > 0, "save 进度回调至少被调用 1 次",
           QStringLiteral("called=%1").arg(progressCalled));

    if (saveOk) {
        QByteArray diskHead = readWhole(savePath).left(64);
        QByteArray virtHead = v->readByteRange(0, 64);
        report(diskHead == virtHead,
               "saved 前 64 B == v.readByteRange(0,64)",
               QStringLiteral("disk:%1 | virt:%2")
                   .arg(QString::fromUtf8(diskHead))
                   .arg(QString::fromUtf8(virtHead)));
    }

    // -------------------------------------------------------------
    // 8. EditorTab::saveL2As 等价流程（sameFile 模拟）
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 8] saveL2As sameFile 模拟：saveAs -> closeFile -> rename temp->target -> reopen");
    QString targetReal = tmp + QStringLiteral("/target.bin");
    QString tempReal   = tmp + QStringLiteral("/target.bin.notepad-safe-save-tmp");
    {
        QFile seed(targetReal);
        seed.open(QIODevice::WriteOnly);
        seed.write("dummy old content\n", 17);
    }
    cancel.store(false);
    QString err2;
    bool ok2 = v->saveAs(tempReal, {}, &cancel, &err2);
    report(ok2, "saveAs(tempReal)", err2);
    v->closeFile();
    report(!v->isOpen(), "closeFile 后 isOpen() == false");
    bool removed = QFile::remove(targetReal);
    bool renamed = QFile::rename(tempReal, targetReal);
    report((removed || !QFileInfo::exists(targetReal)) && renamed,
           "target 可移除 & tempReal -> target 可重命名",
           QStringLiteral("removed=%1 renamed=%2 exists_after=%3")
               .arg(removed?1:0).arg(renamed?1:0).arg(QFileInfo::exists(targetReal)?1:0));
    report(QFileInfo::exists(targetReal), "rename 后 targetReal 存在");
    QString reopenErr;
    bool rok2 = v->openFileSync(targetReal, &reopenErr);
    report(rok2, "重 open targetReal", reopenErr);
    if (rok2) {
        report(v->virtualFileSize() == vSize,
               "重打开 vSize == 保存前 vSize",
               QStringLiteral("cur=%1 expected=%2").arg(v->virtualFileSize()).arg(vSize));
        v->closeFile();
    }

    // -------------------------------------------------------------
    // 9. Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V 快捷键绑定
    // -------------------------------------------------------------
    DIAG_LOG("");
    DIAG_LOG("[Step 9] 模拟 Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V 快捷键 (QKeyEvent 真实发送)");
    {
        VirtualFileView* v2 = new VirtualFileView();
        QString e2;
        v2->openFileSync(inPath, &e2);

        auto sendKey = [&](Qt::Key k, Qt::KeyboardModifiers mod) {
            QKeyEvent ev(QEvent::KeyPress, k, mod);
            QApplication::sendEvent(v2, &ev);
        };
        sendKey(Qt::Key_A, Qt::ControlModifier);
        report(v2->hasSelection(), "Ctrl+A 后 hasSelection=true（若 FAIL=快捷键未绑定）",
               QStringLiteral("sel=[%1,%2)")
                   .arg(v2->selectionStart()).arg(v2->selectionEnd()));

        v2->selectBytes(0, 16);
        cb->clear(QClipboard::Clipboard);
        app.processEvents();
        sendKey(Qt::Key_C, Qt::ControlModifier);
        app.processEvents();
        QString afterCB = cb->text(QClipboard::Clipboard);
        report(!afterCB.isEmpty(), "Ctrl+C 后剪贴板非空（若 FAIL=快捷键未绑定）",
               QStringLiteral("size=%1").arg(afterCB.size()));

        qint64 sizeBeforeCut = v2->virtualFileSize();
        v2->selectBytes(0, 4);
        sendKey(Qt::Key_X, Qt::ControlModifier);
        report(v2->virtualFileSize() == sizeBeforeCut - 4,
               "Ctrl+X 后 vSize -= 4（若 FAIL=快捷键未绑定）",
               QStringLiteral("before=%1 after=%2")
                   .arg(sizeBeforeCut).arg(v2->virtualFileSize()));

        qint64 sizeBeforePaste = v2->virtualFileSize();
        cb->setText(QStringLiteral("KEYV"), QClipboard::Clipboard);
        app.processEvents();
        sendKey(Qt::Key_V, Qt::ControlModifier);
        report(v2->virtualFileSize() == sizeBeforePaste + 4,
               "Ctrl+V 后 vSize += 4（若 FAIL=快捷键未绑定）",
               QStringLiteral("before=%1 after=%2")
                   .arg(sizeBeforePaste).arg(v2->virtualFileSize()));

        delete v2;
    }

    // 收尾
    delete v;

    DIAG_LOG("");
    DIAG_LOG("========================================");
    DIAG_LOG("  PASS = %d    FAIL = %d", g_pass, g_fail);
    DIAG_LOG("========================================");
    return g_fail == 0 ? 0 : 1;
}
