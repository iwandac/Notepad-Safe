#pragma once

#include "EditorTab.h"

#include <QHash>
#include <QPointer>
#include <QVector>

// Non-visual ownership for the currently presented result set. The Dock may
// render this session but no longer declares targets or revisions as view state.
struct SearchResultSession final {
    bool multiFileMode = false;
    QString multiPattern;
    qint64 multiMatchCount = 0;
    int multiStaleCount = 0;
    int multiIncompleteCount = 0;
    QHash<QString, QPointer<EditorTab>> multiTargets;
    QHash<QString, quint64> multiTargetRevisions;
    QPointer<EditorTab> singleTarget;
    QPointer<EditorTab> rerunTarget;
    quint64 singleTargetRevision = 0;
    QVector<QMetaObject::Connection> targetConnections;
};

