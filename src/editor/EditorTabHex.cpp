#include "EditorTab.h"
#include "HexProjection.h"
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




// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================

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
    if (hexOverlayObject) {
        const auto result = handleHexAccessibleFocusTraversal(ev)
            ? HexMouseSelectionController::FilterResult::Consume
            : m_hexMouseSelection.filter(obj, ev);
        if (result == HexMouseSelectionController::FilterResult::Consume)
            return true;
        if (result == HexMouseSelectionController::FilterResult::SourceRelease)
            return QWidget::eventFilter(obj, ev);
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
            m_hexMouseSelection.finishNativeSelection();
            syncHexSelectionFromAscii();
            return QWidget::eventFilter(obj, ev);
        }
        QTimer::singleShot(0, this, [this, asciiSurface]() {
            m_hexMouseSelection.finishNativeSelection();
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
    return m_hexMouseSelection.asciiSelectionByteRange(start, end);
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
        || m_hexMouseSelection.sourceSelectionActive()
        || m_hexMouseSelection.sourceAuthoritative()
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
    if (end == start && !m_hexMouseSelection.selecting()) {
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
    } else if (!m_hexMouseSelection.selecting()) {
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
    const long sourceAnchorPosition = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETANCHOR);
    const long sourceCaretPosition = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETCURRENTPOS);
    const bool sourceForward = sourceCaretPosition >= sourceAnchorPosition;
    m_l2View->selectBytes(sourceForward ? start : end,
                          sourceForward ? end : start);
    m_lastL2Byte = end > start ? end : start;
}

void EditorTab::syncHexSelectionFromAscii()
{
    if (m_adjustingAsciiCaret || m_adjustingHexCaret || m_bridge.applying
        || m_hexMouseSelection.selecting()
        || m_hexMouseSelection.sourceSelectionActive()
        || m_hexMouseSelection.sourceAuthoritative()
        || !m_hexAsciiPreview) {
        return;
    }
    qint64 start = 0;
    qint64 end = 0;
    if (!asciiSelectionByteRange(&start, &end))
        return;
    const QTextCursor directionalCursor = m_hexAsciiPreview->textCursor();
    const bool forward = directionalCursor.position() >= directionalCursor.anchor();
    const qint64 anchorByte = forward ? start : end;
    const qint64 caretByte = forward ? end : start;

    const auto overlayPosition = [this](qint64 globalByte) -> long {
        const qint64 local = qBound<qint64>(
            0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
        if (local >= m_bridge.windowLen)
            return m_editor->length();
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3);
        return m_editor->positionFromLineIndex(line, column);
    };

    const long first = overlayPosition(anchorByte);
    const long last = overlayPosition(caretByte);
    if (first < 0 || last < 0)
        return;
    m_adjustingHexCaret = true;
    if (end > start) {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, first, last);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, first);
    }
    m_adjustingHexCaret = false;
    m_l2View->selectBytes(anchorByte, caretByte);
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
    m_hexMouseSelection.relinquishAuthority();

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
        QString limitError;
        if (!ByteClipboard::payloadWithinLimit(end - start, &limitError)) {
            showError(limitError);
            return true;
        }
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
        QString limitError;
        if (!ByteClipboard::payloadWithinLimit(pasted.size(), &limitError)) {
            if (isSecureBinaryDocument() && !pasted.isEmpty())
                pasted.fill(0);
            showError(limitError);
            return true;
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
    return m_hexMouseSelection.hexSelectionByteRange(start, end, nibble);
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
        m_hexMouseSelection.relinquishAuthority();
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

    if ((event->modifiers() == Qt::ControlModifier)
        && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        // A pointer drag publishes a source-authoritative byte range which is
        // intentionally independent of Scintilla's projected text selection.
        // Ctrl+Home/End is a collapsing navigation command: release that range
        // before moving, otherwise the next edit still replaces the stale
        // source selection even though the visible caret moved.
        m_hexMouseSelection.relinquishAuthority();
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
        QString limitError;
        if (!ByteClipboard::payloadWithinLimit(pasted.size(), &limitError)) {
            if (isSecureBinaryDocument() && !pasted.isEmpty())
                pasted.fill(0);
            showError(limitError);
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
        QString limitError;
        if (!ByteClipboard::payloadWithinLimit(end - start, &limitError)) {
            showError(limitError);
            return true;
        }
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
