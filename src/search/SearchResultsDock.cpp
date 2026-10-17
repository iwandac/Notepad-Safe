#include "SearchResultsDock.h"
#include "SearchResultsController.h"
#include "SearchResultsFeedback.h"
#include "SearchResultsDelegate.h"
#include "SearchResultsViewPolicy.h"
#include "SearchResultsAggregateModel.h"
#include "LineIndex.h"

#include <QTreeView>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QStackedWidget>
#include <QPointer>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QUuid>
#include <limits>
#include <utility>

// ============================================================================
//  SearchResultsDock 实现
// ============================================================================
SearchResultsDock::SearchResultsDock(SearchResultsController &controller,
                                     QWidget *parent)
    : QDockWidget(QStringLiteral("搜索结果"), parent)
    , m_controller(&controller)
{
    // ⚠️ 关键修复：禁用浮动和移动，防止双击 / 拖动后嵌入错位
    //  - Fixed: 允许用户关闭/显示，不允许脱离位置
    //  - setAllowedAreas: 仅允许底部。标题栏在中央区域顶部，停靠在顶部会占据窗口上沿。
    //  - MainWindow::addDockWidget(Qt::BottomDockWidgetArea) 已在 MainWindow 里设置，这里再次约束
    setAllowedAreas(Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    m_feedback = new SearchResultsFeedback(container);
    layout->addWidget(m_feedback);

    // All providers share one virtual QTreeView.
    m_stack = new QStackedWidget(container);

    m_treeView = new QTreeView(m_stack);
    m_treeView->setObjectName(QStringLiteral("searchResultsTree"));
    SearchResultsViewPolicy::configure(m_treeView);
    m_stack->addWidget(m_treeView);

    layout->addWidget(m_stack);
    setWidget(container);

    m_stack->setCurrentIndex(0);
    connect(m_treeView, &QTreeView::doubleClicked,
            this, &SearchResultsDock::onModelDoubleClicked);
    connect(m_feedback, &SearchResultsFeedback::rerunRequested, this, [this]() {
        // Keep a value snapshot: an earlier receiver may clear the controller.
        const SearchResultTarget target = m_controller->rerunTarget();
        emit rerunRequested(target.documentId, target.expectedRevision);
    });

    // 关键：安装事件过滤器到 m_treeView viewport，以捕获 context 列内的点击位置
    //   —— 用于按"点击的匹配词"精准跳转（而非仅跳到第一个）
    m_treeView->viewport()->installEventFilter(this);
}
SearchResultsDock::~SearchResultsDock() = default;

void SearchResultsDock::updateTechnicalDetails()
{
    m_feedback->setTechnicalDetails(searchResultDiagnostics(
        m_model, m_controller->isMultiFile() ? m_multiModel : nullptr));
}

void SearchResultsDock::setRerunAvailable(bool available,
                                          const SearchResultTarget &preferredTarget)
{
    m_controller->setRerunTarget(available ? preferredTarget
                                           : SearchResultTarget{});
    m_feedback->setRerunAvailable(available);
}

void SearchResultsDock::updateSummary(const QString &pattern, const QString &fileName,
                                      qint64 totalMatches)
{
    m_feedback->setSummaryText(QStringLiteral("%1 · “%2” · %3 处")
                           .arg(fileName, pattern).arg(totalMatches));
}

void SearchResultsDock::setResults(const QVector<SearchMatch> &matches,
                                   const QString &pattern,
                                   const QString &fileName,
                                   const SearchResultTarget &target,
                                   bool incomplete)
{
    auto *model = new SearchResultsModel(this);
    model->setTextEncoding(TextEncoding::Utf8);
    model->setResults(canonicalResults(matches), {}, 0);
    if (incomplete)
        model->setIncompleteReason(QStringLiteral("结果未完整生成"));
    setResultsModel(model, pattern, fileName, target);
}

void SearchResultsDock::clearResults()
{
    setRerunAvailable(false);
    SearchResultsModel *oldSingleModel = m_model;
    if (m_controller->singleTarget().isValid())
        emit clearSearchHighlightsRequested(
            m_controller->singleTarget().documentId);
    for (const SearchResultTarget &target : m_controller->targets())
        emit clearSearchHighlightsRequested(target.documentId);
    m_lastHitRow = -1;
    m_lastHitMatchIdx = -1;
    m_controller->clear();
    m_singlePreviewMatches.clear();
    m_model = nullptr;
    if (m_treeView)
        SearchResultsViewPolicy::bindModel(m_treeView, nullptr, false);
    if (oldSingleModel && oldSingleModel->QObject::parent() == this)
        delete oldSingleModel;
    delete m_multiModel;
    m_multiModel = nullptr;
    m_feedback->setSummaryText({});
    m_feedback->resetTechnicalDetails();
    hide();
}

void SearchResultsDock::setResultsModel(SearchResultsModel *model,
                                        const QString &pattern,
                                        const QString &fileName,
                                        const SearchResultTarget &target)
{
    setRerunAvailable(false);
    m_feedback->resetTechnicalDetails();
    m_controller->beginSingle(target);
    m_singlePreviewMatches.clear();
    SearchResultsModel *oldModel = m_model;
    m_model = model;
    // 切到 L2 视图
    m_stack->setCurrentIndex(0);
    SearchResultsViewPolicy::bindModel(m_treeView, model, false);
    if (oldModel && oldModel != model && oldModel->QObject::parent() == this)
        delete oldModel;
    if (model) {
        qint64 total = model->totalMatches();
        qint64 lines = model->groupedCount();
        const QString groupUnit = model->bytePositionMode()
            ? QStringLiteral("个预览片段") : QStringLiteral("行");
        m_feedback->setSummaryText(QStringLiteral("%1 · “%2” · %3 处 / %4 %5")
                               .arg(fileName, pattern).arg(total)
                               .arg(lines).arg(groupUnit));
        if (model->isIncomplete()) {
            m_feedback->setSummaryText(
                QStringLiteral("%1 · “%2” · 至少 %3 处（%4）")
                    .arg(fileName, pattern).arg(total)
                    .arg(model->incompleteUserReason()));
        }
    } else {
        updateSummary(pattern, fileName, 0);
    }
    updateTechnicalDetails();
}

void SearchResultsDock::beginSingleFileResults(const QString &pattern,
                                                const QString &fileName,
                                                const SearchResultTarget &target,
                                                TextEncoding encoding,
                                                bool bytePositionMode)
{
    auto *preview = new SearchResultsModel(this);
    preview->setBytePositionMode(bytePositionMode);
    preview->setTextEncoding(encoding);
    setResultsModel(preview, pattern, fileName, target);
    m_feedback->setSummaryText(QStringLiteral("%1 · 正在搜索 “%2”…")
                           .arg(fileName, pattern));
    show();
    raise();
}

void SearchResultsDock::appendSingleFilePreview(
    const SearchResultTarget &target,
    const QVector<LargeFileSearchMatch> &batch,
    TextEncoding encoding, bool bytePositionMode)
{
    const SearchResultTarget &expected = m_controller->singleTarget();
    if (m_controller->isMultiFile() || !target.isValid()
        || target.documentId != expected.documentId
        || target.expectedRevision != expected.expectedRevision || !m_model
        || batch.isEmpty()
        || m_model->textEncoding() != encoding
        || m_model->bytePositionMode() != bytePositionMode) {
        return;
    }
    const int remaining = 5000 - m_singlePreviewMatches.size();
    if (remaining <= 0) return;
    const int take = qMin(remaining, batch.size());
    for (int i = 0; i < take; ++i) m_singlePreviewMatches.append(batch.at(i));
    m_model->setResults(m_singlePreviewMatches, {}, 0);
    m_feedback->setSummaryText(QStringLiteral("%1 · 已发现至少 %2 处…")
                           .arg(target.displayName).arg(m_singlePreviewMatches.size()));
}

void SearchResultsDock::decorateResultsFor(const QUuid &documentId,
                                            const LineIndex &lineIndex)
{
    if (documentId.isNull() || lineIndex.isIncomplete()) {
        return;
    }
    if (!m_controller->isMultiFile()) {
        if (m_controller->singleTarget().documentId == documentId && m_model)
            m_model->decorateLineNumbers(lineIndex);
        return;
    }
    if (!m_multiModel)
        return;
    const QString targetId = m_controller->targetId(documentId);
    if (!targetId.isEmpty())
        m_multiModel->decorate(targetId, lineIndex);
}

void SearchResultsDock::invalidateResultsFor(const QUuid &documentId)
{
    if (!m_controller->contains(documentId))
        return;
    emit clearSearchHighlightsRequested(documentId);

    if (m_controller->isMultiFile()) {
        const QString targetId = m_controller->targetId(documentId);

        if (m_multiModel && m_multiModel->invalidate(targetId)) {
            m_controller->markStale();
        }
        m_controller->invalidateRevision(targetId);
        m_feedback->setSummaryText(
            QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件不完整，%4 个已失效；请重新查找。")
                .arg(m_controller->pattern()).arg(m_controller->matchCount())
                .arg(m_controller->incompleteCount()).arg(m_controller->staleCount()));
        setRerunAvailable(true);
        show();
        return;
    }

    SearchResultsModel *oldModel = m_model;
    const qint64 previousMatches = oldModel ? oldModel->totalMatches() : 0;
    m_model = nullptr;
    SearchResultsViewPolicy::bindModel(m_treeView, nullptr, false);
    if (oldModel && oldModel->QObject::parent() == this)
        delete oldModel;
    const SearchResultTarget invalidated = m_controller->singleTarget();
    m_controller->clear();
    m_feedback->setSummaryText(QStringLiteral("文档内容已修改，原有 %1 处搜索结果已失效；请重新搜索。")
                       .arg(previousMatches));
    setRerunAvailable(true, invalidated);
    show();
}

void SearchResultsDock::closeResultsFor(const QUuid &documentId)
{
    if (!m_controller->contains(documentId))
        return;
    if (m_controller->isMultiFile()) {
        invalidateResultsFor(documentId);
        return;
    }
    SearchResultsModel *oldModel = m_model;
    const qint64 previousMatches = oldModel ? oldModel->totalMatches() : 0;
    m_model = nullptr;
    SearchResultsViewPolicy::bindModel(m_treeView, nullptr, false);
    if (oldModel && oldModel->QObject::parent() == this)
        oldModel->deleteLater();
    m_controller->clear();
    m_feedback->setSummaryText(QStringLiteral("源文档已关闭，原有 %1 处搜索结果已失效；请重新打开文件后查找。")
                       .arg(previousMatches));
    setRerunAvailable(false);
}

void SearchResultsDock::beginMultiFileResults(const QString &pattern, int targetCount)
{
    setRerunAvailable(false);
    m_feedback->resetTechnicalDetails();
    m_controller->beginMulti(pattern);
    m_stack->setCurrentIndex(0);
    SearchResultsViewPolicy::bindModel(m_treeView, nullptr, false);
    delete m_multiModel;
    m_multiModel = new MultiSearchResultsModel(this);
    SearchResultsViewPolicy::bindModel(m_treeView, m_multiModel, true);
    m_feedback->setSummaryText(QStringLiteral("正在全部 %1 个打开文件中搜索 “%2”…")
                           .arg(targetCount).arg(pattern));
    show();
    raise();
}

void SearchResultsDock::beginMultiFileTarget(const SearchResultTarget &target,
                                              TextEncoding encoding,
                                              bool bytePositionMode)
{
    if (!m_controller->isMultiFile() || !m_multiModel || !target.isValid()) return;
    m_controller->ensureMultiTarget(target);
    auto *preview = new SearchResultsModel(m_multiModel);
    preview->setBytePositionMode(bytePositionMode);
    preview->setTextEncoding(encoding);
    m_multiModel->addPreview(target, preview);
}

void SearchResultsDock::appendMultiFilePreview(
    const SearchResultTarget &target,
    const QVector<LargeFileSearchMatch> &batch,
    TextEncoding encoding, bool bytePositionMode)
{
    if (!m_controller->isMultiFile() || !m_multiModel
        || !target.isValid() || batch.isEmpty()) return;
    const QString targetId = m_controller->ensureMultiTarget(target);
    if (m_multiModel->appendPreview(targetId, batch, encoding,
                                    bytePositionMode)) {
        m_feedback->setSummaryText(QStringLiteral("正在全部打开文件中搜索 “%1”…")
                               .arg(m_controller->pattern()));
    }
}

void SearchResultsDock::appendMultiFileResults(const SearchResultTarget &target,
                                               const QVector<SearchMatch> &matches,
                                               bool incomplete)
{
    if (!m_controller->isMultiFile() || !target.isValid())
        return;
    m_controller->ensureMultiTarget(target);
    if (m_multiModel) {
        m_multiModel->addL1(target, matches, incomplete);
        m_treeView->expand(m_multiModel->index(m_multiModel->rowCount() - 1, 0));
    }
    if (incomplete)
        m_controller->markIncomplete();
    m_controller->addMatches(matches.size());
    updateTechnicalDetails();
    m_feedback->setSummaryText(m_controller->incompleteCount() > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整；可查看技术详情后重试。")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()).arg(m_controller->incompleteCount())
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()));
}

void SearchResultsDock::appendMultiFileResults(const SearchResultTarget &target,
                                               SearchResultsModel *model)
{
    if (!m_controller->isMultiFile() || !target.isValid() || !model)
        return;
    const QString targetId = m_controller->ensureMultiTarget(target);
    if (m_multiModel) {
        if (!m_multiModel->finish(targetId, model))
            m_multiModel->addModel(target, model);
        for (int row = 0; row < m_multiModel->rowCount(); ++row) {
            const QModelIndex group = m_multiModel->index(row, 0);
            if (group.data(TargetRole).toString() == targetId) {
                m_treeView->expand(group);
                break;
            }
        }
    }
    if (model->isIncomplete())
        m_controller->markIncomplete();
    m_controller->addMatches(model->totalMatches());
    updateTechnicalDetails();
    m_feedback->setSummaryText(m_controller->incompleteCount() > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整；可查看技术详情后重试。")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()).arg(m_controller->incompleteCount())
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()));
}

void SearchResultsDock::finishMultiFileResults(bool cancelled)
{
    if (!m_controller->isMultiFile())
        return;
    if (cancelled) {
        m_feedback->setSummaryText(
            QStringLiteral("已取消在全部打开文件中搜索 “%1”；已保留 %2 处完成结果，可重新查找。")
                .arg(m_controller->pattern()).arg(m_controller->matchCount()));
        return;
    }
    if (m_controller->staleCount() > 0 || m_controller->incompleteCount() > 0) {
        m_feedback->setSummaryText(
            QStringLiteral("在全部打开文件中搜索 “%1”：找到至少 %2 处；%3 个文件不完整，%4 个已失效；可查看技术详情后重新查找。")
                .arg(m_controller->pattern()).arg(m_controller->matchCount())
                .arg(m_controller->incompleteCount()).arg(m_controller->staleCount()));
    } else {
        m_feedback->setSummaryText(QStringLiteral("在全部打开文件中搜索 “%1”：找到 %2 处匹配")
                               .arg(m_controller->pattern()).arg(m_controller->matchCount()));
    }
}

bool SearchResultsDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_treeView->viewport()
        && (m_model || m_multiModel)) {
        if (event->type() == QEvent::MouseMove
            || event->type() == QEvent::HoverMove) {
            const QPoint position = event->type() == QEvent::MouseMove
                ? static_cast<QMouseEvent *>(event)->position().toPoint()
                : static_cast<QHoverEvent *>(event)->position().toPoint();
            const QModelIndex hovered = m_treeView->indexAt(position);
            m_treeView->setProperty("searchHoverRow",
                                    hovered.isValid() ? hovered.row() : -1);
            m_treeView->setProperty("searchHoverHasParent",
                                    hovered.parent().isValid());
            m_treeView->setProperty("searchHoverParentRow",
                                    hovered.parent().isValid()
                                        ? hovered.parent().row() : -1);
            m_treeView->viewport()->update();
        } else if (event->type() == QEvent::Leave) {
            m_treeView->setProperty("searchHoverRow", -1);
            m_treeView->setProperty("searchHoverHasParent", false);
            m_treeView->setProperty("searchHoverParentRow", -1);
            m_treeView->viewport()->update();
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                // 计算点击落在了哪一行 + 哪一列
                QModelIndex idx = m_treeView->indexAt(me->pos());
                if (idx.isValid() && idx.column() == SearchResultsModel::COL_CONTEXT
                    && !idx.data(SearchResultsModel::ContextRole).isNull()) {
                    QRect colRect = m_treeView->visualRect(idx);
                    int xInCol = me->pos().x() - colRect.left();
                    QFontMetrics fm(m_treeView->font());
                    int matchIdx = hitTestSearchResultMatch(
                        idx, xInCol, colRect.width(), fm);
                    m_lastHitMatchIdx = matchIdx;
                    m_lastHitRow = idx.row();
                } else {
                    m_lastHitMatchIdx = -1;
                    m_lastHitRow = -1;
                }
            }
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void SearchResultsDock::onModelDoubleClicked(const QModelIndex &index)
{
    if ((!m_model && !m_multiModel) || !index.isValid()) return;
    if (m_controller->isMultiFile() && m_multiModel) {
        MultiSearchResultsModel::Group *group = m_multiModel->groupFor(index);
        if (!group || !index.parent().isValid() || group->stale)
            return;
        const QVariantList ranges =
            index.data(SearchResultsModel::MatchByteRangesRole).toList();
        if (ranges.isEmpty()) return;
        int chosen = (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0)
            ? m_lastHitMatchIdx : 0;
        chosen = qBound(0, chosen, ranges.size() - 1);
        const QVariantMap range = ranges.at(chosen).toMap();
        emit jumpToDocumentByteOffset(
            group->target.documentId, group->target.expectedRevision,
            range.value(QStringLiteral("byteOffset")).toULongLong(),
            range.value(QStringLiteral("length")).toInt());
        return;
    }
    if (!m_model) return;
    const SearchResultTarget target = m_controller->singleTarget();
    qint64 byteOff = -1;
    int    length  = 0;
    // 如果当前按下位置是同一个行并且有命中的匹配索引，则按该索引跳转
    if (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0) {
        if (m_model->matchByteOffsetAtIndex(index.row(), m_lastHitMatchIdx, &byteOff, &length)) {
            // 命中：跳转到用户点击的具体匹配词
            if (target.isValid())
                emit jumpToDocumentByteOffset(
                    target.documentId, target.expectedRevision, byteOff, length);
            else
                emit jumpToByteOffset(byteOff, length);
            return;
        }
    }
    // 回退：跳转到第一个匹配（保持兼容）
    byteOff = m_model->matchByteOffset(index);
    length  = m_model->matchLength(index);
    if (byteOff >= 0) {
        if (target.isValid())
            emit jumpToDocumentByteOffset(
                target.documentId, target.expectedRevision, byteOff, length);
        else
            emit jumpToByteOffset(byteOff, length);
    }
}
