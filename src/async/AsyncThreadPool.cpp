#include "AsyncThreadPool.h"
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>

AsyncThreadPool& AsyncThreadPool::instance()
{
    static AsyncThreadPool inst;
    return inst;
}

AsyncThreadPool::AsyncThreadPool()
{
    const int ideal = qMax(1, QThread::idealThreadCount());
    configuredMaxThreads_ = qBound(2, ideal - 2, MaximumWorkerThreads);
    pool_.setMaxThreadCount(configuredMaxThreads_);
}

AsyncThreadPool::~AsyncThreadPool()
{
    pool_.waitForDone(5000);
}

int AsyncThreadPool::maxThreads() const { return configuredMaxThreads_; }

int AsyncThreadPool::totalActiveThreadCount()
{
    QMutexLocker lock(&capacityMutex_);
    return pool_.activeThreadCount() + externalActive_;
}

class WrappedRunnable : public QRunnable
{
public:
    std::function<void()> fn;
    void run() override {
        try {
            if (fn) fn();
        } catch (...) {
            // AsyncTaskManager::submit catches worker exceptions. This is a
            // final guard against exceptions from a future direct caller.
        }
    }
};

void AsyncThreadPool::startWorker(std::function<void()> fn, AsyncTaskPriority priority)
{
    WrappedRunnable *r = new WrappedRunnable();
    r->fn = std::move(fn);
    r->setAutoDelete(true);

    // QThreadPool processes larger integer priorities first. Our enum uses
    // zero for Highest, hence the sign inversion.
    QMutexLocker lock(&capacityMutex_);
    pool_.start(r, -static_cast<int>(priority));
}

void AsyncThreadPool::runBatchBlocking(QVector<std::function<void()>> jobs,
                                       AsyncTaskPriority priority)
{
    Q_UNUSED(priority);
    if (jobs.isEmpty())
        return;

    const int granted = reserveExternalCapacity(jobs.size());
    if (granted <= 0) {
        for (auto &job : jobs) {
            if (job)
                job();
        }
        return;
    }

    QThreadPool batchPool;
    batchPool.setMaxThreadCount(granted);
    QSemaphore completed;
    for (auto &job : jobs) {
        auto wrapped = [this, job = std::move(job), &completed]() mutable {
            externalWorkerStarted();
            try {
                if (job)
                    job();
            } catch (...) {
                // The owning central task reports its domain error. Always
                // release capacity even when an individual segment fails.
            }
            externalWorkerFinished();
            completed.release();
        };
        auto *runnable = QRunnable::create(std::move(wrapped));
        runnable->setAutoDelete(true);
        batchPool.start(runnable);
    }
    completed.acquire(jobs.size());
    batchPool.waitForDone();
    releaseExternalCapacity(granted);
}

int AsyncThreadPool::reserveExternalCapacity(int requestedWorkers)
{
    if (requestedWorkers <= 0)
        return 0;
    QMutexLocker lock(&capacityMutex_);
    // The caller is normally already occupying one central worker. Account
    // for it and reserve one *additional* central slot so a long parallel line
    // build cannot prevent a newly submitted search/navigation task from
    // starting. Central + private occupancy remains capped at ten.
    const int centralOccupancy = qMax(1, pool_.activeThreadCount());
    constexpr int interactiveCentralSpare = 1;
    const int available = qMax(0, configuredMaxThreads_
                                  - centralOccupancy
                                  - externalReserved_
                                  - interactiveCentralSpare);
    const int granted = qMin(requestedWorkers, available);
    if (granted <= 0)
        return 0;
    externalReserved_ += granted;
    pool_.setMaxThreadCount(qMax(1, configuredMaxThreads_ - externalReserved_));
    return granted;
}

void AsyncThreadPool::releaseExternalCapacity(int reservedWorkers)
{
    if (reservedWorkers <= 0)
        return;
    QMutexLocker lock(&capacityMutex_);
    externalReserved_ = qMax(0, externalReserved_ - reservedWorkers);
    pool_.setMaxThreadCount(qMax(1, configuredMaxThreads_ - externalReserved_));
}

void AsyncThreadPool::externalWorkerStarted()
{
    QMutexLocker lock(&capacityMutex_);
    ++externalActive_;
}

void AsyncThreadPool::externalWorkerFinished()
{
    QMutexLocker lock(&capacityMutex_);
    externalActive_ = qMax(0, externalActive_ - 1);
}
