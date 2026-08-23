#include "Pcre2Regex.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <QByteArray>
#include <QObject>
#include <QScopeGuard>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {

QString pcreError(int code)
{
    PCRE2_UCHAR buffer[256] = {};
    const int length = pcre2_get_error_message(code, buffer, sizeof(buffer));
    if (length <= 0)
        return QObject::tr("PCRE2 错误 %1").arg(code);
    return QString::fromUtf8(reinterpret_cast<const char *>(buffer), length);
}

QByteArray escapeLiteral(const QByteArray &input)
{
    static constexpr char metacharacters[] = "\\.^$|()[]{}*+?";
    QByteArray escaped;
    escaped.reserve(input.size() * 2);
    for (const char value : input) {
        if (value > 0 && std::strchr(metacharacters, value))
            escaped.append('\\');
        escaped.append(value);
    }
    return escaped;
}

bool validateUtf8(const uchar *data, qint64 size,
                  const std::function<bool()> &isCancelled,
                  const std::function<void(qint64)> &reportProgress,
                  qint64 *badOffset)
{
    qint64 i = 0;
    qint64 nextCancelCheck = 0;
    const auto continuation = [](uchar value) {
        return (value & 0xc0u) == 0x80u;
    };
    while (i < size) {
        if (i >= nextCancelCheck) {
            if (reportProgress)
                reportProgress(i);
            if (isCancelled && isCancelled())
                return false;
            nextCancelCheck = i + Q_INT64_C(1024) * 1024;
        }
        uchar first = data[i];
        if (first <= 0x7f) {
            // Logs are overwhelmingly ASCII. Validate eight bytes per load
            // until either a high bit or the next cancellation checkpoint;
            // memcpy keeps the load alignment-safe on every platform.
            constexpr quint64 HighBits = Q_UINT64_C(0x8080808080808080);
            const qint64 asciiLimit = qMin(size, nextCancelCheck);
            while (i + qint64(sizeof(quint64)) <= asciiLimit) {
                quint64 word = 0;
                std::memcpy(&word, data + i, sizeof(word));
                if (word & HighBits)
                    break;
                i += sizeof(word);
            }
            if (i >= size)
                continue;
            first = data[i];
            if (first <= 0x7f) {
                ++i;
                continue;
            }
        }
        if (first >= 0xc2 && first <= 0xdf) {
            if (i + 1 < size && continuation(data[i + 1])) {
                i += 2;
                continue;
            }
        } else if (first >= 0xe0 && first <= 0xef) {
            if (i + 2 < size && continuation(data[i + 1])
                && continuation(data[i + 2])
                && !(first == 0xe0 && data[i + 1] < 0xa0)
                && !(first == 0xed && data[i + 1] >= 0xa0)) {
                i += 3;
                continue;
            }
        } else if (first >= 0xf0 && first <= 0xf4) {
            if (i + 3 < size && continuation(data[i + 1])
                && continuation(data[i + 2]) && continuation(data[i + 3])
                && !(first == 0xf0 && data[i + 1] < 0x90)
                && !(first == 0xf4 && data[i + 1] >= 0x90)) {
                i += 4;
                continue;
            }
        }
        if (badOffset)
            *badOffset = i;
        return false;
    }
    if (reportProgress)
        reportProgress(size);
    return true;
}

qint64 nextUtf8Offset(const uchar *subject, qint64 size, qint64 offset)
{
    if (offset >= size)
        return size + 1;
    ++offset;
    while (offset < size && (subject[offset] & 0xc0u) == 0x80u)
        ++offset;
    return offset;
}

} // namespace

struct Pcre2Regex::Impl {
    pcre2_code *code = nullptr;
    bool utfMode = true;
    bool matchInvalidUtf = false;
    bool literalMode = false;
    bool jitReady = false;

    ~Impl()
    {
        if (code)
            pcre2_code_free(code);
    }
};

namespace {

uint firstCodePoint(const QString &text)
{
    if (text.isEmpty()) return 0;
    const QChar first = text.front();
    return first.isHighSurrogate() && text.size() > 1
        && text.at(1).isLowSurrogate()
        ? QChar::surrogateToUcs4(first, text.at(1)) : first.unicode();
}

uint lastCodePoint(const QString &text)
{
    if (text.isEmpty()) return 0;
    const QChar last = text.back();
    return last.isLowSurrogate() && text.size() > 1
        && text.at(text.size() - 2).isHighSurrogate()
        ? QChar::surrogateToUcs4(text.at(text.size() - 2), last)
        : last.unicode();
}

bool usesUnspacedWordSegmentation(uint codePoint)
{
    switch (QChar::script(codePoint)) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
    case QChar::Script_Hangul:
    case QChar::Script_Bopomofo:
        return true;
    default:
        return false;
    }
}

bool needsWholeWordBoundary(uint codePoint)
{
    return !usesUnspacedWordSegmentation(codePoint)
        && (codePoint == uint('_') || QChar::isLetterOrNumber(codePoint));
}

} // namespace

Pcre2Regex::Pcre2Regex() : impl_(std::make_unique<Impl>()) {}
Pcre2Regex::~Pcre2Regex() = default;
Pcre2Regex::Pcre2Regex(Pcre2Regex &&) noexcept = default;
Pcre2Regex &Pcre2Regex::operator=(Pcre2Regex &&) noexcept = default;

bool Pcre2Regex::compile(const QString &pattern, bool literal,
                         bool caseSensitive, bool wholeWord, QString *error,
                         bool utfMode, bool matchInvalidUtf)
{
    if (impl_->code) {
        pcre2_code_free(impl_->code);
        impl_->code = nullptr;
    }
    impl_->jitReady = false;
    QByteArray expression = pattern.toUtf8();
    impl_->utfMode = utfMode;
    impl_->matchInvalidUtf = utfMode && matchInvalidUtf;
    impl_->literalMode = literal;
    if (literal)
        expression = escapeLiteral(expression);
    if (wholeWord) {
        const QByteArray wordClass = utfMode
            ? QByteArrayLiteral("\\p{L}\\p{N}_")
            : QByteArrayLiteral("A-Za-z0-9_");
        // CJK scripts do not require spaces between words. Treat a literal CJK
        // query as an exact phrase and apply boundaries only to query edges
        // that use space-delimited word semantics. Regex callers retain the
        // traditional two-sided boundary because their effective edges cannot
        // be inferred from the expression text.
        const bool leftBoundary = !literal
            || needsWholeWordBoundary(firstCodePoint(pattern));
        const bool rightBoundary = !literal
            || needsWholeWordBoundary(lastCodePoint(pattern));
        expression = (leftBoundary
                ? QByteArrayLiteral("(?<![") + wordClass + QByteArrayLiteral("])")
                : QByteArray())
            + QByteArrayLiteral("(?:") + expression + QByteArrayLiteral(")")
            + (rightBoundary
                ? QByteArrayLiteral("(?![") + wordClass + QByteArrayLiteral("])")
                : QByteArray());
    }
    if (expression.isEmpty()) {
        if (error)
            *error = QObject::tr("查找文本为空。");
        return false;
    }

    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    // Offset windows and PCRE2's match/depth/heap limits are the preemption
    // boundary. PCRE2_AUTO_CALLOUT inserts a callback before every pattern
    // item, disables important start optimizations and makes multi-GiB literal-
    // anchored log regexes hundreds of times slower.
    uint32_t options = PCRE2_USE_OFFSET_LIMIT;
    if (utfMode)
        options |= PCRE2_UTF | PCRE2_UCP;
    if (impl_->matchInvalidUtf)
        options |= PCRE2_MATCH_INVALID_UTF;
    if (!caseSensitive)
        options |= PCRE2_CASELESS;
    impl_->code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(expression.constData()),
        PCRE2_SIZE(expression.size()), options, &errorCode, &errorOffset, nullptr);
    if (!impl_->code) {
        if (error) {
            *error = literal
                ? QObject::tr("无法准备整词文本搜索（字节 %1）：%2")
                      .arg(qulonglong(errorOffset)).arg(pcreError(errorCode))
                : QObject::tr("正则无效（字节 %1）：%2")
                      .arg(qulonglong(errorOffset)).arg(pcreError(errorCode));
        }
        return false;
    }
    impl_->jitReady = pcre2_jit_compile(
        impl_->code, PCRE2_JIT_COMPLETE) == 0;
    return true;
}

bool Pcre2Regex::compileBytePattern(const QByteArray &pattern,
                                    bool caseSensitive, QString *error)
{
    if (impl_->code) {
        pcre2_code_free(impl_->code);
        impl_->code = nullptr;
    }
    impl_->jitReady = false;
    if (pattern.isEmpty()) {
        if (error)
            *error = QObject::tr("正则表达式为空。");
        return false;
    }
    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    uint32_t options = PCRE2_USE_OFFSET_LIMIT;
    if (!caseSensitive)
        options |= PCRE2_CASELESS;
    impl_->code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.constData()),
        PCRE2_SIZE(pattern.size()), options, &errorCode, &errorOffset, nullptr);
    impl_->utfMode = false;
    impl_->matchInvalidUtf = false;
    impl_->literalMode = false;
    if (!impl_->code) {
        if (error)
            *error = QObject::tr("无法准备当前编码的正则：%1")
                         .arg(pcreError(errorCode));
        return false;
    }
    impl_->jitReady = pcre2_jit_compile(
        impl_->code, PCRE2_JIT_COMPLETE) == 0;
    return true;
}

bool Pcre2Regex::isValid() const
{
    return impl_ && impl_->code;
}

Pcre2Regex::StartByteFilter Pcre2Regex::startByteFilter() const
{
    StartByteFilter filter;
    if (!isValid())
        return filter;

    uint32_t type = 0;
    uint32_t codeUnit = 0;
    if (pcre2_pattern_info(impl_->code, PCRE2_INFO_FIRSTCODETYPE, &type) == 0
        && type == 1
        && pcre2_pattern_info(impl_->code, PCRE2_INFO_FIRSTCODEUNIT,
                              &codeUnit) == 0) {
        if (codeUnit < 0x80u)
            filter.asciiBytes.append(char(codeUnit));
        else
            filter.anyNonAscii = true;
        return filter;
    }

    const uint8_t *bitmap = nullptr;
    if (pcre2_pattern_info(impl_->code, PCRE2_INFO_FIRSTBITMAP, &bitmap) != 0
        || !bitmap) {
        return filter;
    }
    for (int value = 0; value < 128; ++value) {
        if ((bitmap[value / 8] & (1u << (value & 7))) != 0)
            filter.asciiBytes.append(char(value));
    }
    for (int value = 128; value < 256; ++value) {
        if ((bitmap[value / 8] & (1u << (value & 7))) != 0) {
            filter.anyNonAscii = true;
            break;
        }
    }
    return filter;
}

bool Pcre2Regex::expandReplacementAt(
    const uchar *subject, qint64 subjectSize,
    qint64 matchOffset, qint64 matchLength,
    const QString &replacement, QByteArray *expanded, QString *error) const
{
    if (expanded)
        expanded->clear();
    if (!expanded || !isValid() || !subject || subjectSize < 0
        || matchOffset < 0 || matchLength < 0
        || matchOffset > subjectSize || matchLength > subjectSize - matchOffset) {
        if (error)
            *error = QObject::tr("正则替换的匹配范围无效。");
        return false;
    }

    pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(
        impl_->code, nullptr);
    if (!matchData) {
        if (error)
            *error = QObject::tr("无法分配正则替换上下文。");
        return false;
    }
    const auto cleanup = qScopeGuard([&] { pcre2_match_data_free(matchData); });
    const int result = pcre2_match(
        impl_->code, reinterpret_cast<PCRE2_SPTR>(subject),
        PCRE2_SIZE(subjectSize), PCRE2_SIZE(matchOffset),
        PCRE2_ANCHORED | (impl_->utfMode && !impl_->matchInvalidUtf
                              ? PCRE2_NO_UTF_CHECK : 0),
        matchData, nullptr);
    if (result < 0) {
        if (error)
            *error = QObject::tr("无法重新解析当前正则匹配：%1")
                         .arg(pcreError(result));
        return false;
    }
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
    const uint32_t captureCount = pcre2_get_ovector_count(matchData);
    if (!ovector || captureCount == 0
        || qint64(ovector[0]) != matchOffset
        || qint64(ovector[1]) != matchOffset + matchLength) {
        if (error)
            *error = QObject::tr("正文已变化，当前选区不再是同一个正则匹配。");
        return false;
    }

    QString output;
    output.reserve(replacement.size() + int(qMin<qint64>(matchLength, 4096)));
    const auto appendCapture = [&](int number) -> bool {
        if (number < 0 || uint32_t(number) >= captureCount) {
            if (error)
                *error = QObject::tr("替换文本引用了不存在的捕获组 %1。").arg(number);
            return false;
        }
        const PCRE2_SIZE start = ovector[number * 2];
        const PCRE2_SIZE end = ovector[number * 2 + 1];
        if (start == PCRE2_UNSET || end == PCRE2_UNSET)
            return true; // unmatched optional group expands to empty
        if (start > end || end > PCRE2_SIZE(subjectSize)) {
            if (error)
                *error = QObject::tr("捕获组 %1 的范围无效。").arg(number);
            return false;
        }
        output.append(QString::fromUtf8(
            reinterpret_cast<const char *>(subject + start), qsizetype(end - start)));
        return true;
    };
    const auto captureNumber = [&](const QString &token, int *number) -> bool {
        bool numeric = false;
        const int parsed = token.toInt(&numeric);
        if (numeric) {
            *number = parsed;
            return true;
        }
        const QByteArray name = token.toUtf8();
        const int resolved = pcre2_substring_number_from_name(
            impl_->code, reinterpret_cast<PCRE2_SPTR>(name.constData()));
        if (resolved < 0) {
            if (error)
                *error = QObject::tr("替换文本引用了不存在的命名捕获组“%1”。")
                             .arg(token);
            return false;
        }
        *number = resolved;
        return true;
    };

    for (qsizetype i = 0; i < replacement.size(); ++i) {
        const QChar ch = replacement.at(i);
        if (ch == QLatin1Char('$') && i + 1 < replacement.size()) {
            const QChar next = replacement.at(i + 1);
            if (next == QLatin1Char('$')) {
                output.append(QLatin1Char('$'));
                ++i;
                continue;
            }
            QString token;
            if (next == QLatin1Char('{')) {
                const qsizetype close = replacement.indexOf(QLatin1Char('}'), i + 2);
                if (close < 0) {
                    if (error)
                        *error = QObject::tr("替换文本中的 ${...} 引用缺少右花括号。");
                    return false;
                }
                token = replacement.mid(i + 2, close - i - 2);
                i = close;
            } else if (next.isDigit()) {
                qsizetype end = i + 1;
                while (end < replacement.size() && replacement.at(end).isDigit())
                    ++end;
                token = replacement.mid(i + 1, end - i - 1);
                i = end - 1;
            } else {
                output.append(ch);
                continue;
            }
            int number = -1;
            if (token.isEmpty() || !captureNumber(token, &number)
                || !appendCapture(number)) {
                if (token.isEmpty() && error)
                    *error = QObject::tr("替换文本中的捕获组引用为空。");
                return false;
            }
            continue;
        }
        if (ch == QLatin1Char('\\') && i + 1 < replacement.size()) {
            const QChar next = replacement.at(++i);
            if (next == QLatin1Char('n')) output.append(QLatin1Char('\n'));
            else if (next == QLatin1Char('r')) output.append(QLatin1Char('\r'));
            else if (next == QLatin1Char('t')) output.append(QLatin1Char('\t'));
            else if (next == QLatin1Char('\\')) output.append(QLatin1Char('\\'));
            else if (next.isDigit()) {
                QString token(next);
                while (i + 1 < replacement.size() && replacement.at(i + 1).isDigit())
                    token.append(replacement.at(++i));
                int number = -1;
                if (!captureNumber(token, &number) || !appendCapture(number))
                    return false;
            } else {
                output.append(QLatin1Char('\\'));
                output.append(next);
            }
            continue;
        }
        output.append(ch);
    }
    *expanded = output.toUtf8();
    if (!output.isEmpty())
        output.fill(QChar(0));
    return true;
}

Pcre2Regex::ScanResult Pcre2Regex::scan(
    const uchar *subject, qint64 subjectSize,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(qint64, qint64)> &onMatch,
    QString *error, const Limits &limits,
    const std::function<qint64(qint64)> &advanceAfterEmpty,
    qint64 startOffset,
    const std::function<qint64(qint64)> &normalizeMatchStart,
    const std::function<qint64(qint64)> &nextCandidateStart,
    qint64 maxMatchStartExclusive,
    const std::function<void(qint64)> &reportValidationProgress) const
{
    if (!isValid() || !subject || subjectSize < 0
        || startOffset < 0 || startOffset > subjectSize
        || quint64(subjectSize) > quint64(std::numeric_limits<PCRE2_SIZE>::max())) {
        if (error)
            *error = QObject::tr("正则扫描器或输入无效。");
        return ScanResult::Failed;
    }
    if (isCancelled && isCancelled())
        return ScanResult::Cancelled;

    const qint64 scanStartLimit = maxMatchStartExclusive
            == (std::numeric_limits<qint64>::max)()
        ? (subjectSize == (std::numeric_limits<qint64>::max)()
               ? subjectSize : subjectSize + 1)
        : qBound<qint64>(0, maxMatchStartExclusive,
                         subjectSize == (std::numeric_limits<qint64>::max)()
                             ? subjectSize : subjectSize + 1);
    if (startOffset >= scanStartLimit) {
        if (reportProgress)
            reportProgress(qMin(subjectSize, scanStartLimit));
        return ScanResult::Complete;
    }

    qint64 badOffset = -1;
    if (impl_->utfMode && !impl_->matchInvalidUtf
        && !validateUtf8(subject, subjectSize, isCancelled,
                         reportValidationProgress, &badOffset)) {
        if (isCancelled && isCancelled())
            return ScanResult::Cancelled;
        if (error) {
            *error = impl_->literalMode
                ? QObject::tr(
                      "Unicode 整词搜索要求有效 UTF-8；在字节 %1 发现无效序列。请切换十六进制字节搜索。")
                      .arg(badOffset)
                : QObject::tr(
                      "正则搜索要求有效 UTF-8；在字节 %1 发现无效序列。请切换十六进制视图或使用字面值搜索。")
                      .arg(badOffset);
        }
        return ScanResult::Failed;
    }

    pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(
        impl_->code, nullptr);
    pcre2_match_context *matchContext = pcre2_match_context_create(nullptr);
    if (!matchData || !matchContext) {
        if (matchData) pcre2_match_data_free(matchData);
        if (matchContext) pcre2_match_context_free(matchContext);
        if (error) *error = QObject::tr("无法分配正则匹配上下文。");
        return ScanResult::Failed;
    }
    const auto cleanup = qScopeGuard([&] {
        pcre2_match_data_free(matchData);
        pcre2_match_context_free(matchContext);
    });

    pcre2_set_match_limit(matchContext, limits.matchLimit);
    pcre2_set_depth_limit(matchContext, limits.depthLimit);
    pcre2_set_heap_limit(matchContext, limits.heapLimitKiB);
    struct CalloutControl {
        const std::function<bool()> *cancelled = nullptr;
        const std::function<void(qint64)> *progress = nullptr;
        quint64 calls = 0;
        qint64 lastProgress = 0;
    } calloutControl{&isCancelled, &reportProgress, 0, startOffset};
    pcre2_set_callout(
        matchContext,
        [](pcre2_callout_block *block, void *data) -> int {
            auto *control = static_cast<CalloutControl *>(data);
            if (!control || ((++control->calls & 0x0fffu) != 0u))
                return 0;
            if (block && control->progress && *control->progress) {
                const qint64 current = qint64(block->current_position);
                if (current >= control->lastProgress
                                   + Q_INT64_C(8) * 1024 * 1024) {
                    control->lastProgress = current;
                    (*control->progress)(current);
                }
            }
            return control->cancelled && *control->cancelled
                       && (*control->cancelled)()
                ? PCRE2_ERROR_CALLOUT : 0;
        },
        &calloutControl);
    PCRE2_SIZE offset = PCRE2_SIZE(startOffset);
    const uint32_t noUtfCheck = impl_->utfMode && !impl_->matchInvalidUtf
        ? PCRE2_NO_UTF_CHECK : 0;
    uint32_t matchOptions = noUtfCheck;
    bool retryEmptyAtSameOffset = false;
    while (offset <= PCRE2_SIZE(subjectSize)
           && qint64(offset) < scanStartLimit) {
        if (nextCandidateStart && !retryEmptyAtSameOffset) {
            const qint64 candidate = nextCandidateStart(qint64(offset));
            if (candidate < 0) {
                return isCancelled && isCancelled()
                    ? ScanResult::Cancelled : ScanResult::Failed;
            }
            if (candidate > subjectSize || candidate >= scanStartLimit)
                break;
            offset = PCRE2_SIZE(candidate);
        }
        // Offset limits preserve full-subject semantics (a match may extend
        // beyond the window) while bounding the interval between cancellation
        // and progress checks. One MiB keeps invalid-UTF binary presentations
        // responsive without allocating a converted copy of the file.
        constexpr PCRE2_SIZE SearchWindow = PCRE2_SIZE(1) * 1024 * 1024;
        const PCRE2_SIZE maximumAllowedStart = PCRE2_SIZE(
            qMin(subjectSize, scanStartLimit - 1));
        const PCRE2_SIZE offsetLimit = std::min(
            maximumAllowedStart,
            offset > std::numeric_limits<PCRE2_SIZE>::max() - SearchWindow
                ? maximumAllowedStart : offset + SearchWindow);
        pcre2_set_offset_limit(matchContext, offsetLimit);
        const uint32_t currentOptions = matchOptions
            | (nextCandidateStart ? PCRE2_ANCHORED : 0);
        int result = impl_->jitReady
            ? pcre2_jit_match(
                impl_->code, reinterpret_cast<PCRE2_SPTR>(subject),
                PCRE2_SIZE(subjectSize), offset, currentOptions,
                matchData, matchContext)
            : pcre2_match(
                impl_->code, reinterpret_cast<PCRE2_SPTR>(subject),
                PCRE2_SIZE(subjectSize), offset, currentOptions,
                matchData, matchContext);
        if (result == PCRE2_ERROR_JIT_BADOPTION) {
            result = pcre2_match(
                impl_->code, reinterpret_cast<PCRE2_SPTR>(subject),
                PCRE2_SIZE(subjectSize), offset, currentOptions,
                matchData, matchContext);
        }
        if (result == PCRE2_ERROR_NOMATCH) {
            if (retryEmptyAtSameOffset) {
                const qint64 advanced = advanceAfterEmpty
                    ? advanceAfterEmpty(qint64(offset))
                    : impl_->utfMode
                        ? nextUtf8Offset(subject, subjectSize, qint64(offset))
                        : qint64(offset) + 1;
                if (advanced > subjectSize)
                    break;
                offset = PCRE2_SIZE(advanced);
                matchOptions = noUtfCheck;
                retryEmptyAtSameOffset = false;
                continue;
            }
            if (nextCandidateStart) {
                if (offset >= PCRE2_SIZE(subjectSize))
                    break;
                ++offset;
                matchOptions = noUtfCheck;
                continue;
            }
            if (offsetLimit < maximumAllowedStart) {
                qint64 advanced = qint64(offsetLimit) + 1;
                while (impl_->utfMode && advanced < subjectSize
                       && (subject[advanced] & 0xc0u) == 0x80u) {
                    ++advanced;
                }
                offset = PCRE2_SIZE(advanced);
                if (reportProgress)
                    reportProgress(qint64(offsetLimit));
                continue;
            }
            break;
        }
        if (result == PCRE2_ERROR_CALLOUT && isCancelled && isCancelled())
            return ScanResult::Cancelled;
        if (result < 0) {
            if (error) {
                if (result == PCRE2_ERROR_MATCHLIMIT || result == PCRE2_ERROR_DEPTHLIMIT
                    || result == PCRE2_ERROR_HEAPLIMIT) {
                    *error = QObject::tr(
                        "正则表达式达到安全资源上限（%1）。请简化回溯、缩小搜索范围或改用字面值。")
                                 .arg(pcreError(result));
                } else {
                    *error = QObject::tr("正则扫描失败：%1").arg(pcreError(result));
                }
            }
            return ScanResult::Failed;
        }

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
        const PCRE2_SIZE start = ovector[0];
        const PCRE2_SIZE end = ovector[1];
        if (qint64(start) >= scanStartLimit)
            break;
        if (start > end || end > PCRE2_SIZE(subjectSize)
            || end - start > PCRE2_SIZE(std::numeric_limits<qint32>::max())) {
            if (error)
                *error = QObject::tr("单个正则匹配超过 2 GiB，无法在结果模型中表示。");
            return ScanResult::Failed;
        }
        if (normalizeMatchStart) {
            const qint64 normalized = normalizeMatchStart(qint64(start));
            if (normalized != qint64(start)) {
                if (normalized <= qint64(start) || normalized > subjectSize) {
                    if (error)
                        *error = QObject::tr("编码边界扫描器返回了无效偏移。");
                    return ScanResult::Failed;
                }
                offset = PCRE2_SIZE(normalized);
                matchOptions = noUtfCheck;
                retryEmptyAtSameOffset = false;
                continue;
            }
        }
        if (onMatch && !onMatch(qint64(start), qint64(end - start)))
            return ScanResult::Cancelled;
        if (reportProgress)
            reportProgress(qint64(end));

        offset = end;
        if (start == end) {
            // Perl-compatible global matching: retry a non-empty alternative at
            // the same position before advancing one complete UTF-8 code point.
            matchOptions = noUtfCheck | PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
            retryEmptyAtSameOffset = true;
        } else {
            matchOptions = noUtfCheck;
            retryEmptyAtSameOffset = false;
        }
    }
    if (reportProgress)
        reportProgress(qMin(subjectSize, scanStartLimit));
    return ScanResult::Complete;
}
