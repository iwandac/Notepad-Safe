#include "WindowChrome.h"

#include <QWidget>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#pragma comment(lib, "dwmapi.lib")
#endif

ChromeHit WindowChrome::hitTest(QPoint local, QSize window, int resizeBorder,
                                QRect captionArea, QRect maxButton, bool maximized,
                                const QVector<QRect> &interactive)
{
    if (!maximized) {
        const bool left = local.x() < resizeBorder;
        const bool right = local.x() >= window.width() - resizeBorder;
        const bool top = local.y() < resizeBorder;
        const bool bottom = local.y() >= window.height() - resizeBorder;
        if (top && left) return ChromeHit::TopLeft;
        if (top && right) return ChromeHit::TopRight;
        if (bottom && left) return ChromeHit::BottomLeft;
        if (bottom && right) return ChromeHit::BottomRight;
        if (left) return ChromeHit::Left;
        if (right) return ChromeHit::Right;
        if (top) return ChromeHit::Top;
        if (bottom) return ChromeHit::Bottom;
    }
    // Snap Layouts appear only when Windows sees HTMAXBUTTON.
    if (maxButton.contains(local))
        return ChromeHit::MaxButton;
    for (const QRect &rect : interactive) {
        if (rect.contains(local))
            return ChromeHit::Client;
    }
    if (captionArea.contains(local))
        return ChromeHit::Caption;
    return ChromeHit::Client;
}

#ifdef Q_OS_WIN
namespace {
LRESULT toNative(ChromeHit hit)
{
    switch (hit) {
    case ChromeHit::Caption: return HTCAPTION;
    case ChromeHit::MaxButton: return HTMAXBUTTON;
    case ChromeHit::Left: return HTLEFT;
    case ChromeHit::Right: return HTRIGHT;
    case ChromeHit::Top: return HTTOP;
    case ChromeHit::Bottom: return HTBOTTOM;
    case ChromeHit::TopLeft: return HTTOPLEFT;
    case ChromeHit::TopRight: return HTTOPRIGHT;
    case ChromeHit::BottomLeft: return HTBOTTOMLEFT;
    case ChromeHit::BottomRight: return HTBOTTOMRIGHT;
    case ChromeHit::Client: break;
    }
    return HTCLIENT;
}

// Windows 11 attributes, defined here so older SDK headers still compile.
constexpr DWORD WindowCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD CornerRound = 2;              // DWMWCP_ROUND
constexpr DWORD SystemBackdropType = 38;      // DWMWA_SYSTEMBACKDROP_TYPE
constexpr DWORD BackdropMainWindow = 2;       // DWMSBT_MAINWINDOW (Mica)
} // namespace
#endif

void WindowChrome::install(QWidget *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, WindowCornerPreference, &CornerRound, sizeof(CornerRound));
    // Fails quietly before Windows 11 22H2; the solid ground color stays.
    DwmSetWindowAttribute(hwnd, SystemBackdropType, &BackdropMainWindow,
                          sizeof(BackdropMainWindow));
    // A one-pixel frame keeps the DWM shadow once the caption is removed.
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(window)
#endif
}

bool WindowChrome::handleNativeEvent(QWidget *window, void *message, qintptr *result,
                                     int resizeBorder, QRect captionArea,
                                     QRect maxButton,
                                     const QVector<QRect> &interactive)
{
#ifdef Q_OS_WIN
    auto *msg = static_cast<MSG *>(message);
    if (!window || !msg || !result)
        return false;
    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (!msg->wParam)
            return false;
        // The client area covers the whole window. A maximized window
        // overhangs the monitor by its frame, so pull that back in.
        if (IsZoomed(msg->hwnd)) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            const UINT dpi = GetDpiForWindow(msg->hwnd);
            const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            params->rgrc[0].left += frame;
            params->rgrc[0].top += frame;
            params->rgrc[0].right -= frame;
            params->rgrc[0].bottom -= frame;
        }
        *result = 0;
        return true;
    }
    case WM_NCHITTEST: {
        // The client area is the whole window (pulled in when maximized), so
        // client coordinates are the widget's own coordinates.
        POINT origin{0, 0};
        ClientToScreen(msg->hwnd, &origin);
        const qreal scale = window->devicePixelRatioF();
        const QPoint client(qRound((GET_X_LPARAM(msg->lParam) - origin.x) / scale),
                            qRound((GET_Y_LPARAM(msg->lParam) - origin.y) / scale));
        *result = toNative(hitTest(client, window->size(), resizeBorder, captionArea,
                                   maxButton, IsZoomed(msg->hwnd), interactive));
        return true;
    }
    case WM_NCLBUTTONDOWN:
        // Windows does not draw or run our maximize button; swallow the press
        // and toggle on release so Snap Layouts still see HTMAXBUTTON.
        if (msg->wParam == HTMAXBUTTON) {
            *result = 0;
            return true;
        }
        return false;
    case WM_NCLBUTTONUP:
        if (msg->wParam == HTMAXBUTTON) {
            if (window->isMaximized())
                window->showNormal();
            else
                window->showMaximized();
            *result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
#else
    Q_UNUSED(window) Q_UNUSED(message) Q_UNUSED(result) Q_UNUSED(resizeBorder)
    Q_UNUSED(captionArea) Q_UNUSED(maxButton) Q_UNUSED(interactive)
    return false;
#endif
}
