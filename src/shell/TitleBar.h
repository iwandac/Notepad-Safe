#pragma once

#include "ThemeTokens.h"

#include <QDateTime>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

enum class SecurityPillState { Hidden, Unlocked, ExpiringToday, Locked };

struct SecurityPillView {
    SecurityPillState state = SecurityPillState::Hidden;
    QString text;
    QString toolTip;
};

// What the title bar's security pill says for the current session. No
// identity hides the pill; times are local.
SecurityPillView securityPillView(bool hasIdentity, bool active,
                                  const QDateTime &expiresLocal,
                                  const QDateTime &nowLocal);

// The 40 px title bar of the frameless window: main menu button, document
// title, command center, security pill and the three window buttons.
class TitleBar final : public QWidget {
    Q_OBJECT

public:
    explicit TitleBar(QWidget *window, QWidget *parent = nullptr);

    // Without styling (Windows high contrast) the bar uses the platform palette.
    void setTokens(const ThemeTokens &tokens, bool styled = true);
    void setSecurityPill(const SecurityPillView &view);

    // Rects in window coordinates for WindowChrome::hitTest.
    QRect captionRect() const;
    QRect maxButtonRect() const;
    QVector<QRect> interactiveRects() const;

    QToolButton *menuButton() const { return m_menuButton; }

signals:
    void mainMenuRequested();
    void commandPaletteRequested();
    void securityPillClicked();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateWindowButtons();
    void applyStyle();
    QRect toWindow(const QWidget *child) const;

    QWidget *m_window = nullptr;
    ThemeTokens m_tokens = ThemeTokens::light();
    bool m_styled = true;
    SecurityPillView m_pill;
    QToolButton *m_menuButton = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_commandCenter = nullptr;
    QToolButton *m_securityPill = nullptr;
    QToolButton *m_minimize = nullptr;
    QToolButton *m_maximize = nullptr;
    QToolButton *m_close = nullptr;
};
