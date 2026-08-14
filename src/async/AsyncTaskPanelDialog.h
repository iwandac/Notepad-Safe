#ifndef ASYNCTASKPANELDIALOG_H
#define ASYNCTASKPANELDIALOG_H

#include <QDialog>
class QListView;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QStyledItemDelegate;

/**
 * @brief 后台任务详情面板 — 状态/进度/取消/异常堆栈/批次 一览
 */
class AsyncTaskPanelDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AsyncTaskPanelDialog(QWidget *parent = nullptr);
    ~AsyncTaskPanelDialog() override;

public slots:
    void refreshHeaderSummary(int queued, int running, int finishedRecent);
    void refreshThreadOccupancy(int runningThreads, int maxThreads);

private slots:
    void onCurrentRowChanged(const QModelIndex &current, const QModelIndex &previous);
    void onClearHistory();

private:
    QLabel  *m_summaryQueued, *m_summaryRunning, *m_summaryDone, *m_summaryThreads;
    QPushButton *m_clearBtn, *m_closeBtn;
    QListView *m_listView;
    QPlainTextEdit *m_detailsEdit;
    QStyledItemDelegate *m_delegate;
};

#endif // ASYNCTASKPANELDIALOG_H
