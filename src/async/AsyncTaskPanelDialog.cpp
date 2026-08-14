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
        switch (static_cast<AsyncTaskState>(state)) {
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
        pbar.maximum = total > 0 ? 100 : 0;
        pbar.progress = qBound(0, percent, 100);
        if (percent < 0 || total <= 0) {
            pbar.maximum = 0; pbar.progress = 0; // 忙态
        }
        pbar.text = percent < 0 || total <= 0
            ? (total <= 0 ? tr("忙") : tr("%1 / -").arg(done))
            : tr("%1%  %2 / %3").arg(percent).arg(done).arg(total);
        pbar.textVisible = true;
        pbar.palette = option.palette;
        style->drawControl(QStyle::CE_ProgressBar, &pbar, painter);

        // 右上角：取消按钮（仅 Queued / Running 状态显示为可点击，已结束显示为灰色禁用占位）
        {
            const bool cancellable = (static_cast<AsyncTaskState>(state) == AsyncTaskState::Queued)
                                  || (static_cast<AsyncTaskState>(state) == AsyncTaskState::Running);
            QRect cancelRect(r.right() - 74, r.top() + 1, 70, painter->fontMetrics().height() + 8);
            QStyleOptionButton btn;
            btn.rect = cancelRect;
            btn.text = tr("取消");
            btn.state = QStyle::State_Enabled | (cancellable ? QStyle::State_Raised : (QStyle::State_ReadOnly | QStyle::State_Off));
            if (!cancellable) btn.state &= ~QStyle::State_Enabled;
            btn.palette = option.palette;
            style->drawControl(QStyle::CE_PushButton, &btn, painter);
        }

        // 第三行：若有错误，红色 1 行概要
        if (!err.isEmpty()) {
            const int thirdLineY = r.top() + painter->fontMetrics().lineSpacing() * 2 + 10;
            QRect errR(r.left(), thirdLineY, r.width() - 90, painter->fontMetrics().lineSpacing() * 2);
            painter->setPen(QColor(228, 83, 74));
            QString oneLine = err.left(140);
            if (err.size() > 140) oneLine += QLatin1String("…");
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
            QRect cancelRect(r.right() - 72, r.top(), 72, 26);
            if (cancelRect.contains(me->pos())) {
                const int st = index.data(AsyncTaskManager::TaskStateRole).toInt();
                if (st == static_cast<int>(AsyncTaskState::Queued) || st == static_cast<int>(AsyncTaskState::Running)) {
                    qint64 id = index.data(AsyncTaskManager::TaskIdRole).toLongLong();
                    AsyncTaskManager::instance().cancel(id);
                    return true;
                }
            }
        }
        // 每次绘制时顺便在取消按钮位置画文字 — 这里 paint 里不做，而在我们自定义 paint 里补：等一下直接在 paint 里追加按钮图
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};
}

AsyncTaskPanelDialog::AsyncTaskPanelDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("后台任务管理器"));
    resize(780, 560);
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
    makePill(m_summaryDone,    tr("已完成近: -"));
    makePill(m_summaryThreads, tr("线程: -/-"));
    topRow->addWidget(m_summaryQueued);
    topRow->addWidget(m_summaryRunning);
    topRow->addWidget(m_summaryDone);
    topRow->addWidget(m_summaryThreads);
    topRow->addStretch(1);
    m_clearBtn = new QPushButton(tr("清空历史"));
    m_closeBtn = new QPushButton(tr("关闭"));
    topRow->addWidget(m_clearBtn);
    topRow->addWidget(m_closeBtn);
    rootLay->addLayout(topRow);

    m_listView = new QListView(this);
    m_delegate = new AsyncTaskItemDelegate(m_listView);
    m_listView->setItemDelegate(m_delegate);
    m_listView->setModel(&AsyncTaskManager::instance());
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setAlternatingRowColors(true);
    m_listView->setUniformItemSizes(false);
    rootLay->addWidget(m_listView, 3);

    m_detailsEdit = new QPlainTextEdit(this);
    m_detailsEdit->setReadOnly(true);
    m_detailsEdit->setPlaceholderText(tr("点击上面任意任务，查看详细描述与异常堆栈"));
    m_detailsEdit->setMaximumHeight(180);
    rootLay->addWidget(m_detailsEdit, 2);

    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::hide);
    connect(m_clearBtn, &QPushButton::clicked, this, &AsyncTaskPanelDialog::onClearHistory);
    connect(m_listView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &AsyncTaskPanelDialog::onCurrentRowChanged);

    auto &mgr = AsyncTaskManager::instance();
    connect(&mgr, &AsyncTaskManager::summaryChanged,   this, &AsyncTaskPanelDialog::refreshHeaderSummary);
    connect(&mgr, &AsyncTaskManager::threadCountChanged,this, &AsyncTaskPanelDialog::refreshThreadOccupancy);
    // 初始同步一次
    refreshHeaderSummary(mgr.queuedCount(), mgr.runningCount(), 0);
    refreshThreadOccupancy(mgr.activeThreadCount(), 0);
}

AsyncTaskPanelDialog::~AsyncTaskPanelDialog() = default;

void AsyncTaskPanelDialog::refreshHeaderSummary(int queued, int running, int finishedRecent)
{
    m_summaryQueued->setText(tr("排队中: %1").arg(queued));
    m_summaryRunning->setText(tr("运行中: %1").arg(running));
    m_summaryDone->setText(tr("已完成近: %1").arg(finishedRecent));
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
    if (!current.isValid()) { m_detailsEdit->clear(); return; }
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
    out += t.description.isEmpty() ? QLatin1String("(无)") : t.description;
    if (!t.error.isNull()) {
        out += QLatin1String("\n\n======== 异常 ========\n");
        out += t.error.toString();
    }
    m_detailsEdit->setPlainText(out);
}

void AsyncTaskPanelDialog::onClearHistory()
{
    AsyncTaskManager::instance().clearHistory();
}

// — 补充：AsyncTaskDelegate::paint 内缺的"取消按钮"绘制，放在全局 paint 末尾追加最清晰。
//   我们使用 QPainter::drawControl 直接画 CE_PushButton + 居中文字。
namespace {
struct _DeferredCancelButtonPaint {
    // 用一个小技巧：paint 执行前我们无法在 delegate 内部拿到局部 static？改为直接在 paint 里内联画即可。已在上文 paint 中做了预留。
};
}
// 为了不改动上面大段 paint 再追加一次 — 下面用一个小覆盖：直接把 cancel 按钮画在 paint 末尾。
// 我们通过 #include "moc_AsyncTaskPanelDialog.cpp" 风格不可行，改为在 StatusBarTaskWidget 编译单元里通过重写 delegate paint 追加按钮不太好；
// 最干净的做法：把"画取消按钮"放到上面 paint() 的末尾 — 我们重新用一个小宏替换注入：
#if 0
// 占位（上面 paint 实际没画按钮）。下面实际生效的补丁：我们在 editorEvent 处理取消，但没有视觉 — 会导致用户看不到取消按钮。
// 所以我们用下面这个全局 free 函数在 paint 最后再画一次：
#endif
