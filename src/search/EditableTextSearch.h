#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>
#include <limits>

struct EditableTextHit {
    qint64 byteOffset = -1;
    qint32 byteLength = 0;
    int utf16Offset = -1;
    int utf16Length = 0;
};

QVector<EditableTextHit> findEditableTextLiteralHits(
    const QByteArray &subject, const QString &pattern, bool caseSensitive,
    bool wholeWord, const std::function<bool()> &cancelled = {},
    const std::function<void(qint64)> &progress = {}, bool firstOnly = false,
    bool reverse = false, qint64 byteBoundary = -1,
    qsizetype maximumHits = std::numeric_limits<qsizetype>::max());
