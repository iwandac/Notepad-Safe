#include "EditableTextFindAllTask.h"

#include "AsyncTaskManager.h"
#include "EditableTextSearch.h"
#include "Pcre2Regex.h"

#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <atomic>
#include <limits>
#include <memory>
#include <utility>

namespace {

enum class Execution { Pending, Running, Skipped };

struct Snapshot {
    QByteArray bytes;
    bool sensitive = false;
    ~Snapshot() { clear(); }
    void clear()
    {
        if (sensitive && !bytes.isEmpty()) bytes.fill(0);
        bytes.clear();
    }
};

struct Terminal {
    qint64 id;
    AsyncTaskState state;
    QString error;
};

template<typename Result>
struct Job {
    // Only the Pending -> Running/Skipped winner may take/clear snapshot.
    std::atomic<Execution> execution{Execution::Pending};
    std::atomic_bool workerExited{false};
    Snapshot snapshot;
    Result result; // published by workerExited release
    // The remaining fields and QPointer are read/written only on the UI thread.
    qint64 id = 0;
    QPointer<QObject> owner, delivery;
    std::function<void(qint64, Result)> callback;
    QMetaObject::Connection finishedConnection, ownerDestroyedConnection,
        deliveryDestroyedConnection;
    QVector<Terminal> earlyTerminals;
    bool terminalSeen = false;
    bool delivered = false;
    bool abandoned = false;
    Terminal terminal{0, AsyncTaskState::Queued, {}};
};

template<typename Result>
void disconnectJob(const std::shared_ptr<Job<Result>> &job)
{
    QObject::disconnect(job->finishedConnection);
    QObject::disconnect(job->ownerDestroyedConnection);
    QObject::disconnect(job->deliveryDestroyedConnection);
}

template<typename Result>
void skipPendingSnapshot(const std::shared_ptr<Job<Result>> &job)
{
    Execution expected = Execution::Pending;
    if (job->execution.compare_exchange_strong(expected, Execution::Skipped,
                                               std::memory_order_acq_rel))
        job->snapshot.clear();
}

template<typename Result>
void contextDestroyed(const std::shared_ptr<Job<Result>> &job, AsyncTaskManager *dispatcher)
{
    // Destruction slots explicitly clear their context: QWidget can emit
    // destroyed() before QObject clears QPointers. Keep a live receiver's
    // terminal callback even after the document stops owning the work.
    if (!job->owner) skipPendingSnapshot(job);
    if (!job->owner && !job->delivery) {
        job->abandoned = true;
        job->callback = {};
        disconnectJob(job);
    }
    if (!job->owner && job->id > 0) dispatcher->cancel(job->id);
}

template<typename Result>
void settle(const std::shared_ptr<Job<Result>> &job, AsyncTaskManager *dispatcher)
{
    if (job->abandoned || job->delivered || !job->terminalSeen) return;
    if (job->execution.load(std::memory_order_acquire) != Execution::Skipped
        && !job->workerExited.load(std::memory_order_acquire)) return;
    job->delivered = true;
    disconnectJob(job);
    auto result = std::move(job->result);
    if (job->terminal.state == AsyncTaskState::Canceled) {
        result = {};
        result.error = QObject::tr("已取消。");
    } else if (job->terminal.state == AsyncTaskState::Failed) {
        result.ok = false;
        if (result.error.isEmpty()) result.error = job->terminal.error;
        if (result.error.isEmpty()) result.error = QObject::tr("查找未完成，请重试。");
    }
    // Always queue, even when cancel() synchronously emitted taskFinished.
    // The caller can first finish assigning/clearing its accepted task ID.
    QMetaObject::invokeMethod(dispatcher,
        [owner = job->owner, delivery = job->delivery, callback = std::move(job->callback),
         id = job->id, result = std::move(result)]() mutable {
            if ((owner || delivery) && callback) callback(id, std::move(result));
        }, Qt::QueuedConnection);
}

template<typename Result>
void observeTerminal(const std::shared_ptr<Job<Result>> &job, AsyncTaskManager *dispatcher,
                     const Terminal &terminal)
{
    if (job->abandoned || job->terminalSeen) return;
    job->terminalSeen = true;
    job->terminal = terminal;
    if (terminal.state == AsyncTaskState::Canceled) skipPendingSnapshot(job);
    settle(job, dispatcher);
}

EditableTextFindAllResult scanSnapshot(
    const QByteArray &subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, qint64 taskId, const AsyncCancelPtr &cancel,
    const std::function<void(qint64, qint64)> &reportProgress,
    const std::function<void(const AsyncErrorInfo &)> &reportError)
{
    EditableTextFindAllResult result;
    result.matches.reserve(256);
    result.hits.reserve(256);
    const auto cancelled = [&] {
        return cancel && cancel->load(std::memory_order_acquire);
    };
    AsyncTaskManager::instance().updateTaskPhase(
        taskId, regex ? QObject::tr("执行正则匹配") : QObject::tr("扫描字面值"),
        1, 1, 0, subject.size());
    bool ok = true;
    int lineNumber = 0;
    qint64 lineStart = 0;
    qint64 cachedContextStart = -1;
    QString cachedContext;
    const auto recordMatch = [&](qint64 byteOffset, qint64 byteLength) {
        // One extra match proves overflow; do not retain or shape that sentinel.
        if (result.matches.size() == 200000) {
            result.truncated = true;
            return false;
        }
        if (byteLength < 0 || byteLength > std::numeric_limits<qint32>::max()) {
            result.error = QObject::tr("单个匹配过长。");
            return false;
        }
        while (lineStart < byteOffset) {
            const qsizetype newline = subject.indexOf('\n', qsizetype(lineStart));
            if (newline < 0 || newline >= byteOffset) break;
            lineStart = qint64(newline) + 1;
            ++lineNumber;
        }
        if (cachedContextStart != lineStart) {
            qsizetype lineEnd = subject.indexOf('\n', qsizetype(lineStart));
            if (lineEnd < 0) lineEnd = subject.size();
            qint64 contextLength = qint64(lineEnd) - lineStart;
            if (contextLength > 0
                && subject.at(qsizetype(lineStart + contextLength - 1)) == '\r')
                --contextLength;
            cachedContext = QString::fromUtf8(subject.constData() + lineStart, contextLength);
            cachedContextStart = lineStart;
        }
        SearchMatch match;
        match.line = lineNumber;
        match.index = QString::fromUtf8(subject.constData() + lineStart, byteOffset - lineStart).size();
        match.length = QString::fromUtf8(subject.constData() + byteOffset, byteLength).size();
        match.text = QString::fromUtf8(subject.constData() + byteOffset, byteLength);
        match.context = cachedContext;
        match.byteOffset = byteOffset;
        match.byteLength = qint32(byteLength);
        result.hits.push_back({byteOffset, qint32(byteLength)});
        result.matches.push_back(std::move(match));
        return true;
    };
    const auto progress = [reportProgress, total = qint64(subject.size())](qint64 done) {
        reportProgress(done, total);
    };
    if (regex) {
        Pcre2Regex compiled;
        ok = compiled.compile(pattern, false, caseSensitive, wholeWord, &result.error);
        if (ok) {
            const auto scanned = compiled.scan(
                reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
                cancelled, progress, recordMatch, &result.error);
            ok = scanned != Pcre2Regex::ScanResult::Failed
                && (scanned != Pcre2Regex::ScanResult::Cancelled || result.truncated)
                && !cancelled() && result.error.isEmpty();
        }
    } else {
        const auto literalHits = findEditableTextLiteralHits(
            subject, pattern, caseSensitive, wholeWord, cancelled, progress,
            false, false, -1, 200001);
        if (cancelled()) ok = false;
        else {
            for (const EditableTextHit &hit : literalHits) {
                if (!recordMatch(hit.byteOffset, hit.byteLength)) {
                    ok = result.truncated && result.error.isEmpty() && !cancelled();
                    break;
                }
            }
        }
    }
    if (!ok && result.error.isEmpty() && cancelled()) result.error = QObject::tr("已取消。");
    if (!ok && !result.error.isEmpty() && result.error != QObject::tr("已取消。")) {
        AsyncErrorInfo taskError;
        taskError.message = result.error;
        taskError.userMessage = QObject::tr("查找未完成。请检查查找条件，查看技术详情后重试，或缩小查找范围。");
        reportError(taskError);
    }
    result.ok = ok;
    return result;
}

template<typename Result, typename Scan>
qint64 startSnapshotTask(
    QByteArray subject, bool wipeSnapshot, QObject *ownerContext,
    QObject *deliveryContext, std::function<void(qint64, Result)> callback,
    AsyncTaskPriority priority, const QString &title, const QString &description,
    const QString &category, Scan scan)
{
    auto *dispatcher = &AsyncTaskManager::instance();
    auto job = std::make_shared<Job<Result>>();
    job->snapshot.bytes = std::move(subject);
    job->snapshot.sensitive = wipeSnapshot;
    if (QThread::currentThread() != dispatcher->thread() || !ownerContext || !deliveryContext
        || ownerContext->thread() != dispatcher->thread()
        || deliveryContext->thread() != dispatcher->thread()) return -1;
    job->owner = ownerContext;
    job->delivery = deliveryContext;
    job->callback = std::move(callback);
    job->finishedConnection = QObject::connect(
        dispatcher, &AsyncTaskManager::taskFinished, dispatcher,
        [job, dispatcher](qint64 id, AsyncTaskState state) {
            Terminal terminal{id, state, dispatcher->taskById(id).error.message};
            if (job->id == 0) job->earlyTerminals.append(std::move(terminal));
            else if (id == job->id) observeTerminal(job, dispatcher, terminal);
        });
    job->ownerDestroyedConnection = QObject::connect(
        ownerContext, &QObject::destroyed, dispatcher, [job, dispatcher](QObject *owner) {
            job->owner.clear();
            if (job->delivery.data() == owner) job->delivery.clear();
            contextDestroyed(job, dispatcher);
        });
    if (deliveryContext != ownerContext) {
        job->deliveryDestroyedConnection = QObject::connect(
            deliveryContext, &QObject::destroyed, dispatcher, [job, dispatcher]() {
                job->delivery.clear();
                contextDestroyed(job, dispatcher);
            });
    }
    const qint64 snapshotSize = job->snapshot.bytes.size();
    job->id = dispatcher->submit(
        AsyncTaskType::LargeFileSearch, priority, title, description,
        [job, dispatcher, scan = std::move(scan), wipeSnapshot]
        (qint64 taskId, AsyncCancelPtr cancel, const auto &reportProgress, const auto &reportError) {
            Execution expected = Execution::Pending;
            if (!job->execution.compare_exchange_strong(expected, Execution::Running,
                                                        std::memory_order_acq_rel)) return;
            // This guard runs even when scanning throws. The manager retains
            // its existing exception diagnostics; settle waits for both facts.
            const auto exited = qScopeGuard([job, dispatcher]() {
                job->workerExited.store(true, std::memory_order_release);
                QMetaObject::invokeMethod(dispatcher, [job, dispatcher]() {
                    settle(job, dispatcher);
                }, Qt::QueuedConnection);
            });
            QByteArray snapshot = std::move(job->snapshot.bytes);
            const auto wipe = qScopeGuard([&]() {
                if (wipeSnapshot && !snapshot.isEmpty()) snapshot.fill(0);
            });
            job->result = scan(snapshot, taskId, cancel, reportProgress, reportError);
        }, 0, category, snapshotSize);
    // taskAdded observers may synchronously cancel (and even clear history)
    // inside submit. Bind only the exact returned ID, never a guessed task.
    for (const Terminal &terminal : std::as_const(job->earlyTerminals)) {
        if (terminal.id == job->id) observeTerminal(job, dispatcher, terminal);
    }
    job->earlyTerminals.clear();
    if (job->id <= 0 || job->abandoned) {
        disconnectJob(job);
        skipPendingSnapshot(job);
    }
    // Owner destruction may happen synchronously inside submit, before its ID
    // is known. Cancel that exact accepted job without discarding delivery.
    if (!job->owner && job->id > 0) dispatcher->cancel(job->id);
    return job->id;
}


} // namespace

qint64 startEditableTextFindAllTask(
    QByteArray subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, const QString &displayName,
    bool wipeSnapshot, QObject *ownerContext, QObject *deliveryContext,
    std::function<void(qint64, EditableTextFindAllResult)> callback)
{
    return startSnapshotTask<EditableTextFindAllResult>(
        std::move(subject), wipeSnapshot, ownerContext, deliveryContext,
        std::move(callback), AsyncTaskPriority::High,
        QObject::tr("查找全部: %1").arg(displayName), QObject::tr("扫描当前文档"),
        QLatin1String("SearchFindAll"),
        [pattern, regex, caseSensitive, wholeWord](
            const QByteArray &snapshot, qint64 taskId, const AsyncCancelPtr &cancel,
            const auto &progress, const auto &error) {
            return scanSnapshot(snapshot, pattern, regex, caseSensitive,
                                wholeWord, taskId, cancel, progress, error);
        });
}

qint64 startEditableTextNavigationTask(
    QByteArray subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, bool forward, bool wrap,
    qint64 boundary, bool currentZeroLengthHitAtEnd, const QString &displayName,
    bool wipeSnapshot, QObject *ownerContext, QObject *deliveryContext,
    std::function<void(qint64, EditableTextNavigationResult)> callback)
{
    return startSnapshotTask<EditableTextNavigationResult>(
        std::move(subject), wipeSnapshot, ownerContext, deliveryContext,
        std::move(callback), AsyncTaskPriority::Highest,
        forward ? QObject::tr("查找下一个: %1").arg(displayName)
                : QObject::tr("查找上一个: %1").arg(displayName),
        QObject::tr("找到一处即返回"), QLatin1String("SearchNavigation"),
        [pattern, regex, caseSensitive, wholeWord, forward, wrap, boundary,
         currentZeroLengthHitAtEnd](const QByteArray &snapshot, qint64 taskId,
            const AsyncCancelPtr &cancel, const auto &progress, const auto &reportError) {
            AsyncTaskManager::instance().updateTaskPhase(
                taskId, regex ? QObject::tr("执行正则匹配") : QObject::tr("定向扫描字面值"),
                1, 1, 0, snapshot.size());
            const auto cancelled = [&] {
                return cancel && cancel->load(std::memory_order_acquire);
            };
            auto result = scanEditableTextNavigation(
                snapshot, pattern, regex, caseSensitive, wholeWord, forward,
                wrap, boundary, currentZeroLengthHitAtEnd, cancelled, progress);
            if (!result.ok && !result.error.isEmpty() && !cancelled()) {
                AsyncErrorInfo taskError;
                taskError.message = result.error;
                taskError.userMessage = QObject::tr("未能完成定位匹配。请检查查找条件，查看技术详情后重试。");
                reportError(taskError);
            }
            return result;
        });
}
