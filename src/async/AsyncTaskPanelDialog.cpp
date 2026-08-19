#include "AsyncTaskPanelDialog.h"
#include "AsyncTaskManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListView>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QModelIndex>
#include <QApplication>
#include <QStyle>
#include <QMouseEvent>
#include <QDateTime>

namespace {
// ====== 每一行：标题 + 状态标签 + 进度条 + 取消按钮（Queued/Running 时显示）+ 错误红色小字
class AsyncTaskItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        QStyle *style = qApp->style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, nullptr);

        const QRect r = option.rect.adjusted(8, 6, -8, -6);
        const QString title     = index.data(AsyncTaskManager::TaskTitleRole).toString();
        const QString typeT     = index.data(AsyncTaskManager::TaskTypeTextRole).toString();
        const QString stateT    = index.data(AsyncTaskManager::TaskStateTextRole).toString();
        const QString prioT     = index.data(AsyncTaskManager::TaskPriorityTextRole).toString();
        const int percent       = index.data(AsyncTaskManager::TaskPercentRole).toInt();
        const qint64 done       = index.data(AsyncTaskManager::TaskProgressDoneRole).toLongLong();
        const qint64 total      = index.data(AsyncTaskManager::TaskProgressTotalRole).toLongLong();
        const int state         = index.data(AsyncTaskManager::TaskStateRole).toInt();
        const auto taskState    = static_cast<AsyncTaskState>(state);
        const QString err       = index.data(AsyncTaskManager::TaskErrorRole).toString();
        const qint64 elapsedMs = index.data(AsyncTaskManager::TaskElapsedMsRole).toLongLong();

        QFont f = painter->font();
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(option.palette.color((option.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(r.topLeft() + QPoint(0, painter->fontMetrics().ascent()), title);

        f.setBold(false);
        f.setPointSize(qMax(8, f.pointSize() - 1));
        painter->setFont(f);
        painter->setPen(option.palette.color(QPalette::Mid));
        int tw = painter->fontMetrics().horizontalAdvance(typeT + "  |  " + prioT);
        painter->drawText(r.topRight() + QPoint(-tw, painter->fontMetrics().ascent() - 2),
                          QStringLiteral("%1  |  %2").arg(typeT, prioT));

        // 第二行：状态 pill + 耗时
        QRect line2 = r.adjusted(0, painter->fontMetrics().lineSpacing() + 4, 0, -painter->fontMetrics().lineSpacing() - 22);
        line2.setHeight(painter->fontMetrics().lineSpacing());
        const QString pillText = QStringLiteral(" %1  ·  %2ms  ").arg(stateT).arg(elapsedMs);
        QColor pillBg;
        QColor pillFg;
        switch (taskState) {
        case AsyncTaskState::Running:  pillBg = QColor(122, 90, 248, 28); pillFg = QColor(122, 90, 248); break;
        case AsyncTaskState::Finished: pillBg = QColor(46, 158, 106, 22);  pillFg = QColor(46, 158, 106); break;
        case AsyncTaskState::Failed:   pillBg = QColor(228, 83, 74, 22);   pillFg = QColor(228, 83, 74);  break;
        case AsyncTaskState::Canceled: pillBg = QColor(242, 176, 75, 22);  pillFg = QColor(196, 140, 50); break;
        case AsyncTaskState::Queued:
        default:                       pillBg = QColor(107, 115, 134, 22); pillFg = option.palette.color(QPalette::Mid).darker(120); break;
        }
        QRect pillR = QRect(line2.topLeft(), QSize(painter->fontMetrics().horizontalAdvance(pillText) + 16, painter->fontMetrics().height() + 4));
        painter->setPen(Qt::NoPen);
        painter->setBrush(pillBg);
        painter->drawRoundedRect(pillR, 6, 6);
        painter->setPen(pillFg);
        painter->drawText(pillR, Qt::AlignCenter, pillText);

        // 进度条：右侧 82 像素留作取消按钮
        QRect progR = pillR;
        progR.setLeft(pillR.right() + 10);
        progR.setRight(r.right() - 82);
        progR.setHeight(painter->fontMetrics().height() + 4);
        progR.translate(0, -1);
        QStyleOptionProgressBar pbar;
        pbar.rect = progR;
        pbar.minimum = 0;
        const bool active = taskState == AsyncTaskState::Queued
                            || taskState == AsyncTaskState::Running;
        const bool knownProgress = total > 0 && percent >= 0;
        if (knownProgress) {
            pbar.maximum = 100;
            pbar.progress = qBound(0, percent, 100);
            pbar.text = tr("%1%  %2 / %3").arg(percent).arg(done).arg(total);
        } else if (active) {
            pbar.maximum = 0;
            pbar.progress = 0;
            pbar.text = tr("处理中");
        } else {
            // A terminal task with unknown total is not busy. Keep a stable,
            // non-animated row instead of leaving an indeterminate stripe.
            pbar.maximum = 100;
            pbar.progress = taskState == AsyncTaskState::Finished ? 100 : 0;
            pbar.text = stateT;
        }
        pbar.textVisible = true;
        pbar.palette = option.palette;
        style->drawControl(QStyle::CE_ProgressBar, &pbar, painter);

        // 同一几何同时用于绘制和 editorEvent 命中；已结束任务不显示伪按钮。
        const bool cancellable = taskState == AsyncTaskState::Queued
                              || taskState == AsyncTaskState::Running;
        if (cancellable) {
            QRect cancelRect(r.right() - 74, pillR.top(), 70, pillR.height());
            QStyleOptionButton btn;
            btn.rect = cancelRect;
            btn.text = tr("取消");
            btn.state = QStyle::State_Enabled | QStyle::State_Raised;
            btn.palette = option.palette;
            style->drawControl(QStyle::CE_PushButton, &btn, painter);
        }

        // 第三行：若有错误，红色 1 行概要
        if (!err.isEmpty()) {
            const int thirdLineY = r.top() + painter->fontMetrics().lineSpacing() * 2 + 10;
            QRect errR(r.left(), thirdLineY, r.width() - 90, painter->fontMetrics().lineSpacing() * 2);
            painter->setPen(QColor(228, 83, 74));
            QString oneLine = err.section(QLatin1Char('\n'), 0, 0).left(140);
            if (err.section(QLatin1Char('\n'), 0, 0).size() > 140)
                oneLine += QChar(0x2026);
            painter->drawText(errR, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, oneLine);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        const QString err = index.data(AsyncTaskManager::TaskErrorRole).toString();
        int base = option.fontMetrics.lineSpacing() * 3 + 26;
        if (!err.isEmpty()) base += option.fontMetrics.lineSpacing() * 2;
        return QSize(s.width(), base);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        Q_UNUSED(model);
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent*>(event);
            const QRect r = option.rect.adjusted(8, 6, -8, -6);
            QFont compactFont = option.font;
            compactFont.setPointSize(qMax(8, compactFont.pointSize() - 1));
            const QFontMetrics metrics(compactFont);
            const int pillTop = r.top() + metrics.lineSpacing() + 4;
            QRect cancelRect(r.right() - 74, pillTop, 70, metrics.height() + 4);
            if (cancelRect.contains(me->pos())) {
                const int st = index.data(AsyncTaskManager::TaskStateRole).toInt();
                if (st == static_cast<int>(AsyncTaskState::Queued) || st == static_cast<int>(AsyncTaskState::Running)) {
                    qint64 id = index.data(AsyncTaskManager::TaskIdRole).toLongLong();
                    AsyncTaskManager::instance().cancel(id);
                    return true;
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};
}

AsyncTaskPanelDialog::AsyncTaskPanelDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("后台任务"));
    setObjectName(QStringLiteral("backgroundTaskPanel"));
    resize(860, 580);
    setModal(false);  // 非模态 — 不阻塞任何操作

    auto *rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(14, 14, 14, 14);
    rootLay->setSpacing(12);

    // 总览头
    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);
    auto makePill = [](QLabel *&out, const QString &label) {
        out = new QLabel(label);
        out->setStyleSheet(QStringLiteral(
            "QLabel{padding:4px 12px;border-radius:999px;"
            "background:palette(alternate-base);border:1px solid palette(midlight);"
            "color:palette(text);font:600 12px 'Segoe UI', system-ui;}"));
    };
    makePill(m_summaryQueued,  tr("排队中: -"));
    makePill(m_summaryRunning, tr("运行中: -"));
    makePill(m_summaryDone,    tr("最近完成: -"));
    makePill(m_summaryFailed,  tr("失败: -"));
    makePill(m_summaryThreads, tr("线程: -/-"));
    m_summaryQueued->setObjectName(QStringLiteral("taskSummaryQueued"));
    m_summaryRunning->setObjectName(QStringLiteral("taskSummaryRunning"));
    m_summaryDone->setObjectName(QStringLiteral("taskSummaryFinished"));
    m_summaryFailed->setObjectName(QStringLiteral("taskSummaryFailed"));
    m_summaryThreads->setObjectName(QStringLiteral("taskSummaryThreads"));
    topRow->addWidget(m_summaryQueued);
    topRow->addWidget(m_summaryRunning);
    topRow->addWidget(m_summaryDone);
    topRow->addWidget(m_summaryFailed);
    topRow->addWidget(m_summaryThreads);
    topRow->addStretch(1);
    rootLay->addLayout(topRow);

    auto *actionRow = new QHBoxLayout();
    actionRow->addStretch(1);
    m_cancelSelectedBtn = new QPushButton(tr("取消所选"));
    m_cancelAllBtn = new QPushButton(tr("取消所有"));
    m_clearBtn = new QPushButton(tr("清空历史"));
    m_closeBtn = new QPushButton(tr("关闭"));
    m_cancelSelectedBtn->setObjectName(QStringLiteral("cancelSelectedTaskButton"));
    m_cancelAllBtn->setObjectName(QStringLiteral("cancelAllTasksButton"));
    m_clearBtn->setObjectName(QStringLiteral("clearTaskHistoryButton"));
    m_closeBtn->setObjectName(QStringLiteral("closeTaskPanelButton"));
    actionRow->addWidget(m_cancelSelectedBtn);
    actionRow->addWidget(m_cancelAllBtn);
    actionRow->addWidget(m_clearBtn);
    actionRow->addWidget(m_closeBtn);
    rootLay->addLayout(actionRow);

    m_listView = new QListView(this);
    m_listView->setObjectName(QStringLiteral("backgroundTaskList"));
    m_listView->setAccessibleName(tr("后台任务列表"));
    m_delegate = new AsyncTaskItemDelegate(m_listView);
    m_listView->setItemDelegate(m_delegate);
    m_listView->setModel(&AsyncTaskManager::instance());
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setAlternatingRowColors(true);
    m_listView->setUniformItemSizes(false);
    rootLay->addWidget(m_listView, 3);

    m_detailsEdit = new QPlainTextEdit(this);
    m_detailsEdit->setObjectName(QStringLiteral("backgroundTaskDetails"));
    m_detailsEdit->setReadOnly(true);
    m_detailsEdit->setPlaceholderText(tr("点击上面任意任务，查看详细描述与异常堆栈"));
    m_detailsEdit->setMaximumHeight(180);
    rootLay->addWidget(m_detailsEdit, 2);

    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::hide);
    connect(m_cancelSelectedBtn, &QPushButton::clicked,
            this, &AsyncTaskPanelDialog::onCancelSelected);
    connect(m_cancelAllBtn, &QPushButton::clicked,
            this, &AsyncTaskPanelDialog::onCancelAll);
    connect(m_clearBtn, &QPushButton::clicked, this, &AsyncTaskPanelDialog::onClearHistory);
    connect(m_listView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &AsyncTaskPanelDialog::onCurrentRowChanged);

    auto &mgr = AsyncTaskManager::instance();
    connect(&mgr, &AsyncTaskManager::summaryChanged,   this, &AsyncTaskPanelDialog::refreshHeaderSummary);
    connect(&mgr, &AsyncTaskManager::threadCountChanged,this, &AsyncTaskPanelDialog::refreshThreadOccupancy);
    connect(&mgr, &QAbstractItemModel::modelReset,
            this, &AsyncTaskPanelDialog::updateActionStates);
    connect(&mgr, &QAbstractItemModel::rowsInserted,
            this, &AsyncTaskPanelDialog::updateActionStates);
    connect(&mgr, &QAbstractItemModel::rowsRemoved,
            this, &AsyncTaskPanelDialog::updateActionStates);
    // 初始同步一次
    refreshHeaderSummary(mgr.queuedCount(), mgr.runningCount(), 0);
    refreshThreadOccupancy(mgr.activeThreadCount(), mgr.maximumThreadCount());
    updateActionStates();
}

AsyncTaskPanelDialog::~AsyncTaskPanelDialog() = default;

void AsyncTaskPanelDialog::refreshHeaderSummary(int queued, int running, int finishedRecent)
{
    m_summaryQueued->setText(tr("排队中: %1").arg(queued));
    m_summaryRunning->setText(tr("运行中: %1").arg(running));
    m_summaryDone->setText(tr("最近完成: %1").arg(finishedRecent));
    m_summaryFailed->setText(tr("失败: %1").arg(
        AsyncTaskManager::instance().failedCount()));
    updateActionStates();
}

void AsyncTaskPanelDialog::refreshThreadOccupancy(int runningThreads, int maxThreads)
{
    m_summaryThreads->setText(maxThreads > 0
        ? tr("线程: %1/%2").arg(runningThreads).arg(maxThreads)
        : tr("活跃线程: %1").arg(runningThreads));
}

void AsyncTaskPanelDialog::onCurrentRowChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    if (!current.isValid()) {
        m_detailsEdit->clear();
        updateActionStates();
        return;
    }
    auto &mgr = AsyncTaskManager::instance();
    qint64 id = current.data(AsyncTaskManager::TaskIdRole).toLongLong();
    AsyncTask t = mgr.taskById(id);

    QString out;
    out += tr("任务ID: %1    批次ID: %2\n").arg(t.id).arg(t.batchId);
    out += tr("类型: %1    优先级: %2    状态: %3\n")
        .arg(t.typeText(), t.priorityText(), t.stateText());
    out += tr("分类标签: %1\n").arg(t.categoryTag.isEmpty() ? QLatin1String("-") : t.categoryTag);
    auto fmt = [](const QDateTime &dt){ return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("-"); };
    out += tr("创建: %1   启动: %2   结束: %3   耗时: %4ms\n")
        .arg(fmt(t.createdAt), fmt(t.startedAt), fmt(t.finishedAt)).arg(t.elapsedMs());
    out += tr("进度: %1 / %2  (%3%)\n")
        .arg(t.progressDone).arg(t.progressTotal)
        .arg(t.percent() < 0 ? QStringLiteral("-") : QString::number(t.percent()));
    out += QLatin1String("\n描述:\n");
    out += t.description.isEmpty() ? QStringLiteral("(无)") : t.description;
    if (!t.error.isNull()) {
        out += QStringLiteral("\n\n======== 异常 ========\n");
        out += t.error.toString();
    }
    m_detailsEdit->setPlainText(out);
    updateActionStates();
}

void AsyncTaskPanelDialog::onCancelSelected()
{
    const QModelIndex current = m_listView->currentIndex();
    if (!current.isValid())
        return;
    const int state = current.data(AsyncTaskManager::TaskStateRole).toInt();
    if (state != static_cast<int>(AsyncTaskState::Queued)
        && state != static_cast<int>(AsyncTaskState::Running)) {
        return;
    }
    AsyncTaskManager::instance().cancel(
        current.data(AsyncTaskManager::TaskIdRole).toLongLong());
}

void AsyncTaskPanelDialog::onCancelAll()
{
    AsyncTaskManager::instance().cancelAllByType();
}

void AsyncTaskPanelDialog::onClearHistory()
{
    AsyncTaskManager::instance().clearHistory();
    m_detailsEdit->clear();
}

void AsyncTaskPanelDialog::updateActionStates()
{
    const int active = AsyncTaskManager::instance().cancellableCount();
    m_cancelAllBtn->setEnabled(active > 0);
    m_clearBtn->setEnabled(AsyncTaskManager::instance().rowCount() > active);

    const QModelIndex current = m_listView->currentIndex();
    const int state = current.isValid()
                          ? current.data(AsyncTaskManager::TaskStateRole).toInt()
                          : -1;
    m_cancelSelectedBtn->setEnabled(
        state == static_cast<int>(AsyncTaskState::Queued)
        || state == static_cast<int>(AsyncTaskState::Running));
}
