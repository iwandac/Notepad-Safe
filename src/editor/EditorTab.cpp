#include "EditorTab.h"
#include "EditorSurfaceAccessibility.h"
#include "EditorSurfaceStyle.h"
// Hex character rows use the shared measured Scintilla-authoritative grid.
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "ReadOnlyTailController.h"
#include "WindowedReadOnlyProvider.h"
#include "EditorSearchSource.h"
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

} // namespace



EditorTab::ExternalFileState EditorTab::externalFileState() const
{
    if (isSecureDocument() || m_documentSession.state().location.isEmpty())
        return ExternalFileState::NotTracked;
    return compareDiskFileVersion(m_documentSession.state().location, m_documentSession.diskVersion());
}

void EditorTab::refreshDiskVersionBaseline()
{
    m_documentSession.clearDiskVersion();
    if (isSecureDocument() || m_documentSession.state().location.isEmpty())
        return;
    DiskFileVersion captured;
    if (captureDiskFileVersion(m_documentSession.state().location, &captured) == ExternalFileState::Unchanged)
        m_documentSession.setDiskVersion(std::move(captured));
}

bool EditorTab::representsDiskFile(const DiskFileVersion &candidate) const
{
    if (isSecureDocument() || m_documentSession.state().location.isEmpty() || !candidate.valid)
        return false;
    if (sameDiskFile(m_documentSession.diskVersion(), candidate))
        return true;
    return sameDiskFilePath(m_documentSession.state().location, candidate.absolutePath);
}

EditorTab::EditorTab(QWidget *parent)
    : DocumentSearchSurface(parent), m_hexMouseSelection(this)
{
    m_searchSource = new EditorSearchSource(this, [this] {
        return EditorSearchSource::Resources{m_l2View, m_windowedReadOnlyProvider,
            m_l2Mode && m_l2ReadOnlyByPolicy && !isSecureDocument(),
            m_documentSession.state().location};
    });
    connect(m_searchSource, &EditorSearchSource::ready,
            this, [this] { emit l2FileOpenedChanged(true); });
    connect(m_searchSource, &EditorSearchSource::failed,
            this, [this](const QString &error) {
                m_lastSearchError = error;
                emit l2LineBuildFinished(false);
            });
    m_editor = new QsciScintilla(this);
    configurePrimaryEditorAccessibility(m_editor);
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
    m_searchSource->closeAndOwnerAlive();
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
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View && m_l2View->isOpen()) {
        m_l2View->closeFile(/*allowProcessEvents=*/false);
    }
}

bool EditorTab::openSurface(const DocumentSurfaceOpenRequest &request,
                            QString *error)
{
    if (!request.windowed)
        return loadFile(request.path, request.forcedEncoding);

    const bool binary = request.contentKind == DocumentContentKind::Binary;
    const bool hex = request.presentation == DocumentPresentationKind::Hex;
    const bool opened = openLargeFileL2(
        request.path, error,
        hex ? L2Presentation::Hex : L2Presentation::Text,
        binary ? L2ContentKind::Binary : L2ContentKind::Text,
        request.forcedEncoding, request.readPolicy, request.requireReadOnly);
    if (opened && hex)
        setViewModeL2(ViewMode::Hex);
    return opened;
}

void EditorTab::shutdownL2BeforeClose() {
    const QPointer<EditorTab> owner(this);
    if (!m_searchSource->closeAndOwnerAlive()) return;
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (!owner)
        return;
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
    if (!owner)
        return;
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
        Q_UNUSED(m);
        m_documentSession.setDirty(isModified());
        emit modificationChanged(isModified());
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
    m_documentSession.setContentKind(DocumentContentKind::Text);
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
    m_documentSession.setLocation(QFileInfo(path).absoluteFilePath());
    m_documentSession.clearRecoveryPath();  // 加载正式文件，清除 autosave 关联

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
    if (!m_searchSource->closeAndOwnerAlive()) return false;
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
    m_documentSession.setContentKind(DocumentContentKind::Text);
    if (data.size() >= FileTierPolicy::StandardEditLimitBytes)
        return false;

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    m_encodingOrigin = result.origin;
    m_encodingConfidence = result.confidence;
    m_encodingDecodeErrors = result.decodeErrors;
    m_editor->setEolMode(detectLineEndingMode(result.text));
    // 关键：不设置 m_documentSession.state().location，保持未命名状态，对用户透明
    m_documentSession.setRecoveryPath(backingPath);
    m_documentSession.clearDiskVersion();

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
            m_documentSession.setDirty(isModified());
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
        m_documentSession.setDirty(isModified());
        emit modificationChanged(isModified());
        emit titleChanged(tabTitle());
    }
}

bool EditorTab::isUntitled() const
{
    return m_documentSession.state().location.isEmpty();
}

DocumentState EditorTab::documentState() const noexcept
{
    DocumentState state = m_documentSession.state();
    // Dirty state is owned by the editing surface.  QScintilla, metadata and
    // windowed overlays can each change it, so snapshots must observe the
    // current aggregate instead of a stale cached transition.
    state.dirty = isModified();
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

QString EditorTab::filePath() const
{
    return m_documentSession.state().location;
}

void EditorTab::setFilePath(const QString &path)
{
    m_documentSession.bindPlainFile(path);
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
}

QString EditorTab::fileName() const
{
    if (isSecureDocument()) {
        if (m_documentSession.secureMasked())
            return QStringLiteral("已锁定文档");
        if (!m_documentSession.state().location.isEmpty())
            return QFileInfo(m_documentSession.state().location).fileName();
        return m_documentSession.secureTitle().isEmpty() ? QStringLiteral("未命名")
                                               : m_documentSession.secureTitle();
    }
    if (m_documentSession.state().location.isEmpty())
        return QStringLiteral("未命名");
    return QFileInfo(m_documentSession.state().location).fileName();
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
    if (!m_searchSource->closeAndOwnerAlive()) return;
    Q_ASSERT(!id.isNull());
    m_metadataModified = false;
    m_documentSession.setSecureTransitioning(true);
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_l2View && m_l2View->isOpen())
        m_l2View->closeFile(true);
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(0);
    m_l2Mode = false;
    m_documentSession.clearRecoveryPath();
    m_documentSession.clearSecureBackingPath();
    m_documentSession.bindSecureFile(id, filePath, DocumentContentKind::Text);
    m_documentSession.setSecureTitle(title.trimmed());
    m_documentSession.setSecureMasked(false);
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);
    m_documentSession.clearDiskVersion();
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
    m_documentSession.setSecureTransitioning(false);
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
    m_documentSession.setSecureTransitioning(true);
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    m_l2Mode = false;

    VirtualFileView *view = ensureL2View();
    QString localError;
    if (!view->openMemory(std::move(bytes), ViewMode::Hex, &localError)) {
        m_documentSession.setSecureTransitioning(false);
        if (error)
            *error = localError;
        return false;
    }
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);

    m_documentSession.clearRecoveryPath();
    m_documentSession.clearSecureBackingPath();
    m_documentSession.bindSecureFile(id, filePath, DocumentContentKind::Binary);
    m_documentSession.setSecureTitle(title.trimmed());
    m_documentSession.setSecureMasked(false);
    m_documentSession.clearDiskVersion();
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
    m_documentSession.setSecureTransitioning(false);
    emit l2ModeStatusChanged();
    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    emit cursorPositionChanged(0, 0);
    return true;
}

void EditorTab::setSecurePayloadLimitExclusive(qint64 limit)
{
    m_documentSession.setSecurePayloadLimitExclusive(qMax<qint64>(0, limit));
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(m_documentSession.securePayloadLimitExclusive());
}

void EditorTab::maskSecureDocument()
{
    if (!isSecureDocument() || m_documentSession.secureMasked())
        return;
    m_documentSession.setSecureTransitioning(true);
    m_documentSession.setSecureMasked(true);
    // A note title is commonly derived from its first plaintext line. Do not
    // leave that content resident or visible in the tab strip while locked;
    // the encrypted backing store restores it on the next unlock.
    m_documentSession.clearSecureTitle();
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
    m_documentSession.setSecureTransitioning(false);
    emit titleChanged(tabTitle());
}

void EditorTab::beginSecureDocumentLocking()
{
    if (!isSecureDocument() || m_documentSession.secureMasked() || m_documentSession.secureTransitioning())
        return;
    m_documentSession.setSecureTransitioning(true);
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
    if (!isSecureDocument() || m_documentSession.secureMasked() || !m_documentSession.secureTransitioning())
        return;
    m_documentSession.setSecureTransitioning(false);
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
    if (m_documentSession.secureTitle() == normalized)
        return;
    m_documentSession.setSecureTitle(normalized);
    emit titleChanged(tabTitle());
}

void EditorTab::convertToPlainDocument(const QString &filePath)
{
    m_metadataModified = false;
    m_documentSession.setSecureTransitioning(true);
    clearL2BridgeBaseline();
    m_documentSession.clearSecureTitle();
    m_documentSession.setSecureMasked(false);
    setSecurePayloadLimitExclusive(0);
    m_documentSession.bindPlainFile(filePath, true);
    m_documentSession.clearRecoveryPath();
    m_editor->setReadOnly(false);
    m_editor->setModified(false);
    m_documentSession.setSecureTransitioning(false);
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
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

// ============ autosave 自清理 ============

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
    if (m_windowedReadOnlyProvider && m_windowedReadOnlyProvider->editor())
        m_windowedReadOnlyProvider->editor()->setWhitespaceVisibility(
            on ? QsciScintilla::WsVisible : QsciScintilla::WsInvisible);
}

void EditorTab::setEolVisible(bool on)
{
    m_eolVisible = on;
    if (m_editor)
        m_editor->setEolVisibility(on);
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
}

void EditorTab::setTabWidth(int columns)
{
    m_tabWidth = qBound(1, columns, 16);
    if (m_editor)
        m_editor->setTabWidth(m_tabWidth);
}

void EditorTab::setBackspaceUnindents(bool on)
{
    m_backspaceUnindents = on;
    if (m_editor)
        m_editor->setBackspaceUnindents(on);
}

void EditorTab::setWordCompletionEnabled(bool on)
{
    m_wordCompletionEnabled = on;
    if (!m_editor)
        return;
    m_editor->setAutoCompletionSource(on ? QsciScintilla::AcsAll
                                         : QsciScintilla::AcsNone);
    m_editor->setAutoCompletionThreshold(on ? 3 : -1);
}

QsciScintilla *EditorTab::activeTextEditor() const
{
    return m_editor;
}

QWidget *EditorTab::l1Page() const
{
    return m_editor;
}

bool EditorTab::canSwitchWindowedPresentation() const
{
    return windowedReadOnlyActive() && !isBinaryDocument();
}

bool EditorTab::reopenWindowedPresentation(ViewMode target, QString *error)
{
    if (!canSwitchWindowedPresentation()) {
        if (error)
            *error = QObject::tr("只有只读大文本文件可以在文本与十六进制查看之间切换。");
        return false;
    }
    const QPointer<EditorTab> owner(this);
    if (auto *tail = m_windowedReadOnlyProvider->tailController())
        tail->setTailEnabled(false);
    if (!owner || !m_windowedReadOnlyProvider || viewModeL2() == target)
        return owner && viewModeL2() == target;
    const qint64 top = m_windowedReadOnlyProvider->visibleTopOffset();
    setViewModeL2(target);
    if (owner && m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->goToOffset(top);
    return owner && viewModeL2() == target;
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
        const QPointer<EditorTab> owner(this);
        if (m_windowedReadOnlyProvider)
            m_windowedReadOnlyProvider->setTextEncoding(enc);
        if (!owner) return;
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
        // Secure Text is a logical UTF-8 payload; m_documentSession.state().location points to its
        // encrypted KDBX container and must never be decoded as document text.
        if (m_documentSession.state().location.isEmpty() || isSecureDocument()) return;
        QFile f(m_documentSession.state().location);
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
    if (m_documentSession.secureMasked() || isSecureDocumentTransitioning()
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
    if (m_documentSession.secureMasked())
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
    if (windowedReadOnlyActive() || m_searchSource->usesWindowedSource()) {
        const QPointer<EditorTab> owner(this);
        if (!validateSearchSource()) return;
        byteOffset = qBound<qint64>(
            0, byteOffset,
            qMax<qint64>(0, m_windowedReadOnlyProvider->fileSize() - 1));
        m_windowedReadOnlyProvider->goToOffset(byteOffset, selectLength);
        if (!owner || !validateSearchSource()) return;
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
