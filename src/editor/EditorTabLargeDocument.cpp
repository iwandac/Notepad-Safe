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

} // namespace



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
        m_windowedReadOnlyProvider->setPresentation(
            mode == ViewMode::Hex ? WindowedReadOnlyProvider::Presentation::Hex
                                  : WindowedReadOnlyProvider::Presentation::Text);
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
    if (windowedReadOnlyActive())
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
            refreshDiskVersionBaseline();
            if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
                window->statusBar()->showMessage(
                    QObject::tr("源文件已在磁盘上变化；已刷新只读视图并取消旧的编辑准备"),
                    8000);
            }
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
    if (windowedReadOnlyActive() && m_windowedReadOnlyProvider)
        return m_windowedReadOnlyProvider->fileSize();
    if (m_l2Mode && m_l2View && m_l2View->isOpen())
        return m_l2View->virtualFileSize();
    // For a saved ordinary text file the user-visible size is the source file
    // on disk, not Scintilla's internal UTF-8 buffer length. Modified content
    // remains a separate editor state until the next successful save refreshes
    // this bounded disk-version snapshot.
    if (!isSecureDocument() && m_diskVersion.valid)
        return m_diskVersion.size;
    return m_editor ? static_cast<qint64>(m_editor->length()) : 0;
}

qint64 EditorTab::contentCharacterCount() const
{
    const bool hexMode = isL2Active() && viewModeL2() == ViewMode::Hex;
    if (hexMode)
        return contentSizeBytes();
    if (windowedReadOnlyActive() && m_windowedReadOnlyProvider)
        return m_windowedReadOnlyProvider->characterCount();

    if (m_characterCountRevision == m_documentState.revision
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
    m_characterCountRevision = m_documentState.revision;
    m_cachedCharacterCountHex = hexMode;
    m_cachedCharacterCount = count;
    return count;
}

bool EditorTab::ensureWindowedSearchSource(QString *error)
{
    // Search-source availability is a document capability, not a presentation
    // detail.  QStackedWidget::currentWidget() is appropriate for routing
    // visible-editor commands, but it is not a reliable lifetime marker for
    // the already-open production provider (dialogs and transient view changes
    // can make the provider non-current without closing its source file).
    const bool hasWindowedSource = m_l2Mode && m_windowedReadOnlyProvider
        && m_windowedReadOnlyProvider->fileSize() > 0
        && !m_documentState.location.isEmpty();
    if (!hasWindowedSource) {
        const bool hasMappedSource = m_l2View && m_l2View->isOpen();
        if (!hasMappedSource && error) {
            *error = QObject::tr(
                "当前大文件搜索源不可用（文档模式=%1，窗口源=%2，文件大小=%3，路径=%4）。")
                .arg(m_l2Mode ? QObject::tr("大文件") : QObject::tr("普通文件"))
                .arg(m_windowedReadOnlyProvider ? QObject::tr("存在")
                                                : QObject::tr("不存在"))
                .arg(m_windowedReadOnlyProvider
                         ? m_windowedReadOnlyProvider->fileSize() : -1)
                .arg(m_documentState.location.isEmpty() ? QObject::tr("为空")
                                          : QObject::tr("存在"));
        }
        return hasMappedSource;
    }
    if (m_l2View && (m_l2View->isOpen() || m_l2View->mmapSize() > 0))
        return true;
    if (m_documentState.location.isEmpty()) {
        if (error)
            *error = QObject::tr("当前大文件没有可搜索的磁盘源。");
        return false;
    }
    VirtualFileView *view = ensureL2View();
    view->setFullLineIndexBuildEnabled(false);
    view->setViewMode(m_windowedReadOnlyProvider->presentation()
                          == WindowedReadOnlyProvider::Presentation::Hex
                      ? ViewMode::Hex : ViewMode::Text);
    QString openError;
    if (!view->openFile(m_documentState.location, &openError)) {
        if (error)
            *error = openError;
        return false;
    }
    return true;
}

void EditorTab::ensureHexAsciiPreview(QWidget *parent)
{
    if (!parent)
        return;
    if (!m_hexAsciiPreview) {
        m_hexAsciiPreview = new QPlainTextEdit(parent);
        m_hexAsciiPreview->setObjectName(QStringLiteral("HexAsciiPreview"));
        m_hexAsciiPreview->setAccessibleName(QStringLiteral("编码字符预览"));
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
        m_hexAsciiPreview->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { border: 0; border-left: 1px solid #c8c8c8;"
            " background: #fafafa; padding-left: 8px; }"));
        m_hexAsciiPreview->installEventFilter(this);
        m_hexAsciiPreview->viewport()->installEventFilter(this);
        emit externalDropSurfaceCreated(m_hexAsciiPreview);
        emit externalDropSurfaceCreated(m_hexAsciiPreview->viewport());
        connect(m_hexAsciiPreview, &QPlainTextEdit::cursorPositionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
        connect(m_hexAsciiPreview, &QPlainTextEdit::selectionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
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
    m_bridge.windowStart = s;
    m_bridge.windowLen   = e - s;
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
    ++m_documentState.revision;
    m_l1SearchCacheValid = false;
    m_l1SearchCache.clear();
    m_lastL1MatchIndex = -1;
    m_l1NavigationHitValid = false;
    m_l2NavigationHitValid = false;
    m_l2SearchCacheValid = false;
    clearSearchHighlights();
    if (m_l1FindAllTaskId > 0) {
        AsyncTaskManager::instance().cancel(m_l1FindAllTaskId);
        m_l1FindAllTaskId = 0;
    }
    cancelPendingL2SearchForContentChange();
    emit contentChanged(m_documentState.revision);
}

void EditorTab::cancelPendingL2SearchForContentChange()
{
    if (!m_pendingL2Search.active || !m_l2View)
        return;
    if (LargeFileSearch *search = m_l2View->searchEngine()) {
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
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, indicator, 0x00B7FF);
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

bool EditorTab::eventFilter(QObject* obj, QEvent* ev)
{
    const bool hexOverlayObject = m_l2Mode && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex
        && (obj == m_editor || obj == m_editor->viewport()
            || obj == m_hexAsciiPreview
            || (m_hexAsciiPreview && obj == m_hexAsciiPreview->viewport()));
    const bool textOverlayObject = m_l2Mode && m_l2View
        && m_l2View->viewMode() == ViewMode::Text
        && (obj == m_editor || obj == m_editor->viewport());
    if (textOverlayObject && ev->type() == QEvent::Wheel) {
        m_bridge.viewportNavigationPending = true;
    } else if (textOverlayObject && ev->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(ev);
        switch (key->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            m_bridge.viewportNavigationPending = true;
            break;
        default:
            break;
        }
    }
    if (hexOverlayObject && ev->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(ev)->button() == Qt::LeftButton) {
        m_hexMouseSelecting = true;
    }
    if (hexOverlayObject && ev->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(ev);
        if (wheel->modifiers().testFlag(Qt::ControlModifier)
            || wheel->modifiers().testFlag(Qt::ShiftModifier)
            || qAbs(wheel->angleDelta().x()) > qAbs(wheel->angleDelta().y())) {
            return QWidget::eventFilter(obj, ev);
        }
        int lineDelta = 0;
        if (!wheel->pixelDelta().isNull()) {
            const int lineHeight = qMax(1, m_editor->fontMetrics().lineSpacing());
            m_hexWheelPixelRemainder += wheel->pixelDelta().y();
            lineDelta = -(m_hexWheelPixelRemainder / lineHeight);
            m_hexWheelPixelRemainder %= lineHeight;
        } else {
            m_hexWheelAngleRemainder += wheel->angleDelta().y();
            const int steps = m_hexWheelAngleRemainder / 120;
            m_hexWheelAngleRemainder %= 120;
            lineDelta = -steps * qMax(1, QApplication::wheelScrollLines());
        }
        if (lineDelta != 0) {
            QScrollBar *global = m_l2View->verticalScrollBar();
            global->setValue(qBound(global->minimum(),
                                    global->value() + lineDelta,
                                    global->maximum()));
            // Do not wait for the legacy slider debounce when the requested
            // global row approaches the edge of the 256 KiB projection. Move
            // the active byte window during the wheel gesture so the next row
            // is painted immediately and both Hex/ASCII panes stay continuous.
            const int rowsOnScreen = qMax(1, int(m_editor->SendScintilla(
                QsciScintillaBase::SCI_LINESONSCREEN)));
            const qint64 centerRow = qint64(global->value()) + rowsOnScreen / 2;
            const qint64 targetByte = qBound<qint64>(
                0, centerRow * m_hexBytesPerRow,
                qMax<qint64>(0, m_l2View->virtualFileSize() - 1));
            m_l2View->syncActiveWindowToCursor(targetByte);
            alignHexOverlayToGlobalScroll();
        }
        wheel->accept();
        return true;
    }
    if (hexOverlayObject && ev->type() == QEvent::InputMethod) {
        const bool asciiSurface = m_hexAsciiPreview
            && (obj == m_hexAsciiPreview
                || obj == m_hexAsciiPreview->viewport());
        return handleHexInputMethodEvent(
            static_cast<QInputMethodEvent *>(ev), asciiSurface);
    }
    if (m_hexAsciiPreview
        && (obj == m_hexAsciiPreview || obj == m_hexAsciiPreview->viewport())
        && ev->type() == QEvent::KeyPress && isL2Mode() && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex) {
        if (handleHexAsciiKeyPress(static_cast<QKeyEvent *>(ev)))
            return true;
    }
    if ((obj == m_editor || obj == m_editor->viewport())
        && ev->type() == QEvent::KeyPress
        && isL2Mode() && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex) {
        auto *keyEvent = static_cast<QKeyEvent *>(ev);
        if (handleHexKeyPress(keyEvent))
            return true;
    }
    if (hexOverlayObject && ev->type() == QEvent::MouseButtonRelease) {
        // Do not rewrite Scintilla's selection while its mouse-drag state
        // machine still owns the anchor. Normalize once the release has been
        // fully processed; doing it during MouseMove can repeatedly restart
        // the native drag and stall the GUI event loop.
        const bool asciiSurface = m_hexAsciiPreview
            && (obj == m_hexAsciiPreview
                || obj == m_hexAsciiPreview->viewport());
        if (asciiSurface) {
            // QPlainTextEdit has already updated its cursor during press/move.
            // Publish that byte position before the release returns so an IME
            // commit or key event queued immediately behind the click cannot
            // observe the previous model/status position. This path only
            // changes the opposite Hex pane, so it does not interfere with the
            // ASCII widget's native release/selection state machine.
            m_hexMouseSelecting = false;
            syncHexSelectionFromAscii();
            return QWidget::eventFilter(obj, ev);
        }
        QTimer::singleShot(0, this, [this, asciiSurface]() {
            m_hexMouseSelecting = false;
            if (asciiSurface)
                syncHexSelectionFromAscii();
            else
                syncAsciiSelectionFromHex();
        });
    }

    // 监听 VFV viewport 的 Resize：Scintilla overlay 必须同步跟着 resize，
    // 否则 reparent 后 vp->size() 为 0（stack 刚切，布局未完成）→ Scintilla 0x0 不显示。
    if (ev->type() == QEvent::Resize && m_l2Mode && m_l2View) {
        QWidget* vp = m_l2View->viewport();
        if (obj == vp && m_editor) {
            adaptHexBytesPerRow(true);
            layoutL2Overlay();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

bool EditorTab::asciiSelectionByteRange(qint64 *start, qint64 *end) const
{
    if (!start || !end || !m_hexAsciiPreview || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex || m_bridge.windowStart < 0) {
        return false;
    }
    const QTextCursor cursor = m_hexAsciiPreview->textCursor();
    const auto mapPosition = [this](int position) {
        const QTextBlock block = m_hexAsciiPreview->document()->findBlock(position);
        if (!block.isValid())
            return m_bridge.windowLen;
        const qint64 column = qBound<qint64>(
            0LL, qint64(position - block.position()),
            qint64(qMin(m_hexBytesPerRow, block.text().size())));
        return qMin(m_bridge.windowLen,
                    qint64(block.blockNumber()) * m_hexBytesPerRow + column);
    };
    qint64 localStart = mapPosition(cursor.selectionStart());
    qint64 localEnd = mapPosition(cursor.selectionEnd());
    if (localEnd < localStart)
        std::swap(localStart, localEnd);
    *start = m_bridge.windowStart + localStart;
    *end = m_bridge.windowStart + localEnd;
    return true;
}

bool EditorTab::handleHexInsertModeKey(QKeyEvent *event)
{
    if (!event || event->key() != Qt::Key_Insert
        || event->modifiers() != Qt::NoModifier || isReadOnlyByPolicy()) {
        return false;
    }
    toggleHexOverwriteMode();
    return true;
}

void EditorTab::toggleHexOverwriteMode()
{
    if (!m_l2View || m_l2View->viewMode() != ViewMode::Hex
        || isReadOnlyByPolicy()) {
        return;
    }
    m_hexOverwriteMode = !m_hexOverwriteMode;
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
        window->statusBar()->showMessage(
            m_hexOverwriteMode ? QObject::tr("Hex 覆盖模式 (OVR)")
                               : QObject::tr("Hex 插入模式 (INS)"),
            1800);
    }
}

void EditorTab::setAsciiCaret(qint64 globalByte)
{
    if (!m_hexAsciiPreview || !m_l2View || m_bridge.windowStart < 0)
        return;
    globalByte = qBound<qint64>(0, globalByte, m_l2View->virtualFileSize());
    m_l2View->syncActiveWindowToCursor(globalByte);
    // The byte provider is the source of truth for navigation and subsequent
    // edits. Rebuilding a decoded Hex character row can temporarily mirror
    // the old Scintilla caret back into it, so restore the requested collapsed
    // byte selection before positioning either visual caret.
    m_l2View->selectBytes(globalByte, globalByte);
    const qint64 local = qBound<qint64>(0, globalByte - m_bridge.windowStart,
                                        m_bridge.windowLen);
    const int line = int(local / m_hexBytesPerRow);
    const int column = int(local % m_hexBytesPerRow);
    const QTextBlock block = m_hexAsciiPreview->document()->findBlockByNumber(line);
    if (!block.isValid())
        return;
    QTextCursor cursor(m_hexAsciiPreview->document());
    cursor.setPosition(qMin(block.position() + column,
                            block.position() + block.text().size()));
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setTextCursor(cursor);
    m_adjustingAsciiCaret = false;
    setHexCaret(globalByte, 0);
    m_lastL2Byte = globalByte;
}

void EditorTab::syncAsciiSelectionFromHex()
{
    if (m_adjustingHexCaret || m_adjustingAsciiCaret || m_bridge.applying
        || !m_hexAsciiPreview || !m_hexAsciiPreview->isVisible()) {
        return;
    }
    qint64 start = 0;
    qint64 end = 0;
    int nibble = 0;
    if (!hexSelectionByteRange(&start, &end, &nibble))
        return;

    // Scintilla still performs mouse hit testing against the textual
    // projection ("00 01 ...").  A click can therefore leave its visible
    // caret on a separator space even though the source model has already
    // resolved that position to the following byte.  Keep the byte model and
    // the visible caret identical after every native mouse/cursor update.
    if (end == start && !m_hexMouseSelecting) {
        const qint64 local = qBound<qint64>(
            0, start - m_bridge.windowStart, m_bridge.windowLen);
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3
                               + qBound(0, nibble, 1));
        const long expected = m_editor->positionFromLineIndex(line, column);
        const long actual = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        if (expected >= 0 && actual != expected)
            setHexCaret(start, nibble);
    } else if (!m_hexMouseSelecting) {
        const auto boundaryPosition = [this](qint64 globalByte) -> long {
            const qint64 local = qBound<qint64>(
                0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
            if (local >= m_bridge.windowLen)
                return m_editor->length();
            const int line = int(qMin<qint64>(
                local / m_hexBytesPerRow, INT_MAX));
            const int column = int((local % m_hexBytesPerRow) * 3);
            return m_editor->positionFromLineIndex(line, column);
        };
        const long actualAnchor = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETANCHOR);
        const long actualCaret = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        const bool forward = actualCaret >= actualAnchor;
        const long expectedAnchor = boundaryPosition(forward ? start : end);
        const long expectedCaret = boundaryPosition(forward ? end : start);
        // Mouse drag and native Shift movement operate on characters. Snap
        // both ends to source-byte boundaries so the highlighted Hex range,
        // ASCII mirror and the bytes affected by Cut/Delete cannot disagree.
        if (expectedAnchor >= 0 && expectedCaret >= 0
            && (actualAnchor != expectedAnchor || actualCaret != expectedCaret)) {
            m_adjustingHexCaret = true;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                    expectedAnchor, expectedCaret);
            m_adjustingHexCaret = false;
        }
    }

    const auto documentPosition = [this](qint64 globalByte) -> int {
        const qint64 local = qBound<qint64>(
            0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
        if (local >= m_bridge.windowLen)
            return qMax(0, m_hexAsciiPreview->document()->characterCount() - 1);
        const QTextBlock block = m_hexAsciiPreview->document()->findBlockByNumber(
            int(local / m_hexBytesPerRow));
        if (!block.isValid())
            return qMax(0, m_hexAsciiPreview->document()->characterCount() - 1);
        return qMin(block.position() + int(local % m_hexBytesPerRow),
                    block.position() + int(block.text().size()));
    };

    // Do not feed an unselected Hex caret into QPlainTextEdit on every
    // SCN_UPDATEUI. setTextCursor() calls ensureCursorVisible(), which used to
    // pull the ASCII pane back to the caret after each wheel gesture. A real
    // byte selection is still mirrored below; otherwise scrolling owns the
    // viewport and both panes remain locked to the same first row.
    if (end == start && !m_hexAsciiPreview->textCursor().hasSelection()) {
        // Mirror the logical caret as well as the selection. Preserve both
        // scroll offsets because QPlainTextEdit::setTextCursor() calls
        // ensureCursorVisible(); without restoration, a Hex-wheel gesture can
        // drag the synchronized ASCII pane back to an off-screen caret.
        const int verticalValue = m_hexAsciiPreview->verticalScrollBar()->value();
        const int horizontalValue = m_hexAsciiPreview->horizontalScrollBar()->value();
        QTextCursor mirrored(m_hexAsciiPreview->document());
        mirrored.setPosition(documentPosition(start));
        m_adjustingAsciiCaret = true;
        m_hexAsciiPreview->setTextCursor(mirrored);
        m_adjustingAsciiCaret = false;
        m_hexAsciiPreview->verticalScrollBar()->setValue(verticalValue);
        m_hexAsciiPreview->horizontalScrollBar()->setValue(horizontalValue);
        m_hexAsciiPreview->setExtraSelections({});
        const int first = int(m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        m_hexAsciiPreview->verticalScrollBar()->setValue(first);
        m_l2View->selectBytes(start, end);
        m_lastL2Byte = start;
        return;
    }

    QTextCursor cursor(m_hexAsciiPreview->document());
    cursor.setPosition(documentPosition(start));
    if (end > start)
        cursor.setPosition(documentPosition(end), QTextCursor::KeepAnchor);
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setTextCursor(cursor);
    m_adjustingAsciiCaret = false;
    QList<QTextEdit::ExtraSelection> mirroredSelections;
    if (end > start) {
        QTextEdit::ExtraSelection mirrored;
        mirrored.cursor = cursor;
        mirrored.format.setBackground(QColor(51, 153, 255));
        mirrored.format.setForeground(Qt::white);
        mirroredSelections.append(mirrored);
    }
    m_hexAsciiPreview->setExtraSelections(mirroredSelections);
    const int first = int(m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    m_hexAsciiPreview->verticalScrollBar()->setValue(first);
    m_l2View->selectBytes(start, end);
    m_lastL2Byte = end > start ? end : start;
}

void EditorTab::syncHexSelectionFromAscii()
{
    if (m_adjustingAsciiCaret || m_adjustingHexCaret || m_bridge.applying
        || m_hexMouseSelecting || !m_hexAsciiPreview) {
        return;
    }
    qint64 start = 0;
    qint64 end = 0;
    if (!asciiSelectionByteRange(&start, &end))
        return;

    const auto overlayPosition = [this](qint64 globalByte) -> long {
        const qint64 local = qBound<qint64>(
            0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
        if (local >= m_bridge.windowLen)
            return m_editor->length();
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3);
        return m_editor->positionFromLineIndex(line, column);
    };

    const long first = overlayPosition(start);
    const long last = overlayPosition(end);
    if (first < 0 || last < 0)
        return;
    m_adjustingHexCaret = true;
    if (end > start) {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, first, last);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, first);
    }
    m_adjustingHexCaret = false;
    m_l2View->selectBytes(start, end);
    m_lastL2Line = int(qMin<qint64>(start / m_hexBytesPerRow, INT_MAX));
    m_lastL2Col = int(start % m_hexBytesPerRow);
    m_lastL2Byte = end > start ? end : start;
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
}

void EditorTab::refreshHexAsciiProjectionFromEditor()
{
    if (!m_editor || !m_hexAsciiPreview || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex) {
        return;
    }
    QByteArray previewBytes;
    QString previewError;
    if (!parseHexBytes(m_editor->text(), &previewBytes, &previewError))
        return;
    QString ascii = decodeByteGridText(previewBytes,
                                       m_l2View->displayEncoding(),
                                       m_hexBytesPerRow);
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setPlainText(ascii);
    alignPlainTextRowsToScintilla(m_hexAsciiPreview, m_editor);
    m_adjustingAsciiCaret = false;
    if (isSecureBinaryDocument()) {
        if (!previewBytes.isEmpty()) previewBytes.fill(0);
        if (!ascii.isEmpty()) ascii.fill(QChar(0));
    }
    syncAsciiSelectionFromHex();
}

bool EditorTab::updateFixedHexProjection(qint64 globalStart, qint32 oldLength,
                                         const QByteArray &replacement)
{
    if (!m_l2View || !m_editor || !m_hexAsciiPreview
        || m_l2View->viewMode() != ViewMode::Hex || oldLength <= 0
        || oldLength != replacement.size() || replacement.size() > 4096
        || globalStart < m_bridge.windowStart
        || globalStart + oldLength
               > m_bridge.windowStart + m_bridge.baseline.size()) {
        return false;
    }

    const qint64 localStart = globalStart - m_bridge.windowStart;
    const bool previousApplying = m_bridge.applying;
    const int asciiVertical = m_hexAsciiPreview->verticalScrollBar()->value();
    const int asciiHorizontal = m_hexAsciiPreview->horizontalScrollBar()->value();
    m_bridge.applying = true;
    m_adjustingAsciiCaret = true;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    static constexpr char digits[] = "0123456789ABCDEF";
    for (qsizetype index = 0; index < replacement.size(); ++index) {
        const qint64 localByte = localStart + index;
        const int line = int(localByte / m_hexBytesPerRow);
        const int byteColumn = int(localByte % m_hexBytesPerRow);
        const long hexPosition = m_editor->positionFromLineIndex(
            line, byteColumn * 3);
        if (hexPosition < 0) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
            m_adjustingAsciiCaret = false;
            m_bridge.applying = previousApplying;
            return false;
        }
        const uchar value = uchar(replacement.at(index));
        const char hex[2] = {digits[value >> 4], digits[value & 0x0f]};
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETRANGE,
                                hexPosition, hexPosition + 2);
        m_editor->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                static_cast<uintptr_t>(2),
                                static_cast<const char *>(hex));

    }
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_bridge.baseline.replace(int(localStart), replacement.size(), replacement);

    // Multibyte decoding means one edited byte can change the character that
    // begins a few bytes earlier. Rebuild only the affected rows plus one
    // bounded neighbor on each side instead of decoding the whole active
    // window after every Hex keystroke.
    const qint64 totalRows = (m_bridge.baseline.size() + m_hexBytesPerRow - 1)
        / m_hexBytesPerRow;
    const qint64 firstRow = qMax<qint64>(
        0, localStart / m_hexBytesPerRow - 1);
    const qint64 lastRowExclusive = qMin<qint64>(
        totalRows,
        (localStart + replacement.size() + m_hexBytesPerRow - 1)
                / m_hexBytesPerRow + 1);
    const qint64 segmentStart = firstRow * m_hexBytesPerRow;
    const qint64 segmentEnd = qMin<qint64>(
        m_bridge.baseline.size(), lastRowExclusive * m_hexBytesPerRow);
    const QString characterRows = decodeByteGridText(
        m_bridge.baseline.mid(segmentStart, segmentEnd - segmentStart),
        m_l2View->displayEncoding(), m_hexBytesPerRow);
    const QTextBlock firstBlock =
        m_hexAsciiPreview->document()->findBlockByNumber(int(firstRow));
    const QTextBlock afterBlock =
        m_hexAsciiPreview->document()->findBlockByNumber(int(lastRowExclusive));
    if (firstBlock.isValid()) {
        QTextCursor cursor(m_hexAsciiPreview->document());
        cursor.setPosition(firstBlock.position());
        const int endPosition = afterBlock.isValid()
            ? afterBlock.position()
            : qMax(0, m_hexAsciiPreview->document()->characterCount() - 1);
        cursor.setPosition(endPosition, QTextCursor::KeepAnchor);
        cursor.insertText(characterRows
            + (afterBlock.isValid() ? QString(QLatin1Char('\n')) : QString()));
    }
    m_hexAsciiPreview->verticalScrollBar()->setValue(asciiVertical);
    m_hexAsciiPreview->horizontalScrollBar()->setValue(asciiHorizontal);
    m_adjustingAsciiCaret = false;
    m_bridge.applying = previousApplying;
    syncAsciiSelectionFromHex();
    applyL2SearchHighlightsToOverlay();
    return true;
}

bool EditorTab::replaceHexAsciiBytes(qint64 from, qint64 to, QByteArray bytes,
                                     QString *error)
{
    QString flushError;
    if (!bridgeFlushOverlayToPatches(&flushError)) {
        if (error) *error = flushError;
        if (isSecureBinaryDocument() && !bytes.isEmpty())
            bytes.fill(0);
        return false;
    }
    const qint32 oldLength = qint32(qMin<qint64>(qMax<qint64>(0, to - from),
                                                 INT_MAX));
    QString replaceError;
    if (!m_l2View->replaceBytes(from, oldLength, bytes, &replaceError)) {
        if (error) *error = replaceError;
        if (isSecureBinaryDocument() && !bytes.isEmpty())
            bytes.fill(0);
        return false;
    }

    const qint64 caret = from + bytes.size();
    const bool updatedInPlace = updateFixedHexProjection(from, oldLength, bytes);
    const qint64 previousWindowStart = m_l2View->activeWindowByteStart();
    const qint64 previousWindowEnd = m_l2View->activeWindowByteEnd();
    m_l2View->syncActiveWindowToCursor(caret);
    if (!updatedInPlace
        && previousWindowStart == m_l2View->activeWindowByteStart()
        && previousWindowEnd == m_l2View->activeWindowByteEnd()) {
        bridgePopulateScintilla(previousWindowStart, previousWindowEnd);
    }
    setAsciiCaret(caret);
    m_hexAsciiPreview->setFocus(Qt::OtherFocusReason);
    if (isSecureBinaryDocument() && !bytes.isEmpty())
        bytes.fill(0);
    return true;
}

bool EditorTab::handleHexAsciiKeyPress(QKeyEvent *event)
{
    if (!event || !m_l2View || isReadOnlyByPolicy())
        return false;
    if (handleHexInsertModeKey(event))
        return true;
    qint64 start = 0;
    qint64 end = 0;
    if (!asciiSelectionByteRange(&start, &end))
        return false;
    const bool selected = end > start;
    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };
    const auto replace = [this, &showError](qint64 from, qint64 to,
                                            QByteArray bytes) {
        QString error;
        if (!replaceHexAsciiBytes(from, to, std::move(bytes), &error)) {
            showError(error);
        }
    };

    if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
        if (!selected)
            return true;
        QByteArray bytes = m_l2View->readByteRange(start, end);
        ByteClipboard::setCharacterSelection(bytes, m_l2View->displayEncoding());
        if (isSecureBinaryDocument() && !bytes.isEmpty())
            bytes.fill(0);
        if (event->matches(QKeySequence::Cut))
            replace(start, end, {});
        return true;
    }
    if (event->matches(QKeySequence::Paste)) {
        QByteArray pasted;
        if (!ByteClipboard::rawClipboardBytes(&pasted)) {
            bool representable = true;
            pasted = encodeTextFragment(QGuiApplication::clipboard()->text(),
                                        m_l2View->displayEncoding(),
                                        &representable);
            if (!representable) {
                showError(QObject::tr("剪贴板文本无法用当前字符栏编码无损表示。"));
                return true;
            }
        }
        qint64 overwriteEnd = end;
        if (!selected && m_hexOverwriteMode) {
            const qint64 available = m_l2View->virtualFileSize() - start;
            if (available <= 0)
                return true;
            if (pasted.size() > available)
                pasted.truncate(int(available));
            overwriteEnd = start + pasted.size();
        }
        replace(start, overwriteEnd, std::move(pasted));
        return true;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (!selected) {
            if (event->key() == Qt::Key_Backspace) {
                // At byte zero Backspace is a boundary no-op.  Falling
                // through with start==0 used to delete the first byte, making
                // Backspace indistinguishable from Delete at the file start.
                if (start <= 0)
                    return true;
                --start;
            } else if (start >= m_l2View->virtualFileSize()) {
                return true;
            }
            end = qMin(m_l2View->virtualFileSize(), start + 1);
        }
        if (end > start)
            replace(start, end, {});
        return true;
    }
    const QString typed = event->text();
    if (!typed.isEmpty()
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        const bool hasControl = std::any_of(
            typed.cbegin(), typed.cend(), [](QChar ch) {
                return ch.category() == QChar::Other_Control;
            });
        bool representable = true;
        QByteArray encoded = encodeTextFragment(
            typed, m_l2View->displayEncoding(), &representable);
        if (hasControl || !representable || encoded.isEmpty()) {
            showError(QObject::tr("输入字符无法用当前字符栏编码无损表示。"));
            return true;
        }
        if (!selected && m_hexOverwriteMode
            && start >= m_l2View->virtualFileSize()) {

            return true;
        }

        const qint64 overwriteEnd = selected || !m_hexOverwriteMode
            ? end : qMin(m_l2View->virtualFileSize(), start + encoded.size());
        replace(start, overwriteEnd, std::move(encoded));
        return true;
    }
    return false;
}

bool EditorTab::handleHexInputMethodEvent(QInputMethodEvent *event,
                                          bool asciiSurface)
{
    if (!event || !m_l2View || m_l2View->viewMode() != ViewMode::Hex)
        return false;

    // Preedit text is presentation state owned by the IME. Showing it inside
    // "00 01 ..." would temporarily (and with some plugins permanently)
    // corrupt the fixed Hex grid, so byte surfaces never expose composition.
    // Only a fully validated commit may enter the source-byte command path.
    event->accept();
    const QString committed = event->commitString();
    if (committed.isEmpty() || isReadOnlyByPolicy())
        return true;

    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };

    // Validate the complete commit before changing one byte. The character
    // pane uses its selected encoding; the Hex pane still accepts nibbles only.
    for (const QChar ch : committed) {
        if (!asciiSurface && hexNibble(ch) < 0) {
            showError(QObject::tr(
                "十六进制视图只接受 0–9、A–F；输入法候选文本未写入文件。"));
            return true;
        }
    }

    if (asciiSurface) {
        qint64 start = 0;
        qint64 end = 0;
        if (!asciiSelectionByteRange(&start, &end)) {
            showError(QObject::tr("无法把输入法提交定位到源字节。"));
            return true;
        }
        const bool hasControl = std::any_of(
            committed.cbegin(), committed.cend(), [](QChar ch) {
                return ch.category() == QChar::Other_Control;
            });
        bool representable = true;
        QByteArray replacement = encodeTextFragment(
            committed, m_l2View->displayEncoding(), &representable);
        if (hasControl || !representable || replacement.isEmpty()) {
            showError(QObject::tr("输入法文本无法用当前字符栏编码无损表示。"));
            return true;
        }
        if (start == end && m_hexOverwriteMode) {
            const qint64 available = m_l2View->virtualFileSize() - start;
            if (available <= 0) {
                if (isSecureBinaryDocument() && !replacement.isEmpty())
                    replacement.fill(0);
                return true;
            }
            if (replacement.size() > available)
                replacement.truncate(int(available));
            end = start + replacement.size();
        }
        QString replaceError;
        if (!replaceHexAsciiBytes(start, end, std::move(replacement),
                                  &replaceError)) {
            showError(replaceError);
        }
        return true;
    }

    if (!asciiSurface) {
        // An IME commit is one user transaction. Replaying its characters as
        // independent nibble key presses is incorrect in INS mode because a
        // high-nibble insertion rebuilds the projection and moves the caret
        // between characters. Build one byte-level replacement instead.
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }

        qint64 start = 0;
        qint64 end = 0;
        int nibble = 0;
        if (!hexSelectionByteRange(&start, &end, &nibble)) {
            showError(QObject::tr("无法把输入法提交定位到源字节。"));
            return true;
        }
        const bool selected = end > start;
        QByteArray replacement;
        qint32 oldLength = 0;
        qint64 caretByte = start;
        int caretNibble = 0;

        const auto appendNibblePairs = [&committed](QByteArray *target,
                                                     int firstNibble) {
            for (int i = firstNibble; i < committed.size(); i += 2) {
                const int high = hexNibble(committed.at(i));
                const int low = i + 1 < committed.size()
                    ? hexNibble(committed.at(i + 1)) : 0;
                target->append(char((high << 4) | low));
            }
        };

        if (selected) {
            appendNibblePairs(&replacement, 0);
            oldLength = qint32(qMin<qint64>(end - start, INT_MAX));
            if (committed.size() % 2 == 0) {
                caretByte = start + replacement.size();
            } else {
                caretByte = start + qMax(0, replacement.size() - 1);
                caretNibble = 1;
            }
        } else if (!m_hexOverwriteMode) {
            if (nibble == 0) {
                appendNibblePairs(&replacement, 0);
                caretByte = start + committed.size() / 2;
                caretNibble = committed.size() % 2;
            } else {
                if (start >= m_l2View->virtualFileSize())
                    return true;
                QByteArray current = m_l2View->readByteRange(start, start + 1);
                if (current.isEmpty())
                    return true;
                current[0] = char((quint8(current.at(0)) & 0xf0)
                                  | hexNibble(committed.at(0)));
                replacement.append(current.at(0));
                appendNibblePairs(&replacement, 1);
                oldLength = 1;
                const int remaining = committed.size() - 1;
                caretByte = start + 1 + remaining / 2;
                caretNibble = remaining % 2;
                if (isSecureBinaryDocument())
                    current.fill(0);
            }
        } else {
            const qint64 availableNibbles = qMax<qint64>(
                0, (m_l2View->virtualFileSize() - start) * 2 - nibble);
            const int applied = int(qMin<qint64>(
                committed.size(), qMin<qint64>(availableNibbles, INT_MAX)));
            if (applied <= 0)
                return true;
            const int bytesTouched = (nibble + applied + 1) / 2;
            replacement = m_l2View->readByteRange(
                start, start + bytesTouched);
            for (int i = 0; i < applied; ++i) {
                const int destinationNibble = nibble + i;
                const int byteIndex = destinationNibble / 2;
                const int value = hexNibble(committed.at(i));
                const quint8 old = quint8(replacement.at(byteIndex));
                replacement[byteIndex] = destinationNibble % 2 == 0
                    ? char((value << 4) | (old & 0x0f))
                    : char((old & 0xf0) | value);
            }
            oldLength = qint32(replacement.size());
            const int next = nibble + applied;
            caretByte = start + next / 2;
            caretNibble = next % 2;
        }

        QString replaceError;
        if (!m_l2View->replaceBytes(start, oldLength, replacement,
                                    &replaceError)) {
            showError(replaceError);
            if (isSecureBinaryDocument() && !replacement.isEmpty())
                replacement.fill(0);
            return true;
        }
        if (isSecureBinaryDocument() && !replacement.isEmpty())
            replacement.fill(0);
        m_l2View->syncActiveWindowToCursor(caretByte);
        bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                m_l2View->activeWindowByteEnd());
        setHexCaret(caretByte, caretNibble);
        return true;
    }

    for (const QChar ch : committed) {
        const int key = int(ch.toUpper().unicode());
        QKeyEvent typed(QEvent::KeyPress, key, Qt::NoModifier, QString(ch));
        const bool handled = handleHexKeyPress(&typed);
        if (!handled) {
            showError(QObject::tr("输入法提交未能转换为原始字节，文件保持不变。"));
            break;
        }
    }
    return true;
}

bool EditorTab::hexSelectionByteRange(qint64 *start, qint64 *end,
                                      int *nibble) const
{
    if (!start || !end || !m_editor || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex
        || m_bridge.windowStart < 0) {
        return false;
    }

    long selectionStart = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONSTART);
    long selectionEnd = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONEND);
    if (selectionEnd < selectionStart)
        std::swap(selectionStart, selectionEnd);
    const bool selected = selectionEnd > selectionStart;

    const auto mapPosition = [this](long position, bool endBoundary,
                                    int *outNibble) -> qint64 {
        int line = 0;
        int column = 0;
        m_editor->lineIndexFromPosition(position, &line, &column);
        line = qMax(0, line);
        column = qMax(0, column);
        qint64 byteInLine = endBoundary ? (qint64(column) + 2) / 3
                                        : qint64(column) / 3;
        int localNibble = column % 3;
        if (!endBoundary && localNibble == 2) {
            ++byteInLine;
            localNibble = 0;
        }
        byteInLine = qBound<qint64>(qint64(0), byteInLine,
                                    qint64(m_hexBytesPerRow));
        if (outNibble)
            *outNibble = qBound(0, localNibble, 1);
        return qint64(line) * m_hexBytesPerRow + byteInLine;
    };

    int localNibble = 0;
    qint64 localStart = mapPosition(selectionStart, false, &localNibble);
    qint64 localEnd = selected
        ? mapPosition(selectionEnd, true, nullptr)
        : localStart;
    const qint64 windowEnd = m_bridge.windowStart + m_bridge.windowLen;
    *start = qBound(m_bridge.windowStart,
                    m_bridge.windowStart + localStart, windowEnd);
    *end = qBound(*start, m_bridge.windowStart + localEnd, windowEnd);
    if (nibble)
        *nibble = localNibble;
    return true;
}

void EditorTab::setHexCaret(qint64 globalByte, int nibble)
{
    if (!m_editor || !m_l2View || m_bridge.windowStart < 0)
        return;
    globalByte = qBound<qint64>(0, globalByte, m_l2View->virtualFileSize());
    m_l2View->syncActiveWindowToCursor(globalByte);
    if (globalByte < m_bridge.windowStart
        || globalByte > m_bridge.windowStart + m_bridge.windowLen) {
        return;
    }

    qint64 local = globalByte - m_bridge.windowStart;
    const qint64 size = m_l2View->virtualFileSize();
    if (globalByte == size) {
        // EOF is a real insertion boundary, not an alias for the low nibble of
        // the last byte.  Keeping it reachable makes Ctrl+End followed by Hex
        // input behave like mature binary editors in INS mode.
        m_adjustingHexCaret = true;
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                m_editor->length());
        m_adjustingHexCaret = false;
        m_lastL2Line = int(qMin<qint64>(globalByte / m_hexBytesPerRow, INT_MAX));
        m_lastL2Col = int(globalByte % m_hexBytesPerRow);
        m_lastL2Byte = globalByte;
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
        return;
    }
    const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
    const int column = int((local % m_hexBytesPerRow) * 3
                           + qBound(0, nibble, 1));
    const long position = m_editor->positionFromLineIndex(line, column);
    if (position >= 0) {
        m_adjustingHexCaret = true;
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, position);
        m_adjustingHexCaret = false;
        m_lastL2Line = int(qMin<qint64>(globalByte / m_hexBytesPerRow, INT_MAX));
        m_lastL2Col = int(globalByte % m_hexBytesPerRow);
        m_lastL2Byte = globalByte;
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    }
}

bool EditorTab::handleHexKeyPress(QKeyEvent *event)
{
    if (!event || !m_l2View)
        return false;
    if (handleHexInsertModeKey(event))
        return true;

    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };
    const auto refresh = [this](qint64 caretByte, int caretNibble) {
        m_l2View->syncActiveWindowToCursor(caretByte);
        bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                m_l2View->activeWindowByteEnd());
        setHexCaret(caretByte, caretNibble);
    };

    qint64 start = 0;
    qint64 end = 0;
    int nibble = 0;
    if (!hexSelectionByteRange(&start, &end, &nibble))
        return false;
    const bool hasSelection = end > start;

    const bool shiftOnly = event->modifiers() == Qt::ShiftModifier;
    const bool controlShift = event->modifiers()
                              == (Qt::ControlModifier | Qt::ShiftModifier);
    if ((shiftOnly || controlShift)
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
            || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown
            || event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        const auto sourceBoundary = [this](long position) -> qint64 {
            int line = 0;
            int column = 0;
            m_editor->lineIndexFromPosition(position, &line, &column);
            const qint64 local = qint64(qMax(0, line)) * m_hexBytesPerRow
                + (qint64(qMax(0, column)) + 2) / 3;
            return qBound<qint64>(
                0, m_bridge.windowStart + local,
                m_l2View->virtualFileSize());
        };
        const auto overlayBoundary = [this](qint64 globalByte) -> long {
            const qint64 local = qBound<qint64>(
                0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
            if (local >= m_bridge.windowLen)
                return m_editor->length();
            return m_editor->positionFromLineIndex(
                int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX)),
                int((local % m_hexBytesPerRow) * 3));
        };
        const long anchorPosition = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETANCHOR);
        const long caretPosition = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        qint64 anchorByte = hasSelection ? sourceBoundary(anchorPosition) : start;
        qint64 caretByte = hasSelection ? sourceBoundary(caretPosition) : start;
        const qint64 size = m_l2View->virtualFileSize();
        const qint64 pageRows = qMax<qint64>(1, m_editor->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN));
        if (controlShift && event->key() == Qt::Key_Home)
            caretByte = 0;
        else if (controlShift && event->key() == Qt::Key_End)
            caretByte = size;
        else if (event->key() == Qt::Key_Left)
            caretByte = qMax<qint64>(0, caretByte - 1);
        else if (event->key() == Qt::Key_Right)
            caretByte = qMin(size, caretByte + 1);
        else if (event->key() == Qt::Key_Up)
            caretByte = qMax<qint64>(0, caretByte - m_hexBytesPerRow);
        else if (event->key() == Qt::Key_Down)
            caretByte = qMin(size, caretByte + m_hexBytesPerRow);
        else if (event->key() == Qt::Key_PageUp)
            caretByte = qMax<qint64>(
                0, caretByte - pageRows * m_hexBytesPerRow);
        else if (event->key() == Qt::Key_PageDown)
            caretByte = qMin(size,
                caretByte + pageRows * m_hexBytesPerRow);
        else {
            const qint64 rowStart = (caretByte / m_hexBytesPerRow)
                                    * m_hexBytesPerRow;
            caretByte = event->key() == Qt::Key_Home
                ? rowStart : qMin(size, rowStart + m_hexBytesPerRow);
        }
        const long newAnchor = overlayBoundary(anchorByte);
        const long newCaret = overlayBoundary(caretByte);
        if (newAnchor >= 0 && newCaret >= 0) {
            m_adjustingHexCaret = true;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                    newAnchor, newCaret);
            m_adjustingHexCaret = false;
            syncAsciiSelectionFromHex();
        }
        return true;
    }

    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        const qint64 local = qMax<qint64>(0, start - m_bridge.windowStart);
        const qint64 row = local / qMax(1, m_hexBytesPerRow);
        const qint64 rowStart = m_bridge.windowStart + row * m_hexBytesPerRow;
        if (event->key() == Qt::Key_Home) {
            setHexCaret(qMin(rowStart, m_l2View->virtualFileSize()), 0);
        } else {
            const qint64 rowEnd = qMin(
                qMin(rowStart + m_hexBytesPerRow,
                     m_bridge.windowStart + m_bridge.windowLen),
                m_l2View->virtualFileSize());
            if (rowEnd > rowStart)
                setHexCaret(rowEnd - 1, 1);
        }
        syncAsciiSelectionFromHex();
        return true;
    }

    // Scintilla sees the Hex projection as ordinary text ("00 01 ...") and
    // would therefore place the caret on separator spaces.  Horizontal Hex
    // navigation is byte/nibble navigation: high -> low -> next byte high,
    // with the inverse sequence for Left.  Keep application shortcuts and
    // Shift-selection on their dedicated paths.
    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        qint64 targetByte = start;
        int targetNibble = nibble;
        if (event->key() == Qt::Key_Right) {
            if (targetNibble == 0) {
                targetNibble = 1;
            } else if (targetByte < m_l2View->virtualFileSize()) {
                ++targetByte;
                targetNibble = 0;
            }
        } else {
            if (targetNibble == 1) {
                targetNibble = 0;
            } else if (targetByte > 0) {
                --targetByte;
                targetNibble = 1;
            }
        }
        setHexCaret(targetByte, targetNibble);
        syncAsciiSelectionFromHex();
        return true;
    }

    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown)) {
        qint64 rowDelta = event->key() == Qt::Key_Up ? -1
            : event->key() == Qt::Key_Down ? 1
            : event->key() == Qt::Key_PageUp
                ? -qMax<qint64>(1, m_editor->SendScintilla(
                    QsciScintillaBase::SCI_LINESONSCREEN))
                : qMax<qint64>(1, m_editor->SendScintilla(
                    QsciScintillaBase::SCI_LINESONSCREEN));
        qint64 targetByte = start + rowDelta * m_hexBytesPerRow;
        if (m_l2View->virtualFileSize() > 0)
            targetByte = qBound<qint64>(
                0, targetByte, m_l2View->virtualFileSize() - 1);
        else
            targetByte = 0;
        setHexCaret(targetByte, nibble);
        syncAsciiSelectionFromHex();
        return true;
    }

    if (!hasSelection
        && (event->modifiers() == Qt::ControlModifier)
        && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        if (event->key() == Qt::Key_Home)
            setHexCaret(0, 0);
        else
            setHexCaret(m_l2View->virtualFileSize(), 0);
        syncAsciiSelectionFromHex();
        return true;
    }

    // Read-only Hex projections still require byte-aware navigation above,
    // but no mutating key may fall through to Scintilla's text projection.
    if (isReadOnlyByPolicy()) {
        if (event->matches(QKeySequence::Cut)
            || event->matches(QKeySequence::Paste)
            || event->key() == Qt::Key_Delete
            || event->key() == Qt::Key_Backspace
            || event->key() == Qt::Key_Insert
            || (!event->text().isEmpty()
                && !(event->modifiers()
                     & (Qt::ControlModifier | Qt::AltModifier
                        | Qt::MetaModifier)))) {
            return true;
        }
        return false;
    }

    if (event->matches(QKeySequence::Paste)) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        QByteArray pasted;
        QString parseError;
        if (!parseHexBytes(QGuiApplication::clipboard()->text(), &pasted, &parseError)
            || pasted.isEmpty()) {
            showError(parseError.isEmpty()
                          ? QObject::tr("剪贴板中没有十六进制字节。")
                          : parseError);
            return true;
        }
        QString error;
        qint64 replaced = end - start;
        if (!hasSelection && m_hexOverwriteMode) {
            const qint64 available = m_l2View->virtualFileSize() - start;
            if (available <= 0) {
                if (isSecureBinaryDocument() && !pasted.isEmpty())
                    pasted.fill(0);
                return true;
            }
            if (pasted.size() > available)
                pasted.truncate(int(available));
            replaced = pasted.size();
        }
        const qint32 oldLength = qint32(qMin<qint64>(replaced, INT_MAX));
        if (!m_l2View->replaceBytes(start, oldLength, pasted, &error)) {
            showError(error);
            return true;
        }
        if (isSecureBinaryDocument() && !pasted.isEmpty())
            pasted.fill(0);
        refresh(start + pasted.size(), 0);
        return true;
    }

    if (event->matches(QKeySequence::Cut)) {
        if (!hasSelection)
            return true;
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        QByteArray selected = m_l2View->readByteRange(start, end);
        QGuiApplication::clipboard()->setText(formatHexBytes(selected));
        if (isSecureBinaryDocument() && !selected.isEmpty())
            selected.fill(0);
        QString error;
        if (!m_l2View->replaceBytes(start, qint32(end - start), {}, &error))
            showError(error);
        else
            refresh(start, 0);
        return true;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        if (!hasSelection) {
            if (event->key() == Qt::Key_Backspace) {
                if (start <= 0)
                    return true;
                --start;
            } else if (start >= m_l2View->virtualFileSize()) {
                return true;
            }
            end = qMin(m_l2View->virtualFileSize(), start + 1);
        }
        if (end <= start)
            return true;
        QString error;
        if (!m_l2View->replaceBytes(start, qint32(end - start), {}, &error))
            showError(error);
        else
            refresh(start, 0);
        return true;
    }

    const QString typed = event->text();
    if (typed.size() == 1 && hexNibble(typed.at(0)) >= 0
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);

        const int value = hexNibble(typed.at(0));
        const qint64 size = m_l2View->virtualFileSize();
        QByteArray replacement;
        qint32 replacedLength = 0;

        if (end > start) {
            // Direct typing replaces the complete byte selection in both OVR
            // and INS.  The first digit creates X0; the second edits its low
            // nibble.  Partial projection text is never allowed to survive.
            replacement = QByteArray(1, char(value << 4));
            replacedLength = qint32(qMin<qint64>(end - start, INT_MAX));
            nibble = 0;
        } else if (!m_hexOverwriteMode && nibble == 0) {
            // INS inserts only when entering the high nibble.  Low-nibble
            // entry completes the byte just inserted, matching established
            // Hex-editor behavior.
            replacement = QByteArray(1, char(value << 4));
        } else {
            if (start >= size)
                return true; // OVR cannot extend the document at EOF.
            replacement = m_l2View->readByteRange(start, start + 1);
            if (replacement.size() != 1) {
                showError(QObject::tr("无法读取待编辑字节，文件保持不变。"));
                return true;
            }
            const uchar oldByte = uchar(replacement.at(0));
            replacement[0] = char(nibble == 0
                ? ((value << 4) | (oldByte & 0x0f))
                : ((oldByte & 0xf0) | value));
            replacedLength = 1;
        }

        QString editError;
        if (!m_l2View->replaceBytes(start, replacedLength, replacement,
                                    &editError)) {
            showError(editError);
            if (isSecureBinaryDocument() && !replacement.isEmpty())
                replacement.fill(0);
            return true;
        }
        if (isSecureBinaryDocument() && !replacement.isEmpty())
            replacement.fill(0);

        const bool completedByte = nibble == 1;
        refresh(start + (completedByte ? 1 : 0), completedByte ? 0 : 1);
        return true;
    }

    // Navigation and application shortcuts remain native. Text-producing keys
    // are rejected so spaces/newlines cannot corrupt the byte projection.
    if (!typed.isEmpty()
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        showError(QObject::tr("十六进制视图只接受 0–9、A–F；ASCII 栏为同步预览。"));
        return true;
    }
    return false;
}

// S2 background ILoader promotion was removed after real-user validation.
// Files >= 32 MiB remain on WindowedReadOnlyProvider for their entire lifetime.
