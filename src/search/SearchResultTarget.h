#pragma once

#include "DocumentState.h"

#include <QString>

// Immutable identity captured when a search starts. Search presentation never
// owns an editor widget; navigation is resolved by the application shell from
// documentId and rejected when expectedRevision is stale.
struct SearchResultTarget {
    QUuid documentId;
    quint64 expectedRevision = 0;
    QString displayName;

    bool isValid() const noexcept { return !documentId.isNull(); }
    QString key() const
    {
        return documentId.toString(QUuid::WithoutBraces);
    }
};

inline SearchResultTarget searchResultTarget(const DocumentState &state,
                                              const QString &displayName)
{
    return {state.id, state.revision, displayName};
}
