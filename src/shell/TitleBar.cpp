#include "TitleBar.h"

#include "ShellGlyphs.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>

SecurityPillView securityPillView(bool hasIdentity, bool active,
                                  const QDateTime &expiresLocal,
                                  const QDateTime &nowLocal)
{
    if (!hasIdentity)
        return {};
    if (!active)
        return {SecurityPillState::Locked, QStringLiteral("已锁定"),
                QStringLiteral("安全会话已锁定，点击打开安全中心")};
    const QString tip = QStringLiteral("本设备会话有效至 %1，点击立即锁定")
                            .arg(expiresLocal.toString(QStringLiteral("MM-dd HH:mm")));
    if (expiresLocal.date() == nowLocal.date())
        return {SecurityPillState::ExpiringToday,
                QStringLiteral("今天 %1 到期").arg(expiresLocal.toString(QStringLiteral("HH:mm"))),
                tip};
    return {SecurityPillState::Unlocked, QStringLiteral("已解锁"), tip};
}

namespace {
QToolButton *windowButton(const QString &objectName, const QString &tip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIconSize(QSize(10, 10));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setFixedSize(46, 40);
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    return button;
}

QString hex(const QColor &color)
{
    return color.name(QColor::HexRgb);
}
} // namespace

TitleBar::TitleBar(QWidget *window, QWidget *parent)
    : QWidget(parent), m_window(window)
{
    setObjectName(QStringLiteral("shellTitleBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(m_tokens.titleBar);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 0, 0);
    layout->setSpacing(4);

    m_menuButton = new QToolButton(this);
    m_menuButton->setObjectName(QStringLiteral("shellMainMenuButton"));
    m_menuButton->setIconSize(QSize(14, 14));
    m_menuButton->setToolTip(QStringLiteral("主菜单 (Alt)"));
    m_menuButton->setAccessibleName(QStringLiteral("主菜单"));
    m_menuButton->setFixedSize(m_tokens.control + 4, m_tokens.control);
    m_menuButton->setFocusPolicy(Qt::NoFocus);
    m_menuButton->setAutoRaise(true);
    connect(m_menuButton, &QToolButton::clicked, this, &TitleBar::mainMenuRequested);
    layout->addWidget(m_menuButton);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("shellWindowTitle"));
    m_title->setTextFormat(Qt::PlainText);
    m_title->setMinimumWidth(80);
    layout->addWidget(m_title, 1);

    m_commandCenter = new QPushButton(QStringLiteral("搜索或运行命令    Ctrl+K"), this);
    m_commandCenter->setObjectName(QStringLiteral("shellCommandCenter"));
    m_commandCenter->setAccessibleName(QStringLiteral("命令面板"));
    m_commandCenter->setFixedHeight(m_tokens.control);
    m_commandCenter->setMinimumWidth(240);
    m_commandCenter->setMaximumWidth(420);
    m_commandCenter->setFocusPolicy(Qt::NoFocus);
    connect(m_commandCenter, &QPushButton::clicked, this, &TitleBar::commandPaletteRequested);
    layout->addWidget(m_commandCenter, 2);
    layout->addStretch(1);

    m_securityPill = new QToolButton(this);
    m_securityPill->setObjectName(QStringLiteral("shellSecurityPill"));
    m_securityPill->setFixedHeight(m_tokens.control);
    m_securityPill->setFocusPolicy(Qt::NoFocus);
    m_securityPill->hide();
    connect(m_securityPill, &QToolButton::clicked, this, &TitleBar::securityPillClicked);
    layout->addWidget(m_securityPill);
    layout->addSpacing(6);

    m_minimize = windowButton(QStringLiteral("shellMinimizeButton"),
                              QStringLiteral("最小化"), this);
    m_maximize = windowButton(QStringLiteral("shellMaximizeButton"),
                              QStringLiteral("最大化"), this);
    m_close = windowButton(QStringLiteral("shellCloseButton"), QStringLiteral("关闭"), this);
    connect(m_minimize, &QToolButton::clicked, this, [this] {
        if (m_window) m_window->showMinimized();
    });
    connect(m_maximize, &QToolButton::clicked, this, [this] {
        if (!m_window) return;
        if (m_window->isMaximized()) m_window->showNormal();
        else m_window->showMaximized();
    });
    connect(m_close, &QToolButton::clicked, this, [this] {
        if (m_window) m_window->close();
    });
    layout->addWidget(m_minimize);
    layout->addWidget(m_maximize);
    layout->addWidget(m_close);

    if (m_window) {
        m_title->setText(m_window->windowTitle());
        connect(m_window, &QWidget::windowTitleChanged, m_title, &QLabel::setText);
        m_window->installEventFilter(this);
    }
    updateWindowButtons();
    applyStyle();
}

void TitleBar::setTokens(const ThemeTokens &tokens, bool styled)
{
    m_tokens = tokens;
    m_styled = styled;
    applyStyle();
}

void TitleBar::setSecurityPill(const SecurityPillView &view)
{
    m_pill = view;
    m_securityPill->setVisible(view.state != SecurityPillState::Hidden);
    m_securityPill->setText(view.text);
    m_securityPill->setToolTip(view.toolTip);
    m_securityPill->setAccessibleName(view.text);
    applyStyle();
}

QRect TitleBar::toWindow(const QWidget *child) const
{
    if (!child || !child->isVisible() || !m_window)
        return {};
    return QRect(child->mapTo(m_window, QPoint(0, 0)), child->size());
}

QRect TitleBar::captionRect() const
{
    if (!m_window)
        return {};
    return QRect(mapTo(m_window, QPoint(0, 0)), size());
}

QRect TitleBar::maxButtonRect() const
{
    return toWindow(m_maximize);
}

QVector<QRect> TitleBar::interactiveRects() const
{
    return {toWindow(m_menuButton), toWindow(m_commandCenter), toWindow(m_securityPill),
            toWindow(m_minimize), toWindow(m_close)};
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Windows handles caption double-clicks through HTCAPTION; this covers
    // platforms and test runs without the native hit test.
    if (event->button() == Qt::LeftButton && m_window) {
        if (m_window->isMaximized()) m_window->showNormal();
        else m_window->showMaximized();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TitleBar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange)
        applyStyle();
    QWidget::changeEvent(event);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && (event->type() == QEvent::WindowStateChange
                                || event->type() == QEvent::ActivationChange)) {
        updateWindowButtons();
        applyStyle();
    }
    return QWidget::eventFilter(watched, event);
}

void TitleBar::updateWindowButtons()
{
    const bool maximized = m_window && m_window->isMaximized();
    updateIcons();
    m_maximize->setToolTip(maximized ? QStringLiteral("向下还原") : QStringLiteral("最大化"));
    m_maximize->setAccessibleName(m_maximize->toolTip());
}

void TitleBar::updateIcons()
{
    // Painted glyphs, not an icon font (font fallback lookup is slow).
    const bool active = !m_window || m_window->isActiveWindow();
    const QColor glyph = !m_styled ? palette().color(QPalette::ButtonText)
        : active ? m_tokens.ink : m_tokens.faint;
    const bool maximized = m_window && m_window->isMaximized();
    m_menuButton->setIcon(shellGlyphIcon(ShellGlyph::Menu, glyph, glyph, 14));
    m_minimize->setIcon(shellGlyphIcon(ShellGlyph::Minimize, glyph, glyph, 10));
    m_maximize->setIcon(shellGlyphIcon(maximized ? ShellGlyph::Restore : ShellGlyph::Maximize,
                                       glyph, glyph, 10));
    m_close->setIcon(shellGlyphIcon(ShellGlyph::Close, glyph,
                                    m_styled ? QColor(Qt::white) : glyph, 10));
}

void TitleBar::applyStyle()
{
    updateIcons();
    if (!m_styled) {
        setStyleSheet(QString());
        return;
    }
    const ThemeTokens &t = m_tokens;
    const bool active = !m_window || m_window->isActiveWindow();
    const bool exposed = m_pill.state == SecurityPillState::Unlocked
        || m_pill.state == SecurityPillState::ExpiringToday;
    const QColor pillInk = exposed ? t.exposed : t.seal;
    const QColor pillBack = exposed ? t.exposedSoft : t.sealSoft;
    setStyleSheet(QStringLiteral(
        "#shellTitleBar { background:%1; border-bottom:1px solid %2; }"
        "#shellWindowTitle { color:%3; font-size:9pt; padding-left:6px; }"
        "#shellMainMenuButton { color:%4; border:0; border-radius:%5px; }"
        "#shellMainMenuButton:hover { background:%6; }"
        "#shellCommandCenter { color:%7; background:%8; border:1px solid %2;"
        " border-radius:%5px; text-align:left; padding:0 10px; }"
        "#shellCommandCenter:hover { background:%6; color:%4; }"
        "#shellSecurityPill { color:%9; background:%10; border:0; border-radius:%11px;"
        " padding:0 12px; font-weight:600; }"
        "#shellMinimizeButton, #shellMaximizeButton, #shellCloseButton { color:%4;"
        " border:0; border-radius:0; }"
        "#shellMinimizeButton:hover, #shellMaximizeButton:hover { background:%6; }"
        "#shellCloseButton:hover { background:%12; color:#FFFFFF; }")
        .arg(hex(t.chrome), hex(t.lineSoft), hex(active ? t.muted : t.faint),
             hex(active ? t.ink : t.faint))
        .arg(t.radiusControl)
        .arg(hex(t.hover), hex(t.faint), hex(t.paper), hex(pillInk), hex(pillBack))
        .arg(t.radiusPill)
        .arg(hex(t.danger)));
}
