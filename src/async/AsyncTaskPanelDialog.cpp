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
#include <QScrollBar>
#include <QTextCursor>

namespace {
QString taskDuration(qint64 milliseconds)
{
    if (milliseconds < 0) return QString();
    const qint64 seconds = qMax<qint64>(1, (milliseconds + 999) / 1000);
    if (seconds < 60) return QObject::tr("%1 秒").arg(seconds);
    const qint64 minutes = (seconds + 59) / 60;
    if (minutes < 60) return QObject::tr("%1 分钟").arg(minutes);
    return QObject::tr("%1 小时 %2 分钟").arg(minutes / 60).arg(minutes % 60);
}
}

namespace {
// Public row text comes only from the shared task projection; diagnostics stay
// in the opt-in technical section below.
class AsyncTaskItemDelegate : public QStyledItemDelegate
{
    // Use identical text widths/fonts/flags for measuring and painting. Native
    // progress controls are visual indicators, never the source of row text.
    static constexpr int TextFlags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere;
    struct RowLayout {
        QFont titleFont, textFont;
        QString title, body, error;
        QRect titleRect, bodyRect, progressRect, cancelRect, errorRect;
        int height = 0;
    };

    static RowLayout rowLayout(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        RowLayout row;
        row.titleFont = option.font;
        row.titleFont.setBold(true);
        row.textFont = option.font;
        row.textFont.setBold(false);
        row.textFont.setPointSize(qMax(8, row.textFont.pointSize() - 1));
        row.title = index.data(AsyncTaskManager::TaskTitleRole).toString();
        row.body = index.data(AsyncTaskManager::TaskUserStatusRole).toString()
            + QStringLiteral(" · ") + index.data(AsyncTaskManager::TaskUserProgressRole).toString();
        row.error = index.data(AsyncTaskManager::TaskUserErrorRole).toString();
        const int left = option.rect.left() + 8;
        const int width = qMax(1, option.rect.width() - 16);
        int top = option.rect.top() + 6;
        const auto textRect = [left, width](const QFont &font, const QString &text, int y) {
            const QFontMetrics metrics(font);
            const int height = qMax(metrics.height(), metrics.boundingRect(
                QRect(0, 0, width, 100000), TextFlags, text).height());
            return QRect(left, y, width, height);
        };
        row.titleRect = textRect(row.titleFont, row.title, top);
        top = row.titleRect.bottom() + 5;
        row.bodyRect = textRect(row.textFont, row.body, top);
        top = row.bodyRect.bottom() + 7;
        const QFontMetrics metrics(row.textFont);
        const int controlHeight = qMax(24, metrics.height() + 8);
        const int barHeight = qMax(12, metrics.height());
        const auto state = static_cast<AsyncTaskState>(index.data(AsyncTaskManager::TaskStateRole).toInt());
        const bool cancellable = (state == AsyncTaskState::Queued || state == AsyncTaskState::Running)
            && index.data(AsyncTaskManager::TaskCancellableRole).toBool();
        if (cancellable)
            row.cancelRect = QRect(left + qMax(0, width - 70), top, qMin(70, width), controlHeight);
        row.progressRect = QRect(left, top + (controlHeight - barHeight) / 2,
            qMax(1, width - (cancellable ? 78 : 0)), barHeight);
        top += controlHeight;
        if (!row.error.isEmpty()) {
            row.errorRect = textRect(row.textFont, row.error, top + 6);
            top = row.errorRect.bottom() + 1;
        }
        row.height = top - option.rect.top() + 6;
        return row;
    }

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        QStyle *style = qApp->style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, nullptr);
        const bool selected = option.state & QStyle::State_Selected;
        const QColor foreground = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        // Some native styles do not paint selection for this primitive. Always
        // pair the actual row background with the same palette's foreground.
        if (selected) painter->fillRect(option.rect, option.palette.brush(QPalette::Highlight));

        const RowLayout row = rowLayout(option, index);
        const auto state = static_cast<AsyncTaskState>(index.data(AsyncTaskManager::TaskStateRole).toInt());
        const bool active = state == AsyncTaskState::Queued || state == AsyncTaskState::Running;
        const int precise = index.data(AsyncTaskManager::TaskPercentHundredthsRole).toInt();

        painter->setFont(row.titleFont);
        painter->setPen(foreground);
        painter->drawText(row.titleRect, TextFlags, row.title);
        painter->setFont(row.textFont);
        painter->drawText(row.bodyRect, TextFlags, row.body);

        QStyleOptionProgressBar progress;
        progress.rect = row.progressRect;
        progress.state = option.state | QStyle::State_Horizontal;
        progress.minimum = 0;
        if (precise >= 0) {
            progress.maximum = 10000;
            progress.progress = qBound(0, precise, 10000);
        } else if (active) {
            progress.maximum = 0;
            progress.progress = 0;
        } else {
            // A terminal unknown-total task is never a busy animation.
            progress.maximum = 100;
            progress.progress = state == AsyncTaskState::Finished ? 100 : 0;
        }
        progress.textVisible = false;
        progress.palette = option.palette;
        style->drawControl(QStyle::CE_ProgressBar, &progress, painter);
        if (!row.cancelRect.isEmpty()) {
            QStyleOptionButton button;
            button.rect = row.cancelRect;
            button.text = tr("取消");
            button.state = QStyle::State_Enabled | QStyle::State_Raised;
            button.palette = option.palette;
            style->drawControl(QStyle::CE_PushButton, &button, painter);
        }
        if (!row.error.isEmpty()) {
            painter->setPen(foreground);
            painter->drawText(row.errorRect, TextFlags, row.error);
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem measured = option;
        const auto *view = qobject_cast<QListView *>(parent());
        measured.rect = QRect(0, 0, view ? view->viewport()->width() : qMax(1, option.rect.width()), 0);
        return QSize(0, rowLayout(measured, index).height);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        Q_UNUSED(model);
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent*>(event);
            if (rowLayout(option, index).cancelRect.contains(me->pos())) {
                const int st = index.data(AsyncTaskManager::TaskStateRole).toInt();
                if (index.data(AsyncTaskManager::TaskCancellableRole).toBool()
                    && (st == static_cast<int>(AsyncTaskState::Queued)
                        || st == static_cast<int>(AsyncTaskState::Running))) {
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
        out->setTextFormat(Qt::PlainText);
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
    m_listView->setResizeMode(QListView::Adjust);
    rootLay->addWidget(m_listView, 3);

    m_selectedSummary = new QLabel(tr("选择任务以查看状态。"), this);
    m_selectedSummary->setObjectName(QStringLiteral("backgroundTaskSummary"));
    m_selectedSummary->setAccessibleName(tr("所选任务"));
    m_selectedSummary->setTextFormat(Qt::PlainText);
    m_selectedSummary->setWordWrap(true);
    m_selectedSummary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    rootLay->addWidget(m_selectedSummary);

    m_detailsToggle = new QPushButton(tr("技术详情"), this);
    m_detailsToggle->setObjectName(QStringLiteral("backgroundTaskDetailsToggle"));
    m_detailsToggle->setAccessibleName(tr("技术详情"));
    m_detailsToggle->setCheckable(true);
    m_detailsToggle->setAutoDefault(false);
    m_detailsToggle->setAccessibleDescription(tr("已折叠，按空格展开技术详情。"));
    rootLay->addWidget(m_detailsToggle, 0, Qt::AlignLeft);
    m_technicalDetails = new QWidget(this);
    m_technicalDetails->setObjectName(QStringLiteral("backgroundTaskTechnicalDetails"));
    m_technicalDetails->setAccessibleName(tr("技术详情"));
    auto *technicalLayout = new QVBoxLayout(m_technicalDetails);
    technicalLayout->setContentsMargins(0, 0, 0, 0);
    technicalLayout->addWidget(m_summaryThreads, 0, Qt::AlignLeft);
    m_detailsEdit = new QPlainTextEdit(m_technicalDetails);
    m_detailsEdit->setObjectName(QStringLiteral("backgroundTaskDetails"));
    m_detailsEdit->setAccessibleName(tr("任务技术详情"));
    m_detailsEdit->setReadOnly(true);
    m_detailsEdit->setPlaceholderText(tr("点击上面任意任务，查看详细描述与异常堆栈"));
    m_detailsEdit->setMaximumHeight(180);
    technicalLayout->addWidget(m_detailsEdit);
    rootLay->addWidget(m_technicalDetails, 2);
    m_technicalDetails->hide();
    connect(m_detailsToggle, &QPushButton::toggled, this, [this](bool expanded) {
        if (!expanded && m_technicalDetails->isAncestorOf(QApplication::focusWidget()))
            m_detailsToggle->setFocus();
        m_technicalDetails->setVisible(expanded);
        m_detailsToggle->setAccessibleDescription(expanded
            ? tr("已展开，按空格折叠技术详情。") : tr("已折叠，按空格展开技术详情。"));
    });

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
    connect(&mgr, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        const QModelIndex current = m_listView->currentIndex();
        if (current.isValid()) m_selectedTaskId = current.data(AsyncTaskManager::TaskIdRole).toLongLong();
        m_modelResetting = true;
    });
    connect(&mgr, &QAbstractItemModel::modelReset, this, [this]() {
        QModelIndex restored;
        auto *model = m_listView->model();
        for (int row = 0; m_selectedTaskId != 0 && row < model->rowCount(); ++row) {
            const QModelIndex candidate = model->index(row, 0);
            if (candidate.data(AsyncTaskManager::TaskIdRole).toLongLong() == m_selectedTaskId) {
                restored = candidate;
                break;
            }
        }
        m_listView->setCurrentIndex(restored);
        if (!restored.isValid()) m_selectedTaskId = 0;
        m_modelResetting = false;
        refreshSelectedTask();
    });
    connect(&mgr, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &) {
            const QModelIndex current = m_listView->currentIndex();
            if (current.isValid() && current.row() >= top.row() && current.row() <= bottom.row())
                refreshSelectedTask();
            else
                updateActionStates();
        });
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
    if (m_modelResetting) return;
    const qint64 taskId = current.isValid() ? current.data(AsyncTaskManager::TaskIdRole).toLongLong() : 0;
    if (taskId != m_selectedTaskId) m_detailsEdit->clear();
    m_selectedTaskId = taskId;
    refreshSelectedTask();
}

void AsyncTaskPanelDialog::refreshSelectedTask()
{
    if (m_modelResetting) return;
    const AsyncTask t = AsyncTaskManager::instance().taskById(m_selectedTaskId);
    if (m_selectedTaskId == 0 || t.id != m_selectedTaskId) {
        m_selectedSummary->setText(tr("选择任务以查看状态。"));
        m_selectedSummary->setAccessibleDescription(m_selectedSummary->text());
        m_detailsEdit->clear();
        updateActionStates();
        return;
    }
    const QString summary = t.userText().summary;
    m_selectedSummary->setText(summary);
    m_selectedSummary->setAccessibleDescription(summary);

    QString out;
    out += tr("任务ID: %1    批次ID: %2\n").arg(t.id).arg(t.batchId);
    out += tr("类型: %1    优先级: %2    状态: %3\n")
        .arg(t.typeText(), t.priorityText(), t.stateText());
    out += tr("分类标签: %1\n").arg(t.categoryTag.isEmpty() ? QLatin1String("-") : t.categoryTag);
    auto fmt = [](const QDateTime &dt){ return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("-"); };
    out += tr("创建: %1   启动: %2   结束: %3   耗时: %4ms\n")
        .arg(fmt(t.createdAt), fmt(t.startedAt), fmt(t.finishedAt)).arg(t.elapsedMs());
    if (!t.progressPhase.isEmpty()) {
        out += tr("当前阶段: %1/%2  %3\n")
            .arg(t.progressPhaseIndex).arg(t.progressPhaseCount)
            .arg(t.progressPhase);
    }
    if (!t.completedPhases.isEmpty())
        out += tr("已完成阶段: %1\n").arg(t.completedPhases.join(QStringLiteral(" -> ")));
    out += tr("进度: %1 / %2  (%3%)\n")
        .arg(t.progressDone).arg(t.progressTotal)
        .arg(t.percentHundredths() < 0 ? QStringLiteral("-")
             : QString::number(t.percentHundredths() / 100.0, 'f', 2));
    const qint64 remainingMs = t.estimatedRemainingMs();
    if (remainingMs >= 0)
        out += tr("预计剩余: %1\n").arg(taskDuration(remainingMs));
    out += QStringLiteral("\n描述:\n");
    out += t.description.isEmpty() ? QStringLiteral("(无)") : t.description;
    if (!t.error.isNull()) {
        out += QStringLiteral("\n\n======== 异常 ========\n");
        out += t.error.toString();
    }
    const QString previousText = m_detailsEdit->toPlainText();
    if (previousText != out) {
        const QTextCursor previous = m_detailsEdit->textCursor();
        // Copy positions before setPlainText: even a copied QTextCursor still
        // tracks its document and moves when that document is replaced.
        const int anchor = previous.anchor();
        const int position = previous.position();
        const int start = qMin(anchor, position);
        const int end = qMax(anchor, position);
        int restoredAnchor = qBound(0, anchor, int(out.size()));
        int restoredPosition = qBound(0, position, int(out.size()));
        if (start != end) {
            const QString selectedText = previousText.mid(start, end - start);
            int relocatedStart = out.indexOf(selectedText);
            if (relocatedStart >= 0 && out.indexOf(selectedText, relocatedStart + 1) >= 0) {
                // Repeated text is safe only inside an unchanged prefix/suffix;
                // do not silently select some other occurrence of a diagnostic.
                int prefix = 0;
                const int commonLength = qMin(previousText.size(), out.size());
                while (prefix < commonLength && previousText.at(prefix) == out.at(prefix)) ++prefix;
                int suffix = 0;
                while (suffix < commonLength - prefix
                    && previousText.at(previousText.size() - suffix - 1) == out.at(out.size() - suffix - 1)) ++suffix;
                relocatedStart = end <= prefix ? start
                    : (start >= previousText.size() - suffix ? start + out.size() - previousText.size() : -1);
            }
            if (relocatedStart >= 0) {
                restoredAnchor = anchor < position ? relocatedStart : relocatedStart + end - start;
                restoredPosition = anchor < position ? relocatedStart + end - start : relocatedStart;
            } else {
                // The selected text changed or became ambiguous. Keep a caret,
                // rather than making Copy return unrelated replacement text.
                restoredAnchor = restoredPosition;
            }
        }
        const int vertical = m_detailsEdit->verticalScrollBar()->value();
        const int horizontal = m_detailsEdit->horizontalScrollBar()->value();
        m_detailsEdit->setPlainText(out);
        // The public summary must stay live, but inspecting/copying technical
        // details must not jump to the top on each existing one-second tick.
        QTextCursor restored = m_detailsEdit->textCursor();
        restored.setPosition(restoredAnchor);
        restored.setPosition(restoredPosition, QTextCursor::KeepAnchor);
        m_detailsEdit->setTextCursor(restored);
        m_detailsEdit->verticalScrollBar()->setValue(vertical);
        m_detailsEdit->horizontalScrollBar()->setValue(horizontal);
    }
    updateActionStates();
}

void AsyncTaskPanelDialog::onCancelSelected()
{
    const QModelIndex current = m_listView->currentIndex();
    if (!current.isValid() || !current.data(AsyncTaskManager::TaskCancellableRole).toBool())
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
}

void AsyncTaskPanelDialog::updateActionStates()
{
    auto &manager = AsyncTaskManager::instance();
    const int active = manager.queuedCount() + manager.runningCount();
    m_cancelAllBtn->setEnabled(manager.cancellableCount() > 0);
    m_clearBtn->setEnabled(manager.rowCount() > active);

    const QModelIndex current = m_listView->currentIndex();
    const int state = current.isValid()
                          ? current.data(AsyncTaskManager::TaskStateRole).toInt()
                          : -1;
    m_cancelSelectedBtn->setEnabled(
        current.data(AsyncTaskManager::TaskCancellableRole).toBool()
        && (state == static_cast<int>(AsyncTaskState::Queued)
            || state == static_cast<int>(AsyncTaskState::Running)));
}
