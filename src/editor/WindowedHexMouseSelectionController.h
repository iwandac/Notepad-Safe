#pragma once

#include <QTimer>
#include <QtGlobal>

class QEvent;
class QObject;
class QPoint;
class WindowedReadOnlyProvider;

// Owns one physical Hex/ASCII mouse gesture for the bounded read-only
// projection. Window ownership, source bytes and selection authority remain in
// WindowedReadOnlyProvider; this controller owns only transient input state.
class WindowedHexMouseSelectionController final
{
public:
    enum class FilterResult { Continue, Consume, SourceRelease };

    explicit WindowedHexMouseSelectionController(
        WindowedReadOnlyProvider *provider);
    FilterResult filter(QObject *surface, QEvent *event);
    void reset();

private:
    qint64 sourceBoundaryAtPoint(QObject *surface, const QPoint &point) const;
    void step();

    WindowedReadOnlyProvider *provider_ = nullptr;
    QTimer timer_;
    bool selecting_ = false;
    bool sourceActive_ = false;
    bool asciiSurface_ = false;
    int autoScrollDirection_ = 0;
    int byteColumn_ = 0;
    qint64 anchor_ = -1;
    qint64 caret_ = -1;
    quint64 generation_ = 0;
};
