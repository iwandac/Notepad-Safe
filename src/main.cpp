#include "MainWindow.h"
#include "AppComposition.h"
#include "SingleInstanceCoordinator.h"
#include "StartupCoordinator.h"
#include "StartupTrace.h"
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStringList>
#include <QFileInfo>


namespace {

int runApplication(QApplication &app, StartupTrace &trace, bool &primary)
{
    // Keep menu feedback immediately readable without an animated transition.
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
    app.setApplicationName("Notepad Safe");
    app.setOrganizationName("NotepadSafe");

    // 收集命令行中的文件路径（拖到图标上/右键打开方式/命令行传参）
    QStringList files;
    const QStringList args = app.arguments();
    // args[0] 是可执行文件本身，跳过
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        // 过滤 Qt 的开关参数（如 -stylesheet 等）
        if (a.startsWith("-")) continue;
        QFileInfo fi(a);
        const QString canonical = fi.canonicalFilePath();
        files << QDir::cleanPath(canonical.isEmpty()
                                     ? fi.absoluteFilePath() : canonical);
    }

    // Claim the per-profile local endpoint before constructing MainWindow or
    // SessionManager. Only the primary process can therefore become a session
    // writer. A secondary sends a bounded path-only request and exits.
    SingleInstanceCoordinator singleInstance;
    const auto instanceResult = singleInstance.start(files);
    if (instanceResult.disposition
        == SingleInstanceCoordinator::StartResult::Disposition::Forwarded) {
        return 0;
    }
    if (instanceResult.disposition
        == SingleInstanceCoordinator::StartResult::Disposition::Failed) {
        QMessageBox::critical(nullptr, QStringLiteral("无法连接到现有窗口"),
                              instanceResult.userMessage);
        return instanceResult.exitCode;
    }

    primary = true;
    AppComposition composition;
    trace.record(StartupTrace::Stage::CompositionReady);
    MainWindow w(nullptr, &composition);
    trace.record(StartupTrace::Stage::WindowReady);
    if (trace.enabled()) {
        QObject::connect(&w, &MainWindow::firstPaintCompleted, &w, [&] {
            trace.record(StartupTrace::Stage::FirstPaint);
            trace.recordPaintCounts({composition.heavyFactoryCalls(),
                composition.documentTools().instantiatedCount(),
                composition.documentProviders().instantiatedCount(),
                composition.persistenceAdapters().instantiatedCount(),
                composition.inferenceProviders().instantiatedCount()});
        }, Qt::DirectConnection);
        QObject::connect(&w, &MainWindow::sessionRestoreCompleted, &w, [&trace] {
            trace.record(StartupTrace::Stage::RestoreCompleted);
        }, Qt::DirectConnection);
    }
    const auto deliverRequest = [&w](const QStringList &paths) {
        if (!paths.isEmpty())
            w.openFilesFromExternal(paths);
        if (w.isMinimized())
            w.showNormal();
        w.raise();
        w.activateWindow();
    };
    QObject::connect(&singleInstance, &SingleInstanceCoordinator::requestReceived,
                     &w, deliverRequest);
    QObject::connect(&w, &MainWindow::sessionRestoreCompleted,
                     &singleInstance,
                     &SingleInstanceCoordinator::setDeliveryReady);
    QObject::connect(&w, &MainWindow::shutdownCommitted,
                     &singleInstance,
                     &SingleInstanceCoordinator::stopAcceptingAndDrain,
                     Qt::DirectConnection);
    StartupCoordinator startup;
    if (trace.enabled()) {
        QObject::connect(&startup, &StartupCoordinator::postPaintIdleTurnCompleted,
                         &startup, [&trace] {
            trace.record(StartupTrace::Stage::PostPaintIdleTurn);
        }, Qt::DirectConnection);
    }
    startup.observe(&w, files);
    trace.record(StartupTrace::Stage::ShowBegin);
    w.show();
    trace.record(StartupTrace::Stage::ShowReturned);
    // StartupCoordinator waits for the first real paint, yields one clean
    // input-capable event-loop turn, then starts external-open/session work.
    // No manual event pumping, fixed delay, provider construction or KDBX
    // initialization belongs on the process-entry critical path.
    startup.releasePostPaintWork();

    const int exitCode = app.exec();
    trace.record(StartupTrace::Stage::EventLoopFinished);
    // Stop and join the forwarding worker while MainWindow/SessionManager are
    // still alive. The coordinator keeps QLockFile ownership until its later
    // destructor, so no replacement writer can overlap their teardown.
    singleInstance.stopAccepting();
    return exitCode;
}

} // namespace

int main(int argc, char *argv[])
{
    StartupTrace trace;
    QApplication app(argc, argv);
    trace.record(StartupTrace::Stage::AppReady);
    bool primary = false;
    const int exitCode = runApplication(app, trace, primary);
    // All trace callback contexts and primary business owners have now ended;
    // QApplication remains alive. Export failure never changes business exit.
    if (primary)
        trace.exportToStdout(exitCode);
    return exitCode;
}
