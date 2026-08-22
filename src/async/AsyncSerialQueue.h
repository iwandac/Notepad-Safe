#pragma once

#include "AsyncTaskManager.h"

#include <QObject>
#include <QQueue>
#include <functional>

// A thin serial admission queue for mutable security services. It does not own
// a thread: each command executes on the central AsyncThreadPool, and the next
// command is admitted only after the previous task reaches a terminal state.
class AsyncSerialQueue final : public QObject
{
public:
    struct Request {
        AsyncTaskType type = AsyncTaskType::Other;
        AsyncTaskPriority priority = AsyncTaskPriority::High;
        QString title;
        QString categoryTag;
        bool cancellable = false;
        AsyncTaskManager::WorkerFn worker;
        std::function<void(AsyncTaskState)> completed;
    };

    static AsyncSerialQueue &security();
    static AsyncSerialQueue &session();

    void enqueue(Request request);
    bool busy() const noexcept { return activeTaskId_ > 0 || !pending_.isEmpty(); }

private:
    AsyncSerialQueue();
    void startNext();

    QQueue<Request> pending_;
    qint64 activeTaskId_ = 0;
};
