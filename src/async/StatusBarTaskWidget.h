#ifndef STATUSBARTASKWIDGET_H
#define STATUSBARTASKWIDGET_H

#include <QWidget>
#include <QPointer>
#include <QSet>
#include <QTimer>
class QToolButton;
class AsyncTaskPanelDialog;
class QFrame;

/**
 * @brief 状态栏任务 chip —— 活跃任务或未清理失败存在时显示。
 *
 * 设计（经验 1382463 教训：避免"每次点击 new 对话框"内存持续增长）：
 *   - AsyncTaskPanelDialog 为成员持有 unique_ptr，首次点击时创建，后续 show/raise 复用。
 */
class StatusBarTaskWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StatusBarTaskWidget(QWidget *parent = nullptr);
    ~StatusBarTaskWidget() override;

public slots:
    void onThreadCountChanged(int running, int maxThreads);
    void onSummaryChanged(int queued, int running, int finished);
    void showPanel();

private slots:
    void onTogglePanel();
    void checkLongRunningTasks();

private:
    QToolButton *m_tasksBtn;      // 点击弹出任务中心
    QScopedPointer<AsyncTaskPanelDialog> m_panel;
    int m_runningThreads = 0;
    int m_maxThreads = 0;
    int m_failedTasks = 0;
    QTimer m_estimateTimer;
    QSet<qint64> m_notifiedLongTasks;
    QPointer<QFrame> m_longTaskBubble;

    void showLongTaskBubble(qint64 taskId, const QString &title,
                            qint64 remainingMs, qint64 elapsedMs);
};

#endif // STATUSBARTASKWIDGET_H
