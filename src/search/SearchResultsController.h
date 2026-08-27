#pragma once

#include "SearchResultTarget.h"

#include <QHash>
#include <QVector>

// Small value-state object for the lifetime and revision contract of a
// presented result set. It deliberately knows no concrete UI or editor type.
class SearchResultsController final {
public:
    void clear();
    void beginSingle(const SearchResultTarget &target);
    void beginMulti(const QString &pattern);

    bool isMultiFile() const noexcept { return multiFileMode_; }
    QString pattern() const { return multiPattern_; }
    qint64 matchCount() const noexcept { return multiMatchCount_; }
    int staleCount() const noexcept { return multiStaleCount_; }
    int incompleteCount() const noexcept { return multiIncompleteCount_; }
    void addMatches(qint64 count) noexcept { multiMatchCount_ += count; }
    void markIncomplete() noexcept { ++multiIncompleteCount_; }
    void markStale() noexcept { ++multiStaleCount_; }

    const SearchResultTarget &singleTarget() const noexcept { return singleTarget_; }
    const SearchResultTarget &rerunTarget() const noexcept { return rerunTarget_; }
    void setRerunTarget(const SearchResultTarget &target) { rerunTarget_ = target; }
    QVector<SearchResultTarget> targets() const;

    QString ensureMultiTarget(const SearchResultTarget &target);
    QString targetId(const QUuid &documentId) const;
    SearchResultTarget target(const QString &targetId) const;
    bool contains(const QUuid &documentId) const;
    void invalidateRevision(const QString &targetId);

private:
    bool multiFileMode_ = false;
    QString multiPattern_;
    qint64 multiMatchCount_ = 0;
    int multiStaleCount_ = 0;
    int multiIncompleteCount_ = 0;
    QHash<QString, SearchResultTarget> multiTargets_;
    SearchResultTarget singleTarget_;
    SearchResultTarget rerunTarget_;
};
