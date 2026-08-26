#include "AsyncTask.h"

#include <limits>

QString AsyncErrorInfo::toString() const
{
    QString out;
    if (!sourceFile.isEmpty()) {
        out = QStringLiteral("%1:%2  ").arg(sourceFile).arg(sourceLine);
    }
    out += message;
    if (!stackFrames.isEmpty()) {
        out += QStringLiteral("\n堆栈:\n  ");
        out += stackFrames.join(QLatin1String("\n  "));
    }
    return out;
}

qint64 AsyncTask::elapsedMs() const
{
    QDateTime from = startedAt.isValid() ? startedAt : createdAt;
    QDateTime to   = finishedAt.isValid() ? finishedAt : QDateTime::currentDateTime();
    if (!from.isValid()) return 0;
    return from.msecsTo(to);
}

qint64 AsyncTask::estimatedRemainingMs() const
{
    if (state != AsyncTaskState::Running || progressTotal <= 0
        || progressDone <= 0 || progressDone >= progressTotal) {
        return -1;
    }
    const QDateTime phaseStart = phaseStartedAt.isValid()
        ? phaseStartedAt : startedAt;
    const qint64 elapsed = phaseStart.isValid()
        ? phaseStart.msecsTo(QDateTime::currentDateTime()) : elapsedMs();
    // A single early sample produced absurd multi-year estimates for scans
    // that had not yet left their setup phase. Require time, sample count and
    // at least 1% of the current phase before publishing a linear ETA.
    if (elapsed < 3000 || progressSamples < 3
        || progressDone * 100 < progressTotal) {
        return -1;
    }
    const long double remaining = static_cast<long double>(elapsed)
        * static_cast<long double>(progressTotal - progressDone)
        / static_cast<long double>(progressDone);
    if (remaining < 0.0L
        || remaining > static_cast<long double>(std::numeric_limits<qint64>::max())) {
        return -1;
    }
    return qint64(remaining);
}

QString AsyncTask::stateText() const
{
    switch (state) {
    case AsyncTaskState::Queued:   return QObject::tr("排队中");
    case AsyncTaskState::Running:  return QObject::tr("运行中");
    case AsyncTaskState::Finished: return QObject::tr("完成");
    case AsyncTaskState::Failed:   return QObject::tr("失败");
    case AsyncTaskState::Canceled: return QObject::tr("已取消");
    }
    return QObject::tr("未知");
}

QString AsyncTask::priorityText() const
{
    switch (priority) {
    case AsyncTaskPriority::Highest: return QObject::tr("最高(安全/导航)");
    case AsyncTaskPriority::High:    return QObject::tr("高(用户操作)");
    case AsyncTaskPriority::Normal:  return QObject::tr("普通(补扫)");
    case AsyncTaskPriority::Low:     return QObject::tr("低(预扫)");
    case AsyncTaskPriority::Lowest:  return QObject::tr("最低");
    }
    return QObject::tr("-");
}

QString AsyncTask::typeText() const
{
    switch (type) {
    case AsyncTaskType::LineIndexBuild: return QObject::tr("行索引构建");
    case AsyncTaskType::LargeFileSearch:return QObject::tr("大文件搜索");
    case AsyncTaskType::EncodingConversion: return QObject::tr("编码转换");
    case AsyncTaskType::SessionSnapshot: return QObject::tr("恢复快照");
    case AsyncTaskType::SecureOpen: return QObject::tr("安全打开");
    case AsyncTaskType::SecureSave: return QObject::tr("安全保存");
    case AsyncTaskType::SecureIdentity: return QObject::tr("安全身份");
    case AsyncTaskType::VaultOperation: return QObject::tr("账户安全操作");
    default: return QObject::tr("其他");
    }
}
