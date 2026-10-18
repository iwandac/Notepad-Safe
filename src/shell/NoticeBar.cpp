#include "NoticeBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

NoticeBar::NoticeBar(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("noticeBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 6, 8, 6);
    layout->setSpacing(8);

    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("noticeText"));
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    layout->addWidget(m_text, 1);

    m_actions = new QHBoxLayout;
    m_actions->setSpacing(6);
    layout->addLayout(m_actions);

    m_dismiss = new QToolButton(this);
    m_dismiss->setObjectName(QStringLiteral("noticeDismiss"));
    m_dismiss->setText(QStringLiteral("✕"));
    m_dismiss->setToolTip(QStringLiteral("关闭提示"));
    m_dismiss->setAccessibleName(QStringLiteral("关闭提示"));
    m_dismiss->setAutoRaise(true);
    connect(m_dismiss, &QToolButton::clicked, this, &NoticeBar::dismiss);
    layout->addWidget(m_dismiss);

    m_autoHide.setSingleShot(true);
    connect(&m_autoHide, &QTimer::timeout, this, &NoticeBar::dismiss);
    hide();
    applyStyle();
}

void NoticeBar::setTokens(const ThemeTokens &tokens, bool styled)
{
    m_tokens = tokens;
    m_styled = styled;
    applyStyle();
}

void NoticeBar::showNotice(NoticeKind kind, const QString &text,
                           QVector<NoticeAction> actions, int autoHideMs)
{
    m_autoHide.stop();
    while (QLayoutItem *item = m_actions->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_kind = kind;
    m_text->setText(text);
    setAccessibleName(text);
    for (int i = 0; i < actions.size() && i < 2; ++i) {
        auto *button = new QPushButton(actions.at(i).label, this);
        button->setObjectName(QStringLiteral("noticeAction%1").arg(i));
        const std::function<void()> run = actions.at(i).run;
        // Hide first: the action may show a newer notice of its own.
        connect(button, &QPushButton::clicked, this, [this, run] {
            dismiss();
            if (run)
                run();
        });
        m_actions->addWidget(button);
    }
    applyStyle();
    show();
    if (autoHideMs > 0)
        m_autoHide.start(autoHideMs);
}

void NoticeBar::dismiss()
{
    m_autoHide.stop();
    hide();
}

void NoticeBar::applyStyle()
{
    if (!m_styled) {
        setStyleSheet(QString());
        return;
    }
    const ThemeTokens &t = m_tokens;
    const bool error = m_kind == NoticeKind::Error;
    setStyleSheet(QStringLiteral(
        "#noticeBar { background:%1; border-bottom:1px solid %2; }"
        "#noticeText { color:%3; }"
        "#noticeDismiss { color:%4; border:0; padding:2px 6px; }")
        .arg((error ? t.dangerSoft : t.selection).name(), t.lineSoft.name(),
             (error ? t.danger : t.ink).name(), t.muted.name()));
}
