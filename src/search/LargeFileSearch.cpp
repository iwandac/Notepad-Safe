#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"
#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"

#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QDateTime>
#include <QUuid>
#include <QPointer>
#include <QThread>
#include <cstring>
#include <algorithm>
#include <utility>

namespace {

constexpr quint32 OVR_MAGIC   = 0x564F534Du; // 'MSOV' little endian stored
constexpr quint32 OVR_VERSION = 2;
constexpr int     OVERRUN_CTX_MAX = 512;
constexpr qint64  OVERRUN_FILE_LIMIT = Q_INT64_C(1024) * 1024 * 1024;

QString overrunDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe");
    QDir().mkpath(d);
    return d;
}

// 大小写转换（只对 ASCII 做；UTF-8 不处理以保持复杂度可控）
inline uchar foldCaseAscii(uchar c, bool caseSensitive) {
    if (caseSensitive) return c;
    if (c >= 'A' && c <= 'Z') return uchar(c + ('a' - 'A'));
    return c;
}

int hexNibble(QChar ch)
{
    if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
        return ch.unicode() - QLatin1Char('0').unicode();
    if (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
        return 10 + ch.unicode() - QLatin1Char('a').unicode();
    if (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))
        return 10 + ch.unicode() - QLatin1Char('A').unicode();
    return -1;
}

QString escapeRawRegexLiteral(const QString &input)
{
    static const QString metacharacters = QStringLiteral("\\.^$|()[]{}*+?");
    QString escaped;
    escaped.reserve(input.size() * 2);
    for (const QChar ch : input) {
        if (metacharacters.contains(ch))
            escaped.append(QLatin1Char('\\'));
        escaped.append(ch);
    }
    return escaped;
}

uint edgeCodePoint(const QString &text, bool first)
{
    if (text.isEmpty()) return 0;
    const int index = first ? 0 : text.size() - 1;
    const QChar edge = text.at(index);
    if (first && edge.isHighSurrogate() && text.size() > 1
        && text.at(1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(edge, text.at(1));
    }
    if (!first && edge.isLowSurrogate() && text.size() > 1
        && text.at(index - 1).isHighSurrogate()) {
        return QChar::surrogateToUcs4(text.at(index - 1), edge);
    }
    return edge.unicode();
}

bool needsRawWordBoundary(uint codePoint)
{
    switch (QChar::script(codePoint)) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
    case QChar::Script_Hangul:
    case QChar::Script_Bopomofo:
        return false;
    default:
        return codePoint == uint('_') || QChar::isLetterOrNumber(codePoint);
    }
}

QString rawSearchExpression(const QString &pattern,
                            const LargeFileSearch::Options &opts)
{
    QString expression = opts.regexMode ? pattern : escapeRawRegexLiteral(pattern);
    if (opts.wholeWord) {
        const bool leftBoundary = opts.regexMode
            || needsRawWordBoundary(edgeCodePoint(pattern, true));
        const bool rightBoundary = opts.regexMode
            || needsRawWordBoundary(edgeCodePoint(pattern, false));
        expression = (leftBoundary ? QStringLiteral("(?<!\\w)") : QString())
            + QStringLiteral("(?:") + expression + QStringLiteral(")")
            + (rightBoundary ? QStringLiteral("(?!\\w)") : QString());
    }
    return expression;
}

std::function<qint64(qint64)> makeEncodedBoundaryNormalizer(
    const uchar *subject, qint64 subjectSize, TextEncoding encoding,
    qint64 initialBoundary = 0)
{
    if (encoding == TextEncoding::Windows1252)
        return [](qint64 candidate) { return candidate; };

    auto cursor = std::make_shared<qint64>(initialBoundary);
    return [subject, subjectSize, encoding, cursor](qint64 candidate) {
        while (*cursor < candidate && *cursor < subjectSize) {
            // All supported DBCS encodings encode ASCII as one byte. Skip
            // long ASCII log runs eight bytes at a time before falling back
            // to the authoritative decoder for multibyte units.
            if (encoding != TextEncoding::Utf16LE
                && encoding != TextEncoding::Utf16BE) {
                constexpr quint64 HighBits = Q_UINT64_C(0x8080808080808080);
                while (*cursor + qint64(sizeof(quint64)) <= candidate) {
                    quint64 word = 0;
                    std::memcpy(&word, subject + *cursor, sizeof(word));
                    if (word & HighBits)
                        break;
                    *cursor += sizeof(word);
                }
                if (*cursor >= candidate)
                    break;
            }
            const qint64 next = EncodedRegexCompiler::nextUnitOffset(
                subject, subjectSize, *cursor, encoding);
            if (next <= *cursor)
                return subjectSize + 1;
            *cursor = next;
        }
        return *cursor;
    };
}

std::function<qint64(qint64)> makeByteCandidateFinder(
    const uchar *subject, qint64 subjectSize, const QByteArray &needle,
    bool caseSensitive, qint64 initialOffset,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<qint64(qint64)> &normalizeStart,
    qint64 maxCandidateStartExclusive = (std::numeric_limits<qint64>::max)())
{
    if (needle.isEmpty())
        return {};
    int anchorIndex = 0;
    for (int index = 0; index < needle.size(); ++index) {
        if (uchar(needle.at(index)) != 0) {
            anchorIndex = index;
            break;
        }
    }
    const uchar anchor = uchar(needle.at(anchorIndex));
    const uchar foldedAnchor = foldCaseAscii(anchor, false);
    const uchar alternateAnchor = foldedAnchor >= 'a' && foldedAnchor <= 'z'
        ? uchar(foldedAnchor - ('a' - 'A')) : foldedAnchor;
    struct State {
        qint64 searchOffset = 0;
        qint64 lastReported = 0;
    };
    auto state = std::make_shared<State>();
    state->searchOffset = initialOffset + anchorIndex;
    state->lastReported = initialOffset;
    const qint64 candidateLimit = maxCandidateStartExclusive
            == (std::numeric_limits<qint64>::max)()
        ? subjectSize
        : qBound<qint64>(0, maxCandidateStartExclusive, subjectSize);
    const qint64 anchorSearchLimit = candidateLimit >= subjectSize - anchorIndex
        ? subjectSize : candidateLimit + anchorIndex;
    return [subject, subjectSize, needle, caseSensitive, anchorIndex,
            anchor, foldedAnchor, alternateAnchor, state, isCancelled,
            reportProgress, normalizeStart, candidateLimit,
            anchorSearchLimit](qint64 minimumOffset) {
        if (minimumOffset >= candidateLimit)
            return subjectSize + 1;
        state->searchOffset = qMax(
            state->searchOffset, minimumOffset + qint64(anchorIndex));
        constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
        constexpr qint64 ProgressStep = Q_INT64_C(1) * 1024 * 1024;
        while (state->searchOffset < anchorSearchLimit) {
            if (isCancelled && isCancelled())
                return qint64(-1);
            const qint64 chunkEnd = qMin(anchorSearchLimit,
                                         state->searchOffset + Chunk);
            const size_t available = size_t(chunkEnd - state->searchOffset);
            const uchar *hit = nullptr;
            if (!caseSensitive && alternateAnchor != foldedAnchor) {
                // A pair of independent memchr calls is only linear when both
                // cases are sparse.  With dense single-case input, the absent
                // alternate used to rescan the complete remaining chunk before
                // every nearby hit.  Visit this bounded chunk once and accept
                // either ASCII spelling so the candidate cursor stays monotonic.
                const uchar *cursor = subject + state->searchOffset;
                const uchar *end = subject + chunkEnd;
                while (cursor < end) {
                    if (*cursor == foldedAnchor || *cursor == alternateAnchor) {
                        hit = cursor;
                        break;
                    }
                    ++cursor;
                }
            } else {
                hit = static_cast<const uchar *>(std::memchr(
                    subject + state->searchOffset, anchor, available));
            }
            if (!hit) {
                state->searchOffset = chunkEnd;
                const qint64 examined = qMin(
                    candidateLimit,
                    qMax(minimumOffset, state->searchOffset - anchorIndex));
                if (reportProgress && examined > state->lastReported) {
                    state->lastReported = examined;
                    reportProgress(examined);
                }
                continue;
            }
            const qint64 hitOffset = hit - subject;
            state->searchOffset = hitOffset + 1;
            const qint64 candidate = hitOffset - anchorIndex;
            if (reportProgress
                && candidate >= state->lastReported + ProgressStep) {
                state->lastReported = qMin(candidate, candidateLimit);
                reportProgress(state->lastReported);
            }
            if (candidate < minimumOffset || candidate < 0
                || candidate >= candidateLimit
                || candidate > subjectSize - needle.size())
                continue;
            bool same = true;
            for (int index = 0; index < needle.size(); ++index) {
                if (foldCaseAscii(subject[candidate + index], caseSensitive)
                    != foldCaseAscii(uchar(needle.at(index)), caseSensitive)) {
                    same = false;
                    break;
                }
            }
            if (!same)
                continue;
            const qint64 normalized = normalizeStart
                ? normalizeStart(candidate) : candidate;
            if (normalized == candidate)
                return candidate;
        }
        return subjectSize + 1;
    };
}

qint64 findPreviousByteLiteral(
    const uchar *subject, qint64 subjectSize, const QByteArray &needle,
    bool caseSensitive, qint64 startInclusive, qint64 endExclusive,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress)
{
    if (!subject || needle.isEmpty() || endExclusive <= startInclusive)
        return subjectSize + 1;
    const qint64 last = qMin(endExclusive - qint64(needle.size()),
                             subjectSize - qint64(needle.size()));
    if (last < startInclusive)
        return subjectSize + 1;

    constexpr qint64 CancelStep = Q_INT64_C(256) * 1024;
    constexpr qint64 ProgressStep = Q_INT64_C(1) * 1024 * 1024;
    qint64 examined = 0;
    qint64 nextCancel = CancelStep;
    qint64 nextProgress = ProgressStep;
    const uchar tail = foldCaseAscii(uchar(needle.back()), caseSensitive);
    for (qint64 candidate = last; candidate >= startInclusive; --candidate) {
        ++examined;
        if (examined >= nextCancel) {
            nextCancel += CancelStep;
            if (isCancelled && isCancelled())
                return -1;
        }
        if (examined >= nextProgress) {
            nextProgress += ProgressStep;
            if (reportProgress)
                reportProgress(examined);
        }
        if (foldCaseAscii(subject[candidate + needle.size() - 1],
                          caseSensitive) != tail) {
            continue;
        }
        bool same = true;
        for (qsizetype index = 0; index < needle.size(); ++index) {
            if (foldCaseAscii(subject[candidate + index], caseSensitive)
                != foldCaseAscii(uchar(needle.at(index)), caseSensitive)) {
                same = false;
                break;
            }
        }
        if (same)
            return candidate;
    }
    if (reportProgress)
        reportProgress(last - startInclusive + 1);
    return subjectSize + 1;
}

bool unicodeWordCharacter(const QString &text, int index)
{
    if (index < 0 || index >= text.size())
        return false;
    uint scalar = text.at(index).unicode();
    if (text.at(index).isLowSurrogate() && index > 0
        && text.at(index - 1).isHighSurrogate()) {
        scalar = QChar::surrogateToUcs4(text.at(index - 1), text.at(index));
    } else if (text.at(index).isHighSurrogate() && index + 1 < text.size()
               && text.at(index + 1).isLowSurrogate()) {
        scalar = QChar::surrogateToUcs4(text.at(index), text.at(index + 1));
    }
    return scalar == uint('_') || QChar::isLetterOrNumber(scalar);
}

bool literalWholeWordAccepted(const QString &text, int start, int length,
                              const QString &pattern)
{
    if (pattern.isEmpty())
        return false;
    const bool requireLeft = needsRawWordBoundary(edgeCodePoint(pattern, true));
    const bool requireRight = needsRawWordBoundary(edgeCodePoint(pattern, false));
    return (!requireLeft || start == 0 || !unicodeWordCharacter(text, start - 1))
        && (!requireRight || start + length >= text.size()
            || !unicodeWordCharacter(text, start + length));
}

QVector<qint32> utf16ToSourceBoundaries(const DecodedTextProjection &projection)
{
    QVector<qint32> mapping(projection.text.size() + 1, 0);
    int utf16 = 0;
    const int scalarCount = projection.sourceBoundaries.size() - 1;
    for (int scalar = 0; scalar < scalarCount && utf16 < projection.text.size(); ++scalar) {
        const int units = projection.text.at(utf16).isHighSurrogate()
                && utf16 + 1 < projection.text.size()
                && projection.text.at(utf16 + 1).isLowSurrogate()
            ? 2 : 1;
        mapping[utf16] = projection.sourceBoundaries.at(scalar);
        if (units == 2)
            mapping[utf16 + 1] = projection.sourceBoundaries.at(scalar);
        utf16 += units;
        mapping[utf16] = projection.sourceBoundaries.at(scalar + 1);
    }
    while (utf16 < mapping.size()) {
        mapping[utf16] = projection.sourceBoundaries.isEmpty()
            ? 0 : projection.sourceBoundaries.back();
        ++utf16;
    }
    return mapping;
}

struct ProjectedLiteralScanResult {
    bool ok = true;
    bool stopped = false;
};

ProjectedLiteralScanResult scanProjectedLiteralRange(
    qint64 virtualSize, qint64 rangeStart, qint64 rangeEnd,
    const QString &pattern, const LargeFileSearch::Options &opts, bool reverse,
    const std::function<QByteArray(qint64, qint64)> &readBytes,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(qint64, qint32)> &onMatch,
    QString *error)
{
    ProjectedLiteralScanResult result;
    if (pattern.isEmpty() || rangeEnd <= rangeStart)
        return result;
    constexpr qint64 CoreBytes = Q_INT64_C(2) * 1024 * 1024;
    constexpr qint64 PrefixBytes = 16;
    const qint64 overlap = qBound<qint64>(
        64, qint64(pattern.toUtf8().size()) * 4 + 16,
        Q_INT64_C(8) * 1024 * 1024);
    qint64 scanned = 0;
    qint64 cursor = reverse ? rangeEnd : rangeStart;
    const Qt::CaseSensitivity sensitivity = opts.caseSensitive
        ? Qt::CaseSensitive : Qt::CaseInsensitive;

    while (reverse ? cursor > rangeStart : cursor < rangeEnd) {
        if (isCancelled && isCancelled()) {
            result.ok = false;
            return result;
        }
        const qint64 coreStart = reverse
            ? qMax(rangeStart, cursor - CoreBytes) : cursor;
        const qint64 coreEnd = reverse
            ? cursor : qMin(rangeEnd, cursor + CoreBytes);
        const qint64 windowStart = qMax<qint64>(0, coreStart - PrefixBytes);
        const qint64 windowEnd = qMin(virtualSize, coreEnd + overlap);
        const QByteArray bytes = readBytes(windowStart, windowEnd);
        if (bytes.size() != windowEnd - windowStart) {
            result.ok = false;
            if (error)
                *error = QObject::tr("无法读取文本搜索窗口。");
            return result;
        }
        const DecodedTextProjection projection = decodeTextProjection(
            bytes, opts.textEncoding, windowStart == 0, false);
        const QVector<qint32> sourceAt = utf16ToSourceBoundaries(projection);
        int hit = reverse
            ? projection.text.lastIndexOf(pattern, -1, sensitivity)
            : projection.text.indexOf(pattern, 0, sensitivity);
        while (hit >= 0) {
            const int endUtf16 = qMin(projection.text.size(), hit + pattern.size());
            const qint64 sourceStart = windowStart + sourceAt.value(hit);
            const qint64 sourceEnd = windowStart + sourceAt.value(endUtf16);
            if (sourceStart >= coreStart && sourceStart < coreEnd
                && sourceStart >= rangeStart && sourceStart < rangeEnd
                && (!opts.wholeWord
                    || literalWholeWordAccepted(projection.text, hit,
                                                endUtf16 - hit, pattern))) {
                const qint64 length = qMax<qint64>(1, sourceEnd - sourceStart);
                if (length > std::numeric_limits<qint32>::max()) {
                    result.ok = false;
                    if (error) *error = QObject::tr("单个匹配过长。");
                    return result;
                }
                if (onMatch && !onMatch(sourceStart, qint32(length))) {
                    result.stopped = true;
                    return result;
                }
            }
            hit = reverse
                ? projection.text.lastIndexOf(pattern, hit - 1, sensitivity)
                : projection.text.indexOf(pattern, hit + 1, sensitivity);
        }
        scanned += coreEnd - coreStart;
        if (reportProgress)
            reportProgress(scanned);
        cursor = reverse ? coreStart : coreEnd;
    }
    return result;
}

std::function<qint64(qint64)> makeStartByteCandidateFinder(
    const uchar *subject, qint64 subjectSize,
    const Pcre2Regex::StartByteFilter &filter, qint64 initialOffset,
    bool validUtf8, const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    qint64 maxCandidateStartExclusive = (std::numeric_limits<qint64>::max)())
{
    if (filter.isEmpty())
        return {};
    struct State { qint64 offset = 0; qint64 lastReported = 0; };
    auto state = std::make_shared<State>();
    state->offset = initialOffset;
    state->lastReported = initialOffset;
    const qint64 candidateLimit = maxCandidateStartExclusive
            == (std::numeric_limits<qint64>::max)()
        ? subjectSize
        : qBound<qint64>(0, maxCandidateStartExclusive, subjectSize);
    return [subject, subjectSize, filter, validUtf8, state, isCancelled,
            reportProgress, candidateLimit](qint64 minimumOffset) {
        if (minimumOffset >= candidateLimit)
            return subjectSize + 1;
        state->offset = qMax(state->offset, minimumOffset);
        if (validUtf8) {
            while (state->offset < subjectSize
                   && (subject[state->offset] & 0xc0u) == 0x80u) {
                ++state->offset;
            }
        }
        constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
        constexpr quint64 HighBits = Q_UINT64_C(0x8080808080808080);
        constexpr qint64 ProgressStep = Q_INT64_C(1) * 1024 * 1024;
        while (state->offset < candidateLimit) {
            if (isCancelled && isCancelled())
                return qint64(-1);
            const qint64 end = qMin(candidateLimit, state->offset + Chunk);
            const uchar *best = nullptr;
            const size_t available = size_t(end - state->offset);
            for (char candidate : filter.asciiBytes) {
                const auto *hit = static_cast<const uchar *>(std::memchr(
                    subject + state->offset, uchar(candidate), available));
                if (hit && (!best || hit < best))
                    best = hit;
            }
            if (filter.anyNonAscii) {
                qint64 cursor = state->offset;
                const qint64 highLimit = best ? qint64(best - subject) : end;
                while (cursor + qint64(sizeof(quint64)) <= highLimit) {
                    quint64 word = 0;
                    std::memcpy(&word, subject + cursor, sizeof(word));
                    if (word & HighBits)
                        break;
                    cursor += sizeof(word);
                }
                while (cursor < highLimit && subject[cursor] < 0x80u)
                    ++cursor;
                if (cursor < highLimit)
                    best = subject + cursor;
            }
            if (best) {
                const qint64 result = best - subject;
                state->offset = result + 1;
                if (reportProgress
                    && result >= state->lastReported + ProgressStep) {
                    state->lastReported = result;
                    reportProgress(result);
                }
                return result;
            }
            state->offset = end;
            if (reportProgress && end > state->lastReported) {
                state->lastReported = end;
                reportProgress(end);
            }
        }
        return subjectSize + 1;
    };
}

QByteArray safeInvalidUtfAsciiLiteralFilter(
    const QString &pattern, const LargeFileSearch::Options &opts,
    bool invalidUtfText)
{
    if (!invalidUtfText || pattern.isEmpty())
        return {};
    static const QString regexMetacharacters =
        QStringLiteral("\\.^$|()[]{}*+?");
    QByteArray bytes;
    bytes.reserve(pattern.size());
    for (const QChar ch : pattern) {
        if (ch.unicode() > 0x7f)
            return {};
        if (opts.regexMode && regexMetacharacters.contains(ch))
            return {};
        // Unicode caseless matching gives ASCII K/S non-ASCII equivalents
        // (Kelvin sign and long s). Do not use an ASCII-only prefilter when it
        // could suppress those authoritative PCRE2 matches.
        if (!opts.caseSensitive
            && (ch == QLatin1Char('k') || ch == QLatin1Char('K')
                || ch == QLatin1Char('s') || ch == QLatin1Char('S'))) {
            return {};
        }
        bytes.append(char(ch.unicode()));
    }
    return bytes;
}

// A UTF PCRE2 scan has two real pieces of work: validating the mapped bytes
// before PCRE2_NO_UTF_CHECK is safe, then matching.  Both phases use source
// byte offsets, so forwarding them directly makes the task jump to 100% after
// validation and then move backwards when matching starts.  Keep one
// monotonic user-visible scale and reserve a bounded share for validation.
class RegexProgressPhases final
{
public:
    RegexProgressPhases(qint64 total, bool validatesUtf8,
                        std::function<void(qint64)> report,
                        std::function<void(const QString &, int, int,
                                           qint64, qint64)> reportPhase = {})
        : total_(qMax<qint64>(0, total)),
          validationBudget_(validatesUtf8 ? total_ / 5 : 0),
          report_(std::move(report)), reportPhase_(std::move(reportPhase))
    {
    }

    std::function<void(qint64)> validationReporter()
    {
        if (validationBudget_ <= 0 || !report_)
            return {};
        return [this](qint64 done) {
            if (reportPhase_)
                reportPhase_(QObject::tr("验证文本编码"), 1, 2, done, total_);
            emitMonotonic(scale(done, 0, validationBudget_));
        };
    }

    std::function<void(qint64)> searchReporter()
    {
        if (!report_)
            return {};
        return [this](qint64 done) {
            if (reportPhase_) {
                const int count = validationBudget_ > 0 ? 2 : 1;
                reportPhase_(QObject::tr("执行正则匹配"), count, count,
                             done, total_);
            }
            emitMonotonic(scale(done, validationBudget_,
                                total_ - validationBudget_));
        };
    }

private:
    qint64 scale(qint64 done, qint64 base, qint64 span) const
    {
        if (total_ <= 0)
            return 0;
        const qint64 clamped = qBound<qint64>(0, done, total_);
        const long double fraction = static_cast<long double>(clamped)
            / static_cast<long double>(total_);
        return qBound<qint64>(0, base + qint64(fraction * span), total_);
    }

    void emitMonotonic(qint64 done)
    {
        if (done <= lastReported_ && lastReported_ != 0)
            return;
        lastReported_ = done;
        report_(done);
    }

    qint64 total_ = 0;
    qint64 validationBudget_ = 0;
    qint64 lastReported_ = 0;
    std::function<void(qint64)> report_;
    std::function<void(const QString &, int, int, qint64, qint64)> reportPhase_;
};


bool usesProjectedLiteral(const QString &pattern,
                          const LargeFileSearch::Options &opts,
                          const QByteArray &literal, bool usePcre2)
{
    if (usePcre2 || opts.hexBytesMode || opts.byteContextMode)
        return false;
    const bool asciiPattern = std::all_of(
        pattern.cbegin(), pattern.cend(),
        [](QChar ch) { return ch.unicode() <= 0x7f; });
    if (literal.isEmpty() || pattern.contains(QChar::ReplacementCharacter)) {
        return true;
    }
    // Binary Text can apply an ASCII word boundary directly to source bytes;
    // this is both meaningful for its replacement-character projection and
    // much cheaper than decoding a PE/log image solely to reject "catalog".
    if (opts.wholeWord && !(opts.tolerateInvalidEncoding && asciiPattern))
        return true;
    if (!opts.caseSensitive) {
        return std::any_of(pattern.cbegin(), pattern.cend(),
                           [](QChar ch) { return ch.unicode() > 0x7f; });
    }
    return false;
}

bool requiresMultibyteCharacterBoundaries(
    const LargeFileSearch::Options &opts)
{
    return !opts.byteContextMode
        && opts.textEncoding != TextEncoding::Windows1252
        && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
}

bool asciiWholeWordAt(const std::function<QByteArray(qint64, qint64)> &read,
                      qint64 virtualSize, qint64 offset, qint64 length)
{
    const auto isWord = [](uchar value) {
        return value == '_' || (value >= '0' && value <= '9')
            || (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
    };
    if (offset > 0) {
        const QByteArray before = read(offset - 1, offset);
        if (!before.isEmpty() && isWord(uchar(before.at(0))))
            return false;
    }
    if (offset + length < virtualSize) {
        const QByteArray after = read(offset + length, offset + length + 1);
        if (!after.isEmpty() && isWord(uchar(after.at(0))))
            return false;
    }
    return true;
}

bool readOverrunHeader(QDataStream &stream, quint32 *version, quint64 *count)
{
    quint32 magic = 0;
    quint32 parsedVersion = 0;
    quint64 parsedCount = 0;
    stream >> magic >> parsedVersion >> parsedCount;
    if (stream.status() != QDataStream::Ok || magic != OVR_MAGIC
        || (parsedVersion != 1 && parsedVersion != OVR_VERSION)) {
        return false;
    }
    if (version) *version = parsedVersion;
    if (count) *count = parsedCount;
    return true;
}

bool readOverrunRecord(QDataStream &stream, quint32 version,
                       LargeFileSearchMatch *match)
{
    if (!match)
        return false;
    quint64 byteOffset = 0;
    qint32 length = 0;
    qint32 line = 0;
    qint32 col = 0;
    quint64 contextByteOffset = 0;
    QByteArray context;
    stream >> byteOffset >> length >> line >> col;
    if (version >= 2)
        stream >> contextByteOffset;
    stream >> context;
    if (stream.status() != QDataStream::Ok)
        return false;
    match->byteOffset = qint64(byteOffset);
    match->length = length;
    match->line = line;
    match->col = col;
    match->contextByteOffset = version >= 2
        ? qint64(contextByteOffset)
        : qMax<qint64>(0, qint64(byteOffset) - col);
    match->lineContext = std::move(context);
    return true;
}

} // namespace

bool LargeFileSearch::preparePattern(const QString &pattern, const LargeFileSearch::Options &opts,
                          QByteArray *literal, bool *usePcre2, QString *error)
{
    if (opts.hexBytesMode) {
        *usePcre2 = false;
        return LargeFileSearch::parseHexBytePattern(pattern, literal, error);
    }
    // PCRE2 is an explicitly selected, high-cost route. Literal case folding,
    // whole-word checks and malformed-byte display projection have their own
    // linear matcher and must never reach the regex engine implicitly.
    *usePcre2 = opts.regexMode;
    if (!*usePcre2) {
        bool representable = true;
        *literal = encodeTextFragment(pattern, opts.textEncoding, &representable);
        if (!representable)
            literal->clear(); // decoded-projection search remains meaningful
    }
    if (pattern.isEmpty()) {
        if (error)
            *error = QStringLiteral("查找文本为空。");
        return false;
    }
    return true;
}

bool LargeFileSearch::parseHexBytePattern(const QString &pattern,
                                          QByteArray *bytes,
                                          QString *error)
{
    if (!bytes)
        return false;
    bytes->clear();
    QString digits;
    digits.reserve(pattern.size());
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar ch = pattern.at(i);
        if (ch.isSpace() || ch == QLatin1Char('_') || ch == QLatin1Char('-')
            || ch == QLatin1Char(',')) {
            continue;
        }
        if (ch == QLatin1Char('0') && i + 1 < pattern.size()
            && (pattern.at(i + 1) == QLatin1Char('x')
                || pattern.at(i + 1) == QLatin1Char('X'))) {
            ++i;
            continue;
        }
        if (hexNibble(ch) < 0) {
            if (error) {
                *error = QObject::tr("十六进制字节串第 %1 个字符“%2”无效。")
                             .arg(i + 1).arg(ch);
            }
            return false;
        }
        digits.append(ch);
        if (digits.size() > 2 * 1024 * 1024) {
            if (error)
                *error = QObject::tr("十六进制搜索模式过长（最多 1 MiB 字节）。");
            return false;
        }
    }
    if (digits.isEmpty()) {
        if (error) *error = QObject::tr("请输入至少一个十六进制字节。");
        return false;
    }
    if ((digits.size() % 2) != 0) {
        if (error)
            *error = QObject::tr("十六进制字节必须由成对数字组成，例如 DE AD BE EF。");
        return false;
    }
    bytes->reserve(digits.size() / 2);
    for (qsizetype i = 0; i < digits.size(); i += 2) {
        bytes->append(char((hexNibble(digits.at(i)) << 4)
                           | hexNibble(digits.at(i + 1))));
    }
    return true;
}

// ============================================================================
//  构造 / 析构 / 数据源
// ============================================================================
LargeFileSearch::LargeFileSearch(QObject* parent)
    : QObject(parent)
    , overrunFp_(nullptr)
{
    maxMemMatches_ = DynamicThresholds::instance().MAX_MATCHES_MEM;
}

LargeFileSearch::~LargeFileSearch() {
    stopWorkers(true);
    closeOverrunFile();
    if (!overrunFile_.isEmpty()) QFile::remove(overrunFile_);
}

void LargeFileSearch::setSource(const uchar* mmapBase, qint64 mmapSize,
                                const LineIndex* lineIndex, const PatchList* patchList)
{
    // All four source objects are borrowed. Never replace them while a queued
    // or running search can still dereference the previous snapshot.
    const QPointer<LargeFileSearch> owner(this);
    stopWorkers(false);
    if (!owner)
        return;
    mmapBase_ = mmapBase;
    mmapSize_ = mmapSize;
    lineIndex_ = lineIndex;
    lineIndexCompleteAtSnapshot_ = lineIndex && !lineIndex->isIncomplete();
    patchList_ = patchList;
    memMatches_.clear();
    storageFailed_ = false;
    scanError_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunCheckpointBytes_.clear();
    overrunGroupedCount_ = 0;
    overrunLastLine_ = std::numeric_limits<qint32>::min();
    overrunLastContext_ = std::numeric_limits<qint64>::min();
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
}

void LargeFileSearch::requestCancel() {
    cancel_.store(true, std::memory_order_relaxed);
}


// ============================================================================
//  溢出文件操作
// ============================================================================
bool LargeFileSearch::prepareOverrunFile(QString* errMsg) {
    if (overrunFp_) return true;
    QString fn = QStringLiteral("matches_%1_%2_%3.bin")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    overrunFile_ = QDir(overrunDir()).filePath(fn);
    auto* f = new QFile(overrunFile_);
    if (!f->open(QIODevice::WriteOnly)) {
        if (errMsg) *errMsg = QStringLiteral("无法创建溢出文件：%1").arg(f->errorString());
        delete f;
        overrunFile_.clear();
        return false;
    }
    overrunFp_ = f;
    QDataStream s(overrunFp_);
    s.setByteOrder(QDataStream::LittleEndian);
    s << OVR_MAGIC << OVR_VERSION << quint64(0); // count 占位，收尾时回填
    return true;
}

void LargeFileSearch::closeOverrunFile() {
    if (overrunFp_) {
        // 回填 count
        overrunFp_->seek(8);
        QDataStream s(overrunFp_);
        s.setByteOrder(QDataStream::LittleEndian);
        s << quint64(overrunCount_);
        overrunFp_->close();
        delete overrunFp_; overrunFp_ = nullptr;
    }
}

void LargeFileSearch::appendMatches(const QVector<LargeFileSearchMatch>& batch) {
    qint64 cap = qint64(maxMemMatches_);
    const auto sameDisplayGroup = [](const LargeFileSearchMatch &left,
                                     const LargeFileSearchMatch &right) {
        return left.line == right.line
               && left.contextByteOffset == right.contextByteOffset;
    };
    const auto writeOverrun = [this](const LargeFileSearchMatch &match) -> bool {
        if (!overrunFp_ && !prepareOverrunFile(nullptr)) {
            storageFailed_ = true;
            return false;
        }
        QDataStream stream(overrunFp_);
        stream.setByteOrder(QDataStream::LittleEndian);
        if (overrunFp_->pos() >= OVERRUN_FILE_LIMIT) {
            scanError_ = QObject::tr(
                "搜索结果已达到 1 GiB 临时存储上限；已保留此前结果，请缩小范围后重试。");
            return false;
        }
        if (match.line != overrunLastLine_
            || match.contextByteOffset != overrunLastContext_) {
            if ((overrunGroupedCount_ % 512) == 0) {
                overrunCheckpoints_.append(qMakePair(overrunGroupedCount_,
                                                      overrunFp_->pos()));
                overrunCheckpointBytes_.append(match.byteOffset);
            }
            ++overrunGroupedCount_;
            overrunLastLine_ = match.line;
            overrunLastContext_ = match.contextByteOffset;
        }
        stream << quint64(match.byteOffset) << qint32(match.length)
               << qint32(match.line) << qint32(match.col)
               << quint64(match.contextByteOffset);
        QByteArray context = match.lineContext;
        if (context.size() > OVERRUN_CTX_MAX)
            context.truncate(OVERRUN_CTX_MAX);
        stream << context;
        if (stream.status() != QDataStream::Ok) {
            storageFailed_ = true;
            return false;
        }
        ++overrunCount_;
        return true;
    };
    for (const LargeFileSearchMatch& m : batch) {
        if (!overrunFp_ && overrunCount_ == 0 && qint64(memMatches_.size()) < cap) {
            memMatches_.append(m);
        } else {
            if (!overrunFp_ && overrunCount_ == 0 && !memMatches_.isEmpty()
                && sameDisplayGroup(memMatches_.constLast(), m)) {
                int tail = memMatches_.size() - 1;
                while (tail > 0 && sameDisplayGroup(memMatches_.at(tail - 1), m))
                    --tail;
                const QVector<LargeFileSearchMatch> spilled = memMatches_.mid(tail);
                memMatches_.erase(memMatches_.begin() + tail, memMatches_.end());
                for (const LargeFileSearchMatch &previous : spilled) {
                    if (!writeOverrun(previous))
                        return;
                }
            }
            if (!writeOverrun(m))
                return;
        }
    }
}

// ============================================================================
//  KMP
// ============================================================================
QVector<int> LargeFileSearch::kmpBuildFail(const QByteArray& needle, bool caseSensitive) {
    QVector<int> fail(needle.size(), 0);
    int k = 0;
    const int m = needle.size();
    for (int q = 1; q < m; ++q) {
        while (k > 0 &&
               foldCaseAscii(needle[k], caseSensitive) != foldCaseAscii(needle[q], caseSensitive))
            k = fail[k-1];
        if (foldCaseAscii(needle[k], caseSensitive) == foldCaseAscii(needle[q], caseSensitive))
            ++k;
        fail[q] = k;
    }
    return fail;
}

QVector<QPair<qint64, int>> LargeFileSearch::kmpMatches(const uchar* text, qint64 n,
                                                        const QByteArray& needle,
                                                        bool caseSensitive)
{
    QVector<QPair<qint64, int>> out;
    if (needle.isEmpty() || n <= 0) return out;
    const int m = needle.size();
    QVector<int> fail = kmpBuildFail(needle, caseSensitive);
    int q = 0;
    for (qint64 i = 0; i < n; ++i) {
        uchar c = foldCaseAscii(text[i], caseSensitive);
        while (q > 0 && foldCaseAscii(needle[q], caseSensitive) != c) q = fail[q-1];
        if (foldCaseAscii(needle[q], caseSensitive) == c) ++q;
        if (q == m) {
            qint64 pos = i + 1 - m;
            out.append(qMakePair(pos, m));
            q = fail[q-1];
        }
    }
    return out;
}

// ============================================================================
//  readVirtualBytes / fillLineContext
// ============================================================================
QByteArray LargeFileSearch::readVirtualBytes(qint64 start, qint64 end) const {
    if (!mmapBase_ || end <= start) return {};
    if (start < 0) start = 0;
    qint64 cap = mmapSize_;
    if (patchList_) {
        // patch 会影响虚拟大小，但无法直接查询；使用 renderRange 时会 clamp 到实际
        return patchList_->renderRange(mmapBase_, mmapSize_, start, end);
    }
    if (end > cap) end = cap;
    return QByteArray((const char*)(mmapBase_ + start), int(end - start));
}

void LargeFileSearch::fillLineContext(LargeFileSearchMatch& m,
                                      TextEncoding encoding) const {
    const qint64 originalMatch = patchList_
        ? patchList_->virtToOrig(m.byteOffset) : m.byteOffset;
    const bool indexed = lineIndex_ && lineIndexCompleteAtSnapshot_;
    const qint64 virtualSize = patchList_
        ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    constexpr qint64 SideBytes = OVERRUN_CTX_MAX / 2;
    constexpr qint64 DecoderOverlap = 8;
    constexpr qint64 MaxVisibleMatchBytes = 64 * 1024;

    const auto assignContext = [&](qint64 allowedStart, qint64 allowedEnd,
                                   qint64 desiredStart, qint64 desiredEnd) {
        allowedStart = qBound<qint64>(0, allowedStart, virtualSize);
        allowedEnd = qBound<qint64>(allowedStart, allowedEnd, virtualSize);
        desiredStart = qBound(allowedStart, desiredStart, allowedEnd);
        desiredEnd = qBound(desiredStart, desiredEnd, allowedEnd);
        const qint64 rawStart = qMax(allowedStart,
                                     desiredStart - DecoderOverlap);
        const qint64 rawEnd = qMin(allowedEnd, desiredEnd + DecoderOverlap);
        const QByteArray raw = readVirtualBytes(rawStart, rawEnd);
        const DecodedTextProjection projection = decodeTextProjection(
            raw, encoding, rawStart == 0, false);
        if (projection.sourceBoundaries.isEmpty()) {
            m.contextByteOffset = desiredStart;
            m.lineContext = readVirtualBytes(desiredStart, desiredEnd);
            return;
        }
        const qint32 relativeStart = qint32(qBound<qint64>(
            0, desiredStart - rawStart, raw.size()));
        const qint32 relativeEnd = qint32(qBound<qint64>(
            relativeStart, desiredEnd - rawStart, raw.size()));
        auto startIt = std::upper_bound(
            projection.sourceBoundaries.cbegin(),
            projection.sourceBoundaries.cend(), relativeStart);
        if (startIt != projection.sourceBoundaries.cbegin())
            --startIt;
        auto endIt = std::lower_bound(
            projection.sourceBoundaries.cbegin(),
            projection.sourceBoundaries.cend(), relativeEnd);
        const qint32 alignedStart = startIt == projection.sourceBoundaries.cend()
            ? projection.sourceBoundaries.constLast() : *startIt;
        const qint32 alignedEnd = endIt == projection.sourceBoundaries.cend()
            ? projection.sourceBoundaries.constLast() : *endIt;
        m.contextByteOffset = rawStart + alignedStart;
        m.lineContext = raw.mid(alignedStart,
                                qMax(0, alignedEnd - alignedStart));
    };

    if (!indexed) {
        // Binary/mixed data has no trustworthy global line. Build one stable,
        // centered byte window and align its ends with the frozen encoding.
        const qint64 visibleMatchEnd = qMin(
            virtualSize, m.byteOffset
                + qMin<qint64>(qMax<qint32>(0, m.length),
                               MaxVisibleMatchBytes));
        assignContext(0, virtualSize,
                      qMax<qint64>(0, m.byteOffset - SideBytes),
                      qMin(virtualSize, visibleMatchEnd + SideBytes));
        m.line = -1;
        m.col = qint32(qBound<qint64>(
            qint64(0), m.byteOffset - m.contextByteOffset,
            qint64(INT32_MAX)));
        return;
    }
    int line = 0;
    qint64 originalColumn = 0;
    lineIndex_->byteToLineCol(originalMatch, &line, &originalColumn);
    Q_UNUSED(originalColumn);
    const qint64 originalLineStart = lineIndex_->lineToByteOffset(line);
    const qint64 originalLineEnd = lineIndex_->lineToByteOffset(line + 1);
    const qint64 lineStart = patchList_
        ? patchList_->origToVirtForLineStart(originalLineStart) : originalLineStart;
    const qint64 lineEnd = patchList_
        ? patchList_->origToVirtForLineStart(originalLineEnd) : originalLineEnd;
    m.line = line;
    m.col = qint32(qBound<qint64>(qint64(0), m.byteOffset - lineStart,
                                  qint64(INT32_MAX)));
    const qint64 visibleMatchEnd = qMin(
        lineEnd, m.byteOffset
            + qMin<qint64>(qMax<qint32>(0, m.length), MaxVisibleMatchBytes));
    assignContext(lineStart, lineEnd,
                  qMax(lineStart, m.byteOffset - SideBytes),
                  qMin(lineEnd, visibleMatchEnd + SideBytes));
}

bool LargeFileSearch::sourceInUse()
{
    Q_ASSERT(QThread::currentThread() == thread());
    std::lock_guard<std::mutex> lock(workerMutex_);
    return workerReservations_ != 0 || bool(currentWorker_) || stoppingWorkers_ || shuttingDown_;
}

bool LargeFileSearch::rebindIdleSource(const uchar *base, qint64 size,
                                      const LineIndex *lines, const PatchList *patches)
{
    if (sourceInUse()) return false;
    mmapBase_ = base;
    mmapSize_ = size;
    lineIndex_ = lines;
    lineIndexCompleteAtSnapshot_ = lines && !lines->isIncomplete();
    patchList_ = patches;
    return true;
}

void LargeFileSearch::fillByteContext(LargeFileSearchMatch& m) const
{
    constexpr qint64 BytesPerRow = 16;
    constexpr qint64 SideContext = OVERRUN_CTX_MAX / 4;
    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    const qint64 rowStart = (m.byteOffset / BytesPerRow) * BytesPerRow;
    const qint64 matchEnd = qMin(virtualSize,
        m.byteOffset + qMax<qint64>(0, m.length));
    qint64 contextStart = qMax<qint64>(0, m.byteOffset - SideContext);
    qint64 contextEnd = qMin(virtualSize,
        qMax(contextStart + OVERRUN_CTX_MAX / 2, matchEnd + SideContext));
    if (contextEnd - contextStart > OVERRUN_CTX_MAX) {
        contextEnd = qMin(virtualSize, matchEnd + SideContext);
        contextStart = qMax<qint64>(0, contextEnd - OVERRUN_CTX_MAX);
    }
    QByteArray preview = readVirtualBytes(contextStart, contextEnd);
    m.line = qint32(qMin<qint64>(rowStart / BytesPerRow, INT32_MAX));
    m.col = qint32(m.byteOffset - rowStart);
    m.contextByteOffset = contextStart;
    m.lineContext = std::move(preview);
}

bool LargeFileSearch::scanAllBounded(
    qint64 virtualSize, const QString &pattern, const Options &opts,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(const QVector<LargeFileSearchMatch>&)> &reportBatch)
{
    scanError_.clear();
    const auto closeSpool = qScopeGuard([this] { closeOverrunFile(); });
    constexpr qsizetype PublishBatchSize = 1000;
    QVector<LargeFileSearchMatch> pending;
    pending.reserve(PublishBatchSize);
    const auto publish = [&]() -> bool {
        if (pending.isEmpty())
            return true;
        appendMatches(pending);
        if (storageFailed_ || !scanError_.isEmpty())
            return false;
        if (reportBatch && !reportBatch(pending))
            return false;
        pending.clear();
        return true;
    };

    QByteArray literalBytes;
    bool usePcre2 = false;
    if (!preparePattern(pattern, opts, &literalBytes, &usePcre2, &scanError_))
        return false;

    const bool multibyteLiteral = !usePcre2
        && requiresMultibyteCharacterBoundaries(opts);
    const bool projectedLiteral = usesProjectedLiteral(
        pattern, opts, literalBytes, usePcre2)
        || (multibyteLiteral && patchList_ && patchList_->isModified());
    if (!usePcre2 && opts.reportPhase)
        opts.reportPhase(QObject::tr("扫描字面值"), 1, 1, 0, virtualSize);
    if (projectedLiteral) {
        const auto projected = scanProjectedLiteralRange(
            virtualSize, 0, virtualSize, pattern, opts, false,
            [this](qint64 start, qint64 end) {
                return readVirtualBytes(start, end);
            }, isCancelled, reportProgress,
            [&](qint64 offset, qint32 length) {
                LargeFileSearchMatch match;
                match.byteOffset = offset;
                match.length = length;
                fillLineContext(match, opts.textEncoding);
                pending.append(std::move(match));
                const bool firstVisibleResult = memMatches_.isEmpty()
                    && overrunCount_ == 0 && pending.size() == 1;
                return (!firstVisibleResult && pending.size() < PublishBatchSize)
                    || publish();
            }, &scanError_);
        if (!projected.ok)
            return false;
        return publish();
    }

    if (!usePcre2) {
        if (literalBytes.isEmpty())
            return true;
        const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
        if ((!patchList_ || !patchList_->isModified()) && mmapBase_) {
            const auto normalizeStart = multibyteLiteral
                ? makeEncodedBoundaryNormalizer(
                    mmapBase_, virtualSize, opts.textEncoding)
                : std::function<qint64(qint64)>();
            const auto nextCandidate = makeByteCandidateFinder(
                mmapBase_, virtualSize, literalBytes, caseSensitive, 0,
                isCancelled, reportProgress, normalizeStart);
            qint64 candidate = nextCandidate ? nextCandidate(0) : virtualSize + 1;
            while (candidate >= 0 && candidate <= virtualSize - literalBytes.size()) {
                if (opts.wholeWord && !asciiWholeWordAt(
                        [this](qint64 start, qint64 end) {
                            return readVirtualBytes(start, end);
                        }, virtualSize, candidate, literalBytes.size())) {
                    candidate = nextCandidate(candidate + 1);
                    continue;
                }
                LargeFileSearchMatch match;
                match.byteOffset = candidate;
                match.length = literalBytes.size();
                if (opts.byteContextMode)
                    fillByteContext(match);
                else
                    fillLineContext(match, opts.textEncoding);
                pending.append(std::move(match));
                const bool firstVisibleResult = memMatches_.isEmpty()
                    && overrunCount_ == 0 && pending.size() == 1;
                if ((firstVisibleResult || pending.size() >= PublishBatchSize)
                    && !publish()) {
                    return false;
                }
                candidate = nextCandidate(candidate + 1);
            }
            if (candidate < 0)
                return false;
            return publish();
        }
        const QVector<int> failure = kmpBuildFail(literalBytes, caseSensitive);
        const int needleSize = literalBytes.size();
        int matched = 0;
        constexpr qint64 ReadChunk = Q_INT64_C(16) * 1024 * 1024;
        for (qint64 start = 0; start < virtualSize; ) {
            if (isCancelled && isCancelled())
                return false;
            const qint64 end = qMin(virtualSize, start + ReadChunk);
            const QByteArray bytes = readVirtualBytes(start, end);
            if (bytes.isEmpty() && end > start)
                break;
            for (qsizetype index = 0; index < bytes.size(); ++index) {
                const uchar value = foldCaseAscii(uchar(bytes.at(index)), caseSensitive);
                while (matched > 0
                       && foldCaseAscii(uchar(literalBytes.at(matched)), caseSensitive)
                              != value) {
                    matched = failure.at(matched - 1);
                }
                if (foldCaseAscii(uchar(literalBytes.at(matched)), caseSensitive) == value)
                    ++matched;
                if (matched == needleSize) {
                    const qint64 offset = start + index + 1 - needleSize;
                    if (opts.wholeWord && !asciiWholeWordAt(
                            [this](qint64 from, qint64 to) {
                                return readVirtualBytes(from, to);
                            }, virtualSize, offset, needleSize)) {
                        matched = failure.at(matched - 1);
                        continue;
                    }
                    LargeFileSearchMatch match;
                    match.byteOffset = offset;
                    match.length = needleSize;
                    if (opts.byteContextMode)
                        fillByteContext(match);
                    else
                        fillLineContext(match, opts.textEncoding);
                    pending.append(std::move(match));
                    const bool firstVisibleResult = memMatches_.isEmpty()
                        && overrunCount_ == 0 && pending.size() == 1;
                    if ((firstVisibleResult || pending.size() >= PublishBatchSize)
                        && !publish())
                        return false;
                    matched = failure.at(matched - 1);
                }
            }
            start = end;
            if (reportProgress)
                reportProgress(start);
        }
    } else {
        const bool invalidUtfText = !opts.byteContextMode
            && opts.tolerateInvalidEncoding
            && (opts.textEncoding == TextEncoding::Utf8
                || opts.textEncoding == TextEncoding::Utf8Bom);
        const QByteArray invalidUtfLiteralFilter =
            safeInvalidUtfAsciiLiteralFilter(pattern, opts, invalidUtfText);
        const bool encodedByteRegex = !opts.byteContextMode
            && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
        Pcre2Regex compiled;
        QByteArray requiredLiteral;
        bool requiredLiteralCaseSensitive = true;
        Pcre2Regex::StartByteFilter startByteFilter;
        const bool binaryWholeWord = opts.byteContextMode
            && !opts.regexMode && opts.wholeWord;
        if (encodedByteRegex) {
            EncodedRegexCompiler::Output translated;
            const QString expression = rawSearchExpression(pattern, opts);
            if (!EncodedRegexCompiler::compile(
                    expression, opts.textEncoding, opts.caseSensitive,
                    &translated, &scanError_)
                || !compiled.compileBytePattern(
                    translated.bytePattern, translated.caseSensitive,
                    &scanError_)) {
                return false;
            }
            requiredLiteral = translated.requiredLiteral;
            requiredLiteralCaseSensitive =
                translated.requiredLiteralCaseSensitive;
        } else {
            if (!compiled.compile(pattern, !opts.regexMode, opts.caseSensitive,
                                  opts.wholeWord, &scanError_,
                                  !binaryWholeWord, invalidUtfText)) {
                return false;
            }
            startByteFilter = compiled.startByteFilter();
        }

        const bool validatesUtf8 = !encodedByteRegex && !binaryWholeWord
            && !invalidUtfText;
        RegexProgressPhases progressPhases(
            virtualSize, validatesUtf8, reportProgress, opts.reportPhase);
        const auto regexProgress = progressPhases.searchReporter();
        const auto validationProgress = progressPhases.validationReporter();

        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_snapshot_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        const bool needsSnapshot = patchList_ && patchList_->isModified();
        if (needsSnapshot) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                scanError_ = QObject::tr("无法创建正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            constexpr qint64 SnapshotChunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += SnapshotChunk) {
                if (isCancelled && isCancelled())
                    return false;
                const qint64 end = qMin(virtualSize, start + SnapshotChunk);
                const QByteArray bytes = readVirtualBytes(start, end);
                if (bytes.size() != end - start || snapshot.write(bytes) != bytes.size()) {
                    scanError_ = QObject::tr("无法写入正则搜索快照：%1")
                                     .arg(snapshot.errorString());
                    return false;
                }
            }
            if (!snapshot.flush()) {
                scanError_ = QObject::tr("无法刷新正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                scanError_ = QObject::tr("无法映射正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmapSnapshot = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });

        const auto recordMatch = [&](qint64 offset, qint64 length) {
                LargeFileSearchMatch match;
                match.byteOffset = offset;
                match.length = qint32(length);
                if (opts.byteContextMode)
                    fillByteContext(match);
                else
                    fillLineContext(match, opts.textEncoding);
                pending.append(std::move(match));
                const bool firstVisibleResult = memMatches_.isEmpty()
                    && overrunCount_ == 0 && pending.size() == 1;
                return (!firstVisibleResult && pending.size() < PublishBatchSize)
                    || publish();
            };
        if (encodedByteRegex) {
            const auto normalizeStart = makeEncodedBoundaryNormalizer(
                subject, virtualSize, opts.textEncoding);
            const auto nextCandidate = makeByteCandidateFinder(
                subject, virtualSize, requiredLiteral,
                requiredLiteralCaseSensitive, 0, isCancelled,
                regexProgress, normalizeStart);
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {},
                [subject, virtualSize, encoding = opts.textEncoding](qint64 offset) {
                    return EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, offset, encoding);
                }, 0, normalizeStart, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else if (!invalidUtfLiteralFilter.isEmpty()) {
            const auto nextCandidate = makeByteCandidateFinder(
                subject, virtualSize, invalidUtfLiteralFilter,
                opts.caseSensitive, 0, isCancelled, regexProgress, {});
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else if (!startByteFilter.isEmpty()) {
            const auto nextCandidate = makeStartByteCandidateFinder(
                subject, virtualSize, startByteFilter, 0, !invalidUtfText,
                isCancelled, regexProgress);
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else {
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, {},
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        }
    }

    const bool ok = publish();
    return ok && !storageFailed_ && scanError_.isEmpty();
}

bool LargeFileSearch::scanOneBounded(
    qint64 virtualSize, const QString &pattern, const Options &opts,
    qint64 fromByte, bool forward, bool wrap,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    LargeFileSearchMatch *match, QString *error) const
{
    if (match) {
        *match = {};
        match->byteOffset = -1;
    }
    if (error) error->clear();

    QByteArray literalBytes;
    bool usePcre2 = false;
    if (!preparePattern(pattern, opts, &literalBytes, &usePcre2, error))
        return false;

    const bool multibyteLiteral = !usePcre2
        && requiresMultibyteCharacterBoundaries(opts);
    const bool projectedLiteral = usesProjectedLiteral(
        pattern, opts, literalBytes, usePcre2)
        || (multibyteLiteral
            && (!forward || (patchList_ && patchList_->isModified())));
    if (!usePcre2 && opts.reportPhase)
        opts.reportPhase(QObject::tr("定向扫描字面值"), 1, 1, 0, virtualSize);
    if (projectedLiteral) {
        qint64 scanned = 0;
        const auto runRange = [&](qint64 start, qint64 end, bool reverse,
                                  LargeFileSearchMatch *accepted) {
            const auto projected = scanProjectedLiteralRange(
                virtualSize, start, end, pattern, opts, reverse,
                [this](qint64 from, qint64 to) {
                    return readVirtualBytes(from, to);
                }, isCancelled,
                [&](qint64 done) {
                    if (reportProgress)
                        reportProgress(scanned + done);
                },
                [&](qint64 offset, qint32 length) {
                    accepted->byteOffset = offset;
                    accepted->length = length;
                    return false;
                }, error);
            scanned += qMax<qint64>(0, end - start);
            return projected.ok;
        };
        LargeFileSearchMatch found;
        found.byteOffset = -1;
        bool ok = true;
        if (forward) {
            const qint64 start = fromByte < 0 ? 0
                : (fromByte >= virtualSize ? virtualSize : fromByte + 1);
            ok = runRange(start, virtualSize, false, &found);
            if (ok && found.byteOffset < 0 && wrap && start > 0)
                ok = runRange(0, start, false, &found);
        } else {
            const qint64 end = fromByte == std::numeric_limits<qint64>::max()
                ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
            ok = runRange(0, end, true, &found);
            if (ok && found.byteOffset < 0 && wrap && end < virtualSize)
                ok = runRange(end, virtualSize, true, &found);
        }
        if (!ok)
            return false;
        if (found.byteOffset >= 0) {
            fillLineContext(found, opts.textEncoding);
            if (match)
                *match = std::move(found);
        }
        return true;
    }

    if (!usePcre2 && (!patchList_ || !patchList_->isModified()) && mmapBase_) {
        const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
        const auto findFirstInRange = [&](qint64 start, qint64 end,
                                          LargeFileSearchMatch *accepted) {
            const auto normalizeStart = multibyteLiteral
                ? makeEncodedBoundaryNormalizer(
                    mmapBase_, virtualSize, opts.textEncoding)
                : std::function<qint64(qint64)>();
            const auto next = makeByteCandidateFinder(
                mmapBase_, virtualSize, literalBytes, caseSensitive, start,
                isCancelled, reportProgress, normalizeStart, end);
            qint64 candidate = next ? next(start) : virtualSize + 1;
            while (candidate >= 0 && candidate < end
                   && opts.wholeWord && !asciiWholeWordAt(
                       [this](qint64 from, qint64 to) {
                           return readVirtualBytes(from, to);
                       }, virtualSize, candidate, literalBytes.size())) {
                candidate = next(candidate + 1);
            }
            if (candidate < 0) return false;
            if (candidate < end) {
                accepted->byteOffset = candidate;
                accepted->length = literalBytes.size();
            }
            return true;
        };
        const auto findLastInRange = [&](qint64 start, qint64 end,
                                         LargeFileSearchMatch *accepted) {
            qint64 searchEnd = end;
            qint64 candidate = virtualSize + 1;
            do {
                candidate = findPreviousByteLiteral(
                    mmapBase_, virtualSize, literalBytes, caseSensitive,
                    start, searchEnd, isCancelled, reportProgress);
                if (candidate < 0 || candidate > virtualSize - literalBytes.size())
                    break;
                searchEnd = candidate;
            } while (opts.wholeWord && !asciiWholeWordAt(
                [this](qint64 from, qint64 to) {
                    return readVirtualBytes(from, to);
                }, virtualSize, candidate, literalBytes.size()));
            if (candidate >= 0 && candidate <= virtualSize - literalBytes.size()) {
                accepted->byteOffset = candidate;
                accepted->length = literalBytes.size();
            }
            return candidate >= 0;
        };
        LargeFileSearchMatch found;
        found.byteOffset = -1;
        bool ok = true;
        if (forward) {
            const qint64 start = fromByte < 0 ? 0
                : (fromByte >= virtualSize ? virtualSize : fromByte + 1);
            ok = findFirstInRange(start, virtualSize, &found);
            if (ok && found.byteOffset < 0 && wrap && start > 0)
                ok = findFirstInRange(0, start, &found);
        } else {
            const qint64 end = fromByte == std::numeric_limits<qint64>::max()
                ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
            ok = findLastInRange(0, end, &found);
            if (ok && found.byteOffset < 0 && wrap && end < virtualSize)
                ok = findLastInRange(end, virtualSize, &found);
        }
        if (!ok)
            return false;
        if (found.byteOffset >= 0) {
            if (opts.byteContextMode)
                fillByteContext(found);
            else
                fillLineContext(found, opts.textEncoding);
            if (match)
                *match = std::move(found);
        }
        return true;
    }

    const bool encodedByteRegex = usePcre2 && !opts.byteContextMode
        && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
    const bool binaryWholeWord = opts.byteContextMode
        && !opts.regexMode && opts.wholeWord;
    Pcre2Regex compiled;
    QByteArray requiredLiteral;
    bool requiredLiteralCaseSensitive = true;
    Pcre2Regex::StartByteFilter startByteFilter;
    bool invalidUtfText = false;
    if (usePcre2) {
        if (encodedByteRegex) {
            EncodedRegexCompiler::Output translated;
            const QString expression = rawSearchExpression(pattern, opts);
            if (!EncodedRegexCompiler::compile(
                    expression, opts.textEncoding, opts.caseSensitive,
                    &translated, error)
                || !compiled.compileBytePattern(
                    translated.bytePattern, translated.caseSensitive, error)) {
                return false;
            }
            requiredLiteral = translated.requiredLiteral;
            requiredLiteralCaseSensitive =
                translated.requiredLiteralCaseSensitive;
        } else {
            invalidUtfText = !opts.byteContextMode
                && opts.tolerateInvalidEncoding
                && (opts.textEncoding == TextEncoding::Utf8
                    || opts.textEncoding == TextEncoding::Utf8Bom);
            if (!compiled.compile(pattern, !opts.regexMode, opts.caseSensitive,
                                  opts.wholeWord, error,
                                  !binaryWholeWord, invalidUtfText)) {
                return false;
            }
            startByteFilter = compiled.startByteFilter();
        }
    }
    const QByteArray invalidUtfLiteralFilter =
        safeInvalidUtfAsciiLiteralFilter(pattern, opts, invalidUtfText);
    const bool validatesUtf8 = usePcre2 && !encodedByteRegex
        && !binaryWholeWord && !invalidUtfText;
    RegexProgressPhases progressPhases(
        virtualSize, validatesUtf8, reportProgress, opts.reportPhase);
    const auto regexProgress = progressPhases.searchReporter();
    const auto validationProgress = progressPhases.validationReporter();

    if (usePcre2) {
        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_navigation_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        if (patchList_ && patchList_->isModified()) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                if (error) *error = QObject::tr("无法创建搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += Chunk) {
                if (isCancelled && isCancelled())
                    return false;
                const qint64 end = qMin(virtualSize, start + Chunk);
                const QByteArray bytes = readVirtualBytes(start, end);
                if (bytes.size() != end - start
                    || snapshot.write(bytes) != bytes.size()) {
                    if (error) *error = QObject::tr("无法写入搜索快照：%1")
                                            .arg(snapshot.errorString());
                    return false;
                }
            }
            if (!snapshot.flush()) {
                if (error) *error = QObject::tr("无法刷新搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                if (error) *error = QObject::tr("无法映射搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmap = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });
        const std::function<qint64(qint64)> advance = encodedByteRegex
            ? std::function<qint64(qint64)>(
                [subject, virtualSize, encoding = opts.textEncoding](qint64 offset) {
                    return EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, offset, encoding);
                })
            : std::function<qint64(qint64)>();

        LargeFileSearchMatch found;
        found.byteOffset = -1;
        bool domainFailure = false;
        const auto accept = [&](qint64 offset, qint64 length, bool stopAtFirst) {
            if (length < 0 || length > std::numeric_limits<qint32>::max()) {
                domainFailure = true;
                if (error) *error = QObject::tr("单个匹配过长。");
                return false;
            }
            found.byteOffset = offset;
            found.length = qint32(length);
            return !stopAtFirst;
        };
        const auto runForward = [&](qint64 start, qint64 endExclusive) {
            const auto normalizeStart = encodedByteRegex
                ? makeEncodedBoundaryNormalizer(
                    subject, virtualSize, opts.textEncoding, start)
                : std::function<qint64(qint64)>();
            std::function<qint64(qint64)> nextCandidate;
            if (encodedByteRegex) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, requiredLiteral,
                    requiredLiteralCaseSensitive, start, isCancelled,
                    regexProgress, normalizeStart, endExclusive);
            } else if (!invalidUtfLiteralFilter.isEmpty()) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, invalidUtfLiteralFilter,
                    opts.caseSensitive, start, isCancelled,
                    regexProgress, {}, endExclusive);
            } else {
                nextCandidate = makeStartByteCandidateFinder(
                    subject, virtualSize, startByteFilter, start,
                    !invalidUtfText, isCancelled,
                    regexProgress, endExclusive);
            }
            const auto result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                [&](qint64 offset, qint64 length) {
                    return accept(offset, length, true);
                }, error, {}, advance, start, normalizeStart, nextCandidate,
                endExclusive, validationProgress);
            return result != Pcre2Regex::ScanResult::Failed
                && !domainFailure
                && (!(isCancelled && isCancelled()));
        };

        bool ok = true;
        if (forward) {
            const qint64 start = fromByte < 0 ? 0
                : fromByte >= virtualSize ? virtualSize
                : encodedByteRegex
                    ? EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, fromByte, opts.textEncoding)
                    : fromByte + 1;
            ok = runForward(start, virtualSize + 1);
            if (ok && found.byteOffset < 0 && wrap && start > 0)
                ok = runForward(0, start);
        } else {
            const qint64 end = fromByte == std::numeric_limits<qint64>::max()
                ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
            const auto normalizeStart = encodedByteRegex
                ? makeEncodedBoundaryNormalizer(
                    subject, virtualSize, opts.textEncoding)
                : std::function<qint64(qint64)>();
            std::function<qint64(qint64)> nextCandidate;
            if (encodedByteRegex) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, requiredLiteral,
                    requiredLiteralCaseSensitive, 0, isCancelled,
                    regexProgress, normalizeStart, end);
            } else if (!invalidUtfLiteralFilter.isEmpty()) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, invalidUtfLiteralFilter,
                    opts.caseSensitive, 0, isCancelled,
                    regexProgress, {}, end);
            } else {
                nextCandidate = makeStartByteCandidateFinder(
                    subject, virtualSize, startByteFilter, 0,
                    !invalidUtfText, isCancelled,
                    regexProgress, end);
            }
            const auto result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                [&](qint64 offset, qint64 length) {
                    if (offset >= end) {
                        return false;
                    }
                    return accept(offset, length, false);
                }, error, {}, advance, 0, normalizeStart, nextCandidate, end,
                validationProgress);
            ok = result != Pcre2Regex::ScanResult::Failed
                && !domainFailure
                && (!(isCancelled && isCancelled()));
            if (ok && found.byteOffset < 0 && wrap && end < virtualSize) {
                found.byteOffset = -1;
                const auto wrappedNormalizer = encodedByteRegex
                    ? makeEncodedBoundaryNormalizer(
                        subject, virtualSize, opts.textEncoding, end)
                    : std::function<qint64(qint64)>();
                std::function<qint64(qint64)> wrappedCandidate;
                if (encodedByteRegex) {
                    wrappedCandidate = makeByteCandidateFinder(
                        subject, virtualSize, requiredLiteral,
                        requiredLiteralCaseSensitive, end, isCancelled,
                        regexProgress, wrappedNormalizer, virtualSize + 1);
                } else if (!invalidUtfLiteralFilter.isEmpty()) {
                    wrappedCandidate = makeByteCandidateFinder(
                        subject, virtualSize, invalidUtfLiteralFilter,
                        opts.caseSensitive, end, isCancelled,
                        regexProgress, {}, virtualSize + 1);
                } else {
                    wrappedCandidate = makeStartByteCandidateFinder(
                        subject, virtualSize, startByteFilter, end,
                        !invalidUtfText, isCancelled,
                        regexProgress, virtualSize + 1);
                }
                const auto wrapped = compiled.scan(
                    subject, virtualSize, isCancelled, regexProgress,
                    [&](qint64 offset, qint64 length) {
                        return accept(offset, length, false);
                    }, error, {}, advance, end, wrappedNormalizer,
                    wrappedCandidate, virtualSize + 1, validationProgress);
                ok = wrapped != Pcre2Regex::ScanResult::Failed
                    && !domainFailure
                    && (!(isCancelled && isCancelled()));
            }
        }
        if (!ok)
            return false;
        if (found.byteOffset >= 0) {
            if (opts.byteContextMode)
                fillByteContext(found);
            else
                fillLineContext(found, opts.textEncoding);
            if (match) *match = std::move(found);
        }
        return true;
    }

    constexpr qint64 CoreBytes = Q_INT64_C(16) * 1024 * 1024;
    constexpr qint64 PrefixBytes = Q_INT64_C(64) * 1024;
    constexpr qint64 OverlapBytes = Q_INT64_C(16) * 1024 * 1024;
    qint64 scannedBytes = 0;

    const auto scanCore = [&](qint64 coreStart, qint64 coreEnd,
                              LargeFileSearchMatch *accepted) -> bool {
        if (isCancelled && isCancelled())
            return false;
        const qint64 windowStart = qMax<qint64>(0, coreStart - PrefixBytes);
        const qint64 windowEnd = qMin(virtualSize, coreEnd + OverlapBytes);
        const QByteArray bytes = readVirtualBytes(windowStart, windowEnd);
        if (bytes.size() != windowEnd - windowStart) {
            if (error) *error = QObject::tr("无法读取搜索窗口。");
            return false;
        }

        bool found = false;
        LargeFileSearchMatch best;
        const auto consider = [&](qint64 localOffset, qint64 length) {
            const qint64 globalOffset = windowStart + localOffset;
            if (globalOffset < coreStart || globalOffset >= coreEnd)
                return true;
            if (length < 0 || length > std::numeric_limits<qint32>::max()) {
                if (error) *error = QObject::tr("单个匹配过长。");
                return false;
            }
            best.byteOffset = globalOffset;
            best.length = qint32(length);
            found = true;
            // 前向扫描的首个结果就是最近结果；后向扫描保留本窗口最后一个。
            return !forward;
        };

        if (!usePcre2) {
            if (literalBytes.isEmpty())
                return true;
            const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
            const auto *windowSubject =
                reinterpret_cast<const uchar *>(bytes.constData());
            const auto nextCandidate = makeByteCandidateFinder(
                windowSubject, bytes.size(), literalBytes, caseSensitive, 0,
                isCancelled, {}, {});
            qint64 candidate = nextCandidate ? nextCandidate(0) : bytes.size() + 1;
            while (candidate >= 0
                   && candidate <= bytes.size() - literalBytes.size()) {
                if (!consider(candidate, literalBytes.size()) && found)
                    break;
                candidate = nextCandidate(candidate + 1);
            }
            if (candidate < 0 && !found)
                return false;
        } else {
            const auto noCancel = [&]() {
                return isCancelled && isCancelled();
            };
            const auto *windowSubject =
                reinterpret_cast<const uchar *>(bytes.constData());
            const auto result = compiled.scan(
                windowSubject, bytes.size(), noCancel, {}, consider, error, {},
                encodedByteRegex
                    ? std::function<qint64(qint64)>(
                        [windowSubject, size = qint64(bytes.size()),
                         encoding = opts.textEncoding](qint64 offset) {
                            return EncodedRegexCompiler::nextUnitOffset(
                                windowSubject, size, offset, encoding);
                        })
                    : std::function<qint64(qint64)>());
            if (result == Pcre2Regex::ScanResult::Failed)
                return false;
            if (result == Pcre2Regex::ScanResult::Cancelled && !found)
                return false;
        }

        if (found && accepted)
            *accepted = best;
        return true;
    };

    const auto scanForwardRange = [&](qint64 start, qint64 end,
                                      LargeFileSearchMatch *accepted) {
        for (qint64 coreStart = start; coreStart < end; ) {
            const qint64 coreEnd = qMin(end, coreStart + CoreBytes);
            if (!scanCore(coreStart, coreEnd, accepted))
                return false;
            scannedBytes += coreEnd - coreStart;
            if (reportProgress) reportProgress(scannedBytes);
            if (accepted && accepted->byteOffset >= 0)
                return true;
            coreStart = coreEnd;
        }
        return true;
    };
    const auto scanReverseRange = [&](qint64 start, qint64 end,
                                      LargeFileSearchMatch *accepted) {
        for (qint64 coreEnd = end; coreEnd > start; ) {
            const qint64 coreStart = qMax(start, coreEnd - CoreBytes);
            if (!scanCore(coreStart, coreEnd, accepted))
                return false;
            scannedBytes += coreEnd - coreStart;
            if (reportProgress) reportProgress(scannedBytes);
            if (accepted && accepted->byteOffset >= 0)
                return true;
            coreEnd = coreStart;
        }
        return true;
    };

    LargeFileSearchMatch found;
    found.byteOffset = -1;
    bool ok = true;
    if (forward) {
        const qint64 start = fromByte < 0 ? 0
            : (fromByte >= virtualSize ? virtualSize : fromByte + 1);
        ok = scanForwardRange(start, virtualSize, &found);
        if (ok && found.byteOffset < 0 && wrap && start > 0)
            ok = scanForwardRange(0, start, &found);
    } else {
        const qint64 end = fromByte == std::numeric_limits<qint64>::max()
            ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
        ok = scanReverseRange(0, end, &found);
        if (ok && found.byteOffset < 0 && wrap && end < virtualSize)
            ok = scanReverseRange(end, virtualSize, &found);
    }
    if (!ok)
        return false;
    if (found.byteOffset >= 0) {
        if (opts.byteContextMode)
            fillByteContext(found);
        else
            fillLineContext(found, opts.textEncoding);
        if (match) *match = std::move(found);
    }
    return true;
}

// ============================================================================
//  findAll（异步）
// ============================================================================
