#pragma once

#include <QTimer>
#include <QtGlobal>

class EditorTab;
class QObject;
class QEvent;
class QKeyEvent;
class QPoint;

class HexMouseSelectionController final
{
public:
    enum class FilterResult { Continue, Consume, SourceRelease };

    explicit HexMouseSelectionController(EditorTab *tab);
    FilterResult filter(QObject *object, QEvent *event);
    bool handleSourceKeyPress(QKeyEvent *event);
    bool asciiSelectionByteRange(qint64 *start, qint64 *end) const;
    bool hexSelectionByteRange(qint64 *start, qint64 *end,
                               int *nibble = nullptr) const;
    void applySourceSelectionToProjection(qint64 anchor, qint64 caret);
    bool selecting() const { return selecting_; }
    bool sourceSelectionActive() const { return sourceSelectionActive_; }
    bool sourceAuthoritative() const { return sourceAuthoritative_; }
    void finishNativeSelection() { selecting_ = false; }
    void relinquishAuthority() { sourceAuthoritative_ = false; }

private:
    qint64 pointToSourceByte(QObject *object, const QPoint &point) const;
    void step();

    EditorTab *tab_ = nullptr;
    QTimer timer_;
    bool selecting_ = false;
    bool sourceSelectionActive_ = false;
    bool sourceAuthoritative_ = false;
    bool asciiSurface_ = false;
    quint64 gestureGeneration_ = 0;
    qint64 anchorByte_ = -1;
    qint64 caretByte_ = -1;
    int byteColumn_ = 0;
    int autoScrollDirection_ = 0;
};
