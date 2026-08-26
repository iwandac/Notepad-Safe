#include "SearchResultsDock.h"
#include "SearchResultsController.h"
#include "VirtualFileView.h"

#include <QTreeView>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTextLayout>
#include <QApplication>
#include <QPalette>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QUuid>
#include <QSizePolicy>
#include <limits>
#include <utility>

namespace {
constexpr int TargetRole = Qt::UserRole + 20;
constexpr int StaleRole = Qt::UserRole + 25;

QVector<LargeFileSearchMatch> canonicalResults(
    const QVector<SearchMatch> &matches)
{
    QVector<LargeFileSearchMatch> results;
    results.reserve(matches.size());
    for (const SearchMatch &source : matches) {
        LargeFileSearchMatch result;
        result.byteOffset = source.byteOffset;
        result.length = source.byteLength;
        result.line = source.line;
        result.col = source.index;
        result.lineContext = source.context.toUtf8();
        const qint64 prefixBytes = source.context.left(source.index).toUtf8().size();
        result.contextByteOffset = qMax<qint64>(0, source.byteOffset - prefixBytes);
        results.append(std::move(result));
    }
    return results;
}

struct VisualMatchRange {
    int start = 0;
    int length = 0;
    int sourceIndex = -1;
};

struct PreparedContext {
    QString text;
    QVector<VisualMatchRange> matches;
};

int previousUtf16Boundary(const QString &text, int position)
{
    int candidate = qMax(0, position - 1);
    if (candidate > 0 && text.at(candidate).isLowSurrogate()
        && text.at(candidate - 1).isHighSurrogate()) {
        --candidate;
    }
    return candidate;
}

PreparedContext prepareContext(const QModelIndex &index, int maxWidth,
                               const QFontMetrics &metrics)
{
    PreparedContext prepared;
    const QString text = index.data(SearchResultsModel::ContextRole).toString();
    const QVariantList ranges =
        index.data(SearchResultsModel::MatchPositionsRole).toList();
    if (text.isEmpty() || ranges.isEmpty() || maxWidth <= 0) {
        prepared.text = metrics.elidedText(text, Qt::ElideRight, qMax(0, maxWidth));
        return prepared;
    }

    const QVariantMap primary = ranges.first().toMap();
    const int primaryStart = qBound(
        0, primary.value(QStringLiteral("start")).toInt(), text.size());
    const int primaryLength = qBound(
        0, primary.value(QStringLiteral("length")).toInt(),
        text.size() - primaryStart);
    const int primaryEnd = primaryStart + primaryLength;
    const QString ellipsis(QChar(0x2026));
    const int matchWidth = metrics.horizontalAdvance(
        text.mid(primaryStart, primaryLength));
    const int ellipsisWidth = metrics.horizontalAdvance(ellipsis);
    const bool hasLeading = primaryStart > 0;
    const bool hasTrailing = primaryEnd < text.size();
    int decorationBudget = qMax(0, maxWidth - matchWidth);
    bool showLeadingEllipsis = false;
    bool showTrailingEllipsis = false;
    if (hasLeading && hasTrailing && decorationBudget >= 2 * ellipsisWidth) {
        showLeadingEllipsis = true;
        showTrailingEllipsis = true;
        decorationBudget -= 2 * ellipsisWidth;
    } else if (hasLeading && decorationBudget >= ellipsisWidth) {
        showLeadingEllipsis = true;
        decorationBudget -= ellipsisWidth;
    } else if (hasTrailing && decorationBudget >= ellipsisWidth) {
        showTrailingEllipsis = true;
        decorationBudget -= ellipsisWidth;
    }
    // Build the visible window around the match explicitly.  Passing the
    // complete tail to elidedText() lets the platform elider cut through the
    // match when font shaping and QFontMetrics disagree by a few pixels.  The
    // match is the result row's primary information, so context yields first.
    const int contextBudget = decorationBudget;
    const int prefixBudget = contextBudget * 2 / 5;
    const int suffixBudget = contextBudget - prefixBudget;
    int sourceStart = primaryStart;
    while (sourceStart > 0) {
        const int candidate = previousUtf16Boundary(text, sourceStart);
        if (metrics.horizontalAdvance(
                text.mid(candidate, primaryStart - candidate)) > prefixBudget) {
            break;
        }
        sourceStart = candidate;
    }

    int sourceEnd = primaryEnd;
    while (sourceEnd < text.size()) {
        int candidate = sourceEnd + 1;
        if (candidate < text.size() && text.at(sourceEnd).isHighSurrogate()
            && text.at(candidate).isLowSurrogate()) {
            ++candidate;
        }
        if (metrics.horizontalAdvance(
                text.mid(primaryEnd, candidate - primaryEnd)) > suffixBudget) {
            break;
        }
        sourceEnd = candidate;
    }

    const QString leading = sourceStart > 0 && showLeadingEllipsis
        ? ellipsis : QString();
    const QString trailing = sourceEnd < text.size() && showTrailingEllipsis
        ? ellipsis : QString();
    prepared.text = leading + text.mid(sourceStart, sourceEnd - sourceStart)
        + trailing;
    const int visibleContentEnd = prepared.text.size() - trailing.size();

    for (int i = 0; i < ranges.size(); ++i) {
        const QVariantMap range = ranges.at(i).toMap();
        const int start = qBound(0, range.value(QStringLiteral("start")).toInt(),
                                 text.size());
        const int length = qMax(0, range.value(QStringLiteral("length")).toInt());
        const int end = qBound(start, start + length, text.size());
        int visualStart = leading.size() + start - sourceStart;
        int visualEnd = leading.size() + end - sourceStart;
        if (visualEnd <= 0 || visualStart >= visibleContentEnd)
            continue;
        visualStart = qBound(0, visualStart, visibleContentEnd);
        visualEnd = qBound(visualStart, visualEnd, visibleContentEnd);
        if (visualEnd > visualStart)
            prepared.matches.append({visualStart, visualEnd - visualStart, i});
    }
    return prepared;
}

int hitTestModelMatch(const QModelIndex &index, int xInColumn,
                      int columnWidth, const QFontMetrics &metrics)
{
    constexpr int HorizontalPadding = 6;
    const int x = xInColumn - HorizontalPadding;
    const PreparedContext prepared = prepareContext(
        index, qMax(0, columnWidth - 2 * HorizontalPadding), metrics);
    if (x < 0)
        return -1;
    for (const VisualMatchRange &range : prepared.matches) {
        const int startX = metrics.horizontalAdvance(
            prepared.text.left(range.start));
        const int endX = metrics.horizontalAdvance(
            prepared.text.left(range.start + range.length));
        if (x >= startX && x <= endX)
            return range.sourceIndex;
    }
    return -1;
}
} // namespace

// One hierarchical model serves mixed Scintilla and mmap results.  Top-level
// rows are documents; child rows stay virtual, so a million-match mmap result
// does not become a million QTreeWidgetItem/QLabel allocations.
class MultiSearchResultsModel final : public QAbstractItemModel {
public:
    struct Group {
        QPointer<EditorTab> tab;
        QString targetId;
        QString displayName;
        QPointer<SearchResultsModel> results;
        QVector<LargeFileSearchMatch> preview;
        qint64 totalMatches = 0;
        QString incompleteReason;
        bool stale = false;
    };

    explicit MultiSearchResultsModel(QObject *parent = nullptr)
        : QAbstractItemModel(parent) {}
    ~MultiSearchResultsModel() override { qDeleteAll(groups_); }

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override
    {
        if (row < 0 || column < 0 || column >= SearchResultsModel::COL_COUNT)
            return {};
        if (!parent.isValid()) {
            if (row >= groups_.size()) return {};
            return createIndex(row, column, encode(row, false));
        }
        if (parent.column() != 0 || isChild(parent) || parent.row() >= groups_.size())
            return {};
        const Group *group = groups_.at(parent.row());
        if (row >= childCount(group)) return {};
        return createIndex(row, column, encode(parent.row(), true));
    }

    QModelIndex parent(const QModelIndex &child) const override
    {
        if (!child.isValid() || !isChild(child)) return {};
        const int groupRow = decodedGroup(child);
        if (groupRow < 0 || groupRow >= groups_.size()) return {};
        return createIndex(groupRow, 0, encode(groupRow, false));
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (!parent.isValid()) return groups_.size();
        if (parent.column() != 0 || isChild(parent)
            || parent.row() < 0 || parent.row() >= groups_.size())
            return 0;
        return childCount(groups_.at(parent.row()));
    }

    int columnCount(const QModelIndex & = QModelIndex()) const override
    {
        return SearchResultsModel::COL_COUNT;
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case SearchResultsModel::COL_LINE: return QStringLiteral("文件 / 位置");
        case SearchResultsModel::COL_CONTEXT: return QStringLiteral("内容");
        case SearchResultsModel::COL_MATCHCOUNT: return QStringLiteral("匹配数");
        default: return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid()) return Qt::NoItemFlags;
        const Group *group = groupFor(index);
        if (!group || (isChild(index) && group->stale))
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid()) return {};
        const Group *group = groupFor(index);
        if (!group) return {};
        if (!isChild(index)) {
            if (role == TargetRole) return group->targetId;
            if (role == StaleRole) return group->stale;
            if (role == Qt::ToolTipRole && !group->incompleteReason.isEmpty())
                return group->incompleteReason;
            if (role == Qt::FontRole) {
                QFont font;
                font.setBold(true);
                return font;
            }
            if (role != Qt::DisplayRole) return {};
            if (index.column() == 0) {
                return group->displayName;
            }
            if (index.column() == SearchResultsModel::COL_CONTEXT)
                return group->stale
                    ? QStringLiteral("结果已失效，请重新搜索此文件")
                    : (group->incompleteReason.isEmpty()
                        ? QString() : QStringLiteral("结果不完整"));
            if (index.column() == SearchResultsModel::COL_MATCHCOUNT)
                return group->totalMatches;
            return {};
        }

        if (group->stale) return {};
        if (role == TargetRole) return group->targetId;
        if (role == StaleRole) return false;
        if (!group->results) return {};
        const QModelIndex source = group->results->index(index.row(), index.column());
        return group->results->data(source, role);
    }

    void addL1(EditorTab *tab, const QString &targetId,
               const QVector<SearchMatch> &matches, bool incomplete = false)
    {
        auto *group = new Group;
        group->tab = tab;
        group->targetId = targetId;
        group->displayName = tab ? tab->fileName() : QString();
        auto *model = new SearchResultsModel(this);
        model->setTextEncoding(TextEncoding::Utf8);
        model->setResults(canonicalResults(matches), {}, 0);
        group->results = model;
        group->totalMatches = matches.size();
        if (incomplete)
            group->incompleteReason = QStringLiteral("结果未完整生成");
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void addModel(EditorTab *tab, const QString &targetId,
                  SearchResultsModel *model)
    {
        auto *group = new Group;
        group->tab = tab;
        group->targetId = targetId;
        group->displayName = tab ? tab->fileName() : QString();
        group->results = model;
        group->totalMatches = model ? model->totalMatches() : 0;
        group->incompleteReason = model ? model->incompleteReason() : QString();
        if (model) model->setParent(this);
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void addPreview(EditorTab *tab, const QString &targetId,
                    SearchResultsModel *model)
    {
        addModel(tab, targetId, model);
    }

    bool appendPreview(const QString &targetId,
                       const QVector<LargeFileSearchMatch> &batch,
                       TextEncoding encoding, bool bytePositionMode)
    {
        Group *group = findGroup(targetId);
        if (!group || !group->results || group->stale || batch.isEmpty()
            || group->results->textEncoding() != encoding
            || group->results->bytePositionMode() != bytePositionMode) {
            return false;
        }
        const int remaining = 5000 - group->preview.size();
        if (remaining <= 0) return true;
        const int take = qMin(remaining, batch.size());
        for (int i = 0; i < take; ++i) group->preview.append(batch.at(i));
        group->results->setResults(group->preview, {}, 0);
        group->totalMatches = group->preview.size();
        const int row = groups_.indexOf(group);
        emit dataChanged(index(row, 0), index(row, SearchResultsModel::COL_COUNT - 1));
        return true;
    }

    bool finish(const QString &targetId, SearchResultsModel *model)
    {
        Group *group = findGroup(targetId);
        if (!group || !model) return false;
        SearchResultsModel *oldModel = group->results;
        beginResetModel();
        group->results = model;
        group->preview.clear();
        group->totalMatches = model->totalMatches();
        group->incompleteReason = model->incompleteReason();
        model->setParent(this);
        endResetModel();
        if (oldModel && oldModel != model) delete oldModel;
        return true;
    }

    bool decorate(const QString &targetId, const LineIndex &lineIndex)
    {
        Group *group = findGroup(targetId);
        if (!group || !group->results || group->stale
            || !group->results->hasUnknownLineNumbers()) {
            return false;
        }
        beginResetModel();
        const bool changed = group->results->decorateLineNumbers(lineIndex);
        endResetModel();
        return changed;
    }

    bool invalidate(const QString &targetId)
    {
        for (int row = 0; row < groups_.size(); ++row) {
            Group *group = groups_.at(row);
            if (group->targetId != targetId || group->stale) continue;
            const QModelIndex groupIndex = index(row, 0);
            const int count = childCount(group);
            if (count > 0)
                beginRemoveRows(groupIndex, 0, count - 1);
            group->stale = true;
            if (count > 0)
                endRemoveRows();
            const QModelIndex groupFirst = index(row, 0);
            const QModelIndex groupLast = index(row, SearchResultsModel::COL_COUNT - 1);
            emit dataChanged(groupFirst, groupLast);
            return true;
        }
        return false;
    }

    Group *groupFor(const QModelIndex &index) const
    {
        const int row = decodedGroup(index);
        return row >= 0 && row < groups_.size() ? groups_.at(row) : nullptr;
    }

private:
    QVector<Group *> groups_;
    Group *findGroup(const QString &targetId) const
    {
        for (Group *group : groups_) {
            if (group->targetId == targetId) return group;
        }
        return nullptr;
    }
    static quintptr encode(int group, bool child)
    {
        return (quintptr(group + 1) << 1) | (child ? 1u : 0u);
    }
    static bool isChild(const QModelIndex &index) { return (index.internalId() & 1u) != 0; }
    static int decodedGroup(const QModelIndex &index)
    {
        return int(index.internalId() >> 1) - 1;
    }
    static int childCount(const Group *group)
    {
        if (!group || group->stale) return 0;
        return group->results ? group->results->groupedCount() : 0;
    }
};

// ============================================================================
//  自定义 delegate：对上下文列中的匹配关键词染色
// ============================================================================
class SearchResultItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit SearchResultItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto *tree = qobject_cast<const QTreeView *>(parent());
        const bool trackedHover = tree
            && tree->property("searchHoverRow").toInt() == index.row()
            && tree->property("searchHoverHasParent").toBool()
                   == index.parent().isValid()
            && (!index.parent().isValid()
                || tree->property("searchHoverParentRow").toInt()
                       == index.parent().row());
        const bool hoveredRow = trackedHover
            || (option.state & QStyle::State_MouseOver);
        const bool selected = option.state & QStyle::State_Selected;
        const auto paintHover = [&] {
            if (!hoveredRow || selected)
                return;
            QColor hover = option.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(option.rect, hover);
        };
        // 只对 COL_CONTEXT 列做特殊绘制
        if (index.column() != SearchResultsModel::COL_CONTEXT) {
            QStyledItemDelegate::paint(painter, option, index);
            paintHover();
            return;
        }

        const QVariantList encodedRanges =
            index.data(SearchResultsModel::MatchPositionsRole).toList();
        if (index.data(SearchResultsModel::ContextRole).isNull()
            || encodedRanges.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            paintHover();
            return;
        }

        painter->save();

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // 让当前平台样式一次性绘制交替色、悬停、整行选择和焦点边框。
        // 之后只覆盖文字，不再手工拼接几块不同高度的背景。
        opt.text.clear();
        opt.icon = QIcon();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        paintHover();

        constexpr int HorizontalPadding = 6;
        const QRect textRect = option.rect.adjusted(HorizontalPadding, 0,
                                                    -HorizontalPadding, 0);
        const QFontMetrics metrics(option.font);
        const PreparedContext prepared = prepareContext(index, textRect.width(),
                                                        metrics);
        QTextLayout textLayout(prepared.text, option.font);
        QVector<QTextLayout::FormatRange> formats;
        for (const VisualMatchRange &range : prepared.matches) {
            QTextCharFormat format;
            format.setFontWeight(QFont::DemiBold);
            format.setBackground(selected ? QColor(255, 255, 255, 72)
                                          : QColor(255, 214, 64, 155));
            format.setForeground(selected
                ? option.palette.color(QPalette::HighlightedText)
                : QColor(132, 72, 0));
            formats.append({range.start, range.length, format});
        }
        textLayout.setFormats(formats);
        textLayout.beginLayout();
        QTextLine line = textLayout.createLine();
        if (line.isValid())
            line.setLineWidth(textRect.width());
        textLayout.endLayout();
        painter->setPen(selected
            ? option.palette.color(QPalette::HighlightedText)
            : option.palette.color(QPalette::Text));
        if (line.isValid()) {
            const qreal y = textRect.top()
                + (textRect.height() - line.height()) / 2.0;
            textLayout.draw(painter, QPointF(textRect.left(), y));
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        // paint() deliberately lays out exactly one visual line. Letting the
        // platform delegate derive height from the full, potentially multi-KiB
        // DisplayRole creates a second and contradictory wrapping policy.
        hint.setHeight(option.fontMetrics.height() + 8);
        return hint;
    }
};

// ============================================================================
//  SearchResultsDock 实现
// ============================================================================
SearchResultsDock::SearchResultsDock(QWidget *parent)
    : QDockWidget(QStringLiteral("搜索结果"), parent)
    , m_controller(new SearchResultsController(this))
{
    // ⚠️ 关键修复：禁用浮动和移动，防止双击 / 拖动后嵌入错位
    //  - Fixed: 允许用户关闭/显示，不允许脱离位置
    //  - setAllowedAreas: 仅允许底部和顶部（底部初始）
    //  - MainWindow::addDockWidget(Qt::BottomDockWidgetArea) 已在 MainWindow 里设置，这里再次约束
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    auto *summaryRow = new QHBoxLayout;
    summaryRow->setContentsMargins(0, 0, 0, 0);
    m_summary = new QLabel(container);
    m_summary->setObjectName(QStringLiteral("searchResultsSummary"));
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setMinimumWidth(0);
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_rerunButton = new QPushButton(QStringLiteral("重新搜索"), container);
    m_rerunButton->setObjectName(QStringLiteral("searchResultsRerunButton"));
    m_rerunButton->setToolTip(QStringLiteral("使用当前查找条件重新生成有效结果"));
    m_rerunButton->hide();
    summaryRow->addWidget(m_summary, 1);
    summaryRow->addWidget(m_rerunButton);
    layout->addLayout(summaryRow);

    // All providers share one virtual QTreeView.
    m_stack = new QStackedWidget(container);

    m_treeView = new QTreeView(m_stack);
    m_treeView->setObjectName(QStringLiteral("searchResultsTree"));
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setWordWrap(false);
    m_treeView->setTextElideMode(Qt::ElideRight);
    m_treeView->setMouseTracking(true);
    m_treeView->viewport()->setAttribute(Qt::WA_Hover, true);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 搜索结果的核心信息是上下文。禁止 QHeaderView 自动把最后一列（匹配数）
    // 吞掉全部余量，明确让“内容”列占据可用宽度。
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // One delegate owns the whole row so hover/selection geometry cannot
    // diverge between the location, context and count columns.
    m_treeView->setItemDelegate(new SearchResultItemDelegate(m_treeView));
    m_stack->addWidget(m_treeView);

    layout->addWidget(m_stack);
    setWidget(container);

    m_stack->setCurrentIndex(0);
    connect(m_treeView, &QTreeView::doubleClicked,
            this, &SearchResultsDock::onModelDoubleClicked);
    connect(m_rerunButton, &QPushButton::clicked, this, [this]() {
        emit rerunRequested(m_controller->rerunTarget());
    });
    connect(m_controller, &SearchResultsController::targetContentChanged,
            this, &SearchResultsDock::invalidateResultsFor);
    connect(m_controller, &SearchResultsController::targetDecorationReady,
            this, &SearchResultsDock::decorateResultsFor);
    connect(m_controller, &SearchResultsController::targetClosed, this,
            [this](const QString &targetId, bool multiFile) {
                if (multiFile) {
                    if (m_multiModel && m_multiModel->invalidate(targetId))
                        m_controller->markStale();
                    m_summary->setText(
                        QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                            .arg(m_controller->pattern()).arg(m_controller->matchCount())
                            .arg(m_controller->incompleteCount()).arg(m_controller->staleCount()));
                    setRerunAvailable(true);
                    return;
                }
                SearchResultsModel *oldModel = m_model;
                m_model = nullptr;
                m_treeView->setModel(nullptr);
                if (oldModel && oldModel->QObject::parent() == this)
                    oldModel->deleteLater();
                m_summary->setText(QStringLiteral("源文档已关闭，搜索结果已失效。"));
                setRerunAvailable(false);
            });

    // 关键：安装事件过滤器到 m_treeView viewport，以捕获 context 列内的点击位置
    //   —— 用于按"点击的匹配词"精准跳转（而非仅跳到第一个）
    m_treeView->viewport()->installEventFilter(this);
}

SearchResultsDock::~SearchResultsDock() = default;

void SearchResultsDock::setRerunAvailable(bool available,
                                          EditorTab *preferredTarget)
{
    m_controller->setRerunTarget(available ? preferredTarget : nullptr);
    if (m_rerunButton)
        m_rerunButton->setVisible(available);
}

void SearchResultsDock::updateSummary(const QString &pattern, const QString &fileName,
                                      qint64 totalMatches)
{
    m_summary->setText(QStringLiteral("%1 · “%2” · %3 处")
                           .arg(fileName, pattern).arg(totalMatches));
}

void SearchResultsDock::setResults(const QVector<SearchMatch> &matches,
                                   const QString &pattern,
                                   const QString &fileName,
                                   EditorTab *target,
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
    if (m_controller->singleTarget())
        m_controller->singleTarget()->clearSearchHighlights();
    for (EditorTab *target : m_controller->targets())
        target->clearSearchHighlights();
    m_lastHitRow = -1;
    m_lastHitMatchIdx = -1;
    m_controller->clear();
    m_singlePreviewMatches.clear();
    m_model = nullptr;
    if (m_treeView)
        m_treeView->setModel(nullptr);
    if (oldSingleModel && oldSingleModel->QObject::parent() == this)
        delete oldSingleModel;
    delete m_multiModel;
    m_multiModel = nullptr;
    if (m_summary)
        m_summary->clear();
    hide();
}

void SearchResultsDock::setResultsModel(SearchResultsModel *model,
                                        const QString &pattern,
                                        const QString &fileName,
                                        EditorTab *target)
{
    setRerunAvailable(false);
    m_controller->beginSingle(target);
    m_singlePreviewMatches.clear();
    SearchResultsModel *oldModel = m_model;
    m_model = model;
    // 切到 L2 视图
    m_stack->setCurrentIndex(0);
    m_treeView->setModel(model);  // nullptr 时会清空
    m_treeView->setRootIsDecorated(false);
    if (oldModel && oldModel != model && oldModel->QObject::parent() == this)
        delete oldModel;
    if (model) {
        // setModel() 会按新模型的列数重建 header section；构造阶段对尚不存在的
        // 第 2/3 列设置 resize mode 在部分 Qt 版本中不会保留，因此必须在这里
        // 再应用一次产品布局。
        auto *header = m_treeView->header();
        header->setStretchLastSection(false);
        header->setSectionResizeMode(SearchResultsModel::COL_LINE,
                                     QHeaderView::ResizeToContents);
        header->setSectionResizeMode(SearchResultsModel::COL_CONTEXT,
                                     QHeaderView::Stretch);
        header->setSectionResizeMode(SearchResultsModel::COL_MATCHCOUNT,
                                     QHeaderView::ResizeToContents);
        qint64 total = model->totalMatches();
        qint64 lines = model->groupedCount();
        const QString groupUnit = model->bytePositionMode()
            ? QStringLiteral("个字节窗口") : QStringLiteral("行");
        m_summary->setText(QStringLiteral("%1 · “%2” · %3 处 / %4 %5")
                               .arg(fileName, pattern).arg(total)
                               .arg(lines).arg(groupUnit));
        if (model->isIncomplete()) {
            m_summary->setText(
                QStringLiteral("%1 · “%2” · 至少 %3 处（%4）")
                    .arg(fileName, pattern).arg(total)
                    .arg(model->incompleteReason()));
        }
        // The line index may have completed after this search captured its
        // byte snapshot but before the model reached the Dock.
        decorateResultsFor(target);
    } else {
        updateSummary(pattern, fileName, 0);
    }
}

void SearchResultsDock::beginSingleFileResults(const QString &pattern,
                                                const QString &fileName,
                                                EditorTab *target,
                                                TextEncoding encoding,
                                                bool bytePositionMode)
{
    auto *preview = new SearchResultsModel(this);
    preview->setBytePositionMode(bytePositionMode);
    preview->setTextEncoding(encoding);
    setResultsModel(preview, pattern, fileName, target);
    m_summary->setText(QStringLiteral("%1 · 正在搜索 “%2”…")
                           .arg(fileName, pattern));
    show();
    raise();
}

void SearchResultsDock::appendSingleFilePreview(
    EditorTab *target, const QVector<LargeFileSearchMatch> &batch,
    TextEncoding encoding, bool bytePositionMode)
{
    if (m_controller->isMultiFile() || !target
        || target != m_controller->singleTarget() || !m_model
        || !m_controller->isCurrent(target) || batch.isEmpty()
        || m_model->textEncoding() != encoding
        || m_model->bytePositionMode() != bytePositionMode) {
        return;
    }
    const int remaining = 5000 - m_singlePreviewMatches.size();
    if (remaining <= 0) return;
    const int take = qMin(remaining, batch.size());
    for (int i = 0; i < take; ++i) m_singlePreviewMatches.append(batch.at(i));
    m_model->setResults(m_singlePreviewMatches, {}, 0);
    m_summary->setText(QStringLiteral("%1 · 已发现至少 %2 处…")
                           .arg(target->fileName()).arg(m_singlePreviewMatches.size()));
}

void SearchResultsDock::decorateResultsFor(EditorTab *tab)
{
    if (!tab || !tab->isL2Mode() || !tab->l2View()
        || tab->l2View()->lineIndex().isIncomplete()) {
        return;
    }
    const LineIndex &index = tab->l2View()->lineIndex();
    if (!m_controller->isMultiFile()) {
        if (m_controller->singleTarget() == tab && m_model)
            m_model->decorateLineNumbers(index);
        return;
    }
    if (!m_multiModel)
        return;
    const QString targetId = m_controller->targetId(tab);
    if (!targetId.isEmpty())
        m_multiModel->decorate(targetId, index);
}

void SearchResultsDock::invalidateResultsFor(EditorTab *tab)
{
    if (!tab)
        return;
    tab->clearSearchHighlights();

    if (m_controller->isMultiFile()) {
        QString targetId = m_controller->targetId(tab);
        if (targetId.isEmpty())
            targetId = m_controller->ensureMultiTarget(tab);

        if (m_multiModel && m_multiModel->invalidate(targetId)) {
            m_controller->markStale();
        }
        m_controller->invalidateRevision(targetId);
        m_summary->setText(
            QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                .arg(m_controller->pattern()).arg(m_controller->matchCount())
                .arg(m_controller->incompleteCount()).arg(m_controller->staleCount()));
        setRerunAvailable(true);
        show();
        return;
    }

    SearchResultsModel *oldModel = m_model;
    m_model = nullptr;
    m_treeView->setModel(nullptr);
    if (oldModel && oldModel->QObject::parent() == this)
        delete oldModel;
    m_controller->clear();
    m_summary->setText(QStringLiteral("文档内容已修改，搜索结果已失效；请重新搜索。"));
    setRerunAvailable(true, tab);
    show();
}

void SearchResultsDock::beginMultiFileResults(const QString &pattern, int targetCount)
{
    setRerunAvailable(false);
    m_controller->beginMulti(pattern);
    m_stack->setCurrentIndex(0);
    m_treeView->setModel(nullptr);
    delete m_multiModel;
    m_multiModel = new MultiSearchResultsModel(this);
    m_treeView->setModel(m_multiModel);
    m_treeView->setRootIsDecorated(true);
    auto *header = m_treeView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(SearchResultsModel::COL_LINE,
                                 QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SearchResultsModel::COL_CONTEXT,
                                 QHeaderView::Stretch);
    header->setSectionResizeMode(SearchResultsModel::COL_MATCHCOUNT,
                                 QHeaderView::ResizeToContents);
    m_summary->setText(QStringLiteral("正在全部 %1 个打开文件中搜索 “%2”…")
                           .arg(targetCount).arg(pattern));
    show();
    raise();
}

void SearchResultsDock::beginMultiFileTarget(EditorTab *tab,
                                              TextEncoding encoding,
                                              bool bytePositionMode)
{
    if (!m_controller->isMultiFile() || !m_multiModel || !tab) return;
    const QString targetId = m_controller->ensureMultiTarget(tab);
    auto *preview = new SearchResultsModel(m_multiModel);
    preview->setBytePositionMode(bytePositionMode);
    preview->setTextEncoding(encoding);
    m_multiModel->addPreview(tab, targetId, preview);
}

void SearchResultsDock::appendMultiFilePreview(
    EditorTab *tab, const QVector<LargeFileSearchMatch> &batch,
    TextEncoding encoding, bool bytePositionMode)
{
    if (!m_controller->isMultiFile() || !m_multiModel || !tab || batch.isEmpty()) return;
    const QString targetId = m_controller->ensureMultiTarget(tab);
    if (m_multiModel->appendPreview(targetId, batch, encoding,
                                    bytePositionMode)) {
        m_summary->setText(QStringLiteral("正在全部打开文件中搜索 “%1”…")
                               .arg(m_controller->pattern()));
    }
}

void SearchResultsDock::appendMultiFileResults(EditorTab *tab,
                                               const QVector<SearchMatch> &matches,
                                               bool incomplete)
{
    if (!m_controller->isMultiFile() || !tab)
        return;
    const QString targetId = m_controller->ensureMultiTarget(tab);
    if (m_multiModel) {
        m_multiModel->addL1(tab, targetId, matches, incomplete);
        m_treeView->expand(m_multiModel->index(m_multiModel->rowCount() - 1, 0));
    }
    if (incomplete)
        m_controller->markIncomplete();
    m_controller->addMatches(matches.size());
    m_summary->setText(m_controller->incompleteCount() > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()).arg(m_controller->incompleteCount())
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()));
}

void SearchResultsDock::appendMultiFileResults(EditorTab *tab,
                                               SearchResultsModel *model)
{
    if (!m_controller->isMultiFile() || !tab || !model)
        return;
    const QString targetId = m_controller->ensureMultiTarget(tab);
    if (m_multiModel) {
        if (!m_multiModel->finish(targetId, model))
            m_multiModel->addModel(tab, targetId, model);
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
    decorateResultsFor(tab);
    m_summary->setText(m_controller->incompleteCount() > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()).arg(m_controller->incompleteCount())
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_controller->pattern()).arg(m_controller->matchCount()));
}

void SearchResultsDock::finishMultiFileResults(bool cancelled)
{
    if (!m_controller->isMultiFile())
        return;
    if (cancelled) {
        m_summary->setText(
            QStringLiteral("已取消在全部打开文件中搜索 “%1”；已保留 %2 处完成结果")
                .arg(m_controller->pattern()).arg(m_controller->matchCount()));
        return;
    }
    if (m_controller->staleCount() > 0 || m_controller->incompleteCount() > 0) {
        m_summary->setText(
            QStringLiteral("在全部打开文件中搜索 “%1”：找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                .arg(m_controller->pattern()).arg(m_controller->matchCount())
                .arg(m_controller->incompleteCount()).arg(m_controller->staleCount()));
    } else {
        m_summary->setText(QStringLiteral("在全部打开文件中搜索 “%1”：找到 %2 处匹配")
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
                    int matchIdx = hitTestModelMatch(idx, xInCol,
                                                     colRect.width(), fm);
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
        EditorTab *tab = group->tab;
        if (!m_controller->isCurrent(tab, group->targetId)) {
            invalidateResultsFor(tab);
            return;
        }
        const QVariantList ranges =
            index.data(SearchResultsModel::MatchByteRangesRole).toList();
        if (ranges.isEmpty()) return;
        int chosen = (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0)
            ? m_lastHitMatchIdx : 0;
        chosen = qBound(0, chosen, ranges.size() - 1);
        const QVariantMap range = ranges.at(chosen).toMap();
        emit jumpToTabByteOffset(
            tab, range.value(QStringLiteral("byteOffset")).toULongLong(),
            range.value(QStringLiteral("length")).toInt());
        return;
    }
    if (!m_model) return;
    if (m_controller->singleTarget()
        && !m_controller->isCurrent(m_controller->singleTarget())) {
        invalidateResultsFor(m_controller->singleTarget());
        return;
    }
    qint64 byteOff = -1;
    int    length  = 0;
    // 如果当前按下位置是同一个行并且有命中的匹配索引，则按该索引跳转
    if (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0) {
        if (m_model->matchByteOffsetAtIndex(index.row(), m_lastHitMatchIdx, &byteOff, &length)) {
            // 命中：跳转到用户点击的具体匹配词
            if (m_controller->singleTarget())
                emit jumpToTabByteOffset(m_controller->singleTarget(), byteOff, length);
            else
                emit jumpToByteOffset(byteOff, length);
            return;
        }
    }
    // 回退：跳转到第一个匹配（保持兼容）
    byteOff = m_model->matchByteOffset(index);
    length  = m_model->matchLength(index);
    if (byteOff >= 0) {
        if (m_controller->singleTarget())
            emit jumpToTabByteOffset(m_controller->singleTarget(), byteOff, length);
        else
            emit jumpToByteOffset(byteOff, length);
    }
}

#include "SearchResultsDock.moc"
