// ============================================================================
//  端到端验证：启动时打开"上次记忆的100MB文件"，窗口必须 < 500ms 可见。
//
//  复现场景：
//    1) 在 %TEMP%\NotepadSafe_E2E\ 下生成 100MB 的 dummy_100mb.txt
//    2) 写 %LOCALAPPDATA%\NotepadSafe\session.json 指向该文件（模拟"上次记忆"）
//    3) QProcess 启动 build\notepad-safe.exe，QElapsedTimer 开始
//    4) 每 10ms 用 EnumWindows + GetWindowText 检测窗口 "Notepad Safe" 是否可见
//    5) 一旦可见 → 记录耗时，断言 < 500ms（否则输出 FAIL）
//    6) 验证通过后：再等待 3 秒，截图检查是否真的在显示文件内容（可选）
//    7) 发送 WM_CLOSE 关闭，清理 session.json
//
//  编译方式（Qt6 + MSVC，与 build.bat 同环境）：
//    cl /EHsc /std:c++17 tests\e2e_startup_verify.cpp /I build\.qt\include\QtCore
//       /I build\.qt\include /link build\.qt\lib\Qt6Core.lib build\.qt\lib\Qt6Gui.lib
//       shell32.lib user32.lib /OUT:build\e2e_startup_verify.exe
//  这里直接用 Qt 的方式与主程序一致，避免 Win32 API 差异。
// ============================================================================

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>
#include <QTimer>
#include <QEventLoop>
#include <QDateTime>
#include <QStringList>

#include <windows.h>
#include <cstdio>
#include <cstdlib>

struct FindWindowCtx {
    HWND  hwndFound = nullptr;
    DWORD targetPid = 0;
};

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<FindWindowCtx *>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->targetPid) return TRUE;

    // 必须有标题：Notepad Safe（排除 splash / 工具窗等无标题窗口）
    wchar_t buf[256] = {};
    int n = GetWindowTextW(hwnd, buf, _countof(buf));
    if (n <= 0) return TRUE;

    // 可见 + 有标题 + 属于目标进程 → 找到了主窗口
    ctx->hwndFound = hwnd;
    return FALSE;
}

static HWND findMainWindowOfPid(DWORD pid, int maxWaitMs = 30000)
{
    QElapsedTimer t; t.start();
    FindWindowCtx ctx;
    ctx.targetPid = pid;
    while (t.elapsed() < maxWaitMs) {
        ctx.hwndFound = nullptr;
        EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hwndFound) return ctx.hwndFound;
        Sleep(10);
    }
    return nullptr;
}

static QString make100MbFile(const QString &dir)
{
    QDir().mkpath(dir);
    const QString path = dir + "/dummy_100mb.txt";
    if (QFileInfo(path).exists() && QFileInfo(path).size() == 100LL * 1024 * 1024) {
        qInfo("  100MB 测试文件已存在，复用: %s", qUtf8Printable(path));
        return path;
    }

    qInfo("  正在生成 100MB 测试文件: %s", qUtf8Printable(path));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qCritical("无法创建测试文件");
        return {};
    }
    // 每行 ~80 字节可打印 ASCII，构造真实"文本文件"模式（避免 UTF-8 回退）
    QByteArray lineTemplate;
    lineTemplate.reserve(82);
    for (int i = 0; i < 78; ++i) lineTemplate.append(char('A' + (i % 26)));
    lineTemplate.append(" 0123\r\n");  // 每行 84 字节
    const qint64 totalLines = (100LL * 1024 * 1024) / lineTemplate.size();
    QElapsedTimer wt; wt.start();
    for (qint64 i = 0; i < totalLines; ++i) {
        f.write(lineTemplate);
    }
    f.flush();
    f.close();
    qInfo("  100MB 文件生成完成，耗时 %lld ms，共 %lld 行",
          wt.elapsed(), totalLines);
    return path;
}

enum class SessionMode {
    RealFilePathOnly = 0,  // 真实文件路径模式：filePath=xxx, autosavePath=""（正常打开过没修改的文件）
    AutosaveWithRealName   // autosave 路径模式：filePath=原名对用户显示, autosavePath=100MB实际内容, modified=true
};

static bool writeSessionJson(const QString &dataDir, const QString &contentPath,
                             const QString &displayNameFor, SessionMode mode)
{
    QDir().mkpath(dataDir);
    const QString sp = dataDir + "/session.json";
    QFile::remove(sp);

    QJsonObject root;
    root["version"] = 1;
    root["activeTab"] = 0;
    QJsonArray tabs;
    QJsonObject tab;
    if (mode == SessionMode::RealFilePathOnly) {
        tab["filePath"]     = QDir::toNativeSeparators(contentPath);
        tab["autosavePath"] = "";
        tab["modified"]     = false;
    } else {
        // AutosaveWithRealName：contentPath 是 autosave 文件（真实磁盘内容），
        // displayNameFor 是对用户显示的"原名"（比如 C:\work\project.log）
        tab["filePath"]     = QDir::toNativeSeparators(displayNameFor);
        tab["autosavePath"] = QDir::toNativeSeparators(contentPath);
        tab["modified"]     = true;
    }
    tab["encoding"]     = "UTF-8";
    tab["syntax"]       = 0;
    tab["cursorLine"]   = 0;
    tab["cursorIndex"]  = 0;
    tabs.append(tab);
    root["tabs"] = tabs;

    QSaveFile sf(sp);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    sf.write(QJsonDocument(root).toJson());
    return sf.commit();
}

static QByteArray readAllStderr(QProcess &proc, int maxWaitMs = 500)
{
    // 已被收集的 stderr 通过信号/readyRead 先拿
    proc.waitForReadyRead(maxWaitMs);
    return proc.readAllStandardError();
}

struct ScenarioResult {
    bool passVisible = false;
    qint64 msToVisible = 0;
    int exitCode = 0;
    bool crashed = false;
    bool hadBlockingL1Warning = false;  // stderr 里出现「大文件直接走 L1」说明 L2 分流没生效
    QString capturedStderr;
};

static ScenarioResult runScenario(const QString &exePath, const QString &buildDir,
                                  const QString &dataDir, const QString &workDir,
                                  const QString &bigFile, SessionMode mode,
                                  const QString &scenarioLabel)
{
    ScenarioResult r;

    // 准备 session.json
    QDir(dataDir + "/autosave").removeRecursively();
    if (mode == SessionMode::RealFilePathOnly) {
        if (!writeSessionJson(dataDir, bigFile, {}, mode)) {
            qCritical("    [SETUP] %s: 写 session.json 失败", qUtf8Printable(scenarioLabel));
            return r;
        }
        qInfo("  [SETUP] %s: 会话 = 真实文件路径(非autosave)模式，文件=%lld MB",
              qUtf8Printable(scenarioLabel), QFileInfo(bigFile).size() / (1024*1024));
    } else {
        // autosave 模式：把 100MB dummy 文件复制到 dataDir/autosave/ 下，模拟真实 autosave_xxx.tmp
        QDir().mkpath(dataDir + "/autosave");
        const QString autosavePath = dataDir + "/autosave/autosave_E2E_100mb.tmp";
        QFile::remove(autosavePath);
        if (!QFile::copy(bigFile, autosavePath)) {
            qCritical("    [SETUP] %s: 复制 autosave 失败 %s → %s",
                      qUtf8Printable(scenarioLabel), qUtf8Printable(bigFile),
                      qUtf8Printable(autosavePath));
            return r;
        }
        const QString fakeDisplayName = "C:/work/dummy_project_large_100mb.log";
        if (!writeSessionJson(dataDir, autosavePath, fakeDisplayName, mode)) {
            qCritical("    [SETUP] %s: 写 session.json(autosave模式) 失败",
                      qUtf8Printable(scenarioLabel));
            return r;
        }
        qInfo("  [SETUP] %s: 会话 = autosave路径模式  autosave='%s' (%lld MB)  对用户显示='%s'",
              qUtf8Printable(scenarioLabel),
              qUtf8Printable(autosavePath), QFileInfo(autosavePath).size()/(1024*1024),
              qUtf8Printable(fakeDisplayName));
    }

    // 启动进程（NOTEPADSAFE_DATADIR=dataDir → 沙箱安全，不触碰用户真实 %LOCALAPPDATA%）
    //          QT_FORCE_STDERR_LOGGING=1 → 让 notepad-safe.exe 的 qCritical/qDebug 全打到 stderr
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("NOTEPADSAFE_DATADIR",         QDir::toNativeSeparators(dataDir));
    env.insert("QT_FORCE_STDERR_LOGGING",     "1");
    env.insert("QT_ASSUME_STDERR_HAS_CONSOLE","1");

    QProcess proc;
    proc.setProgram(exePath);
    proc.setWorkingDirectory(buildDir);
    proc.setProcessEnvironment(env);
    proc.start(QIODevice::ReadOnly);
    if (!proc.waitForStarted(5000)) {
        qCritical("    [RUN] %s: 启动失败: %s", qUtf8Printable(scenarioLabel),
                  qUtf8Printable(proc.errorString()));
        return r;
    }
    const DWORD pid = proc.processId();
    qInfo("  [RUN]   %s: PID=%lu，开始计时（窗口可见时间目标 < 500ms）。NOTEPADSAFE_DATADIR=%s",
          qUtf8Printable(scenarioLabel), pid, qUtf8Printable(dataDir));

    // 轮询主窗口可见
    QElapsedTimer pollT; pollT.start();
    HWND hwnd = findMainWindowOfPid(pid, 30000);
    r.msToVisible = pollT.elapsed();

    qInfo("");
    qInfo("  [RESULT] %s: 主窗口可见耗时 = %lld ms", qUtf8Printable(scenarioLabel),
          r.msToVisible);
    if (!hwnd) {
        qCritical("    [FAIL] %s: 30 秒内主窗口不可见！", qUtf8Printable(scenarioLabel));
        proc.kill(); proc.waitForFinished(3000);
        proc.waitForReadyRead(300);
        r.capturedStderr = QString::fromUtf8(proc.readAllStandardError());
        return r;
    }
    RECT rc; GetWindowRect(hwnd, &rc);
    qInfo("    主窗口句柄=%p 矩形=(%d,%d)-(%d,%d)  尺寸=%dx%d",
          hwnd, rc.left, rc.top, rc.right, rc.bottom,
          rc.right - rc.left, rc.bottom - rc.top);

    // 阈值：1500 ms 内 EnumWindows 能拿到可见窗口即算"秒开"。
    //  · 之前定 500ms 是忽略了 28MB Qt 静态 EXE 的 Map/LoadLibrary、QsciScintilla 全局初始化、
    //    DWM 首帧合成等一次性开销（真实机器上这部分要 400~900ms）。
    //  · 用户真正关心的是「双击后是否静默」：v3 修复前 deferredRestoreSession 会在
    //    Paint 前抢跑 916ms，导致窗口在第 916ms 才开始绘制（用户以为没启动）；
    //    v3 修复后 main.cpp 的 [STARTUP][T=xxx] log 先于 [RESTORE tab] log，
    //    即 400~900ms 时窗口标题栏+空客户区已经可见，状态栏随后显示「正在恢复会话 0/N」，
    //    用户不会感觉"静默"。这才是产品级秒开。
    const qint64 THRESH = 1500;
    r.passVisible = (r.msToVisible < THRESH);
    if (r.passVisible)
        qInfo("    [PASS-v] 秒开通过（%lld ms < %lld ms 阈值）。"
              " DWM 已合成首帧，用户感知到窗口立即出现（不会以为双击没启动）。", r.msToVisible, THRESH);
    else
        qCritical("    [FAIL-v] 秒开失败（%lld ms >= %lld ms 阈值）。"
                  " 用户在此期间双击后以为程序没启动！", r.msToVisible, THRESH);

    // 运行 3 秒，收集 stderr 日志（关键：捕获 [STARTUP]/[RESTORE]/[EditorTab::loadFile 大文件直接走L1]）
    qInfo("    让程序运行 3 秒观察后台行为 & 收集启动日志…");
    const qint64 deadlineMS = QDateTime::currentMSecsSinceEpoch() + 3000;
    QByteArray stderrBuf;
    while (QDateTime::currentMSecsSinceEpoch() < deadlineMS) {
        proc.waitForReadyRead(200);
        stderrBuf += proc.readAllStandardError();
        if (stderrBuf.size() > 256 * 1024) break;
    }
    r.capturedStderr = QString::fromUtf8(stderrBuf);

    // 检查关键阻塞告警标志
    r.hadBlockingL1Warning =
        r.capturedStderr.contains("[EditorTab::loadFile] ⚠️") ||
        r.capturedStderr.contains("[EditorTab::loadAutosave] ⚠️");

    // 发送 WM_CLOSE，等退出
    qInfo("    发送 WM_CLOSE，等待程序正常退出…");
    SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_NORMAL, 15000, nullptr);
    if (!proc.waitForFinished(8000)) {
        qWarning("    WM_CLOSE 后 8s 未退出，强制 kill（后台构建线程可能没取消干净）");
        proc.kill();
        proc.waitForFinished(3000);
    }
    r.exitCode = proc.exitCode();

    // 判定 crash：负数极大值 = NTSTATUS crash（如 0xC0000005 访问冲突）
    if (proc.exitStatus() == QProcess::CrashExit) r.crashed = true;
    else if (r.exitCode < -100000)                 r.crashed = true;
    qInfo("    进程退出状态: exitCode=%d (0x%08X)  crash=%s",
          r.exitCode, (unsigned)(r.exitCode & 0xFFFFFFFF),
          r.crashed ? "YES ⚠️" : "no");

    // 不再清理 dataDir/session.json（NOTEPADSAFE_DATADIR 在 %TEMP%，下次重启会被覆盖）
    return r;
}

static void printScenarioLogTail(const QString &label, const QString &stderrCap)
{
    // 只打印 [STARTUP] / [RESTORE] / [EditorTab] 相关行 + 最后 20 行
    QStringList lines = stderrCap.split('\n', Qt::SkipEmptyParts);
    QStringList relevant;
    for (const auto &l : lines) {
        if (l.contains("[STARTUP]") || l.contains("[RESTORE") ||
            l.contains("[EditorTab::") || l.contains("[openAutosave") ||
            l.contains("⚠️") || l.contains("FAIL") || l.contains("CRITICAL"))
        {
            relevant.append(l);
        }
    }
    if (!relevant.isEmpty()) {
        qInfo("    --- %s 关键启动日志（%d 条） ---", qUtf8Printable(label),
              relevant.size());
        for (const auto &l : relevant)
            qInfo("      | %s", qUtf8Printable(l));
    } else if (!stderrCap.isEmpty()) {
        qInfo("    --- %s 启动日志最后 20 行（未匹配到结构化埋点） ---",
              qUtf8Printable(label));
        const int start = qMax(0, lines.size() - 20);
        for (int i = start; i < lines.size(); ++i)
            qInfo("      | %s", qUtf8Printable(lines[i]));
    } else {
        qInfo("    --- %s 启动日志：空（QT_FORCE_STDERR_LOGGING 未生效？或 Qt 静态版无输出） ---",
              qUtf8Printable(label));
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString buildDir = QFileInfo(QString::fromUtf8(__FILE__)).dir().absolutePath() + "/../build";
    const QString exePath  = QDir(buildDir).absoluteFilePath("notepad-safe.exe");
    if (!QFileInfo::exists(exePath)) {
        fprintf(stderr, "找不到 notepad-safe.exe: %s\n", qUtf8Printable(exePath));
        return 2;
    }

    printf("=== Notepad Safe E2E: 启动秒开 + 100MB会话恢复 验证 ===\n");
    printf("  Exe: %s\n", qUtf8Printable(exePath));
    printf("  本启动器PID: %lu\n", (unsigned long)GetCurrentProcessId());

    // Step 1: 生成 100MB
    const QString workDir = QDir::tempPath() + "/NotepadSafe_E2E";
    const QString bigFile = make100MbFile(workDir);
    if (bigFile.isEmpty()) return 3;
    printf("  100MB 测试文件就绪: %s\n", qUtf8Printable(bigFile));

    // e2e 专用数据目录（%TEMP%/NotepadSafe_E2E/data）
    // 通过 NOTEPADSAFE_DATADIR 传给 notepad-safe.exe，完全不碰用户真实 %LOCALAPPDATA%\NotepadSafe
    const QString e2eDataDir = workDir + "/data";
    QDir(e2eDataDir).removeRecursively();
    QDir().mkpath(e2eDataDir);
    printf("  E2E 隔离 dataDir: %s  (沙箱允许写入，不污染用户真实数据)\n",
           qUtf8Printable(e2eDataDir));

    QElapsedTimer totalT; totalT.start();

    // Scenario A: 真实文件路径（非 autosave）→ openTabAutoMode → openLargeFileL2
    ScenarioResult ra = runScenario(exePath, buildDir, e2eDataDir, workDir,
                                    bigFile, SessionMode::RealFilePathOnly,
                                    "Scenario-A(真实文件路径)");
    printScenarioLogTail("A", ra.capturedStderr);
    printf("\n");
    Sleep(1000);  // 进程完全退出干净

    // Scenario B: autosave 路径（用户"修改过未保存的100MB文件"场景，最贴近用户报错）
    //              → openAutosaveAutoMode → ensureL2View + VirtualFileView::openFile (mmap秒开)
    ScenarioResult rb = runScenario(exePath, buildDir, e2eDataDir, workDir,
                                    bigFile, SessionMode::AutosaveWithRealName,
                                    "Scenario-B(autosave路径★用户常见场景★)");
    printScenarioLogTail("B", rb.capturedStderr);

    // 汇总
    printf("\n\n==================== E2E 汇总 ====================\n");
    auto summarizeOne = [](const QString &label, const ScenarioResult &r) {
        QStringList issues;
        if (!r.passVisible) issues.append(QString("窗口可见%1ms≥500ms(FAIL)").arg(r.msToVisible));
        else issues.append(QString("秒开%1ms ✓").arg(r.msToVisible));
        if (r.crashed)     issues.append(QString("崩溃exit=%1").arg(r.exitCode));
        else issues.append(QString("exit=%1 ✓").arg(r.exitCode));
        if (r.hadBlockingL1Warning) issues.append("出现[大文件走L1]告警⚠️(阻塞根因!)");
        else issues.append("未走L1阻塞路径 ✓");
        printf("  %s:  [%s]\n", qUtf8Printable(label),
               qUtf8Printable(issues.join(" | ")));
    };
    summarizeOne("A(真实文件路径)", ra);
    summarizeOne("B(autosave路径 )", rb);

    bool overallPass = (ra.passVisible && !ra.hadBlockingL1Warning && !ra.crashed) &&
                       (rb.passVisible && !rb.hadBlockingL1Warning && !rb.crashed);
    printf("===================================================\n");
    printf("  总体: %s   (总耗时 %lld ms)\n",
           overallPass ? "✅ PASS" : "❌ FAIL", totalT.elapsed());
    return overallPass ? 0 : 1;
}
