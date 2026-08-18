#include "SearchResultsDock.h"
#include "VirtualFileView.h"

#include <QTreeView>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QPalette>
#include <QMouseEvent>
#include <QUuid>
#include <limits>
#include <utility>

namespace {
constexpr int TargetRole = Qt::UserRole + 20;
constexpr int L2Role = Qt::UserRole + 21;
constexpr int PositionRole = Qt::UserRole + 22;
constexpr int IndexRole = Qt::UserRole + 23;
constexpr int MatchLengthRole = Qt::UserRole + 24;
constexpr int StaleRole = Qt::UserRole + 25;

struct GroupedL1Line {
    int line = 0;
    QString context;
    QVector<QPair<int, int>> matches;
    QVector<QPair<qint64, int>> byteRanges;
};

QVector<GroupedL1Line> groupL1Matches(const QVector<SearchMatch> &matches)
{
    QVector<GroupedL1Line> grouped;
    for (const SearchMatch &match : matches) {
        if (grouped.isEmpty() || grouped.constLast().line != match.line) {
            GroupedL1Line line;
            line.line = match.line;
            line.context = match.context;
            grouped.append(std::move(line));
        }
        grouped.last().matches.append(qMakePair(match.index, match.length));
        grouped.last().byteRanges.append(qMakePair(match.byteOffset,
                                                    int(match.byteLength)));
    }
    return grouped;
}

int hitTestModelMatch(const QModelIndex &index, int xInColumn,
                      const QFontMetrics &metrics)
{
    const QString text = index.data(SearchResultsModel::ContextRole).toString();
    const QVariantList ranges =
        index.data(SearchResultsModel::MatchPositionsRole).toList();
    const int x = xInColumn - 4;
    if (x < 0 || text.isEmpty() || ranges.isEmpty()) return -1;
    int nearest = -1;
    int nearestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < ranges.size(); ++i) {
        const QVariantMap range = ranges.at(i).toMap();
        const int start = qBound(0, range.value(QStringLiteral("start")).toInt(),
                                 text.size());
        const int length = qMax(0, range.value(QStringLiteral("length")).toInt());
        const int end = qBound(start, start + length, text.size());
        const int startX = metrics.horizontalAdvance(text.left(start));
        const int endX = metrics.horizontalAdvance(text.left(end));
        if (x >= startX && x <= endX) return i;
        const int distance = qMin(qAbs(x - startX), qAbs(x - endX));
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    return nearest;
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
        QVector<GroupedL1Line> l1;
        QPointer<SearchResultsModel> l2;
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
            if (role == Qt::FontRole && index.column() == 0) {
                QFont font;
                font.setBold(true);
                return font;
            }
            if (role != Qt::DisplayRole) return {};
            if (index.column() == 0) {
                return group->stale
                    ? QStringLiteral("%1 — 结果已失效，请重新搜索此文件")
                          .arg(group->displayName)
                    : (group->incompleteReason.isEmpty()
                        ? QStringLiteral("%1 — %2 处")
                              .arg(group->displayName).arg(group->totalMatches)
                        : QStringLiteral("%1 — 至少 %2 处（结果不完整）")
                              .arg(group->displayName).arg(group->totalMatches));
            }
            if (index.column() == SearchResultsModel::COL_MATCHCOUNT)
                return group->totalMatches;
            return {};
        }

        if (group->stale) return {};
        if (role == TargetRole) return group->targetId;
        if (role == L2Role) return bool(group->l2);
        if (role == StaleRole) return false;
        if (group->l2) {
            const QModelIndex source = group->l2->index(index.row(), index.column());
            return group->l2->data(source, role);
        }
        if (index.row() < 0 || index.row() >= group->l1.size()) return {};
        const GroupedL1Line &line = group->l1.at(index.row());
        if (role == PositionRole || role == SearchResultsModel::LineRole)
            return line.line;
        if (role == IndexRole)
            return line.matches.isEmpty() ? 0 : line.matches.constFirst().first;
        if (role == MatchLengthRole || role == SearchResultsModel::LengthRole)
            return line.matches.isEmpty() ? 0 : line.matches.constFirst().second;
        if (role == SearchResultsModel::ByteOffsetRole)
            return line.byteRanges.isEmpty() ? QVariant{} : line.byteRanges.first().first;
        if (role == SearchResultsModel::MatchByteRangesRole) {
            QVariantList ranges;
            for (const auto &match : line.byteRanges) {
                if (match.first < 0 || match.second < 0) continue;
                QVariantMap range;
                range.insert(QStringLiteral("byteOffset"), match.first);
                range.insert(QStringLiteral("length"), match.second);
                ranges.append(range);
            }
            return ranges;
        }
        if (role == SearchResultsModel::ContextRole) return line.context;
        if (role == SearchResultsModel::MatchCountInLineRole) return line.matches.size();
        if (role == SearchResultsModel::MatchPositionsRole) {
            QVariantList ranges;
            for (const auto &match : line.matches) {
                QVariantMap range;
                range.insert(QStringLiteral("start"), match.first);
                range.insert(QStringLiteral("length"), match.second);
                ranges.append(range);
            }
            return ranges;
        }
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case SearchResultsModel::COL_LINE: return line.line + 1;
            case SearchResultsModel::COL_CONTEXT: return line.context;
            case SearchResultsModel::COL_MATCHCOUNT: return line.matches.size();
            default: return {};
            }
        }
        return {};
    }

    void addL1(EditorTab *tab, const QString &targetId,
               const QVector<SearchMatch> &matches, bool incomplete = false)
    {
        auto *group = new Group;
        group->tab = tab;
        group->targetId = targetId;
        group->displayName = tab ? tab->fileName() : QString();
        group->l1 = groupL1Matches(matches);
        group->totalMatches = matches.size();
        if (incomplete)
            group->incompleteReason = QStringLiteral("结果达到 200,000 处安全上限");
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void addL2(EditorTab *tab, const QString &targetId, SearchResultsModel *model)
    {
        auto *group = new Group;
        group->tab = tab;
        group->targetId = targetId;
        group->displayName = tab ? tab->fileName() : QString();
        group->l2 = model;
        group->totalMatches = model ? model->totalMatches() : 0;
        group->incompleteReason = model ? model->incompleteReason() : QString();
        if (model) model->setParent(this);
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void addL2Preview(EditorTab *tab, const QString &targetId,
                      SearchResultsModel *model)
    {
        addL2(tab, targetId, model);
    }

    bool appendL2Preview(const QString &targetId,
                         const QVector<LargeFileSearchMatch> &batch)
    {
        Group *group = findGroup(targetId);
        if (!group || !group->l2 || group->stale || batch.isEmpty()) return false;
        const int remaining = 5000 - group->preview.size();
        if (remaining <= 0) return true;
        const int take = qMin(remaining, batch.size());
        for (int i = 0; i < take; ++i) group->preview.append(batch.at(i));
        group->l2->setResults(group->preview, {}, 0);
        group->totalMatches = group->preview.size();
        const int row = groups_.indexOf(group);
        emit dataChanged(index(row, 0), index(row, SearchResultsModel::COL_COUNT - 1));
        return true;
    }

    bool finishL2(const QString &targetId, SearchResultsModel *model)
    {
        Group *group = findGroup(targetId);
        if (!group || !model) return false;
        SearchResultsModel *oldModel = group->l2;
        beginResetModel();
        group->l2 = model;
        group->preview.clear();
        group->totalMatches = model->totalMatches();
        group->incompleteReason = model->incompleteReason();
        model->setParent(this);
        endResetModel();
        if (oldModel && oldModel != model) delete oldModel;
        return true;
    }

    bool decorateL2(const QString &targetId, const LineIndex &lineIndex)
    {
        Group *group = findGroup(targetId);
        if (!group || !group->l2 || group->stale
            || !group->l2->hasUnknownLineNumbers()) {
            return false;
        }
        beginResetModel();
        const bool changed = group->l2->decorateLineNumbers(lineIndex);
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
        return group->l2 ? group->l2->groupedCount() : group->l1.size();
    }
};

class FlatL1SearchResultsModel final : public QAbstractTableModel {
public:
    explicit FlatL1SearchResultsModel(const QVector<SearchMatch> &matches,
                                      QObject *parent = nullptr)
        : QAbstractTableModel(parent), lines_(groupL1Matches(matches)) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    { return parent.isValid() ? 0 : lines_.size(); }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    { return parent.isValid() ? 0 : SearchResultsModel::COL_COUNT; }
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case SearchResultsModel::COL_LINE: return QStringLiteral("行");
        case SearchResultsModel::COL_CONTEXT: return QStringLiteral("内容");
        case SearchResultsModel::COL_MATCHCOUNT: return QStringLiteral("匹配数");
        default: return {};
        }
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= lines_.size()) return {};
        const GroupedL1Line &line = lines_.at(index.row());
        if (role == PositionRole || role == SearchResultsModel::LineRole) return line.line;
        if (role == IndexRole) return line.matches.isEmpty() ? 0 : line.matches.first().first;
        if (role == MatchLengthRole || role == SearchResultsModel::LengthRole)
            return line.matches.isEmpty() ? 0 : line.matches.first().second;
        if (role == SearchResultsModel::ByteOffsetRole)
            return line.byteRanges.isEmpty() ? QVariant{} : line.byteRanges.first().first;
        if (role == SearchResultsModel::MatchByteRangesRole) {
            QVariantList ranges;
            for (const auto &match : line.byteRanges) {
                if (match.first < 0 || match.second < 0) continue;
                QVariantMap range;
                range.insert(QStringLiteral("byteOffset"), match.first);
                range.insert(QStringLiteral("length"), match.second);
                ranges.append(range);
            }
            return ranges;
        }
        if (role == SearchResultsModel::ContextRole) return line.context;
        if (role == SearchResultsModel::MatchCountInLineRole) return line.matches.size();
        if (role == SearchResultsModel::MatchPositionsRole) {
            QVariantList ranges;
            for (const auto &match : line.matches) {
                QVariantMap range;
                range.insert(QStringLiteral("start"), match.first);
                range.insert(QStringLiteral("length"), match.second);
                ranges.append(range);
            }
            return ranges;
        }
        if (role != Qt::DisplayRole) return {};
        switch (index.column()) {
        case SearchResultsModel::COL_LINE: return line.line + 1;
        case SearchResultsModel::COL_CONTEXT: return line.context;
        case SearchResultsModel::COL_MATCHCOUNT: return line.matches.size();
        default: return {};
        }
    }
private:
    QVector<GroupedL1Line> lines_;
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
        // 只对 COL_CONTEXT 列做特殊绘制
        if (index.column() != SearchResultsModel::COL_CONTEXT) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const QString text = index.data(SearchResultsModel::ContextRole).toString();
        const QVariantList encodedRanges =
            index.data(SearchResultsModel::MatchPositionsRole).toList();
        if (text.isNull() || encodedRanges.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->save();

        // 背景（交替行颜色 + 选中高亮）
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, opt.palette.highlight());
        } else if (opt.features & QStyleOptionViewItem::Alternate) {
            painter->fillRect(opt.rect, opt.palette.alternateBase());
        } else {
            painter->fillRect(opt.rect, opt.palette.base());
        }

        // 文字区域（留 padding）
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);
        QFontMetrics fm(opt.font);
        int ascent = fm.ascent();
        int y = opt.rect.y() + (opt.rect.height() - fm.height()) / 2 + ascent;

        // 先画匹配背景
        QColor hlBg(0xFF, 0xFF, 0x00, 0xC0);  // 半透明黄
        QColor hlFg(0xFF, 0x00, 0x00);         // 红
        QColor normalFg = (opt.state & QStyle::State_Selected)
                          ? opt.palette.color(QPalette::HighlightedText)
                          : opt.palette.color(QPalette::Text);

        // 计算文本宽度限制（超出显示区域的用省略号）
        int maxWidth = textRect.width();
        QString elidedText = fm.elidedText(text, Qt::ElideRight, maxWidth);

        // Draw the shaped string as a whole.  Painting one QChar at a time
        // splits emoji surrogate pairs and combining sequences, disables font
        // fallback/kerning and makes the result text visibly corrupt.  Match
        // ranges are UTF-16 positions, so their pixel rectangles can be
        // derived from shaped prefixes; clipping a second full-string paint
        // then changes only the matched foreground without breaking glyphs.
        painter->setFont(opt.font);
        QVector<QRect> highlightRects;
        for (const QVariant &encoded : encodedRanges) {
            const QVariantMap range = encoded.toMap();
            const int start = qBound(
                0, range.value(QStringLiteral("start")).toInt(), elidedText.size());
            const int end = qBound(
                start,
                start + qMax(0, range.value(QStringLiteral("length")).toInt()),
                elidedText.size());
            if (end <= start)
                continue;
            const int startX = fm.horizontalAdvance(elidedText.left(start));
            const int endX = fm.horizontalAdvance(elidedText.left(end));
            QRect highlightRect(textRect.left() + startX, opt.rect.y(),
                                qMax(1, endX - startX), opt.rect.height());
            highlightRect = highlightRect.intersected(textRect);
            if (!highlightRect.isEmpty()) {
                painter->fillRect(highlightRect, hlBg);
                highlightRects.append(highlightRect);
            }
        }

        painter->setPen(normalFg);
        painter->drawText(textRect.left(), y, elidedText);
        painter->setPen(hlFg);
        for (const QRect &highlightRect : std::as_const(highlightRects)) {
            painter->save();
            painter->setClipRect(highlightRect);
            painter->drawText(textRect.left(), y, elidedText);
            painter->restore();
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

// ============================================================================
//  SearchResultsDock 实现
// ============================================================================
SearchResultsDock::SearchResultsDock(QWidget *parent)
    : QDockWidget(QStringLiteral("搜索结果"), parent)
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
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 搜索结果的核心信息是上下文。禁止 QHeaderView 自动把最后一列（匹配数）
    // 吞掉全部余量，明确让“内容”列占据可用宽度。
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // 设置自定义 delegate 对上下文列染色
    m_treeView->setItemDelegateForColumn(SearchResultsModel::COL_CONTEXT,
                                         new SearchResultItemDelegate(m_treeView));
    m_stack->addWidget(m_treeView);

    layout->addWidget(m_stack);
    setWidget(container);

    m_stack->setCurrentIndex(0);
    connect(m_treeView, &QTreeView::doubleClicked,
            this, &SearchResultsDock::onModelDoubleClicked);
    connect(m_rerunButton, &QPushButton::clicked, this, [this]() {
        emit rerunRequested(m_rerunTarget.data());
    });

    // 关键：安装事件过滤器到 m_treeView viewport，以捕获 context 列内的点击位置
    //   —— 用于按"点击的匹配词"精准跳转（而非仅跳到第一个）
    m_treeView->viewport()->installEventFilter(this);
}

void SearchResultsDock::setRerunAvailable(bool available,
                                          EditorTab *preferredTarget)
{
    m_rerunTarget = available ? preferredTarget : nullptr;
    if (m_rerunButton)
        m_rerunButton->setVisible(available);
}

void SearchResultsDock::updateSummary(const QString &pattern, const QString &fileName,
                                      qint64 totalMatches)
{
    m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：找到 %3 处匹配")
                           .arg(fileName)
                           .arg(pattern)
                           .arg(totalMatches));
}

void SearchResultsDock::setResults(const QVector<SearchMatch> &matches,
                                   const QString &pattern,
                                   const QString &fileName,
                                   EditorTab *target,
                                   bool incomplete)
{
    setRerunAvailable(false);
    SearchResultsModel *oldModel = m_model;
    m_model = nullptr;
    resetTargetWatches();
    m_multiFileMode = false;
    m_multiTargets.clear();
    m_multiTargetRevisions.clear();
    m_singleTarget.clear();
    m_singleTargetRevision = 0;
    watchTarget(target);
    // L1 and L2 now share the same virtual view/delegate/navigation path.
    m_stack->setCurrentIndex(0);
    m_treeView->setModel(nullptr);
    if (oldModel && oldModel->QObject::parent() == this)
        delete oldModel;
    delete m_multiModel;
    m_multiModel = nullptr;
    delete m_singleL1Model;
    m_singleL1Model = new FlatL1SearchResultsModel(matches, this);
    m_treeView->setModel(m_singleL1Model);
    m_treeView->setRootIsDecorated(false);
    if (incomplete) {
        m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：至少 %3 处匹配（结果已截断）")
                               .arg(fileName).arg(pattern).arg(matches.size()));
    } else {
        updateSummary(pattern, fileName, qint64(matches.size()));
    }
}

void SearchResultsDock::clearResults()
{
    setRerunAvailable(false);
    SearchResultsModel *oldSingleModel = m_model;
    if (m_singleTarget)
        m_singleTarget->clearSearchHighlights();
    for (auto it = m_multiTargets.cbegin(); it != m_multiTargets.cend(); ++it) {
        if (it.value())
            it.value()->clearSearchHighlights();
    }
    resetTargetWatches();
    m_lastHitRow = -1;
    m_lastHitMatchIdx = -1;
    m_multiFileMode = false;
    m_multiPattern.clear();
    m_multiMatchCount = 0;
    m_multiStaleCount = 0;
    m_multiIncompleteCount = 0;
    m_multiTargets.clear();
    m_multiTargetRevisions.clear();
    m_singleTarget.clear();
    m_singleTargetRevision = 0;
    m_singlePreviewMatches.clear();
    m_model = nullptr;
    if (m_treeView)
        m_treeView->setModel(nullptr);
    delete m_singleL1Model;
    m_singleL1Model = nullptr;
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
    resetTargetWatches();
    m_multiFileMode = false;
    m_multiTargets.clear();
    m_multiTargetRevisions.clear();
    m_singleTarget.clear();
    m_singleTargetRevision = 0;
    watchTarget(target);
    m_singlePreviewMatches.clear();
    SearchResultsModel *oldModel = m_model;
    m_model = model;
    delete m_singleL1Model;
    m_singleL1Model = nullptr;
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
        m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：找到 %3 处匹配（%4 %5）")
                               .arg(fileName)
                               .arg(pattern)
                               .arg(total)
                               .arg(lines)
                               .arg(groupUnit));
        if (model->isIncomplete()) {
            m_summary->setText(
                QStringLiteral("在 %1 中搜索 \"%2\"：至少 %3 处匹配（结果不完整：%4）")
                    .arg(fileName).arg(pattern).arg(total)
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
                                                EditorTab *target)
{
    auto *preview = new SearchResultsModel(this);
    preview->setBytePositionMode(target && target->isBinaryDocument());
    setResultsModel(preview, pattern, fileName, target);
    m_summary->setText(QStringLiteral("正在 %1 中搜索 “%2”…")
                           .arg(fileName, pattern));
    show();
    raise();
}

void SearchResultsDock::appendSingleFilePreview(
    EditorTab *target, const QVector<LargeFileSearchMatch> &batch)
{
    if (m_multiFileMode || !target || target != m_singleTarget || !m_model
        || !targetRevisionIsCurrent(target) || batch.isEmpty()) {
        return;
    }
    const int remaining = 5000 - m_singlePreviewMatches.size();
    if (remaining <= 0) return;
    const int take = qMin(remaining, batch.size());
    for (int i = 0; i < take; ++i) m_singlePreviewMatches.append(batch.at(i));
    m_model->setResults(m_singlePreviewMatches, {}, 0);
    m_summary->setText(QStringLiteral("正在 %1 中搜索：已发现至少 %2 处…")
                           .arg(target->fileName()).arg(m_singlePreviewMatches.size()));
}

QString SearchResultsDock::registerTarget(EditorTab *tab)
{
    for (auto it = m_multiTargets.cbegin(); it != m_multiTargets.cend(); ++it) {
        if (it.value() == tab)
            return it.key();
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_multiTargets.insert(id, tab);
    watchTarget(tab, id);
    return id;
}

void SearchResultsDock::resetTargetWatches()
{
    for (const auto &connection : std::as_const(m_targetConnections))
        QObject::disconnect(connection);
    m_targetConnections.clear();
}

void SearchResultsDock::watchTarget(EditorTab *tab, const QString &targetId)
{
    if (!tab)
        return;
    if (targetId.isEmpty()) {
        m_singleTarget = tab;
        m_singleTargetRevision = tab->contentRevision();
    } else {
        m_multiTargetRevisions.insert(targetId, tab->contentRevision());
    }
    QPointer<EditorTab> guard(tab);
    m_targetConnections.push_back(connect(
        tab, &EditorTab::contentChanged, this,
        [this, guard](quint64) {
            if (guard)
                invalidateResultsFor(guard);
        }));
    m_targetConnections.push_back(connect(
        tab, &EditorTab::l2LineBuildFinished, this,
        [this, guard](bool ok) {
            if (ok && guard)
                decorateResultsFor(guard);
        }));
    m_targetConnections.push_back(connect(
        tab, &QObject::destroyed, this,
        [this, targetId]() {
            if (m_multiFileMode && !targetId.isEmpty()) {
                if (m_multiModel && m_multiModel->invalidate(targetId))
                    ++m_multiStaleCount;
                m_multiTargetRevisions.remove(targetId);
                m_summary->setText(
                    QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                        .arg(m_multiPattern).arg(m_multiMatchCount)
                        .arg(m_multiIncompleteCount).arg(m_multiStaleCount));
                setRerunAvailable(true);
            } else if (!m_multiFileMode) {
                SearchResultsModel *oldModel = m_model;
                m_singleTarget.clear();
                m_singleTargetRevision = 0;
                m_model = nullptr;
                m_treeView->setModel(nullptr);
                delete m_singleL1Model;
                m_singleL1Model = nullptr;
                if (oldModel && oldModel->QObject::parent() == this)
                    oldModel->deleteLater();
                m_summary->setText(QStringLiteral("源文档已关闭，搜索结果已失效。"));
                setRerunAvailable(false);
            }
        }));
}

void SearchResultsDock::decorateResultsFor(EditorTab *tab)
{
    if (!tab || !tab->isL2Mode() || !tab->l2View()
        || tab->l2View()->lineIndex().isIncomplete()) {
        return;
    }
    const LineIndex &index = tab->l2View()->lineIndex();
    if (!m_multiFileMode) {
        if (m_singleTarget == tab && m_model)
            m_model->decorateLineNumbers(index);
        return;
    }
    if (!m_multiModel)
        return;
    for (auto it = m_multiTargets.cbegin(); it != m_multiTargets.cend(); ++it) {
        if (it.value() == tab) {
            m_multiModel->decorateL2(it.key(), index);
            return;
        }
    }
}

bool SearchResultsDock::targetRevisionIsCurrent(EditorTab *tab,
                                                const QString &targetId) const
{
    if (!tab)
        return false;
    if (targetId.isEmpty())
        return m_singleTarget == tab && m_singleTargetRevision == tab->contentRevision();
    return m_multiTargets.value(targetId) == tab
           && m_multiTargetRevisions.value(targetId, std::numeric_limits<quint64>::max())
                  == tab->contentRevision();
}

void SearchResultsDock::invalidateResultsFor(EditorTab *tab)
{
    if (!tab)
        return;
    tab->clearSearchHighlights();

    if (m_multiFileMode) {
        QString targetId;
        for (auto it = m_multiTargets.cbegin(); it != m_multiTargets.cend(); ++it) {
            if (it.value() == tab) {
                targetId = it.key();
                break;
            }
        }
        if (targetId.isEmpty())
            targetId = registerTarget(tab);

        if (m_multiModel && m_multiModel->invalidate(targetId)) {
            ++m_multiStaleCount;
        }
        m_multiTargetRevisions.remove(targetId);
        m_summary->setText(
            QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                .arg(m_multiPattern).arg(m_multiMatchCount)
                .arg(m_multiIncompleteCount).arg(m_multiStaleCount));
        setRerunAvailable(true);
        show();
        return;
    }

    resetTargetWatches();
    SearchResultsModel *oldModel = m_model;
    m_model = nullptr;
    m_treeView->setModel(nullptr);
    delete m_singleL1Model;
    m_singleL1Model = nullptr;
    if (oldModel && oldModel->QObject::parent() == this)
        delete oldModel;
    m_multiFileMode = false;
    m_multiTargets.clear();
    m_multiTargetRevisions.clear();
    m_singleTarget.clear();
    m_singleTargetRevision = 0;
    m_summary->setText(QStringLiteral("文档内容已修改，搜索结果已失效；请重新搜索。"));
    setRerunAvailable(true, tab);
    show();
}

void SearchResultsDock::beginMultiFileResults(const QString &pattern, int targetCount)
{
    setRerunAvailable(false);
    resetTargetWatches();
    m_multiFileMode = true;
    m_multiPattern = pattern;
    m_multiMatchCount = 0;
    m_multiStaleCount = 0;
    m_multiIncompleteCount = 0;
    m_multiTargets.clear();
    m_multiTargetRevisions.clear();
    m_singleTarget.clear();
    m_singleTargetRevision = 0;
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

void SearchResultsDock::beginMultiFileTarget(EditorTab *tab)
{
    if (!m_multiFileMode || !m_multiModel || !tab) return;
    const QString targetId = registerTarget(tab);
    auto *preview = new SearchResultsModel(m_multiModel);
    preview->setBytePositionMode(tab->isBinaryDocument());
    m_multiModel->addL2Preview(tab, targetId, preview);
}

void SearchResultsDock::appendMultiFilePreview(
    EditorTab *tab, const QVector<LargeFileSearchMatch> &batch)
{
    if (!m_multiFileMode || !m_multiModel || !tab || batch.isEmpty()) return;
    const QString targetId = registerTarget(tab);
    if (m_multiModel->appendL2Preview(targetId, batch)) {
        m_summary->setText(QStringLiteral("正在全部打开文件中搜索 “%1”…")
                               .arg(m_multiPattern));
    }
}

void SearchResultsDock::appendMultiFileResults(EditorTab *tab,
                                               const QVector<SearchMatch> &matches,
                                               bool incomplete)
{
    if (!m_multiFileMode || !tab)
        return;
    const QString targetId = registerTarget(tab);
    if (m_multiModel) {
        m_multiModel->addL1(tab, targetId, matches, incomplete);
        m_treeView->expand(m_multiModel->index(m_multiModel->rowCount() - 1, 0));
    }
    if (incomplete)
        ++m_multiIncompleteCount;
    m_multiMatchCount += matches.size();
    m_summary->setText(m_multiIncompleteCount > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整")
              .arg(m_multiPattern).arg(m_multiMatchCount).arg(m_multiIncompleteCount)
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_multiPattern).arg(m_multiMatchCount));
}

void SearchResultsDock::appendMultiFileResults(EditorTab *tab,
                                               SearchResultsModel *model)
{
    if (!m_multiFileMode || !tab || !model)
        return;
    const QString targetId = registerTarget(tab);
    if (m_multiModel) {
        if (!m_multiModel->finishL2(targetId, model))
            m_multiModel->addL2(tab, targetId, model);
        for (int row = 0; row < m_multiModel->rowCount(); ++row) {
            const QModelIndex group = m_multiModel->index(row, 0);
            if (group.data(TargetRole).toString() == targetId) {
                m_treeView->expand(group);
                break;
            }
        }
    }
    if (model->isIncomplete())
        ++m_multiIncompleteCount;
    m_multiMatchCount += model->totalMatches();
    decorateResultsFor(tab);
    m_summary->setText(m_multiIncompleteCount > 0
        ? QStringLiteral("在全部打开文件中搜索 “%1”：已找到至少 %2 处；%3 个文件结果不完整")
              .arg(m_multiPattern).arg(m_multiMatchCount).arg(m_multiIncompleteCount)
        : QStringLiteral("在全部打开文件中搜索 “%1”：已找到 %2 处")
              .arg(m_multiPattern).arg(m_multiMatchCount));
}

void SearchResultsDock::finishMultiFileResults(bool cancelled)
{
    if (!m_multiFileMode)
        return;
    if (cancelled) {
        m_summary->setText(
            QStringLiteral("已取消在全部打开文件中搜索 “%1”；已保留 %2 处完成结果")
                .arg(m_multiPattern).arg(m_multiMatchCount));
        return;
    }
    if (m_multiStaleCount > 0 || m_multiIncompleteCount > 0) {
        m_summary->setText(
            QStringLiteral("在全部打开文件中搜索 “%1”：找到至少 %2 处；%3 个文件不完整，%4 个已失效")
                .arg(m_multiPattern).arg(m_multiMatchCount)
                .arg(m_multiIncompleteCount).arg(m_multiStaleCount));
    } else {
        m_summary->setText(QStringLiteral("在全部打开文件中搜索 “%1”：找到 %2 处匹配")
                               .arg(m_multiPattern).arg(m_multiMatchCount));
    }
}

bool SearchResultsDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_treeView->viewport()
        && (m_model || m_multiModel || m_singleL1Model)) {
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
                    int matchIdx = hitTestModelMatch(idx, xInCol, fm);
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
    if ((!m_model && !m_multiModel && !m_singleL1Model) || !index.isValid()) return;
    if (m_multiFileMode && m_multiModel) {
        MultiSearchResultsModel::Group *group = m_multiModel->groupFor(index);
        if (!group || !index.parent().isValid() || group->stale)
            return;
        EditorTab *tab = group->tab;
        if (!targetRevisionIsCurrent(tab, group->targetId)) {
            invalidateResultsFor(tab);
            return;
        }
        if (index.data(L2Role).toBool()) {
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
        } else {
            const QVariantList byteRanges =
                index.data(SearchResultsModel::MatchByteRangesRole).toList();
            int chosen = (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0)
                ? m_lastHitMatchIdx : 0;
            if (!byteRanges.isEmpty()) {
                chosen = qBound(0, chosen, byteRanges.size() - 1);
                const QVariantMap byteRange = byteRanges.at(chosen).toMap();
                emit jumpToTabByteOffset(
                    tab, byteRange.value(QStringLiteral("byteOffset")).toLongLong(),
                    byteRange.value(QStringLiteral("length")).toInt());
                return;
            }
            const QVariantList ranges =
                index.data(SearchResultsModel::MatchPositionsRole).toList();
            chosen = ranges.isEmpty() ? 0 : qBound(0, chosen, ranges.size() - 1);
            const QVariantMap range = ranges.isEmpty() ? QVariantMap{} : ranges.at(chosen).toMap();
            emit jumpToTab(tab, index.data(PositionRole).toInt(),
                           ranges.isEmpty() ? index.data(IndexRole).toInt()
                                            : range.value(QStringLiteral("start")).toInt(),
                           ranges.isEmpty() ? index.data(MatchLengthRole).toInt()
                                            : range.value(QStringLiteral("length")).toInt());
        }
        return;
    }
    if (m_singleL1Model) {
        if (m_singleTarget && !targetRevisionIsCurrent(m_singleTarget)) {
            invalidateResultsFor(m_singleTarget);
            return;
        }
        const QVariantList byteRanges =
            index.data(SearchResultsModel::MatchByteRangesRole).toList();
        int chosen = (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0)
            ? m_lastHitMatchIdx : 0;
        if (!byteRanges.isEmpty()) {
            chosen = qBound(0, chosen, byteRanges.size() - 1);
            const QVariantMap byteRange = byteRanges.at(chosen).toMap();
            if (m_singleTarget) {
                emit jumpToTabByteOffset(
                    m_singleTarget,
                    byteRange.value(QStringLiteral("byteOffset")).toLongLong(),
                    byteRange.value(QStringLiteral("length")).toInt());
            }
            return;
        }
        const int line = index.data(PositionRole).toInt();
        const QVariantList ranges =
            index.data(SearchResultsModel::MatchPositionsRole).toList();
        chosen = ranges.isEmpty() ? 0 : qBound(0, chosen, ranges.size() - 1);
        const QVariantMap range = ranges.isEmpty() ? QVariantMap{} : ranges.at(chosen).toMap();
        const int column = ranges.isEmpty() ? index.data(IndexRole).toInt()
                                            : range.value(QStringLiteral("start")).toInt();
        const int length = ranges.isEmpty() ? index.data(MatchLengthRole).toInt()
                                            : range.value(QStringLiteral("length")).toInt();
        if (m_singleTarget)
            emit jumpToTab(m_singleTarget, line, column, length);
        else
            emit jumpTo(line, column, length);
        return;
    }
    if (!m_model) return;
    if (m_singleTarget && !targetRevisionIsCurrent(m_singleTarget)) {
        invalidateResultsFor(m_singleTarget);
        return;
    }
    qint64 byteOff = -1;
    int    length  = 0;
    // 如果当前按下位置是同一个行并且有命中的匹配索引，则按该索引跳转
    if (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0) {
        if (m_model->matchByteOffsetAtIndex(index.row(), m_lastHitMatchIdx, &byteOff, &length)) {
            // 命中：跳转到用户点击的具体匹配词
            if (m_singleTarget)
                emit jumpToTabByteOffset(m_singleTarget, byteOff, length);
            else
                emit jumpToByteOffset(byteOff, length);
            return;
        }
    }
    // 回退：跳转到第一个匹配（保持兼容）
    byteOff = m_model->matchByteOffset(index);
    length  = m_model->matchLength(index);
    if (byteOff >= 0) {
        if (m_singleTarget)
            emit jumpToTabByteOffset(m_singleTarget, byteOff, length);
        else
            emit jumpToByteOffset(byteOff, length);
    }
}

#include "SearchResultsDock.moc"
