#pragma once

#include "AsyncSerialQueue.h"
#include "VaultTypes.h"

#include <QEventLoop>
#include <QPointer>
#include <QProgressDialog>
#include <QSharedPointer>
#include <QWidget>
#include <stdexcept>

// Keeps existing synchronous UI flows intact while moving blocking crypto and
// KDBX I/O to the central framework. The nested loop processes painting and
// system events; application modality prevents re-entrant user commands.
inline VaultError runSerializedSecurityOperation(
    QWidget *parent,
    AsyncTaskType type,
    AsyncTaskPriority priority,
    const QString &title,
    const std::function<VaultError()> &operation,
    bool showProgress = true)
{
    struct SharedResult {
        VaultError error;
        bool completed = false;
    };
    auto result = QSharedPointer<SharedResult>::create();
    QEventLoop loop;
    QPointer<QProgressDialog> progress;
    if (showProgress) {
        progress = new QProgressDialog(title, QString(), 0, 0, parent);
        progress->setObjectName(QStringLiteral("securityOperationProgress"));
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(150);
        progress->setAutoClose(false);
        progress->setCancelButton(nullptr);
        progress->show();
    }

    AsyncSerialQueue::Request request;
    request.type = type;
    request.priority = priority;
    request.title = title;
    request.categoryTag = QStringLiteral("Security");
    request.cancellable = false;
    request.worker = [result, operation](qint64, AsyncCancelPtr,
                                         const auto &, const auto &) {
        result->error = operation();
        if (result->error.isError())
            throw std::runtime_error("secure operation failed");
    };
    request.completed = [result, &loop, progress](AsyncTaskState) {
        result->completed = true;
        if (progress)
            progress->close();
        loop.quit();
    };
    AsyncSerialQueue::security().enqueue(std::move(request));
    if (!result->completed)
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (progress)
        progress->deleteLater();
    return result->error;
}
