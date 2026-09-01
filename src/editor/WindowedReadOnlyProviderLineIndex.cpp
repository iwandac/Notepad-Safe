#include "WindowedReadOnlyProvider.h"

#include "AsyncTaskManager.h"
#include "ReadOnlyTextAnalysis.h"

#include <QPointer>

#include <functional>
#include <memory>
#include <utility>

namespace {

QString lineIndexSourceReadMessage(const OrdinaryReadResult &result)
{
    if (result.status == OrdinaryReadStatus::SecureType)
        return QObject::tr("源文件已变成安全文件，已停止普通读取。请从“打开”重新打开该文件。");
    if (result.status == OrdinaryReadStatus::Changed)
        return QObject::tr("源文件已在磁盘上变化，旧结果已失效；请等待只读视图刷新后重新查找。");
    if (result.status == OrdinaryReadStatus::PolicyUnavailable)
        return QObject::tr("未配置文件安全读取规则，已拒绝普通读取。请重新打开文件。");
    return QObject::tr("源文件暂时无法读取，旧结果已失效。请检查文件后重新打开。");
}

} // namespace

void WindowedReadOnlyProvider::cancelLineNumberIndex()
{
    ++m_lineNumberGeneration;
    const qint64 taskId = std::exchange(m_lineNumberTaskId, -1);
    QObject::disconnect(m_lineNumberFinishedConnection);
    m_lineNumberFinishedConnection = QMetaObject::Connection();
    m_lineNumberPending = false;
    m_characterCountPending = false;
    if (taskId >= 0) AsyncTaskManager::instance().cancel(taskId);
}

void WindowedReadOnlyProvider::settleLineNumberTask(qint64 taskId,
                                                     quint64 generation)
{
    if (taskId < 0 || m_lineNumberGeneration != generation
        || m_lineNumberTaskId != taskId)
        return;
    QObject::disconnect(m_lineNumberFinishedConnection);
    m_lineNumberFinishedConnection = QMetaObject::Connection();
    m_lineNumberTaskId = -1;
    m_lineNumberPending = false;
    const bool characterCountWasPending = m_characterCountPending;
    m_characterCountPending = false;
    if (characterCountWasPending)
        emit characterCountChanged(m_characterCount, false);
}

void WindowedReadOnlyProvider::startLineNumberIndex()
{
    const QPointer<WindowedReadOnlyProvider> guard(this);
    if (m_statisticsDeferred || m_presentation != Presentation::Text) return;
    cancelLineNumberIndex();
    if (!guard || m_binaryContent || m_path.isEmpty() || m_fileSize <= 0) return;

    m_lineBreakPrefix.clear();
    m_lineBreakPrefixEndsWithCr.clear();
    m_lineNumberPending = true;
    m_characterCount = -1;
    m_characterCountPending = true;
    const quint64 generation = ++m_lineNumberGeneration;
    emit characterCountChanged(-1, true);
    if (!guard || m_lineNumberGeneration != generation) return;
    configureProjectionMargin();

    const QString path = m_path;
    const qint64 size = m_fileSize;
    const TextEncoding encoding = m_textEncoding;
    const auto policy = m_readPolicy;
    const auto expected = sourceStamp();
    m_lineBreakPrefixEncoding = encoding;
    auto *dispatcher = &AsyncTaskManager::instance();
    // taskAdded is synchronous, so cancellation can publish a terminal before
    // submit() returns its id. Retain only that narrow-window evidence and
    // reconcile it against the actual id for this generation.
    const auto submittedTaskId = std::make_shared<qint64>(-1);
    const auto earlyTerminalIds = std::make_shared<QVector<qint64>>();
    m_lineNumberFinishedConnection = QObject::connect(
        dispatcher, &AsyncTaskManager::taskFinished, this,
        [guard, generation, submittedTaskId, earlyTerminalIds]
        (qint64 taskId, AsyncTaskState) {
            if (!guard || guard->m_lineNumberGeneration != generation) return;
            if (*submittedTaskId < 0) {
                earlyTerminalIds->append(taskId);
                return;
            }
            if (taskId == *submittedTaskId)
                guard->settleLineNumberTask(taskId, generation);
        });
    const qint64 submittedId = dispatcher->submit(
        AsyncTaskType::LineIndexBuild, AsyncTaskPriority::Normal,
        tr("分析大文件文本"), tr("单次后台扫描生成全局行号并统计 Unicode 字符。"),
        [guard, generation, path, size, encoding, policy, expected, dispatcher]
        (qint64, AsyncCancelPtr cancel,
         std::function<void(qint64, qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo &)> reportError) {
            auto result = ReadOnlyTextAnalysis::analyze(
                path, policy, expected, encoding, cancel.get(), reportProgress);
            if (result.status == ReadOnlyTextAnalysis::Result::Status::Canceled) return;
            if (result.status == ReadOnlyTextAnalysis::Result::Status::Failed) {
                AsyncErrorInfo error;
                error.message = QObject::tr("无法分析大文件文本：%1").arg(result.source.error);
                error.userMessage = lineIndexSourceReadMessage(result.source);
                if (reportError) reportError(error);
                QMetaObject::invokeMethod(dispatcher, [guard, generation, failure = result.source] {
                    if (!guard || guard->m_lineNumberGeneration != generation) return;
                    guard->m_lineNumberPending = false;
                    guard->m_characterCountPending = false;
                    guard->reportSourceFailure(failure);
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(
                dispatcher, [guard, generation, expected,
                             result = std::move(result), encoding]() mutable {
                    if (!guard || guard->m_lineNumberGeneration != generation
                        || !sameDiskFileStamp(guard->sourceStamp(), expected)
                        || !guard->validateCurrent() || !guard
                        || guard->m_lineNumberGeneration != generation)
                        return;
                    guard->m_lineBreakPrefix = std::move(result.lineBreakPrefix);
                    guard->m_lineBreakPrefixEndsWithCr =
                        std::move(result.lineBreakPrefixEndsWithCr);
                    guard->m_lineBreakPrefixEncoding = encoding;
                    guard->m_lineNumberPending = false;
                    guard->m_characterCount = result.characterCount;
                    guard->m_characterCountPending = false;
                    guard->configureProjectionMargin();
                    emit guard->characterCountChanged(result.characterCount, false);
                }, Qt::QueuedConnection);
        }, 0, QLatin1String("LineNumberIndex"), size);
    *submittedTaskId = submittedId;
    if (guard && m_lineNumberGeneration == generation) {
        m_lineNumberTaskId = submittedId;
        if (earlyTerminalIds->contains(submittedId))
            settleLineNumberTask(submittedId, generation);
    } else if (submittedId >= 0) {
        dispatcher->cancel(submittedId);
    }
}

void WindowedReadOnlyProvider::setStatisticsDeferred(bool deferred)
{
    if (m_statisticsDeferred == deferred) return;
    m_statisticsDeferred = deferred;
    const QPointer<WindowedReadOnlyProvider> self(this);
    const quint64 generation = m_lineNumberGeneration + 1;
    cancelLineNumberIndex();
    if (!self || m_lineNumberGeneration != generation || m_statisticsDeferred != deferred) return;
    m_lineBreakPrefix.clear();
    m_lineBreakPrefixEndsWithCr.clear();
    m_characterCount = -1;
    configureProjectionMargin();
    emit characterCountChanged(-1, false);
    if (self && !deferred && m_lineNumberGeneration == generation) startLineNumberIndex();
}
