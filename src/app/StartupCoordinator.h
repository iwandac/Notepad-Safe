#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class MainWindow;

class StartupCoordinator final : public QObject {
    Q_OBJECT
public:
    explicit StartupCoordinator(QObject *parent = nullptr);

    void observe(MainWindow *window, QStringList externalFiles);
    void releasePostPaintWork();

signals:
    // Emitted on a clean event-loop turn after the first real paint.  Startup
    // tests use this as the first-input responsiveness boundary; deferred
    // document/session work is queued only after this signal.
    void postPaintIdleTurnCompleted();

private:
    void scheduleIfReady();

    QPointer<MainWindow> window_;
    QStringList externalFiles_;
    bool paintObserved_ = false;
    bool released_ = false;
    bool scheduled_ = false;
};
