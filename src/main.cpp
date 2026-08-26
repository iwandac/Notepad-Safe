#include "MainWindow.h"
#include "AppComposition.h"
#include "StartupCoordinator.h"
#include <QApplication>
#include <QStringList>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QDebug>
#include <QGuiApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Notepad Safe");
    app.setOrganizationName("NotepadSafe");

    QElapsedTimer startupTimer;
    startupTimer.start();

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

    // ====================================================================
    //  启动秒开保障（修复"全程静默无提示直到全量加载才弹窗"的核心体验 bug）
    //
    //  · 问题根因：Qt 中 QWidget::show() 仅将 Show/Paint 事件放入事件队列（Low 优先级），
    //    如果紧接着事件循环中的第一份"工作"（zero-timer deferredRestoreSession
    //    + restoreNextSessionTab 里的 loadAutosave 同步读 100MB）阻塞 3~5s，
    //    则 DWM 合成器收不到首帧绘制消息，窗口完全不显示（用户以为双击没启动）。
    //
    //  · 解决方式：
    //    1) w.show() 后立刻 raise() + activateWindow()：要求窗口管理器把本窗置顶
    //    2) sync()：触发 Qt 到操作系统窗口系统的一次强制同步刷新
    //    3)两轮 processEvents(AllEvents)：让 Show/Paint/DWM 合成消息都被真正派发，
    //       确保 deferredRestoreSession 运行前，窗口帧（标题栏+空客户区+状态栏）已可见。
    // ====================================================================
    w.raise();
    w.activateWindow();
    QGuiApplication::sync();
    QApplication::processEvents(QEventLoop::AllEvents);
    QApplication::processEvents(QEventLoop::AllEvents);

    const qint64 msToVisible = startupTimer.elapsed();
    qDebug("[STARTUP][T=%6lld] 主窗口可见（标题栏+空客户区+状态栏已绘制完成）。"
           " 会话恢复开始前耗时 = %lld ms。「秒开验证」基准：此时窗口应对用户可见。",
           startupTimer.elapsed(), msToVisible);

    // ====================================================================
    //  启动会话恢复（v3 正确顺序：窗口可见 → 30ms 安全窗口让 DWM 合成首帧 → 开始恢复）
    //
    //  · 为什么不放在 MainWindow 构造里？ v2 的 zero-timer 会抢在 Paint 事件前运行，
    //    导致先同步跑完 RESTORE tab 流程才绘制窗口 → 用户看到「双击静默无提示」。
    //  · 为什么不在这里直接调用 w.deferredRestoreSession()？ 直接调用仍然在本次
    //    processEvents 上下文中，可能仍先于 DWM 合成的首帧 SwapBuffers。
    //  · 为什么 30ms？ 两个 Monitor 刷新周期（60Hz 16.6ms）+ 余量；
    //    此时用户已看到标题栏和空客户区（不会以为启动失败），状态栏随后显示
    //    "正在恢复会话 0/N" 提供明确进度反馈。
    // ====================================================================
    // Post-paint work is released only after the compatibility paint bridge
    // above has completed. StartupCoordinator observed the actual first paint;
    // no fixed delay can accidentally run restoration ahead of it.
    startup.releasePostPaintWork();

    return app.exec();
}
