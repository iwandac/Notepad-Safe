#include "HexMouseSelectionController.h"

#include "EditorTab.h"
#include "VirtualFileView.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <Qsci/qsciscintillabase.h>

HexMouseSelectionController::HexMouseSelectionController(EditorTab *tab)
    : tab_(tab)
{
    timer_.setInterval(40);
    QObject::connect(&timer_, &QTimer::timeout, [this]() { step(); });
}

HexMouseSelectionController::FilterResult HexMouseSelectionController::filter(
    QObject *object, QEvent *event)
{
    if (event->type() == QEvent::KeyPress
        && handleSourceKeyPress(static_cast<QKeyEvent *>(event))) {
        return FilterResult::Consume;
    }
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        selecting_ = true;
        ++gestureGeneration_;
        sourceSelectionActive_ = false;
        sourceAuthoritative_ = false;
        autoScrollDirection_ = 0;
        timer_.stop();
        const auto *mouse = static_cast<QMouseEvent *>(event);
        asciiSurface_ = tab_->m_hexAsciiPreview
            && (object == tab_->m_hexAsciiPreview
                || object == tab_->m_hexAsciiPreview->viewport());
        anchorByte_ = pointToSourceByte(object, mouse->position().toPoint());
        caretByte_ = anchorByte_;
        if (anchorByte_ >= 0) {
            byteColumn_ = int(anchorByte_ % qMax(1, tab_->m_hexBytesPerRow));
            tab_->m_l2View->selectBytes(anchorByte_, anchorByte_);
        }
    }
    if (event->type() == QEvent::MouseMove && selecting_) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        QWidget *surface = asciiSurface_
            ? (tab_->m_hexAsciiPreview
                   ? tab_->m_hexAsciiPreview->viewport() : nullptr)
            : (tab_->m_editor ? tab_->m_editor->viewport() : nullptr);
        if (surface && (object == surface
                        || (asciiSurface_
                            && object == tab_->m_hexAsciiPreview)
                        || (!asciiSurface_
                            && object == tab_->m_editor))) {
            const int edge = qMax(8, surface->fontMetrics().height());
            const int y = mouse->position().toPoint().y();
            int direction = y <= edge ? -1
                : y >= surface->height() - edge ? 1 : 0;
            if (direction != 0) {
                const QScrollBar *global = tab_->m_l2View->verticalScrollBar();
                if ((direction < 0 && global->value() <= global->minimum())
                    || (direction > 0
                        && global->value() >= global->maximum())) {
                    direction = 0;
                }
            }
            autoScrollDirection_ = direction;
            if (direction != 0 && anchorByte_ >= 0) {
                sourceSelectionActive_ = true;
                sourceAuthoritative_ = true;
                if (!timer_.isActive())
                    timer_.start();
                step();
                event->accept();
                return FilterResult::Consume;
            }
            timer_.stop();
            if (sourceSelectionActive_) {
                const qint64 caret = pointToSourceByte(
                    object, mouse->position().toPoint());
                if (caret >= 0) {
                    caretByte_ = caret;
                    tab_->m_l2View->selectBytes(anchorByte_, caret);
                    applySourceSelectionToProjection(anchorByte_, caret);
                }
                event->accept();
                return FilterResult::Consume;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        timer_.stop();
        autoScrollDirection_ = 0;
        if (sourceSelectionActive_) {
            const qint64 anchor = anchorByte_;
            const qint64 caret = caretByte_;
            const quint64 generation = gestureGeneration_;
            QTimer::singleShot(0, tab_, [this, anchor, caret, generation]() {
                if (generation != gestureGeneration_)
                    return;
                applySourceSelectionToProjection(anchor, caret);
                if (tab_->m_l2View)
                    tab_->m_l2View->selectBytes(anchor, caret);
                selecting_ = false;
                QTimer::singleShot(0, tab_, [this, generation]() {
                    if (generation != gestureGeneration_)
                        return;
                    sourceSelectionActive_ = false;
                    anchorByte_ = -1;
                    caretByte_ = -1;
                });
            });
            return FilterResult::SourceRelease;
        }
    }
    return FilterResult::Continue;
}

bool HexMouseSelectionController::handleSourceKeyPress(QKeyEvent *event)
{
    if (!sourceAuthoritative_ || !event || !tab_->m_l2View)
        return false;
    const bool plain = event->modifiers() == Qt::NoModifier;
    const bool shift = event->modifiers() == Qt::ShiftModifier;
    if ((plain || shift)
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        timer_.stop();
        ++gestureGeneration_;
        selecting_ = false;
        sourceSelectionActive_ = false;
        if (plain) {
            const qint64 target = event->key() == Qt::Key_Left
                ? tab_->m_l2View->selectionStart()
                : tab_->m_l2View->selectionEnd();
            sourceAuthoritative_ = false;
            tab_->m_l2View->selectBytes(target, target);
            tab_->setHexCaret(target, 0);
            tab_->syncAsciiSelectionFromHex();
            return true;
        }
        const qint64 anchor = tab_->m_l2View->selectionAnchorByte();
        qint64 caret = tab_->m_l2View->selectionCaretByte();
        caret += event->key() == Qt::Key_Left ? -1 : 1;
        caret = qBound<qint64>(0, caret,
                               tab_->m_l2View->virtualFileSize());
        tab_->m_l2View->syncActiveWindowToCursor(caret);
        tab_->m_l2View->selectBytes(anchor, caret);
        applySourceSelectionToProjection(anchor, caret);
        return true;
    }
    return false;
}

qint64 HexMouseSelectionController::pointToSourceByte(
    QObject *object, const QPoint &point) const
{
    if (!tab_->m_l2View || tab_->m_bridge.windowStart < 0
        || tab_->m_hexBytesPerRow <= 0) {
        return -1;
    }
    const bool asciiSurface = tab_->m_hexAsciiPreview
        && (object == tab_->m_hexAsciiPreview
            || object == tab_->m_hexAsciiPreview->viewport());
    if (asciiSurface) {
        const QTextCursor cursor = tab_->m_hexAsciiPreview->cursorForPosition(point);
        const QTextBlock block = cursor.block();
        if (!block.isValid())
            return -1;
        const qint64 column = qBound<qint64>(
            qint64(0), qint64(cursor.position() - block.position()),
            qint64(qMin(tab_->m_hexBytesPerRow, block.text().size())));
        return qBound<qint64>(
            0, tab_->m_bridge.windowStart
                   + qint64(block.blockNumber()) * tab_->m_hexBytesPerRow
                   + column,
            tab_->m_l2View->virtualFileSize());
    }
    const long position = tab_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
        static_cast<unsigned long>(qMax(0, point.x())),
        static_cast<long>(qMax(0, point.y())));
    if (position < 0)
        return -1;
    int line = 0;
    int column = 0;
    tab_->m_editor->lineIndexFromPosition(position, &line, &column);
    qint64 byteInRow = qMax(0, column) / 3;
    if (qMax(0, column) % 3 == 2)
        ++byteInRow;
    return qBound<qint64>(
        0, tab_->m_bridge.windowStart
               + qint64(qMax(0, line)) * tab_->m_hexBytesPerRow
               + qBound<qint64>(qint64(0), byteInRow,
                                qint64(tab_->m_hexBytesPerRow)),
        tab_->m_l2View->virtualFileSize());
}

bool HexMouseSelectionController::asciiSelectionByteRange(
    qint64 *start, qint64 *end) const
{
    if (!start || !end || !tab_->m_hexAsciiPreview || !tab_->m_l2View
        || tab_->m_l2View->viewMode() != ViewMode::Hex
        || tab_->m_bridge.windowStart < 0) {
        return false;
    }
    if (sourceAuthoritative_) {
        *start = tab_->m_l2View->selectionStart();
        *end = tab_->m_l2View->selectionEnd();
        return true;
    }
    const QTextCursor cursor = tab_->m_hexAsciiPreview->textCursor();
    const auto mapPosition = [this](int position) {
        const QTextBlock block = tab_->m_hexAsciiPreview->document()
            ->findBlock(position);
        if (!block.isValid())
            return tab_->m_bridge.windowLen;
        const qint64 column = qBound<qint64>(
            qint64(0), qint64(position - block.position()),
            qint64(qMin(tab_->m_hexBytesPerRow, block.text().size())));
        return qMin(tab_->m_bridge.windowLen,
                    qint64(block.blockNumber()) * tab_->m_hexBytesPerRow
                        + column);
    };
    qint64 localStart = mapPosition(cursor.selectionStart());
    qint64 localEnd = mapPosition(cursor.selectionEnd());
    if (localEnd < localStart)
        std::swap(localStart, localEnd);
    *start = tab_->m_bridge.windowStart + localStart;
    *end = tab_->m_bridge.windowStart + localEnd;
    return true;
}

bool HexMouseSelectionController::hexSelectionByteRange(
    qint64 *start, qint64 *end, int *nibble) const
{
    if (!start || !end || !tab_->m_editor || !tab_->m_l2View
        || tab_->m_l2View->viewMode() != ViewMode::Hex
        || tab_->m_bridge.windowStart < 0) {
        return false;
    }
    if (sourceAuthoritative_) {
        *start = tab_->m_l2View->selectionStart();
        *end = tab_->m_l2View->selectionEnd();
        if (nibble)
            *nibble = 0;
        return true;
    }
    long selectionStart = tab_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONSTART);
    long selectionEnd = tab_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONEND);
    if (selectionEnd < selectionStart)
        std::swap(selectionStart, selectionEnd);
    const bool selected = selectionEnd > selectionStart;
    const auto mapPosition = [this](long position, bool endBoundary,
                                    int *outNibble) -> qint64 {
        int line = 0;
        int column = 0;
        tab_->m_editor->lineIndexFromPosition(position, &line, &column);
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
                                    qint64(tab_->m_hexBytesPerRow));
        if (outNibble)
            *outNibble = qBound(0, localNibble, 1);
        return qint64(line) * tab_->m_hexBytesPerRow + byteInLine;
    };
    int localNibble = 0;
    const qint64 localStart = mapPosition(selectionStart, false, &localNibble);
    const qint64 localEnd = selected
        ? mapPosition(selectionEnd, true, nullptr) : localStart;
    const qint64 windowEnd = tab_->m_bridge.windowStart
        + tab_->m_bridge.windowLen;
    *start = qBound(tab_->m_bridge.windowStart,
                    tab_->m_bridge.windowStart + localStart, windowEnd);
    *end = qBound(*start, tab_->m_bridge.windowStart + localEnd, windowEnd);
    if (nibble)
        *nibble = localNibble;
    return true;
}

void HexMouseSelectionController::step()
{
    if (!selecting_ || !sourceSelectionActive_
        || autoScrollDirection_ == 0 || !tab_->m_l2View
        || !tab_->m_editor || tab_->m_l2View->viewMode() != ViewMode::Hex
        || anchorByte_ < 0) {
        timer_.stop();
        return;
    }
    QScrollBar *global = tab_->m_l2View->verticalScrollBar();
    const int rowsOnScreen = qMax(1, int(tab_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_LINESONSCREEN)));
    const int stepRows = qMax(1, rowsOnScreen / 6);
    const int nextTop = qBound(global->minimum(),
        global->value() + autoScrollDirection_ * stepRows,
        global->maximum());
    if (nextTop == global->value()
        && ((nextTop == global->minimum()
             && autoScrollDirection_ < 0)
            || (nextTop == global->maximum()
                && autoScrollDirection_ > 0))) {
        timer_.stop();
    }
    global->setValue(nextTop);
    const qint64 row = autoScrollDirection_ > 0
        ? qint64(nextTop) + rowsOnScreen - 1 : qint64(nextTop);
    qint64 caret = row * tab_->m_hexBytesPerRow + byteColumn_;
    if (autoScrollDirection_ > 0)
        ++caret;
    caret = qBound<qint64>(0, caret, tab_->m_l2View->virtualFileSize());
    caretByte_ = caret;
    tab_->m_l2View->syncActiveWindowToCursor(caret);
    tab_->m_l2View->selectBytes(anchorByte_, caret);
    applySourceSelectionToProjection(anchorByte_, caret);
}

void HexMouseSelectionController::applySourceSelectionToProjection(
    qint64 anchor, qint64 caret)
{
    if (!tab_->m_l2View || !tab_->m_editor || !tab_->m_hexAsciiPreview
        || tab_->m_l2View->viewMode() != ViewMode::Hex
        || tab_->m_bridge.windowStart < 0 || tab_->m_bridge.windowLen <= 0) {
        return;
    }
    const qint64 windowStart = tab_->m_bridge.windowStart;
    const qint64 windowEnd = windowStart + tab_->m_bridge.windowLen;
    const qint64 visibleAnchor = qBound(windowStart, qint64(anchor), windowEnd);
    const qint64 visibleCaret = qBound(windowStart, qint64(caret), windowEnd);
    const auto hexPosition = [this, windowStart](qint64 sourceByte) -> long {
        const qint64 local = sourceByte - windowStart;
        if (local >= tab_->m_bridge.windowLen)
            return tab_->m_editor->length();
        return tab_->m_editor->positionFromLineIndex(
            int(qMin<qint64>(local / tab_->m_hexBytesPerRow, INT_MAX)),
            int((local % tab_->m_hexBytesPerRow) * 3));
    };
    const auto asciiPosition = [this, windowStart](qint64 sourceByte) -> int {
        const qint64 local = sourceByte - windowStart;
        if (local >= tab_->m_bridge.windowLen)
            return qMax(0, tab_->m_hexAsciiPreview->document()->characterCount() - 1);
        const QTextBlock block = tab_->m_hexAsciiPreview->document()
            ->findBlockByNumber(int(local / tab_->m_hexBytesPerRow));
        if (!block.isValid())
            return qMax(0, tab_->m_hexAsciiPreview->document()->characterCount() - 1);
        return qMin(block.position() + int(local % tab_->m_hexBytesPerRow),
                    block.position() + block.text().size());
    };
    tab_->m_adjustingHexCaret = true;
    tab_->m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                 hexPosition(visibleAnchor),
                                 hexPosition(visibleCaret));
    tab_->m_adjustingHexCaret = false;
    QTextCursor asciiCursor(tab_->m_hexAsciiPreview->document());
    asciiCursor.setPosition(asciiPosition(visibleAnchor));
    asciiCursor.setPosition(asciiPosition(visibleCaret), QTextCursor::KeepAnchor);
    tab_->m_adjustingAsciiCaret = true;
    tab_->m_hexAsciiPreview->setTextCursor(asciiCursor);
    tab_->m_adjustingAsciiCaret = false;
    const qint64 globalFirst = tab_->m_l2View->verticalScrollBar()->value();
    const int localFirst = int(qMax<qint64>(
        0, globalFirst - windowStart / tab_->m_hexBytesPerRow));
    tab_->m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                 static_cast<unsigned long>(localFirst));
    tab_->m_hexAsciiPreview->verticalScrollBar()->setValue(localFirst);
}
