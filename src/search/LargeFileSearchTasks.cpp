#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"
#include "AsyncTaskManager.h"
#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"

#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QScopeGuard>
#include <QThread>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QDateTime>
#include <QUuid>
#include <QPointer>
#include <cstring>
#include <exception>
#include <algorithm>
#include <utility>

namespace {

constexpr quint32 OVR_MAGIC = 0x564F534Du;
constexpr quint32 OVR_VERSION = 2;

QString overrunDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe");
    QDir().mkpath(d);
    return d;
}

bool readOverrunHeader(QDataStream &stream, quint32 *version, quint64 *count)
{
    quint32 magic = 0;
    quint32 parsedVersion = 0;
    quint64 parsedCount = 0;
    stream >> magic >> parsedVersion >> parsedCount;
    if (stream.status() != QDataStream::Ok || magic != OVR_MAGIC
        || (parsedVersion != 1 && parsedVersion != OVR_VERSION)) {
        return false;
    }
    if (version) *version = parsedVersion;
    if (count) *count = parsedCount;
    return true;
}

bool readOverrunRecord(QDataStream &stream, quint32 version,
                       LargeFileSearchMatch *match)
{
    if (!match)
        return false;
    quint64 byteOffset = 0;
    qint32 length = 0;
    qint32 line = 0;
    qint32 col = 0;
    quint64 contextByteOffset = 0;
    QByteArray context;
    stream >> byteOffset >> length >> line >> col;
    if (version >= 2)
        stream >> contextByteOffset;
    stream >> context;
    if (stream.status() != QDataStream::Ok)
        return false;
    match->byteOffset = qint64(byteOffset);
    match->length = length;
    match->line = line;
    match->col = col;
    match->contextByteOffset = version >= 2
        ? qint64(contextByteOffset)
        : qMax<qint64>(0, qint64(byteOffset) - col);
    match->lineContext = std::move(context);
    return true;
}

} // namespace

struct LargeFileSearch::WorkerReservation {
    enum class Phase { Pending, Running, Settled };
    std::atomic<Phase> phase{Phase::Pending};
    bool released = false; // workerMutex_; distinct from winning the atomic claim.
    qint64 taskId = 0; // The following binding/connection fields are engine-thread only.
    struct Terminal { qint64 id; AsyncTaskState state; AsyncErrorInfo error; };
    QVector<Terminal> earlyTerminals;
    QMetaObject::Connection terminalConnection;
    std::function<void(const QString &, const QString &)> failure;
    // Only the winning Pending canceller or Running worker writes the outcome.
    // The engine reads it only after the existing mutex/CV release barrier.
    QString raw;
    QString userMessage;
    std::function<void()> completion;
};

LargeFileSearch::WorkerReservationPtr LargeFileSearch::reserveWorker(
    std::function<void(const QString &, const QString &)> failure)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto worker = std::make_shared<WorkerReservation>();
    worker->failure = std::move(failure);
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (shuttingDown_ || stoppingWorkers_ || workerReservations_ != 0 || currentWorker_)
            return {};
        currentWorker_ = worker;
        ++workerReservations_;
    }
    std::weak_ptr<WorkerReservation> weak = worker;
    worker->terminalConnection = connect(&AsyncTaskManager::instance(),
        &AsyncTaskManager::taskFinished, this,
        [this, weak](qint64 id, AsyncTaskState state) {
            if (const auto reservation = weak.lock())
                observeWorkerTerminal(reservation, id, state,
                    AsyncTaskManager::instance().taskById(id).error);
        });
    return worker;
}

bool LargeFileSearch::claimWorker(const WorkerReservationPtr &worker)
{
    auto expected = WorkerReservation::Phase::Pending;
    return worker->phase.compare_exchange_strong(expected,
        WorkerReservation::Phase::Running, std::memory_order_acq_rel);
}

void LargeFileSearch::setWorkerFailure(const WorkerReservationPtr &worker,
                                      const QString &raw, const QString &userMessage)
{
    worker->raw = raw;
    worker->userMessage = userMessage;
    worker->completion = {};
}

void LargeFileSearch::setWorkerCompletion(const WorkerReservationPtr &worker,
                                         std::function<void()> completion)
{
    worker->raw.clear();
    worker->userMessage.clear();
    worker->completion = std::move(completion);
}

void LargeFileSearch::recordWorkerException(const WorkerReservationPtr &worker)
{
    QString raw;
    try {
        throw;
    } catch (const std::exception &error) {
        raw = QString::fromUtf8(error.what());
    } catch (...) {
        raw = QObject::tr("未知异常(非std::exception)");
    }
    if (raw.isEmpty())
        raw = QObject::tr("搜索任务异常退出。");
    setWorkerFailure(worker, raw,
        QObject::tr("查找未完成；可查看技术详情后重试，或缩小查找范围。"));
}

void LargeFileSearch::releaseWorker(const WorkerReservationPtr &worker)
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    Q_ASSERT(!worker->released && workerReservations_ > 0);
    worker->released = true;
    --workerReservations_;
    workerIdle_.notify_all();
}

void LargeFileSearch::finishWorker(const WorkerReservationPtr &worker)
{
    // Queue while the reservation still protects this QObject. Even when the
    // GUI receives it immediately, deliverWorker waits for the final release.
    auto release = qScopeGuard([this, worker] {
        auto expected = WorkerReservation::Phase::Running;
        const bool owned = worker->phase.compare_exchange_strong(expected,
            WorkerReservation::Phase::Settled, std::memory_order_acq_rel);
        Q_ASSERT(owned);
        if (owned)
            releaseWorker(worker);
    });
    QMetaObject::invokeMethod(this, [this, worker] { deliverWorker(worker); },
                              Qt::QueuedConnection);
}

bool LargeFileSearch::isCurrentWorker(const WorkerReservationPtr &worker) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return currentWorker_ == worker && !shuttingDown_ && !stoppingWorkers_;
}

void LargeFileSearch::deliverWorker(const WorkerReservationPtr &worker)
{
    if (!isCurrentWorker(worker))
        return;
    {
        std::unique_lock<std::mutex> lock(workerMutex_);
        workerIdle_.wait(lock, [&worker] { return worker->released; });
    }
    QObject::disconnect(worker->terminalConnection);
    cancel_.store(false, std::memory_order_release);
    currentWorker_.reset();
    const QPointer<LargeFileSearch> owner(this);
    emit sourceBecameIdle();
    if (!owner) return;
    // No query/connection cleanup after invoking user code: a direct receiver
    // may start the next query, replace the source, or destroy this engine.
    if (!worker->raw.isEmpty())
        worker->failure(worker->raw, worker->userMessage);
    else if (worker->completion)
        worker->completion();
}

void LargeFileSearch::observeWorkerTerminal(const WorkerReservationPtr &worker,
    qint64 taskId, AsyncTaskState state, const AsyncErrorInfo &error)
{
    if (!isCurrentWorker(worker))
        return;
    if (worker->taskId == 0) {
        worker->earlyTerminals.push_back({taskId, state, error});
        return;
    }
    if (worker->taskId != taskId)
        return;
    auto expected = WorkerReservation::Phase::Pending;
    if (!worker->phase.compare_exchange_strong(expected,
            WorkerReservation::Phase::Settled, std::memory_order_acq_rel))
        return; // UI Queued/Canceled does not prove the worker has not entered.
    const bool canceled = state == AsyncTaskState::Canceled;
    setWorkerFailure(worker,
        canceled ? QObject::tr("已取消。") : (error.message.isEmpty()
            ? QObject::tr("搜索任务未能启动。") : error.message),
        canceled ? QObject::tr("查找已停止，结果不完整；可重新查找。")
            : QObject::tr("查找未完成；可查看技术详情后重试，或缩小查找范围。"));
    releaseWorker(worker);
    QMetaObject::invokeMethod(this, [this, worker] { deliverWorker(worker); },
                              Qt::QueuedConnection);
}

void LargeFileSearch::bindWorkerTask(const WorkerReservationPtr &worker, qint64 taskId)
{
    worker->taskId = taskId;
    if (!isCurrentWorker(worker)) {
        AsyncTaskManager::instance().cancel(taskId);
        return;
    }
    lastTaskId_ = taskId;
    const auto early = std::move(worker->earlyTerminals);
    for (const auto &terminal : early) {
        if (terminal.id == taskId)
            observeWorkerTerminal(worker, taskId, terminal.state, terminal.error);
    }
}

void LargeFileSearch::abandonWorker(const WorkerReservationPtr &worker)
{
    QObject::disconnect(worker->terminalConnection);
    if (currentWorker_ == worker)
        currentWorker_.reset();
    auto expected = WorkerReservation::Phase::Pending;
    if (worker->phase.compare_exchange_strong(expected,
            WorkerReservation::Phase::Settled, std::memory_order_acq_rel))
        releaseWorker(worker);
}

void LargeFileSearch::stopWorkers(bool permanent)
{
    Q_ASSERT(QThread::currentThread() == thread());
    WorkerReservationPtr worker;
    bool alreadyStopping;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        alreadyStopping = stoppingWorkers_;
        stoppingWorkers_ = true;
        shuttingDown_ = shuttingDown_ || permanent;
        worker = currentWorker_;
    }
    cancel_.store(true, std::memory_order_release);
    const qint64 taskId = lastTaskId_;
    // Retire Pending before calling the manager: taskAdded can re-enter here
    // before submit has returned an id, and cancel() itself emits direct signals.
    if (worker)
        abandonWorker(worker);
    const QPointer<LargeFileSearch> owner(this);
    if (taskId > 0)
        AsyncTaskManager::instance().cancel(taskId);
    if (!owner)
        return;

    std::unique_lock<std::mutex> lock(workerMutex_);
    workerIdle_.wait(lock, [this] { return workerReservations_ == 0; });
    lastTaskId_ = 0;
    stoppingWorkers_ = alreadyStopping;
    if (!shuttingDown_ && !alreadyStopping)
        cancel_.store(false, std::memory_order_release);
}

void LargeFileSearch::cancelAndWait()
{
    const QPointer<LargeFileSearch> owner(this);
    stopWorkers(false);
    if (owner) emit sourceBecameIdle();
}

void LargeFileSearch::findAll(const QString& pattern, const Options& opts) {
    findAllViaTaskManager(pattern, opts, QString());
}

QString LargeFileSearch::failureUserMessage(const QString &raw) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (failurePresentation_.raw == raw && !failurePresentation_.userMessage.isEmpty())
        return failurePresentation_.userMessage;
    return QObject::tr("查找未完成；可查看技术详情后重试，或缩小查找范围。");
}

void LargeFileSearch::publishFailure(const QString &raw, const QString &userMessage)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // Scoped signal context, not retained failure history. Nested emissions
    // restore their caller's context and rejected starts overwrite no state.
    const FailurePresentation previous = failurePresentation_;
    QPointer<LargeFileSearch> guard(this);
    failurePresentation_ = {raw, userMessage};
    emit failed(raw);
    if (guard)
        guard->failurePresentation_ = previous;
}

// ============================================================================
//  findAllViaTaskManager：AsyncTaskManager 统一派发，AsyncTaskPanel 可见/可取消
// ============================================================================
qint64 LargeFileSearch::findAllViaTaskManager(const QString& pattern, const Options& opts,
                                              const QString& fileDisplayName)
{
    const QPointer<LargeFileSearch> idleOwner(this);
    const auto idleOnRejection = qScopeGuard([idleOwner] {
        if (idleOwner && !idleOwner->sourceInUse()) emit idleOwner->sourceBecameIdle();
    });
    if (!mmapBase_) {
        publishFailure(QStringLiteral("数据源未设置。"),
            QObject::tr("文件尚未准备好进行查找；请稍后重试。"));
        return -1;
    }
    QByteArray literalBytes;
    bool usePcre2 = false;
    QString patternError;
    if (!preparePattern(pattern, opts, &literalBytes,
                              &usePcre2, &patternError)) {
        publishFailure(patternError,
            QObject::tr("查找条件无法使用；请检查表达式或字节格式后重试。"));
        return -1;
    }

    const auto worker = reserveWorker([this](const QString &raw, const QString &userMessage) {
        publishFailure(raw, userMessage);
    });
    if (!worker) {
        publishFailure(QStringLiteral("已有搜索正在运行。"),
            QObject::tr("已有查找正在进行；请等待完成或停止后再试。"));
        return -1;
    }
    QPointer<LargeFileSearch> owner(this);
    auto releaseOnSubmitFailure = qScopeGuard([owner, worker] {
        if (owner)
            owner->abandonWorker(worker);
    });
    // Rejecting an overlap must not reset an active query's cancellation,
    // matches, or spool. All mutations follow the exclusive reservation.
    if (cancel_.exchange(false, std::memory_order_acq_rel)) {
        abandonWorker(worker);
        releaseOnSubmitFailure.dismiss();
        publishFailure(QStringLiteral("已取消。"),
            QObject::tr("查找已停止，结果不完整；可重新查找。"));
        return -1;
    }
    memMatches_.clear();
    storageFailed_ = false;
    scanError_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunCheckpointBytes_.clear();
    overrunGroupedCount_ = 0;
    overrunLastLine_ = std::numeric_limits<qint32>::min();
    overrunLastContext_ = std::numeric_limits<qint64>::min();
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
    memMatches_.reserve(int(qMin<qint64>(maxMemMatches_, qint64(2000000))));

    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;

    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("(未命名大文件)")
        : fileDisplayName;
    const QString title = QObject::tr("搜索大文件: %1").arg(displayName);
    const QString desc  = QObject::tr("%1 · %2 · 区分大小写:%3 · 整词:%4")
        .arg(displayName)
        .arg(opts.regexMode ? QObject::tr("正则表达式") : QObject::tr("字面值"))
        .arg(opts.caseSensitive ? QObject::tr("是") : QObject::tr("否"))
        .arg(opts.wholeWord ? QObject::tr("是") : QObject::tr("否"));

    const qint64 submittedTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch,
        AsyncTaskPriority::High,
        title,
        desc,
        /* workerFn = */ [this, worker, virtualSize, pattern, opts]
        (qint64 taskId, AsyncCancelPtr cancel,
         std::function<void(qint64,qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo&)> reportError)
        {
            // No dereference of the captured engine before the shared claim:
            // a canceled Pending wrapper may outlive the engine and its source.
            if (!LargeFileSearch::claimWorker(worker))
                return;
            auto release = qScopeGuard([this, worker] { finishWorker(worker); });
            try {
                const auto isCancelled = [&]() {
                    return (cancel && cancel->load(std::memory_order_acquire))
                        || cancel_.load(std::memory_order_relaxed);
                };
                Options taskOptions = opts;
                taskOptions.reportPhase = [taskId](
                    const QString &phase, int index, int count,
                    qint64 done, qint64 total) {
                    AsyncTaskManager::instance().updateTaskPhase(
                        taskId, phase, index, count, done, total);
                };
                const bool ok = scanAllBounded(
                    virtualSize, pattern, taskOptions, isCancelled,
                    [this, worker, reportProgress, virtualSize](qint64 done) {
                        reportProgress(done, virtualSize);
                        QMetaObject::invokeMethod(this, [this, worker, done, virtualSize] {
                            if (isCurrentWorker(worker))
                                emit progress(done, virtualSize);
                        }, Qt::QueuedConnection);
                    },
                    [this, worker](const QVector<LargeFileSearchMatch> &batch) {
                        QMetaObject::invokeMethod(this, [this, worker, batch] {
                            if (isCurrentWorker(worker))
                                emit partialBatch(batch);
                        }, Qt::QueuedConnection);
                        return true;
                    });
                if (!ok) {
                    const QString raw = storageFailed_
                        ? QObject::tr("搜索结果临时存储写入失败。")
                        : (!scanError_.isEmpty() ? scanError_ : QObject::tr("已取消。"));
                    const QString userMessage = storageFailed_
                        ? QObject::tr("搜索结果临时保存失败，查找未完成；请检查临时目录是否可用，查看技术详情后重试。")
                        : (!scanError_.isEmpty()
                            ? QObject::tr("查找未完成；可查看技术详情后重试，或缩小查找范围。")
                            : QObject::tr("查找已停止，结果不完整；可重新查找。"));
                    setWorkerFailure(worker, raw, userMessage);
                    if (storageFailed_ || !scanError_.isEmpty()) {
                        AsyncErrorInfo taskError;
                        taskError.message = raw;
                        taskError.userMessage = userMessage;
                        reportError(taskError);
                    }
                    return;
                }
                std::atomic_thread_fence(std::memory_order_release);
                const qint64 count = qint64(memMatches_.size()) + overrunCount_;
                reportProgress(virtualSize, virtualSize);
                QMetaObject::invokeMethod(this, [this, worker, virtualSize] {
                    if (isCurrentWorker(worker))
                        emit progress(virtualSize, virtualSize);
                }, Qt::QueuedConnection);
                setWorkerCompletion(worker, [this, count] { emit finished(count); });
            } catch (...) {
                recordWorkerException(worker);
                throw; // Preserve the Manager's existing exception diagnostics/state.
            }
        },
        /* batchId = */ 0,
        /* categoryTag = */ QLatin1String("Search"),
        /* progressTotalInit = */ virtualSize
    );
    if (owner)
        owner->bindWorkerTask(worker, submittedTaskId);
    else
        AsyncTaskManager::instance().cancel(submittedTaskId);
    releaseOnSubmitFailure.dismiss();
    return submittedTaskId;
}

qint64 LargeFileSearch::findOneViaTaskManager(
    const QString &pattern, const Options &opts, qint64 fromByte,
    bool forward, bool wrap, const QString &fileDisplayName,
    QObject *callbackContext, OneMatchCallback callback)
{
    const QPointer<LargeFileSearch> idleOwner(this);
    const auto idleOnRejection = qScopeGuard([idleOwner] {
        if (idleOwner && !idleOwner->sourceInUse()) emit idleOwner->sourceBecameIdle();
    });
    if (!mmapBase_) {
        if (callback)
            callback(false, {}, QObject::tr("数据源未就绪。"));
        return -1;
    }
    QString validationError;
    QByteArray literal;
    bool usePcre2 = false;
    if (!preparePattern(pattern, opts, &literal, &usePcre2,
                              &validationError)) {
        if (callback)
            callback(false, {}, validationError);
        return -1;
    }
    QPointer<QObject> guard(callbackContext);
    const auto worker = reserveWorker([guard, callback](const QString &raw, const QString &) {
        if (guard && callback)
            QMetaObject::invokeMethod(guard, [callback, raw] { callback(false, {}, raw); },
                                      Qt::QueuedConnection);
    });
    if (!worker) {
        if (callback)
            callback(false, {}, QObject::tr("搜索任务正在运行。"));
        return -1;
    }
    QPointer<LargeFileSearch> owner(this);
    auto releaseOnSubmitFailure = qScopeGuard([owner, worker] {
        if (owner)
            owner->abandonWorker(worker);
    });
    cancel_.store(false, std::memory_order_release);
    const qint64 virtualSize = patchList_
        ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("大文件") : fileDisplayName;
    const qint64 submittedTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch, AsyncTaskPriority::Highest,
        forward ? QObject::tr("查找下一个: %1").arg(displayName)
                : QObject::tr("查找上一个: %1").arg(displayName),
        QObject::tr("定向扫描，找到一处即返回"),
        [this, worker, virtualSize, pattern, opts, fromByte, forward, wrap,
         guard, callback = std::move(callback)]
        (qint64 taskId, AsyncCancelPtr cancel,
         std::function<void(qint64, qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo &)> reportError) mutable {
            if (!LargeFileSearch::claimWorker(worker))
                return;
            auto release = qScopeGuard([this, worker] { finishWorker(worker); });
            try {
                const auto isCancelled = [this, cancel]() {
                    return (cancel && cancel->load(std::memory_order_acquire))
                        || cancel_.load(std::memory_order_acquire);
                };
                LargeFileSearchMatch found;
                found.byteOffset = -1;
                QString error;
                Options taskOptions = opts;
                taskOptions.reportPhase = [taskId](
                    const QString &phase, int index, int count,
                    qint64 done, qint64 total) {
                    AsyncTaskManager::instance().updateTaskPhase(
                        taskId, phase, index, count, done, total);
                };
                const bool ok = scanOneBounded(
                    virtualSize, pattern, taskOptions, fromByte, forward, wrap,
                    isCancelled,
                    [reportProgress, virtualSize](qint64 done) {
                        reportProgress(qMin(done, virtualSize), virtualSize);
                    },
                    &found, &error);
                if (!ok && error.isEmpty() && isCancelled())
                    error = QObject::tr("已取消。");
                if (!ok && !error.isEmpty() && error != QObject::tr("已取消。")) {
                    AsyncErrorInfo taskError;
                    taskError.message = error;
                    taskError.userMessage = QObject::tr("未能完成定位匹配。请检查查找条件，查看技术详情后重试。");
                    reportError(taskError);
                }
                const bool matched = ok && found.byteOffset >= 0;
                // The engine queues while protected, then releases before its GUI
                // completion posts to the external context. No worker-side QPointer
                // check/use window remains after source-close can cross the barrier.
                setWorkerCompletion(worker,
                    [guard, callback = std::move(callback), matched,
                     found = std::move(found), error = std::move(error)]() mutable {
                        if (guard && callback)
                            QMetaObject::invokeMethod(guard,
                                [callback = std::move(callback), matched,
                                 found = std::move(found), error = std::move(error)]() mutable {
                                    callback(matched, std::move(found), std::move(error));
                                }, Qt::QueuedConnection);
                    });
            } catch (...) {
                recordWorkerException(worker);
                throw;
            }
        },
        0, QLatin1String("SearchNavigation"), virtualSize);
    if (owner)
        owner->bindWorkerTask(worker, submittedTaskId);
    else
        AsyncTaskManager::instance().cancel(submittedTaskId);
    releaseOnSubmitFailure.dismiss();
    return submittedTaskId;
}

// ============================================================================
//  findNext / findPrev
// ============================================================================
qint64 LargeFileSearch::findNext(qint64 fromByte, LargeFileSearchMatch* outMatch) const {
    // Resident prefix is bounded; the spool starts from the nearest sparse
    // checkpoint so F3 latency does not grow linearly with total result count.
    for (const auto& m : memMatches_) {
        if (m.byteOffset > fromByte) {
            if (outMatch) *outMatch = m;
            return m.byteOffset;
        }
    }
    if (overrunCount_ > 0 && QFile::exists(overrunFile_)) {
        QFile f(overrunFile_);
        if (f.open(QIODevice::ReadOnly)) {
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            quint32 version = 0;
            if (!readOverrunHeader(s, &version, nullptr))
                return -1;
            const auto checkpoint = std::upper_bound(
                overrunCheckpointBytes_.cbegin(), overrunCheckpointBytes_.cend(),
                fromByte);
            if (checkpoint != overrunCheckpointBytes_.cbegin()) {
                const qsizetype index = std::distance(overrunCheckpointBytes_.cbegin(),
                                                      checkpoint - 1);
                if (index < overrunCheckpoints_.size())
                    f.seek(overrunCheckpoints_.at(index).second);
            }
            while (!s.atEnd()) {
                LargeFileSearchMatch match;
                if (!readOverrunRecord(s, version, &match))
                    break;
                if (match.byteOffset > fromByte) {
                    if (outMatch) *outMatch = match;
                    return match.byteOffset;
                }
            }
        }
    }
    return -1;
}
qint64 LargeFileSearch::findPrev(qint64 fromByte, LargeFileSearchMatch* outMatch) const {
    qint64 best = -1;
    const LargeFileSearchMatch* bestM = nullptr;
    for (const auto& m : memMatches_) {
        if (m.byteOffset < fromByte && (best < 0 || m.byteOffset > best)) {
            best = m.byteOffset; bestM = &m;
        }
    }
    if (overrunCount_ > 0 && QFile::exists(overrunFile_)) {
        QFile f(overrunFile_);
        if (f.open(QIODevice::ReadOnly)) {
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            quint32 version = 0;
            if (!readOverrunHeader(s, &version, nullptr))
                return best;
            const auto checkpoint = std::lower_bound(
                overrunCheckpointBytes_.cbegin(), overrunCheckpointBytes_.cend(),
                fromByte);
            if (checkpoint == overrunCheckpointBytes_.cbegin())
                return best;
            const qsizetype checkpointIndex = std::distance(
                overrunCheckpointBytes_.cbegin(), checkpoint - 1);
            if (checkpointIndex < overrunCheckpoints_.size())
                f.seek(overrunCheckpoints_.at(checkpointIndex).second);
            static thread_local LargeFileSearchMatch tmp;
            while (!s.atEnd()) {
                LargeFileSearchMatch match;
                if (!readOverrunRecord(s, version, &match))
                    break;
                if (match.byteOffset >= fromByte)
                    break;
                if (match.byteOffset < fromByte && match.byteOffset > best) {
                    best = match.byteOffset;
                    tmp = std::move(match);
                    bestM = &tmp;
                }
            }
        }
    }
    if (best >= 0 && bestM && outMatch) *outMatch = *bestM;
    return best;
}

// ============================================================================
//  replaceAll（§5.6 步骤 3，同步执行；进度分批，用户可见事务始终为 1 UndoGroup）
// ============================================================================
bool LargeFileSearch::replaceAll(const QString& pattern, const QString& replacement,
                                 const Options& optsIn, PatchList& patches,
                                 ReplaceCallbacks cb, QString* errMsg,
                                 qint64* outReplacedCount)
{
    if (outReplacedCount) *outReplacedCount = 0;
    Options opts = optsIn;
    opts.forward = true;
    // Step 1：先跑一次 findAll（同步的本地版本，避免信号队列）
    cancel_.store(false);
    memMatches_.clear();
    storageFailed_ = false;
    scanError_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunCheckpointBytes_.clear();
    overrunGroupedCount_ = 0;
    overrunLastLine_ = std::numeric_limits<qint32>::min();
    overrunLastContext_ = std::numeric_limits<qint64>::min();
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }

    bool replacementEncodingOk = true;
    QByteArray replBytes = opts.byteContextMode
        ? replacement.toUtf8()
        : encodeTextFragment(replacement, opts.textEncoding,
                             &replacementEncodingOk);
    if (!opts.regexMode && !replacementEncodingOk) {
        if (errMsg) {
            *errMsg = QObject::tr(
                "替换文本包含 %1 无法表示的字符；尚未修改文件。")
                          .arg(encodingName(opts.textEncoding));
        }
        return false;
    }

    // Step 1 简化：直接收集所有匹配（分批收集，但这里内存直接做；溢出则失败——replaceAll
    // 必须有完整匹配列表才能正确按 byteOffset 从大到小替换）
    QVector<LargeFileSearchMatch> allMatches;
    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    bool resultLimitExceeded = false;
    bool zeroLengthReplacementUnsupported = false;
    const bool scanOk = scanAllBounded(
        virtualSize, pattern, opts,
        [this] { return cancel_.load(std::memory_order_relaxed); },
        {},
        [&](const QVector<LargeFileSearchMatch> &batch) {
            if (std::any_of(batch.cbegin(), batch.cend(),
                            [](const LargeFileSearchMatch &match) {
                                return match.length == 0;
                            })) {
                zeroLengthReplacementUnsupported = true;
                return false;
            }
            if (qint64(allMatches.size()) + batch.size() > maxMemMatches_) {
                resultLimitExceeded = true;
                return false;
            }
            allMatches += batch;
            return true;
        });
    if (!scanOk) {
        if (errMsg) {
            if (zeroLengthReplacementUnsupported) {
                *errMsg = QObject::tr(
                    "批量替换暂不接受零长度匹配；请让表达式至少消费一个字符。");
            } else if (resultLimitExceeded) {
                *errMsg = QObject::tr(
                    "匹配超过安全批量替换上限（%1 处）。请缩小查找范围后分批替换。")
                              .arg(maxMemMatches_);
            } else if (!scanError_.isEmpty()) {
                *errMsg = scanError_;
            } else {
                *errMsg = QObject::tr("取消");
            }
        }
        return false;
    }

    qint64 total = allMatches.size();
    if (total == 0) {
        if (cb.progressBatch) cb.progressBatch(0, 0);
        if (outReplacedCount) *outReplacedCount = 0;
        return true;
    }

    // Offsets and capture expansions are defined against the same immutable
    // pre-replacement snapshot. Sort first so the replacement byte vector
    // remains index-aligned while patches are later applied back-to-front.
    std::sort(allMatches.begin(), allMatches.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                  return a.byteOffset > b.byteOffset;
              });
    QVector<QByteArray> expandedReplacements;
    const auto wipeExpansions = qScopeGuard([&] {
        if (!replBytes.isEmpty())
            replBytes.fill(0);
        for (QByteArray &value : expandedReplacements) {
            if (!value.isEmpty())
                value.fill(0);
        }
    });
    if (opts.regexMode) {
        const bool encodedByteRegex = !opts.byteContextMode
            && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
        Pcre2Regex compiled;
        QString expansionError;
        if (encodedByteRegex) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "原始编码文本视图为只读；请转换并另存为 UTF-8 后替换。");
            }
            return false;
        }
        if (!compiled.compile(pattern, false, opts.caseSensitive,
                              opts.wholeWord, &expansionError)) {
            if (errMsg) *errMsg = expansionError;
            return false;
        }

        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_replace_snapshot_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        if (patchList_ && patchList_->isModified()) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法创建正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            constexpr qint64 SnapshotChunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += SnapshotChunk) {
                if (cancel_.load()) {
                    if (errMsg) *errMsg = QObject::tr("取消");
                    return false;
                }
                const qint64 end = qMin(virtualSize, start + SnapshotChunk);
                QByteArray chunk = readVirtualBytes(start, end);
                const bool writeOk = chunk.size() == end - start
                    && snapshot.write(chunk) == chunk.size();
                if (!chunk.isEmpty())
                    chunk.fill(0);
                if (!writeOk) {
                    if (errMsg) {
                        *errMsg = QObject::tr("无法写入正则替换快照：%1")
                                      .arg(snapshot.errorString());
                    }
                    return false;
                }
            }
            if (!snapshot.flush()) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法刷新正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法映射正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmapSnapshot = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });
        expandedReplacements.reserve(allMatches.size());
        for (const LargeFileSearchMatch &match : std::as_const(allMatches)) {
            QByteArray expanded;
            const bool expandedOk = compiled.expandReplacementAt(
                subject, virtualSize, match.byteOffset, match.length,
                replacement, &expanded, &expansionError);
            if (!expandedOk) {
                if (errMsg) *errMsg = expansionError;
                return false;
            }
            expandedReplacements.append(std::move(expanded));
        }
    }

    // Step 2：压力检测（估算替换导致的内存增量 = Σ|replBytes.size() - m.length|）
    qint64 estSize = 0;
    qint64 undoBytes = patches.l1Bytes();
    for (qsizetype index = 0; index < allMatches.size(); ++index) {
        const qsizetype replacementSize = opts.regexMode
            ? expandedReplacements.at(index).size() : replBytes.size();
        estSize += qAbs<qint64>(replacementSize - allMatches.at(index).length);
        const qint64 recordBytes = qint64(allMatches.at(index).length)
            + replacementSize + 128;
        if (recordBytes < 0 || undoBytes > patches.l1Limit() - recordBytes) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "全部替换超过当前可撤销内存预算；尚未修改文件。请缩小范围后分批执行。");
            }
            return false;
        }
        undoBytes += recordBytes;
    }
    if (cb.onPressureSoftWarn && patches.pressureWillExceedSoftWarn(estSize)) {
        bool decision = cb.onPressureSoftWarn(
            QObject::tr("预估替换将使 Patch 达到 ~%1 MB，建议中间保存后再继续。是否立即保存并继续？")
                .arg(double(patches.l1Bytes() + estSize)/1048576., 1, 'f', 1));
        if (decision) {
            if (!mmapBase_ || !patchList_) {
                if (errMsg) *errMsg = QStringLiteral("缺少数据源无法中间保存");
                return false;
            }
            QString path = cb.needSaveAsPath ? cb.needSaveAsPath() : QString();
            if (path.isEmpty()) {
                if (errMsg) *errMsg = QStringLiteral("未提供保存路径。");
                return false;
            }
            std::atomic<bool> c{false};
            if (!patches.applyAndSave(mmapBase_, mmapSize_, path, {}, &c, errMsg)) return false;
            patches.resetAfterSave();
        }
    }

    // Step 3：一次 Replace All = 一个 UndoGroup。BATCH 只控制进度/取消
    // 检查频率，不能改变用户可见的撤销边界。预算检查保证该 group 不会
    // spill 到会清空 undo history 的 L2 cold storage。
    const int BATCH = DynamicThresholds::instance().REPLACE_BATCH_SIZE;
    qint64 done = 0;
    qint64 applied = 0;
    bool groupOpen = true;
    patches.beginUndoGroup();
    const auto rollbackUncommitted = qScopeGuard([&] {
        if (!groupOpen)
            return;
        patches.endUndoGroup();
        if (applied > 0) {
            QString rollbackError;
            if (!patches.undo(&rollbackError) && errMsg) {
                *errMsg += QObject::tr("；自动回滚失败：%1").arg(rollbackError);
            }
        }
    });
    for (qint64 bStart = 0; bStart < total; bStart += BATCH) {
        if (cancel_.load()) { if (errMsg) *errMsg = QStringLiteral("取消"); return false; }
        qint64 bEnd = qMin<qint64>(bStart + BATCH, total);
        for (qint64 i = bStart; i < bEnd; ++i) {
            const LargeFileSearchMatch& m = allMatches[int(i)];
            QByteArray oldBytes;
            if (mmapBase_) {
                oldBytes = readVirtualBytes(m.byteOffset, m.byteOffset + m.length);
            }
            if (oldBytes.size() < m.length) oldBytes.resize(m.length, '\0');
            const QByteArray &replacementBytes = opts.regexMode
                ? expandedReplacements.at(int(i)) : replBytes;
            QString localErr;
            bool ok = patches.replaceWithSnapshots(m.byteOffset, m.length,
                                                   replacementBytes, oldBytes,
                                                   &localErr);
            if (!ok) {
                if (errMsg) *errMsg = QObject::tr("第 %1 处替换失败：%2").arg(i).arg(localErr);
                return false;
            }
            ++applied;
        }
        done = bEnd;
        if (cb.progressBatch) cb.progressBatch(done, total);

        if (cb.onPressureSoftWarn && patches.needSoftWarn()) {
            bool doSave = cb.onPressureSoftWarn(
                QObject::tr("Patch 占用已达 %1/%2 MB，是否保存后继续？")
                    .arg(double(patches.l1Bytes())/1048576., 1, 'f', 1)
                    .arg(double(patches.l1Limit())/1048576., 1, 'f', 1));
            if (doSave) {
                QString path = cb.needSaveAsPath ? cb.needSaveAsPath() : QString();
                if (path.isEmpty()) { if (errMsg) *errMsg = QStringLiteral("未提供保存路径。"); return false; }
                std::atomic<bool> c{false};
                if (!patches.applyAndSave(mmapBase_, mmapSize_, path, {}, &c, errMsg)) return false;
                patches.resetAfterSave();
            }
        }
    }
    patches.endUndoGroup();
    groupOpen = false;
    if (outReplacedCount) *outReplacedCount = done;
    return true;
}
