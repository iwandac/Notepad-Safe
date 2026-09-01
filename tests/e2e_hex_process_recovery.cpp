// HX-R01-PROCESS-KILL: formal process-boundary recovery acceptance.
//
// This harness launches the shipping notepad-safe.exe with an isolated data
// directory, drives its real Hex UI through Win32 input, waits until the
// product has atomically published an encrypted recovery point, terminates
// that exact PID, and launches the same executable again.  It intentionally
// has no in-process access to MainWindow, EditorTab, SessionManager, or test
// hooks: the only observations are the product window, the OS clipboard,
// source bytes, and files committed below NOTEPADSAFE_DATADIR.

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

constexpr int kWindowWaitMs = 20'000;
constexpr int kUiSettleMs = 2'000;
constexpr int kCheckpointWaitMs = 45'000;

struct WindowSearch {
    DWORD pid = 0;
    HWND result = nullptr;
};

BOOL CALLBACK enumWindow(HWND hwnd, LPARAM raw)
{
    auto *search = reinterpret_cast<WindowSearch *>(raw);
    if (!search || !IsWindowVisible(hwnd))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid)
        return TRUE;
    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, int(std::size(title))) <= 0)
        return TRUE;
    search->result = hwnd;
    return FALSE;
}

HWND waitForMainWindow(QProcess &process, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    WindowSearch search{DWORD(process.processId()), nullptr};
    while (timer.elapsed() < timeoutMs) {
        search.result = nullptr;
        EnumWindows(enumWindow, reinterpret_cast<LPARAM>(&search));
        if (search.result)
            return search.result;
        if (process.state() == QProcess::NotRunning)
            return nullptr;
        QThread::msleep(25);
    }
    return nullptr;
}

bool focusHexSurface(HWND window)
{
    if (!window)
        return false;
    ShowWindow(window, SW_RESTORE);
    BringWindowToTop(window);
    SetForegroundWindow(window);

    RECT client{};
    if (!GetClientRect(window, &client))
        return false;
    POINT point{
        client.left + (client.right - client.left) * 28 / 100,
        client.top + (client.bottom - client.top) * 42 / 100,
    };
    if (!ClientToScreen(window, &point) || !SetCursorPos(point.x, point.y))
        return false;

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(2, inputs, sizeof(INPUT)) != 2)
        return false;
    QThread::msleep(150);
    return GetForegroundWindow() == window;
}

bool activateProductWindow(HWND window)
{
    if (!window)
        return false;
    ShowWindow(window, SW_RESTORE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    QThread::msleep(150);
    return GetForegroundWindow() == window;
}

bool dragClientRange(HWND window, int fromX, int fromY, int toX, int toY)
{
    POINT from{fromX, fromY};
    POINT to{toX, toY};
    if (!ClientToScreen(window, &from) || !ClientToScreen(window, &to)
        || !SetCursorPos(from.x, from.y)) {
        return false;
    }
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    if (SendInput(1, &down, sizeof(INPUT)) != 1)
        return false;
    for (int step = 1; step <= 5; ++step) {
        const int x = from.x + (to.x - from.x) * step / 5;
        const int y = from.y + (to.y - from.y) * step / 5;
        SetCursorPos(x, y);
        QThread::msleep(20);
    }
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const bool released = SendInput(1, &up, sizeof(INPUT)) == 1;
    QThread::msleep(100);
    return released;
}

bool captureClientBitmap(HWND window, const QString &path)
{
    RECT client{};
    if (!window || !GetClientRect(window, &client))
        return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC source = GetDC(window);
    HDC memory = source ? CreateCompatibleDC(source) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(source, width, height) : nullptr;
    if (!source || !memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (source) ReleaseDC(window, source);
        return false;
    }
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, width, height, source, 0, 0,
                               SRCCOPY | CAPTUREBLT) != FALSE;
    SelectObject(memory, old);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD stride = (DWORD(width * 3) + 3) & ~DWORD(3);
    std::vector<BYTE> pixels(stride * DWORD(height));
    const bool extracted = copied
        && GetDIBits(memory, bitmap, 0, UINT(height), pixels.data(), &info,
                     DIB_RGB_COLORS) != 0;
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, source);
    if (!extracted)
        return false;

    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + DWORD(pixels.size());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(reinterpret_cast<const char *>(&header), sizeof(header))
               == sizeof(header)
        && file.write(reinterpret_cast<const char *>(&info.bmiHeader),
                      sizeof(info.bmiHeader)) == sizeof(info.bmiHeader)
        && file.write(reinterpret_cast<const char *>(pixels.data()),
                      qint64(pixels.size())) == qint64(pixels.size());
}

bool sendKey(WORD key, bool down)
{
    const bool extended = key == VK_HOME || key == VK_END
        || key == VK_INSERT || key == VK_DELETE
        || key == VK_LEFT || key == VK_RIGHT
        || key == VK_UP || key == VK_DOWN
        || key == VK_PRIOR || key == VK_NEXT;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP)
        | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool sendChord(WORD modifier, WORD key)
{
    return sendKey(modifier, true) && sendKey(key, true)
        && sendKey(key, false) && sendKey(modifier, false);
}

bool sendPlainKey(WORD key)
{
    return sendKey(key, true) && sendKey(key, false);
}

bool toggleHexInsertFromProductMenu()
{
    // The visible product command is 编辑(&E) -> 十六进制插入模式(&I).
    // Driving the menu avoids depending on a native child widget's handling
    // of VK_INSERT while still exercising the real QAction and product state.
    if (!sendChord(VK_MENU, 'E'))
        return false;
    QThread::msleep(100);
    return sendPlainKey('I');
}

bool setClipboardText(const QString &text)
{
    if (!OpenClipboard(nullptr))
        return false;
    EmptyClipboard();
    const SIZE_T bytes = SIZE_T(text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void *target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(target, text.utf16(), SIZE_T(text.size()) * sizeof(wchar_t));
    reinterpret_cast<wchar_t *>(target)[text.size()] = L'\0';
    GlobalUnlock(memory);
    const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!ok)
        GlobalFree(memory);
    CloseClipboard();
    return ok;
}

QString clipboardText()
{
    if (!OpenClipboard(nullptr))
        return {};
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return {};
    }
    const wchar_t *text = static_cast<const wchar_t *>(GlobalLock(data));
    const QString result = text ? QString::fromWCharArray(text) : QString();
    if (text)
        GlobalUnlock(data);
    CloseClipboard();
    return result;
}

QString waitForClipboardChange(const QString &sentinel, int timeoutMs = 3'000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QString current = clipboardText();
        if (!current.isEmpty() && current != sentinel)
            return current;
        QThread::msleep(25);
    }
    return {};
}

QByteArray parseVisibleHex(QString text, bool *ok)
{
    text.remove(' ');
    text.remove('\r');
    text.remove('\n');
    const QByteArray latin = text.toLatin1();
    const QByteArray bytes = QByteArray::fromHex(latin);
    if (ok)
        *ok = !latin.isEmpty() && (latin.size() % 2) == 0
            && bytes.size() * 2 == latin.size();
    return bytes;
}

bool copyCurrentSelection(QByteArray *bytes)
{
    const QString sentinel = QStringLiteral("NSP-CLIPBOARD-SENTINEL-7D3A");
    if (!setClipboardText(sentinel) || !sendChord(VK_CONTROL, 'C'))
        return false;
    const QString text = waitForClipboardChange(sentinel);
    bool parsed = false;
    const QByteArray result = parseVisibleHex(text, &parsed);
    if (parsed && bytes)
        *bytes = result;
    return parsed;
}

bool copyAllBytes(QByteArray *bytes)
{
    if (!sendChord(VK_CONTROL, 'A'))
        return false;
    QThread::msleep(150);
    return copyCurrentSelection(bytes);
}

bool establishFirstFourByteMouseSelection(HWND window,
                                          const QByteArray &expected)
{
    // First make byte zero visible through the public keyboard surface. The
    // bounded geometry probes account only for normal font/theme rounding;
    // accepting the one-byte copy first proves that the pointer really found
    // byte zero rather than relying on screen coordinates as the oracle.
    if (!sendChord(VK_CONTROL, VK_HOME))
        return false;
    // This harness is a QCore process, while the shipping Qt GUI declares
    // per-monitor DPI awareness. On the 150% acceptance desktop Win32
    // virtualizes the caller's points; these are the logical counterparts of
    // the visibly captured x=159/y=144 first-byte cell and its 32 px pitch.
    const int startXs[] = {104, 106, 108};
    const int rowYs[] = {94, 96, 98};
    const int oneByteDeltas[] = {20, 21, 22};
    int byteZeroX = -1;
    int byteZeroY = -1;
    int glyphDelta = -1;
    for (const int y : rowYs) {
        for (const int startX : startXs) {
            for (const int delta : oneByteDeltas) {
                if (!dragClientRange(window, startX, y,
                                     startX + delta, y)) {
                    continue;
                }
                QByteArray selected;
                if (copyCurrentSelection(&selected)
                    && selected == expected.left(1)) {
                    byteZeroX = startX;
                    byteZeroY = y;
                    glyphDelta = delta;
                    break;
                }
            }
            if (byteZeroX >= 0)
                break;
        }
        if (byteZeroX >= 0)
            break;
    }
    if (byteZeroX < 0)
        return false;

    // A Hex byte occupies three monospace columns. The one-byte end point is
    // therefore also the measured byte pitch; four pitches select [0,4).
    // Collapse the verified one-byte selection first so a press inside A1 is
    // not interpreted as dragging the existing selection as text.
    if (!sendChord(VK_CONTROL, VK_HOME))
        return false;
    for (int adjustment : {-2, 0, 2}) {
        if (!dragClientRange(window, byteZeroX, byteZeroY,
                             byteZeroX + 4 * glyphDelta + adjustment,
                             byteZeroY)) {
            continue;
        }
        QByteArray selected;
        if (copyCurrentSelection(&selected) && selected == expected)
            return true;
    }
    return false;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool treeContains(const QString &root, const QByteArray &needle, QString *path)
{
    QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString candidate = files.next();
        if (readAll(candidate).contains(needle)) {
            if (path)
                *path = candidate;
            return true;
        }
    }
    return false;
}

bool readCommittedCheckpoint(const QString &dataDir, QJsonObject *tab,
                             QString *journalPath)
{
    const QString sessionPath = QDir(dataDir).filePath(QStringLiteral("session.json"));
    const QByteArray json = readAll(sessionPath);
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonArray tabs = document.object().value(QStringLiteral("tabs")).toArray();
    if (tabs.size() != 1 || !tabs.first().isObject())
        return false;
    const QJsonObject candidate = tabs.first().toObject();
    const QString journal = candidate.value(QStringLiteral("autosavePath")).toString();
    if (candidate.value(QStringLiteral("presentation")).toString()
            != QStringLiteral("hex")
        || !candidate.value(QStringLiteral("modified")).toBool()
        || !candidate.value(QStringLiteral("hexInsertMode")).toBool()
        || candidate.value(QStringLiteral("selectionStartByte")).toString()
               != QStringLiteral("0")
        || candidate.value(QStringLiteral("selectionEndByte")).toString()
               != QStringLiteral("4")
        || journal.isEmpty() || !QFileInfo::exists(journal)) {
        return false;
    }
    if (tab)
        *tab = candidate;
    if (journalPath)
        *journalPath = journal;
    return true;
}

bool waitForCommittedCheckpoint(const QString &dataDir, QJsonObject *tab,
                                QString *journalPath)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kCheckpointWaitMs) {
        if (readCommittedCheckpoint(dataDir, tab, journalPath))
            return true;
        QThread::msleep(100);
    }
    return false;
}

bool terminateExactProcess(QProcess &process)
{
    const DWORD pid = DWORD(process.processId());
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!handle)
        return false;
    const bool terminated = TerminateProcess(handle, 0xE2) != FALSE;
    if (terminated)
        WaitForSingleObject(handle, 5'000);
    CloseHandle(handle);
    process.waitForFinished(5'000);
    return terminated && process.state() == QProcess::NotRunning;
}

bool startProduct(QProcess &process, const QString &exePath,
                  const QString &dataDir, const QStringList &arguments,
                  HWND *window)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NOTEPADSAFE_DATADIR"),
                       QDir::toNativeSeparators(dataDir));
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"),
                       QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setProgram(exePath);
    process.setArguments(arguments);
    process.setWorkingDirectory(QFileInfo(exePath).absolutePath());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(5'000))
        return false;
    *window = waitForMainWindow(process, kWindowWaitMs);
    if (!*window)
        return false;
    QThread::msleep(kUiSettleMs);
    return true;
}

int fail(const char *message, QProcess *process = nullptr)
{
    std::fprintf(stderr, "[HX-R01-PROCESS-KILL][FAIL] %s\n", message);
    if (process) {
        const QByteArray output = process->readAll();
        if (!output.isEmpty())
            std::fprintf(stderr, "%s\n", output.constData());
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(3'000);
        }
    }
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
        return fail("expected absolute path to notepad-safe.exe");
    const QString exePath = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    if (!QFileInfo::exists(exePath))
        return fail("product executable does not exist");

    QTemporaryDir temp(QStringLiteral("%1/NotepadSafe-HX-R01-XXXXXX")
                           .arg(QDir::tempPath()));
    if (!temp.isValid())
        return fail("could not create isolated test directory");
    const QString dataDir = temp.filePath(QStringLiteral("app-data"));
    if (!QDir().mkpath(dataDir))
        return fail("could not create isolated app data directory");

    QByteArray original(192, '\0');
    for (int i = 0; i < original.size(); ++i)
        original[i] = char((i * 43 + 17) & 0xff);
    original[0] = '\0';
    const QString sourcePath = temp.filePath(QStringLiteral("controlled-b1.bin"));
    if (!writeAll(sourcePath, original))
        return fail("could not create controlled B1 source");
    const QByteArray sourceHash = sha256(original);
    const QByteArray canary = QByteArray::fromHex("A1B2C3D4E5F60718");
    const QByteArray expected = canary + original;

    QProcess first;
    HWND firstWindow = nullptr;
    if (!startProduct(first, exePath, dataDir, {sourcePath}, &firstWindow))
        return fail("first product process did not expose its main window", &first);
    if (!activateProductWindow(firstWindow))
        return fail("could not activate the real product window", &first);

    // Turn on INS, move to byte zero, and paste the canary through the real
    // product action. The source file must remain unchanged until an explicit
    // Save, while the recovery point must contain the logical patch.
    if (!toggleHexInsertFromProductMenu()
        || !sendChord(VK_CONTROL, VK_HOME)
        || !setClipboardText(QString::fromLatin1(canary.toHex(' ').toUpper()))
        || !sendChord(VK_CONTROL, 'V')) {
        return fail("could not drive the real Hex edit journey", &first);
    }
    QThread::msleep(250);
    QByteArray editedVisible;
    if (!copyAllBytes(&editedVisible) || editedVisible != expected) {
        std::fprintf(stderr,
                     "[HX-R01-PROCESS-KILL][DIAG] edited-size=%lld expected-size=%lld "
                     "edited-prefix=%s expected-prefix=%s\n",
                     static_cast<long long>(editedVisible.size()),
                     static_cast<long long>(expected.size()),
                     editedVisible.left(24).toHex().constData(),
                     expected.left(24).toHex().constData());
        return fail("real Hex UI did not apply the INS canary exactly", &first);
    }
    if (!establishFirstFourByteMouseSelection(firstWindow, canary.left(4))) {
        const QString capturePath = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("e2e_hex_selection_failure.bmp"));
        captureClientBitmap(firstWindow, capturePath);
        std::fprintf(stderr, "[HX-R01-PROCESS-KILL][DIAG] capture=%s\n",
                     qUtf8Printable(capturePath));
        return fail("real Hex UI did not establish [0,4) selection", &first);
    }

    QJsonObject checkpointTab;
    QString journalPath;
    if (!waitForCommittedCheckpoint(dataDir, &checkpointTab, &journalPath)) {
        const QByteArray sessionJson = readAll(
            QDir(dataDir).filePath(QStringLiteral("session.json")));
        std::fprintf(stderr, "[HX-R01-PROCESS-KILL][DIAG] session.json=%s\n",
                     sessionJson.isEmpty() ? "<missing>" : sessionJson.constData());
        QDirIterator files(dataDir, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo info(files.next());
            std::fprintf(stderr, "[HX-R01-PROCESS-KILL][DIAG] file=%s size=%lld\n",
                         qUtf8Printable(info.absoluteFilePath()),
                         static_cast<long long>(info.size()));
        }
        return fail("no deterministic encrypted recovery point within 45 seconds", &first);
    }
    if (sha256(readAll(sourcePath)) != sourceHash)
        return fail("autosave unexpectedly changed the authoritative source file", &first);
    QString plaintextPath;
    if (treeContains(dataDir, canary, &plaintextPath))
        return fail("recovery storage contains the plaintext binary canary", &first);
    if (treeContains(dataDir, QByteArrayLiteral("NotepadSafe.BinaryPatchJournal.v1"),
                     &plaintextPath)) {
        return fail("recovery storage contains the plaintext journal payload", &first);
    }
    if (readAll(journalPath).isEmpty())
        return fail("committed recovery journal is empty", &first);

    const qint64 killedPid = first.processId();
    if (!terminateExactProcess(first))
        return fail("could not force-terminate the exact product PID", &first);
    std::printf("[HX-R01-PROCESS-KILL] terminated PID=%lld after committed checkpoint\n",
                static_cast<long long>(killedPid));

    QProcess second;
    HWND secondWindow = nullptr;
    if (!startProduct(second, exePath, dataDir, {}, &secondWindow))
        return fail("second product process did not expose its main window", &second);
    if (!activateProductWindow(secondWindow))
        return fail("could not activate the restored product window", &second);

    // restoreDocumentSessionView establishes focus and the persisted source
    // selection. Invoke the global Copy action without clicking the editor
    // again: the first four inserted bytes prove selection restoration.
    QByteArray restoredSelection;
    if (!copyCurrentSelection(&restoredSelection)
        || restoredSelection != canary.left(4)) {
        return fail("restart did not restore the exact [0,4) byte selection", &second);
    }
    QByteArray restored;
    if (!copyAllBytes(&restored) || restored != expected)
        return fail("restart did not restore the patched bytes exactly", &second);
    if (sha256(readAll(sourcePath)) != sourceHash)
        return fail("restart recovery changed the source file", &second);

    // Re-establish byte zero through the visible Hex pointer/copy surface. The
    // helper first copies A1 as a one-byte origin proof and then [0,4), so the
    // following Ctrl+Home has an unambiguous selection to collapse at zero.
    // Typing one byte must insert (not overwrite), proving INS survived the
    // process boundary as well.
    if (!establishFirstFourByteMouseSelection(secondWindow, canary.left(4))) {
        return fail("could not re-establish byte zero before INS probe", &second);
    }
    if (!sendChord(VK_CONTROL, VK_HOME)
        || !sendPlainKey('F') || !sendPlainKey('F')) {
        return fail("could not probe restored INS state", &second);
    }
    QThread::msleep(150);
    QByteArray insertProbe;
    if (!copyAllBytes(&insertProbe)
        || insertProbe != QByteArray(1, char(0xff)) + expected) {
        std::fprintf(stderr,
                     "[HX-R01-PROCESS-KILL][DIAG] insert-probe-size=%lld "
                     "expected-size=%lld probe-prefix=%s expected-prefix=%s\n",
                     static_cast<long long>(insertProbe.size()),
                     static_cast<long long>(expected.size() + 1),
                     insertProbe.left(24).toHex().constData(),
                     (QByteArray(1, char(0xff)) + expected).left(24)
                         .toHex().constData());
        return fail("restored Hex mode is not INS", &second);
    }
    if (sha256(readAll(sourcePath)) != sourceHash)
        return fail("post-recovery UI probe changed the source file", &second);

    if (!terminateExactProcess(second))
        return fail("could not terminate the verification process", &second);
    std::printf("[HX-R01-PROCESS-KILL][PASS] source=%s journal=%s\n",
                sourceHash.toHex().constData(), qUtf8Printable(journalPath));
    return 0;
}
