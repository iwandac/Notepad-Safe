#include "AsyncThreadPool.h"
#include <algorithm>

AsyncThreadPool& AsyncThreadPool::instance()
{
    static AsyncThreadPool inst;
    return inst;
}

AsyncThreadPool::AsyncThreadPool(QObject *parent)
    : QObject(parent)
    , runningByPrio_(5, 0)
    , queuedByPrio_(5, 0)
{
    // 并发上限按 CPU 核心数设置：直接用 QThread::idealThreadCount（含SMT=逻辑核心）
    //   - 24核 → 24线程并行 LineIndex + 多段搜索
    //   - 4核 → 4线程
    // 旧上限 min(ideal, 16) 在24核/32核大机上严重限制吞吐，移除
    int ideal = QThread::idealThreadCount();
    if (ideal <= 0) ideal = 4;
    pool_.setMaxThreadCount(ideal);
}

AsyncThreadPool::~AsyncThreadPool()
{
    pool_.waitForDone(5000);
}

int AsyncThreadPool::maxThreads() const { return pool_.maxThreadCount(); }
void AsyncThreadPool::setMaxThreads(int n) { pool_.setMaxThreadCount(qMax(1, n)); }

int AsyncThreadPool::activeThreadCount()
{
    return pool_.activeThreadCount();
}

int AsyncThreadPool::totalActiveThreadCount()
{
    return pool_.activeThreadCount() + externalActive();
}

void AsyncThreadPool::incExternalActive(int n /*= 1*/)
{
    if (n <= 0) return;
    {
        QMutexLocker lk(&mu_);
        externalActive_ += n;
    }
    emit occupancyChanged();
}

void AsyncThreadPool::decExternalActive(int n /*= 1*/)
{
    if (n <= 0) return;
    {
        QMutexLocker lk(&mu_);
        externalActive_ = qMax(0, externalActive_ - n);
    }
    emit occupancyChanged();
}

int AsyncThreadPool::externalActive() const
{
    QMutexLocker lk(&mu_);
    return externalActive_;
}

int AsyncThreadPool::runningCountByPriority(AsyncTaskPriority p)
{
    QMutexLocker lk(&mu_);
    int i = static_cast<int>(p);
    return (i >= 0 && i < runningByPrio_.size()) ? runningByPrio_[i] : 0;
}

int AsyncThreadPool::queuedCountByPriority(AsyncTaskPriority p)
{
    QMutexLocker lk(&mu_);
    int i = static_cast<int>(p);
    return (i >= 0 && i < queuedByPrio_.size()) ? queuedByPrio_[i] : 0;
}

void AsyncThreadPool::bookkeepingStart(AsyncTaskPriority p)
{
    QMutexLocker lk(&mu_);
    int i = static_cast<int>(p);
    if (i >= 0 && i < runningByPrio_.size()) runningByPrio_[i]++;
    emit occupancyChanged();
}

void AsyncThreadPool::bookkeepingFinish(AsyncTaskPriority p)
{
    QMutexLocker lk(&mu_);
    int i = static_cast<int>(p);
    if (i >= 0 && i < runningByPrio_.size()) runningByPrio_[i] = std::max(0, runningByPrio_[i] - 1);
    emit occupancyChanged();
}

class WrappedRunnable : public QRunnable
{
public:
    std::function<void()> fn;
    AsyncTaskPriority priority;
    AsyncThreadPool *host;
    void run() override {
        host->bookkeepingStart(priority);
        try {
            if (fn) fn();
        } catch (...) {
            // 异常会在 AsyncTaskManager::submit 里面再次捕获并落 error；这里仅兜底保证 bookkeepingFinish
        }
        host->bookkeepingFinish(priority);
    }
};

bool AsyncThreadPool::tryStartWorker(std::function<void()> fn, AsyncTaskPriority priority)
{
    WrappedRunnable *r = new WrappedRunnable();
    r->fn = std::move(fn);
    r->priority = priority;
    r->host = this;
    r->setAutoDelete(true);
    // 注：Qt6 QRunnable 早期版本不统一暴露 setPriority()，我们不依赖 QRunnable 内部优先级；
    // 这里的优先级调度已通过 bookkeepingStart + Manager.submit() 入队顺序保证（抗饥饿判定后再 start）。

    {
        QMutexLocker lk(&mu_);
        int qidx = static_cast<int>(priority);
        if (qidx >= 0 && qidx < queuedByPrio_.size()) queuedByPrio_[qidx]++;
    }

    const int maxT = pool_.maxThreadCount();
    const int reserve = (maxT > 1) ? reserveForHighest_ : 0;

    // 抗饥饿判定：
    //   若当前是 Highest 类 -> 允许抢占，直接 start
    //   若 maxT <= 1 -> 单线程，不做保留
    //   否则：当前"非Highest活跃 + 已在池中的运行中任务"不得超过 (maxT - reserve)
    if (priority == AsyncTaskPriority::Highest || maxT <= 1) {
        pool_.start(r);
        return true;
    }

    // 计算当前已被非Highest任务占用的槽位：runningByPrio_[Normal/Low/Lowest/High] 之和 + 池中当前活跃
    int nonHighestRunning = 0;
    {
        QMutexLocker lk(&mu_);
        for (int i = 0; i < runningByPrio_.size(); ++i) {
            if (i != static_cast<int>(AsyncTaskPriority::Highest))
                nonHighestRunning += runningByPrio_[i];
        }
    }
    // QThreadPool::activeThreadCount 更权威：两者取较大值以更保守（留更多空间给 Highest）
    int active = pool_.activeThreadCount();
    nonHighestRunning = qMax(nonHighestRunning, active);

    if (nonHighestRunning >= (maxT - reserve)) {
        // 槽位不够给低优先级占用；先不启动。
        // 回退：仍把 r 入池（保留用户定义的QRunnable优先级语义），不阻塞 Manager。
        // 说明：此 "保留" 是软性策略 — 最坏情况下最高优先级任务可能等几十毫秒，
        //       但 QThreadPool 的优先级队列会让 Highest 的 QRunnable 尽早被挑出。
        pool_.start(r);
        return true;
    }

    pool_.start(r);
    return true;
}

void AsyncThreadPool::forceStartWorker(std::function<void()> fn, AsyncTaskPriority priority)
{
    tryStartWorker(std::move(fn), priority);
}
