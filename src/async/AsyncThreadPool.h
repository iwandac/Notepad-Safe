#ifndef ASYNCTHREADPOOL_H
#define ASYNCTHREADPOOL_H

#include <QObject>
#include <QThreadPool>
#include <QMutex>
#include <QVector>
#include <functional>
#include "AsyncTask.h"

/**
 * @brief 统一线程池调度器：优先级调度 + 抗饥饿（解密 Highest 类保留 1 条线程永不被低优先级占满）
 *
 * 经验 1477701 修复：
 * - 以前：加密/解密混在单一FIFO队列，加密大量进入后解密饥饿；
 * - 现在：
 *    1) 入队时通过 priority 参数设置 QRunnable::setPriority()，QThreadPool 已经支持优先级升序优先。
 *    2) 额外引入 "activeCountForPriority()" 检查：若除 Highest 外的活跃线程已占 (maxThreads - reserveThreadsForHighest)，
 *       则低优先级任务暂时延后入池（由 Manager 通过 tryStart/下次事件再尝试），避免池被加密全占导致解密无法立即跑。
 */
class AsyncThreadPool : public QObject
{
    Q_OBJECT
public:
    static AsyncThreadPool& instance();

    int  maxThreads() const;
    void setMaxThreads(int n);

    /** 为 Highest(解密/紧急交互) 永远保留多少条线程槽，默认 1 */
    int  reserveThreadsForHighest() const { return reserveForHighest_; }
    void setReserveThreadsForHighest(int n) { reserveForHighest_ = qMax(0, qMin(n, maxThreads() - 1)); }

    int  activeThreadCount();     // 当前池内活跃 worker 数（QThreadPool::activeThreadCount）
    int  runningCountByPriority(AsyncTaskPriority p);  // 某优先级已登记的 running 计数
    int  queuedCountByPriority(AsyncTaskPriority p);

    /**
     * @brief 外部并行增量计数：LargeFileSearch / LineIndex 内部使用独立 QThreadPool 做分块并行时，
     *        通过这两个接口通告活跃线程数，让 UI「活跃/总数」显示反映实际并行度。
     *        incExternalActive(n) 表示新增 n 条外部活跃线程；decExternalActive(n) 在退出时回调。
     *        实际 UI 显示 = pool_.activeThreadCount() + externalActive_。
     */
    void incExternalActive(int n = 1);
    void decExternalActive(int n = 1);
    int  externalActive() const;
    int  totalActiveThreadCount();    // = activeThreadCount() + externalActive_

    /**
     * @brief 尝试立刻启动一个可抢占的 worker：fn 放到池里执行；成功返回 true。
     *        若因低优先级槽不足（需要为 Highest 保留）返回 false，Manager 会把任务留在队列稍后重试。
     */
    bool tryStartWorker(std::function<void()> fn, AsyncTaskPriority priority);

    /** 不管限制，强制启动（用于内部Highest类回退） */
    void forceStartWorker(std::function<void()> fn, AsyncTaskPriority priority);

signals:
    void occupancyChanged();   // 活跃线程数变化（由 Manager 轮询兜底，这个信号仅在明显变化时发）

private:
    explicit AsyncThreadPool(QObject *parent = nullptr);
    ~AsyncThreadPool() override;
    QThreadPool pool_;
    int reserveForHighest_ = 1;

    // 活跃/排队计数（不依赖 QThreadPool 内部实现，更安全）
    mutable QMutex mu_;
    QVector<int> runningByPrio_;  // index = (int)priority
    QVector<int> queuedByPrio_;
    int externalActive_ = 0;      // 外部并行增量（LargeFileSearch/LineIndex 内部线程池通告）

    void bookkeepingStart(AsyncTaskPriority p);
    void bookkeepingFinish(AsyncTaskPriority p);

    friend class WrappedRunnable;  // WrappedRunnable 在 .cpp 中定义，需要调用 bookkeepingStart/Finish
};

#endif // ASYNCTHREADPOOL_H
