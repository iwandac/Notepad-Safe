#ifndef STATUSBARTASKWIDGET_H
#define STATUSBARTASKWIDGET_H

#include <QWidget>
class QLabel;
class QToolButton;
class AsyncTaskPanelDialog;

/**
 * @brief 状态栏永久 widget —— 显示线程占用 + 任务数 + 点击弹出任务详情面板。
 *
 * 设计（经验 1382463 教训：避免"每次点击 new 对话框"内存持续增长）：
 *   - AsyncTaskPanelDialog 为成员持有 unique_ptr，首次点击时创建，后续 show/raise 复用。
 */
class StatusBarTaskWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StatusBarTaskWidget(QWidget *parent = nullptr);
    ~StatusBarTaskWidget() override;

public slots:
    void onThreadCountChanged(int running, int maxThreads);
    void onSummaryChanged(int queued, int running, int finished);

private slots:
    void onTogglePanel();

private:
    QLabel *m_threadLabel;        // "线程: 2/8"
    QToolButton *m_tasksBtn;      // "后台任务 1" — 点击弹出面板
    QScopedPointer<AsyncTaskPanelDialog> m_panel;
};

#endif // STATUSBARTASKWIDGET_H
