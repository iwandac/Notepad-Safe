#ifndef ASYNCTHREADPOOL_H
#define ASYNCTHREADPOOL_H

#include <QThreadPool>
#include <QMutex>
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
    static constexpr int MaximumWorkerThreads = 10;

    static AsyncThreadPool& instance();

    int  maxThreads() const;
    int  totalActiveThreadCount();

    /**
     * @brief 将 worker 放入统一线程池，按 priority 调度。
     */
    void startWorker(std::function<void()> fn, AsyncTaskPriority priority);

    // Private pools (currently LineIndex) reserve from the same process-wide
    // budget. A zero result means the caller must stay serial.
    int reserveExternalCapacity(int requestedWorkers);
    void releaseExternalCapacity(int reservedWorkers);

    // Report actual worker occupancy separately from reserved capacity so the
    // task UI shows real activity instead of an allocation estimate.
    void externalWorkerStarted();
    void externalWorkerFinished();

private:
    AsyncThreadPool();
    ~AsyncThreadPool();
    QThreadPool pool_;
    mutable QMutex capacityMutex_;
    int externalReserved_ = 0;
    int externalActive_ = 0;
};

#endif // ASYNCTHREADPOOL_H
