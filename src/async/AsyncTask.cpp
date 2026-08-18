#include "AsyncTask.h"

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
    case AsyncTaskPriority::Highest: return QObject::tr("最高(解密/交互)");
    case AsyncTaskPriority::High:    return QObject::tr("高(搜索/保存)");
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
    case AsyncTaskType::FileEncrypt:    return QObject::tr("文件加密");
    case AsyncTaskType::FileDecrypt:    return QObject::tr("文件解密");
    case AsyncTaskType::LargeFileSave:  return QObject::tr("大文件保存");
    default: return QObject::tr("其他");
    }
}
