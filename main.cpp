#include "MainWindow.h"
#include <QApplication>
#include <QStringList>
#include <QFileInfo>
#include <QTimer>

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

    MainWindow w;
    w.show();

    // 显示后再处理外部文件：确保窗口可见且默认标签已初始化
    if (!files.isEmpty()) {
        QTimer::singleShot(50, [&w, files]() {
            w.openFilesFromExternal(files);
        });
    }

    return app.exec();
}
