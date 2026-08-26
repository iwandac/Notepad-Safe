#pragma once

#include "DocumentState.h"

#include <optional>

// A single owner replaces the historical split between the rendered view and
// the clipboard bridge. Every user selection atomically supersedes the prior
// range, so repeated copy cannot reuse a stale first-selection cache.
class ReadOnlySelectionModel final {
public:
    void select(SourceByteRange range)
    {
        selection_ = range.isValid() ? std::optional<SourceByteRange>(range)
                                     : std::nullopt;
        ++generation_;
    }

    void clear()
    {
        selection_.reset();
        ++generation_;
    }

    const std::optional<SourceByteRange> &selection() const noexcept
    {
        return selection_;
    }

    quint64 generation() const noexcept { return generation_; }

private:
    std::optional<SourceByteRange> selection_;
    quint64 generation_ = 0;
};

