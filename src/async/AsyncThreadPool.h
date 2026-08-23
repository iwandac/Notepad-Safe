#ifndef ASYNCTHREADPOOL_H
#define ASYNCTHREADPOOL_H

#include <QThreadPool>
#include <QMutex>
#include <QVector>
#include <functional>
#include "AsyncTask.h"

/**
 * @brief 统一线程池调度器。
 *
 * AsyncTaskPriority 映射到 QThreadPool 的原生队列优先级。已经运行的任务不会被
 * 抢占；优先级只决定空闲 worker 下一次取哪个排队任务。
 */
class AsyncThreadPool
{
public:
    static constexpr int MaximumWorkerThreads = 12;

    static AsyncThreadPool& instance();

    int  maxThreads() const;
    int  totalActiveThreadCount();

    /**
     * @brief 将 worker 放入统一线程池，按 priority 调度。
     */
    void startWorker(std::function<void()> fn, AsyncTaskPriority priority);

    /**
     * Run stateful crypto/KDBX work on the framework-owned security lane.
     * The lane has one non-expiring worker so QObject-based KeePassXC state
     * never migrates between pool threads. It is included in maxThreads().
     */
    void startSecurityWorker(std::function<void()> fn,
                             AsyncTaskPriority priority);

    /**
     * Run a bounded parallel batch on framework-owned workers. The caller may
     * block, but first-party domain code never creates a QThread/QThreadPool.
     * One central interactive slot remains available while the batch runs.
     */
    void runBatchBlocking(QVector<std::function<void()>> jobs,
                          AsyncTaskPriority priority = AsyncTaskPriority::Normal);

private:
    AsyncThreadPool();
    ~AsyncThreadPool();
    QThreadPool pool_;
    QThreadPool securityPool_;
    mutable QMutex capacityMutex_;
    int configuredMaxThreads_ = MaximumWorkerThreads;
};

#endif // ASYNCTHREADPOOL_H
