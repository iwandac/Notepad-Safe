#ifndef ASYNCTASKMANAGER_H
#define ASYNCTASKMANAGER_H

#include <QAbstractListModel>
#include <QTimer>
#include <QMutex>
#include <QList>
#include <functional>
#include "AsyncTask.h"

class AsyncTaskManager : public QAbstractListModel
{
    Q_OBJECT
public:
    static AsyncTaskManager& instance();

    enum Roles {
        TaskIdRole = Qt::UserRole + 1,
        TaskBatchIdRole,
        TaskTypeRole,
        TaskPriorityRole,
        TaskStateRole,
        TaskTitleRole,
        TaskDescriptionRole,
        TaskProgressDoneRole,
        TaskProgressTotalRole,
        TaskPercentRole,
        TaskCreatedAtRole,
        TaskStartedAtRole,
        TaskFinishedAtRole,
        TaskElapsedMsRole,
        TaskErrorRole,
        TaskCategoryTagRole,
        TaskStateTextRole,
        TaskPriorityTextRole,
        TaskTypeTextRole
    };

    // ========= 基本查询 =========
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    AsyncTask taskById(qint64 id) const;
    int runningCount() const;   // 状态==Running 的任务数
    int queuedCount() const;    // 状态==Queued
    int errorCount() const;     // 失败/已取消 的最近 100 条内
    int activeThreadCount();    // 池内活跃线程

    // ========= 提交 / 管理 =========
    using WorkerFn = std::function<void(
        qint64 taskId,
        AsyncCancelPtr cancel,
        std::function<void(qint64 done, qint64 total)> reportProgress,
        std::function<void(const AsyncErrorInfo& err)> reportError
    )>;

    /**
     * @brief 提交异步任务：立即返回，永不阻塞主线程。
     *        workerFn 在 AsyncThreadPool 的 worker 线程里执行，里面已 try/catch 包裹异常落 error。
     * @return taskId（全局唯一）
     */
    qint64 submit(AsyncTaskType type,
                  AsyncTaskPriority priority,
                  const QString &title,
                  const QString &description = QString(),
                  WorkerFn workerFn = WorkerFn(),
                  qint64 batchId = 0,
                  const QString &categoryTag = QString(),
                  qint64 progressTotalInit = -1);

    /** 取消单个任务（设置 cancel 原子 + 改为 Canceled 状态） */
    Q_INVOKABLE void cancel(qint64 taskId);

    /** 公开进度更新（主线程调用，同步写入 tasks_/history_） */
    Q_INVOKABLE void updateTaskProgress(qint64 taskId, qint64 done, qint64 total);

    /** 批量取消：按类型；若 type==Unknown 则取消当前所有 Queued/Running */
    Q_INVOKABLE void cancelAllByType(AsyncTaskType type = AsyncTaskType::Unknown);

    /** 清空所有 Finished/Failed/Canceled 历史（最多 100 条） */
    Q_INVOKABLE void clearHistory();

signals:
    void taskAdded(qint64 taskId);
    void taskProgressChanged(qint64 taskId, qint64 done, qint64 total);
    void taskFinished(qint64 taskId, AsyncTaskState finalState);  // Finished / Failed / Canceled
    void threadCountChanged(int running, int maxThreads);         // 每秒推送：当前活跃线程/并发上限
    void summaryChanged(int queued, int running, int finishedRecent);

private slots:
    void onTickStats();  // 1s 定时器：线程占用 / summaryChanged

private:
    explicit AsyncTaskManager(QObject *parent = nullptr);
    ~AsyncTaskManager() override;

    qint64 nextId();

    // 仅由内部 QueuedConnection 调用，保证不跨线程直接改模型索引
    Q_INVOKABLE void updateTaskProgressImpl(qint64 taskId, qint64 done, qint64 total);
    Q_INVOKABLE void markTaskRunningImpl(qint64 taskId);
    Q_INVOKABLE void finishTaskImpl(qint64 taskId, AsyncTaskState finalState, const AsyncErrorInfo &err);

    void pushHistory(const AsyncTask &t);  // 已结束任务移入 history（最多 100 条）

private:
    mutable QMutex mu_;
    qint64 nextId_ = 1;
    QList<AsyncTask> tasks_;           // Queued + Running
    QList<AsyncTask> history_;         // Finished/Failed/Canceled（最近 100）
    int maxHistory_ = 100;
    QTimer statsTimer_;
    int lastRunning_ = -1;
    int lastQueued_ = -1;
    int lastFinished_ = -1;
};

#endif // ASYNCTASKMANAGER_H
