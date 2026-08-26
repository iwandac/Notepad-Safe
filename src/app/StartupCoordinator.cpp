#include "StartupCoordinator.h"

#include "MainWindow.h"

#include <QMetaObject>

StartupCoordinator::StartupCoordinator(QObject *parent)
    : QObject(parent)
{
}

void StartupCoordinator::observe(MainWindow *window, QStringList externalFiles)
{
    window_ = window;
    externalFiles_ = std::move(externalFiles);
    connect(window, &MainWindow::firstPaintCompleted, this, [this]() {
        paintObserved_ = true;
        scheduleIfReady();
    });
}

void StartupCoordinator::releasePostPaintWork()
{
    released_ = true;
    scheduleIfReady();
}

void StartupCoordinator::scheduleIfReady()
{
    if (scheduled_ || !paintObserved_ || !released_ || !window_)
        return;
    scheduled_ = true;
    QMetaObject::invokeMethod(this, [this]() {
        if (!window_)
            return;
        if (!externalFiles_.isEmpty())
            window_->openFilesFromExternal(externalFiles_);
        window_->deferredRestoreSession();
    }, Qt::QueuedConnection);
}
