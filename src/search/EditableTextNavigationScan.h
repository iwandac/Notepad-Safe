#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

struct EditableTextNavigationResult {
    qint64 byteOffset = -1;
    qint64 byteLength = 0;
    QString error;
    bool ok = false;
};

// Pure directional matching over one immutable UTF-8 snapshot. No document,
// QObject, task lifecycle, or selection state is retained here.
// Both callbacks are required (the production task always supplies them).
EditableTextNavigationResult scanEditableTextNavigation(
    const QByteArray &subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, bool forward, bool wrap,
    qint64 boundary, bool currentZeroLengthHitAtEnd,
    const std::function<bool()> &cancelled,
    const std::function<void(qint64, qint64)> &reportProgress);
