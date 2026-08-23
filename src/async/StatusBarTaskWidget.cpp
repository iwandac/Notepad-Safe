#include "StatusBarTaskWidget.h"
#include "AsyncTaskPanelDialog.h"
#include "AsyncTaskManager.h"
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QGuiApplication>

namespace {
QString compactDuration(qint64 milliseconds)
{
    if (milliseconds < 0)
        return QString();
    const qint64 seconds = qMax<qint64>(1, (milliseconds + 999) / 1000);
    if (seconds < 60)
        return QObject::tr("%1 秒").arg(seconds);
    const qint64 minutes = (seconds + 59) / 60;
    if (minutes < 60)
        return QObject::tr("%1 分钟").arg(minutes);
    const qint64 hours = minutes / 60;
    const qint64 rest = minutes % 60;
    return rest > 0 ? QObject::tr("%1 小时 %2 分钟").arg(hours).arg(rest)
                    : QObject::tr("%1 小时").arg(hours);
}
}

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

    m_estimateTimer.setInterval(1000);
    connect(&m_estimateTimer, &QTimer::timeout,
            this, &StatusBarTaskWidget::checkLongRunningTasks);
    m_estimateTimer.start();

    onThreadCountChanged(mgr.activeThreadCount(), mgr.maximumThreadCount());
    onSummaryChanged(mgr.queuedCount(), mgr.runningCount(), 0);
}

StatusBarTaskWidget::~StatusBarTaskWidget() = default;

void StatusBarTaskWidget::onThreadCountChanged(int running, int maxThreads)
{
    m_runningThreads = running;
    m_maxThreads = maxThreads;
    const int active = AsyncTaskManager::instance().cancellableCount();
    QString tooltip = tr("打开后台任务；活跃任务 %1，失败 %2，工作线程 %3/%4")
                          .arg(active).arg(m_failedTasks)
                          .arg(running).arg(maxThreads);
    if (m_longestRemainingMs >= 0)
        tooltip += tr("；最长预计还需 %1")
                       .arg(compactDuration(m_longestRemainingMs));
    m_tasksBtn->setToolTip(tooltip);
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

void StatusBarTaskWidget::checkLongRunningTasks()
{
    auto &manager = AsyncTaskManager::instance();
    qint64 longestRemaining = -1;
    for (int row = 0; row < manager.rowCount(); ++row) {
        const QModelIndex item = manager.index(row, 0);
        if (item.data(AsyncTaskManager::TaskStateRole).toInt()
            != static_cast<int>(AsyncTaskState::Running)) {
            continue;
        }
        const qint64 taskId = item.data(AsyncTaskManager::TaskIdRole).toLongLong();
        const qint64 elapsed = item.data(AsyncTaskManager::TaskElapsedMsRole).toLongLong();
        const qint64 remaining =
            item.data(AsyncTaskManager::TaskEstimatedRemainingMsRole).toLongLong();
        if (remaining > longestRemaining)
            longestRemaining = remaining;

        // 至少观察 3 秒再预测，避免启动阶段的缓存抖动制造误报。
        const bool predictedLong = elapsed >= 3000 && remaining >= 0
            && elapsed + remaining >= 30000;
        const bool provenLongWithoutEstimate = elapsed >= 30000 && remaining < 0;
        if ((!predictedLong && !provenLongWithoutEstimate)
            || m_notifiedLongTasks.contains(taskId)) {
            continue;
        }
        m_notifiedLongTasks.insert(taskId);
        showLongTaskBubble(taskId,
            item.data(AsyncTaskManager::TaskTitleRole).toString(),
            remaining, elapsed);
        break;
    }

    m_longestRemainingMs = longestRemaining;
    onThreadCountChanged(m_runningThreads, m_maxThreads);
}

void StatusBarTaskWidget::showLongTaskBubble(qint64 taskId,
                                             const QString &title,
                                             qint64 remainingMs,
                                             qint64 elapsedMs)
{
    Q_UNUSED(taskId);
    if (!m_tasksBtn->isVisible() || m_longTaskBubble)
        return;

    auto *bubble = new QFrame(window(), Qt::Popup | Qt::FramelessWindowHint);
    bubble->setObjectName(QStringLiteral("longTaskNotification"));
    bubble->setAttribute(Qt::WA_DeleteOnClose);
    bubble->setFrameShape(QFrame::StyledPanel);
    bubble->setStyleSheet(QStringLiteral(
        "QFrame#longTaskNotification{background:palette(window);"
        "border:1px solid palette(mid);border-radius:8px;}"));
    auto *layout = new QVBoxLayout(bubble);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    auto *heading = new QLabel(tr("后台任务需要较长时间"), bubble);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    const QString timing = remainingMs >= 0
        ? tr("%1\n预计还需 %2").arg(title, compactDuration(remainingMs))
        : tr("%1\n已运行 %2").arg(title, compactDuration(elapsedMs));
    auto *message = new QLabel(timing, bubble);
    message->setWordWrap(true);
    message->setMaximumWidth(320);
    layout->addWidget(message);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    auto *view = new QPushButton(tr("查看后台任务"), bubble);
    view->setObjectName(QStringLiteral("viewLongTaskButton"));
    actions->addWidget(view);
    layout->addLayout(actions);
    connect(view, &QPushButton::clicked, this, [this, bubble]() {
        bubble->close();
        showPanel();
    });
    connect(bubble, &QObject::destroyed, this, [this]() {
        m_longTaskBubble = nullptr;
    });

    bubble->adjustSize();
    const QPoint anchor = m_tasksBtn->mapToGlobal(QPoint(m_tasksBtn->width(), 0));
    QRect available;
    if (QScreen *screen = QGuiApplication::screenAt(anchor))
        available = screen->availableGeometry();
    const int x = available.isValid()
        ? qBound(available.left(), anchor.x() - bubble->width(),
                 available.right() - bubble->width() + 1)
        : anchor.x() - bubble->width();
    const int y = available.isValid()
        ? qMax(available.top(), anchor.y() - bubble->height() - 8)
        : anchor.y() - bubble->height() - 8;
    bubble->move(x, y);
    m_longTaskBubble = bubble;
    bubble->show();
    bubble->raise();
}
