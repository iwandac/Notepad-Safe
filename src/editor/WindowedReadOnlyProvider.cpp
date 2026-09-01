#include "WindowedReadOnlyProvider.h"
#include "EditorSurfaceStyle.h"
#include "WindowedHexMouseSelectionController.h"
// Windowed Hex character rows use the measured B1 Hex grid.
#include "ByteClipboard.h"
#include "ReadOnlyTextAnalysis.h"
#include "ReadOnlyTailController.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <QApplication>
#include <QByteArrayView>
#include <QClipboard>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "Sci_Position.h"
#include "AsyncTaskManager.h"
#include "SharedReadOnlyFile.h"

namespace {

constexpr int kSliderMaximum = 1'000'000;
constexpr qint64 kHexWindowBytes = 256LL * 1024;
// One MiB is ample for the visible viewport and keeps the first projection
// comfortably interactive even for a multi-GiB file containing one enormous
// physical line. Search and indexing read the source directly; large
// selections may still expand independently.
constexpr qint64 kTextWindowBytes = 1LL * 1024 * 1024;
// Completing a physical line is best-effort. An untrusted log can contain a
// multi-GiB line, so line alignment must not turn a bounded preview into a
// large synchronous UI allocation. Explicit selections are sized separately.
constexpr qint64 kMaxTextProjectionBytes = 1LL * 1024 * 1024;
constexpr qint64 kMaxClipboardBytes = 64LL * 1024 * 1024;
using ReadOnlyTextAnalysis::countPhysicalLineBreaks;
using ReadOnlyTextAnalysis::PrefixChunkBytes;

QString sourceReadMessage(const OrdinaryReadResult &result)
{
    if (result.status == OrdinaryReadStatus::SecureType)
        return QObject::tr("源文件已变成安全文件，已停止普通读取。请从“打开”重新打开该文件。");
    if (result.status == OrdinaryReadStatus::Changed)
        return QObject::tr("源文件已在磁盘上变化，旧结果已失效；请等待只读视图刷新后重新查找。");
    if (result.status == OrdinaryReadStatus::PolicyUnavailable)
        return QObject::tr("未配置文件安全读取规则，已拒绝普通读取。请重新打开文件。");
    return QObject::tr("源文件暂时无法读取，旧结果已失效。请检查文件后重新打开。");
}

struct SourceChunk {
    QByteArray bytes;
    OrdinaryReadResult admission;
};

SourceChunk readSourceChunk(const QString &path, const PersistenceReadPolicy &policy,
                           const DiskFileStamp &expected, qint64 offset, qsizetype count)
{
    SharedReadOnlyFile reader;
    SourceChunk chunk{{}, openOrdinaryReadSource(reader, path, policy, &expected)};
    if (!chunk.admission.isReady()) return chunk;
    chunk.bytes = reader.readAt(offset, count, &chunk.admission.error);
    DiskFileStamp afterRead;
    reader.captureStamp(&afterRead);
    captureDiskFileStamp(path, &chunk.admission.stamp);
    if (!sameDiskFileStamp(expected, afterRead)
        || !sameDiskFileStamp(expected, chunk.admission.stamp))
        chunk.admission.status = OrdinaryReadStatus::Changed;
    else if (chunk.bytes.size() != count)
        chunk.admission.status = OrdinaryReadStatus::Unreadable;
    if (!chunk.admission.isReady()) chunk.bytes.clear();
    return chunk;
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

qint64 clampOffset(qint64 offset, qint64 size)
{
    if (size <= 0)
        return 0;
    return std::clamp(offset, 0LL, size - 1);
}

} // namespace

bool ownsWindowedReadOnlyInputObject(const WindowedReadOnlyProvider *provider,
                                     const QObject *object)
{
    if (!provider || !object)
        return false;
    const QPlainTextEdit *ascii = provider->asciiPreview();
    return object == ascii || (ascii && object == ascii->viewport());
}

WindowedReadOnlyProvider::WindowedReadOnlyProvider(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("windowedReadOnlySlice"));
    installEventFilter(this);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_editor = new QsciScintilla(this);
    m_editor->setObjectName(QStringLiteral("windowProjectionEditor"));
    m_editor->setAccessibleName(tr("只读文档内容"));
    m_editor->setAccessibleDescription(
        tr("当前窗口中的只读文本或十六进制字节"));
    applySharedEditorSurfaceStyle(m_editor);
    m_editor->setUtf8(true);
    m_editor->setReadOnly(true);
    m_editor->setWrapMode(QsciScintilla::WrapNone);
    m_editor->setFolding(QsciScintilla::NoFoldStyle);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 1UL);
    m_editor->installEventFilter(this);
    if (m_editor->viewport())
        m_editor->viewport()->installEventFilter(this);

    m_asciiPreview = new QPlainTextEdit(this);
    m_asciiPreview->setObjectName(QStringLiteral("HexAsciiPreview"));
    m_asciiPreview->setAccessibleName(tr("编码字符预览"));
    m_asciiPreview->setAccessibleDescription(
        tr("与十六进制字节同步的只读字符表面"));
    m_asciiPreview->setToolTip(
        tr("字符栏（按当前编码进行多字节解码；只读）"));
    m_asciiPreview->setFont(m_editor->font());
    m_asciiPreview->setReadOnly(true);
    m_asciiPreview->setAttribute(Qt::WA_InputMethodEnabled, false);
    m_asciiPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_asciiPreview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_asciiPreview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_asciiPreview->document()->setDocumentMargin(0);
    m_asciiPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { border: 0; border-left: 1px solid palette(mid);"
        " padding-left: 8px; }"));
    m_asciiPreview->installEventFilter(this);
    if (m_asciiPreview->viewport())
        m_asciiPreview->viewport()->installEventFilter(this);
    connect(m_asciiPreview, &QPlainTextEdit::cursorPositionChanged,
            this, &WindowedReadOnlyProvider::syncHexSelectionFromAscii);
    connect(m_asciiPreview, &QPlainTextEdit::selectionChanged,
            this, &WindowedReadOnlyProvider::syncHexSelectionFromAscii);
    m_asciiPreview->hide();
    connect(m_editor, &QsciScintilla::SCN_UPDATEUI,
            this, [this]() {
        syncAsciiSelectionFromHex();
        syncAsciiScrollFromHex();
    });
    connect(m_editor, &QsciScintillaBase::SCN_PAINTED, this, [this]() {
        if (!m_scrollToEndAfterPaint)
            return;
        // Scintilla finalizes viewport metrics during paint. setText-time
        // SCI_LINESONSCREEN can therefore be stale even in physical-line
        // mode, leaving the EOF window loaded but its viewport several rows
        // above the actual end. Calibrate once after the first real paint.
        m_scrollToEndAfterPaint = false;
        scrollProjectionToEnd();
        m_globalOffset = std::max<qint64>(0, m_fileSize - 1);
        m_updatingScrollBar = true;
        m_globalScrollBar->setValue(m_globalScrollBar->maximum());
        m_updatingScrollBar = false;
    });
    connect(m_editor, &QsciScintilla::selectionChanged, this, [this]() {
        if (m_presentation == Presentation::Text
            && !m_applyingSourceSelection) {
            // A native user selection is the new authority. Search navigation,
            // Select All and cross-window keyboard selection may have installed
            // a source-byte virtual selection; retaining it here would make
            // every later Copy read the first selection instead of the visible
            // one.
            clearVirtualSelection();
        }
        syncAsciiSelectionFromHex();
    });
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, [this](int, int) { syncAsciiSelectionFromHex(); });

    m_hexMouseSelection =
        std::make_unique<WindowedHexMouseSelectionController>(this);

    m_globalScrollBar = new QScrollBar(Qt::Vertical, this);
    m_globalScrollBar->setObjectName(QStringLiteral("globalLargeFileScrollBar"));
    m_globalScrollBar->setAccessibleName(tr("文档全局垂直位置"));
    m_globalScrollBar->setAccessibleDescription(
        tr("在整个只读大文件中上下移动"));
    m_globalScrollBar->setFocusPolicy(Qt::StrongFocus);
    m_globalScrollBar->setRange(0, kSliderMaximum);
    m_globalScrollBar->setSingleStep(1000);
    m_globalScrollBar->setPageStep(10000);
    connect(m_globalScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_updatingScrollBar && m_fileSize > 0)
            reloadWindow(offsetForSlider(value), 0);
    });

    layout->addWidget(m_editor, 3);
    layout->addWidget(m_asciiPreview, 1);
    layout->addWidget(m_globalScrollBar);
    m_tailController = new ReadOnlyTailController(this);
    outer->addWidget(m_tailController);
    outer->addLayout(layout, 1);

    m_fileWatcher = new QFileSystemWatcher(this);
    m_remapDebounce = new QTimer(this);
    m_remapDebounce->setSingleShot(true);
    m_remapDebounce->setInterval(200);
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString &) { observeFileChange(); });
    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString &) { observeFileChange(); });
    connect(m_remapDebounce, &QTimer::timeout, this, [this]() {
        const QPointer<WindowedReadOnlyProvider> self(this);
        QString error;
        if (!refreshFileMapping(&error) && self && !error.isEmpty())
            emit sourceReloadFailed(error);
    });
    // Windows directory notifications do not report ordinary in-place appends.
    // A cheap metadata poll covers growing logs; directoryChanged remains the
    // fast path for rename/atomic replacement.
    m_filePoll = new QTimer(this);
    m_filePoll->setInterval(500);
    connect(m_filePoll, &QTimer::timeout, this, &WindowedReadOnlyProvider::observeFileChange);
}

DiskFileStamp WindowedReadOnlyProvider::sourceStamp() const
{
    return {m_sourceVersion.valid, m_sourceVersion.size,
            m_sourceVersion.modifiedMs, m_sourceVersion.fileIdentity};
}

void WindowedReadOnlyProvider::observeSourceStamp(const DiskFileStamp &stamp, DiskFileState state)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    const quint64 observedGeneration = m_searchGeneration;
    m_tailController->observeSource(stamp, state);
    if (!self || m_searchGeneration != observedGeneration) return;
    if (sameDiskFileStamp(stamp, m_observedStamp)
        || (!stamp.valid && !m_observedStamp.valid)) return;
    m_observedStamp = stamp;
    const quint64 generation = ++m_searchGeneration;
    ++m_lineNumberGeneration;
    const qint64 searchTask = std::exchange(m_searchTaskId, -1);
    const qint64 lineTask = std::exchange(m_lineNumberTaskId, -1);
    m_searchResultPending = false;
    m_lineNumberPending = m_characterCountPending = false;
    m_characterCount = -1;
    // Invalidate the document revision/Dock before cancellation can deliver an
    // old terminal, and before a debounced refresh reads any new body bytes.
    emit sourceFileChanged(m_fileSize, stamp.valid ? stamp.size : 0);
    if (searchTask >= 0) AsyncTaskManager::instance().cancel(searchTask);
    if (lineTask >= 0) AsyncTaskManager::instance().cancel(lineTask);
    if (!self || m_searchGeneration != generation) return;
    m_remapDebounce->start();
    emit characterCountChanged(-1, false);
}

void WindowedReadOnlyProvider::observeFileChange()
{
    if (m_path.isEmpty()) return;
    DiskFileStamp stamp;
    const auto state = captureDiskFileStamp(m_path, &stamp);
    observeSourceStamp(stamp, state);
}

void WindowedReadOnlyProvider::reportSourceFailure(const OrdinaryReadResult &result)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    const quint64 generation = m_searchGeneration;
    m_tailController->sourceFailed(result);
    if (!self || m_searchGeneration != generation) return;
    observeSourceStamp(result.status == OrdinaryReadStatus::Unreadable ? DiskFileStamp{} : result.stamp);
    if (self) emit sourceReloadFailed(sourceReadMessage(result));
}

bool WindowedReadOnlyProvider::validateCurrent(QString *error)
{
    SharedReadOnlyFile reader;
    const auto expected = sourceStamp();
    const auto result = openOrdinaryReadSource(reader, m_path, m_readPolicy, &expected);
    if (result.isReady()) return true;
    if (error) *error = sourceReadMessage(result);
    reportSourceFailure(result);
    return false;
}

WindowedReadOnlyProvider::~WindowedReadOnlyProvider()
{
    closeFile();
}

bool WindowedReadOnlyProvider::openFile(const QString &path, Presentation presentation,
    const PersistenceReadPolicy &policy, QString *error,
    std::optional<TextEncoding> forcedEncoding, EncodingDetection *detection)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    const quint64 closingGeneration = m_searchGeneration + 1;
    closeFile();
    if (!self || m_searchGeneration != closingGeneration) return false;
    m_path = QFileInfo(path).absoluteFilePath();
    m_readPolicy = policy;
    SharedReadOnlyFile reader;
    const auto admitted = openOrdinaryReadSource(reader, m_path, m_readPolicy);
    QString openError;
    if (!admitted.isReady()) {
        if (error) *error = sourceReadMessage(admitted);
        return false;
    }
    const auto detected = forcedEncoding || m_binaryContent ? EncodingDetection{}
        : detectFileEncoding(reader.size(), [&reader](qint64 offset, qsizetype count) {
            return reader.readAt(offset, count);
        });
    if (!reader.captureVersion(&m_sourceVersion, &openError)
        || !sameDiskFileStamp(sourceStamp(), admitted.stamp)) {
        if (error) *error = tr("源文件读取期间发生变化或无法读取，请重试。");
        return false;
    }
    m_fileSize = m_sourceVersion.size;
    m_observedStamp = sourceStamp();
    m_textEncoding = forcedEncoding.value_or(m_binaryContent ? TextEncoding::Utf8 : detected.encoding);
    if (detection) *detection = detected;
    // The windowed provider intentionally keeps no persistent source handle.
    // This lets other editors atomically replace the path and lets log rotators
    // rename it while the projected text remains visible in Scintilla.
    reader.close();
    m_presentation = presentation;
    m_editor->setAttribute(Qt::WA_InputMethodEnabled,
                           presentation == Presentation::Text);
    m_windowSpan = presentation == Presentation::Hex ? kHexWindowBytes : kTextWindowBytes;
    m_asciiPreview->setVisible(presentation == Presentation::Hex);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR,
                            presentation == Presentation::Hex ? 0UL : 1UL);
    m_editor->setWrapMode(presentation == Presentation::Text && m_wordWrap
                              ? QsciScintilla::WrapWord
                              : QsciScintilla::WrapNone);
    if (presentation == Presentation::Hex)
        m_hexBytesPerRow = calculateHexBytesPerRow();
    m_globalScrollBar->setPageStep(std::max(
        1, int((static_cast<long double>(m_windowSpan) * kSliderMaximum) / qMax<qint64>(1, m_fileSize))));
    if (!reloadWindow(0, 0, -1, false, error) || !self || !validateCurrent(error)) return false;
    m_filePoll->start();
    startLineNumberIndex();
    return true;
}

void WindowedReadOnlyProvider::closeFile()
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    const quint64 generation = ++m_searchGeneration;
    m_statisticsDeferred = false;
    m_tailController->reset();
    if (!self || m_searchGeneration != generation) return;
    if (m_remapDebounce)
        m_remapDebounce->stop();
    if (m_filePoll)
        m_filePoll->stop();
    if (m_hexMouseSelection)
        m_hexMouseSelection->reset();
    if (m_fileWatcher && !m_fileWatcher->files().isEmpty())
        m_fileWatcher->removePaths(m_fileWatcher->files());
    if (m_fileWatcher && !m_fileWatcher->directories().isEmpty())
        m_fileWatcher->removePaths(m_fileWatcher->directories());
    const qint64 searchTask = std::exchange(m_searchTaskId, -1);
    m_searchResultPending = false;
    cancelLineNumberIndex();
    if (searchTask >= 0) AsyncTaskManager::instance().cancel(searchTask);
    if (!self || m_searchGeneration != generation) return;
    m_fileSize = 0;
    m_globalOffset = 0;
    m_windowStart = 0;
    m_windowEnd = 0;
    clearVirtualSelection();
    m_path.clear();
    if (m_asciiPreview)
        m_asciiPreview->clear();
    m_observedStamp = {};
    m_sourceVersion = {};
    m_readPolicy = {};
    m_characterCount = -1;
    m_characterCountPending = false;
    m_lineNumberPending = false;
    m_lineBreakPrefix.clear();
    m_lineBreakPrefixEndsWithCr.clear();
}

bool WindowedReadOnlyProvider::refreshFileMapping(QString *error)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    if (m_path.isEmpty()) {
        if (error)
            *error = tr("没有可重新加载的源文件。");
        return false;
    }
    observeFileChange();
    if (!self) return false;
    const quint64 generation = m_searchGeneration;
    m_remapDebounce->stop();
    const qint64 preservedTop = visibleTopOffset();
    const qint64 preservedAnchor = m_globalOffset;
    const int preservedHorizontal = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETXOFFSET));

    SharedReadOnlyFile reader;
    const auto admitted = openOrdinaryReadSource(reader, m_path, m_readPolicy);
    QString openError;
    if (!admitted.isReady()) {
        if (error) *error = sourceReadMessage(admitted);
        reportSourceFailure(admitted);
        return false;
    }
    DiskFileVersion version;
    DiskFileStamp current;
    const auto candidate = m_tailController->prepareCandidate(admitted.stamp,
        [&reader](qint64 offset, qsizetype count) { return reader.readAt(offset, count); });
    if (!candidate.readable) {
        auto failure = admitted;
        failure.status = OrdinaryReadStatus::Unreadable;
        if (error) *error = sourceReadMessage(failure);
        reportSourceFailure(failure);
        return false;
    }
    if (!reader.captureVersion(&version, &openError)
        || !reader.captureStamp(&current, &openError)
        || !sameDiskFileStamp(current, admitted.stamp)) {
        if (error) *error = tr("读取期间源文件再次变化，未接纳新内容。请重试。");
        observeFileChange();
        return false;
    }
    reader.close();
    captureDiskFileStamp(m_path, &current);
    if (!sameDiskFileStamp(current, admitted.stamp)) {
        if (error) *error = tr("接纳前源文件再次变化，未接纳新内容。请重试。");
        observeSourceStamp(current);
        return false;
    }
    if (!self || m_searchGeneration != generation) return false;
    m_sourceVersion = std::move(version);
    m_fileSize = m_sourceVersion.size;
    m_observedStamp = admitted.stamp;
    emit sourceVersionAccepted(m_path, m_sourceVersion);
    if (!self || m_searchGeneration != generation || !validateCurrent(error)
        || !self || m_searchGeneration != generation) return false;
    m_globalScrollBar->setEnabled(m_fileSize > 0);
    m_globalScrollBar->setPageStep(m_fileSize > 0
        ? std::max(1, int((static_cast<long double>(m_windowSpan) * kSliderMaximum)
                          / m_fileSize))
        : kSliderMaximum);
    const auto projection = m_tailController->acceptCandidate(candidate, error);
    if (projection != ReadOnlyTailController::Projection::Refresh)
        return self && m_searchGeneration == generation
            && projection == ReadOnlyTailController::Projection::Preserved;
    if (!self || m_searchGeneration != generation) return false;

    if (m_selectionAuthority == SelectionAuthority::SourceBytes) {
        m_sourceSelection.selectEndpoints(
            {std::clamp(m_sourceSelection.anchor().value, 0LL, m_fileSize)},
            {std::clamp(m_sourceSelection.caret().value, 0LL, m_fileSize)});
    }
    if (m_fileSize > 0) {
        if (!reloadWindow(clampOffset(preservedAnchor, m_fileSize), 0,
                          std::clamp(preservedTop, 0LL, m_fileSize - 1), false, error)) return false;
        if (!self || m_searchGeneration != generation) return false;
        directScintilla(m_editor, QsciScintillaBase::SCI_SETXOFFSET,
                        static_cast<ScintillaUnsigned>(
                            m_presentation == Presentation::Hex
                                ? 0 : std::max(0, preservedHorizontal)));
    } else {
        m_windowStart = 0;
        m_windowEnd = 0;
        m_globalOffset = 0;
        m_editor->setReadOnly(false);
        m_editor->clear();
        m_editor->setReadOnly(true);
        m_updatingScrollBar = true;
        m_globalScrollBar->setValue(0);
        m_updatingScrollBar = false;
    }
    if (!self || !validateCurrent(error)) return false;
    startLineNumberIndex();
    return true;
}

void WindowedReadOnlyProvider::setPresentation(Presentation presentation)
{
    if (m_presentation == presentation || m_path.isEmpty())
        return;
    m_presentation = presentation;
    m_editor->setAttribute(Qt::WA_InputMethodEnabled,
                           presentation == Presentation::Text);
    m_windowSpan = presentation == Presentation::Hex ? kHexWindowBytes : kTextWindowBytes;
    m_asciiPreview->setVisible(presentation == Presentation::Hex);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR,
                            presentation == Presentation::Hex ? 0UL : 1UL);
    m_editor->setWrapMode(presentation == Presentation::Text && m_wordWrap
                              ? QsciScintilla::WrapWord
                              : QsciScintilla::WrapNone);
    if (presentation == Presentation::Hex)
        m_hexBytesPerRow = calculateHexBytesPerRow();
    m_globalScrollBar->setPageStep(std::max(
        1, int((static_cast<long double>(m_windowSpan) * kSliderMaximum) / m_fileSize)));
    const QPointer<WindowedReadOnlyProvider> self(this);
    if (!reloadWindow(m_globalOffset, 0) || !self) return;
    if (presentation == Presentation::Text && !m_binaryContent) {
        if ((m_lineBreakPrefix.isEmpty() || m_characterCount < 0)
            && !m_lineNumberPending)
            startLineNumberIndex();
    }
}

void WindowedReadOnlyProvider::setBinaryContent(bool binary)
{
    if (m_binaryContent == binary)
        return;
    m_binaryContent = binary;
    if (binary) {
        cancelLineNumberIndex();
        m_lineBreakPrefix.clear();
        m_lineBreakPrefixEndsWithCr.clear();
        m_characterCount = -1;
        emit characterCountChanged(-1, false);
    } else if (m_presentation == Presentation::Text && !m_path.isEmpty()) {
        startLineNumberIndex();
    }
    if (!m_path.isEmpty())
        reloadWindow(m_globalOffset, 0, visibleTopOffset(), true);
}

void WindowedReadOnlyProvider::setTextEncoding(TextEncoding encoding)
{
    if (m_textEncoding == encoding)
        return;
    m_textEncoding = encoding;
    if (!m_path.isEmpty()) {
        const QPointer<WindowedReadOnlyProvider> self(this);
        if (!reloadWindow(m_globalOffset, 0, visibleTopOffset(), true) || !self) return;
        if (m_presentation == Presentation::Text && !m_binaryContent) {
            startLineNumberIndex();
        }
    }
}

void WindowedReadOnlyProvider::setWordWrap(bool enabled)
{
    Q_UNUSED(enabled);
    // This provider exclusively serves S2/S3. Word wrapping a bounded 2 MiB
    // projection makes global scroll positions depend on lazy local layout
    // and causes expensive reload/reflow cycles during thumb dragging.
    if (!m_wordWrap && m_editor->wrapMode() == QsciScintilla::WrapNone)
        return;
    const qint64 preservedTop = visibleTopOffset();
    m_wordWrap = false;
    m_editor->setWrapMode(QsciScintilla::WrapNone);
    m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
    m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    if (!m_path.isEmpty())
        reloadWindow(m_globalOffset, 0, preservedTop, true);
}

QByteArray WindowedReadOnlyProvider::sourceBytes(qint64 offset, qsizetype length, QString *error) const
{
    if (m_path.isEmpty() || offset < 0 || offset >= m_fileSize || length <= 0)
        return {};
    const qint64 available = std::min<qint64>(length, m_fileSize - offset);
    const auto read = readSourceChunk(m_path, m_readPolicy, sourceStamp(), offset, available);
    const auto &result = read.admission;
    if (result.isReady()) return read.bytes;
    if (error) *error = sourceReadMessage(result);
    const QPointer<WindowedReadOnlyProvider> guard(const_cast<WindowedReadOnlyProvider *>(this));
    QMetaObject::invokeMethod(&AsyncTaskManager::instance(),
        [guard, result, generation = m_searchGeneration] {
            if (guard && guard->m_searchGeneration == generation) guard->reportSourceFailure(result);
        }, Qt::QueuedConnection);
    return {};
}

int WindowedReadOnlyProvider::visibleVerticalScrollBarCount() const
{
    int count = 0;
    const auto bars = findChildren<QScrollBar *>();
    for (const QScrollBar *bar : bars) {
        if (bar->orientation() == Qt::Vertical && bar->isVisibleTo(this))
            ++count;
    }
    return count;
}

qint64 WindowedReadOnlyProvider::visibleTopOffset() const
{
    if (m_path.isEmpty() || !m_editor)
        return 0;
    const qint64 precise = sourceOffsetAtViewportY(1);
    if (precise >= 0)
        return precise;
    const int firstVisible = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    return sourceOffsetForProjectedLine(firstVisible);
}

qint64 WindowedReadOnlyProvider::visibleBottomOffset() const
{
    if (m_path.isEmpty() || !m_editor || !m_editor->viewport())
        return 0;
    if (m_presentation == Presentation::Text) {
        if (m_windowEnd == m_fileSize) {
            // SCI_LINESONSCREEN counts fully visible rows while Scintilla's
            // native maximum may deliberately leave the final document row
            // only partially visible. A line-count reconstruction therefore
            // reports a false gap at EOF. Query the painted position of the
            // actual document end instead; this is also what the user sees.
            const qint64 documentEnd = directScintilla(
                m_editor, QsciScintillaBase::SCI_GETLENGTH);
            const int endY = static_cast<int>(directScintilla(
                m_editor, QsciScintillaBase::SCI_POINTYFROMPOSITION, 0,
                static_cast<ScintillaSigned>(documentEnd)));
            if (endY >= 0 && endY < m_editor->viewport()->height())
                return m_windowEnd;
        }
        const int firstVisible = static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const int linesOnScreen = std::max(1, static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
        if (firstVisible + linesOnScreen >= projectedDisplayLineCount())
            return m_windowEnd;
        // Bottom visibility means the furthest source character painted in the
        // viewport, not the start of its last display row. With wrapping, the
        // old left-edge probe under-reported by up to one complete wrapped row
        // even when the physical EOF marker was already on screen.
        const qint64 position = directScintilla(
            m_editor, QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
            static_cast<ScintillaUnsigned>(
                std::max(2, m_editor->viewport()->width() - 2)),
            static_cast<ScintillaSigned>(
                std::max(1, m_editor->viewport()->height() - 2)));
        if (position >= 0)
            return sourceOffsetForProjectedPosition(position, true);
    }
    const qint64 offset = sourceOffsetAtViewportY(
        std::max(1, m_editor->viewport()->height() - 2));
    return offset >= 0 ? offset : visibleTopOffset();
}

qint64 WindowedReadOnlyProvider::selectionStartOffset() const
{
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        return m_sourceSelection.selection()
            ? m_sourceSelection.selection()->start.value : 0;
    if (m_presentation == Presentation::Hex && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()))) {
        const QTextCursor cursor = m_asciiPreview->textCursor();
        return std::min(sourceOffsetForAsciiPosition(cursor.anchor(), false),
                        sourceOffsetForAsciiPosition(cursor.position(), false));
    }
    const qint64 start = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETSELECTIONSTART);
    return sourceOffsetForProjectedPosition(start, false);
}

qint64 WindowedReadOnlyProvider::selectionEndOffset() const
{
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        return m_sourceSelection.selection()
            ? m_sourceSelection.selection()->end.value : 0;
    if (m_presentation == Presentation::Hex && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()))) {
        const QTextCursor cursor = m_asciiPreview->textCursor();
        return std::max(sourceOffsetForAsciiPosition(cursor.anchor(), true),
                        sourceOffsetForAsciiPosition(cursor.position(), true));
    }
    const qint64 start = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETSELECTIONSTART);
    const qint64 end = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETSELECTIONEND);
    if (start == end)
        return sourceOffsetForProjectedPosition(end, false);
    return sourceOffsetForProjectedPosition(end, true);
}

qint64 WindowedReadOnlyProvider::selectionAnchorOffset() const
{
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        return m_sourceSelection.anchor().value;
    if (m_presentation == Presentation::Hex && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()))) {
        const QTextCursor cursor = m_asciiPreview->textCursor();
        const bool forward = cursor.anchor() <= cursor.position();
        return sourceOffsetForAsciiPosition(cursor.anchor(), !forward);
    }
    const qint64 anchor = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETANCHOR);
    const qint64 caret = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
    if (anchor == caret)
        return sourceOffsetForProjectedPosition(anchor, false);
    if (m_presentation == Presentation::Hex)
        return sourceBoundaryForHexPosition(anchor);
    return sourceOffsetForProjectedPosition(anchor, anchor > caret);
}

qint64 WindowedReadOnlyProvider::selectionCaretOffset() const
{
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        return m_sourceSelection.caret().value;
    if (m_presentation == Presentation::Hex && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()))) {
        const QTextCursor cursor = m_asciiPreview->textCursor();
        const bool forward = cursor.anchor() <= cursor.position();
        return sourceOffsetForAsciiPosition(cursor.position(), forward);
    }
    const qint64 anchor = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETANCHOR);
    const qint64 caret = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
    if (anchor == caret)
        return sourceOffsetForProjectedPosition(caret, false);
    if (m_presentation == Presentation::Hex)
        return sourceBoundaryForHexPosition(caret);
    return sourceOffsetForProjectedPosition(caret, caret > anchor);
}

bool WindowedReadOnlyProvider::copySelection(QString *error) const
{
    const qint64 start = selectionStartOffset();
    const qint64 end = selectionEndOffset();
    if (end <= start)
        return false;
    const qint64 length = end - start;
    if (!ByteClipboard::payloadWithinLimit(length, error)) {
        return false;
    }
    QByteArray selected = sourceBytes(start, static_cast<qsizetype>(length), error);
    if (selected.size() != length) return false;
    const bool asciiFocused = m_presentation == Presentation::Hex
        && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()));
    if (m_presentation == Presentation::Hex && !asciiFocused) {
        ByteClipboard::setHexSelection(selected);
    } else if (m_presentation == Presentation::Hex) {
        ByteClipboard::setCharacterSelection(selected, m_textEncoding);
    } else {
        QGuiApplication::clipboard()->setText(
            decodeTextProjection(selected, m_textEncoding, false, false).text);
    }
    return true;
}

void WindowedReadOnlyProvider::goToOffset(qint64 offset, qint64 selectionBytes)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    m_tailController->pauseForReading();
    if (!self || m_path.isEmpty() || !validateCurrent() || !self)
        return;
    if (selectionBytes > 0) {
        m_selectionAuthority = SelectionAuthority::SourceBytes;
        const qint64 anchor = clampOffset(offset, m_fileSize);
        m_sourceSelection.selectEndpoints(
            {anchor}, {std::min(m_fileSize, anchor + selectionBytes)});
    } else {
        clearVirtualSelection();
    }
    reloadWindow(clampOffset(offset, m_fileSize), selectionBytes);
}

void WindowedReadOnlyProvider::selectAllSource()
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    m_tailController->pauseForReading();
    if (!self) return;
    if (m_path.isEmpty() || m_fileSize <= 0)
        return;
    m_selectionAuthority = SelectionAuthority::SourceBytes;
    m_sourceSelection.selectEndpoints({0}, {m_fileSize});
    applyVirtualSelection();
}

void WindowedReadOnlyProvider::findBytesAsync(const QByteArray &needle, qint64 fromOffset)
{
    if (m_path.isEmpty() || needle.isEmpty() || m_searchResultPending || !validateCurrent())
        return;
    m_searchResultPending = true;
    m_lastSearchResult = -1;
    m_searchNeedle = needle;
    const qint64 size = m_fileSize;
    const QString path = m_path;
    const auto policy = m_readPolicy;
    const auto expected = sourceStamp();
    const qint64 begin = std::clamp(fromOffset, 0LL, size);
    QPointer<WindowedReadOnlyProvider> guard(this);
    const quint64 generation = ++m_searchGeneration;
    auto *dispatcher = &AsyncTaskManager::instance();
    const qint64 submittedId = dispatcher->submit(
        AsyncTaskType::LargeFileSearch,
        AsyncTaskPriority::High,
        tr("搜索当前大文件"),
        tr("在只读窗口源中查找下一处字节序列。"),
        [guard, generation, path, size, begin, needle, policy, expected, dispatcher]
        (qint64, AsyncCancelPtr cancel,
         std::function<void(qint64, qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo &)> reportError) {
        const auto clearPending = qScopeGuard([guard, generation, dispatcher] {
            QMetaObject::invokeMethod(dispatcher, [guard, generation] {
                if (!guard || guard->m_searchGeneration != generation) return;
                guard->m_searchResultPending = false;
                guard->m_searchTaskId = -1;
            }, Qt::QueuedConnection);
        });
        const qint64 last = size - needle.size();
        qint64 result = -1;
        constexpr qint64 chunkSize = 4LL * 1024 * 1024;
        const qint64 overlap = std::max<qint64>(0, qint64(needle.size()) - 1);
        qint64 chunkStart = begin;
        while (chunkStart <= last) {
            if (cancel && cancel->load(std::memory_order_acquire))
                return;
            const qint64 wanted = std::min<qint64>(
                size - chunkStart, chunkSize + overlap);
            const auto read = readSourceChunk(path, policy, expected, chunkStart, wanted);
            const auto &admission = read.admission;
            if (!admission.isReady()) {
                AsyncErrorInfo error;
                error.message = QObject::tr("读取只读查找源失败：%1").arg(admission.error);
                error.userMessage = sourceReadMessage(admission);
                if (reportError) reportError(error);
                QMetaObject::invokeMethod(dispatcher, [guard, generation, admission] {
                    if (!guard || guard->m_searchGeneration != generation) return;
                    guard->m_searchResultPending = false;
                    guard->m_searchTaskId = -1;
                    guard->reportSourceFailure(admission);
                }, Qt::QueuedConnection);
                return;
            }
            const QByteArray &chunk = read.bytes;
            const qsizetype local = chunk.indexOf(needle);
            if (local >= 0) {
                result = chunkStart + local;
                break;
            }
            chunkStart += std::min<qint64>(chunkSize, qint64(chunk.size()));
            const qint64 scanned = std::min(chunkStart, size);
            if (reportProgress)
                reportProgress(scanned, size);
            QMetaObject::invokeMethod(dispatcher, [guard, generation, scanned, size]() {
                if (guard && guard->m_searchGeneration == generation)
                    emit guard->searchProgress(scanned, size);
            }, Qt::QueuedConnection);
        }
        if (cancel && cancel->load(std::memory_order_acquire))
            return;
        QMetaObject::invokeMethod(dispatcher, [guard, generation, result, needle, expected]() {
            if (!guard || guard->m_searchGeneration != generation
                || !sameDiskFileStamp(guard->sourceStamp(), expected)
                || !guard->validateCurrent() || !guard
                || guard->m_searchGeneration != generation)
                return;
            guard->m_lastSearchResult = result;
            guard->m_searchResultPending = false;
            guard->m_searchTaskId = -1;
            if (result >= 0)
                guard->goToOffset(result, needle.size());
            if (!guard || guard->m_searchGeneration != generation) return;
            emit guard->searchFinished(result);
        }, Qt::QueuedConnection);
    }, 0, QLatin1String("WindowedSearch"), size);
    if (guard && m_searchGeneration == generation) m_searchTaskId = submittedId;
    else if (submittedId >= 0) dispatcher->cancel(submittedId);
}

bool WindowedReadOnlyProvider::searchRunning() const
{
    // The operation is only complete after the GUI thread has consumed the
    // future and committed navigation/result state. QFuture::isRunning() can
    // turn false one event-loop turn earlier, which exposes stale -1 results.
    return m_searchResultPending;
}

bool WindowedReadOnlyProvider::eventFilter(QObject *watched, QEvent *event)
{
    const bool providerSurface = watched == this;
    const bool editorSurface = m_editor
        && (watched == m_editor || watched == m_editor->viewport());
    const bool asciiSurface = m_asciiPreview
        && (watched == m_asciiPreview || watched == m_asciiPreview->viewport());
    if ((providerSurface || editorSurface || asciiSurface) && m_fileSize > 0) {
        if (m_presentation == Presentation::Hex && m_hexMouseSelection) {
            const auto result = m_hexMouseSelection->filter(watched, event);
            if (result
                == WindowedHexMouseSelectionController::FilterResult::Consume) {
                return true;
            }
            if (result
                == WindowedHexMouseSelectionController::FilterResult::SourceRelease) {
                // The native surface must observe release to clear its own
                // mouse capture; the controller reapplies source authority on
                // the next event-loop turn.
                return QWidget::eventFilter(watched, event);
            }
        }
        if (m_presentation == Presentation::Hex && asciiSurface
            && event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            m_asciiMouseSelecting = true;
        }
        if (m_presentation == Presentation::Hex && asciiSurface
            && event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            QTimer::singleShot(0, this, [this]() {
                m_asciiMouseSelecting = false;
                syncHexSelectionFromAscii();
            });
        }
        if ((providerSurface || editorSurface) && event->type() == QEvent::Resize
            && m_presentation == Presentation::Hex
            && !m_hexRelayoutPending) {
            const qint64 topOffset = visibleTopOffset();
            m_hexRelayoutPending = true;
            QTimer::singleShot(0, this, [this, topOffset]() {
                m_hexRelayoutPending = false;
                if (m_path.isEmpty() || m_presentation != Presentation::Hex)
                    return;
                const int desired = calculateHexBytesPerRow();
                if (desired != m_hexBytesPerRow) {
                    m_hexBytesPerRow = desired;
                    reloadWindow(m_globalOffset, 0, topOffset, true);
                }
            });
        }
        if (event->type() == QEvent::Wheel) {
            const auto *wheel = static_cast<QWheelEvent *>(event);
            if (wheel->modifiers().testFlag(Qt::ControlModifier)
                || wheel->modifiers().testFlag(Qt::ShiftModifier)
                || qAbs(wheel->angleDelta().x()) > qAbs(wheel->angleDelta().y()))
                return QWidget::eventFilter(watched, event);

            int lineDelta = 0;
            if (!wheel->pixelDelta().isNull()) {
                const int lineHeight = std::max(1, QFontMetrics(m_editor->font()).lineSpacing());
                m_wheelPixelRemainder += wheel->pixelDelta().y();
                lineDelta = -(m_wheelPixelRemainder / lineHeight);
                m_wheelPixelRemainder %= lineHeight;
            } else {
                m_wheelAngleRemainder += wheel->angleDelta().y();
                const int steps = m_wheelAngleRemainder / 120;
                m_wheelAngleRemainder %= 120;
                lineDelta = -steps * std::max(1, QApplication::wheelScrollLines());
            }
            if (lineDelta != 0)
                scrollByLines(lineDelta);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            const bool extend = key->modifiers().testFlag(Qt::ShiftModifier);
            if (key->matches(QKeySequence::Copy)) {
                (void)copySelection();
                event->accept();
                return true;
            }
            if (key->matches(QKeySequence::SelectAll)) {
                selectAllSource();
                event->accept();
                return true;
            }

            if (m_presentation == Presentation::Hex && editorSurface) {
                const qint64 anchorPosition = directScintilla(
                    m_editor, QsciScintillaBase::SCI_GETANCHOR);
                const qint64 caretPosition = directScintilla(
                    m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
                const bool hasSelection = anchorPosition != caretPosition;
                int line = 0;
                int column = 0;
                m_editor->lineIndexFromPosition(caretPosition, &line, &column);
                int nibble = std::clamp(column % 3, 0, 1);
                qint64 caretByte = hasSelection
                    ? sourceBoundaryForHexPosition(caretPosition)
                    : sourceOffsetForProjectedPosition(caretPosition, false);
                qint64 anchorByte = hasSelection
                    ? sourceBoundaryForHexPosition(anchorPosition) : caretByte;
                const int pageRows = std::max(1, static_cast<int>(directScintilla(
                    m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
                const bool navigation = key->key() == Qt::Key_Left
                    || key->key() == Qt::Key_Right
                    || key->key() == Qt::Key_Up
                    || key->key() == Qt::Key_Down
                    || key->key() == Qt::Key_Home
                    || key->key() == Qt::Key_End
                    || key->key() == Qt::Key_PageUp
                    || key->key() == Qt::Key_PageDown;
                if (navigation) {
                    if (extend) {
                        if (key->modifiers().testFlag(Qt::ControlModifier)
                            && key->key() == Qt::Key_Home) {
                            caretByte = 0;
                        } else if (key->modifiers().testFlag(Qt::ControlModifier)
                                   && key->key() == Qt::Key_End) {
                            caretByte = m_fileSize;
                        } else if (key->key() == Qt::Key_Left) {
                            caretByte = std::max(0LL, caretByte - 1);
                        } else if (key->key() == Qt::Key_Right) {
                            caretByte = std::min(m_fileSize, caretByte + 1);
                        } else if (key->key() == Qt::Key_Up) {
                            caretByte = std::max(0LL, caretByte - m_hexBytesPerRow);
                        } else if (key->key() == Qt::Key_Down) {
                            caretByte = std::min(m_fileSize,
                                                 caretByte + m_hexBytesPerRow);
                        } else if (key->key() == Qt::Key_PageUp) {
                            caretByte = std::max(
                                0LL, caretByte - qint64(pageRows) * m_hexBytesPerRow);
                        } else if (key->key() == Qt::Key_PageDown) {
                            caretByte = std::min(
                                m_fileSize,
                                caretByte + qint64(pageRows) * m_hexBytesPerRow);
                        } else {
                            const qint64 rowStart = (caretByte / m_hexBytesPerRow)
                                                    * m_hexBytesPerRow;
                            caretByte = key->key() == Qt::Key_Home
                                ? rowStart
                                : std::min(m_fileSize,
                                           rowStart + m_hexBytesPerRow);
                        }
                        setHexSourceSelection(anchorByte, caretByte);
                    } else if (key->modifiers().testFlag(Qt::ControlModifier)
                               && (key->key() == Qt::Key_Home
                                   || key->key() == Qt::Key_End)) {
                        setHexCaretAtSource(key->key() == Qt::Key_Home
                                               ? 0 : m_fileSize - 1,
                                           key->key() == Qt::Key_End ? 1 : 0);
                    } else if (key->key() == Qt::Key_Left
                               || key->key() == Qt::Key_Right) {
                        if (hasSelection) {
                            const qint64 target = key->key() == Qt::Key_Left
                                ? std::min(anchorByte, caretByte)
                                : std::max(anchorByte, caretByte);
                            setHexCaretAtSource(
                                std::min(target, m_fileSize - 1),
                                key->key() == Qt::Key_Left ? 0 : 1);
                        } else if (key->key() == Qt::Key_Right) {
                            if (nibble == 0)
                                setHexCaretAtSource(caretByte, 1);
                            else
                                setHexCaretAtSource(
                                    std::min(m_fileSize - 1, caretByte + 1), 0);
                        } else if (nibble == 1) {
                            setHexCaretAtSource(caretByte, 0);
                        } else {
                            setHexCaretAtSource(std::max(0LL, caretByte - 1), 1);
                        }
                    } else {
                        qint64 target = caretByte;
                        if (key->key() == Qt::Key_Up)
                            target -= m_hexBytesPerRow;
                        else if (key->key() == Qt::Key_Down)
                            target += m_hexBytesPerRow;
                        else if (key->key() == Qt::Key_PageUp)
                            target -= qint64(pageRows) * m_hexBytesPerRow;
                        else if (key->key() == Qt::Key_PageDown)
                            target += qint64(pageRows) * m_hexBytesPerRow;
                        else {
                            const qint64 rowStart = (caretByte / m_hexBytesPerRow)
                                                    * m_hexBytesPerRow;
                            target = key->key() == Qt::Key_Home
                                ? rowStart
                                : std::min(m_fileSize - 1,
                                           rowStart + m_hexBytesPerRow - 1);
                            nibble = key->key() == Qt::Key_End ? 1 : 0;
                        }
                        setHexCaretAtSource(
                            std::clamp(target, 0LL, m_fileSize - 1), nibble);
                    }
                    event->accept();
                    return true;
                }
            }

            if (key->key() == Qt::Key_PageDown || key->key() == Qt::Key_PageUp) {
                if (extend)
                    beginVirtualSelection();
                else
                    clearVirtualSelection();
                const int pageLines = std::max(1, static_cast<int>(directScintilla(
                    m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
                scrollByLines(key->key() == Qt::Key_PageDown ? pageLines : -pageLines);
                if (extend)
                    updateVirtualSelection(visibleTopOffset());
                event->accept();
                return true;
            }

            if (key->matches(QKeySequence::Cut)
                || key->matches(QKeySequence::Paste)
                || key->key() == Qt::Key_Delete
                || key->key() == Qt::Key_Backspace
                || key->key() == Qt::Key_Insert
                || (!key->text().isEmpty()
                    && !(key->modifiers()
                         & (Qt::ControlModifier | Qt::AltModifier
                            | Qt::MetaModifier)))) {
                event->accept();
                return true;
            }

            qint64 destination = -1;
            if (key->key() == Qt::Key_Home
                && key->modifiers().testFlag(Qt::ControlModifier))
                destination = 0;
            else if (key->key() == Qt::Key_End
                     && key->modifiers().testFlag(Qt::ControlModifier))
                destination = m_fileSize;
            else
                return QWidget::eventFilter(watched, event);
            if (extend)
                beginVirtualSelection();
            else
                clearVirtualSelection();
            const qint64 displayOffset = destination >= m_fileSize
                ? m_fileSize - 1 : destination;
            reloadWindow(displayOffset, 0);
            if (extend)
                updateVirtualSelection(destination);
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool WindowedReadOnlyProvider::reloadWindow(qint64 anchorOffset, qint64 selectionBytes,
                                           qint64 requestedTopOffset,
                                           bool preserveHorizontal, QString *error)
{
    const QPointer<WindowedReadOnlyProvider> self(this);
    const auto tailIntent = m_tailController->projectionIntent();
    const bool previousApplying = std::exchange(m_applyingSourceSelection, true);
    const auto selectionTransaction = qScopeGuard([self, previousApplying] {
        if (self) self->m_applyingSourceSelection = previousApplying;
    });
    const auto expected = sourceStamp();
    const quint64 generation = m_searchGeneration;
    SharedReadOnlyFile reader;
    auto admission = openOrdinaryReadSource(reader, m_path, m_readPolicy, &expected);
    if (!admission.isReady()) {
        if (error) *error = sourceReadMessage(admission);
        reportSourceFailure(admission);
        return false;
    }
    bool readFailed = false;
    const auto readBytes = [&reader, &readFailed](qint64 offset, qsizetype count) {
        const QByteArray bytes = reader.readAt(offset, count);
        readFailed = readFailed || bytes.size() != count;
        return bytes;
    };
    anchorOffset = clampOffset(anchorOffset, m_fileSize);
    const int horizontalOffset = preserveHorizontal
        ? static_cast<int>(directScintilla(m_editor, QsciScintillaBase::SCI_GETXOFFSET))
        : 0;
    qint64 start = std::max(0LL, anchorOffset - m_windowSpan / 3);
    if (start + m_windowSpan > m_fileSize)
        start = std::max(0LL, m_fileSize - m_windowSpan);

    if (m_presentation == Presentation::Hex) {
        start -= start % m_hexBytesPerRow;
    } else if (m_binaryContent) {
        // Binary Text has no global line authority. Keep only a tiny decoder
        // overlap at the window boundary; never scan backwards for a
        // synthetic newline.
        start = std::max<qint64>(0, start - 8);
        if (m_textEncoding == TextEncoding::Utf16LE
            || m_textEncoding == TextEncoding::Utf16BE) {
            start -= start % 2;
        }
    } else if (m_textEncoding == TextEncoding::Utf16LE
               || m_textEncoding == TextEncoding::Utf16BE) {
        start -= start % 2;
        if (start > 0) {
            const qint64 lookBehind = std::min<qint64>(kMaxTextProjectionBytes,
                                                       start);
            const qint64 scanStart = start - lookBehind;
            const QByteArray prefix = readBytes(scanStart,
                                                   static_cast<qsizetype>(lookBehind));
            for (qsizetype i = prefix.size() - (prefix.size() % 2) - 2;
                 i >= 0; i -= 2) {
                const uchar a = uchar(prefix.at(i));
                const uchar b = uchar(prefix.at(i + 1));
                const bool newline = m_textEncoding == TextEncoding::Utf16LE
                    ? a == 0x0a && b == 0x00 : a == 0x00 && b == 0x0a;
                if (newline) {
                    start = scanStart + i + 2;
                    break;
                }
            }
        }
    } else if (start > 0) {
        const qint64 lookBehind = std::min<qint64>(kMaxTextProjectionBytes, start);
        const QByteArray prefix = readBytes(start - lookBehind,
                                               static_cast<qsizetype>(lookBehind));
        const qsizetype lineBreak = prefix.lastIndexOf('\n');
        if (lineBreak >= 0)
            start = start - lookBehind + lineBreak + 1;
    }
    // Alignment may move the start backwards after the window was pinned to EOF.
    // Never let that create a small uncovered tail: the requested anchor is the
    // position truth and must always be part of the projected range.
    qint64 end = std::min(
        m_fileSize,
        std::max(start + m_windowSpan,
                 anchorOffset + std::max<qint64>(1, selectionBytes)));
    if (m_presentation == Presentation::Text
        && (m_textEncoding == TextEncoding::Utf16LE
            || m_textEncoding == TextEncoding::Utf16BE)
        && end < m_fileSize && end % 2 != 0) {
        ++end;
    }
    if (m_presentation == Presentation::Text && m_binaryContent
        && end < m_fileSize) {
        end = std::min(m_fileSize, end + 8);
        if ((m_textEncoding == TextEncoding::Utf16LE
             || m_textEncoding == TextEncoding::Utf16BE)
            && end < m_fileSize && end % 2 != 0) {
            ++end;
        }
    } else if (m_presentation == Presentation::Text && end < m_fileSize
        && end - start < kMaxTextProjectionBytes) {
        const qint64 scanBytes = std::min(kMaxTextProjectionBytes - (end - start),
                                          m_fileSize - end);
        const QByteArray suffix = readBytes(end, static_cast<qsizetype>(scanBytes));
        if (m_textEncoding == TextEncoding::Utf16LE
            || m_textEncoding == TextEncoding::Utf16BE) {
            qsizetype lineEnd = -1;
            for (qsizetype i = 0; i + 1 < suffix.size(); i += 2) {
                const uchar a = uchar(suffix.at(i));
                const uchar b = uchar(suffix.at(i + 1));
                const bool newline = m_textEncoding == TextEncoding::Utf16LE
                    ? a == 0x0a && b == 0x00 : a == 0x00 && b == 0x0a;
                if (newline) { lineEnd = i; break; }
            }
            end += lineEnd >= 0 ? lineEnd + 2 : suffix.size() - suffix.size() % 2;
        } else {
            const qsizetype lineEnd = suffix.indexOf('\n');
            end += lineEnd >= 0 ? lineEnd + 1 : suffix.size();
        }
    }
    const qsizetype length = static_cast<qsizetype>(end - start);

    const QByteArray source = readBytes(start, length);
    reader.captureStamp(&admission.stamp);
    if (readFailed || !sameDiskFileStamp(expected, admission.stamp)) {
        admission.status = readFailed ? OrdinaryReadStatus::Unreadable : OrdinaryReadStatus::Changed;
        if (error) *error = sourceReadMessage(admission);
        reportSourceFailure(admission);
        return false;
    }
    reader.close();
    if (!validateCurrent(error) || !self || m_searchGeneration != generation) return false;
    end = start + source.size();
    QByteArray projection;
    QString textProjection;
    if (m_presentation == Presentation::Hex)
        projection = formatHexWindow(reinterpret_cast<const uchar *>(source.constData()),
                                     source.size(), start);
    else {
        const DecodedTextProjection decoded = decodeTextProjection(
            source, m_textEncoding, start == 0);
        textProjection = decoded.text;
        m_projectedToSource = decoded.projectedToSource;
        m_sourceBoundaries = decoded.sourceBoundaries;
        m_projectedBoundaries = decoded.projectedBoundaries;
    }

    m_windowStart = start;
    m_windowEnd = end;
    m_editor->setReadOnly(false);
    m_editor->setText(m_presentation == Presentation::Hex
                          ? QString::fromLatin1(projection)
                          : textProjection);
    if (!self) return false;
    m_editor->setReadOnly(true);
    if (m_searchGeneration != generation || !m_tailController->permitsProjection(tailIntent)) return false;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 1UL);
    if (m_presentation == Presentation::Hex) {
        populateAsciiProjection(source);
        m_asciiPreview->show();
    } else {
        m_asciiPreview->hide();
    }
    configureProjectionMargin();
    m_globalOffset = anchorOffset;
    m_updatingScrollBar = true;
    m_globalScrollBar->setValue(sliderForOffset(anchorOffset));
    m_updatingScrollBar = false;
    selectProjectedBytes(anchorOffset, selectionBytes);
    if (requestedTopOffset >= 0) {
        const qint64 topPosition = projectedPositionForSourceOffset(requestedTopOffset);
        directScintilla(m_editor, QsciScintillaBase::SCI_GOTOPOS,
                        static_cast<ScintillaUnsigned>(std::max<qint64>(0, topPosition)));
        directScintilla(m_editor, QsciScintillaBase::SCI_SCROLLCARET);
        const int caretY = static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_POINTYFROMPOSITION, 0,
            static_cast<ScintillaSigned>(std::max<qint64>(0, topPosition))));
        const int docLine = static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_LINEFROMPOSITION,
            static_cast<ScintillaUnsigned>(std::max<qint64>(0, topPosition))));
        const int lineHeight = std::max(1, static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_TEXTHEIGHT,
            static_cast<ScintillaUnsigned>(std::max(0, docLine)))));
        if (caretY > 0) {
            directScintilla(m_editor, QsciScintillaBase::SCI_LINESCROLL, 0,
                            static_cast<ScintillaSigned>(caretY / lineHeight));
        }
    }
    if (anchorOffset >= m_fileSize - 1
        && m_searchGeneration == generation && m_tailController->permitsProjection(tailIntent)
        && (requestedTopOffset < 0 || m_tailController->state() != ReadOnlyTailController::State::Stopped)) {
        // K02: loading the final mmap window and reaching its final viewport
        // row are separate operations. Always perform a post-paint settle for
        // both Text and Hex; it is bounded to the current projection and does
        // not scan or lay out the full source file.
        m_scrollToEndAfterPaint = true;
        scrollProjectionToEnd();
        m_editor->viewport()->update();
    } else {
        m_scrollToEndAfterPaint = false;
    }
    if (preserveHorizontal) {
        directScintilla(m_editor, QsciScintillaBase::SCI_SETXOFFSET,
                        static_cast<ScintillaUnsigned>(
                            m_presentation == Presentation::Hex
                                ? 0 : std::max(0, horizontalOffset)));
    }
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        applyVirtualSelection();
    syncAsciiScrollFromHex();
    emit windowReady(start, end, anchorOffset);
    return true;
}

void WindowedReadOnlyProvider::scrollByLines(int lineDelta)
{
    if (m_path.isEmpty() || lineDelta == 0)
        return;

    auto firstVisible = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    const int linesOnScreen = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
    int lastFirst = std::max(0, projectedDisplayLineCount() - linesOnScreen);

    const bool needsForwardWindow = lineDelta > 0
        && firstVisible + lineDelta > lastFirst && m_windowEnd < m_fileSize;
    const bool needsBackwardWindow = lineDelta < 0
        && firstVisible + lineDelta < 0 && m_windowStart > 0;
    if (needsForwardWindow || needsBackwardWindow) {
        const qint64 preservedTop = sourceOffsetForProjectedLine(firstVisible);
        reloadWindow(preservedTop, 0, preservedTop, true);
        firstVisible = static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        lastFirst = std::max(0, projectedDisplayLineCount() - linesOnScreen);
    }

    const int targetFirst = std::clamp(firstVisible + lineDelta, 0, lastFirst);
    directScintilla(m_editor, QsciScintillaBase::SCI_LINESCROLL, 0,
                    static_cast<ScintillaSigned>(targetFirst - firstVisible));
    m_globalOffset = visibleTopOffset();
    emit windowReady(m_windowStart, m_windowEnd, m_globalOffset);
    m_updatingScrollBar = true;
    if (m_windowEnd == m_fileSize && targetFirst >= lastFirst)
        m_globalScrollBar->setValue(m_globalScrollBar->maximum());
    else
        m_globalScrollBar->setValue(sliderForOffset(m_globalOffset));
    m_updatingScrollBar = false;
    syncAsciiScrollFromHex();
}

void WindowedReadOnlyProvider::beginVirtualSelection()
{
    if (m_selectionAuthority == SelectionAuthority::SourceBytes)
        return;
    const qint64 anchorPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETANCHOR);
    const qint64 caretPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
    if (anchorPosition == caretPosition) {
        const qint64 anchor = sourceOffsetForProjectedPosition(anchorPosition, false);
        m_sourceSelection.selectEndpoints({anchor}, {anchor});
    } else {
        m_sourceSelection.selectEndpoints(
            {sourceOffsetForProjectedPosition(anchorPosition, true)},
            {sourceOffsetForProjectedPosition(caretPosition, true)});
    }
    m_selectionAuthority = SelectionAuthority::SourceBytes;
}

void WindowedReadOnlyProvider::updateVirtualSelection(qint64 caretOffset)
{
    if (m_selectionAuthority != SelectionAuthority::SourceBytes)
        beginVirtualSelection();
    m_sourceSelection.selectEndpoints(
        m_sourceSelection.anchor(),
        {std::clamp(caretOffset, 0LL, m_fileSize)});
    applyVirtualSelection();
}

void WindowedReadOnlyProvider::clearVirtualSelection()
{
    m_selectionAuthority = SelectionAuthority::NativeProjection;
    m_sourceSelection.clear();
}

void WindowedReadOnlyProvider::applyVirtualSelection()
{
    if (m_path.isEmpty()
        || m_selectionAuthority != SelectionAuthority::SourceBytes)
        return;
    QScopedValueRollback<bool> selectionTransaction(
        m_applyingSourceSelection, true);
    if (!m_sourceSelection.selection())
        return;
    const qint64 low = m_sourceSelection.selection()->start.value;
    const qint64 high = m_sourceSelection.selection()->end.value;
    const qint64 visibleLow = std::clamp(low, m_windowStart, m_windowEnd);
    const qint64 visibleHigh = std::clamp(high, m_windowStart, m_windowEnd);
    const qint64 startPosition = projectedPositionForSourceOffset(visibleLow);
    const qint64 endPosition = projectedPositionForSourceOffset(visibleHigh);
    const bool forward = m_sourceSelection.caret().value
        >= m_sourceSelection.anchor().value;
    directScintilla(m_editor, QsciScintillaBase::SCI_SETSEL,
                    static_cast<ScintillaUnsigned>(forward ? startPosition : endPosition),
                    static_cast<ScintillaSigned>(forward ? endPosition : startPosition));
    if (m_presentation == Presentation::Hex && m_asciiPreview) {
        QTextCursor cursor(m_asciiPreview->document());
        cursor.setPosition(asciiPositionForSourceOffset(
            forward ? visibleLow : visibleHigh));
        cursor.setPosition(asciiPositionForSourceOffset(
            forward ? visibleHigh : visibleLow), QTextCursor::KeepAnchor);
        m_syncingAsciiSelection = true;
        m_asciiPreview->setTextCursor(cursor);
        m_syncingAsciiSelection = false;
    }
}

qint64 WindowedReadOnlyProvider::sourceOffsetAtViewportY(int y) const
{
    if (m_path.isEmpty() || !m_editor || !m_editor->viewport())
        return -1;
    const qint64 position = directScintilla(
        m_editor, QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
        static_cast<ScintillaUnsigned>(
            m_presentation == Presentation::Hex
                ? std::max(2, m_editor->marginWidth(0) + 2) : 2),
        static_cast<ScintillaSigned>(std::max(0, y)));
    if (position < 0)
        return -1;
    return sourceOffsetForProjectedPosition(position, false);
}

int WindowedReadOnlyProvider::projectedDisplayLineCount() const
{
    if (!m_editor)
        return 0;
    const int documentLines = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETLINECOUNT)));
    const int lastDocumentLine = documentLines - 1;
    const int firstVisibleForLast = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
        static_cast<ScintillaUnsigned>(lastDocumentLine)));
    const int wrappedCount = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_WRAPCOUNT,
        static_cast<ScintillaUnsigned>(lastDocumentLine))));
    return std::max(documentLines, firstVisibleForLast + wrappedCount);
}

void WindowedReadOnlyProvider::scrollProjectionToEnd()
{
    if (!m_editor)
        return;
    // Let Scintilla compute its own exact maximum. Reconstructing this from
    // SCI_LINESONSCREEN is subtly wrong while a newly assigned document is
    // settling: the value is based on the previous paint and can leave one or
    // more physical rows below the viewport. SCI_SCROLLTOEND delegates to
    // Editor::MaxScrollPos(), which is the same bound used by the native
    // editor's scrolling implementation and remains valid with its scrollbar
    // hidden behind our global byte-range scrollbar.
    directScintilla(m_editor, QsciScintillaBase::SCI_SCROLLTOEND);

    // Keep a deterministic fallback for platforms where the viewport has not
    // yet acquired a non-zero height. The post-paint call will run the native
    // path above again with final metrics.
    const int linesOnScreen = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
    const int lastFirst = std::max(0, projectedDisplayLineCount() - linesOnScreen);
    const int firstVisible = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    if (firstVisible < lastFirst) {
        directScintilla(m_editor, QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                        static_cast<ScintillaUnsigned>(lastFirst));
    }
    syncAsciiScrollFromHex();
}

bool WindowedReadOnlyProvider::lineNumberBaseForOffset(
    qint64 offset, qint64 *baseLine) const
{
    if (!baseLine || offset < 0 || offset > m_fileSize)
        return false;
    if (offset == 0) {
        *baseLine = 0;
        return true;
    }
    if (m_lineBreakPrefixEncoding != m_textEncoding
        || m_lineBreakPrefix.isEmpty()
        || m_lineBreakPrefix.size() != m_lineBreakPrefixEndsWithCr.size()) {
        return false;
    }
    const qint64 checkpoint = offset / PrefixChunkBytes;
    if (checkpoint < 0 || checkpoint >= m_lineBreakPrefix.size())
        return false;
    const qint64 checkpointOffset = checkpoint * PrefixChunkBytes;
    qint64 breaks = m_lineBreakPrefix.at(qsizetype(checkpoint));
    bool previousWasCr = m_lineBreakPrefixEndsWithCr.at(qsizetype(checkpoint)) != 0;
    if (offset > checkpointOffset) {
        const QByteArray suffix = sourceBytes(
            checkpointOffset, static_cast<qsizetype>(offset - checkpointOffset));
        if (suffix.size() != offset - checkpointOffset)
            return false;
        breaks += countPhysicalLineBreaks(
            suffix, m_textEncoding, previousWasCr, &previousWasCr);
    }
    *baseLine = breaks;
    return true;
}

void WindowedReadOnlyProvider::configureProjectionMargin()
{
    if (!m_editor)
        return;
    if (m_presentation == Presentation::Hex) {
        m_editor->setMarginLineNumbers(0, false);
        m_editor->setMarginType(0, QsciScintilla::TextMargin);
        const int addressDigits = std::max(
            8, static_cast<int>(QString::number(
                std::max<qint64>(0, m_fileSize - 1), 16).size()));
        m_editor->setMarginWidth(0, QString(addressDigits + 2, QLatin1Char('0')));
        const int lines = std::max(0, static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETLINECOUNT)));
        for (int line = 0; line < lines; ++line) {
            const QString address = QString::number(
                std::clamp(m_windowStart + qint64(line) * m_hexBytesPerRow,
                           0LL, m_fileSize), 16)
                                        .rightJustified(addressDigits, QLatin1Char('0'))
                                        .toUpper();
            m_editor->setMarginText(line, address, 0);
        }
        return;
    }

    if (m_binaryContent) {
        // Binary line breaks are display artifacts, not global document
        // structure. Label visible rows with source byte offsets and avoid
        // the full-file line-prefix and character-count tasks entirely.
        directScintilla(m_editor, QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
        m_editor->setMarginLineNumbers(0, false);
        m_editor->setMarginType(0, QsciScintilla::TextMargin);
        const int lines = std::max(1, static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETLINECOUNT)));
        const int digits = std::max(
            8, static_cast<int>(QString::number(
                std::max<qint64>(0, m_fileSize - 1), 16).size()));
        m_editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('0')));
        for (int line = 0; line < lines; ++line) {
            const qint64 offset = sourceOffsetForProjectedLine(line);
            m_editor->setMarginText(
                line, QString::number(offset, 16)
                          .rightJustified(digits, QLatin1Char('0')).toUpper(), 0);
        }
        return;
    }

    // A bounded projection must not expose Scintilla's local 1..N line numbers:
    // they restart after every mmap window and make S2/S3 appear to loop. The
    // character-count pass also builds tiny 1 MiB prefix checkpoints, so the
    // exact global base costs at most one bounded chunk read and no second
    // full-file index.
    directScintilla(m_editor, QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
    m_editor->setMarginLineNumbers(0, false);
    m_editor->setMarginType(0, QsciScintilla::TextMargin);
    const int lines = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETLINECOUNT)));
    qint64 baseLine = 0;
    const bool exact = lineNumberBaseForOffset(m_windowStart, &baseLine);
    const qint64 largest = exact ? baseLine + lines : lines;
    const int lineDigits = std::max(4, int(QString::number(largest).size()));
    m_editor->setMarginWidth(0, QString(lineDigits + 2, QLatin1Char('9')));
    for (int line = 0; line < lines; ++line) {
        m_editor->setMarginText(
            line, exact ? QString::number(baseLine + line + 1)
                        : QStringLiteral("…"), 0);
    }
}

qint64 WindowedReadOnlyProvider::sourceOffsetForProjectedLine(int line) const
{
    if (m_path.isEmpty())
        return 0;
    line = std::max(0, line);
    if (m_presentation == Presentation::Hex)
        return clampOffset(m_windowStart + qint64(line) * m_hexBytesPerRow, m_fileSize);
    int documentLine = line;
    if (m_wordWrap) {
        documentLine = static_cast<int>(directScintilla(
            m_editor, QsciScintillaBase::SCI_DOCLINEFROMVISIBLE,
            static_cast<ScintillaUnsigned>(line)));
    }
    const qint64 localPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_POSITIONFROMLINE,
        static_cast<ScintillaUnsigned>(std::max(0, documentLine)));
    return sourceOffsetForProjectedPosition(localPosition, false);
}

qint64 WindowedReadOnlyProvider::sourceOffsetForProjectedPosition(
    qint64 position, bool endBoundary) const
{
    if (m_path.isEmpty())
        return 0;
    position = std::max(0LL, position);
    if (m_presentation == Presentation::Text) {
        if (m_projectedToSource.isEmpty())
            return m_windowStart;
        const qint32 relative = TextProjectionPositionMapper::toSource(
            m_projectedToSource, position,
            endBoundary ? TextBoundaryAffinity::End
                        : TextBoundaryAffinity::Start);
        return std::clamp(m_windowStart + qint64(relative),
                          m_windowStart, m_windowEnd);
    }

    const int line = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_LINEFROMPOSITION,
        static_cast<ScintillaUnsigned>(position)));
    const qint64 lineStart = directScintilla(
        m_editor, QsciScintillaBase::SCI_POSITIONFROMLINE,
        static_cast<ScintillaUnsigned>(std::max(0, line)));
    const qint64 column = std::max(0LL, position - lineStart);
    qint64 byteInRow = endBoundary ? (column + 2) / 3 : column / 3;
    if (!endBoundary && column % 3 == 2)
        ++byteInRow;
    byteInRow = std::clamp(byteInRow, 0LL, qint64(m_hexBytesPerRow));
    return std::clamp(m_windowStart + qint64(std::max(0, line)) * m_hexBytesPerRow
                          + byteInRow,
                      m_windowStart, std::min(m_fileSize, m_windowEnd));
}

int WindowedReadOnlyProvider::projectedLineForSourceOffset(qint64 offset) const
{
    const qint64 localOffset = std::clamp(offset - m_windowStart, 0LL,
                                          std::max(0LL, m_windowEnd - m_windowStart - 1));
    if (m_presentation == Presentation::Hex)
        return static_cast<int>(localOffset / m_hexBytesPerRow);
    return static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_LINEFROMPOSITION,
        static_cast<ScintillaUnsigned>(projectedPositionForSourceOffset(offset))));
}

qint64 WindowedReadOnlyProvider::projectedPositionForSourceOffset(qint64 offset) const
{
    const qint64 localOffset = std::clamp(offset - m_windowStart, 0LL,
                                          std::max(0LL, m_windowEnd - m_windowStart));
    if (m_presentation == Presentation::Text) {
        if (m_sourceBoundaries.isEmpty())
            return 0;
        return TextProjectionPositionMapper::toProjected(
            m_sourceBoundaries, m_projectedBoundaries, localOffset);
    }
    if (localOffset >= m_windowEnd - m_windowStart)
        return directScintilla(m_editor, QsciScintillaBase::SCI_GETLENGTH);
    const qint64 row = localOffset / m_hexBytesPerRow;
    const qint64 column = localOffset % m_hexBytesPerRow;
    const qint64 lineStart = directScintilla(
        m_editor, QsciScintillaBase::SCI_POSITIONFROMLINE,
        static_cast<ScintillaUnsigned>(row));
    return std::max<qint64>(0, lineStart) + column * 3;
}

int WindowedReadOnlyProvider::calculateHexBytesPerRow() const
{
    if (!m_editor || !m_editor->viewport())
        return 16;
    const int width = std::max(1, this->width()
        - (m_globalScrollBar ? m_globalScrollBar->sizeHint().width() : 0));
    const int glyphWidth = std::max(
        7, QFontMetrics(m_editor->font()).horizontalAdvance(QLatin1Char('0')));
    const int addressDigits = std::max(
        8, static_cast<int>(QString::number(
            std::max<qint64>(0, m_fileSize - 1), 16).size()));
    const int addressWidth = glyphWidth * (addressDigits + 2) + 16;
    for (int candidate = 64; candidate >= 8; candidate -= 4) {
        const int required = addressWidth
            + glyphWidth * (candidate * 3 + candidate + 12) + 52;
        if (required <= width)
            return candidate;
    }
    return 8;
}

qint64 WindowedReadOnlyProvider::offsetForSlider(int value) const
{
    if (m_fileSize <= 1)
        return 0;
    return static_cast<qint64>((static_cast<long double>(value) * (m_fileSize - 1))
                               / kSliderMaximum);
}

int WindowedReadOnlyProvider::sliderForOffset(qint64 offset) const
{
    if (m_fileSize <= 1)
        return 0;
    return static_cast<int>((static_cast<long double>(clampOffset(offset, m_fileSize))
                             * kSliderMaximum) / (m_fileSize - 1));
}

QByteArray WindowedReadOnlyProvider::formatHexWindow(const uchar *data, qsizetype length,
                                                   qint64 absoluteStart) const
{
    Q_UNUSED(absoluteStart);
    static constexpr char digits[] = "0123456789ABCDEF";
    QByteArray result;
    result.reserve(length * 3 + length / m_hexBytesPerRow + 1);
    for (qsizetype row = 0; row < length; row += m_hexBytesPerRow) {
        const qsizetype count = std::min<qsizetype>(m_hexBytesPerRow, length - row);
        for (qsizetype column = 0; column < count; ++column) {
            if (column > 0)
                result += ' ';
            const uchar byte = data[row + column];
            result += digits[byte >> 4];
            result += digits[byte & 0x0f];
        }
        if (row + count < length)
            result += '\n';
    }
    return result;
}

void WindowedReadOnlyProvider::selectProjectedBytes(qint64 absoluteOffset, qint64 selectionBytes)
{
    if (absoluteOffset < m_windowStart || absoluteOffset >= m_windowEnd)
        return;
    QScopedValueRollback<bool> selectionTransaction(
        m_applyingSourceSelection, true);
    qint64 localStart = absoluteOffset - m_windowStart;
    qint64 localEnd = localStart + std::max(0LL, selectionBytes);
    if (m_presentation == Presentation::Hex) {
        localStart = projectedPositionForSourceOffset(absoluteOffset);
        localEnd = projectedPositionForSourceOffset(
            std::min(m_fileSize, absoluteOffset + std::max(0LL, selectionBytes)));
    } else {
        localStart = projectedPositionForSourceOffset(absoluteOffset);
        localEnd = projectedPositionForSourceOffset(
            std::min(m_fileSize, absoluteOffset + std::max(0LL, selectionBytes)));
    }
    directScintilla(m_editor, QsciScintillaBase::SCI_SETSEL,
                    static_cast<ScintillaUnsigned>(localStart),
                    static_cast<ScintillaSigned>(localEnd));
    directScintilla(m_editor, QsciScintillaBase::SCI_SCROLLCARET);
    if (m_presentation == Presentation::Hex && m_asciiPreview) {
        QTextCursor cursor(m_asciiPreview->document());
        cursor.setPosition(asciiPositionForSourceOffset(absoluteOffset));
        if (selectionBytes > 0) {
            cursor.setPosition(asciiPositionForSourceOffset(
                std::min(m_fileSize, absoluteOffset + selectionBytes)),
                QTextCursor::KeepAnchor);
        }
        m_syncingAsciiSelection = true;
        m_asciiPreview->setTextCursor(cursor);
        m_syncingAsciiSelection = false;
    }
}

void WindowedReadOnlyProvider::populateAsciiProjection(const QByteArray &source)
{
    if (!m_asciiPreview)
        return;
    QString ascii = decodeByteGridText(source, m_textEncoding,
                                       m_hexBytesPerRow);
    m_syncingAsciiSelection = true;
    m_asciiPreview->setPlainText(ascii);
    alignPlainTextRowsToScintilla(m_asciiPreview, m_editor);
    m_syncingAsciiSelection = false;
}

qint64 WindowedReadOnlyProvider::sourceOffsetForAsciiPosition(
    int position, bool endBoundary) const
{
    Q_UNUSED(endBoundary);
    if (!m_asciiPreview)
        return m_windowStart;
    const QTextBlock block = m_asciiPreview->document()->findBlock(
        std::max(0, position));
    if (!block.isValid())
        return m_windowEnd;
    const qint64 column = std::clamp<qint64>(
        qint64(position - block.position()), 0, block.text().size());
    return std::clamp(
        m_windowStart + qint64(block.blockNumber()) * m_hexBytesPerRow + column,
        m_windowStart, m_windowEnd);
}

int WindowedReadOnlyProvider::asciiPositionForSourceOffset(qint64 offset) const
{
    if (!m_asciiPreview)
        return 0;
    const qint64 local = std::clamp(offset - m_windowStart, 0LL,
                                    std::max(0LL, m_windowEnd - m_windowStart));
    if (local >= m_windowEnd - m_windowStart)
        return std::max(0, m_asciiPreview->document()->characterCount() - 1);
    const QTextBlock block = m_asciiPreview->document()->findBlockByNumber(
        int(local / m_hexBytesPerRow));
    if (!block.isValid())
        return std::max(0, m_asciiPreview->document()->characterCount() - 1);
    return std::min(block.position() + int(local % m_hexBytesPerRow),
                    block.position() + int(block.text().size()));
}

qint64 WindowedReadOnlyProvider::sourceBoundaryForHexPosition(qint64 position) const
{
    return sourceOffsetForProjectedPosition(position, true);
}

void WindowedReadOnlyProvider::syncAsciiScrollFromHex()
{
    if (m_presentation != Presentation::Hex || !m_asciiPreview)
        return;
    const int first = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    // Do not block this scrollbar: QPlainTextEdit consumes its own
    // valueChanged signal to move the document viewport.
    m_asciiPreview->verticalScrollBar()->setValue(first);
}

void WindowedReadOnlyProvider::syncAsciiSelectionFromHex()
{
    if (m_presentation != Presentation::Hex || !m_asciiPreview
        || m_syncingHexSelection || m_syncingAsciiSelection
        || m_applyingSourceSelection
        || m_selectionAuthority == SelectionAuthority::SourceBytes) {
        return;
    }
    const qint64 anchorPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETANCHOR);
    const qint64 caretPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
    const bool selected = anchorPosition != caretPosition;
    const qint64 anchor = selected
        ? sourceBoundaryForHexPosition(anchorPosition)
        : sourceOffsetForProjectedPosition(anchorPosition, false);
    const qint64 caret = selected
        ? sourceBoundaryForHexPosition(caretPosition) : anchor;
    clearVirtualSelection();
    // Mirroring an unselected Hex caret through setTextCursor() makes
    // QPlainTextEdit ensure that caret is visible. During wheel scrolling that
    // silently drags the ASCII pane back to the caret (often row 0). With no
    // byte selection there is nothing to mirror, so preserve its cursor and
    // synchronize only the visible row.
    if (!selected && !m_asciiPreview->textCursor().hasSelection()) {
        syncAsciiScrollFromHex();
        return;
    }
    QTextCursor cursor(m_asciiPreview->document());
    cursor.setPosition(asciiPositionForSourceOffset(anchor));
    cursor.setPosition(asciiPositionForSourceOffset(caret), QTextCursor::KeepAnchor);
    m_syncingAsciiSelection = true;
    m_asciiPreview->setTextCursor(cursor);
    m_syncingAsciiSelection = false;
    // QPlainTextEdit scrolls a newly assigned cursor into view. Restore the
    // Hex grid's first visible row after mirroring the selection so the two
    // panes remain visually locked even when the caret is outside the viewport.
    syncAsciiScrollFromHex();
}

void WindowedReadOnlyProvider::syncHexSelectionFromAscii()
{
    if (m_presentation != Presentation::Hex || !m_asciiPreview
        || m_asciiMouseSelecting
        || m_syncingAsciiSelection || m_syncingHexSelection
        || m_applyingSourceSelection
        || m_selectionAuthority == SelectionAuthority::SourceBytes) {
        return;
    }
    const QTextCursor cursor = m_asciiPreview->textCursor();
    const qint64 anchor = sourceOffsetForAsciiPosition(cursor.anchor(), false);
    const qint64 caret = sourceOffsetForAsciiPosition(cursor.position(), true);
    clearVirtualSelection();
    const qint64 anchorPosition = projectedPositionForSourceOffset(anchor);
    const qint64 caretPosition = projectedPositionForSourceOffset(caret);
    m_syncingHexSelection = true;
    directScintilla(m_editor, QsciScintillaBase::SCI_SETSEL,
                    static_cast<ScintillaUnsigned>(anchorPosition),
                    static_cast<ScintillaSigned>(caretPosition));
    m_syncingHexSelection = false;
}

void WindowedReadOnlyProvider::setHexCaretAtSource(qint64 offset, int nibble)
{
    if (m_fileSize <= 0)
        return;
    offset = std::clamp(offset, 0LL, m_fileSize - 1);
    if (offset < m_windowStart || offset >= m_windowEnd)
        reloadWindow(offset, 0);
    qint64 position = projectedPositionForSourceOffset(offset);
    position += std::clamp(nibble, 0, 1);
    clearVirtualSelection();
    m_syncingHexSelection = true;
    directScintilla(m_editor, QsciScintillaBase::SCI_GOTOPOS,
                    static_cast<ScintillaUnsigned>(position));
    m_syncingHexSelection = false;
    syncAsciiSelectionFromHex();
}

void WindowedReadOnlyProvider::setHexSourceSelection(qint64 anchor, qint64 caret)
{
    anchor = std::clamp(anchor, 0LL, m_fileSize);
    caret = std::clamp(caret, 0LL, m_fileSize);
    if (anchor < m_windowStart || anchor > m_windowEnd
        || caret < m_windowStart || caret > m_windowEnd) {
        m_selectionAuthority = SelectionAuthority::SourceBytes;
        m_sourceSelection.selectEndpoints({anchor}, {caret});
        const qint64 display = caret >= m_fileSize ? m_fileSize - 1 : caret;
        reloadWindow(std::max<qint64>(0, display), 0);
        applyVirtualSelection();
        return;
    }
    clearVirtualSelection();
    m_syncingHexSelection = true;
    directScintilla(m_editor, QsciScintillaBase::SCI_SETSEL,
                    static_cast<ScintillaUnsigned>(
                        projectedPositionForSourceOffset(anchor)),
                    static_cast<ScintillaSigned>(
                        projectedPositionForSourceOffset(caret)));
    m_syncingHexSelection = false;
    syncAsciiSelectionFromHex();
}

// S2 and S3 both use this provider and never construct a complete Scintilla
// document in the background.
