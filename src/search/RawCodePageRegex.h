#pragma once

#include <QtGlobal>
#include <QString>

#include "TextEncoding.h"

#include <functional>
#include <memory>

// Executes a Perl-compatible expression over the original bytes of a text
// encoding. The iterator decodes one Unicode scalar on demand while retaining
// the raw pointer as its position, so no whole-file transcode or offset map is
// created and every match/capture coordinate is an authoritative byte offset.
class RawCodePageRegex
{
public:
    enum class ScanResult {
        Complete,
        Cancelled,
        Failed
    };

    RawCodePageRegex();
    ~RawCodePageRegex();
    RawCodePageRegex(RawCodePageRegex &&) noexcept;
    RawCodePageRegex &operator=(RawCodePageRegex &&) noexcept;
    RawCodePageRegex(const RawCodePageRegex &) = delete;
    RawCodePageRegex &operator=(const RawCodePageRegex &) = delete;

    bool compile(const QString &pattern, bool caseSensitive,
                 TextEncoding encoding,
                 QString *error = nullptr);
    bool isValid() const;

    // Re-match one authoritative raw-byte range and expand $1/${name}/\1
    // references without decoding and re-encoding captured subject bytes.
    bool expandReplacementAt(const uchar *subject, qint64 subjectSize,
                             qint64 matchOffset, qint64 matchLength,
                             const QString &replacement, QByteArray *expanded,
                             QString *error = nullptr) const;

    ScanResult scan(
        const uchar *subject, qint64 subjectSize,
        const std::function<bool()> &isCancelled,
        const std::function<void(qint64)> &reportProgress,
        const std::function<bool(qint64, qint64)> &onMatch,
        QString *error = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
