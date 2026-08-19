#include "AsyncThreadPool.h"
#include <QMutexLocker>

AsyncThreadPool& AsyncThreadPool::instance()
{
    static AsyncThreadPool inst;
    return inst;
}

AsyncThreadPool::AsyncThreadPool()
{
    pool_.setMaxThreadCount(MaximumWorkerThreads);
}

AsyncThreadPool::~AsyncThreadPool()
{
    pool_.waitForDone(5000);
}

int AsyncThreadPool::maxThreads() const { return MaximumWorkerThreads; }

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

int AsyncThreadPool::reserveExternalCapacity(int requestedWorkers)
{
    if (requestedWorkers <= 0)
        return 0;
    QMutexLocker lock(&capacityMutex_);
    // Keep one central slot available even when reservation happens from a
    // synchronous diagnostic rather than from a central-pool parent worker.
    const int centralOccupancy = qMax(1, pool_.activeThreadCount());
    const int available = qMax(0, MaximumWorkerThreads
                                  - centralOccupancy
                                  - externalReserved_);
    const int granted = qMin(requestedWorkers, available);
    if (granted <= 0)
        return 0;
    externalReserved_ += granted;
    pool_.setMaxThreadCount(qMax(1, MaximumWorkerThreads - externalReserved_));
    return granted;
}

void AsyncThreadPool::releaseExternalCapacity(int reservedWorkers)
{
    if (reservedWorkers <= 0)
        return;
    QMutexLocker lock(&capacityMutex_);
    externalReserved_ = qMax(0, externalReserved_ - reservedWorkers);
    pool_.setMaxThreadCount(qMax(1, MaximumWorkerThreads - externalReserved_));
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
