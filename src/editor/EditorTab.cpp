#include "EditorTab.h"
#include "EditorSurfaceStyle.h"
// Hex character rows use the shared measured Scintilla-authoritative grid.
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
#include "FileTierPolicy.h"
#include "SearchResultsModel.h"
#include "Pcre2Regex.h"
#include "EditableTextSearch.h"
#include "AsyncTaskManager.h"
#include "DraftStore.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QDataStream>
#include <QFont>
#include <QColor>
#include <QMimeDatabase>
#include <QStringConverter>
#include <QStringDecoder>
#include <QMimeType>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QStatusBar>
#include <QPointer>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QApplication>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QSplitter>

#include "Sci_Position.h"
#include <QBuffer>
#include <algorithm>
#include <limits>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>


#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <Qsci/qscilexer.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerxml.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexeryaml.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerjava.h>
#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexerbatch.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexermakefile.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexerperl.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerproperties.h>
#include <Qsci/qscilexermasm.h>

namespace {

// Below this budget, queueing and exposing a background task costs more than a
// bounded first-hit scan of the in-memory UTF-8 document. This is deliberately
// much smaller than the 32 MiB S1 product boundary: tier and work scheduling
// answer different questions.
constexpr qint64 L1InlineNavigationBudgetBytes = 256LL * 1024;

QsciScintilla::EolMode detectLineEndingMode(const QString &text)
{
    qsizetype crlf = 0;
    qsizetype lf = 0;
    qsizetype cr = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (text.at(i) == QLatin1Char('\n')) {
            ++lf;
        }
    }
    if (crlf >= lf && crlf >= cr && crlf > 0)
        return QsciScintilla::EolWindows;
    if (cr > lf && cr > 0)
        return QsciScintilla::EolMac;
    if (lf > 0)
        return QsciScintilla::EolUnix;
    return QsciScintilla::EolWindows;
}

constexpr char LargeRecoveryMagic[] = "NotepadSafe.LargeEditJournal.v1";
constexpr char BinaryRecoveryMagic[] = "NotepadSafe.BinaryPatchJournal.v1";
constexpr quint32 BinaryRecoveryVersion = 1;

QString formatHexBytes(const QByteArray &bytes, int bytesPerRow = 16)
{
    bytesPerRow = qMax(1, bytesPerRow);
    static constexpr char Digits[] = "0123456789ABCDEF";
    QString text;
    text.reserve(bytes.size() * 3 + bytes.size() / bytesPerRow + 1);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes.at(i));
        text.append(QLatin1Char(Digits[value >> 4]));
        text.append(QLatin1Char(Digits[value & 0x0f]));
        const bool endOfLine = ((i + 1) % bytesPerRow) == 0;
        if (i + 1 < bytes.size())
            text.append(endOfLine ? QLatin1Char('\n') : QLatin1Char(' '));
    }
    return text;
}

int hexNibble(QChar ch)
{
    const ushort value = ch.unicode();
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parseHexBytes(const QString &text, QByteArray *bytes, QString *error)
{
    if (!bytes)
        return false;
    bytes->clear();
    bytes->reserve(text.size() / 3 + 1);
    int highNibble = -1;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace()) {
            if (highNibble >= 0) {
                if (error) {
                    *error = QObject::tr(
                        "第 %1 个字符处的十六进制字节只有一位；每个字节必须是两位。")
                                 .arg(i + 1);
                }
                bytes->clear();
                return false;
            }
            continue;
        }
        const int nibble = hexNibble(ch);
        if (nibble < 0) {
            if (error)
                *error = QObject::tr("第 %1 个字符“%2”不是十六进制数字。")
                             .arg(i + 1).arg(ch);
            bytes->clear();
            return false;
        }
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            bytes->append(char((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    if (highNibble >= 0) {
        if (error)
            *error = QObject::tr("末尾的十六进制字节只有一位；每个字节必须是两位。");
        bytes->clear();
        return false;
    }
    return true;
}

bool replaceFileAtomically(const QString &temporaryPath, const QString &targetPath,
                           QString *error)
{
#ifdef Q_OS_WIN
    const std::wstring temporary = QDir::toNativeSeparators(temporaryPath).toStdWString();
    const std::wstring target = QDir::toNativeSeparators(targetPath).toStdWString();
    const BOOL replaced = QFileInfo::exists(targetPath)
        ? ::ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
        : ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    if (replaced)
        return true;
    if (error) {
        *error = QObject::tr("Windows 原子替换失败（错误 %1）；恢复副本保留在 %2")
                     .arg(::GetLastError()).arg(temporaryPath);
    }
    return false;
#else
    QFile input(temporaryPath);
    QSaveFile output(targetPath);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        if (error) *error = QObject::tr("无法打开原子替换文件。");
        return false;
    }
    QByteArray chunk;
    while (!(chunk = input.read(4 * 1024 * 1024)).isEmpty()) {
        if (output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            if (error) *error = QObject::tr("原子替换写入不完整。");
            return false;
        }
    }
    if (input.error() != QFileDevice::NoError || !output.commit()) {
        if (error) *error = QObject::tr("原子替换提交失败。");
        return false;
    }
    QFile::remove(temporaryPath);
    return true;
#endif
}

using ScintillaSigned = intptr_t;
using ScintillaUnsigned = uintptr_t;
using ScintillaDirectFunction = ScintillaSigned (SCI_METHOD *)(
    ScintillaSigned, unsigned int, ScintillaUnsigned, ScintillaSigned);

ScintillaSigned directScintilla(QsciScintilla *editor, unsigned int message,
                                ScintillaUnsigned wParam = 0,
                                ScintillaSigned lParam = 0)
{
    if (!editor)
        return 0;
    const auto function = reinterpret_cast<ScintillaDirectFunction>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTFUNCTION));
    const auto context = reinterpret_cast<ScintillaSigned>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTPOINTER));
    return function && context ? function(context, message, wParam, lParam) : 0;
}

} // namespace

EditorTab::ExternalFileState EditorTab::externalFileState() const
{
    if (isSecureDocument() || m_documentState.location.isEmpty())
        return ExternalFileState::NotTracked;
    return compareDiskFileVersion(m_documentState.location, m_diskVersion);
}

void EditorTab::refreshDiskVersionBaseline()
{
    m_diskVersion = {};
    if (isSecureDocument() || m_documentState.location.isEmpty())
        return;
    DiskFileVersion captured;
    if (captureDiskFileVersion(m_documentState.location, &captured) == ExternalFileState::Unchanged)
        m_diskVersion = std::move(captured);
}

bool EditorTab::representsDiskFile(const DiskFileVersion &candidate) const
{
    if (isSecureDocument() || m_documentState.location.isEmpty() || !candidate.valid)
        return false;
    if (sameDiskFile(m_diskVersion, candidate))
        return true;
    return sameDiskFilePath(m_documentState.location, candidate.absolutePath);
}

EditorTab::EditorTab(QWidget *parent)
    : QWidget(parent)
{
    m_editor = new QsciScintilla(this);
    m_stack  = new QStackedWidget(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_editor);   // index 0 = L1 (QsciScintilla)

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    setupEditor();
    connect(m_editor, &QsciScintilla::selectionChanged,
            this, &EditorTab::commandCapabilitiesChanged);
    connect(m_editor, &QsciScintilla::textChanged,
            this, &EditorTab::commandCapabilitiesChanged);
}

EditorTab::~EditorTab() {
    // ⚠️  注意：此处是 EditorTab 析构体内部！虽然 EditorTab 刚进入析构、成员还完整，
    //     但 QObject 事件循环可能已派发指向本对象/子对象的 QueuedConnection。
    //     任何 processEvents 调用都可能触发半析构对象的 method invoke → 0xC0000005。
    //
    //  正确的关闭路径是 MainWindow::closeEvent() 在 Step 2.5 中显式调
    //  shutdownL2BeforeClose()（allowProcessEvents=true，MainWindow 完整，UI 事件循环仍活跃）。
    //  那里会完成 cancel LineIndex build task + 轮询终态 + unmap mmap。
    //
    //  此处析构体里仅做 dtor-safe 兜底：如果 MainWindow 路径没执行到（如异常析构），
    //  手动调 closeFile(false)—— 禁止 processEvents，保守等待 500ms 让 worker 自退出。
    if (m_l1Splitter) {
        QSignalBlocker blocker(this);
        setSplitView(false);
    }
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View && m_l2View->isOpen()) {
        m_l2View->closeFile(/*allowProcessEvents=*/false);
    }
}

void EditorTab::shutdownL2BeforeClose() {
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (Q_LIKELY(!m_l2Mode || !m_l2View)) return;
    if (!m_l2View->isOpen()) {
        // 虽然没 open，但 Scintilla 可能 reparent 到了 viewport；先回到 stack 避免下一次
        // loadFile L1 找不到 editor 在 stack page 0
        bridgeReparentEditorToStack();
        m_bridge.syncVfvDebounce.stop();
        clearL2BridgeBaseline();
        m_l2Mode = false;
        return;
    }

    // MainWindow 仍活跃：先 flush 最近窗口修改到 patches（不做就可能丢最后编辑），
    // 再 closeFile，再 reparent editor 回 stack。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        qWarning("[EditorTab::shutdownL2BeforeClose] flush overlay→patches 失败（不阻塞关闭，"
                 "可能丢失最近窗口编辑）：%s", qUtf8Printable(flushErr));
    }

    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 mode: closing mmap + "
           "canceling LineIndex build (allow processEvents because MainWindow is still alive).",
           qUtf8Printable(tabTitle()));
    m_l2View->closeFile(/*allowProcessEvents=*/true);
    bridgeReparentEditorToStack();
    m_bridge.syncVfvDebounce.stop();
    clearL2BridgeBaseline();
    m_l2Mode = false;
    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 shutdown done.",
           qUtf8Printable(tabTitle()));
}

void EditorTab::setupEditor()
{
    m_editor->installEventFilter(this);
    if (m_editor->viewport())
        m_editor->viewport()->installEventFilter(this);
    applySharedEditorSurfaceStyle(m_editor);

    // 行号
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->setMarginWidth(0, "99999");

    // 书签 marker（边距 1）
    m_bookmarkMarker = m_editor->markerDefine(QsciScintilla::Circle);
    m_editor->setMarginType(1, QsciScintilla::SymbolMargin);
    m_editor->setMarginWidth(1, 16);
    m_editor->setMarginMarkerMask(1, 1 << m_bookmarkMarker);
    m_editor->setMarkerBackgroundColor(QColor(255, 180, 0), m_bookmarkMarker);
    m_editor->setMarkerForegroundColor(QColor(255, 255, 255), m_bookmarkMarker);

    // New documents follow the Windows convention. Existing documents replace
    // this with their detected dominant line ending before text is installed.
    m_editor->setEolMode(QsciScintilla::EolWindows);

    // 长行标尺（默认 80 列）
    applyEdgeSettings();

    // 横向滚动条按需显示：Scintilla 默认 scrollWidth=2000px，即使空文档也会显示滚动条。
    // setScrollWidthTracking(true) 让 scrollWidth 跟踪实际最长行，配合初始 scrollWidth=1，
    // 只有当某行实际超出视口宽度时才出现横向滚动条。
    // 注意：QScintilla 的 ReconfigureScrollBars() 会覆盖 setHorizontalScrollBarPolicy，
    //       所以必须用 setScrollWidth/setScrollWidthTracking 而非 Qt 策略。
    // 折叠（代码折叠开关）
    m_editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);

    // 括号匹配
    // 自动补全
    m_editor->setAutoCompletionSource(QsciScintilla::AcsAll);
    m_editor->setAutoCompletionThreshold(3);
    m_editor->setBackspaceUnindents(m_backspaceUnindents);

    // Enable Scintilla's native multi-selection editing.  These options make
    // typing and paste operate on every selection instead of only the main one.
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETMULTIPLESELECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETADDITIONALSELECTIONTYPING, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETMULTIPASTE,
                            QsciScintillaBase::SC_MULTIPASTE_EACH);

    // With a large wrapped S1 document Scintilla may refine its display-line
    // count while the native thumb is being dragged.  The value that was the
    // maximum under the pointer can therefore stop being the maximum before
    // mouse release, leaving the final physical rows unreachable in one drag.
    // Remember the user's explicit bottom gesture and resolve it through
    // Scintilla's own MaxScrollPos() after the drag/layout turn settles.
    QScrollBar *s1Vertical = m_editor->verticalScrollBar();
    connect(s1Vertical, &QScrollBar::sliderPressed, this, [this]() {
        m_s1ScrollDragRequestedEnd = false;
    });
    connect(s1Vertical, &QScrollBar::sliderMoved, this,
            [this, s1Vertical](int value) {
        if (!isL2Mode())
            m_s1ScrollDragRequestedEnd = value >= s1Vertical->maximum();
    });
    connect(s1Vertical, &QScrollBar::sliderReleased, this, [this]() {
        const bool scrollToEnd = !isL2Mode() && m_s1ScrollDragRequestedEnd;
        m_s1ScrollDragRequestedEnd = false;
        if (!scrollToEnd)
            return;
        m_editor->SendScintilla(QsciScintillaBase::SCI_SCROLLTOEND);
        QTimer::singleShot(0, this, [this]() {
            if (!isL2Mode() && m_editor)
                m_editor->SendScintilla(QsciScintillaBase::SCI_SCROLLTOEND);
        });
    });

    // 信号
    connect(m_editor, &QsciScintilla::SCN_MODIFYATTEMPTRO, this, [this]() {
        emit modificationChanged(true);
    });
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, [this](int line, int index) {
        if (!isL2Mode() || !m_l2View
            || m_l2View->viewMode() != ViewMode::Hex
            || m_bridge.windowStart < 0) {
            emit cursorPositionChanged(line, index);
            return;
        }

        const qint64 local = qBound<qint64>(0,
                                             qint64(line) * m_hexBytesPerRow + index / 3,
                                             qMax<qint64>(0, m_bridge.windowLen));
        m_lastL2Line = line;
        m_lastL2Col = int(local % m_hexBytesPerRow);
        m_lastL2Byte = qMin(m_l2View->virtualFileSize(),
                            m_bridge.windowStart + local);
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    });
    connect(m_editor, &QsciScintilla::modificationChanged,
            this, [this](bool m) {
        emit modificationChanged(m);
        emit titleChanged(tabTitle());
    });
    connect(m_editor, &QsciScintilla::SCN_MODIFIED, this,
            [this](int, int modificationType, const char *, int, int,
                   int, int, int, int, int) {
        constexpr int contentChanges = QsciScintillaBase::SC_MOD_INSERTTEXT
                                       | QsciScintillaBase::SC_MOD_DELETETEXT;
        const bool userEdit = modificationType & QsciScintillaBase::SC_PERFORMED_USER;
        // S2/S3 is an immutable source with a replaceable Scintilla projection.
        // Navigating to a search hit rebuilds that projection with setText(); it
        // must never advance the source revision or invalidate search results.
        // Real external source changes are reported by the provider below.
        if (m_bridge.installingText || isReadOnlyByPolicy())
            return;
        // A projection rebuild holds m_bridge.applying through one queued
        // paint turn. Keyboard input can legitimately arrive in that narrow
        // interval; never classify an explicit Scintilla user edit as part of
        // the projection or search-result invalidation will be missed.
        if ((modificationType & contentChanges)
            && (!m_bridge.applying || userEdit)) {
            advanceContentRevision();
        }
    });
}

bool EditorTab::loadFile(const QString &path,
                         std::optional<TextEncoding> forcedEncoding)
{
    m_metadataModified = false;
    m_documentState.contentKind = DocumentContentKind::Text;
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(path).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile] ⚠️  大文件(%lld MB)直接走了 L1 loadFile！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openTabAutoMode → openLargeFileL2 窗口化秒开。"
                  " path='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(path));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadFile] readAll 小文件 %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

    DecodeResult result = forcedEncoding
        ? decodeBytesWithEncoding(data, *forcedEncoding)
        : decodeBytes(data);
    m_encoding = result.encoding;
    m_encodingOrigin = result.origin;
    m_encodingConfidence = result.confidence;
    m_encodingDecodeErrors = result.decodeErrors;
    m_editor->setEolMode(detectLineEndingMode(result.text));
    m_documentState.location = QFileInfo(path).absoluteFilePath();
    m_autosavePath.clear();  // 加载正式文件，清除 autosave 关联

    // 大文件分块加载：避免 setText() 一次性重建整个文档导致 UI 长时间卡死
    // This bounded chunk append belongs to the S1 path only.
    const int largeThreshold = 2 * 1024 * 1024; // 2MB 以上走分块路径
    if (data.size() < largeThreshold) {
        // 重置 scrollWidth：tracking 模式只增不减，切换文件时需手动归零
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        // UTF-8 文件可直接使用原始字节，跳过 QString 中间层以节省内存和时间
        // 非 UTF-8 文件需经 QString 转换为 UTF-8 字节
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3); // 跳过 3 字节 BOM
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(false);

    // 成功加载文件：切回 L1 显示，关闭 L2 模式（若之前曾打开 L2）。
    // L2 会把 Scintilla 临时 reparent 到 VirtualFileView 的 viewport；reparent 时
    // QStackedWidget 会自动移除该页，因此不能假设 L1 永远仍是 index 0。
    bridgeReparentEditorToStack();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    m_l2Mode = false;
    emit l2ModeStatusChanged();
    if (m_stack) m_stack->setCurrentWidget(l1Page());

    // 根据后缀猜测语法
    Syntax s = guessSyntaxFromExtension(path);
    setSyntax(s);

    refreshDiskVersionBaseline();

    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::loadAutosave(const QString &autosavePath)
{
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(autosavePath).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave] ⚠️  大 autosave(%lld MB)直接走了 L1 loadAutosave！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openAutosaveAutoMode → 窗口化大文件恢复。"
                  " autosavePath='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(autosavePath));
    }
    QFile file(autosavePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadAutosave] readAll 小 autosave %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

    return loadAutosaveBytes(data, autosavePath);
}

bool EditorTab::loadAutosaveBytes(const QByteArray &data, const QString &backingPath)
{
    m_metadataModified = false;
    m_documentState.contentKind = DocumentContentKind::Text;
    if (data.size() >= FileTierPolicy::StandardEditLimitBytes)
        return false;

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    m_encodingOrigin = result.origin;
    m_encodingConfidence = result.confidence;
    m_encodingDecodeErrors = result.decodeErrors;
    m_editor->setEolMode(detectLineEndingMode(result.text));
    // 关键：不设置 m_documentState.location，保持未命名状态，对用户透明
    m_autosavePath = backingPath;
    m_diskVersion = {};

    // 设置文本（但不清除 modified 标记，让用户知道内容是上次未保存的）
    const int largeThreshold = 2 * 1024 * 1024;
    if (data.size() < largeThreshold) {
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3);
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(true);

    // autosave 不猜语法，保持 Plain 或已设置的语法

    emit encodingChanged(m_encoding);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::saveFile(QString *errMsg)
{
    if (isUntitled()) {
        if (errMsg) *errMsg = QObject::tr("未命名文件，请先指定保存路径。");
        return false;
    }
    // Match native editor semantics: Save on an unchanged named document is a
    // no-op. Binary files are routed through the mapped provider even at 9 MiB;
    // rewriting that entire backing file merely because Ctrl+S was pressed
    // created a visible sibling temporary copy for no user-visible change.
    if (!isModified())
        return true;
    if (isL2Mode())
        return saveL2As(m_documentState.location, errMsg);
    return saveAsFile(m_documentState.location, errMsg);
}

bool EditorTab::saveAsFile(const QString &path, QString *errMsg)
{
    if (sameDiskFilePath(m_documentState.location, path)) {
        const ExternalFileState diskState = externalFileState();
        if (diskState == ExternalFileState::Changed
            || diskState == ExternalFileState::Missing
            || diskState == ExternalFileState::Unreadable) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "目标仍是已被外部修改的原文件。请选择其他路径，或先重新加载磁盘版本。");
            }
            return false;
        }
    }
    if (isL2Mode())
        return saveL2As(path, errMsg);

    // Ordinary Save and explicit encoding conversion share the same strict
    // encoder. Legacy code pages must never replace an unrepresentable edit
    // with '?' or a best-fit character without telling the user.
    if (!writeTextAsEncoding(path, m_editor->text(), m_encoding, errMsg))
        return false;

    m_documentState.location = QFileInfo(path).absoluteFilePath();
    setModified(false);

    // 保存成功：自动清理 autosave 临时文件（自清理机制）
    clearAutosaveFile();
    m_autosavePath.clear();
    refreshDiskVersionBaseline();

    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::isModified() const
{
    if (isL2Mode())
        // Overlay edits become PatchList entries at the next window switch or
        // save. They must still participate in close prompts immediately.
        return m_l2SaveNeeded || m_editor->isModified() || m_metadataModified;
    return m_editor->isModified() || m_metadataModified;
}

void EditorTab::setModified(bool m)
{
    if (isL2Mode()) {
        const bool before = isModified();
        m_l2SaveNeeded = m;
        if (!m) {
            m_metadataModified = false;
            m_editor->setModified(false);
        }
        if (before != isModified()) {
            m_documentState.dirty = isModified();
            emit modificationChanged(isModified());
            emit titleChanged(tabTitle());
        }
        return;
    }
    const bool before = isModified();
    m_metadataModified = m;
    if (!m)
        m_editor->setModified(false);
    if (before != isModified()) {
        m_documentState.dirty = isModified();
        emit modificationChanged(isModified());
        emit titleChanged(tabTitle());
    }
}

bool EditorTab::isUntitled() const
{
    return m_documentState.location.isEmpty();
}

DocumentState EditorTab::documentState() const noexcept
{
    DocumentState state = m_documentState;
    state.capabilities = DocumentCapability::None;

    const bool accessible = !isSecureDocumentMasked()
        && !isSecureDocumentTransitioning();
    if (!accessible)
        return state;

    state.capabilities |= DocumentCapability::Read;
    state.capabilities |= DocumentCapability::Search;
    state.capabilities |= DocumentCapability::Copy;
    state.capabilities |= DocumentCapability::AnalyzeLocally;
    state.capabilities |= DocumentCapability::AnalyzeRemotelyWithConsent;
    if (canModifyCommand())
        state.capabilities |= DocumentCapability::Edit;
    if (!isReadOnlyByPolicy())
        state.capabilities |= DocumentCapability::Save;
    return state;
}

bool EditorTab::createDocumentSnapshot(qint64 maximumBytes,
                                       DocumentSnapshot *snapshot,
                                       QString *error) const
{
    if (!snapshot) {
        if (error)
            *error = tr("文档快照输出为空。");
        return false;
    }
    *snapshot = {};
    const DocumentState state = documentState();
    if (m_secureDocumentMasked || m_secureDocumentTransitioning
        || !state.capabilities.testFlag(DocumentCapability::Read)) {
        if (error)
            *error = tr("文档当前未解锁或不可读取。");
        return false;
    }
    const qint64 size = contentSizeBytes();
    if (maximumBytes <= 0 || size < 0 || size > maximumBytes
        || size > std::numeric_limits<qsizetype>::max()) {
        if (error) {
            *error = tr("文档大小 %1 字节超过本次工具快照上限 %2 字节；请缩小处理范围。")
                         .arg(size).arg(maximumBytes);
        }
        return false;
    }

    DocumentSnapshot result;
    result.documentId = state.id;
    result.revision = state.revision;
    result.contentKind = state.contentKind;
    result.securityClass = state.securityClass;
    result.capabilities = state.capabilities;
    result.encodingName = encodingName(encoding());
    if (!isL2Mode() && state.contentKind == DocumentContentKind::Text) {
        result.text = m_editor->text();
        const qint64 utf8Size = result.text.toUtf8().size();
        if (utf8Size > maximumBytes) {
            if (error)
                *error = tr("编辑后的文本超过本次工具快照上限；请缩小处理范围。");
            return false;
        }
        result.sourceRange = {{0}, {utf8Size}};
        result.encodingName = QStringLiteral("UTF-8 working text");
    } else {
        QByteArray source;
        if (m_windowedReadOnlyProvider) {
            source = m_windowedReadOnlyProvider->sourceBytes(
                0, static_cast<qsizetype>(size));
        } else if (m_l2View) {
            source = m_l2View->readByteRange(0, size);
        }
        if (source.size() != size) {
            if (error)
                *error = tr("无法建立完整且一致的文档快照。");
            return false;
        }
        result.sourceRange = {{0}, {size}};
        if (state.contentKind == DocumentContentKind::Binary) {
            result.bytes = std::move(source);
        } else {
            result.text = decodeBytesWithEncoding(source, encoding()).text;
        }
    }
    *snapshot = std::move(result);
    return true;
}

QString EditorTab::filePath() const
{
    return m_documentState.location;
}

void EditorTab::setFilePath(const QString &path)
{
    m_documentState.location = path;
    m_documentState.origin = path.isEmpty() ? DocumentOrigin::Untitled
                                            : DocumentOrigin::File;
    m_documentState.persistence = path.isEmpty() ? DocumentPersistence::None
                                                 : DocumentPersistence::PlainFile;
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
}

QString EditorTab::fileName() const
{
    if (isSecureDocument()) {
        if (m_secureDocumentMasked)
            return QStringLiteral("已锁定文档");
        if (!m_documentState.location.isEmpty())
            return QFileInfo(m_documentState.location).fileName();
        return m_secureDocumentTitle.isEmpty() ? QStringLiteral("未命名")
                                               : m_secureDocumentTitle;
    }
    if (m_documentState.location.isEmpty())
        return QStringLiteral("未命名");
    return QFileInfo(m_documentState.location).fileName();
}

QString EditorTab::tabTitle() const
{
    QString title = fileName();
    if (isModified())
        title += QStringLiteral(" ●");
    return title;
}

void EditorTab::initializeSecureDocument(const QUuid &id, const QString &title,
                                         const QString &plainText,
                                         const QString &filePath)
{
    Q_ASSERT(!id.isNull());
    m_metadataModified = false;
    m_secureDocumentTransitioning = true;
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_l2View && m_l2View->isOpen())
        m_l2View->closeFile(true);
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(0);
    m_l2Mode = false;
    m_autosavePath.clear();
    m_documentState.id = id;
    m_documentState.location = filePath;
    m_documentState.origin = DocumentOrigin::SecureStore;
    m_documentState.persistence = DocumentPersistence::SecureFile;
    m_documentState.securityClass = DocumentSecurityClass::SecureDocument;
    m_documentState.contentKind = DocumentContentKind::Text;
    m_secureDocumentTitle = title.trimmed();
    m_secureDocumentMasked = false;
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);
    m_diskVersion = {};
    m_encoding = TextEncoding::Utf8;
    m_editor->setEolMode(detectLineEndingMode(plainText));
    m_editor->setScrollWidth(1);
    m_editor->setReadOnly(false);
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_CLEARALL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->setText(plainText);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    if (m_stack)
        m_stack->setCurrentWidget(l1Page());
    setSyntax(Syntax::Markdown);
    emit l2ModeStatusChanged();
    emit encodingChanged(m_encoding);
    emit titleChanged(tabTitle());
}

bool EditorTab::initializeSecureBinaryDocument(const QUuid &id,
                                               const QString &title,
                                               QByteArray bytes,
                                               const QString &filePath,
                                               QString *error)
{
    if (id.isNull()) {
        if (error)
            *error = tr("安全二进制文档标识无效。");
        if (!bytes.isEmpty())
            bytes.fill(0);
        return false;
    }
    m_secureDocumentTransitioning = true;
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    m_l2Mode = false;

    VirtualFileView *view = ensureL2View();
    QString localError;
    if (!view->openMemory(std::move(bytes), ViewMode::Hex, &localError)) {
        m_secureDocumentTransitioning = false;
        if (error)
            *error = localError;
        return false;
    }
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);

    m_autosavePath.clear();
    m_documentState.id = id;
    m_documentState.location = filePath;
    m_documentState.origin = DocumentOrigin::SecureStore;
    m_documentState.persistence = DocumentPersistence::SecureFile;
    m_documentState.securityClass = DocumentSecurityClass::SecureDocument;
    m_documentState.contentKind = DocumentContentKind::Binary;
    m_secureDocumentTitle = title.trimmed();
    m_secureDocumentMasked = false;
    m_diskVersion = {};
    m_encoding = TextEncoding::Utf8;
    m_l2Mode = true;
    m_l2ReadOnlyByPolicy = false;
    m_l2SaveNeeded = false;
    m_lastL2Line = 0;
    m_lastL2Col = 0;
    m_lastL2Byte = 0;
    m_syntax = Syntax::Plain;
    activateL2Overlay(view);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    emit l2ModeStatusChanged();
    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    emit cursorPositionChanged(0, 0);
    return true;
}

bool EditorTab::exportBinaryPayloadBytes(QByteArray *bytes, QString *error)
{
    if (bytes)
        bytes->clear();
    if (!bytes || !isBinaryDocument() || isSecureDocumentMasked()
        || !m_l2View || !m_l2View->isOpen()) {
        if (error)
            *error = tr("安全二进制文档当前不可读取。");
        return false;
    }
    if (!bridgeFlushOverlayToPatches(error))
        return false;
    const qint64 size = m_l2View->virtualFileSize();
    if (size < 0 || size >= 32LL * 1024 * 1024) {
        if (error)
            *error = tr("安全文档必须小于 32 MiB。");
        return false;
    }
    *bytes = m_l2View->readByteRange(0, size);
    return bytes->size() == size;
}

void EditorTab::setSecurePayloadLimitExclusive(qint64 limit)
{
    m_securePayloadLimitExclusive = qMax<qint64>(0, limit);
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(m_securePayloadLimitExclusive);
}

void EditorTab::maskSecureDocument()
{
    if (!isSecureDocument() || m_secureDocumentMasked)
        return;
    setSplitView(false);
    m_secureDocumentTransitioning = true;
    m_secureDocumentMasked = true;
    // A note title is commonly derived from its first plaintext line. Do not
    // leave that content resident or visible in the tab strip while locked;
    // the encrypted backing store restores it on the next unlock.
    if (!m_secureDocumentTitle.isEmpty()) {
        m_secureDocumentTitle.fill(QChar(0));
        m_secureDocumentTitle.clear();
    }
    if (isSecureBinaryDocument()) {
        bridgeReparentEditorToStack();
        if (m_l2View && m_l2View->isOpen())
            m_l2View->closeFile(true);
        m_l2Mode = false;
        if (m_stack)
            m_stack->setCurrentWidget(l1Page());
        clearL2BridgeBaseline();
        emit l2ModeStatusChanged();
    }
    m_editor->setReadOnly(false);
    // Disable undo before removing plaintext so neither Ctrl+Z nor Scintilla's
    // undo buffer can resurrect the secure document while the tab is masked.
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_CLEARALL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->setText(QStringLiteral(
        "此加密文档已锁定。\n\n解锁加密数据后可继续编辑。"));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_editor->setReadOnly(true);
    if (m_stack)
        m_stack->setCurrentWidget(l1Page());
    m_secureDocumentTransitioning = false;
    emit titleChanged(tabTitle());
}

void EditorTab::beginSecureDocumentLocking()
{
    if (!isSecureDocument() || m_secureDocumentMasked || m_secureDocumentTransitioning)
        return;
    m_secureDocumentTransitioning = true;
    if (!m_secureLockingPage) {
        m_secureLockingPage = new QWidget(m_stack);
        m_secureLockingPage->setObjectName(QStringLiteral("secureDocumentLockingPage"));
        auto *layout = new QVBoxLayout(m_secureLockingPage);
        layout->setContentsMargins(32, 32, 32, 32);
        auto *label = new QLabel(
            tr("正在安全保存并锁定…\n\n正文已隐藏，提交完成后将清除解密内容。"),
            m_secureLockingPage);
        label->setObjectName(QStringLiteral("secureDocumentLockingMessage"));
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        layout->addStretch();
        layout->addWidget(label);
        layout->addStretch();
        m_stack->addWidget(m_secureLockingPage);
    }
    m_stack->setCurrentWidget(m_secureLockingPage);
    emit commandCapabilitiesChanged();
}

void EditorTab::cancelSecureDocumentLocking()
{
    if (!isSecureDocument() || m_secureDocumentMasked || !m_secureDocumentTransitioning)
        return;
    m_secureDocumentTransitioning = false;
    if (m_stack) {
        if (isSecureBinaryDocument() && m_l2View)
            m_stack->setCurrentWidget(m_l2View);
        else
            m_stack->setCurrentWidget(l1Page());
    }
    emit commandCapabilitiesChanged();
}

void EditorTab::setSecureDocumentTitle(const QString &title)
{
    if (!isSecureDocument())
        return;
    const QString normalized = title.trimmed();
    if (m_secureDocumentTitle == normalized)
        return;
    m_secureDocumentTitle = normalized;
    emit titleChanged(tabTitle());
}

void EditorTab::convertToPlainDocument(const QString &filePath)
{
    m_metadataModified = false;
    m_secureDocumentTransitioning = true;
    clearL2BridgeBaseline();
    m_documentState.id = QUuid::createUuid();
    m_secureDocumentTitle.clear();
    m_secureDocumentMasked = false;
    setSecurePayloadLimitExclusive(0);
    m_documentState.location = filePath;
    m_documentState.origin = filePath.isEmpty() ? DocumentOrigin::Untitled
                                                : DocumentOrigin::File;
    m_documentState.persistence = filePath.isEmpty() ? DocumentPersistence::None
                                                     : DocumentPersistence::PlainFile;
    m_documentState.securityClass = DocumentSecurityClass::OrdinaryDocument;
    m_autosavePath.clear();
    m_editor->setReadOnly(false);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
}

TextEncoding EditorTab::encoding() const
{
    return m_encoding;
}

void EditorTab::setEncoding(TextEncoding enc)
{
    const bool changed = m_encoding != enc;
    m_encoding = enc;
    m_encodingOrigin = EncodingOrigin::UserSelected;
    m_encodingConfidence = EncodingConfidence::Certain;
    m_encodingDecodeErrors = false;
    if (changed)
        emit encodingChanged(m_encoding);
}

QString EditorTab::lineEndingName() const
{
    switch (m_editor->eolMode()) {
    case QsciScintilla::EolWindows: return QStringLiteral("CRLF");
    case QsciScintilla::EolMac: return QStringLiteral("CR");
    case QsciScintilla::EolUnix: return QStringLiteral("LF");
    }
    return QStringLiteral("LF");
}

Syntax EditorTab::syntax() const
{
    return m_syntax;
}

void EditorTab::setSyntax(Syntax syntax)
{
    if (m_syntax == syntax)
        return;
    m_syntax = syntax;
    applySyntax(syntax);
    emit syntaxChanged(m_syntax);
}

Syntax EditorTab::guessSyntaxFromExtension(const QString &fileName)
{
    const QFileInfo info(fileName);
    const QString ext = info.suffix().toLower();
    const QString baseName = info.fileName().toLower();
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "hh" || ext == "hxx" || ext == "c" || ext == "inl")
        return Syntax::Cpp;
    if (ext == "py" || ext == "pyw")
        return Syntax::Python;
    if (ext == "json")
        return Syntax::Json;
    if (ext == "html" || ext == "htm" || ext == "xhtml")
        return Syntax::Html;
    if (ext == "xml" || ext == "svg" || ext == "ui" || ext == "pro")
        return Syntax::Xml;
    if (ext == "sql")
        return Syntax::Sql;
    if (ext == "yaml" || ext == "yml")
        return Syntax::Yaml;
    if (ext == "md" || ext == "markdown")
        return Syntax::Markdown;
    if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx")
        return Syntax::JavaScript;
    if (ext == "java")
        return Syntax::Java;
    if (ext == "cs")
        return Syntax::CSharp;
    if (ext == "css" || ext == "scss" || ext == "less")
        return Syntax::Css;
    if (ext == "sh" || ext == "bash" || ext == "zsh")
        return Syntax::Bash;
    if (ext == "bat" || ext == "cmd")
        return Syntax::Batch;
    if (ext == "cmake" || baseName == "cmakelists.txt")
        return Syntax::CMake;
    if (ext == "mk" || ext == "mak" || baseName == "makefile")
        return Syntax::Makefile;
    if (ext == "lua")
        return Syntax::Lua;
    if (ext == "rb" || ext == "rake" || baseName == "rakefile")
        return Syntax::Ruby;
    if (ext == "pl" || ext == "pm")
        return Syntax::Perl;
    if (ext == "diff" || ext == "patch")
        return Syntax::Diff;
    if (ext == "properties" || ext == "ini" || ext == "cfg" || ext == "conf")
        return Syntax::Properties;
    if (ext == "asm" || ext == "s")
        return Syntax::Asm;
    return Syntax::Plain;
}

QsciLexer *EditorTab::createLexer(Syntax syntax, QObject *owner) const
{
    if (!owner)
        owner = m_editor;
    switch (syntax) {
    case Syntax::Cpp:      return new QsciLexerCPP(owner);
    case Syntax::Python:   return new QsciLexerPython(owner);
    case Syntax::Json:     return new QsciLexerJSON(owner);
    case Syntax::Html:     return new QsciLexerHTML(owner);
    case Syntax::Xml:      return new QsciLexerXML(owner);
    case Syntax::Sql:      return new QsciLexerSQL(owner);
    case Syntax::Yaml:     return new QsciLexerYAML(owner);
    case Syntax::Markdown: return new QsciLexerMarkdown(owner);
    case Syntax::JavaScript: return new QsciLexerJavaScript(owner);
    case Syntax::Java:       return new QsciLexerJava(owner);
    case Syntax::CSharp:     return new QsciLexerCSharp(owner);
    case Syntax::Css:        return new QsciLexerCSS(owner);
    case Syntax::Bash:       return new QsciLexerBash(owner);
    case Syntax::Batch:      return new QsciLexerBatch(owner);
    case Syntax::CMake:      return new QsciLexerCMake(owner);
    case Syntax::Makefile:   return new QsciLexerMakefile(owner);
    case Syntax::Lua:        return new QsciLexerLua(owner);
    case Syntax::Ruby:       return new QsciLexerRuby(owner);
    case Syntax::Perl:       return new QsciLexerPerl(owner);
    case Syntax::Diff:       return new QsciLexerDiff(owner);
    case Syntax::Properties: return new QsciLexerProperties(owner);
    case Syntax::Asm:        return new QsciLexerMASM(owner);
    case Syntax::Plain:    return nullptr;
    }
    return nullptr;
}

void EditorTab::applySyntax(Syntax syntax)
{
    QsciLexer *lexer = createLexer(syntax);
    if (lexer) {
        QFont font = m_editor->font();
        lexer->setDefaultFont(font);
    }
    m_editor->setLexer(lexer);
    if (m_splitEditor) {
        QsciLexer *splitLexer = createLexer(syntax, m_splitEditor);
        if (splitLexer)
            splitLexer->setDefaultFont(m_splitEditor->font());
        m_splitEditor->setLexer(splitLexer);
    }
    // setLexer 接管 ownership，旧 lexer 自动删除

    // 大文件启用 idle styling：只对可见区域着色，剩余部分在空闲时逐步完成，
    // 避免 setLexer 后一次性对全文档做词法分析导致 UI 卡顿
    // （Notepad++ 对大文件也采用类似策略）
    const int docLen = m_editor->length();
    if (docLen > 5 * 1024 * 1024) { // > 5MB
        // SC_IDLESTYLING_TOVISIBLE=1: 只着色到可见区域，其余在 idle 时完成
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 1UL);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 0UL);
    }
}

// ============ 行操作 ============

void EditorTab::duplicateLine()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_LINEDUPLICATE);
}

void EditorTab::deleteLine()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_LINEDELETE);
}

void EditorTab::moveLineUp()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_MOVESELECTEDLINESUP);
}

void EditorTab::moveLineDown()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_MOVESELECTEDLINESDOWN);
}

void EditorTab::transposeLine()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_LINETRANSPOSE);
}

void EditorTab::selectionToUpper()
{
    QsciScintilla *editor = activeTextEditor();
    if (!supportsTextMutationCommands() || !editor->hasSelectedText())
        return;
    editor->SendScintilla(QsciScintillaBase::SCI_UPPERCASE);
}

void EditorTab::selectionToLower()
{
    QsciScintilla *editor = activeTextEditor();
    if (!supportsTextMutationCommands() || !editor->hasSelectedText())
        return;
    editor->SendScintilla(QsciScintillaBase::SCI_LOWERCASE);
}

void EditorTab::joinSelectedLines()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    const auto start = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
    const auto end = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
    if (end <= start)
        return;
    editor->beginUndoAction();
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, start);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, end);
    editor->SendScintilla(QsciScintillaBase::SCI_LINESJOIN);
    editor->endUndoAction();
}

void EditorTab::splitSelectedLines()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    const auto start = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
    const auto end = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
    if (end <= start)
        return;
    const int pixelWidth = qMax(80, editor->viewport()->width() - 24);
    editor->beginUndoAction();
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, start);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, end);
    editor->SendScintilla(QsciScintillaBase::SCI_LINESSPLIT, pixelWidth);
    editor->endUndoAction();
}

void EditorTab::selectNextOccurrence()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    if (!editor->hasSelectedText()) {
        const auto position = editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
        const auto start = editor->SendScintilla(QsciScintillaBase::SCI_WORDSTARTPOSITION,
                                                   position, 1L);
        const auto end = editor->SendScintilla(QsciScintillaBase::SCI_WORDENDPOSITION,
                                                 position, 1L);
        if (end <= start)
            return;
        editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
    }
    const long oldTargetStart = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETSTART);
    const long oldTargetEnd = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETEND);
    const long oldSearchFlags = editor->SendScintilla(
        QsciScintillaBase::SCI_GETSEARCHFLAGS);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, 0UL);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(editor->length()));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          QsciScintillaBase::SCFIND_MATCHCASE);
    editor->SendScintilla(QsciScintillaBase::SCI_MULTIPLESELECTADDNEXT);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                          static_cast<unsigned long>(oldTargetStart));
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(oldTargetEnd));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          static_cast<unsigned long>(oldSearchFlags));


}

void EditorTab::selectAllOccurrences()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    if (!editor->hasSelectedText()) {
        const auto position = editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
        const auto start = editor->SendScintilla(QsciScintillaBase::SCI_WORDSTARTPOSITION,
                                                   position, 1L);
        const auto end = editor->SendScintilla(QsciScintillaBase::SCI_WORDENDPOSITION,
                                                 position, 1L);
        if (end <= start)
            return;
        editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
    }
    const long oldTargetStart = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETSTART);
    const long oldTargetEnd = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETEND);
    const long oldSearchFlags = editor->SendScintilla(
        QsciScintillaBase::SCI_GETSEARCHFLAGS);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, 0UL);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(editor->length()));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          QsciScintillaBase::SCFIND_MATCHCASE);
    editor->SendScintilla(QsciScintillaBase::SCI_MULTIPLESELECTADDEACH);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                          static_cast<unsigned long>(oldTargetStart));
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(oldTargetEnd));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          static_cast<unsigned long>(oldSearchFlags));
}

void EditorTab::moveToMatchingBrace()
{
    if (isBinaryDocument() || isSecureDocumentMasked())
        return;
    activeTextEditor()->moveToMatchingBrace();
}

void EditorTab::selectToMatchingBrace()
{
    if (isBinaryDocument() || isSecureDocumentMasked())
        return;
    activeTextEditor()->selectToMatchingBrace();
}

void EditorTab::foldCurrentLine()
{
    if (!supportsLexerCommands())
        return;
    int line = 0;
    int index = 0;
    QsciScintilla *editor = activeTextEditor();
    editor->getCursorPosition(&line, &index);
    editor->foldLine(line);
}

void EditorTab::foldAll(bool expand)
{
    if (!supportsLexerCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_FOLDALL,
                            expand ? QsciScintillaBase::SC_FOLDACTION_EXPAND
                                   : QsciScintillaBase::SC_FOLDACTION_CONTRACT);
}

void EditorTab::sortLinesAsc()
{
    int lineFrom, indexFrom, lineTo, indexTo;
    m_editor->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0 || lineTo <= lineFrom)
        return;
    QStringList selected;
    for (int i = lineFrom; i <= lineTo; ++i)
        selected << m_editor->text(i);
    selected.sort();
    QString replacement = selected.join("\n") + "\n";
    m_editor->setSelection(lineFrom, 0, lineTo + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(replacement, lineFrom, 0);
}

void EditorTab::sortLinesDesc()
{
    int lineFrom, indexFrom, lineTo, indexTo;
    m_editor->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0 || lineTo <= lineFrom)
        return;
    QStringList selected;
    for (int i = lineFrom; i <= lineTo; ++i)
        selected << m_editor->text(i);
    std::sort(selected.begin(), selected.end(), std::greater<QString>());
    QString replacement = selected.join("\n") + "\n";
    m_editor->setSelection(lineFrom, 0, lineTo + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(replacement, lineFrom, 0);
}

// ============ 书签 ============

void EditorTab::toggleBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    unsigned mask = m_editor->markersAtLine(line);
    if (mask & (1 << m_bookmarkMarker)) {
        m_editor->markerDelete(line, m_bookmarkMarker);
    } else {
        m_editor->markerAdd(line, m_bookmarkMarker);
    }
}

void EditorTab::nextBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int next = m_editor->markerFindNext(line + 1, 1 << m_bookmarkMarker);
    if (next < 0) {
        // wrap
        next = m_editor->markerFindNext(0, 1 << m_bookmarkMarker);
    }
    if (next >= 0) {
        m_editor->setCursorPosition(next, 0);
        m_editor->ensureLineVisible(next);
    }
}

void EditorTab::prevBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int prev = m_editor->markerFindPrevious(line - 1, 1 << m_bookmarkMarker);
    if (prev < 0) {
        int lines = m_editor->lines();
        prev = m_editor->markerFindPrevious(lines - 1, 1 << m_bookmarkMarker);
    }
    if (prev >= 0) {
        m_editor->setCursorPosition(prev, 0);
        m_editor->ensureLineVisible(prev);
    }
}

void EditorTab::clearBookmarks()
{
    m_editor->markerDeleteAll(m_bookmarkMarker);
}

void EditorTab::clearMarkers()
{
    m_editor->markerDeleteAll(-1);
}

// ============ 搜索 ============

QVector<SearchMatch> EditorTab::findAll(const QString &pattern, bool regex,
                                        bool caseSensitive, bool wholeWord)
{
    QVector<SearchMatch> matches;
    m_lastFindAllTruncated = false;
    m_lastSearchError.clear();
    m_l1SearchCache.clear();
    m_l1SearchCacheValid = false;
    m_lastL1MatchIndex = -1;
    if (pattern.isEmpty()) {
        m_lastSearchError = QObject::tr("查找文本为空。");
        return matches;
    }

    const QByteArray subject = m_editor->text().toUtf8();
    const auto recordMatch = [&](qint64 byteOffset, qint64 byteLength) {
            const int lineNum = int(m_editor->SendScintilla(
                QsciScintillaBase::SCI_LINEFROMPOSITION, long(byteOffset)));
            const qint64 lineByteStart = m_editor->SendScintilla(
                QsciScintillaBase::SCI_POSITIONFROMLINE, lineNum);
            QString lineText = m_editor->text(lineNum);
            while (lineText.endsWith('\n') || lineText.endsWith('\r'))
                lineText.chop(1);

            SearchMatch match;
            match.line = lineNum;
            match.index = QString::fromUtf8(subject.constData() + lineByteStart,
                                             byteOffset - lineByteStart).size();
            match.length = QString::fromUtf8(subject.constData() + byteOffset,
                                              byteLength).size();
            match.text = QString::fromUtf8(subject.constData() + byteOffset, byteLength);
            match.context = lineText;
            match.byteOffset = byteOffset;
            match.byteLength = qint32(byteLength);
            m_l1SearchCache.append({byteOffset, qint32(byteLength)});
            matches.append(std::move(match));
            if (matches.size() >= 200000) {
                m_lastFindAllTruncated = true;
                return false;
            }
            return true;
        };
    if (regex) {
        Pcre2Regex compiled;
        QString scanError;
        if (!compiled.compile(pattern, false, caseSensitive, wholeWord,
                              &scanError)) {
            m_lastSearchError = scanError;
            return matches;
        }
        const Pcre2Regex::ScanResult scanResult = compiled.scan(
            reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
            {}, {}, recordMatch, &scanError);
        if (scanResult == Pcre2Regex::ScanResult::Failed) {
            m_lastSearchError = scanError;
            matches.clear();
            m_l1SearchCache.clear();
            return matches;
        }
        if (scanResult == Pcre2Regex::ScanResult::Cancelled
            && !m_lastFindAllTruncated) {
            m_lastSearchError = QObject::tr("搜索已取消。");
            matches.clear();
            m_l1SearchCache.clear();
            return matches;
        }
    } else {
        const QVector<EditableTextHit> literalHits = findEditableTextLiteralHits(
            subject, pattern, caseSensitive, wholeWord, {}, {}, false, false,
            -1, 200000);
        for (const EditableTextHit &hit : literalHits) {
            if (!recordMatch(hit.byteOffset, hit.byteLength))
                break;
        }
    }
    if (m_lastFindAllTruncated) {
        m_lastFindAllTruncated = false;
        m_lastSearchError = QObject::tr(
            "匹配结果超过 200,000 处，当前文档无法在可用内存内完整保存结果。");
        matches.clear();
        m_l1SearchCache.clear();
        return matches;
    }
    m_l1SearchCachePattern = pattern;
    m_l1SearchCacheRegex = regex;
    m_l1SearchCacheCaseSensitive = caseSensitive;
    m_l1SearchCacheWholeWord = wholeWord;
    m_l1SearchCacheRevision = m_documentState.revision;
    m_l1SearchCacheValid = true;
    return matches;
}

bool EditorTab::findAllL1Async(
    const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
    QObject *callbackContext,
    std::function<void(QVector<SearchMatch>, bool, QString)> callback)
{
    m_lastSearchError.clear();
    if (isL2Mode() || pattern.isEmpty() || m_l1FindAllTaskId > 0)
        return false;

    const quint64 revision = m_documentState.revision;
    const bool wipeSnapshot = isSecureDocument();
    QByteArray subject = m_editor->text().toUtf8();
    const qint64 snapshotSize = subject.size();
    const QString displayName = fileName().isEmpty()
        ? QObject::tr("未命名文档") : fileName();
    QPointer<EditorTab> that(this);
    QPointer<QObject> callbackGuard(callbackContext ? callbackContext : this);

    m_l1FindAllTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch, AsyncTaskPriority::High,
        QObject::tr("查找全部: %1").arg(displayName),
        QObject::tr("扫描当前文档"),
        [that, callbackGuard, subject = std::move(subject), pattern, regex,
         caseSensitive, wholeWord, revision, wipeSnapshot,
         callback = std::move(callback)](
            qint64 taskId, AsyncCancelPtr cancel,
            std::function<void(qint64, qint64)> reportProgress,
            std::function<void(const AsyncErrorInfo &)> reportError) mutable {
            const auto wipe = qScopeGuard([&] {
                if (wipeSnapshot && !subject.isEmpty())
                    subject.fill(0);
            });
            QVector<SearchMatch> matches;
            QVector<L1SearchHit> hits;
            matches.reserve(256);
            hits.reserve(256);
            bool truncated = false;
            QString error;
            const auto cancelled = [&] {
                return cancel && cancel->load(std::memory_order_acquire);
            };
            AsyncTaskManager::instance().updateTaskPhase(
                taskId, regex ? QObject::tr("执行正则匹配")
                              : QObject::tr("扫描字面值"),
                1, 1, 0, subject.size());
            bool ok = true;
            int lineNumber = 0;
            qint64 lineStart = 0;
            qint64 cachedContextStart = -1;
            QString cachedContext;
            const auto recordMatch = [&](qint64 byteOffset, qint64 byteLength) {
                        if (byteLength < 0
                            || byteLength > std::numeric_limits<qint32>::max()) {
                            error = QObject::tr("单个匹配过长。");
                            return false;
                        }
                        while (lineStart < byteOffset) {
                            const qsizetype newline = subject.indexOf(
                                '\n', qsizetype(lineStart));
                            if (newline < 0 || newline >= byteOffset)
                                break;
                            lineStart = qint64(newline) + 1;
                            ++lineNumber;
                        }
                        if (cachedContextStart != lineStart) {
                            qsizetype lineEnd = subject.indexOf(
                                '\n', qsizetype(lineStart));
                            if (lineEnd < 0)
                                lineEnd = subject.size();
                            qint64 contextLength = qint64(lineEnd) - lineStart;
                            if (contextLength > 0
                                && subject.at(qsizetype(lineStart + contextLength - 1))
                                       == '\r') {
                                --contextLength;
                            }
                            cachedContext = QString::fromUtf8(
                                subject.constData() + lineStart, contextLength);
                            cachedContextStart = lineStart;
                        }

                        SearchMatch match;
                        match.line = lineNumber;
                        match.index = QString::fromUtf8(
                            subject.constData() + lineStart,
                            byteOffset - lineStart).size();
                        match.length = QString::fromUtf8(
                            subject.constData() + byteOffset, byteLength).size();
                        match.text = QString::fromUtf8(
                            subject.constData() + byteOffset, byteLength);
                        match.context = cachedContext;
                        match.byteOffset = byteOffset;
                        match.byteLength = qint32(byteLength);
                        hits.push_back({byteOffset, qint32(byteLength)});
                        matches.push_back(std::move(match));
                        if (matches.size() >= 200000) {
                            error = QObject::tr(
                                "匹配结果超过 200,000 处，当前文档无法在可用内存内完整保存结果。");
                            return false;
                        }
                        return true;
                    };
            if (regex) {
                Pcre2Regex compiled;
                ok = compiled.compile(pattern, false, caseSensitive,
                                      wholeWord, &error);
                if (ok) {
                const auto scanResult = compiled.scan(
                    reinterpret_cast<const uchar *>(subject.constData()),
                    subject.size(), cancelled,
                    [reportProgress, total = qint64(subject.size())](qint64 done) {
                        reportProgress(done, total);
                    },
                    recordMatch, &error);
                ok = scanResult != Pcre2Regex::ScanResult::Failed
                    && scanResult != Pcre2Regex::ScanResult::Cancelled
                    && !cancelled() && error.isEmpty();
                }
            } else {
                const QVector<EditableTextHit> literalHits = findEditableTextLiteralHits(
                    subject, pattern, caseSensitive, wholeWord, cancelled,
                    [reportProgress, total = qint64(subject.size())](qint64 done) {
                        reportProgress(done, total);
                    }, false, false, -1, 200000);
                if (cancelled()) {
                    ok = false;
                } else {
                    for (const EditableTextHit &hit : literalHits) {
                        if (!recordMatch(hit.byteOffset, hit.byteLength)) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (!ok && error.isEmpty() && cancelled())
                error = QObject::tr("已取消。");
            if (!ok && !error.isEmpty() && error != QObject::tr("已取消。")) {
                AsyncErrorInfo taskError;
                taskError.message = error;
                reportError(taskError);
            }
            if (!callbackGuard || !callback)
                return;
            QMetaObject::invokeMethod(
                callbackGuard,
                [that, taskId, revision, pattern, regex, caseSensitive,
                 wholeWord, matches = std::move(matches), hits = std::move(hits),
                 truncated, error = std::move(error), ok,
                 callback = std::move(callback)]() mutable {
                    if (!that) {
                        callback({}, false, QObject::tr("文档已关闭。"));
                        return;
                    }
                    if (that->m_l1FindAllTaskId == taskId)
                        that->m_l1FindAllTaskId = 0;
                    if (that->m_documentState.revision != revision) {
                        that->m_lastSearchError = QObject::tr(
                            "文档已变更，请重新查找。");
                        callback({}, false, that->m_lastSearchError);
                        return;
                    }
                    that->m_lastSearchError = ok ? QString() : error;
                    that->m_lastFindAllTruncated = truncated;
                    if (ok) {
                        that->m_l1SearchCache = std::move(hits);
                        that->m_l1SearchCachePattern = pattern;
                        that->m_l1SearchCacheRegex = regex;
                        that->m_l1SearchCacheCaseSensitive = caseSensitive;
                        that->m_l1SearchCacheWholeWord = wholeWord;
                        that->m_l1SearchCacheRevision = revision;
                        that->m_l1SearchCacheValid = true;
                        that->m_lastL1MatchIndex = -1;
                    }
                    callback(ok ? std::move(matches) : QVector<SearchMatch>{},
                             truncated, that->m_lastSearchError);
                }, Qt::QueuedConnection);
        }, 0, QLatin1String("SearchFindAll"), snapshotSize);
    return m_l1FindAllTaskId > 0;
}

void EditorTab::setSearchHighlights(const QVector<SearchMatch> &matches)
{
    if (isL2Mode() || !m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, indicator,
                            QsciScintillaBase::INDIC_ROUNDBOX);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, indicator, 0x00B7FF);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, indicator, 90);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
    for (const SearchMatch &match : matches) {
        const qint64 start = match.byteOffset >= 0
            ? match.byteOffset
            : m_editor->positionFromLineIndex(match.line, match.index);
        const qint64 end = match.byteOffset >= 0 && match.byteLength >= 0
            ? match.byteOffset + match.byteLength
            : m_editor->positionFromLineIndex(match.line,
                                              match.index + match.length);
        if (start >= 0 && end > start) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                                    long(start), long(end - start));
        }
    }
}

void EditorTab::clearSearchHighlights()
{
    m_l2SearchHighlights.clear();
    if (m_l2View)
        m_l2View->clearSearchHighlights();
    if (!m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
}

void EditorTab::selectSearchMatch(int line, int index, int length)
{
    if (isL2Mode()) {
        setCursorPosition(line, index);
        return;
    }
    const int start = m_editor->positionFromLineIndex(line, index);
    const int end = m_editor->positionFromLineIndex(line, index + qMax(0, length));
    if (start >= 0 && end >= start) {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                static_cast<long>(start), static_cast<long>(end));
    }
    m_editor->ensureLineVisible(line);
    m_editor->setFocus();
}

// ============ 光标 ============

void EditorTab::getCursorPosition(int *line, int *index) const
{
    if (isL2Mode()) {
        if (line)  *line  = m_lastL2Line;
        if (index) *index = m_lastL2Col;
        return;
    }
    activeTextEditor()->getCursorPosition(line, index);
}

void EditorTab::setCursorPosition(int line, int index)
{
    if (isL2Mode() && m_l2View) {
        m_l2View->gotoLine(line);
        // L2 没有列跳转（字节偏移跳转用 gotoByteOffset），列置 0
        if (m_lastL2Col != 0) {
            m_lastL2Col = 0;
            emit cursorPositionChanged(m_lastL2Line, 0);
        }
        return;
    }
    QsciScintilla *editor = activeTextEditor();
    editor->setCursorPosition(line, index);
    editor->ensureLineVisible(line);
}

// ============ 长行标尺 ============

void EditorTab::applyEdgeSettings()
{
    for (QsciScintilla *view : {m_editor, m_splitEditor}) {
        if (!view)
            continue;
        view->setEdgeColumn(m_edgeColumn);
        view->setEdgeMode(m_edgeVisible ? QsciScintilla::EdgeLine
                                        : QsciScintilla::EdgeNone);
    }
}

void EditorTab::setEdgeColumn(int column)
{
    if (column < 1) column = 1;
    if (column > 1000) column = 1000;
    m_edgeColumn = column;
    applyEdgeSettings();
}

void EditorTab::setEdgeVisible(bool visible)
{
    m_edgeVisible = visible;
    applyEdgeSettings();
}

bool EditorTab::isEdgeVisible() const
{
    return m_edgeVisible;
}

// ============ 编码切换 ============

void EditorTab::reloadWithEncoding(TextEncoding enc)
{
    if (m_encoding == enc)
        return;
    // 重新解释内容：当前 Unicode 文本 → 用旧编码还原字节 → 用新编码解码
    // 但更实用的做法是：如果磁盘上有文件，从磁盘按新编码重读；否则保持文本不变只改编码标记
    if (!isUntitled() && QFileInfo::exists(m_documentState.location)) {
        // 只有在用户确认后才重载（由上层调用 saveFile 检查），这里简单切换编码标记
        m_encoding = enc;
        emit encodingChanged(m_encoding);
        return;
    }
    // 未命名/内存中：简单切换编码标记（下次保存用新编码）
    m_encoding = enc;
    emit encodingChanged(m_encoding);
}

// ============ autosave 自清理 ============

void EditorTab::clearAutosaveFile() const
{
    if (!m_autosavePath.isEmpty() && QFile::exists(m_autosavePath)) {
        QFile::remove(m_autosavePath);
    }
}

// ============ 大文件分块加载 ============

void EditorTab::loadUtf8Chunked(const QByteArray &utf8)
{
    // 通过 Scintilla 底层 SCI_APPENDTEXT 分块追加文本，
    // 比 setText(整个文本) 快很多倍：setText 会一次性重建行索引、
    // 触发全量重排，而 SCI_APPENDTEXT 是增量追加，Scintilla 内部只做最小化更新。
    //
    // This bounded chunk append belongs to the S1 path only and must not be
    // reused to build a complete S2/S3 document.
    //
    // !! 重要：不能调用 setReadOnly(true) 来防止用户在加载期间编辑 !!
    // 因为 Scintilla 的 Document::InsertString 内部会检查 cb.IsReadOnly()，
    // readOnly 状态下 SCI_APPENDTEXT 会直接返回 0 不插入任何内容，
    // 导致加载完成后编辑框为空（进度条正常显示但内容未写入）。
    // 防干扰改为：每 chunk 间 processEvents(ExcludeUserInputEvents) 仅重绘不接受输入；
    // 且 L1 阈值仅 ~128MB（秒级完成），故不再弹出 WindowModal QProgressDialog。

    // 关闭 undo 收集：加载过程不产生 undo 步骤
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);

    // 清空已有内容并重置 scrollWidth（tracking 只增不减，需手动归零）
    m_editor->setScrollWidth(1);
    m_editor->clear();

    // 加载期间临时关闭换行：WrapWord 模式下每追加一段文本都会触发
    // 整个可见区的换行重算，大文件时极慢；加载完成后恢复
    QsciScintilla::WrapMode savedWrap = m_editor->wrapMode();
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(QsciScintilla::WrapNone);

    const char *raw = utf8.constData();
    const int total = utf8.size();
    const int chunkSize = 512 * 1024; // 512KB per chunk

    // 状态栏非模态进度（若当前窗口为 QMainWindow）；无则静默。
    auto *mw = qobject_cast<QMainWindow*>(this->window());
    const int reportEvery = qMax(1, (2 * 1024 * 1024) / qMax(1, chunkSize)); // ~每 2MB 报一次
    int chunkNo = 0;

    int pos = 0;
    while (pos < total) {
        int end = qMin(pos + chunkSize, total);
        // 回退到完整 UTF-8 字符边界：不截断多字节字符
        while (end < total &&
               (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
            --end;
        // 防御：极端情况下整个 chunk 都是 UTF-8 后续字节，end 会回退到 pos 之前，
        // 此时不再回退，直接使用 chunkSize 范围（可能截断 1 个多字节字符，但避免死循环/负长度）
        if (end <= pos)
            end = qMin(pos + chunkSize, total);

        int len = end - pos;
        m_editor->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                                static_cast<uintptr_t>(len), raw + pos);

        pos = end;
        ++chunkNo;
        if ((chunkNo % reportEvery) == 0 || pos >= total) {
            if (mw && mw->statusBar() && total > 0) {
                int pct = int((qint64(pos) * 100) / qint64(total));
                mw->statusBar()->showMessage(
                    QObject::tr("正在加载文件… %1% (%2 / %3 MB)")
                        .arg(pct)
                        .arg(pos / (1024 * 1024))
                        .arg(total / (1024 * 1024)),
                    0);
            }
        }
        // 仅允许重绘（不接受鼠标/键盘输入），避免模态且防止用户编辑中状态不一致
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mw && mw->statusBar()) mw->statusBar()->clearMessage();

    // 恢复换行模式
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(savedWrap);

    // 恢复 undo 收集并清空 undo 缓冲（加载步骤不可 undo）
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
}

// ============ 自动换行 ============

void EditorTab::setWordWrap(bool on)
{
    if (windowedReadOnlyActive()) {
        // S2/S3 use a bounded source window. Wrapped display-line geometry is
        // not globally addressable without laying out the whole file, so a
        // thumb drag repeatedly reloads and reflows the projection. Keep the
        // read-only analyzer on physical lines; MainWindow retains the user's
        // S1 preference and applies it again after switching tabs.
        m_l2View->setWordWrap(false);
        m_windowedReadOnlyProvider->setWordWrap(false);
        return;
    }
    // L2 模式：委托 VirtualFileView
    if (isL2Mode() && m_l2View) {
        m_l2View->setWordWrap(on);
        if (m_l2View->viewMode() != ViewMode::Hex) {
            m_editor->setWrapMode(on ? QsciScintilla::WrapWord
                                     : QsciScintilla::WrapNone);
            if (on) {
                m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
                m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
            }
            layoutL2Overlay();
            alignTextOverlayToGlobalScroll();
            const quint64 generation = m_bridge.projectionGeneration;
            // Scintilla lays out wrapped display lines lazily. Re-apply the
            // same source anchor after the first repaint; this changes only
            // viewport state and never inserts document line endings.
            QTimer::singleShot(0, this, [this, generation]() {
                if (m_l2Mode && m_bridge.projectionGeneration == generation)
                    alignTextOverlayToGlobalScroll();
            });
        }
        return;
    }
    // L1 模式：QsciScintilla
    if (on) {
        m_editor->setWrapMode(QsciScintilla::WrapWord);
        m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
        m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    } else {
        m_editor->setWrapMode(QsciScintilla::WrapNone);
    }
    if (m_splitEditor) {
        m_splitEditor->setWrapMode(on ? QsciScintilla::WrapWord
                                      : QsciScintilla::WrapNone);
        m_splitEditor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
        m_splitEditor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    }
}

bool EditorTab::wordWrap() const
{
    if (isL2Mode() && m_l2View)
        return m_l2View->wordWrap();
    return m_editor->wrapMode() != QsciScintilla::WrapNone;
}

void EditorTab::setWhitespaceVisible(bool on)
{
    m_whitespaceVisible = on;
    if (m_editor) {
        m_editor->setWhitespaceVisibility(on ? QsciScintilla::WsVisible
                                             : QsciScintilla::WsInvisible);
    }
    if (m_splitEditor)
        m_splitEditor->setWhitespaceVisibility(on ? QsciScintilla::WsVisible
                                                   : QsciScintilla::WsInvisible);
    if (m_windowedReadOnlyProvider && m_windowedReadOnlyProvider->editor())
        m_windowedReadOnlyProvider->editor()->setWhitespaceVisibility(
            on ? QsciScintilla::WsVisible : QsciScintilla::WsInvisible);
}

void EditorTab::setEolVisible(bool on)
{
    m_eolVisible = on;
    if (m_editor)
        m_editor->setEolVisibility(on);
    if (m_splitEditor)
        m_splitEditor->setEolVisibility(on);
    if (m_windowedReadOnlyProvider && m_windowedReadOnlyProvider->editor())
        m_windowedReadOnlyProvider->editor()->setEolVisibility(on);
}

bool EditorTab::convertEols(QsciScintilla::EolMode mode)
{
    if (!supportsTextMutationCommands())
        return false;
    m_editor->beginUndoAction();
    m_editor->convertEols(mode);
    m_editor->setEolMode(mode);
    m_editor->endUndoAction();
    emit encodingChanged(m_encoding);
    return true;
}

void EditorTab::setUseTabs(bool on)
{
    m_useTabs = on;
    if (m_editor)
        m_editor->setIndentationsUseTabs(on);
    if (m_splitEditor)
        m_splitEditor->setIndentationsUseTabs(on);
}

void EditorTab::setTabWidth(int columns)
{
    m_tabWidth = qBound(1, columns, 16);
    if (m_editor)
        m_editor->setTabWidth(m_tabWidth);
    if (m_splitEditor)
        m_splitEditor->setTabWidth(m_tabWidth);
}

void EditorTab::setBackspaceUnindents(bool on)
{
    m_backspaceUnindents = on;
    if (m_editor)
        m_editor->setBackspaceUnindents(on);
    if (m_splitEditor)
        m_splitEditor->setBackspaceUnindents(on);
}

void EditorTab::setWordCompletionEnabled(bool on)
{
    m_wordCompletionEnabled = on;
    if (!m_editor)
        return;
    m_editor->setAutoCompletionSource(on ? QsciScintilla::AcsAll
                                         : QsciScintilla::AcsNone);
    m_editor->setAutoCompletionThreshold(on ? 3 : -1);
    if (m_splitEditor) {
        m_splitEditor->setAutoCompletionSource(on ? QsciScintilla::AcsAll
                                                   : QsciScintilla::AcsNone);
        m_splitEditor->setAutoCompletionThreshold(on ? 3 : -1);
    }
}

QsciScintilla *EditorTab::activeTextEditor() const
{
    if (m_splitEditor && (m_splitEditor->hasFocus()
                          || (m_splitEditor->viewport()
                              && m_splitEditor->viewport()->hasFocus()))) {
        return m_splitEditor;
    }
    return m_editor;
}

QWidget *EditorTab::l1Page() const
{
    return m_l1Splitter ? static_cast<QWidget *>(m_l1Splitter)
                        : static_cast<QWidget *>(m_editor);
}

bool EditorTab::canSplitView() const
{
    return m_editor && !isL2Mode() && !isBinaryDocument()
           && !isSecureDocumentMasked() && !isSecureDocumentTransitioning();
}

Qt::Orientation EditorTab::splitOrientation() const
{
    return m_l1Splitter ? m_l1Splitter->orientation() : Qt::Horizontal;
}

void EditorTab::setSplitView(bool enabled, Qt::Orientation orientation)
{
    if (enabled && !canSplitView())
        return;
    if (!enabled) {
        if (!m_l1Splitter)
            return;
        const bool wasCurrent = m_stack && m_stack->currentWidget() == m_l1Splitter;
        if (m_splitEditor) {
            m_splitEditor->setDocument(QsciDocument());
            delete m_splitEditor;
            m_splitEditor = nullptr;
        }
        QSplitter *splitter = m_l1Splitter;
        m_l1Splitter = nullptr;
        if (m_stack)
            m_stack->removeWidget(splitter);
        m_editor->setParent(m_stack);
        if (m_stack && m_stack->indexOf(m_editor) < 0)
            m_stack->insertWidget(0, m_editor);
        if (wasCurrent && m_stack)
            m_stack->setCurrentWidget(m_editor);
        delete splitter;
        emit commandCapabilitiesChanged();
        return;
    }

    if (m_l1Splitter) {
        m_l1Splitter->setOrientation(orientation);
        m_editor->show();
        if (m_splitEditor)
            m_splitEditor->show();
        // QSplitter preserves pixel sizes when its orientation changes.  A
        // width that was valid for a side-by-side split can become a zero
        // height after switching to a stacked split, leaving the second view
        // logically alive but invisible.  Rebalance both visible views.
        const int extent = orientation == Qt::Horizontal
            ? m_l1Splitter->width() : m_l1Splitter->height();
        const int half = qMax(1, extent / 2);
        m_l1Splitter->setSizes({half, half});
        emit commandCapabilitiesChanged();
        return;
    }

    const bool wasCurrent = m_stack && m_stack->currentWidget() == m_editor;
    const int oldIndex = m_stack ? m_stack->indexOf(m_editor) : 0;
    m_l1Splitter = new QSplitter(orientation, m_stack);
    m_l1Splitter->setObjectName(QStringLiteral("editorSplitView"));
    m_l1Splitter->setChildrenCollapsible(false);
    m_l1Splitter->addWidget(m_editor);
    if (m_stack)
        m_stack->insertWidget(qMax(0, oldIndex), m_l1Splitter);

    m_splitEditor = new QsciScintilla(m_l1Splitter);
    m_splitEditor->setObjectName(QStringLiteral("secondaryScintillaEditor"));
    applySharedEditorSurfaceStyle(m_splitEditor);
    m_splitEditor->setDocument(m_editor->document());
    m_splitEditor->setMarginType(0, QsciScintilla::NumberMargin);
    m_splitEditor->setMarginLineNumbers(0, true);
    m_splitEditor->setMarginWidth(0, "99999");
    m_splitEditor->setMarginWidth(1, 0);
    m_splitEditor->setFolding(QsciScintilla::BoxedTreeFoldStyle);
    m_splitEditor->setEolMode(m_editor->eolMode());
    m_splitEditor->setReadOnly(m_editor->isReadOnly());
    m_splitEditor->setScrollWidth(1);
    m_splitEditor->setScrollWidthTracking(true);
    m_splitEditor->setIndentationsUseTabs(m_useTabs);
    m_splitEditor->setTabWidth(m_tabWidth);
    m_splitEditor->setBackspaceUnindents(m_backspaceUnindents);
    m_splitEditor->setWhitespaceVisibility(m_whitespaceVisible
                                               ? QsciScintilla::WsVisible
                                               : QsciScintilla::WsInvisible);
    m_splitEditor->setEolVisibility(m_eolVisible);
    m_splitEditor->setWrapMode(wordWrap() ? QsciScintilla::WrapWord
                                           : QsciScintilla::WrapNone);
    m_splitEditor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
    m_splitEditor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    m_splitEditor->setAutoCompletionSource(m_wordCompletionEnabled
                                               ? QsciScintilla::AcsAll
                                               : QsciScintilla::AcsNone);
    m_splitEditor->setAutoCompletionThreshold(m_wordCompletionEnabled ? 3 : -1);
    m_splitEditor->SendScintilla(QsciScintillaBase::SCI_SETMULTIPLESELECTION, 1UL);
    m_splitEditor->SendScintilla(QsciScintillaBase::SCI_SETADDITIONALSELECTIONTYPING, 1UL);
    m_splitEditor->SendScintilla(QsciScintillaBase::SCI_SETMULTIPASTE,
                                 QsciScintillaBase::SC_MULTIPASTE_EACH);
    QsciLexer *lexer = createLexer(m_syntax, m_splitEditor);
    if (lexer)
        lexer->setDefaultFont(m_splitEditor->font());
    m_splitEditor->setLexer(lexer);
    applyEdgeSettings();
    m_l1Splitter->addWidget(m_splitEditor);
    // Reparenting the current page out of QStackedWidget can preserve its
    // hidden state.  Both editor surfaces must become visible members of the
    // splitter; otherwise one logical view consumes zero pixels.
    m_editor->show();
    m_splitEditor->show();
    const int extent = orientation == Qt::Horizontal
        ? m_l1Splitter->width() : m_l1Splitter->height();
    const int half = qMax(1, extent / 2);
    m_l1Splitter->setSizes({half, half});
    connect(m_splitEditor, &QsciScintilla::cursorPositionChanged,
            this, &EditorTab::cursorPositionChanged);
    connect(m_splitEditor, &QsciScintilla::selectionChanged,
            this, &EditorTab::commandCapabilitiesChanged);
    connect(m_splitEditor, &QsciScintilla::textChanged,
            this, &EditorTab::commandCapabilitiesChanged);
    if (wasCurrent && m_stack)
        m_stack->setCurrentWidget(m_l1Splitter);
    m_splitEditor->setFocus();
    emit commandCapabilitiesChanged();
}

void EditorTab::setDisplayEncoding(TextEncoding enc)
{
    if (isL2Mode() && m_l2View) {
        if (m_encoding == enc)
            return;
        QString error;
        if (!bridgeFlushOverlayToPatches(&error)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(error, 6000);
            return;
        }
        // L2: preserve the virtual byte stream and rebuild only the visible
        // decoded projection. Pending patches remain attached to byte offsets.
        m_encoding = enc;
        m_encodingOrigin = EncodingOrigin::UserSelected;
        m_encodingConfidence = EncodingConfidence::Certain;
        m_encodingDecodeErrors = false;
        m_l2View->setDisplayEncoding(enc);
        invalidateL2ProjectionSearchState();
        if (m_windowedReadOnlyProvider)
            m_windowedReadOnlyProvider->setTextEncoding(enc);
        if (windowedReadOnlyActive()) {
            clearL2BridgeBaseline();
            emit encodingChanged(m_encoding);
            return;
        }
        const qint64 start = m_l2View->activeWindowByteStart();
        const qint64 end = m_l2View->activeWindowByteEnd();
        clearL2BridgeBaseline();
        if (end > start)
            bridgePopulateScintilla(start, end);
        emit encodingChanged(m_encoding);
    } else {
        // Secure Text is a logical UTF-8 payload; m_documentState.location points to its
        // encrypted KDBX container and must never be decoded as document text.
        if (m_documentState.location.isEmpty() || isSecureDocument()) return;
        QFile f(m_documentState.location);
        if (!f.open(QIODevice::ReadOnly)) return;
        QByteArray raw = f.readAll();
        f.close();
        const DecodeResult decoded = decodeBytesWithEncoding(raw, enc);
        m_editor->setEolMode(detectLineEndingMode(decoded.text));
        m_editor->setText(decoded.text);
        m_editor->setModified(false);
        m_metadataModified = false;
        m_encoding = enc;
        m_encodingOrigin = EncodingOrigin::UserSelected;
        m_encodingConfidence = EncodingConfidence::Certain;
        m_encodingDecodeErrors = decoded.decodeErrors;
        refreshDiskVersionBaseline();
        emit encodingChanged(m_encoding);
    }
}

TextEncoding EditorTab::displayEncoding() const
{
    return m_encoding;
}

bool EditorTab::isHexPresentation() const
{
    return m_l2Mode && m_l2View && m_l2View->viewMode() == ViewMode::Hex;
}

// ============================================================================
//  统一分流：编辑/剪贴板/跳转（外部不要直接调 editor()->xxx()，走这里）
// ============================================================================

void EditorTab::selectAll()
{
    if (windowedReadOnlyActive()) {
        m_windowedReadOnlyProvider->selectAllSource();
        return;
    }
    if (isL2Mode() && m_l2View) {
        // Scintilla is the only visible/input surface in L2. Select the active
        // window; selecting multi-GiB virtual content would be unsafe anyway.
        m_editor->selectAll();
        return;
    }
    activeTextEditor()->selectAll();
}

bool EditorTab::canModifyCommand() const
{
    if (m_secureDocumentMasked || isSecureDocumentTransitioning()
        || windowedReadOnlyActive() || isReadOnlyByPolicy() || !m_editor) {
        return false;
    }
    return !activeTextEditor()->isReadOnly();
}

bool EditorTab::supportsTextMutationCommands() const
{
    // Native Scintilla mutation commands operate on character positions.  Raw
    // byte editing and windowed mmap views use different coordinate systems.
    return canModifyCommand() && !isL2Active() && !isBinaryDocument()
           && !isSecureDocumentMasked();
}

bool EditorTab::supportsLexerCommands() const
{
    return m_editor && !isL2Active() && !isBinaryDocument()
           && !isSecureDocumentMasked() && m_syntax != Syntax::Plain;
}

bool EditorTab::canUndoCommand() const
{
    if (!canModifyCommand())
        return false;
    if (isL2Mode() && m_l2View)
        return m_editor->isUndoAvailable() || m_l2View->canUndo();
    return activeTextEditor()->isUndoAvailable();
}

bool EditorTab::canRedoCommand() const
{
    if (!canModifyCommand())
        return false;
    if (isL2Mode() && m_l2View)
        return m_editor->isRedoAvailable() || m_l2View->canRedo();
    return activeTextEditor()->isRedoAvailable();
}

bool EditorTab::hasSelectionCommand() const
{
    if (m_secureDocumentMasked)
        return false;
    if (windowedReadOnlyActive()) {
        return m_windowedReadOnlyProvider->selectionEndOffset()
               > m_windowedReadOnlyProvider->selectionStartOffset();
    }
    if (!m_editor)
        return false;
    if (isL2Mode() && m_l2View && m_l2View->viewMode() == ViewMode::Hex) {
        qint64 start = 0;
        qint64 end = 0;
        return hexSelectionByteRange(&start, &end) && end > start;
    }
    return activeTextEditor()->hasSelectedText();
}

void EditorTab::undo()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            if (m_l2View->undo(nullptr)) {
                const qint64 caret = qBound<qint64>(
                    0, m_l2View->cursorByte(), m_l2View->virtualFileSize());
                bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                        m_l2View->activeWindowByteEnd());
                setAsciiCaret(caret);
            }
        } else if (m_editor->isUndoAvailable()) {
            m_editor->undo();
        } else if (m_l2View->undo(nullptr)) {
            bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                    m_l2View->activeWindowByteEnd());
        }
        return;
    }
    activeTextEditor()->undo();
}

void EditorTab::redo()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            if (m_l2View->redo(nullptr)) {
                const qint64 caret = qBound<qint64>(
                    0, m_l2View->cursorByte(), m_l2View->virtualFileSize());
                bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                        m_l2View->activeWindowByteEnd());
                setAsciiCaret(caret);
            }
        } else if (m_editor->isRedoAvailable()) {
            m_editor->redo();
        } else if (m_l2View->redo(nullptr)) {
            bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                    m_l2View->activeWindowByteEnd());
        }
        return;
    }
    activeTextEditor()->redo();
}

void EditorTab::cut()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier);
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused)
                (void)handleHexAsciiKeyPress(&event);
            else
                (void)handleHexKeyPress(&event);
            return;
        }
        m_editor->cut();
        return;
    }
    activeTextEditor()->cut();
}

void EditorTab::copy()
{
    if (windowedReadOnlyActive()) {
        QString error;
        if (!m_windowedReadOnlyProvider->copySelection(&error) && !error.isEmpty()) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(error, 5000);
            QApplication::beep();
        }
        return;
    }
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused) {
                QKeyEvent event(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
                (void)handleHexAsciiKeyPress(&event);
                return;
            }
            qint64 start = 0;
            qint64 end = 0;
            if (hexSelectionByteRange(&start, &end) && end > start) {
                QByteArray selected = m_l2View->readByteRange(start, end);

                ByteClipboard::setHexSelection(selected);
                if (isSecureBinaryDocument() && !selected.isEmpty())
                    selected.fill(0);

            }
            return;
        }
        m_editor->copy();
        return;
    }
    activeTextEditor()->copy();
}

void EditorTab::paste()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused)
                (void)handleHexAsciiKeyPress(&event);
            else
                (void)handleHexKeyPress(&event);
            return;
        }
        m_editor->paste();
        return;
    }
    activeTextEditor()->paste();
}

void EditorTab::setFocusEditor()
{
    if (windowedReadOnlyActive()) {
        m_windowedReadOnlyProvider->editor()->setFocus();
        return;
    }
    if (isL2Mode() && m_l2View) {
        m_editor->setFocus();
        return;
    }
    activeTextEditor()->setFocus();
}

void EditorTab::gotoByteOffset(qint64 byteOffset, int selectLength)
{
    if (windowedReadOnlyActive()) {
        byteOffset = qBound<qint64>(
            0, byteOffset,
            qMax<qint64>(0, m_windowedReadOnlyProvider->fileSize() - 1));
        m_windowedReadOnlyProvider->goToOffset(byteOffset, selectLength);
        m_lastL2Byte = byteOffset;
        m_windowedReadOnlyProvider->editor()->setFocus();
        return;
    }
    if (isL2Mode() && m_l2View) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(flushError, 6000);
            return;
        }
        byteOffset = qBound<qint64>(0, byteOffset, m_l2View->virtualFileSize());
        m_l2View->syncActiveWindowToCursor(byteOffset);
        const qint64 local = qMax<qint64>(0, byteOffset - m_bridge.windowStart);
        auto overlayPosition = [this](qint64 localByte) -> long {
            if (m_l2View->viewMode() != ViewMode::Hex)
                return bridgeProjectedPositionForSourceOffset(localByte);
            const int line = int(qMin<qint64>(localByte / m_hexBytesPerRow, INT_MAX));
            const int column = int((localByte % m_hexBytesPerRow) * 3);
            return m_editor->positionFromLineIndex(line, column);
        };
        const long start = overlayPosition(local);
        if (selectLength > 0) {
            m_l2View->selectBytes(byteOffset, byteOffset + qint64(selectLength));
            const long end = overlayPosition(local + selectLength);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
        } else {
            m_l2View->gotoByteOffset(byteOffset, false);
            m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, start);
        }
        m_editor->ensureCursorVisible();
        m_editor->setFocus();
        return;
    }
    // L1：用 Scintilla SCI_GOTOPOS（字节偏移，UTF-8）定位，然后读取 line/col 更新
    if (byteOffset < 0) byteOffset = 0;
    if (byteOffset > INT_MAX) byteOffset = INT_MAX;
    if (selectLength > 0) {
        const qint64 end = qMin<qint64>(m_editor->length(),
                                        byteOffset + selectLength);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                static_cast<long>(byteOffset),
                                static_cast<long>(end));
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                static_cast<long>(byteOffset));
    }
    int line, col;
    m_editor->getCursorPosition(&line, &col);
    emit cursorPositionChanged(line, col);
}
