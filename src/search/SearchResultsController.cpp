#include "SearchResultsController.h"

void SearchResultsController::clear()
{
    multiFileMode_ = false;
    multiPattern_.clear();
    multiMatchCount_ = 0;
    multiStaleCount_ = 0;
    multiIncompleteCount_ = 0;
    multiTargets_.clear();
    singleTarget_ = {};
    rerunTarget_ = {};
}

void SearchResultsController::beginSingle(const SearchResultTarget &target)
{
    clear();
    singleTarget_ = target;
}

void SearchResultsController::beginMulti(const QString &pattern)
{
    clear();
    multiFileMode_ = true;
    multiPattern_ = pattern;
}

QString SearchResultsController::ensureMultiTarget(
    const SearchResultTarget &target)
{
    if (!target.isValid())
        return {};
    const QString id = target.key();
    multiTargets_.insert(id, target);
    return id;
}

QString SearchResultsController::targetId(const QUuid &documentId) const
{
    if (documentId.isNull())
        return {};
    const QString id = documentId.toString(QUuid::WithoutBraces);
    return multiTargets_.contains(id) ? id : QString{};
}

SearchResultTarget SearchResultsController::target(
    const QString &targetId) const
{
    return multiTargets_.value(targetId);
}

QVector<SearchResultTarget> SearchResultsController::targets() const
{
    QVector<SearchResultTarget> result;
    result.reserve(multiTargets_.size());
    for (const auto &target : multiTargets_)
        result.push_back(target);
    return result;
}

bool SearchResultsController::contains(const QUuid &documentId) const
{
    if (documentId.isNull())
        return false;
    if (!multiFileMode_)
        return singleTarget_.documentId == documentId;
    return multiTargets_.contains(documentId.toString(QUuid::WithoutBraces));
}

void SearchResultsController::invalidateRevision(const QString &targetId)
{
    multiTargets_.remove(targetId);
}
