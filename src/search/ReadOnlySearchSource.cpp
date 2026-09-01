#include "ReadOnlySearchSource.h"

#include "AsyncTaskManager.h"
#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "SharedReadOnlyFile.h"

#include <QFileInfo>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>

struct ReadOnlySearchSource::Region {
    SharedReadOnlyFile file;
    LineIndex lines;
    const uchar *base = nullptr;
    // A valid empty file is searchable, but requires no native mapping.
    const uchar empty = 0;
    DiskFileStamp stamp;
};

struct ReadOnlySearchSource::Preparation {
    enum class Execution { Pending, Running, Skipped };
    std::atomic<Execution> execution{Execution::Pending};
    std::atomic_bool workerExited{false};
    std::mutex exitMutex;
    std::condition_variable exited;
    std::unique_ptr<Region> region; // worker-owned until workerExited acquire
    // All remaining fields, including the QPointer, are UI-thread only.
    QPointer<ReadOnlySearchSource> owner;
    QString path;
    DiskFileStamp expected;
    qint64 id = 0;
    struct Terminal { qint64 id; AsyncTaskState state; QString error; };
    QVector<Terminal> earlyTerminals;
    Terminal terminal{0, AsyncTaskState::Queued, {}};
    QMetaObject::Connection terminalConnection;
    bool terminalSeen = false;
    bool delivered = false;
};

ReadOnlySearchSource::ReadOnlySearchSource(QObject *parent)
    : QObject(parent), m_findAll(new LargeFileSearch(this)),
      m_navigation(new LargeFileSearch(this))
{
    connect(m_findAll, &LargeFileSearch::sourceBecameIdle,
            this, &ReadOnlySearchSource::releaseMappingIfIdle);
    connect(m_navigation, &LargeFileSearch::sourceBecameIdle,
            this, &ReadOnlySearchSource::releaseMappingIfIdle);
}

ReadOnlySearchSource::~ReadOnlySearchSource() { close(); }
bool ReadOnlySearchSource::isReady() const { return bool(m_region); }
qint64 ReadOnlySearchSource::preparationTaskId() const
{
    return m_preparation ? m_preparation->id : 0;
}

void ReadOnlySearchSource::settlePreparation(const std::shared_ptr<Preparation> &job)
{
    if (job->delivered || !job->terminalSeen) return;
    if (job->execution.load(std::memory_order_acquire) != Preparation::Execution::Skipped
        && !job->workerExited.load(std::memory_order_acquire)) return;
    job->delivered = true;
    QObject::disconnect(job->terminalConnection);
    // Even synchronous taskAdded cancellation must let ensureReady return and
    // consumers connect before delivering the single terminal notification.
    QMetaObject::invokeMethod(&AsyncTaskManager::instance(), [job] {
        if (job->owner) job->owner->installPreparation(job);
    }, Qt::QueuedConnection);
}

bool ReadOnlySearchSource::ensureReady(const QString &path, const PersistenceReadPolicy &policy,
                                      const DiskFileStamp &expected, QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_closing) {
        if (error) *error = tr("文件查找源正在关闭，请稍后重试。");
        return false;
    }
    if ((m_region && !sameDiskFileStamp(m_region->stamp, expected))
        || (m_preparation && !sameDiskFileStamp(m_preparation->expected, expected))) {
        if (error) *error = tr("旧文件的查找尚在结束，请稍后重新查找。");
        return false;
    }
    if (m_region || m_preparation) return true;
    if (path.isEmpty()) {
        if (error) *error = tr("当前大文件没有可搜索的磁盘源。");
        return false;
    }
    auto *dispatcher = &AsyncTaskManager::instance();
    const QPointer<ReadOnlySearchSource> self(this);
    auto job = std::make_shared<Preparation>();
    job->owner = this;
    job->path = path;
    job->expected = expected;
    m_preparation = job;
    const auto observe = [job](const Preparation::Terminal &terminal) {
        if (job->terminalSeen) return;
        job->terminal = terminal;
        job->terminalSeen = true;
        // A Manager terminal cannot prove a worker never entered. The claim
        // decides who owns cleanup, not its possibly delayed UI state.
        auto expected = Preparation::Execution::Pending;
        job->execution.compare_exchange_strong(expected, Preparation::Execution::Skipped,
                                                 std::memory_order_acq_rel);
        settlePreparation(job);
    };
    job->terminalConnection = connect(dispatcher, &AsyncTaskManager::taskFinished,
        dispatcher, [job, dispatcher, observe](qint64 id, AsyncTaskState state) {
            const auto error = dispatcher->taskById(id).error;
            Preparation::Terminal terminal{id, state,
                error.userMessage.isEmpty() ? error.message : error.userMessage};
            if (job->id == 0) job->earlyTerminals.append(std::move(terminal));
            else if (job->id == id) observe(terminal);
        });
    const qint64 id = dispatcher->submit(AsyncTaskType::Other, AsyncTaskPriority::High,
        tr("准备文件查找：%1").arg(QFileInfo(path).fileName()),
        tr("准备只读文件的搜索字节源"),
        [job, path, policy, expected, dispatcher](qint64 taskId, AsyncCancelPtr cancel,
                               const auto &progress, const auto &reportError) {
            auto expectedExecution = Preparation::Execution::Pending;
            if (!job->execution.compare_exchange_strong(expectedExecution, Preparation::Execution::Running,
                                                        std::memory_order_acq_rel)) return;
            const auto exited = qScopeGuard([job, dispatcher] {
                {
                    std::lock_guard<std::mutex> lock(job->exitMutex);
                    job->workerExited.store(true, std::memory_order_release);
                    job->exited.notify_all();
                }
                QMetaObject::invokeMethod(dispatcher, [job] { settlePreparation(job); },
                                          Qt::QueuedConnection);
            });
            const auto canceled = [&] { return cancel && cancel->load(std::memory_order_acquire); };
            if (canceled()) return;
            auto region = std::make_unique<Region>();
            AsyncTaskManager::instance().updateTaskPhase(taskId,
                QObject::tr("准备文件映射"), 1, 1, 0, -1);
            const auto admitted = openOrdinaryReadSource(region->file, path, policy, &expected);
            const bool opened = admitted.isReady();
            region->stamp = admitted.stamp;
            const qint64 size = region->file.size();
            if (opened && !canceled())
                region->base = size == 0 ? &region->empty : region->file.map(0, size);
            if (canceled()) return;
            if (!region->base) {
                AsyncErrorInfo failure;
                failure.message = opened ? region->file.errorString() : admitted.error;
                failure.userMessage = admitted.status == OrdinaryReadStatus::SecureType
                    ? QObject::tr("源文件已变成安全文件，已停止普通读取。请从“打开”重新打开该文件。")
                    : QObject::tr("无法准备文件进行查找；源文件可能已变化，请重新打开文件后重试。");
                reportError(failure);
                return;
            }
            // This source has no display/index/prefetch/throughput readers.
            // Preserve the old windowed search's bounded context semantics.
            region->lines.setIncompleteWithFileSize(size);
            progress(size, size);
            job->region = std::move(region);
        }, 0, QStringLiteral("Mmap"));
    job->id = id;
    if (!self || self->m_preparation != job) {
        dispatcher->cancel(id);
        return id > 0;
    }
    for (const auto &terminal : std::as_const(job->earlyTerminals)) {
        if (terminal.id == id) observe(terminal);
    }
    job->earlyTerminals.clear();
    if (id <= 0) observe({id, AsyncTaskState::Failed, tr("文件查找准备未能启动。")});
    return id > 0;
}

void ReadOnlySearchSource::installPreparation(const std::shared_ptr<Preparation> &job)
{
    if (m_preparation != job) return;
    if (job->terminal.state != AsyncTaskState::Finished || !job->region) {
        m_preparation.reset();
        job->region.reset();
        emit failed(job->terminal.state == AsyncTaskState::Canceled ? tr("已取消。")
            : (job->terminal.error.isEmpty() ? tr("无法建立大文件搜索映射。") : job->terminal.error));
        return;
    }
    DiskFileStamp current;
    captureDiskFileStamp(job->path, &current);
    if (!sameDiskFileStamp(current, job->region->stamp)) {
        m_preparation.reset();
        job->region.reset();
        emit failed(tr("准备期间源文件已变化；请等待只读视图刷新后重新查找。"));
        return;
    }
    m_region = std::move(job->region);
    const bool allBound = m_findAll->rebindIdleSource(
        m_region->base, m_region->file.size(), &m_region->lines, nullptr);
    const bool navigationBound = m_navigation->rebindIdleSource(
        m_region->base, m_region->file.size(), &m_region->lines, nullptr);
    Q_ASSERT(allBound && navigationBound);
    Q_UNUSED(allBound);
    Q_UNUSED(navigationBound);
    // Delivery still owns the source: an early receiver may reject its query
    // synchronously before the next ready receiver has reserved its lane.
    const QPointer<ReadOnlySearchSource> self(this);
    emit ready();
    if (!self || m_preparation != job) return;
    m_preparation.reset();
    // Synchronous nested-loop waiters resume as ready unwinds. One queued
    // check also releases preparation whose consumers have since retired.
    QMetaObject::invokeMethod(this, [this] { releaseMappingIfIdle(); }, Qt::QueuedConnection);
}

void ReadOnlySearchSource::releaseMappingIfIdle()
{
    if (!m_region || m_preparation || m_findAll->sourceInUse() || m_navigation->sourceInUse()) return;
    m_findAll->rebindIdleSource(nullptr, 0, nullptr, nullptr);
    m_navigation->rebindIdleSource(nullptr, 0, nullptr, nullptr);
    m_region.reset();
}

void ReadOnlySearchSource::close()
{
    const QPointer<ReadOnlySearchSource> self(this);
    const bool previousClosing = m_closing;
    m_closing = true;
    if (auto job = std::exchange(m_preparation, {})) {
        job->owner.clear();
        QObject::disconnect(job->terminalConnection);
        auto expected = Preparation::Execution::Pending;
        job->execution.compare_exchange_strong(expected, Preparation::Execution::Skipped,
                                                 std::memory_order_acq_rel);
        if (job->id > 0) AsyncTaskManager::instance().cancel(job->id);
        if (job->execution.load(std::memory_order_acquire) != Preparation::Execution::Skipped) {
            std::unique_lock<std::mutex> lock(job->exitMutex);
            job->exited.wait(lock, [&job] {
                return job->workerExited.load(std::memory_order_acquire);
            });
        }
        job->region.reset();
    }
    if (!self) return;
    m_findAll->cancelAndWait();
    if (!self) return;
    m_navigation->cancelAndWait();
    if (!self) return;
    releaseMappingIfIdle();
    m_closing = previousClosing;
}
