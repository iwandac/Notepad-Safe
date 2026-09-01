#pragma once

#include "OrdinaryReadSource.h"
#include "TextEncoding.h"
#include <QByteArrayView>
#include <QVector>
#include <atomic>
#include <functional>

namespace ReadOnlyTextAnalysis {

inline constexpr qint64 PrefixChunkBytes = 1LL * 1024 * 1024;

qint64 countPhysicalLineBreaks(QByteArrayView bytes, TextEncoding encoding,
                              bool previousWasCr, bool *endsWithCr);

struct Result {
    enum class Status { Completed, Canceled, Failed };
    Status status = Status::Failed;
    OrdinaryReadResult source;
    QVector<qint64> lineBreakPrefix;
    QVector<quint8> lineBreakPrefixEndsWithCr;
    qint64 characterCount = -1;
};

// Pure worker computation: no owner, task scheduling, generation or UI state.
// Each bounded chunk is admitted using the same handle that reads its bytes.
Result analyze(const QString &path, const PersistenceReadPolicy &policy,
               const DiskFileStamp &expected, TextEncoding encoding,
               const std::atomic_bool *cancel,
               const std::function<void(qint64, qint64)> &progress);

} // namespace ReadOnlyTextAnalysis
