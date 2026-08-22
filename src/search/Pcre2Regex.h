#pragma once

#include <QtGlobal>
#include <QString>

#include <functional>
#include <memory>

class Pcre2Regex
{
public:
    enum class ScanResult {
        Complete,
        Cancelled,
        Failed
    };

    struct Limits {
        quint32 matchLimit = 10'000'000;
        quint32 depthLimit = 1'000;
        quint32 heapLimitKiB = 128 * 1024;
    };

    Pcre2Regex();
    ~Pcre2Regex();
    Pcre2Regex(Pcre2Regex &&) noexcept;
    Pcre2Regex &operator=(Pcre2Regex &&) noexcept;
    Pcre2Regex(const Pcre2Regex &) = delete;
    Pcre2Regex &operator=(const Pcre2Regex &) = delete;

    // literal=true escapes the user text before compilation. wholeWord uses
    // Unicode letter/number/underscore boundaries in text mode and ASCII
    // byte boundaries when utfMode=false (Binary literal search).
    bool compile(const QString &pattern, bool literal, bool caseSensitive,
                 bool wholeWord, QString *error = nullptr,
                 bool utfMode = true, bool matchInvalidUtf = false);

    bool isValid() const;

    // Re-match one authoritative byte range and expand replacement references.
    // Supported syntax: $0/$1.., ${name}, $$, \1, \n, \r, \t and \\.
    // The subject must be the same valid UTF-8 snapshot used to obtain the
    // range; an invalid/missing capture is an error, never literal fallback.
    bool expandReplacementAt(const uchar *subject, qint64 subjectSize,
                             qint64 matchOffset, qint64 matchLength,
                             const QString &replacement, QByteArray *expanded,
                             QString *error = nullptr) const;

    // subject must remain alive for the duration of this call. Match offsets
    // and lengths are UTF-8 byte coordinates, which map directly to mmap and
    // Scintilla positions.
    ScanResult scan(
        const uchar *subject, qint64 subjectSize,
        const std::function<bool()> &isCancelled,
        const std::function<void(qint64)> &reportProgress,
        const std::function<bool(qint64, qint64)> &onMatch,
        QString *error = nullptr,
        const Limits &limits = Limits{}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
