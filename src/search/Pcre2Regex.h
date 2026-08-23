#pragma once

#include <QtGlobal>
#include <QByteArray>
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

    struct StartByteFilter {
        QByteArray asciiBytes;
        bool anyNonAscii = false;

        bool isEmpty() const { return asciiBytes.isEmpty() && !anyNonAscii; }
    };

    Pcre2Regex();
    ~Pcre2Regex();
    Pcre2Regex(Pcre2Regex &&) noexcept;
    Pcre2Regex &operator=(Pcre2Regex &&) noexcept;
    Pcre2Regex(const Pcre2Regex &) = delete;
    Pcre2Regex &operator=(const Pcre2Regex &) = delete;

    // literal=true escapes the user text before compilation. wholeWord applies
    // Unicode/ASCII word boundaries to space-delimited scripts; literal CJK
    // edges use exact-phrase semantics because those scripts do not require
    // spaces between words.
    bool compile(const QString &pattern, bool literal, bool caseSensitive,
                 bool wholeWord, QString *error = nullptr,
                 bool utfMode = true, bool matchInvalidUtf = false);

    // Compile an application-generated PCRE2 8-bit expression. The caller is
    // responsible for encoding semantics and character-boundary guarantees.
    bool compileBytePattern(const QByteArray &pattern, bool caseSensitive,
                            QString *error = nullptr);

    bool isValid() const;

    // PCRE2's conservative first-code-unit set. In UTF mode anyNonAscii is
    // retained because Unicode caseless equivalents may use multibyte UTF-8.
    StartByteFilter startByteFilter() const;

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
        const Limits &limits = Limits{},
        const std::function<qint64(qint64)> &advanceAfterEmpty = {},
        qint64 startOffset = 0,
        const std::function<qint64(qint64)> &normalizeMatchStart = {},
        const std::function<qint64(qint64)> &nextCandidateStart = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
