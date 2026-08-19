#include "StatusBarTaskWidget.h"
#include "AsyncTaskPanelDialog.h"
#include "AsyncTaskManager.h"
#include <QToolButton>
#include <QHBoxLayout>
#include <QIcon>

StatusBarTaskWidget::StatusBarTaskWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 8, 0);
    lay->setSpacing(8);

    m_tasksBtn = new QToolButton(this);
    m_tasksBtn->setObjectName(QLatin1String("StatusBarTaskWidget_btn"));
    m_tasksBtn->setText(tr("后台任务 0"));
    m_tasksBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_tasksBtn->setCursor(Qt::PointingHandCursor);
    m_tasksBtn->setAutoRaise(true);
    m_tasksBtn->setAccessibleName(tr("后台任务"));
    connect(m_tasksBtn, &QToolButton::clicked, this, &StatusBarTaskWidget::onTogglePanel);

    lay->addWidget(m_tasksBtn);
    m_tasksBtn->hide();

    auto &mgr = AsyncTaskManager::instance();
    connect(&mgr, &AsyncTaskManager::threadCountChanged, this, &StatusBarTaskWidget::onThreadCountChanged);
    connect(&mgr, &AsyncTaskManager::summaryChanged, this, &StatusBarTaskWidget::onSummaryChanged);

    onThreadCountChanged(mgr.activeThreadCount(), mgr.maximumThreadCount());
    onSummaryChanged(mgr.queuedCount(), mgr.runningCount(), 0);
}

StatusBarTaskWidget::~StatusBarTaskWidget() = default;

void StatusBarTaskWidget::onThreadCountChanged(int running, int maxThreads)
{
    m_runningThreads = running;
    m_maxThreads = maxThreads;
    const int active = AsyncTaskManager::instance().cancellableCount();
    m_tasksBtn->setToolTip(tr("打开后台任务；活跃任务 %1，失败 %2，工作线程 %3/%4")
                               .arg(active).arg(m_failedTasks)
                               .arg(running).arg(maxThreads));
}

void StatusBarTaskWidget::onSummaryChanged(int queued, int running, int finished)
{
    Q_UNUSED(finished);
    const int active = queued + running;
    m_failedTasks = AsyncTaskManager::instance().failedCount();
    if (active > 0 && m_failedTasks > 0) {
        m_tasksBtn->setText(tr("后台任务 %1 · 失败 %2").arg(active).arg(m_failedTasks));
    } else if (active > 0) {
        m_tasksBtn->setText(tr("后台任务 %1").arg(active));
    } else {
        m_tasksBtn->setText(tr("任务失败 %1").arg(m_failedTasks));
    }
    m_tasksBtn->setVisible(active > 0 || m_failedTasks > 0);
    if (m_failedTasks > 0) {
        m_tasksBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: #d0443e; font-weight: 600; }"));
        m_tasksBtn->setAccessibleDescription(
            tr("有 %1 个失败任务，打开任务中心可查看详情").arg(m_failedTasks));
    } else if (active == 0) {
        m_tasksBtn->setStyleSheet(QString());
        m_tasksBtn->setAccessibleDescription(QString());
    } else {
        m_tasksBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; font-weight: 600; }"
        ).arg(QStringLiteral("#7a5af8")));
        m_tasksBtn->setAccessibleDescription(
            tr("有 %1 个后台任务正在排队或运行").arg(active));
    }
    onThreadCountChanged(m_runningThreads, m_maxThreads);
}

void StatusBarTaskWidget::onTogglePanel()
{
    showPanel();
}

void StatusBarTaskWidget::showPanel()
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
