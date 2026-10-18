#pragma once

#include "SearchResultTarget.h"
#include "SearchMatch.h"
#include "SearchResultsModel.h"
#include "LineIndex.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QPointer>
#include <QVariant>
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

} // namespace

// One hierarchical model serves mixed Scintilla and mmap results.  Top-level
// rows are documents; child rows stay virtual, so a million-match mmap result
// does not become a million QTreeWidgetItem/QLabel allocations.
class MultiSearchResultsModel final : public QAbstractItemModel {
public:
    struct Group {
        SearchResultTarget target;
        QPointer<SearchResultsModel> results;
        QVector<LargeFileSearchMatch> preview;
        qint64 totalMatches = 0;
        QString incompleteReason;
        QString incompleteUserReason;
        QString state;   // 等待 / 搜索中 46% / 已跳过：已锁定; empty once settled
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
            if (role == TargetRole) return group->target.key();
            if (role == StaleRole) return group->stale;
            if (role == Qt::ToolTipRole && !group->incompleteReason.isEmpty())
                return group->incompleteUserReason;
            if (role == Qt::FontRole) {
                QFont font;
                font.setBold(true);
                return font;
            }
            if (role != Qt::DisplayRole) return {};
            if (index.column() == 0) {
                return group->target.displayName;
            }
            if (index.column() == SearchResultsModel::COL_CONTEXT)
                return group->stale
                    ? QStringLiteral("结果已失效，请重新搜索此文件")
                    : !group->state.isEmpty() ? group->state
                    : (group->incompleteReason.isEmpty()
                        ? QString() : group->incompleteUserReason);
            if (index.column() == SearchResultsModel::COL_MATCHCOUNT)
                return group->results ? QVariant(group->totalMatches) : QVariant();
            return {};
        }

        if (group->stale) return {};
        if (role == TargetRole) return group->target.key();
        if (role == StaleRole) return false;
        if (!group->results) return {};
        const QModelIndex source = group->results->index(index.row(), index.column());
        return group->results->data(source, role);
    }

    void addL1(const SearchResultTarget &target,
               const QVector<SearchMatch> &matches, bool incomplete = false)
    {
        auto *model = new SearchResultsModel(this);
        model->setTextEncoding(TextEncoding::Utf8);
        model->setResults(canonicalResults(matches), {}, 0);
        if (incomplete)
            model->setIncompleteReason(QStringLiteral("结果未完整生成"));
        addModel(target, model);
    }

    // Groups are created up front in tab order and filled as files finish,
    // so results already on screen never move.
    void reserve(const SearchResultTarget &target, const QString &state)
    {
        if (findGroup(target.key())) return;
        auto *group = new Group;
        group->target = target;
        group->state = state;
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void settleWaiting(const QString &state)
    {
        for (Group *group : std::as_const(groups_)) {
            if (!group->results && !group->state.isEmpty() && !group->state.startsWith(QStringLiteral("已跳过")))
                setState(group->target.key(), state);
        }
    }

    // A document closed before its search finished leaves nothing to show.
    bool removeUnsettled(const QString &targetId)
    {
        Group *group = findGroup(targetId);
        if (!group || group->state.isEmpty()) return false;
        const int row = groups_.indexOf(group);
        beginRemoveRows({}, row, row);
        if (group->results) group->results->deleteLater();
        delete groups_.takeAt(row);
        endRemoveRows();
        return true;
    }

    bool setState(const QString &targetId, const QString &state)
    {
        Group *group = findGroup(targetId);
        if (!group || group->stale || group->state == state) return false;
        group->state = state;
        const int row = groups_.indexOf(group);
        emit dataChanged(index(row, 0), index(row, SearchResultsModel::COL_COUNT - 1));
        return true;
    }

    void addModel(const SearchResultTarget &target,
                  SearchResultsModel *model, const QString &state = {})
    {
        if (Group *reserved = findGroup(target.key()); reserved && !reserved->results) {
            fill(reserved, model, state);
            return;
        }
        auto *group = new Group;
        group->target = target;
        group->results = model;
        group->totalMatches = model ? model->totalMatches() : 0;
        group->incompleteReason = model ? model->incompleteReason() : QString();
        group->incompleteUserReason = model ? model->incompleteUserReason() : QString();
        if (model) model->setParent(this);
        const int row = groups_.size();
        beginInsertRows({}, row, row);
        groups_.append(group);
        endInsertRows();
    }

    void addPreview(const SearchResultTarget &target,
                    SearchResultsModel *model)
    {
        addModel(target, model, QStringLiteral("搜索中…"));
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
        group->preview.clear();
        fill(group, model, {});
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
            // A file still waiting is searched later on its current text.
            if (group->target.key() != targetId || group->stale || !group->results) continue;
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
    // Swaps a group's rows with row signals, so other groups keep their
    // expansion and scroll position.
    void fill(Group *group, SearchResultsModel *model, const QString &state)
    {
        const QModelIndex groupIndex = index(groups_.indexOf(group), 0);
        const int oldCount = childCount(group);
        if (oldCount > 0) beginRemoveRows(groupIndex, 0, oldCount - 1);
        group->results = nullptr;
        if (oldCount > 0) endRemoveRows();
        if (model) model->setParent(this);
        const int newCount = model ? model->groupedCount() : 0;
        if (newCount > 0) beginInsertRows(groupIndex, 0, newCount - 1);
        group->results = model;
        group->totalMatches = model ? model->totalMatches() : 0;
        group->incompleteReason = model ? model->incompleteReason() : QString();
        group->incompleteUserReason = model ? model->incompleteUserReason() : QString();
        group->state = state;
        if (newCount > 0) endInsertRows();
        emit dataChanged(groupIndex, index(groupIndex.row(), SearchResultsModel::COL_COUNT - 1));
    }
    Group *findGroup(const QString &targetId) const
    {
        for (Group *group : groups_) {
            if (group->target.key() == targetId) return group;
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
