#include "MainWindow.h"
#include "AppComposition.h"
#include "StartupCoordinator.h"
#include <QApplication>
#include <QStringList>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
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
        if (fi.isFile())
            files << fi.absoluteFilePath();
    }

    AppComposition composition;
    MainWindow w(nullptr, &composition);
    StartupCoordinator startup;
    startup.observe(&w, files);
    w.show();
    // StartupCoordinator waits for the first real paint, yields one clean
    // input-capable event-loop turn, then starts external-open/session work.
    // No manual event pumping, fixed delay, provider construction or KDBX
    // initialization belongs on the process-entry critical path.
    startup.releasePostPaintWork();

    return app.exec();
}
