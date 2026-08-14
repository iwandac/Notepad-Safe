#include "StatusBarTaskWidget.h"
#include "AsyncTaskPanelDialog.h"
#include "AsyncTaskManager.h"
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QIcon>

StatusBarTaskWidget::StatusBarTaskWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 8, 0);
    lay->setSpacing(8);

    m_threadLabel = new QLabel(QStringLiteral("线程: -/-"), this);
    m_threadLabel->setObjectName(QLatin1String("StatusBarTaskWidget_thread"));
    m_threadLabel->setStyleSheet(QStringLiteral("color: palette(shadow);"));
    m_threadLabel->setToolTip(tr("AsyncThreadPool 活跃线程 / 并发上限"));

    m_tasksBtn = new QToolButton(this);
    m_tasksBtn->setObjectName(QLatin1String("StatusBarTaskWidget_btn"));
    m_tasksBtn->setText(tr("后台任务 0"));
    m_tasksBtn->setPopupMode(QToolButton::InstantPopup);
    m_tasksBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_tasksBtn->setCursor(Qt::PointingHandCursor);
    m_tasksBtn->setAutoRaise(true);
    connect(m_tasksBtn, &QToolButton::clicked, this, &StatusBarTaskWidget::onTogglePanel);

    lay->addWidget(m_threadLabel);
    lay->addWidget(m_tasksBtn);

    auto &mgr = AsyncTaskManager::instance();
    connect(&mgr, &AsyncTaskManager::threadCountChanged, this, &StatusBarTaskWidget::onThreadCountChanged);
    connect(&mgr, &AsyncTaskManager::summaryChanged, this, &StatusBarTaskWidget::onSummaryChanged);

    // 初始化默认值：先同步一次
    const int maxT = 0;  // 具体值等 threadCountChanged 信号 1s 内刷新即可
    Q_UNUSED(maxT);
}

StatusBarTaskWidget::~StatusBarTaskWidget() = default;

void StatusBarTaskWidget::onThreadCountChanged(int running, int maxThreads)
{
    m_threadLabel->setText(tr("线程: %1/%2").arg(running).arg(maxThreads));
}

void StatusBarTaskWidget::onSummaryChanged(int queued, int running, int finished)
{
    Q_UNUSED(finished);
    m_tasksBtn->setText(tr("后台任务 %1").arg(queued + running));
    if (queued + running == 0) {
        m_tasksBtn->setStyleSheet(QString());
    } else {
        m_tasksBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-weight: 600; }"
        ).arg(QStringLiteral("#7a5af8")));
    }
}

void StatusBarTaskWidget::onTogglePanel()
{
    if (!m_panel) {
        m_panel.reset(new AsyncTaskPanelDialog(window()));
    }
    if (m_panel->isVisible()) {
        m_panel->raise();
        m_panel->activateWindow();
    } else {
        m_panel->show();
        m_panel->raise();
        m_panel->activateWindow();
    }
}
