#pragma once

#include "DocumentState.h"

#include <optional>
#include <algorithm>

// A single owner replaces the historical split between the rendered view and
// the clipboard bridge. Every user selection atomically supersedes the prior
// range, so repeated copy cannot reuse a stale first-selection cache.
class ReadOnlySelectionModel final {
public:
    void select(SourceByteRange range)
    {
        if (!range.isValid()) {
            clear();
            return;
        }
        selectEndpoints(range.start, range.end);
    }

    void selectEndpoints(SourceBytePosition anchor, SourceBytePosition caret)
    {
        anchor_ = anchor;
        caret_ = caret;
        const SourceByteRange normalized{
            {std::min(anchor.value, caret.value)},
            {std::max(anchor.value, caret.value)}};
        selection_ = normalized.isValid()
            ? std::optional<SourceByteRange>(normalized) : std::nullopt;
        ++generation_;
    }

    void clear()
    {
        selection_.reset();
        anchor_ = {};
        caret_ = {};
        ++generation_;
    }

    const std::optional<SourceByteRange> &selection() const noexcept
    {
        return selection_;
    }

    quint64 generation() const noexcept { return generation_; }
    SourceBytePosition anchor() const noexcept { return anchor_; }
    SourceBytePosition caret() const noexcept { return caret_; }

private:
    std::optional<SourceByteRange> selection_;
    SourceBytePosition anchor_;
    SourceBytePosition caret_;
    quint64 generation_ = 0;
};
