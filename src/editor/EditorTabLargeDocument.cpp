#include "EditorTab.h"
#include "HexProjection.h"
#include "EditorSurfaceStyle.h"
#include "EditorSurfaceAccessibility.h"
// Hex character rows use the shared measured Scintilla-authoritative grid.
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
#include "ReadOnlyTailController.h"
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
#include <utility>
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

// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================
void EditorTab::setViewModeL2(ViewMode mode)
{
    if (!m_l2View || m_l2View->viewMode() == mode)
        return;

    QString error;
    if (!bridgeFlushOverlayToPatches(&error)) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(error, 6000);
        return;
    }

    m_l2View->setViewMode(mode);
    invalidateL2ProjectionSearchState();
    if (windowedReadOnlyActive()) {
        const QPointer<EditorTab> owner(this);
        m_windowedReadOnlyProvider->setPresentation(
            mode == ViewMode::Hex ? WindowedReadOnlyProvider::Presentation::Hex
                                  : WindowedReadOnlyProvider::Presentation::Text);
        if (!owner) return;
        // editor() remains the compatibility surface queried by commands,
        // session tests and accessibility tooling even while the windowed
        // provider owns the visible projection. Keep its gutter semantics in
        // sync so Binary->Text never reports hexadecimal addresses as lines.
        m_editor->SendScintilla(QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
        const bool trustedTextLines = mode == ViewMode::Text
            && !isBinaryDocument();
        m_editor->setMarginLineNumbers(0, trustedTextLines);
        m_editor->setMarginType(0, trustedTextLines
                                      ? QsciScintilla::NumberMargin
                                      : QsciScintilla::TextMargin);
        return;
    }
    // The overlay representation changes between decoded text and a reversible
    // two-digit byte grid. Refill even when the mmap active range did not move.
    const qint64 start = m_l2View->activeWindowByteStart();
    const qint64 end = m_l2View->activeWindowByteEnd();
    clearL2BridgeBaseline();
    if (end > start)
        bridgePopulateScintilla(start, end);
}

ViewMode EditorTab::viewModeL2() const
{
    return m_l2View ? m_l2View->viewMode() : ViewMode::Text;
}

bool EditorTab::isL2Active() const
{
    // 不能用 currentIndex()==1：进入 L2 后 Scintilla 会被 reparent 到 VFV 的
    // viewport，QStackedWidget 随即自动移除原 L1 page，VFV 的 index 会从 1 变成 0。
    // 用稳定的页面对象身份判断，避免菜单状态和实际编辑模式永久错位。
    return m_l2Mode && m_stack && m_l2View
        && (m_stack->currentWidget() == m_l2View
            || m_stack->currentWidget() == m_windowedReadOnlyProvider);
}

qint64 EditorTab::currentByteOffset() const
{
    // m_lastL2Byte is a compatibility/search cache. Programmatic navigation
    // and byte selections update VirtualFileView directly, so session state
    // must read the provider's authoritative 64-bit caret position.
    if (windowedReadOnlyActive() || m_searchSource->usesWindowedSource())
        return m_windowedReadOnlyProvider->globalOffset();
    if (m_l2Mode && m_l2View)
        return m_l2View->cursorByte();
    return m_lastL2Byte;
}

// ============================================================================
//  L2 路径（VirtualFileView 大文件虚拟视图）—— 独立实现，不影响 L1
// ============================================================================

VirtualFileView *EditorTab::ensureL2View()
{
    if (!m_l2View) {
        m_l2View = new VirtualFileView(this);
        // 加进 QStackedWidget（index 1 = L2）
        if (m_stack && m_stack->indexOf(m_l2View) < 0)
            m_stack->addWidget(m_l2View);
        emit externalDropSurfaceCreated(m_l2View);
        emit externalDropSurfaceCreated(m_l2View->viewport());

        // 连接 L2 信号：缓存数据 + 转发 EditorTab 的统一/专用信号
        connect(m_l2View, &VirtualFileView::cursorPositionChanged,
                this, [this](int line, int col) {
                    m_lastL2Line = line;
                    m_lastL2Col  = col;
                    if (m_l2View && m_l2View->isOpen()) {
                        m_lastL2Byte = m_l2View->viewMode() == ViewMode::Hex
                            ? qMin(m_l2View->virtualFileSize(),
                                   qint64(line) * m_l2View->hexBytesPerRow()
                                       + qint64(col))
                            : m_l2View->lineIndex().lineToByteOffset(line) + col;
                    }
                    // 转发统一 cursorPositionChanged：MainWindow 状态栏更新走同一个
                    emit cursorPositionChanged(line, col);
                    emit l2CursorPositionChanged(line, col);
                });
        connect(m_l2View, &VirtualFileView::fileOpenedChanged,
                this, &EditorTab::l2FileOpenedChanged);
        connect(m_l2View, &VirtualFileView::saveNeededChanged,
                this, [this](bool needed) {
                    if (needed)
                        advanceContentRevision();
                    m_l2SaveNeeded = needed;
                    emit modificationChanged(needed);
                    emit titleChanged(tabTitle());
                    emit l2SaveNeededChanged(needed);
                });
        connect(m_l2View, &VirtualFileView::undoRedoChanged,
                this, &EditorTab::l2UndoRedoChanged);
        connect(m_l2View, &VirtualFileView::undoRedoChanged,
                this, &EditorTab::commandCapabilitiesChanged);
        connect(m_l2View, &VirtualFileView::lineBuildProgress,
                this, &EditorTab::l2LineBuildProgress);
        connect(m_l2View, &VirtualFileView::lineBuildFinished,
                this, &EditorTab::l2LineBuildFinished);
        connect(m_l2View, &VirtualFileView::lineBuildFinished,
                this, [this](bool ok) {
                    if (ok) {
                        refreshTextProjectionMargin();
                        alignTextOverlayToGlobalScroll();
                    }
                });
    }
    return m_l2View;
}

WindowedReadOnlyProvider *EditorTab::ensureWindowedReadOnlyProvider()
{
    if (!m_windowedReadOnlyProvider) {
        m_windowedReadOnlyProvider = new WindowedReadOnlyProvider(this);
        connect(m_windowedReadOnlyProvider->tailController(), &ReadOnlyTailController::stateChanged,
                this, &EditorTab::commandCapabilitiesChanged);
        m_windowedReadOnlyProvider->setObjectName(
            QStringLiteral("productionWindowedReadOnlyProvider"));
        m_windowedReadOnlyProvider->editor()->setWhitespaceVisibility(
            m_whitespaceVisible ? QsciScintilla::WsVisible
                                : QsciScintilla::WsInvisible);
        m_windowedReadOnlyProvider->editor()->setEolVisibility(m_eolVisible);
        if (m_stack && m_stack->indexOf(m_windowedReadOnlyProvider) < 0)
            m_stack->addWidget(m_windowedReadOnlyProvider);
        emit externalDropSurfaceCreated(m_windowedReadOnlyProvider);
        emit externalDropSurfaceCreated(m_windowedReadOnlyProvider->editor());
        emit externalDropSurfaceCreated(
            m_windowedReadOnlyProvider->editor()->viewport());
        emit externalDropSurfaceCreated(
            m_windowedReadOnlyProvider->asciiPreview());
        emit externalDropSurfaceCreated(
            m_windowedReadOnlyProvider->asciiPreview()->viewport());
        emit externalDropSurfaceCreated(
            m_windowedReadOnlyProvider->globalScrollBar());
        connect(m_windowedReadOnlyProvider->editor(),
                &QsciScintilla::selectionChanged,
                this, &EditorTab::commandCapabilitiesChanged);
        connect(m_windowedReadOnlyProvider, &WindowedReadOnlyProvider::windowReady,
                this, [this](qint64, qint64, qint64 anchor) {
            m_lastL2Byte = anchor;
            if (m_l2View && m_l2View->viewMode() == ViewMode::Hex) {
                const int row = m_windowedReadOnlyProvider->hexBytesPerRow();
                m_lastL2Line = int(qMin<qint64>(anchor / row, INT_MAX));
                m_lastL2Col = int(anchor % row);
            } else {
                m_lastL2Line = 0;
                m_lastL2Col = 0;
            }
            emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
            emit l2CursorPositionChanged(m_lastL2Line, m_lastL2Col);
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::sourceFileChanged,
                this, [this](qint64, qint64) {
            advanceContentRevision();
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::sourceVersionAccepted,
                this, [this, provider = m_windowedReadOnlyProvider](QString path, DiskFileVersion version) {
            if (provider == m_windowedReadOnlyProvider && m_l2ReadOnlyByPolicy
                && !isSecureDocument() && version.valid && !version.sampleHash.isEmpty()
                && sameDiskFilePath(path, version.absolutePath)
                && sameDiskFilePath(path, m_documentSession.state().location))
                m_documentSession.setDiskVersion(std::move(version));
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::sourceReloadFailed,
                this, [this](const QString &message) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(message, 8000);
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::characterCountChanged,
                this, [this](qint64, bool) { emit characterCountChanged(); });
    }
    return m_windowedReadOnlyProvider;
}

bool EditorTab::windowedReadOnlyActive() const
{
    return m_l2Mode && m_stack && m_windowedReadOnlyProvider
        && m_stack->currentWidget() == m_windowedReadOnlyProvider;
}

QsciScintilla *EditorTab::editor() const
{
    // Expose the actual user-visible Scintilla surface. Returning the dormant
    // compatibility editor in S2/S3 made menu, accessibility and GUI checks
    // observe stale Hex margins and scroll state unrelated to the screen.
    if (windowedReadOnlyActive() && m_windowedReadOnlyProvider)
        return m_windowedReadOnlyProvider->editor();
    return m_editor;
}

EditorTab::LargeDocumentRuntimeState EditorTab::largeDocumentRuntimeState() const
{
    if (!m_l2Mode)
        return LargeDocumentRuntimeState::None;
    if (m_l2ReadOnlyByPolicy)
        return LargeDocumentRuntimeState::ReadOnlyAnalysis;
    // B1 Hex uses the byte provider directly and is already editable; it is
    // not an S2/S3 analysis state. Keep None so the status bar can
    // expose the more useful per-document INS/OVR mode.
    return LargeDocumentRuntimeState::None;
}

qint64 EditorTab::contentSizeBytes() const
{
    if (windowedReadOnlyActive() || m_searchSource->usesWindowedSource())
        return m_windowedReadOnlyProvider->fileSize();
    if (m_l2Mode && m_l2View && m_l2View->isOpen())
        return m_l2View->virtualFileSize();
    // For a saved ordinary text file the user-visible size is the source file
    // on disk, not Scintilla's internal UTF-8 buffer length. Modified content
    // remains a separate editor state until the next successful save refreshes
    // this bounded disk-version snapshot.
    if (!isSecureDocument() && m_documentSession.diskVersion().valid)
        return m_documentSession.diskVersion().size;
    return m_editor ? static_cast<qint64>(m_editor->length()) : 0;
}

qint64 EditorTab::contentCharacterCount() const
{
    const bool hexMode = isL2Active() && viewModeL2() == ViewMode::Hex;
    if (hexMode)
        return contentSizeBytes();
    if (windowedReadOnlyActive() || m_searchSource->usesWindowedSource())
        return m_windowedReadOnlyProvider->characterCount();

    if (m_characterCountRevision == m_documentSession.state().revision
        && m_cachedCharacterCountHex == hexMode) {
        return m_cachedCharacterCount;
    }

    qint64 count = 0;
    if (m_l2Mode && m_l2View && m_l2View->isOpen()) {
        // The remaining editable mmap path is bounded to B1 (<32 MiB), so one
        // exact decode is acceptable and is cached until the byte model changes.
        const QByteArray bytes = m_l2View->readByteRange(
            0, m_l2View->virtualFileSize());
        count = QString::fromUtf8(bytes).toUcs4().size();
    } else if (m_editor) {
        count = static_cast<qint64>(m_editor->SendScintilla(
            QsciScintillaBase::SCI_COUNTCHARACTERS, 0,
            m_editor->length()));
    }
    m_characterCountRevision = m_documentSession.state().revision;
    m_cachedCharacterCountHex = hexMode;
    m_cachedCharacterCount = count;
    return count;
}

void EditorTab::ensureHexAsciiPreview(QWidget *parent)
{
    if (!parent)
        return;
    if (!m_hexAsciiPreview) {
        m_hexAsciiPreview = new QPlainTextEdit(parent);
        m_hexAsciiPreview->setObjectName(QStringLiteral("HexAsciiPreview"));
        m_hexAsciiPreview->setToolTip(
            QStringLiteral("字符栏（按当前编码进行多字节解码；编辑仍作用于原始字节）"));
        m_hexAsciiPreview->setFont(m_editor->font());
        m_hexAsciiPreview->setReadOnly(isReadOnlyByPolicy());
        // The character pane accepts text through the selected encoding. Its
        // event filter consumes preedit and applies each complete IME commit as
        // one byte transaction, so native composition can stay available
        // without letting temporary preedit mutate the fixed byte grid.
        m_hexAsciiPreview->setAttribute(Qt::WA_InputMethodEnabled, true);
        m_hexAsciiPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_hexAsciiPreview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_hexAsciiPreview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_hexAsciiPreview->document()->setDocumentMargin(0);
        m_hexAsciiPreview->installEventFilter(this);
        m_hexAsciiPreview->viewport()->installEventFilter(this);
        emit externalDropSurfaceCreated(m_hexAsciiPreview);
        emit externalDropSurfaceCreated(m_hexAsciiPreview->viewport());
        connect(m_hexAsciiPreview, &QPlainTextEdit::cursorPositionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
        connect(m_hexAsciiPreview, &QPlainTextEdit::selectionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
        configureEditableHexOverlayAccessibility(m_editor, m_hexAsciiPreview, m_l2View->verticalScrollBar());
    } else if (m_hexAsciiPreview->parentWidget() != parent) {
        m_hexAsciiPreview->setParent(parent);
    }
}

int EditorTab::preferredHexBytesPerRow() const
{
    if (!m_l2View || !m_l2View->viewport() || !m_editor)
        return 16;
    const int width = m_l2View->viewport()->width();
    const int charWidth = qMax(7, QFontMetrics(m_editor->font()).horizontalAdvance(
                                      QLatin1Char('0')));
    const int addressDigits = qMax(
        8, QString::number(qMax<qint64>(0, m_l2View->virtualFileSize() - 1), 16).size());
    // Do not read Scintilla's current margin width here: on first projection it
    // still belongs to the previous presentation, making the result order-dependent.
    const int addressWidth = charWidth * (addressDigits + 2) + 16;
    for (int candidate = 64; candidate >= 8; candidate -= 4) {
        const int required = addressWidth
            + charWidth * (candidate * 3 + candidate + 12) + 52;
        if (required <= width)
            return candidate;
    }
    return 8;
}

void EditorTab::adaptHexBytesPerRow(bool repopulate)
{
    if (!m_l2View || m_l2View->viewMode() != ViewMode::Hex)
        return;
    const int desired = preferredHexBytesPerRow();
    if (desired == m_hexBytesPerRow)
        return;

    if (m_bridge.windowStart >= 0 && m_editor->isModified()) {
        QString error;
        if (!bridgeFlushOverlayToPatches(&error))
            return;
    }
    m_hexBytesPerRow = desired;
    m_l2View->setHexBytesPerRow(desired);
    if (repopulate && m_bridge.windowStart >= 0 && m_bridge.windowLen > 0) {
        bridgePopulateScintilla(m_bridge.windowStart,
                                m_bridge.windowStart + m_bridge.windowLen);
    }
}

void EditorTab::layoutL2Overlay()
{
    if (!m_l2View || !m_editor)
        return;
    QWidget *viewport = m_l2View->viewport();

    if (!viewport)

        return;

    const bool hex = m_l2View->viewMode() == ViewMode::Hex;
    if (!hex) {
        configureEditorPresentationAccessibility(m_editor, false);
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        m_l2View->setHorizontalScrollBarPolicy(
            m_l2View->wordWrap() ? Qt::ScrollBarAlwaysOff
                                 : Qt::ScrollBarAsNeeded);
        // Text and Hex share the provider's one global scroll contract. Native
        // Scintilla scrollbars describe only the bounded projection and must
        // never compete visually or semantically with the whole-file bars.
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 0UL);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR, 0UL);
        if (m_hexAsciiPreview)
            m_hexAsciiPreview->hide();
        m_editor->setGeometry(0, 0, viewport->width(), viewport->height());
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET,
                                m_l2View->horizontalScrollBar()->value());
        return;
    }
    ensureHexAsciiPreview(viewport);
    configureEditorPresentationAccessibility(m_editor, true);
    m_editor->setAttribute(Qt::WA_InputMethodEnabled, false);
    // The B1 Hex document has exactly one user-visible position truth: the
    // provider's global vertical bar. The projected Scintilla and the mirrored
    // ASCII pane must not expose independent scrollbars.
    m_l2View->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 1UL);
    const int charWidth = qMax(7, QFontMetrics(m_editor->font()).horizontalAdvance(
                                      QLatin1Char('0')));
    const int desiredPreviewWidth = charWidth * (m_hexBytesPerRow + 2) + 24;
    const int previewWidth = qMin(qMax(1, viewport->width() - 1),
                                  qMin(desiredPreviewWidth,
                                       qMax(120, viewport->width() / 4)));
    // The two visible byte panes must tile the viewport. The old preferred
    // width left a third, inert strip to the right on wide windows, which made
    // the Hex page look unfinished and provided no hit target or information.
    const int editorWidth = qMax(1, viewport->width() - previewWidth);
    m_editor->setGeometry(0, 0, editorWidth, viewport->height());
    m_hexAsciiPreview->setGeometry(editorWidth, 0, previewWidth, viewport->height());
    m_hexAsciiPreview->show();
    m_hexAsciiPreview->raise();
    m_editor->raise();
}

void EditorTab::alignTextOverlayToGlobalScroll()
{
    if (!m_l2Mode || !m_l2View || !m_editor
        || m_l2View->viewMode() != ViewMode::Text
        || m_bridge.windowStart < 0 || m_bridge.baseline.isEmpty()) {
        return;
    }

    QScrollBar *global = m_l2View->verticalScrollBar();
    const LineIndex &index = m_l2View->lineIndex();
    const qint64 virtualSize = m_l2View->virtualFileSize();
    qint64 targetByte = 0;
    if (index.isIncomplete() && index.realCoverageBytes() < virtualSize) {
        const qint64 minimum = global->minimum();
        const qint64 span = qMax<qint64>(
            1, qint64(global->maximum()) - minimum);
        const qint64 value = qBound<qint64>(
            minimum, qint64(global->value()),
            qint64(global->maximum())) - minimum;
        targetByte = (virtualSize * value) / span;
    } else {
        targetByte = index.lineToByteOffset(global->value());
        if (m_l2View->patches().isModified())
            targetByte = m_l2View->patches().origToVirtForLineStart(targetByte);
    }
    targetByte = qBound<qint64>(m_bridge.windowStart, targetByte,
                                m_bridge.windowStart + m_bridge.windowLen);
    const qint64 localByte = targetByte - m_bridge.windowStart;
    const long projectedPosition = bridgeProjectedPositionForSourceOffset(localByte);
    const int localDocumentLine = int(m_editor->SendScintilla(
        QsciScintillaBase::SCI_LINEFROMPOSITION, projectedPosition));
    long displayLine = m_editor->SendScintilla(
        QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
        static_cast<unsigned long>(qMax(0, localDocumentLine)));
    if (global->value() == global->maximum()
        && m_bridge.windowStart + m_bridge.windowLen >= virtualSize) {
        const int lastDocumentLine = qMax(0, m_editor->lines() - 1);
        const long lastVisibleStart = m_editor->SendScintilla(
            QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
            static_cast<unsigned long>(lastDocumentLine));
        const long lastWrapCount = m_editor->SendScintilla(
            QsciScintillaBase::SCI_WRAPCOUNT,
            static_cast<unsigned long>(lastDocumentLine));
        displayLine = qMax<long>(0, lastVisibleStart)
            + qMax<long>(1, lastWrapCount);
    }
    const bool wasApplying = m_bridge.applying;
    m_bridge.applying = true;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                            static_cast<unsigned long>(qMax<long>(0, displayLine)));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET,
                            m_l2View->wordWrap()
                                ? 0 : m_l2View->horizontalScrollBar()->value());
    m_bridge.applying = wasApplying;
}

void EditorTab::alignHexOverlayToGlobalScroll()
{
    if (!m_l2Mode || !m_l2View || !m_editor
        || m_l2View->viewMode() != ViewMode::Hex
        || m_bridge.windowStart < 0 || m_hexBytesPerRow <= 0) {
        return;
    }
    const qint64 globalFirst = m_l2View->verticalScrollBar()->value();
    const qint64 windowFirst = m_bridge.windowStart / m_hexBytesPerRow;
    const qint64 requestedLocal = globalFirst - windowFirst;
    const int linesOnScreen = qMax(1, int(m_editor->SendScintilla(
        QsciScintillaBase::SCI_LINESONSCREEN)));
    const int lastLocalFirst = qMax(0, m_editor->lines() - linesOnScreen);
    if (requestedLocal < 0 || requestedLocal > lastLocalFirst)
        return;
    m_bridge.applying = true;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                            static_cast<unsigned long>(requestedLocal));
    if (m_hexAsciiPreview)
        m_hexAsciiPreview->verticalScrollBar()->setValue(int(requestedLocal));
    m_bridge.applying = false;
}

void EditorTab::refreshTextProjectionMargin()
{
    if (!m_l2Mode || !m_l2View || !m_editor
        || m_l2View->viewMode() != ViewMode::Text
        || m_bridge.windowStart < 0) {
        return;
    }

    if (isBinaryDocument()) {
        // Binary CR/LF bytes do not establish a trustworthy whole-file line
        // model. Show each projected row's authoritative source byte offset;
        // this is bounded to the active window and never scans the prefix.
        m_editor->SendScintilla(QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
        m_editor->setMarginLineNumbers(0, false);
        m_editor->setMarginType(0, QsciScintilla::TextMargin);
        const int lines = qMax(1, m_editor->lines());
        const int digits = qMax(
            8, QString::number(
                qMax<qint64>(0, m_l2View->virtualFileSize() - 1), 16).size());
        m_editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('0')));
        for (int localLine = 0; localLine < lines; ++localLine) {
            const long position = m_editor->SendScintilla(
                QsciScintillaBase::SCI_POSITIONFROMLINE,
                static_cast<unsigned long>(localLine));
            const qint64 offset = m_bridge.windowStart
                + bridgeSourceOffsetForProjectedPosition(qMax<long>(0, position));
            m_editor->setMarginText(
                localLine, QString::number(offset, 16)
                               .rightJustified(digits, QLatin1Char('0')).toUpper(), 0);
        }
        return;
    }

    const LineIndex &index = m_l2View->lineIndex();
    const qint64 originalStart = m_l2View->patches().isModified()
        ? m_l2View->patches().virtToOrig(m_bridge.windowStart)
        : m_bridge.windowStart;
    bool exactBase = !index.isIncomplete()
        || originalStart <= index.realCoverageBytes();
    int baseLine = 0;
    qint64 column = 0;
    if (exactBase && !m_l2View->patches().isModified()) {
        index.byteToLineCol(originalStart, &baseLine, &column);
    } else {
        // The compatibility B1/secure-binary bridge is capped below the large
        // windowed tiers.  Its background LineIndex may still be incomplete
        // when the user switches Hex -> Text, and patches can change newline
        // counts.  Count the authoritative virtual bytes up to this bounded
        // window instead of showing the false local 1..N gutter.
        constexpr qint64 Chunk = Q_INT64_C(4) * 1024 * 1024;
        qint64 cursor = 0;
        qint64 lineBreaks = 0;
        while (cursor < m_bridge.windowStart) {
            const qint64 end = qMin(m_bridge.windowStart, cursor + Chunk);
            const QByteArray bytes = m_l2View->readByteRange(cursor, end);
            if (bytes.size() != end - cursor) {
                exactBase = false;
                break;
            }
            lineBreaks += bytes.count('\n');
            cursor = end;
        }
        if (cursor == m_bridge.windowStart) {
            baseLine = int(qMin<qint64>(lineBreaks, INT_MAX));
            exactBase = true;
        }
    }

    m_editor->SendScintilla(QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
    const bool wholeDocumentProjection = m_bridge.windowStart == 0
        && m_bridge.windowLen >= m_l2View->virtualFileSize();
    if (wholeDocumentProjection) {
        // A complete byte-preserving Text projection has the same line-number
        // semantics as an ordinary S1 document. Use Scintilla's native gutter
        // so Binary -> Text does not retain the visual contract of Hex mode.
        m_editor->setMarginLineNumbers(0, true);
        m_editor->setMarginType(0, QsciScintilla::NumberMargin);
        const int digits = qMax(4, QString::number(qMax(1, m_editor->lines())).size());
        m_editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('9')));
        return;
    }
    m_editor->setMarginLineNumbers(0, false);
    m_editor->setMarginType(0, QsciScintilla::TextMargin);
    const int lines = qMax(1, m_editor->lines());
    const qint64 largest = exactBase ? qint64(baseLine) + lines : lines;
    const int digits = qMax(4, QString::number(qMax<qint64>(1, largest)).size());
    m_editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('9')));
    for (int localLine = 0; localLine < lines; ++localLine) {
        m_editor->setMarginText(
            localLine,
            exactBase ? QString::number(qint64(baseLine) + localLine + 1)
                      : QStringLiteral("?"),
            0);
    }
}

void EditorTab::activateL2Overlay(VirtualFileView *view)
{
    if (!view || !m_stack || !m_editor)
        return;
    m_stack->setCurrentWidget(view);

    QWidget *viewport = view->viewport();
    m_editor->setParent(viewport);
    m_editor->setGeometry(QRect(QPoint(0, 0), viewport->size()));
    m_editor->show();
    m_editor->raise();
    m_editor->setFocus();
    viewport->installEventFilter(this);
    layoutL2Overlay();

    // The provider object is stable for the tab lifetime. Install the bridge
    // once and reuse it for mmap files and decrypted in-memory binary payloads.
    if (!m_l2BridgeConnectionsInstalled) {
        m_bridge.syncVfvDebounce.setSingleShot(true);
        m_bridge.syncVfvDebounce.setInterval(250);
        connect(&m_bridge.syncVfvDebounce, &QTimer::timeout,
                this, &EditorTab::bridgeOnSyncDebounce);
        QObject::connect(m_editor, &QsciScintilla::SCN_UPDATEUI, this, [this]() {
            if (Q_UNLIKELY(!m_l2Mode || m_bridge.applying))
                return;
            if (m_hexAsciiPreview && m_hexAsciiPreview->isVisible()) {
                syncAsciiSelectionFromHex();
                const long first = m_editor->SendScintilla(
                    QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
                m_hexAsciiPreview->verticalScrollBar()->setValue(int(first));
            }
            // Paint, wrap-layout and projection replacement also emit
            // SCN_UPDATEUI. Treating every notification as user navigation
            // creates a closed feedback loop with the outer global scrollbar.
            // Only an input event recorded by eventFilter may drive the
            // Scintilla -> provider direction.
            if (m_bridge.viewportNavigationPending)
                m_bridge.syncVfvDebounce.start();
        });
        QObject::connect(view, &VirtualFileView::activeWindowChanged,
                         this, [this](qint64 byteStart, qint64 byteEnd) {
            if (m_l2Mode)
                bridgePopulateScintilla(byteStart, byteEnd);
        });
        QObject::connect(view->horizontalScrollBar(), &QScrollBar::valueChanged,
                         this, [this](int value) {
            if (!m_l2Mode || !m_l2View || !m_editor
                || m_l2View->viewMode() != ViewMode::Text) {
                return;
            }
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET,
                                    m_l2View->wordWrap() ? 0 : value);
        });
        QObject::connect(view->verticalScrollBar(), &QScrollBar::valueChanged,
                         this, [this](int) {
            if (!m_l2Mode || !m_l2View || !m_editor)
                return;
            // A value change may be programmatic (goto/range restoration) or
            // part of a user action. It may align an already-loaded Hex row,
            // but it must never start mmap IO or choose another active window;
            // VirtualFileView's slider/action signals exclusively own that.
            if (m_l2View->viewMode() == ViewMode::Hex)
                alignHexOverlayToGlobalScroll();
        });
        m_l2BridgeConnectionsInstalled = true;
    }

    view->syncActiveWindowToCursor(0);
    const qint64 start = view->activeWindowByteStart();
    const qint64 end = view->activeWindowByteEnd();
    if (start >= 0 && end > start)
        bridgePopulateScintilla(start, end);

    QTimer::singleShot(0, this, [this, view]() {
        if (!m_l2Mode || !m_l2View)
            return;
        if (view->viewport() && m_editor)
            layoutL2Overlay();
    });
}

void EditorTab::clearL2BridgeBaseline()
{
    if (m_hexAsciiPreview && isSecureBinaryDocument()) {
        const int previewLength = qMax(0,
            m_hexAsciiPreview->document()->characterCount() - 1);
        if (previewLength > 0)
            m_hexAsciiPreview->setPlainText(QString(previewLength, QLatin1Char('.')));
        m_hexAsciiPreview->clear();
    }
    if (!m_bridge.baseline.isEmpty() && isSecureBinaryDocument())
        m_bridge.baseline.fill(0);
    m_bridge.baseline.clear();
    m_bridge.baseline.squeeze();
    m_bridge.projectedToSource.clear();
    m_bridge.sourceBoundaries.clear();
    m_bridge.projectedBoundaries.clear();
    m_bridge.windowStart = -1;
    m_bridge.windowLen = 0;
}

// ============================================================================
//  L2ScintillaBridge：VFV 不做任何绘制；Scintilla 是唯一 UI。
// ============================================================================

void EditorTab::bridgeReparentEditorToStack()
{
    if (!m_stack) return;
    if (m_hexAsciiPreview)
        m_hexAsciiPreview->hide();
    // The same Scintilla instance is reused by ordinary Text documents after
    // leaving the Hex provider. Never leak the Hex-specific IME policy.
    m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
    // 先卸载 VFV viewport 的 eventFilter（避免 reparent 后仍收到无效 Resize）
    if (m_l2View && m_l2View->viewport())
        m_l2View->viewport()->removeEventFilter(this);
    // 如果 Scintilla 被 reparent 到 VFV viewport（L2 overlay），把它移回 m_stack page 0
    // （父对象不同或 parent 仍在 stack 外的 QAbstractScrollArea viewport）
    const bool stacked = (m_stack->indexOf(m_editor) >= 0);
    if (!stacked) {
        // 从当前父（可能 VFV viewport）拆下来，放回 stack page 0
        m_editor->setParent(m_stack);
        if (m_stack->indexOf(m_editor) < 0)
            m_stack->insertWidget(0, m_editor);
    }
    // 确保 stack page 0 仍指向 m_editor（即使重复 insertWidget 会被 move，再设一次 idx=0）
    if (m_stack->indexOf(m_editor) < 0)
        m_stack->insertWidget(0, m_editor);
}

void EditorTab::bridgePopulateScintilla(qint64 s, qint64 e)
{
    if (!m_l2View || !m_l2View->isOpen()) return;
    if (s < 0 || e <= s) return;

    // 桥 2：如果已有有效 baseline 窗口（用户可能做了编辑），先 flush diff 回 patches 再切窗口
    if (m_bridge.windowStart >= 0 && m_bridge.windowLen > 0
        && !(m_bridge.windowStart == s && m_bridge.windowLen == (e - s))) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(flushError, 6000);
            return;
        }
    }

    // A debounce started by the previous projection must never be allowed to
    // reinterpret the caret after this window has replaced it.
    m_bridge.syncVfvDebounce.stop();
    m_bridge.viewportNavigationPending = false;
    const quint64 projectionGeneration = ++m_bridge.projectionGeneration;
    adaptHexBytesPerRow(false);
    m_bridge.applying = true;
    QByteArray bytes = m_l2View->readByteRange(s, e);
    m_bridge.projectedToSource.clear();
    m_bridge.sourceBoundaries.clear();
    m_bridge.projectedBoundaries.clear();
    // 走 SCI_SETTEXT（setText），对于 W_active 上限 8MB，主线程 setText 约 100ms 内完成，
    // 不会触发"UI 线程卡死"；甩滚时 bridge 不频繁触发（VFV debounce 已节流）。
    m_bridge.installingText = true;
    m_editor->setReadOnly(false);
    if (m_l2View->viewMode() == ViewMode::Hex) {
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, false);
        m_editor->setUtf8(true);
        m_editor->setText(formatHexBytes(bytes, m_hexBytesPerRow));
        // The row width is chosen to fit the viewport. A reused Scintilla can
        // retain an x-offset from a previous text/Hex projection, which would
        // otherwise hide the first bytes while the absolute offset still says 0.
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, 0);
        ensureHexAsciiPreview(m_l2View->viewport());
        QString ascii = decodeByteGridText(bytes, m_l2View->displayEncoding(),
                                           m_hexBytesPerRow);
        const int oldPreviewLength = qMax(0,
            m_hexAsciiPreview->document()->characterCount() - 1);
        if (isSecureBinaryDocument() && oldPreviewLength > 0) {
            m_hexAsciiPreview->setPlainText(
                QString(oldPreviewLength, QLatin1Char('.')));
        }
        m_hexAsciiPreview->setPlainText(ascii);
        alignPlainTextRowsToScintilla(m_hexAsciiPreview, m_editor);
        m_hexAsciiPreview->setReadOnly(isReadOnlyByPolicy());
        // QPlainTextEdit can recalculate input-method support when editability
        // changes. The character pane remains an encoding-aware input surface;
        // its event filter owns preedit and commit atomically.
        m_hexAsciiPreview->setAttribute(Qt::WA_InputMethodEnabled,
                                        !isReadOnlyByPolicy());
        if (isSecureBinaryDocument() && !ascii.isEmpty())
            ascii.fill(QChar(0));
        m_editor->setWrapMode(QsciScintilla::WrapNone);
        m_editor->setMarginLineNumbers(0, false);
        // QScintilla implements setMarginLineNumbers(false) by forcing a
        // SymbolMargin, so TextMargin must be applied after that call.
        m_editor->setMarginType(0, QsciScintilla::TextMargin);
        const qint64 lastOffset = s + qMax<qint64>(0, bytes.size() - 1);
        const int addressDigits = qMax(8, QString::number(lastOffset, 16).size());
        m_editor->setMarginWidth(0, QString(addressDigits + 2, QLatin1Char('0')));
        const int lines = (bytes.size() + m_hexBytesPerRow - 1) / m_hexBytesPerRow;
        for (int line = 0; line < lines; ++line) {
            const QString address = QString::number(
                                        s + qint64(line) * m_hexBytesPerRow, 16)
                                        .rightJustified(addressDigits, QLatin1Char('0'))
                                        .toUpper();
            m_editor->setMarginText(line, address, 0);
        }
        m_bridge.windowStart = s;
        m_bridge.windowLen = e - s;
        layoutL2Overlay();
        alignHexOverlayToGlobalScroll();
    } else {
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        if (m_hexAsciiPreview)
            m_hexAsciiPreview->hide();
        // Text presentation always means decoded text, independently of the
        // content classifier. Keep a bounded source/projection map so a Binary
        // file can display replacement characters without losing authoritative
        // byte coordinates for search navigation and selection.
        const DecodedTextProjection decoded = decodeTextProjection(
            bytes, m_l2View->displayEncoding(), s == 0);
        m_bridge.projectedToSource = decoded.projectedToSource;
        m_bridge.sourceBoundaries = decoded.sourceBoundaries;
        m_bridge.projectedBoundaries = decoded.projectedBoundaries;
        m_editor->setUtf8(true);
        m_editor->setText(decoded.text);
        m_editor->setWrapMode(m_l2View->wordWrap() ? QsciScintilla::WrapWord
                                                   : QsciScintilla::WrapNone);
    }
    m_bridge.installingText = false;
    m_editor->setModified(false);
    // The L2 overlay is a projection of VirtualFileView, not a second document.
    // setText/SCI_ADDTEXT can leave native Scintilla undo records behind on
    // some builds.  If those records win over PatchList, Ctrl+Z rewinds the
    // formatted Hex grid and sends the mirrored ASCII caret to the window end.
    // A completed projection rebuild is therefore always an undo boundary;
    // user-visible L2 history lives exclusively in PatchList.
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    // Decoded Binary text is an interpretation of the bytes, not a reversible
    // edit buffer. Raw mutation remains available in Hex mode; Text mode is a
    // safe read-only analysis surface.
    const bool decodedBinaryText = isBinaryDocument()
        && m_l2View->viewMode() == ViewMode::Text;
    m_editor->setReadOnly(isReadOnlyByPolicy() || decodedBinaryText);
    // Both Scintilla and QPlainTextEdit may recalculate input-method support
    // when editability changes. This final assignment is authoritative for
    // every initial load, projection remap and Text<->Hex round trip.
    m_editor->setAttribute(
        Qt::WA_InputMethodEnabled,
        m_l2View->viewMode() != ViewMode::Hex);
    if (!m_bridge.baseline.isEmpty() && isSecureBinaryDocument())
        m_bridge.baseline.fill(0);
    m_bridge.baseline    = std::move(bytes);
    m_bridge.windowStart = s; m_bridge.windowLen = e - s;
    m_hexMouseSelection.applySourceSelectionToProjection(m_l2View->selectionAnchorByte(), m_l2View->selectionCaretByte());
    if (m_l2View->viewMode() == ViewMode::Text)
        refreshTextProjectionMargin();
    if (m_l2View->viewMode() == ViewMode::Text)
        alignTextOverlayToGlobalScroll();
    applyL2SearchHighlightsToOverlay();
    // Scintilla/Qt may deliver caret and paint notifications after setText has
    // returned. Keep them inside the projection transaction for one event-loop
    // turn. The generation check prevents an older queued release from opening
    // the gate while a newer projection is still being built.
    QTimer::singleShot(0, this, [this, projectionGeneration]() {
        if (m_bridge.projectionGeneration == projectionGeneration) {
            m_bridge.applying = false;
        }
    });
}

bool EditorTab::bridgeFlushOverlayToPatches(QString* errMsg)
{
    if (!m_l2View || !m_l2View->isOpen()) return true;
    if (isReadOnlyByPolicy()) return true;
    if (isBinaryDocument() && m_l2View->viewMode() == ViewMode::Text)
        return true;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return true;
    if (!m_editor->isModified()) return true;

    QByteArray now;
    if (m_l2View->viewMode() == ViewMode::Hex) {
        if (!parseHexBytes(m_editor->text(), &now, errMsg))
            return false;
    } else {
        if (isBinaryDocument()) {
            const long length = m_editor->SendScintilla(
                QsciScintillaBase::SCI_GETLENGTH);
            now.resize(qMax<long>(0, length) + 1);
            m_editor->SendScintilla(
                QsciScintillaBase::SCI_GETTEXT,
                static_cast<uintptr_t>(now.size()), now.data());
            now.resize(qMax<long>(0, length));
        } else {
            now = m_editor->text().toUtf8();
        }
    }
    if (now == m_bridge.baseline) {
        m_editor->setModified(false);
        return true;
    }

    const qint64 s = m_bridge.windowStart;
    const QByteArray& oldBytes = m_bridge.baseline;
    const qint64 oldLen = oldBytes.size();

    // 最小可用 diff：先找到公共前缀 + 公共后缀（O(n)，8MB 规模毫秒级），
    // 只对中间变化区间做一次 replaceBytes，避免 Myers 复杂度爆炸。
    qint64 commonPrefix = 0;
    const qint64 cmpN = qMin<qint64>(oldLen, now.size());
    while (commonPrefix < cmpN && oldBytes[int(commonPrefix)] == now[int(commonPrefix)])
        ++commonPrefix;
    qint64 commonSuffix = 0;
    while (commonSuffix < cmpN - commonPrefix
           && oldBytes[int(oldLen - 1 - commonSuffix)] == now[int(now.size() - 1 - commonSuffix)])
        ++commonSuffix;

    const qint64 patchOldStart = commonPrefix;
    const qint64 patchOldLen   = oldLen - commonPrefix - commonSuffix;
    const qint64 patchNewStart = commonPrefix;
    const qint64 patchNewLen   = now.size() - commonPrefix - commonSuffix;

    QByteArray newData;
    if (patchNewLen > 0)
        newData = now.mid(int(patchNewStart), int(patchNewLen));

    // 16MB 粘贴硬顶：超出阈值时不允许通过窗口内 flush 写入，避免 PatchList 单条 patch 过大。
    // 用户会被要求走"小范围粘贴分批"，后续可升级为 pasteBytes 分块路径（P1 之后）。
    constexpr qint64 HARD_DELTA_BYTES = Q_INT64_C(16) * 1024 * 1024;
    if (qAbs(qint64(newData.size()) - patchOldLen) > HARD_DELTA_BYTES ||
        qint64(newData.size()) > HARD_DELTA_BYTES) {
        if (errMsg) *errMsg = QObject::tr("窗口内单步修改过大（>16MB），请分批操作或缩小窗口后再修改。");
        return false;
    }

    const qint64 globByte = s + patchOldStart;
    QString localErr;
    const bool ok = m_l2View->replaceBytes(globByte, qint32(patchOldLen), newData, &localErr);
    if (!ok) {
        if (errMsg) *errMsg = localErr.isEmpty() ? QObject::tr("写入 patch 失败") : localErr;
        return false;
    }

    // flush 成功：更新 baseline 为新文本（长度对应 virtualSize）
    // 注意：替换后窗口可能因为前后 patch 变化而长/短，但我们只保存 Scintilla 当前 now 作为
    // baseline，窗口边界以 s~s+virtualChangedLen 为准——下一次切窗口时桥 1 会重新对齐重灌，
    // 所以此处 baseline 按 Scintilla 实际文本即可。
    if (!m_bridge.baseline.isEmpty() && isSecureBinaryDocument())
        m_bridge.baseline.fill(0);
    m_bridge.baseline    = now;
    m_bridge.windowStart = s;
    m_bridge.windowLen   = now.size();
    m_editor->setModified(false);
    return true;
}

void EditorTab::advanceContentRevision()
{
    auto *navigation = m_searchSource->engine(true);
    const qint64 navigationTaskId = navigation && navigation->sourceInUse()
        ? navigation->lastTaskId() : 0;
    m_documentSession.advanceRevision();
    m_l1SearchCacheValid = false;
    m_l1SearchCache.clear();
    m_lastL1MatchIndex = -1;
    m_l1NavigationHitValid = false;
    m_l2NavigationHitValid = false;
    m_l2SearchCacheValid = false;
    clearSearchHighlights();
    const QPointer<EditorTab> owner(this);
    const qint64 l1TaskId = std::exchange(m_l1FindAllTaskId, 0);
    if (navigationTaskId > 0) navigation->requestCancel();
    if (l1TaskId > 0) AsyncTaskManager::instance().cancel(l1TaskId);
    if (!owner) return;
    cancelPendingL2SearchForContentChange();
    if (navigationTaskId > 0) AsyncTaskManager::instance().cancel(navigationTaskId);
    if (owner) emit contentChanged(m_documentSession.state().revision);
}

bool EditorTab::validateSearchSource()
{
    const QPointer<EditorTab> owner(this);
    QString error;
    const bool current = m_searchSource->validateCurrent(&error);
    if (owner && !current) m_lastSearchError = error;
    return owner && current;
}

void EditorTab::cancelPendingL2SearchForContentChange()
{
    if (!m_pendingL2Search.active || !m_l2View)
        return;
    if (LargeFileSearch *search = m_searchSource->engine(); search && search->sourceInUse()) {
        search->requestCancel();
        if (search->lastTaskId() > 0)
            AsyncTaskManager::instance().cancel(search->lastTaskId());
    }
}

void EditorTab::applyL2SearchHighlightsToOverlay()
{
    if (!m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, indicator,
                            QsciScintillaBase::INDIC_ROUNDBOX);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, indicator,
                            scintillaColor(searchHitColor()));
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, indicator, 90);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
    if (!isL2Mode() || m_bridge.windowStart < 0 || m_l2SearchHighlights.isEmpty())
        return;

    const qint64 windowStart = m_bridge.windowStart;
    const qint64 windowEnd = windowStart + m_bridge.windowLen;
    auto first = std::lower_bound(
        m_l2SearchHighlights.cbegin(), m_l2SearchHighlights.cend(), windowStart,
        [](const QPair<qint64, qint32> &match, qint64 value) {
            return match.first < value;
        });
    if (first != m_l2SearchHighlights.cbegin())
        --first;

    auto overlayPosition = [this](qint64 localByte) -> long {
        if (!m_l2View || m_l2View->viewMode() != ViewMode::Hex)
            return bridgeProjectedPositionForSourceOffset(localByte);
        const int line = int(qMin<qint64>(localByte / m_hexBytesPerRow, INT_MAX));
        const int column = int((localByte % m_hexBytesPerRow) * 3);
        return m_editor->positionFromLineIndex(line, column);
    };
    for (auto it = first; it != m_l2SearchHighlights.cend(); ++it) {
        const qint64 matchStart = it->first;
        const qint64 matchEnd = matchStart + qMax<qint32>(0, it->second);
        if (matchStart >= windowEnd)
            break;
        if (matchEnd <= windowStart)
            continue;
        const long start = overlayPosition(qMax(windowStart, matchStart) - windowStart);
        const long end = overlayPosition(qMin(windowEnd, matchEnd) - windowStart);
        if (start >= 0 && end > start) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                                    start, end - start);
        }
    }
}

qint64 EditorTab::bridgeSourceOffsetForProjectedPosition(
    qint64 position, bool endBoundary) const
{
    position = qMax<qint64>(0, position);
    if (!m_l2View || m_l2View->viewMode() == ViewMode::Hex
        || m_bridge.projectedToSource.isEmpty()) {
        return qMin(position, m_bridge.windowLen);
    }
    const qint32 source = TextProjectionPositionMapper::toSource(
        m_bridge.projectedToSource, position,
        endBoundary ? TextBoundaryAffinity::End
                    : TextBoundaryAffinity::Start);
    return qBound<qint64>(qint64(0), qint64(source), m_bridge.windowLen);
}

long EditorTab::bridgeProjectedPositionForSourceOffset(qint64 sourceOffset) const
{
    sourceOffset = qBound<qint64>(qint64(0), sourceOffset, m_bridge.windowLen);
    if (!m_l2View || m_l2View->viewMode() == ViewMode::Hex
        || m_bridge.sourceBoundaries.isEmpty()) {
        return long(qMin<qint64>(sourceOffset, LONG_MAX));
    }
    return long(TextProjectionPositionMapper::toProjected(
        m_bridge.sourceBoundaries, m_bridge.projectedBoundaries, sourceOffset));
}

void EditorTab::bridgeOnSyncDebounce()
{
    const bool navigationRequested = m_bridge.viewportNavigationPending;
    m_bridge.viewportNavigationPending = false;
    if (!navigationRequested || !m_l2View || !m_l2View->isOpen()
        || m_bridge.applying) return;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return;

    // At the global bottom, lazy wrap/layout notifications can temporarily
    // report a centre position from the previous projection. Feeding that
    // value back into the window allocator pulls the file away from EOF and
    // makes the thumb jump upward. The user's explicit maximum position is the
    // stronger anchor until the final projection has settled.
    if (m_l2View->viewMode() == ViewMode::Text
        && m_l2View->verticalScrollBar()->value()
               == m_l2View->verticalScrollBar()->maximum()) {
        m_l2View->syncActiveWindowToCursor(m_l2View->virtualFileSize());
        alignTextOverlayToGlobalScroll();
        return;
    }

    // 取 Scintilla 中心位置：
    //   - 有选区：选区中心字节
    //   - 无选区：(FirstVisibleLine + LinesOnScreen/2) → 转字节
    const int lineCount = m_editor->lines();
    if (lineCount <= 0) return;
    int firstLine = m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    int linesOnScreen = m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
    if (linesOnScreen <= 0) linesOnScreen = 1;
    const int mid = qBound(0, firstLine + linesOnScreen / 2,
                           qMax(0, int(m_editor->SendScintilla(
                               QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
                               static_cast<unsigned long>(lineCount))) - 1));
    // SCI_GETLINESELSTARTPOSITION / CURRENTPOS 太粗糙，直接用 mid 行首 + 当前光标列近似：
    qint64 localByte = 0;
    if (m_l2View->viewMode() == ViewMode::Hex) {
        // The byte grid and global provider share one adaptive row width. This keeps
        // navigation independent of the three display characters per byte.
        localByte = qint64(mid) * m_hexBytesPerRow;
        const qint64 globalFirst = m_bridge.windowStart / m_hexBytesPerRow
                                   + qMax(0, firstLine);
        QSignalBlocker blocker(m_l2View->verticalScrollBar());
        m_l2View->verticalScrollBar()->setValue(int(qBound<qint64>(
            qint64(m_l2View->verticalScrollBar()->minimum()), globalFirst,
            qint64(m_l2View->verticalScrollBar()->maximum()))));
    } else {
        const int x = qMax(1, m_editor->marginWidth(0) + 2);
        const int y = qMax(1, m_editor->viewport()->height() / 2);
        const long position = m_editor->SendScintilla(
            QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
            static_cast<unsigned long>(x), static_cast<long>(y));
        localByte = bridgeSourceOffsetForProjectedPosition(
            qMax<long>(0, position));
    }
    const qint64 globCenter = m_bridge.windowStart + localByte;

    if (m_l2View->viewMode() == ViewMode::Text) {
        QScrollBar *global = m_l2View->verticalScrollBar();
        int globalValue = 0;
        const LineIndex &index = m_l2View->lineIndex();
        const int topX = qMax(1, m_editor->marginWidth(0) + 2);
        const long topPosition = m_editor->SendScintilla(
            QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
            static_cast<unsigned long>(topX), 1L);
        const qint64 globTop = m_bridge.windowStart
            + bridgeSourceOffsetForProjectedPosition(qMax<long>(0, topPosition));
        const long displayedLines = m_editor->SendScintilla(
            QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
            static_cast<unsigned long>(lineCount));
        const bool lastScreen = m_bridge.windowStart + m_bridge.windowLen
                                    >= m_l2View->virtualFileSize()
            && qint64(firstLine) + qint64(linesOnScreen)
                   >= qint64(qMax<long>(0, displayedLines));
        if (lastScreen) {
            globalValue = global->maximum();
        } else if (index.isIncomplete()
            && index.realCoverageBytes() < m_l2View->virtualFileSize()) {
            const qint64 size = qMax<qint64>(1, m_l2View->virtualFileSize());
            const qint64 span = qMax<qint64>(
                1, qint64(global->maximum()) - global->minimum());
            globalValue = int(qBound<qint64>(
                qint64(global->minimum()),
                qint64(global->minimum()) + (globTop * span) / size,
                qint64(global->maximum())));
        } else {
            int line = 0;
            qint64 column = 0;
            const qint64 original = m_l2View->patches().isModified()
                ? m_l2View->patches().virtToOrig(globTop) : globTop;
            index.byteToLineCol(original, &line, &column);
            globalValue = qBound(global->minimum(), line, global->maximum());
        }
        QSignalBlocker blocker(global);
        global->setValue(globalValue);
    }

    m_l2View->syncActiveWindowToCursor(globCenter);
}
