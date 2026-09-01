#pragma once

#include "SearchMatch.h"
#include "EditableTextNavigationScan.h"

#include <QByteArray>
#include <QVector>
#include <functional>

class QObject;

struct EditableTextFindAllHit {
    qint64 byteOffset = -1;
    qint32 byteLength = 0;
};

struct EditableTextFindAllResult {
    QVector<SearchMatch> matches;
    QVector<EditableTextFindAllHit> hits;
    bool truncated = false;
    QString error;
    bool ok = false;
};

// One immutable snapshot job in the existing scheduler, with UI-thread contexts.
// Owner destruction cancels work; a surviving delivery context still receives
// one terminal callback. Delivery destruction does not cancel owner cleanup.
// No document/cache authority is retained here. A callback runs while either
// context survives and must guard access to each context independently.
qint64 startEditableTextFindAllTask(
    QByteArray subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, const QString &displayName,
    bool wipeSnapshot, QObject *ownerContext, QObject *deliveryContext,
    std::function<void(qint64, EditableTextFindAllResult)> callback);

// The same snapshot/terminal ownership contract, in the existing Highest
// priority SearchNavigation lane. No Find All generation is involved.
qint64 startEditableTextNavigationTask(
    QByteArray subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, bool forward, bool wrap,
    qint64 boundary, bool currentZeroLengthHitAtEnd, const QString &displayName,
    bool wipeSnapshot, QObject *ownerContext, QObject *deliveryContext,
    std::function<void(qint64, EditableTextNavigationResult)> callback);
