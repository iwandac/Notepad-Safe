#include "WindowedHexMouseSelectionController.h"

#include "WindowedReadOnlyProvider.h"

#include <algorithm>

#include <QEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

WindowedHexMouseSelectionController::WindowedHexMouseSelectionController(
    WindowedReadOnlyProvider *provider)
    : provider_(provider)
{
    timer_.setInterval(40);
    QObject::connect(&timer_, &QTimer::timeout, [this]() { step(); });
}

void WindowedHexMouseSelectionController::reset()
{
    timer_.stop();
    selecting_ = false;
    sourceActive_ = false;
    autoScrollDirection_ = 0;
    anchor_ = -1;
    caret_ = -1;
    ++generation_;
}

WindowedHexMouseSelectionController::FilterResult
WindowedHexMouseSelectionController::filter(QObject *surface, QEvent *event)
{
    if (!provider_ || !event || provider_->m_fileSize <= 0
        || provider_->m_presentation
               != WindowedReadOnlyProvider::Presentation::Hex) {
        return FilterResult::Continue;
    }
    const bool editorSurface = provider_->m_editor
        && (surface == provider_->m_editor
            || surface == provider_->m_editor->viewport());
    const bool asciiSurface = provider_->m_asciiPreview
        && (surface == provider_->m_asciiPreview
            || surface == provider_->m_asciiPreview->viewport());
    if (!editorSurface && !asciiSurface)
        return FilterResult::Continue;

    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        timer_.stop();
        selecting_ = true;
        sourceActive_ = false;
        asciiSurface_ = asciiSurface;
        autoScrollDirection_ = 0;
        ++generation_;
        provider_->clearVirtualSelection();
        const auto *mouse = static_cast<QMouseEvent *>(event);
        anchor_ = sourceBoundaryAtPoint(surface,
                                        mouse->position().toPoint());
        caret_ = anchor_;
        byteColumn_ = anchor_ >= 0
            ? int(anchor_ % qMax(1, provider_->m_hexBytesPerRow)) : 0;
        return FilterResult::Continue;
    }

    if (event->type() == QEvent::MouseMove && selecting_) {
        QWidget *target = asciiSurface_
            ? (provider_->m_asciiPreview
                   ? provider_->m_asciiPreview->viewport() : nullptr)
            : (provider_->m_editor
                   ? provider_->m_editor->viewport() : nullptr);
        if (!target || (surface != target
                        && (asciiSurface_ ? surface != provider_->m_asciiPreview
                                          : surface != provider_->m_editor))) {
            return FilterResult::Continue;
        }
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const int edge = qMax(8, target->fontMetrics().height());
        const int y = mouse->position().toPoint().y();
        int direction = y <= edge ? -1
            : y >= target->height() - edge ? 1 : 0;
        if ((direction < 0 && provider_->visibleTopOffset() <= 0)
            || (direction > 0
                && provider_->visibleBottomOffset() >= provider_->m_fileSize)) {
            direction = 0;
        }
        autoScrollDirection_ = direction;
        if (direction != 0 && anchor_ >= 0) {
            if (!sourceActive_) {
                provider_->m_selectionAuthority =
                    WindowedReadOnlyProvider::SelectionAuthority::SourceBytes;
                provider_->m_sourceSelection.selectEndpoints({anchor_}, {anchor_});
                sourceActive_ = true;
            }
            if (!timer_.isActive())
                timer_.start();
            step();
            event->accept();
            return FilterResult::Consume;
        }
        timer_.stop();
        if (sourceActive_) {
            const qint64 caret = sourceBoundaryAtPoint(
                surface, mouse->position().toPoint());
            if (caret >= 0) {
                caret_ = caret;
                provider_->m_sourceSelection.selectEndpoints({anchor_}, {caret_});
                provider_->applyVirtualSelection();
            }
            event->accept();
            return FilterResult::Consume;
        }
    }

    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        timer_.stop();
        autoScrollDirection_ = 0;
        if (sourceActive_) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const qint64 caret = sourceBoundaryAtPoint(
                surface, mouse->position().toPoint());
            if (caret >= 0)
                caret_ = caret;
            const qint64 anchor = anchor_;
            const qint64 finalCaret = caret_;
            const quint64 generation = generation_;
            selecting_ = false;
            sourceActive_ = false;
            QTimer::singleShot(0, provider_,
                [this, anchor, finalCaret, generation]() {
                    if (!provider_ || generation != generation_
                        || provider_->m_path.isEmpty()) {
                        return;
                    }
                    provider_->m_selectionAuthority =
                        WindowedReadOnlyProvider::SelectionAuthority::SourceBytes;
                    provider_->m_sourceSelection.selectEndpoints(
                        {anchor}, {finalCaret});
                    provider_->applyVirtualSelection();
                    provider_->m_asciiMouseSelecting = false;
                });
            return FilterResult::SourceRelease;
        }
        selecting_ = false;
    }
    return FilterResult::Continue;
}

qint64 WindowedHexMouseSelectionController::sourceBoundaryAtPoint(
    QObject *surface, const QPoint &point) const
{
    const bool ascii = provider_->m_asciiPreview
        && (surface == provider_->m_asciiPreview
            || surface == provider_->m_asciiPreview->viewport());
    if (ascii) {
        const QTextCursor cursor = provider_->m_asciiPreview->cursorForPosition(point);
        const QTextBlock block = cursor.block();
        if (!block.isValid())
            return -1;
        const qint64 column = std::clamp<qint64>(
            cursor.position() - block.position(), 0,
            qMin(provider_->m_hexBytesPerRow, block.text().size()));
        return std::clamp(
            provider_->m_windowStart
                + qint64(block.blockNumber()) * provider_->m_hexBytesPerRow
                + column,
            0LL, provider_->m_fileSize);
    }
    const long position = provider_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
        static_cast<unsigned long>(qMax(0, point.x())),
        static_cast<long>(qMax(0, point.y())));
    if (position < 0)
        return -1;
    int line = 0;
    int column = 0;
    provider_->m_editor->lineIndexFromPosition(position, &line, &column);
    qint64 byteInRow = qMax(0, column) / 3;
    if (qMax(0, column) % 3 == 2)
        ++byteInRow;
    return std::clamp(
        provider_->m_windowStart
            + qint64(qMax(0, line)) * provider_->m_hexBytesPerRow
            + std::clamp<qint64>(byteInRow, 0, provider_->m_hexBytesPerRow),
        0LL, provider_->m_fileSize);
}

void WindowedHexMouseSelectionController::step()
{
    if (!selecting_ || !sourceActive_ || autoScrollDirection_ == 0
        || anchor_ < 0 || !provider_ || provider_->m_path.isEmpty()) {
        timer_.stop();
        return;
    }
    const int rowsOnScreen = qMax(1, int(provider_->m_editor->SendScintilla(
        QsciScintillaBase::SCI_LINESONSCREEN)));
    provider_->scrollByLines(
        autoScrollDirection_ * qMax(1, rowsOnScreen / 4));
    const int y = autoScrollDirection_ > 0
        ? qMax(1, provider_->m_editor->viewport()->height() - 2) : 1;
    qint64 rowStart = provider_->sourceOffsetAtViewportY(y);
    if (rowStart < 0) {
        rowStart = autoScrollDirection_ > 0
            ? provider_->visibleBottomOffset() : provider_->visibleTopOffset();
    }
    rowStart -= rowStart % qMax(1, provider_->m_hexBytesPerRow);
    qint64 caret = rowStart + byteColumn_;
    if (autoScrollDirection_ > 0)
        ++caret;
    caret = std::clamp(caret, 0LL, provider_->m_fileSize);
    if (caret == caret_
        && ((caret == 0 && autoScrollDirection_ < 0)
            || (caret == provider_->m_fileSize
                && autoScrollDirection_ > 0))) {
        timer_.stop();
    }
    caret_ = caret;
    provider_->m_sourceSelection.selectEndpoints({anchor_}, {caret_});
    provider_->applyVirtualSelection();
}
