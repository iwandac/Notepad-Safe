// ============================================================================
//  E2E GUI 测试：L2 打开大文件后"内容可见"验证
//
//  动机：
//    历史上 3 次出现「打开 L2 什么都不显示」：
//      · VFV paintEvent 自绘被删空 → 内容全灰
//      · Scintilla overlay 被 reparent 后 vp size=0 / singleShot(0) 未布局 → 0x0
//      · QAbstractScrollArea::scrollContentsBy 默认 viewport()->scroll() 把 Scintilla 移走
//    代码层判断（看函数体、变量）全是瞎猜，必须真启动程序、抓窗口像素才能下结论。
//
//  原理：
//    1) 生成 10MB 测试文本（ASCII 行），写 session.json → NOTEPADSAFE_DATADIR
//    2) QProcess 启动 build\notepad-safe.exe（独立数据目录，沙箱安全）
//    3) EnumWindows 轮询主窗口可见 → 记录窗口矩形
//    4) 等待 active window 信号触发时机（文件 open→mmap ready→emit→灌 Scintilla，
//       给 6 秒足够时间，慢机器也能完成 10MB）
//    5) 用 PrintWindow + GetDIBits 抓客户区像素：
//         · 如果 distinctColors < 30（基本都是同一种背景色） → "没内容显示"
//         · 如果像素方差 < 2.0（全均匀色） → "没内容显示"
//         · 其他情况 → PASS
//    6) 同时把抓到的 BMP 写到 %TEMP%\NotepadSafe_E2E\capture_l2.bmp，
//       失败时人眼可以直接打开看画面。
//    7) 发送 WM_CLOSE 关程序，收集 stderr 最后 60 行输出（看 activeWindowChanged、
//       bridgePopulateScintilla、Scintilla setText 埋点）。
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
#include <QDateTime>
#include <QStringList>

#include <windows.h>
#include <windowsx.h>
#include <wingdi.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <unordered_set>

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

    wchar_t buf[256] = {};
    int n = GetWindowTextW(hwnd, buf, _countof(buf));
    if (n <= 0) return TRUE;

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

static QString make40MbFile(const QString &dir)
{
    QDir().mkpath(dir);
    const QString path = dir + "/dummy_40mb.txt";
    // FileTierPolicy S1 上限 = 32 MiB；40 MiB 一定走 S2 mmap 路径
    const qint64 TARGET = 40LL * 1024 * 1024;
    if (QFileInfo(path).exists() && QFileInfo(path).size() == TARGET) {
        qInfo("  40MB 测试文件已存在，复用: %s", qUtf8Printable(path));
        return path;
    }

    qInfo("  正在生成 40MB 测试文件: %s", qUtf8Printable(path));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qCritical("无法创建测试文件");
        return {};
    }
    QByteArray line;
    line.reserve(84);
    for (int i = 0; i < 78; ++i) line.append(char('A' + (i % 26)));
    line.append(" 0123\r\n");  // 每行 84 字节
    const qint64 n = TARGET / line.size();
    QElapsedTimer wt; wt.start();
    for (qint64 i = 0; i < n; ++i) f.write(line);
    // 补齐到 40MB 整
    while (f.size() < TARGET) {
        qint64 miss = TARGET - f.size();
        QByteArray pad(int(qMin<qint64>(miss, line.size())), 'X');
        f.write(pad);
    }
    f.flush(); f.close();
    qInfo("  40MB 文件生成完成，耗时 %lld ms (%lld bytes)", wt.elapsed(), QFileInfo(path).size());
    return path;
}

static bool writeSessionJson(const QString &dataDir, const QString &contentPath)
{
    QDir().mkpath(dataDir);
    const QString sp = dataDir + "/session.json";
    QFile::remove(sp);
    QJsonObject root;
    root["version"] = 1;
    root["activeTab"] = 0;
    QJsonArray tabs;
    QJsonObject tab;
    tab["filePath"]     = QDir::toNativeSeparators(contentPath);
    tab["autosavePath"] = "";
    tab["modified"]     = false;
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

// 抓客户区 HBITMAP。失败返回 NULL。
// 做法：窗口已经由测试置前并保持可见，因此先用 GetDC + BitBlt 抓取
// DWM 实际合成画面。PrintWindow 会要求原生子窗口分别离屏绘制，QScintilla
// 在该路径下可能忽略 sibling dock 的裁剪，产生编辑区覆盖 Dock 的假截图。
// BitBlt 失败时才回退到 PrintWindow(PW_RENDERFULLCONTENT)。
static HBITMAP captureClientArea(HWND hwnd, int *outW, int *outH)
{
    *outW = 0; *outH = 0;
    if (!IsWindow(hwnd)) return nullptr;
    RECT cr;
    if (!GetClientRect(hwnd, &cr)) return nullptr;
    int W = cr.right  - cr.left;
    int H = cr.bottom - cr.top;
    if (W <= 0 || H <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    HDC hdcScreen = GetDC(hwnd);
    if (!hdcScreen) return nullptr;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(hwnd, hdcScreen); return nullptr; }
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, W, H);
    if (!hbm) { DeleteDC(hdcMem); ReleaseDC(hwnd, hdcScreen); return nullptr; }
    auto old = SelectObject(hdcMem, hbm);

    BOOL ok = BitBlt(hdcMem, 0, 0, W, H, hdcScreen, 0, 0,
                     SRCCOPY | CAPTUREBLT);
    if (!ok) {
        // Win8.1+: PW_RENDERFULLCONTENT = 0x00000002。直接用字面值避免版本宏问题。
        ok = PrintWindow(hwnd, hdcMem, 0x00000002);
    }
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcScreen);

    if (!ok) { DeleteObject(hbm); return nullptr; }
    *outW = W; *outH = H;
    return hbm;
}

static bool saveBitmapToFile(const QString &path, HBITMAP hbm, int W, int H)
{
    HDC hdc = GetDC(NULL);
    if (!hdc) return false;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = H;  // positive = bottom-up
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD strideAligned = ((W * 3 + 3) / 4) * 4;
    DWORD imgSize = strideAligned * DWORD(H);

    std::vector<BYTE> pixels(imgSize);
    if (!GetDIBits(hdc, hbm, 0, UINT(H), pixels.data(), &bi, DIB_RGB_COLORS)) {
        ReleaseDC(NULL, hdc);
        return false;
    }
    ReleaseDC(NULL, hdc);

    BITMAPFILEHEADER bfh;
    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType      = 0x4D42;  // "BM"
    bfh.bfOffBits   = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize      = bfh.bfOffBits + imgSize;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(reinterpret_cast<const char *>(&bfh), sizeof(bfh));
    f.write(reinterpret_cast<const char *>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    f.write(reinterpret_cast<const char *>(pixels.data()), imgSize);
    f.flush(); f.close();
    return true;
}

// 计算像素方差 + distinctColors（downsampled 到 ~400 像素避免太慢）
struct TextureAnalysis {
    double variance = 0.0;
    int distinctColors = 0;
    int sampledPixels = 0;
};
static TextureAnalysis analyzeBitmapTexture(HBITMAP hbm, int W, int H)
{
    TextureAnalysis r;
    if (W <= 0 || H <= 0) return r;

    HDC hdc = GetDC(NULL);
    if (!hdc) return r;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD strideAligned = ((DWORD(W) * 3 + 3) / 4) * 4;
    DWORD imgSize = strideAligned * DWORD(H);
    std::vector<BYTE> pixels(imgSize);
    if (!GetDIBits(hdc, hbm, 0, UINT(H), pixels.data(), &bi, DIB_RGB_COLORS)) {
        ReleaseDC(NULL, hdc);
        return r;
    }
    ReleaseDC(NULL, hdc);

    // 每 16 行 × 每 16 列采 1 像素，约 W*H/(256) 样本 → 1280x720 大约 3600 个点
    const int stepX = qMax(1, W / 80);
    const int stepY = qMax(1, H / 50);

    std::unordered_set<DWORD> colors;
    colors.reserve(4096);
    double sumLum = 0.0;
    double sumLumSq = 0.0;
    int n = 0;
    for (int y = 0; y < H; y += stepY) {
        // bottom-up: row 0 is at the end of the buffer
        DWORD rowIdx = DWORD(H - 1 - y) * strideAligned;
        for (int x = 0; x < W; x += stepX) {
            DWORD off = rowIdx + DWORD(x) * 3;
            BYTE b = pixels[off + 0];
            BYTE g = pixels[off + 1];
            BYTE r = pixels[off + 2];
            DWORD rgb = (DWORD(r) << 16) | (DWORD(g) << 8) | DWORD(b);
            colors.insert(rgb);
            // 亮度 Y' = 0.299R + 0.587G + 0.114B
            double y2 = 0.299 * r + 0.587 * g + 0.114 * b;
            sumLum   += y2;
            sumLumSq += y2 * y2;
            ++n;
        }
    }

    r.sampledPixels = n;
    r.distinctColors = int(colors.size());
    if (n > 1) {
        double mean = sumLum / n;
        r.variance = (sumLumSq / n) - mean * mean;
        if (r.variance < 0) r.variance = 0;
    }
    return r;
}

int main(int argc, char *argv[])
{
    // The harness uses raw Win32 geometry and capture APIs. Without explicit
    // DPI awareness Windows virtualizes GetClientRect for this non-GUI test
    // process, cropping the right/bottom of Qt windows on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QCoreApplication app(argc, argv);

    // CMake 可使用任意独立构建目录；主程序与 E2E 可执行文件是同目录产物。
    const QString buildDir = QCoreApplication::applicationDirPath();
    const QString exePath  = QDir(buildDir).absoluteFilePath("notepad-safe.exe");
    if (!QFileInfo::exists(exePath)) {
        fprintf(stderr, "找不到 notepad-safe.exe: %s\n", qUtf8Printable(exePath));
        return 2;
    }

    printf("=== Notepad Safe E2E: L2 打开大文件后「内容可见」验证 ===\n");
    printf("  Exe: %s\n", qUtf8Printable(exePath));
    printf("  本启动器PID: %lu\n", (unsigned long)GetCurrentProcessId());

    // Step 1: 生成 40MB 文件 + 准备数据目录（每次重跑都新建沙箱，避免残留影响）
    // 注意：40 MiB 超过 FileTierPolicy S1 上限 32 MiB，保证走 S2 mmap 路径
    const QString workDir = QDir::tempPath() + "/NotepadSafe_E2E_L2";
    QDir(workDir).removeRecursively();
    QDir().mkpath(workDir);
    const QString bigFile = make40MbFile(workDir);
    if (bigFile.isEmpty()) return 3;

    const QString dataDir = workDir + "/data";
    QDir().mkpath(dataDir);
    if (!writeSessionJson(dataDir, bigFile)) {
        qCritical("写 session.json 失败");
        return 4;
    }
    qInfo("  [SETUP] NOTEPADSAFE_DATADIR=%s", qUtf8Printable(dataDir));
    qInfo("  [SETUP] 会话恢复目标文件 %lld bytes (%.1f MB) : %s",
          QFileInfo(bigFile).size(), QFileInfo(bigFile).size() / (1024.0*1024.0),
          qUtf8Printable(bigFile));

    // Step 2: 启动程序
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("NOTEPADSAFE_DATADIR",         QDir::toNativeSeparators(dataDir));
    env.insert("QT_FORCE_STDERR_LOGGING",     "1");
    env.insert("QT_ASSUME_STDERR_HAS_CONSOLE","1");
    // NOTE: NOTEPADSAFE_L2_MIN_MB 环境变量未被代码支持；此处保留注释但不用。
    // 保证走 L2 靠的是测试文件 > 32MB。

    QProcess proc;
    proc.setProgram(exePath);
    proc.setWorkingDirectory(buildDir);
    proc.setProcessEnvironment(env);
    proc.start(QIODevice::ReadOnly);
    if (!proc.waitForStarted(5000)) {
        qCritical("启动失败: %s", qUtf8Printable(proc.errorString()));
        return 5;
    }
    const DWORD pid = proc.processId();
    qInfo("  [RUN] 启动成功，PID=%lu", pid);

    // Step 3: 等主窗口可见
    QElapsedTimer pollT; pollT.start();
    HWND hwnd = findMainWindowOfPid(pid, 30000);
    if (!hwnd) {
        qCritical("  [FAIL] 30 秒内主窗口不可见");
        proc.kill(); proc.waitForFinished(3000);
        return 6;
    }
    RECT wr; GetWindowRect(hwnd, &wr);
    qInfo("  [RUN] 主窗口 %p 可见耗时 %lld ms，外框=(%d,%d)-(%d,%d) %dx%d",
          hwnd, pollT.elapsed(),
          wr.left, wr.top, wr.right, wr.bottom, wr.right - wr.left, wr.bottom - wr.top);

    // Step 4: 等 L2 打开链路完成（mmap ready → activeWindowChanged → bridgePopulateScintilla
    // → Scintilla setText。我们没有办法跨进程知道 Scintilla 什么时候 setText 完，
    // 所以用"死时间 + 定时重采样"方式：每 500ms 抓客户区分析，连续 3 次满足"有内容"
    // 就立即 PASS；最多等到 10 秒（10MB 在最烂机器也该出字了）。）
    qInfo("  [RUN] 开始观测客户区内容（每 500ms 采样一次，目标：连续 3 次判定'有文本'）");
    const QString capturePath = workDir + "/capture_l2.bmp";
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
    int consecutivePasses = 0;
    TextureAnalysis lastAnalysis;
    int lastW = 0, lastH = 0;
    int  samples = 0;
    bool finalPass = false;

    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        Sleep(500);
        ++samples;
        int W = 0, H = 0;
        HBITMAP hbm = captureClientArea(hwnd, &W, &H);
        if (!hbm) {
            qWarning("    [%d] 抓客户区失败（窗口可能还在初始化）。GetLastError=%lu",
                     samples, (unsigned long)GetLastError());
            continue;
        }
        // 第一次成功时写 BMP 到磁盘，失败后人工可以翻看。
        if (!QFile::exists(capturePath)) {
            saveBitmapToFile(capturePath, hbm, W, H);
        }
        TextureAnalysis a = analyzeBitmapTexture(hbm, W, H);
        DeleteObject(hbm);
        lastAnalysis = a; lastW = W; lastH = H;

        // 判定条件：
        //   · 客户区 size 必须 > 200×100（否则 overlay 尺寸还没就位，不判 PASS）
        //   · distinctColors >= 20 （背景+文本+光标+选中色至少 5~10 种，20 保守）
        //   · 亮度方差 >= 1.0 （纯均匀色方差接近 0）
        bool sizeOk = (W > 200 && H > 100);
        bool colorsOk = (a.distinctColors >= 20);
        bool varOk    = (a.variance >= 1.0);
        bool thisPass = sizeOk && colorsOk && varOk;

        qInfo("    [%d] t=%lldms  frame=%dx%d  samples=%d  distinct=%d  var=%.2f  sizeOK=%d colorsOK=%d varOK=%d  → %s",
              samples, QDateTime::currentMSecsSinceEpoch() - (deadline - 10000),
              W, H, a.sampledPixels, a.distinctColors, a.variance,
              sizeOk, colorsOk, varOk, thisPass ? "PASS" : "WAIT");

        if (thisPass) ++consecutivePasses; else consecutivePasses = 0;
        if (consecutivePasses >= 3) { finalPass = true; break; }
    }

    // 最后再落盘一张 BMP 作为"最终画面"证据
    {
        int W = 0, H = 0;
        HBITMAP hbm = captureClientArea(hwnd, &W, &H);
        if (hbm) {
            saveBitmapToFile(capturePath, hbm, W, H);
            DeleteObject(hbm);
            lastW = W; lastH = H;
        }
    }

    printf("\n");
    printf("==============================================================\n");
    if (finalPass) {
        printf("  RESULT:  PASS — L2 内容可见。连续 3 帧客户区存在文本像素分布\n");
    } else {
        printf("  RESULT:  FAIL — 10 秒内客户区像素始终是无文本的均匀块\n");
    }
    printf("    最后一帧: %dx%d  采样像素数=%d  distinctColors=%d  亮度方差=%.3f\n",
           lastW, lastH, lastAnalysis.sampledPixels, lastAnalysis.distinctColors, lastAnalysis.variance);
    printf("    截图 BMP: %s\n", qUtf8Printable(QDir::toNativeSeparators(capturePath)));
    printf("==============================================================\n");

    // Step 5: WM_CLOSE，收集 stderr
    SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_NORMAL, 15000, nullptr);
    if (!proc.waitForFinished(8000)) {
        qWarning("WM_CLOSE 超时，kill");
        proc.kill(); proc.waitForFinished(3000);
    }

    QByteArray stderrBuf = proc.readAllStandardError();
    QString stderrCap = QString::fromUtf8(stderrBuf);
    QStringList lines = stderrCap.split('\n', Qt::SkipEmptyParts);
    const int start = qMax(0, int(lines.size()) - 80);
    if (!lines.isEmpty()) {
        printf("\n=== 最后 %d 行 stderr 日志 ===\n", int(lines.size()) - start);
        for (int i = start; i < lines.size(); ++i) {
            printf("  | %s\n", qUtf8Printable(lines[i]));
        }
    }

    return finalPass ? 0 : 10;
}
