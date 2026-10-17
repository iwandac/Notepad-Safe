#pragma once

#include <QString>
#include <utility>

enum class SearchScope {
    CurrentDocument,
    AllOpenDocuments,
};

struct SearchQuery {
    QString pattern;
    SearchScope scope = SearchScope::CurrentDocument;
    bool regex = false;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool hexBytes = false;
};

// Single non-visual owner for an all-target search generation.  UI controls
// project this state; they cannot maintain parallel query/cancellation counts.
class SearchSession final {
public:
    int begin(SearchQuery query)
    {
        clearQuery();
        query_ = std::move(query);
        ++generation_;
        pendingTargets_ = 0;
        skippedLockedTargets_ = 0;
        dispatchComplete_ = false;
        cancelRequested_ = false;
        return generation_;
    }

    void reset()
    {
        clearQuery();
        ++generation_;
        pendingTargets_ = 0;
        skippedLockedTargets_ = 0;
        dispatchComplete_ = true;
        cancelRequested_ = false;
    }

    const SearchQuery &query() const noexcept { return query_; }
    int generation() const noexcept { return generation_; }
    bool isCurrent(int generation) const noexcept
    {
        return generation == generation_;
    }
    void requestCancel() noexcept { cancelRequested_ = true; }
    bool cancelRequested() const noexcept { return cancelRequested_; }
    void clearCancelRequest() noexcept { cancelRequested_ = false; }
    void setDispatchComplete(bool complete) noexcept
    {
        dispatchComplete_ = complete;
    }
    bool dispatchComplete() const noexcept { return dispatchComplete_; }
    void scheduleTarget() noexcept { ++pendingTargets_; }
    void settleTarget() noexcept
    {
        if (pendingTargets_ > 0)
            --pendingTargets_;
    }
    int pendingTargets() const noexcept { return pendingTargets_; }
    void skipLockedTarget() noexcept { ++skippedLockedTargets_; }
    int skippedLockedTargets() const noexcept { return skippedLockedTargets_; }
    void clearQuery()
    {
        if (!query_.pattern.isEmpty())
            query_.pattern.fill(QChar(0));
        query_ = {};
    }

private:
    SearchQuery query_;
    int generation_ = 0;
    int pendingTargets_ = 0;
    int skippedLockedTargets_ = 0;
    bool dispatchComplete_ = true;
    bool cancelRequested_ = false;
};
