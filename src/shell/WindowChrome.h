#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>

class QWidget;

enum class ChromeHit {
    Client, Caption, MaxButton,
    Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight
};

// Frameless window support: the client area covers the whole window, and the
// title bar answers hit tests so Windows keeps drag, double-click, Snap
// Layouts on the maximize button, resize edges and Win+Arrow.
class WindowChrome final {
public:
    // Pure hit test in window-local logical px. Interactive rects inside the
    // caption (buttons, the command center) are client area so they get
    // clicks. A maximized window has no resize edges.
    static ChromeHit hitTest(QPoint local, QSize window, int resizeBorder,
                             QRect captionArea, QRect maxButton, bool maximized,
                             const QVector<QRect> &interactive = {});

    // Rounded corners and the Mica backdrop when the OS supports them.
    static void install(QWidget *window);

    // Handles WM_NCCALCSIZE and WM_NCHITTEST. Returns true when the message
    // was consumed and *result is set.
    static bool handleNativeEvent(QWidget *window, void *message, qintptr *result,
                                  int resizeBorder, QRect captionArea,
                                  QRect maxButton,
                                  const QVector<QRect> &interactive);
};
