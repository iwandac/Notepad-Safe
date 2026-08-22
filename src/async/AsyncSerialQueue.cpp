#include "AsyncSerialQueue.h"

#include <QCoreApplication>
#include <QThread>

AsyncSerialQueue &AsyncSerialQueue::security()
{
    // Intentionally process-lifetime. AsyncTaskManager is also a singleton and
    // may still deliver a queued completion during application teardown.
    static auto *queue = new AsyncSerialQueue;
    return *queue;
}

AsyncSerialQueue &AsyncSerialQueue::session()
{
    static auto *queue = new AsyncSerialQueue;
    return *queue;
}

AsyncSerialQueue::AsyncSerialQueue()
    : QObject(QCoreApplication::instance())
{
}

void AsyncSerialQueue::enqueue(Request request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    pending_.enqueue(std::move(request));
    startNext();
}

void AsyncSerialQueue::startNext()
{
    if (activeTaskId_ > 0 || pending_.isEmpty())
        return;

    Request request = std::move(pending_.head());
    pending_.dequeue();
    const auto completed = std::move(request.completed);

    activeTaskId_ = AsyncTaskManager::instance().submit(
        request.type, request.priority, request.title, QString(),
        std::move(request.worker), 0, request.categoryTag, -1,
        request.cancellable);
    const qint64 taskId = activeTaskId_;

    connect(&AsyncTaskManager::instance(), &AsyncTaskManager::taskFinished,
            this,
            [this, taskId, completed](qint64 finishedId, AsyncTaskState state) {
                if (finishedId != taskId)
                    return;
                disconnect(&AsyncTaskManager::instance(), nullptr, this, nullptr);
                activeTaskId_ = 0;
                if (completed)
                    completed(state);
                startNext();
            });
}
