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
    // Unicode letter/number/underscore boundaries in every file tier.
    bool compile(const QString &pattern, bool literal, bool caseSensitive,
                 bool wholeWord, QString *error = nullptr);

    bool isValid() const;

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
