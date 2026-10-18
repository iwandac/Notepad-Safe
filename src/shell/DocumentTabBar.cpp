#include "DocumentTabBar.h"

#include "ShellGlyphs.h"

#include <QAbstractButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <utility>

TabPaintSpec tabPaintSpec(const TabVisualState &s, bool active, bool hovered,
                          const ThemeTokens &t)
{
    TabPaintSpec spec;
    spec.background = active ? t.paper : QColor(Qt::transparent);
    spec.text = active ? t.ink : t.muted;
    spec.fontWeight = active ? 600 : 400;
    const QColor security = s.secureLocked ? t.seal : t.exposed;
    switch (s.kind) {
    case TabVisualState::Kind::Secure:
        spec.icon = security;
        break;
    case TabVisualState::Kind::ReadOnly:
        spec.icon = t.muted;
        break;
    case TabVisualState::Kind::Recovery:
        spec.icon = t.danger;
        break;
    case TabVisualState::Kind::Plain:
        spec.icon = QColor(Qt::transparent);
        break;
    }
    spec.spine = s.kind == TabVisualState::Kind::Secure && active
        ? security : QColor(Qt::transparent);
    spec.showDirtyDot = s.dirty && !hovered;
    spec.showClose = !s.dirty || hovered;
    return spec;
}

namespace {
constexpr int IconWidth = 18;
constexpr int SidePadding = 12;
constexpr int CloseReserve = 26;

// Painted glyphs, not an icon font (font fallback lookup is slow).
bool iconGlyph(TabVisualState::Kind kind, ShellGlyph *glyph)
{
    switch (kind) {
    case TabVisualState::Kind::Secure: *glyph = ShellGlyph::Lock; return true;
    case TabVisualState::Kind::ReadOnly: *glyph = ShellGlyph::Eye; return true;
    case TabVisualState::Kind::Recovery: *glyph = ShellGlyph::Warning; return true;
    case TabVisualState::Kind::Plain: break;
    }
    return false;
}

// QTabBar's own close buttons; recovery tabs install none.
bool isDefaultCloseButton(const QWidget *button)
{
    return button && qstrcmp(button->metaObject()->className(), "CloseButton") == 0;
}
} // namespace

DocumentTabBar::DocumentTabBar(QWidget *parent) : QTabBar(parent)
{
    setObjectName(QStringLiteral("documentTabBar"));
    setDrawBase(false);
    setExpanding(false);
    setElideMode(Qt::ElideMiddle);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
}

void DocumentTabBar::setTokens(const ThemeTokens &tokens)
{
    m_tokens = tokens;
    updateGeometry();
    refresh();
}

void DocumentTabBar::setStateProvider(StateProvider provider)
{
    m_provider = std::move(provider);
    refresh();
}

void DocumentTabBar::refresh()
{
    updateCloseButtons();
    update();
}

TabVisualState DocumentTabBar::stateFor(int index) const
{
    return m_provider ? m_provider(index) : TabVisualState{};
}

QSize DocumentTabBar::tabSizeHint(int index) const
{
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(bold);
    const bool hasIcon = stateFor(index).kind != TabVisualState::Kind::Plain;
    const int width = SidePadding + (hasIcon ? IconWidth : 0)
        + metrics.horizontalAdvance(tabText(index)) + CloseReserve + SidePadding / 2;
    return QSize(qBound(96, width, 240), m_tokens.tab);
}

void DocumentTabBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), m_tokens.chrome);
    painter.fillRect(QRect(0, height() - 1, width(), 1), m_tokens.lineSoft);

    for (int i = 0; i < count(); ++i) {
        const QRect tab = tabRect(i);
        if (!tab.isValid() || !tab.intersects(rect()))
            continue;
        const TabVisualState state = stateFor(i);
        const bool active = i == currentIndex();
        const TabPaintSpec spec = tabPaintSpec(state, active, i == m_hovered, m_tokens);

        const QRectF body = QRectF(tab).adjusted(2, 4, -2, 0);
        if (spec.background.alpha() > 0 || i == m_hovered) {
            const qreal r = m_tokens.radiusTab;
            QPainterPath path;
            path.moveTo(body.bottomLeft());
            path.lineTo(body.left(), body.top() + r);
            path.quadTo(body.topLeft(), QPointF(body.left() + r, body.top()));
            path.lineTo(body.right() - r, body.top());
            path.quadTo(body.topRight(), QPointF(body.right(), body.top() + r));
            path.lineTo(body.bottomRight());
            path.closeSubpath();
            painter.fillPath(path, spec.background.alpha() > 0 ? spec.background
                                                               : m_tokens.hover);
        }
        if (spec.spine.alpha() > 0) {
            painter.fillRect(QRectF(body.left() + m_tokens.radiusTab / 2.0, body.top(),
                                    body.width() - m_tokens.radiusTab, 2),
                             spec.spine);
        }

        int x = tab.left() + SidePadding;
        ShellGlyph glyph;
        if (iconGlyph(state.kind, &glyph)) {
            paintShellGlyph(&painter, glyph,
                            QRectF(x - 1, tab.center().y() - 6, 14, 14), spec.icon);
            x += IconWidth;
        }

        QFont textFont = font();
        textFont.setWeight(QFont::Weight(spec.fontWeight));
        painter.setFont(textFont);
        painter.setPen(spec.text);
        const QRect textRect(x, tab.top() + 2, tab.right() - CloseReserve - x, tab.height());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(textFont).elidedText(tabText(i), elideMode(),
                                                           textRect.width()));

        if (spec.showDirtyDot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(spec.text);
            const QPointF center(tab.right() - CloseReserve / 2.0, tab.center().y() + 2);
            painter.drawEllipse(center, 3.5, 3.5);
        }
    }
}

void DocumentTabBar::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = tabAt(event->position().toPoint());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        refresh();
    }
    QTabBar::mouseMoveEvent(event);
}

void DocumentTabBar::leaveEvent(QEvent *event)
{
    if (m_hovered != -1) {
        m_hovered = -1;
        refresh();
    }
    QTabBar::leaveEvent(event);
}

void DocumentTabBar::tabInserted(int index)
{
    QTabBar::tabInserted(index);
    // Close buttons are created after insertion.
    QTimer::singleShot(0, this, &DocumentTabBar::refresh);
}

void DocumentTabBar::updateCloseButtons()
{
    for (int i = 0; i < count(); ++i) {
        QWidget *button = tabButton(i, QTabBar::RightSide);
        if (!isDefaultCloseButton(button))
            continue;
        const TabPaintSpec spec = tabPaintSpec(stateFor(i), i == currentIndex(),
                                               i == m_hovered, m_tokens);
        button->setVisible(spec.showClose);
    }
}

DocumentTabWidget::DocumentTabWidget(QWidget *parent) : QTabWidget(parent)
{
    m_bar = new DocumentTabBar(this);
    setTabBar(m_bar);
    connect(m_bar, &QTabBar::currentChanged, m_bar, &DocumentTabBar::refresh);
}
