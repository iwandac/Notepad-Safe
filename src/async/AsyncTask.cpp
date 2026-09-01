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

AsyncTaskUserText AsyncTask::userText() const
{
    const auto duration = [](qint64 milliseconds) {
        const qint64 seconds = qMax<qint64>(1, (milliseconds + 999) / 1000);
        if (seconds < 60) return QObject::tr("%1 秒").arg(seconds);
        const qint64 minutes = (seconds + 59) / 60;
        if (minutes < 60) return QObject::tr("%1 分钟").arg(minutes);
        return QObject::tr("%1 小时 %2 分钟").arg(minutes / 60).arg(minutes % 60);
    };
    AsyncTaskUserText out;
    out.title = title;
    out.status = stateText();
    const qint64 remaining = estimatedRemainingMs();
    if (remaining >= 0)
        out.status += QObject::tr(" · 约剩 %1").arg(duration(remaining));
    QString phase;
    if (!progressPhase.isEmpty()) {
        phase = progressPhaseCount > 0
            ? QObject::tr("%1/%2 %3").arg(progressPhaseIndex).arg(progressPhaseCount).arg(progressPhase)
            : progressPhase;
    }
    const int precise = percentHundredths();
    if (precise >= 0)
        out.progress = QString::number(precise / 100.0, 'f', 2) + QLatin1Char('%');
    else
        out.progress = state == AsyncTaskState::Queued || state == AsyncTaskState::Running
            ? QObject::tr("处理中") : stateText();
    if (!phase.isEmpty()) out.progress.prepend(phase + QStringLiteral(" · "));
    if (state == AsyncTaskState::Failed) {
        out.failure = error.userMessage.trimmed();
        if (out.failure.isEmpty())
            out.failure = QObject::tr("此任务未完成，请查看技术详情后重试。");
    }
    out.summary = out.title + QLatin1Char('\n') + out.status
        + QStringLiteral(" · ") + out.progress;
    if (!out.failure.isEmpty()) out.summary += QLatin1Char('\n') + out.failure;
    return out;
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
