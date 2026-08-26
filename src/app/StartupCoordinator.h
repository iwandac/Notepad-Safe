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

private:
    void scheduleIfReady();

    QPointer<MainWindow> window_;
    QStringList externalFiles_;
    bool paintObserved_ = false;
    bool released_ = false;
    bool scheduled_ = false;
};
