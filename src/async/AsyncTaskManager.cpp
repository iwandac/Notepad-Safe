#include "AsyncTaskManager.h"
#include "AsyncThreadPool.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QThread>
#include <algorithm>

#if defined(Q_OS_WIN)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

AsyncTaskManager& AsyncTaskManager::instance()
{
    static AsyncTaskManager inst;
    return inst;
}

AsyncTaskManager::AsyncTaskManager(QObject *parent)
    : QAbstractListModel(parent)
{
    statsTimer_.setInterval(1000);
    connect(&statsTimer_, &QTimer::timeout, this, &AsyncTaskManager::onTickStats);
    statsTimer_.start();
    // AsyncTaskManager 是函数静态单例，它的析构晚于 main() 栈上的 QApplication。
    // 若让活动 QTimer 一直活到静态析构，Qt 的主线程事件调度器已销毁，会报
    // "Timers cannot be stopped from another thread"。在事件循环退出前显式停表。
    if (auto *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            statsTimer_.stop();
        });
    }
    qRegisterMetaType<AsyncTask>("AsyncTask");
    qRegisterMetaType<AsyncTaskState>("AsyncTaskState");
    qRegisterMetaType<AsyncTaskType>("AsyncTaskType");
    qRegisterMetaType<AsyncErrorInfo>("AsyncErrorInfo");
}

AsyncTaskManager::~AsyncTaskManager()
{
    // 单元测试等没有进入 app.exec() 的宿主可能不发 aboutToQuit；仅在线程归属
    // 仍一致时兜底停止，避免跨线程触碰 QObject 定时器。
    if (QThread::currentThread() == thread())
        statsTimer_.stop();
}

qint64 AsyncTaskManager::nextId()
{
    return nextId_++;
}

int AsyncTaskManager::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    QMutexLocker lk(&mu_);
    return tasks_.size() + history_.size();
}

QHash<int, QByteArray> AsyncTaskManager::roleNames() const
{
    QHash<int, QByteArray> rn;
    rn[Qt::DisplayRole]           = "display";
    rn[TaskIdRole]                = "taskId";
    rn[TaskBatchIdRole]           = "batchId";
    rn[TaskTypeRole]              = "type";
    rn[TaskPriorityRole]          = "priority";
    rn[TaskStateRole]             = "state";
    rn[TaskTitleRole]             = "title";
    rn[TaskDescriptionRole]       = "description";
    rn[TaskProgressDoneRole]      = "progressDone";
    rn[TaskProgressTotalRole]     = "progressTotal";
    rn[TaskPercentRole]           = "percent";
    rn[TaskCreatedAtRole]         = "createdAt";
    rn[TaskStartedAtRole]         = "startedAt";
    rn[TaskFinishedAtRole]        = "finishedAt";
    rn[TaskElapsedMsRole]         = "elapsedMs";
    rn[TaskErrorRole]             = "error";
    rn[TaskCategoryTagRole]       = "categoryTag";
    rn[TaskStateTextRole]         = "stateText";
    rn[TaskPriorityTextRole]      = "priorityText";
    rn[TaskTypeTextRole]          = "typeText";
    return rn;
}

static AsyncTask taskAtUnlocked(const QList<AsyncTask> &tasks, const QList<AsyncTask> &history, int row)
{
    if (row < tasks.size()) return tasks[row];
    int h = row - tasks.size();
    if (h >= 0 && h < history.size()) return history[history.size() - 1 - h]; // 倒序：最近完成的在前
    return AsyncTask();
}

QVariant AsyncTaskManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return QVariant();
    AsyncTask t;
    {
        QMutexLocker lk(&mu_);
        if (index.row() < 0 || index.row() >= tasks_.size() + history_.size()) return QVariant();
        t = taskAtUnlocked(tasks_, history_, index.row());
    }
    switch (role) {
    case Qt::DisplayRole:           return QStringLiteral("%1 [%2] %3%").arg(t.title, t.stateText(), t.percent() < 0 ? QStringLiteral("-") : QString::number(t.percent()));
    case TaskIdRole:                return t.id;
    case TaskBatchIdRole:           return t.batchId;
    case TaskTypeRole:              return static_cast<int>(t.type);
    case TaskPriorityRole:          return static_cast<int>(t.priority);
    case TaskStateRole:             return static_cast<int>(t.state);
    case TaskTitleRole:             return t.title;
    case TaskDescriptionRole:       return t.description;
    case TaskProgressDoneRole:      return QVariant::fromValue(t.progressDone);
    case TaskProgressTotalRole:     return QVariant::fromValue(t.progressTotal);
    case TaskPercentRole:           return t.percent();
    case TaskCreatedAtRole:         return t.createdAt;
    case TaskStartedAtRole:         return t.startedAt;
    case TaskFinishedAtRole:        return t.finishedAt;
    case TaskElapsedMsRole:         return QVariant::fromValue(t.elapsedMs());
    case TaskErrorRole:             return t.error.isNull() ? QString() : t.error.toString();
    case TaskCategoryTagRole:       return t.categoryTag;
    case TaskStateTextRole:         return t.stateText();
    case TaskPriorityTextRole:      return t.priorityText();
    case TaskTypeTextRole:          return t.typeText();
    }
    return QVariant();
}

AsyncTask AsyncTaskManager::taskById(qint64 id) const
{
    QMutexLocker lk(&mu_);
    for (const auto &t : tasks_) if (t.id == id) return t;
    for (const auto &t : history_) if (t.id == id) return t;
    return AsyncTask();
}

int AsyncTaskManager::runningCount() const
{
    QMutexLocker lk(&mu_);
    int c = 0;
    for (const auto &t : tasks_) if (t.state == AsyncTaskState::Running) c++;
    return c;
}

int AsyncTaskManager::queuedCount() const
{
    QMutexLocker lk(&mu_);
    int c = 0;
    for (const auto &t : tasks_) if (t.state == AsyncTaskState::Queued) c++;
    return c;
}

int AsyncTaskManager::errorCount() const
{
    QMutexLocker lk(&mu_);
    int c = 0;
    for (const auto &t : history_) if (t.state == AsyncTaskState::Failed || t.state == AsyncTaskState::Canceled) c++;
    return c;
}

int AsyncTaskManager::failedCount() const
{
    QMutexLocker lk(&mu_);
    int count = 0;
    for (const auto &task : history_) {
        if (task.state == AsyncTaskState::Failed)
            ++count;
    }
    return count;
}

int AsyncTaskManager::cancellableCount() const
{
    QMutexLocker lk(&mu_);
    int count = 0;
    for (const auto &task : tasks_) {
        if (task.state == AsyncTaskState::Queued
            || task.state == AsyncTaskState::Running) {
            ++count;
        }
    }
    return count;
}

int AsyncTaskManager::activeThreadCount()
{
    return AsyncThreadPool::instance().totalActiveThreadCount();
}

static inline int findTaskIndex(QList<AsyncTask> &list, qint64 id)
{
    for (int i = 0; i < list.size(); ++i) if (list[i].id == id) return i;
    return -1;
}

qint64 AsyncTaskManager::submit(AsyncTaskType type, AsyncTaskPriority priority, const QString &title, const QString &description,
                                WorkerFn workerFn, qint64 batchId, const QString &categoryTag, qint64 progressTotalInit)
{
    AsyncTask t;
    t.id = nextId();
    t.batchId = batchId;
    t.type = type;
    t.priority = priority;
    // Crypto operations deliberately remain opaque in the global task UI.
    // A caller must never make a password, account name or protected path part
    // of a long-lived task-history row.
    if (type == AsyncTaskType::FileEncrypt) {
        t.title = tr("保存安全文档");
        t.description = tr("正在以原子方式写入加密数据。");
    } else if (type == AsyncTaskType::FileDecrypt) {
        t.title = tr("打开安全文档");
        t.description = tr("正在验证并解密文档。");
    } else {
        t.title = title;
        t.description = description;
    }
    t.progressDone = 0;
    t.progressTotal = progressTotalInit;
    t.createdAt = QDateTime::currentDateTime();
    t.categoryTag = categoryTag;
    t.cancel = AsyncCancelPtr::create();
    t.cancel->store(false, std::memory_order_release);

    int rowInsert;
    {
        QMutexLocker lk(&mu_);
        // 按优先级插入（Highest 在最前）
        rowInsert = 0;
        while (rowInsert < tasks_.size()
               && static_cast<int>(tasks_[rowInsert].priority)
                      <= static_cast<int>(priority)) {
            ++rowInsert;
        }
    }
    beginInsertRows(QModelIndex(), rowInsert, rowInsert);
    {
        QMutexLocker lk(&mu_);
        tasks_.insert(rowInsert, t);
    }
    endInsertRows();
    emit taskAdded(t.id);
    emitSummarySnapshot();

    auto weakTaskId = t.id;
    auto cancelPtr = t.cancel;
    auto mgr = this;

    // 立即在主线程标记 Running（QueuedConnection 自身保证线程安全，同步发也没大问题）
    QMetaObject::invokeMethod(mgr, "markTaskRunningImpl", Qt::QueuedConnection, Q_ARG(qint64, weakTaskId));

    auto reportProgress = [mgr, weakTaskId](qint64 done, qint64 total) {
        QMetaObject::invokeMethod(mgr, "updateTaskProgressImpl", Qt::QueuedConnection,
                                  Q_ARG(qint64, weakTaskId), Q_ARG(qint64, done), Q_ARG(qint64, total));
    };
    auto reportError = [mgr, weakTaskId](const AsyncErrorInfo &err) {
        AsyncErrorInfo copy = err;
        QMetaObject::invokeMethod(mgr, "finishTaskImpl", Qt::QueuedConnection,
                                  Q_ARG(qint64, weakTaskId),
                                  Q_ARG(AsyncTaskState, AsyncTaskState::Failed),
                                  Q_ARG(AsyncErrorInfo, copy));
    };

    AsyncThreadPool::instance().tryStartWorker([mgr, workerFn, weakTaskId, cancelPtr, reportProgress, reportError]() {
        AsyncErrorInfo err;
        AsyncTaskState finalState = AsyncTaskState::Finished;
        try {
            if (cancelPtr && cancelPtr->load(std::memory_order_acquire)) {
                finalState = AsyncTaskState::Canceled;
            } else if (workerFn) {
                workerFn(weakTaskId, cancelPtr, reportProgress, reportError);
            }
            if (cancelPtr && cancelPtr->load(std::memory_order_acquire) && finalState == AsyncTaskState::Finished) {
                finalState = AsyncTaskState::Canceled;
            }
        } catch (const std::exception &e) {
            finalState = AsyncTaskState::Failed;
            err.message = QString::fromUtf8(e.what());
#if defined(Q_OS_WIN)
            // 收集堆栈（最多 32 帧）—— 符号解析暂不做（那需要 dbghelp 依赖），先记录 RVA 后续手动解析。
            void* frames[32];
            USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < n; ++i) {
                err.stackFrames.push_back(QStringLiteral("frame[%1] = 0x%2")
                    .arg(i).arg(reinterpret_cast<quintptr>(frames[i]), 0, 16));
            }
#endif
            reportError(err);
            return; // reportError 已经发 finishTaskImpl(Failed)，下面不再重复
        } catch (...) {
            finalState = AsyncTaskState::Failed;
            err.message = QObject::tr("未知异常(非std::exception)");
            reportError(err);
            return;
        }
        // 正常完成 / 被取消 — 统一走 finishTaskImpl
        QMetaObject::invokeMethod(mgr, "finishTaskImpl", Qt::QueuedConnection,
                                  Q_ARG(qint64, weakTaskId),
                                  Q_ARG(AsyncTaskState, finalState),
                                  Q_ARG(AsyncErrorInfo, err));
    }, priority);

    return t.id;
}

void AsyncTaskManager::cancel(qint64 taskId)
{
    bool found = false;
    bool queuedCanceled = false;
    AsyncCancelPtr cancelRef;
    {
        QMutexLocker lk(&mu_);
        int idx = findTaskIndex(tasks_, taskId);
        if (idx >= 0) {
            if (tasks_[idx].cancel) {
                tasks_[idx].cancel->store(true, std::memory_order_release);
                cancelRef = tasks_[idx].cancel;
            }
            queuedCanceled = tasks_[idx].state == AsyncTaskState::Queued;
            found = true;
        }
    }
    // Never emit model/task signals while mu_ is held. StatusBarTaskWidget and
    // other direct receivers query the model synchronously; emitting under the
    // mutex deadlocks when a just-submitted (still Queued) task is canceled by
    // immediate Save As/close.
    if (queuedCanceled) {
        beginResetModel();
        {
            QMutexLocker lk(&mu_);
            const int idx = findTaskIndex(tasks_, taskId);
            if (idx >= 0 && tasks_[idx].state == AsyncTaskState::Queued) {
                AsyncTask copy = tasks_[idx];
                copy.state = AsyncTaskState::Canceled;
                copy.finishedAt = QDateTime::currentDateTime();
                tasks_.removeAt(idx);
                history_.push_front(copy);
                if (history_.size() > maxHistory_)
                    history_.pop_back();
            } else {
                queuedCanceled = false;
            }
        }
        endResetModel();
    }
    if (queuedCanceled) {
        emit taskFinished(taskId, AsyncTaskState::Canceled);
        emitSummarySnapshot();
        return;
    }
    if (found) {
        // Running 状态：置 cancel 原子后交给 worker 自己在下次检查时退出并回调 finishTaskImpl(Canceled)
        Q_UNUSED(cancelRef);
    }
}

void AsyncTaskManager::cancelAllByType(AsyncTaskType type)
{
    QList<qint64> toCancel;
    {
        QMutexLocker lk(&mu_);
        for (const auto &t : tasks_) {
            if (type == AsyncTaskType::Unknown || t.type == type) toCancel.push_back(t.id);
        }
    }
    for (qint64 id : toCancel) cancel(id);
}

void AsyncTaskManager::clearHistory()
{
    beginResetModel();
    {
        QMutexLocker lk(&mu_);
        history_.clear();
    }
    endResetModel();
    emitSummarySnapshot();
}

// ===== 内部 QueuedConnection 更新接口 =====

void AsyncTaskManager::updateTaskProgressImpl(qint64 taskId, qint64 done, qint64 total)
{
    int row = -1;
    {
        QMutexLocker lk(&mu_);
        int idx = findTaskIndex(tasks_, taskId);
        if (idx < 0) return;
        tasks_[idx].progressDone = done;
        if (total > 0) tasks_[idx].progressTotal = total;
        row = idx;
    }
    if (row >= 0) {
        Q_EMIT dataChanged(index(row), index(row));
        emit taskProgressChanged(taskId, done, total);
    }
}

void AsyncTaskManager::updateTaskProgress(qint64 taskId, qint64 done, qint64 total)
{
    int row = -1;
    {
        QMutexLocker lk(&mu_);
        int idx = findTaskIndex(tasks_, taskId);
        if (idx < 0) return;
        tasks_[idx].progressDone = done;
        if (total > 0) tasks_[idx].progressTotal = total;
        row = idx;
    }
    if (row >= 0) {
        Q_EMIT dataChanged(index(row), index(row));
        emit taskProgressChanged(taskId, done, total);
    }
}

void AsyncTaskManager::markTaskRunningImpl(qint64 taskId)
{
    int row = -1;
    {
        QMutexLocker lk(&mu_);
        int idx = findTaskIndex(tasks_, taskId);
        if (idx < 0) return;
        tasks_[idx].state = AsyncTaskState::Running;
        tasks_[idx].startedAt = QDateTime::currentDateTime();
        row = idx;
    }
    if (row >= 0) {
        Q_EMIT dataChanged(index(row), index(row));
        emitSummarySnapshot();
    }
}

void AsyncTaskManager::finishTaskImpl(qint64 taskId, AsyncTaskState finalState, const AsyncErrorInfo &err)
{
    AsyncTask finished;
    bool found = false;
    beginResetModel();
    {
        QMutexLocker lk(&mu_);
        const int idx = findTaskIndex(tasks_, taskId);
        if (idx < 0) {
            // 可能已被 cancel() 直接移入 history（Queued 时）；忽略重复回调
        } else {
            found = true;
            finished = tasks_[idx];
            finished.state = finalState;
            finished.finishedAt = QDateTime::currentDateTime();
            if (!err.isNull()) finished.error = err;
            // 强制同步最终进度：成功完成时将 progressDone 设为 progressTotal
            if (finalState == AsyncTaskState::Finished && finished.progressTotal > 0 && finished.progressDone < finished.progressTotal) {
                finished.progressDone = finished.progressTotal;
            }
            tasks_.removeAt(idx);
            history_.push_front(finished);
            while (history_.size() > maxHistory_) history_.pop_back();
        }
    }
    endResetModel();
    if (!found)
        return;
    emit taskFinished(taskId, finalState);
    emitSummarySnapshot();
}

void AsyncTaskManager::pushHistory(const AsyncTask &t)
{
    beginResetModel();
    {
        QMutexLocker lk(&mu_);
        history_.push_front(t);
        while (history_.size() > maxHistory_) history_.pop_back();
    }
    endResetModel();
    emitSummarySnapshot();
}

void AsyncTaskManager::onTickStats()
{
    emitSummarySnapshot();
    int maxT = AsyncThreadPool::instance().maxThreads();
    int act  = AsyncThreadPool::instance().totalActiveThreadCount();
    emit threadCountChanged(act, maxT);
}

void AsyncTaskManager::emitSummarySnapshot()
{
    int queued = 0, running = 0, finished = 0;
    {
        QMutexLocker lk(&mu_);
        for (const auto &t : tasks_) {
            if (t.state == AsyncTaskState::Queued) queued++;
            else if (t.state == AsyncTaskState::Running) running++;
        }
        finished = int(history_.size());
    }
    bool changed = (queued != lastQueued_ || running != lastRunning_ || finished != lastFinished_);
    if (changed) {
        lastQueued_ = queued;
        lastRunning_ = running;
        lastFinished_ = finished;
        emit summaryChanged(queued, running, finished);
    }
}
