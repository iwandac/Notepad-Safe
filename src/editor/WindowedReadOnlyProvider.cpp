#include "WindowedReadOnlyProvider.h"
#include "EditorSurfaceStyle.h"
#include "ByteClipboard.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QApplication>
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
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "Sci_Position.h"
#include "SharedReadOnlyFile.h"

namespace {

constexpr int kSliderMaximum = 1'000'000;
constexpr qint64 kHexWindowBytes = 256LL * 1024;
constexpr qint64 kTextWindowBytes = 2LL * 1024 * 1024;
constexpr qint64 kMaxTextProjectionBytes = 8LL * 1024 * 1024;
constexpr qint64 kMaxClipboardBytes = 64LL * 1024 * 1024;

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

WindowedReadOnlyProvider::WindowedReadOnlyProvider(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("windowedReadOnlySlice"));
    installEventFilter(this);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_editor = new QsciScintilla(this);
    m_editor->setObjectName(QStringLiteral("windowProjectionEditor"));
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
    m_asciiPreview->setAccessibleName(tr("ASCII 字节预览"));
    m_asciiPreview->setToolTip(tr("字符栏（不可打印字节显示为 .；只读）"));
    m_asciiPreview->setFont(m_editor->font());
    m_asciiPreview->setReadOnly(true);
    m_asciiPreview->setAttribute(Qt::WA_InputMethodEnabled, false);
    m_asciiPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_asciiPreview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_asciiPreview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_asciiPreview->document()->setDocumentMargin(0);
    m_asciiPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { border: 0; border-left: 1px solid #c8c8c8;"
        " background: #fafafa; padding-left: 8px; }"));
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
        // Wrap counts are lazy in Scintilla. The first setText-time count can
        // describe only logical lines, leaving most of the final wrapped line
        // below the viewport. Recompute exactly after the first real paint.
        m_scrollToEndAfterPaint = false;
        scrollProjectionToEnd();
        m_globalOffset = std::max<qint64>(0, m_fileSize - 1);
        m_updatingScrollBar = true;
        m_globalScrollBar->setValue(m_globalScrollBar->maximum());
        m_updatingScrollBar = false;
    });
    connect(m_editor, &QsciScintilla::selectionChanged,
            this, &WindowedReadOnlyProvider::syncAsciiSelectionFromHex);
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, [this](int, int) { syncAsciiSelectionFromHex(); });

    m_globalScrollBar = new QScrollBar(Qt::Vertical, this);
    m_globalScrollBar->setObjectName(QStringLiteral("globalLargeFileScrollBar"));
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

    m_fileWatcher = new QFileSystemWatcher(this);
    m_remapDebounce = new QTimer(this);
    m_remapDebounce->setSingleShot(true);
    m_remapDebounce->setInterval(200);
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString &) {
        if (!m_path.isEmpty())
            m_remapDebounce->start();
    });
    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString &) {
        const QFileInfo info(m_path);
        if (!m_path.isEmpty() && info.exists()
            && (info.size() != m_observedSize
                || info.lastModified() != m_observedModified))
            m_remapDebounce->start();
    });
    connect(m_remapDebounce, &QTimer::timeout, this, [this]() {
        QString error;
        if (!refreshFileMapping(&error) && !error.isEmpty())
            emit sourceReloadFailed(error);
    });
    // Windows directory notifications do not report ordinary in-place appends.
    // A cheap metadata poll covers growing logs; directoryChanged remains the
    // fast path for rename/atomic replacement.
    m_filePoll = new QTimer(this);
    m_filePoll->setInterval(500);
    connect(m_filePoll, &QTimer::timeout, this, [this]() {
        if (m_path.isEmpty())
            return;
        const QFileInfo info(m_path);
        if (info.exists()
            && (info.size() != m_observedSize
                || info.lastModified() != m_observedModified))
            m_remapDebounce->start();
    });
}

WindowedReadOnlyProvider::~WindowedReadOnlyProvider()
{
    closeFile();
}

bool WindowedReadOnlyProvider::openFile(const QString &path, Presentation presentation,
                                     QString *error)
{
    closeFile();
    m_path = QFileInfo(path).absoluteFilePath();
    QString openError;
    if (!openSharedReadOnlyFile(m_file, m_path, &openError)) {
        if (error)
            *error = tr("无法打开文件：%1").arg(openError);
        return false;
    }
    m_fileSize = m_file.size();
    if (m_fileSize <= 0) {
        if (error)
            *error = tr("切片要求非空文件");
        m_file.close();
        m_fileSize = 0;
        return false;
    }
    // The windowed provider intentionally keeps no persistent source handle.
    // This lets other editors atomically replace the path and lets log rotators
    // rename it while the projected text remains visible in Scintilla.
    m_file.close();
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
    goToOffset(0);
    const QFileInfo observed(m_path);
    m_observedSize = observed.size();
    m_observedModified = observed.lastModified();
    m_filePoll->start();
    return true;
}

void WindowedReadOnlyProvider::closeFile()
{
    if (m_remapDebounce)
        m_remapDebounce->stop();
    if (m_filePoll)
        m_filePoll->stop();
    if (m_fileWatcher && !m_fileWatcher->files().isEmpty())
        m_fileWatcher->removePaths(m_fileWatcher->files());
    if (m_fileWatcher && !m_fileWatcher->directories().isEmpty())
        m_fileWatcher->removePaths(m_fileWatcher->directories());
    m_cancelSearch.storeRelease(true);
    if (m_searchWatcher.isRunning())
        m_searchWatcher.waitForFinished();
    m_searchResultPending = false;
    m_file.close();
    m_fileSize = 0;
    m_globalOffset = 0;
    m_windowStart = 0;
    m_windowEnd = 0;
    clearVirtualSelection();
    m_path.clear();
    if (m_asciiPreview)
        m_asciiPreview->clear();
    m_observedSize = -1;
    m_observedModified = {};
}

bool WindowedReadOnlyProvider::refreshFileMapping(QString *error)
{
    if (m_path.isEmpty()) {
        if (error)
            *error = tr("没有可重新加载的源文件。");
        return false;
    }
    const QFileInfo currentInfo(m_path);
    if (!currentInfo.exists() || !currentInfo.isFile()) {
        if (error)
            *error = tr("源文件暂时不可用：%1").arg(m_path);
        // Atomic replacement may briefly remove a watched path. Keep the old
        // mapping alive and retry instead of blanking a still-useful view.
        if (m_remapDebounce)
            m_remapDebounce->start(500);
        return false;
    }

    const qint64 previousSize = m_fileSize;
    const qint64 preservedTop = visibleTopOffset();
    const qint64 preservedAnchor = m_globalOffset;
    const int preservedHorizontal = static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETXOFFSET));

    m_cancelSearch.storeRelease(true);
    if (m_searchWatcher.isRunning())
        m_searchWatcher.waitForFinished();
    m_searchResultPending = false;
    m_file.close();
    QString openError;
    if (!openSharedReadOnlyFile(m_file, m_path, &openError)) {
        m_fileSize = 0;
        if (error)
            *error = tr("无法重新打开变化后的文件：%1").arg(openError);
        return false;
    }
    m_fileSize = m_file.size();
    m_file.close();
    const QFileInfo observed(m_path);
    m_observedSize = observed.size();
    m_observedModified = observed.lastModified();
    m_globalScrollBar->setEnabled(m_fileSize > 0);
    m_globalScrollBar->setPageStep(m_fileSize > 0
        ? std::max(1, int((static_cast<long double>(m_windowSpan) * kSliderMaximum)
                          / m_fileSize))
        : kSliderMaximum);

    m_selectionAnchor = m_selectionAnchor < 0
        ? -1 : std::clamp(m_selectionAnchor, 0LL, m_fileSize);
    m_selectionCaret = m_selectionCaret < 0
        ? -1 : std::clamp(m_selectionCaret, 0LL, m_fileSize);
    if (m_fileSize > 0) {
        reloadWindow(clampOffset(preservedAnchor, m_fileSize), 0,
                     std::clamp(preservedTop, 0LL, m_fileSize - 1), false);
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
    emit sourceFileChanged(previousSize, m_fileSize);
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
    reloadWindow(m_globalOffset, 0);
}

void WindowedReadOnlyProvider::setWordWrap(bool enabled)
{
    if (m_wordWrap == enabled)
        return;
    const qint64 preservedTop = visibleTopOffset();
    m_wordWrap = enabled;
    m_editor->setWrapMode(m_presentation == Presentation::Text && enabled
                              ? QsciScintilla::WrapWord
                              : QsciScintilla::WrapNone);
    m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
    m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    if (!m_path.isEmpty())
        reloadWindow(m_globalOffset, 0, preservedTop, true);
}

QByteArray WindowedReadOnlyProvider::sourceBytes(qint64 offset, qsizetype length) const
{
    if (m_path.isEmpty() || offset < 0 || offset >= m_fileSize || length <= 0)
        return {};
    const qint64 available = std::min<qint64>(length, m_fileSize - offset);
    SharedReadOnlyFile reader;
    if (!openSharedReadOnlyFile(reader, m_path))
        return {};
    return reader.readAt(offset, static_cast<qsizetype>(available));
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
    if (m_selectionAnchor >= 0 && m_selectionCaret >= 0)
        return std::min(m_selectionAnchor, m_selectionCaret);
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
    if (m_selectionAnchor >= 0 && m_selectionCaret >= 0)
        return std::max(m_selectionAnchor, m_selectionCaret);
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

bool WindowedReadOnlyProvider::copySelection(QString *error) const
{
    const qint64 start = selectionStartOffset();
    const qint64 end = selectionEndOffset();
    if (end <= start)
        return false;
    const qint64 length = end - start;
    if (length > kMaxClipboardBytes) {
        if (error) {
            *error = tr("所选内容超过 64 MiB。请缩小选区后复制，避免剪贴板耗尽内存。");
        }
        return false;
    }
    QByteArray selected = sourceBytes(start, static_cast<qsizetype>(length));
    const bool asciiFocused = m_presentation == Presentation::Hex
        && m_asciiPreview
        && (m_asciiPreview->hasFocus()
            || (m_asciiPreview->viewport()
                && m_asciiPreview->viewport()->hasFocus()));
    if (m_presentation == Presentation::Hex && !asciiFocused) {
        ByteClipboard::setHexSelection(selected);
    } else if (m_presentation == Presentation::Hex) {
        ByteClipboard::setAsciiSelection(selected);
    } else {
        QGuiApplication::clipboard()->setText(QString::fromUtf8(selected));
    }
    return true;
}

void WindowedReadOnlyProvider::goToOffset(qint64 offset, qint64 selectionBytes)
{
    if (m_path.isEmpty())
        return;
    if (selectionBytes > 0) {
        m_selectionAnchor = clampOffset(offset, m_fileSize);
        m_selectionCaret = std::min(m_fileSize, m_selectionAnchor + selectionBytes);
    } else {
        clearVirtualSelection();
    }
    reloadWindow(clampOffset(offset, m_fileSize), selectionBytes);
}

void WindowedReadOnlyProvider::selectAllSource()
{
    if (m_path.isEmpty() || m_fileSize <= 0)
        return;
    m_selectionAnchor = 0;
    m_selectionCaret = m_fileSize;
    applyVirtualSelection();
}

void WindowedReadOnlyProvider::findBytesAsync(const QByteArray &needle, qint64 fromOffset)
{
    if (m_path.isEmpty() || needle.isEmpty() || m_searchResultPending)
        return;
    m_cancelSearch.storeRelease(false);
    m_searchResultPending = true;
    m_lastSearchResult = -1;
    m_searchNeedle = needle;
    const qint64 size = m_fileSize;
    const QString path = m_path;
    const qint64 begin = std::clamp(fromOffset, 0LL, size);
    QPointer<WindowedReadOnlyProvider> guard(this);

    disconnect(&m_searchWatcher, nullptr, this, nullptr);
    connect(&m_searchWatcher, &QFutureWatcher<qint64>::finished, this, [this]() {
        m_lastSearchResult = m_searchWatcher.result();
        if (m_lastSearchResult >= 0)
            goToOffset(m_lastSearchResult, m_searchNeedle.size());
        m_searchResultPending = false;
        emit searchFinished(m_lastSearchResult);
    });
    m_searchWatcher.setFuture(QtConcurrent::run([this, guard, path, size, begin, needle]() {
        const qint64 last = size - needle.size();
        if (begin > last)
            return qint64(-1);
        constexpr qint64 chunkSize = 4LL * 1024 * 1024;
        const qint64 overlap = std::max<qint64>(0, qint64(needle.size()) - 1);
        qint64 chunkStart = begin;
        while (chunkStart <= last) {
            if (m_cancelSearch.loadAcquire())
                return qint64(-1);
            const qint64 wanted = std::min<qint64>(
                size - chunkStart, chunkSize + overlap);
            SharedReadOnlyFile reader;
            if (!openSharedReadOnlyFile(reader, path))
                break;
            const QByteArray chunk = reader.readAt(
                chunkStart, static_cast<qsizetype>(wanted));
            reader.close();
            if (chunk.isEmpty())
                break;
            const qsizetype local = chunk.indexOf(needle);
            if (local >= 0)
                return chunkStart + local;
            chunkStart += std::min<qint64>(chunkSize, qint64(chunk.size()));
            if (guard) {
                const qint64 scanned = std::min(chunkStart, size);
                QMetaObject::invokeMethod(guard, [guard, scanned, size]() {
                    if (guard)
                        emit guard->searchProgress(scanned, size);
                }, Qt::QueuedConnection);
            }
        }
        return qint64(-1);
    }));
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

void WindowedReadOnlyProvider::reloadWindow(qint64 anchorOffset, qint64 selectionBytes,
                                            qint64 requestedTopOffset,
                                            bool preserveHorizontal)
{
    anchorOffset = clampOffset(anchorOffset, m_fileSize);
    const int horizontalOffset = preserveHorizontal
        ? static_cast<int>(directScintilla(m_editor, QsciScintillaBase::SCI_GETXOFFSET))
        : 0;
    qint64 start = std::max(0LL, anchorOffset - m_windowSpan / 3);
    if (start + m_windowSpan > m_fileSize)
        start = std::max(0LL, m_fileSize - m_windowSpan);

    if (m_presentation == Presentation::Hex) {
        start -= start % m_hexBytesPerRow;
    } else if (start > 0) {
        const qint64 lookBehind = std::min<qint64>(kMaxTextProjectionBytes, start);
        const QByteArray prefix = sourceBytes(start - lookBehind,
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
    if (m_presentation == Presentation::Text && end < m_fileSize
        && end - start < kMaxTextProjectionBytes) {
        const qint64 scanBytes = std::min(kMaxTextProjectionBytes - (end - start),
                                          m_fileSize - end);
        const QByteArray suffix = sourceBytes(end, static_cast<qsizetype>(scanBytes));
        const qsizetype lineEnd = suffix.indexOf('\n');
        end += lineEnd >= 0 ? lineEnd + 1 : suffix.size();
    }
    const qsizetype length = static_cast<qsizetype>(end - start);

    const QByteArray source = sourceBytes(start, length);
    end = start + source.size();
    QByteArray projection;
    if (m_presentation == Presentation::Hex)
        projection = formatHexWindow(reinterpret_cast<const uchar *>(source.constData()),
                                     source.size(), start);
    else
        projection = source;

    m_windowStart = start;
    m_windowEnd = end;
    m_editor->setReadOnly(false);
    m_editor->setText(m_presentation == Presentation::Hex
                          ? QString::fromLatin1(projection)
                          : QString::fromUtf8(projection));
    m_editor->setReadOnly(true);
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
    if (anchorOffset >= m_fileSize - 1) {
        m_scrollToEndAfterPaint = m_presentation == Presentation::Text
            && m_wordWrap;
        scrollProjectionToEnd();
        if (m_scrollToEndAfterPaint)
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
    if (m_selectionAnchor >= 0 && m_selectionCaret >= 0)
        applyVirtualSelection();
    syncAsciiScrollFromHex();
    emit windowReady(start, end, anchorOffset);
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
    if (m_selectionAnchor >= 0 && m_selectionCaret >= 0)
        return;
    const qint64 anchorPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETANCHOR);
    const qint64 caretPosition = directScintilla(
        m_editor, QsciScintillaBase::SCI_GETCURRENTPOS);
    if (anchorPosition == caretPosition) {
        m_selectionAnchor = sourceOffsetForProjectedPosition(anchorPosition, false);
        m_selectionCaret = m_selectionAnchor;
    } else {
        m_selectionAnchor = sourceOffsetForProjectedPosition(anchorPosition, true);
        m_selectionCaret = sourceOffsetForProjectedPosition(caretPosition, true);
    }
}

void WindowedReadOnlyProvider::updateVirtualSelection(qint64 caretOffset)
{
    if (m_selectionAnchor < 0)
        beginVirtualSelection();
    m_selectionCaret = std::clamp(caretOffset, 0LL, m_fileSize);
    applyVirtualSelection();
}

void WindowedReadOnlyProvider::clearVirtualSelection()
{
    m_selectionAnchor = -1;
    m_selectionCaret = -1;
}

void WindowedReadOnlyProvider::applyVirtualSelection()
{
    if (m_path.isEmpty() || m_selectionAnchor < 0 || m_selectionCaret < 0)
        return;
    const qint64 low = std::min(m_selectionAnchor, m_selectionCaret);
    const qint64 high = std::max(m_selectionAnchor, m_selectionCaret);
    const qint64 visibleLow = std::clamp(low, m_windowStart, m_windowEnd);
    const qint64 visibleHigh = std::clamp(high, m_windowStart, m_windowEnd);
    const qint64 startPosition = projectedPositionForSourceOffset(visibleLow);
    const qint64 endPosition = projectedPositionForSourceOffset(visibleHigh);
    const bool forward = m_selectionCaret >= m_selectionAnchor;
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
    const int linesOnScreen = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_LINESONSCREEN)));
    const int lastFirst = std::max(0, projectedDisplayLineCount() - linesOnScreen);
    directScintilla(m_editor, QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                    static_cast<ScintillaUnsigned>(lastFirst));
    syncAsciiScrollFromHex();
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

    // Text presentation follows the normal editor contract: the gutter is a
    // decimal text-line landmark, never a hexadecimal byte address. Windowed
    // projections may be replaced while browsing, so these are deliberately
    // the lines currently represented by the editor; a future completed global
    // LineIndex may supply an absolute base without changing the visual mode.
    directScintilla(m_editor, QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    const int lines = std::max(1, static_cast<int>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETLINECOUNT)));
    const int lineDigits = std::max(
        4, static_cast<int>(QString::number(lines).size()) + 1);
    m_editor->setMarginWidth(0, QString(lineDigits, QLatin1Char('9')));
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
    return clampOffset(m_windowStart + std::max(0LL, localPosition), m_fileSize);
}

qint64 WindowedReadOnlyProvider::sourceOffsetForProjectedPosition(
    qint64 position, bool endBoundary) const
{
    if (m_path.isEmpty())
        return 0;
    position = std::max(0LL, position);
    if (m_presentation == Presentation::Text)
        return std::clamp(m_windowStart + position, 0LL, m_fileSize);

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
        static_cast<ScintillaUnsigned>(localOffset)));
}

qint64 WindowedReadOnlyProvider::projectedPositionForSourceOffset(qint64 offset) const
{
    const qint64 localOffset = std::clamp(offset - m_windowStart, 0LL,
                                          std::max(0LL, m_windowEnd - m_windowStart));
    if (m_presentation == Presentation::Text)
        return localOffset;
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
    qint64 localStart = absoluteOffset - m_windowStart;
    qint64 localEnd = localStart + std::max(0LL, selectionBytes);
    if (m_presentation == Presentation::Hex) {
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
    QString ascii;
    ascii.reserve(source.size() + source.size() / std::max(1, m_hexBytesPerRow));
    for (qsizetype row = 0; row < source.size(); row += m_hexBytesPerRow) {
        if (row > 0)
            ascii.append(QLatin1Char('\n'));
        const qsizetype count = std::min<qsizetype>(m_hexBytesPerRow,
                                                     source.size() - row);
        for (qsizetype column = 0; column < count; ++column) {
            const uchar byte = uchar(source.at(row + column));
            ascii.append(byte >= 0x20 && byte <= 0x7e
                             ? QChar(byte) : QLatin1Char('.'));
        }
    }
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
        || m_syncingHexSelection || m_syncingAsciiSelection) {
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
        || m_syncingAsciiSelection || m_syncingHexSelection) {
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
        m_selectionAnchor = anchor;
        m_selectionCaret = caret;
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
