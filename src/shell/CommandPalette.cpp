#include "CommandPalette.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int PaletteWidth = 620;
constexpr int TopOffset = 64;
constexpr int RowHeight = 44;
constexpr int VisibleRows = 9;

QString groupName(int group)
{
    switch (PaletteEntry::Group(group)) {
    case PaletteEntry::Group::OpenDocument: return QStringLiteral("已打开的文档");
    case PaletteEntry::Group::Command: return QStringLiteral("命令");
    case PaletteEntry::Group::Recent: return QStringLiteral("最近打开");
    }
    return {};
}

// Title with an optional muted subtitle on the left; shortcut or group name
// on the right.
class PaletteDelegate final : public QStyledItemDelegate {
public:
    explicit PaletteDelegate(const ThemeTokens *tokens, QObject *parent)
        : QStyledItemDelegate(parent), m_tokens(tokens) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(option.rect.width(), RowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const ThemeTokens &t = *m_tokens;
        const bool enabled = index.data(CommandPaletteModel::EnabledRole).toBool();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF row = QRectF(option.rect).adjusted(6, 2, -6, -2);
        if (selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.selection);
            painter->drawRoundedRect(row, t.radiusControl, t.radiusControl);
        }
        const QRect content = option.rect.adjusted(18, 0, -18, 0);
        const QString right = index.data(CommandPaletteModel::ShortcutRole).toString().isEmpty()
            ? groupName(index.data(CommandPaletteModel::GroupRole).toInt())
            : index.data(CommandPaletteModel::ShortcutRole).toString();
        QFont small = option.font;
        small.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1));
        painter->setFont(small);
        painter->setPen(t.faint);
        const int rightWidth = QFontMetrics(small).horizontalAdvance(right) + 4;
        painter->drawText(content, Qt::AlignVCenter | Qt::AlignRight, right);

        const QRect left = content.adjusted(0, 0, -rightWidth - 12, 0);
        const QString subtitle = index.data(CommandPaletteModel::SubtitleRole).toString();
        painter->setFont(option.font);
        painter->setPen(enabled ? t.ink : t.faint);
        const QFontMetrics metrics(option.font);
        const QString title = metrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, left.width());
        painter->drawText(left, Qt::AlignVCenter | Qt::AlignLeft, title);
        if (!subtitle.isEmpty()) {
            const int used = metrics.horizontalAdvance(title) + 10;
            painter->setFont(small);
            painter->setPen(t.muted);
            painter->drawText(left.adjusted(used, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics(small).elidedText(subtitle, Qt::ElideMiddle,
                                                             qMax(0, left.width() - used)));
        }
        painter->restore();
    }

private:
    const ThemeTokens *m_tokens;
};
} // namespace

CommandPalette::CommandPalette(QWidget *window)
    : QFrame(window, Qt::Popup | Qt::FramelessWindowHint), m_window(window)
{
    setObjectName(QStringLiteral("commandPalette"));
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedWidth(PaletteWidth);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_query = new QLineEdit(this);
    m_query->setObjectName(QStringLiteral("commandPaletteQuery"));
    m_query->setPlaceholderText(QStringLiteral("搜索文档、命令或最近打开的文件"));
    m_query->setAccessibleName(QStringLiteral("命令面板搜索"));
    m_query->setMinimumHeight(m_tokens.primaryButton);
    m_query->installEventFilter(this);
    layout->addWidget(m_query);

    m_model = new CommandPaletteModel(this);
    m_list = new QListView(this);
    m_list->setObjectName(QStringLiteral("commandPaletteList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new PaletteDelegate(&m_tokens, m_list));
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setFixedHeight(RowHeight * VisibleRows + 4);
    layout->addWidget(m_list);

    connect(m_query, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_model->setQuery(text);
        m_list->setCurrentIndex(m_model->index(0));
    });
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
        m_list->setCurrentIndex(index);
        activateCurrent();
    });
    applyStyle();
}

void CommandPalette::setTokens(const ThemeTokens &tokens)
{
    m_tokens = tokens;
    applyStyle();
}

void CommandPalette::open(QVector<PaletteEntry> entries)
{
    m_model->setEntries(std::move(entries));
    m_query->clear();
    m_model->setQuery({});
    m_list->setCurrentIndex(m_model->index(0));
    if (m_window) {
        const QPoint top = m_window->mapToGlobal(QPoint(0, 0));
        move(top.x() + (m_window->width() - width()) / 2, top.y() + TopOffset);
    }
    show();
    raise();
    m_query->setFocus(Qt::PopupFocusReason);
}

void CommandPalette::activateCurrent()
{
    const int row = m_list->currentIndex().row();
    if (row < 0 || !m_model->index(row).data(CommandPaletteModel::EnabledRole).toBool())
        return;
    // Close first so the command runs with focus back in the window.
    hide();
    m_model->activate(row);
}

bool CommandPalette::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int rows = m_model->rowCount();
        int row = m_list->currentIndex().row();
        switch (key->key()) {
        case Qt::Key_Down:
            if (rows > 0) m_list->setCurrentIndex(m_model->index((row + 1) % rows));
            return true;
        case Qt::Key_Up:
            if (rows > 0) m_list->setCurrentIndex(m_model->index((row - 1 + rows) % rows));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activateCurrent();
            return true;
        case Qt::Key_Escape:
            hide();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void CommandPalette::applyStyle()
{
    const ThemeTokens &t = m_tokens;
    setStyleSheet(QStringLiteral(
        "#commandPalette { background:%1; border:1px solid %2; border-radius:%3px; }"
        "#commandPaletteQuery { background:%4; color:%5; border:1px solid %2;"
        " border-radius:%6px; padding:0 10px; font-size:11pt; }"
        "#commandPaletteQuery:focus { border-color:%7; }"
        "#commandPaletteList { background:transparent; border:0; outline:0; }")
        .arg(t.raised.name(), t.line.name())
        .arg(t.radiusPalette)
        .arg(t.paper.name(), t.ink.name())
        .arg(t.radiusControl)
        .arg(t.focus.name()));
}
