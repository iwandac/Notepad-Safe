#include "AsyncThreadPool.h"
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <memory>

AsyncThreadPool& AsyncThreadPool::instance()
{
    static AsyncThreadPool inst;
    return inst;
}

AsyncThreadPool::AsyncThreadPool()
{
    const int ideal = qMax(1, QThread::idealThreadCount());
    configuredMaxThreads_ = qBound(2, ideal - 1, MaximumWorkerThreads);
    pool_.setMaxThreadCount(configuredMaxThreads_ - 1);
    securityPool_.setMaxThreadCount(1);
    // KeePassXC's retained Database object owns a QObject tree. Keeping this
    // worker alive guarantees that consecutive serialized operations have the
    // same thread affinity.
    securityPool_.setExpiryTimeout(-1);
}

AsyncThreadPool::~AsyncThreadPool()
{
    pool_.waitForDone(5000);
    securityPool_.waitForDone(5000);
}

int AsyncThreadPool::maxThreads() const { return configuredMaxThreads_; }

int AsyncThreadPool::totalActiveThreadCount()
{
    return pool_.activeThreadCount() + securityPool_.activeThreadCount();
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

void AsyncThreadPool::startSecurityWorker(std::function<void()> fn,
                                          AsyncTaskPriority priority)
{
    WrappedRunnable *r = new WrappedRunnable();
    r->fn = std::move(fn);
    r->setAutoDelete(true);

    QMutexLocker lock(&capacityMutex_);
    securityPool_.start(r, -static_cast<int>(priority));
}

void AsyncThreadPool::runBatchBlocking(QVector<std::function<void()>> jobs,
                                       AsyncTaskPriority priority)
{
    if (jobs.isEmpty())
        return;

    struct BatchState {
        QVector<std::function<void()>> jobs;
        std::atomic<int> next{0};
        std::atomic<int> completed{0};
        QSemaphore completion;
    };
    auto state = std::make_shared<BatchState>();
    state->jobs = std::move(jobs);
    const int total = state->jobs.size();
    const auto runOne = [state]() {
        const int index = state->next.fetch_add(1, std::memory_order_relaxed);
        if (index >= state->jobs.size())
            return;
        try {
            if (state->jobs[index])
                state->jobs[index]();
        } catch (...) {
            // The owning task converts its domain failure. Completion must
            // still be signalled so the caller cannot deadlock.
        }
        state->completed.fetch_add(1, std::memory_order_release);
        state->completion.release();
    };

    // The batch caller already occupies one framework worker. Start at most
    // max-2 helpers so a newly queued interactive/security task normally has
    // a free slot. Each helper handles one bounded segment and exits, allowing
    // the priority queue to be reconsidered between segments.
    const int generalThreads = configuredMaxThreads_ - 1;
    const int helpers = qMin(total - 1, qMax(0, generalThreads - 2));
    for (int i = 0; i < helpers; ++i)
        startWorker(runOne, priority);

    // The caller participates. If the pool is saturated by several batches,
    // it continues draining work itself; therefore waiting cannot deadlock on
    // helper runnables that have not started yet.
    while (state->next.load(std::memory_order_relaxed) < total)
        runOne();
    while (state->completed.load(std::memory_order_acquire) < total)
        state->completion.acquire();
}
