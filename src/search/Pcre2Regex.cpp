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
                  qint64 *badOffset)
{
    qint64 i = 0;
    qint64 nextCancelCheck = 0;
    const auto continuation = [](uchar value) {
        return (value & 0xc0u) == 0x80u;
    };
    while (i < size) {
        if (i >= nextCancelCheck) {
            if (isCancelled && isCancelled())
                return false;
            nextCancelCheck = i + Q_INT64_C(1024) * 1024;
        }
        const uchar first = data[i];
        if (first <= 0x7f) {
            ++i;
            continue;
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

struct CalloutState {
    const std::function<bool()> *isCancelled = nullptr;
    const std::function<void(qint64)> *reportProgress = nullptr;
    qint64 nextProgress = 0;
};

int cancellationCallout(pcre2_callout_block *block, void *opaque)
{
    auto *state = static_cast<CalloutState *>(opaque);
    if (!state)
        return 0;
    if (state->isCancelled && *state->isCancelled
        && (*state->isCancelled)()) {
        return PCRE2_ERROR_CALLOUT;
    }
    const qint64 current = qint64(block->current_position);
    if (state->reportProgress && *state->reportProgress
        && current >= state->nextProgress) {
        (*state->reportProgress)(current);
        state->nextProgress = current + Q_INT64_C(4) * 1024 * 1024;
    }
    return 0;
}

} // namespace

struct Pcre2Regex::Impl {
    pcre2_code *code = nullptr;

    ~Impl()
    {
        if (code)
            pcre2_code_free(code);
    }
};

Pcre2Regex::Pcre2Regex() : impl_(std::make_unique<Impl>()) {}
Pcre2Regex::~Pcre2Regex() = default;
Pcre2Regex::Pcre2Regex(Pcre2Regex &&) noexcept = default;
Pcre2Regex &Pcre2Regex::operator=(Pcre2Regex &&) noexcept = default;

bool Pcre2Regex::compile(const QString &pattern, bool literal,
                         bool caseSensitive, bool wholeWord, QString *error)
{
    if (impl_->code) {
        pcre2_code_free(impl_->code);
        impl_->code = nullptr;
    }
    QByteArray expression = pattern.toUtf8();
    if (literal)
        expression = escapeLiteral(expression);
    if (wholeWord) {
        expression = QByteArrayLiteral("(?<![\\p{L}\\p{N}_])(?:") + expression
                     + QByteArrayLiteral(")(?![\\p{L}\\p{N}_])");
    }
    if (expression.isEmpty()) {
        if (error)
            *error = QObject::tr("查找文本为空。");
        return false;
    }

    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_AUTO_CALLOUT
                       | PCRE2_USE_OFFSET_LIMIT;
    if (!caseSensitive)
        options |= PCRE2_CASELESS;
    impl_->code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(expression.constData()),
        PCRE2_SIZE(expression.size()), options, &errorCode, &errorOffset, nullptr);
    if (!impl_->code) {
        if (error) {
            *error = QObject::tr("正则无效（字节 %1）：%2")
                         .arg(qulonglong(errorOffset))
                         .arg(pcreError(errorCode));
        }
        return false;
    }
    return true;
}

bool Pcre2Regex::isValid() const
{
    return impl_ && impl_->code;
}

Pcre2Regex::ScanResult Pcre2Regex::scan(
    const uchar *subject, qint64 subjectSize,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(qint64, qint64)> &onMatch,
    QString *error, const Limits &limits) const
{
    if (!isValid() || !subject || subjectSize < 0
        || quint64(subjectSize) > quint64(std::numeric_limits<PCRE2_SIZE>::max())) {
        if (error)
            *error = QObject::tr("正则扫描器或输入无效。");
        return ScanResult::Failed;
    }
    if (isCancelled && isCancelled())
        return ScanResult::Cancelled;

    qint64 badOffset = -1;
    if (!validateUtf8(subject, subjectSize, isCancelled, &badOffset)) {
        if (isCancelled && isCancelled())
            return ScanResult::Cancelled;
        if (error) {
            *error = QObject::tr(
                "正则搜索要求有效 UTF-8；在字节 %1 发现无效序列。请切换十六进制视图或使用大小写敏感的字面值搜索。")
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
    CalloutState callout{&isCancelled, &reportProgress, 0};
    pcre2_set_callout(matchContext, cancellationCallout, &callout);

    PCRE2_SIZE offset = 0;
    uint32_t matchOptions = PCRE2_NO_UTF_CHECK;
    bool retryEmptyAtSameOffset = false;
    while (offset <= PCRE2_SIZE(subjectSize)) {
        constexpr PCRE2_SIZE SearchWindow = PCRE2_SIZE(16) * 1024 * 1024;
        const PCRE2_SIZE offsetLimit = std::min(
            PCRE2_SIZE(subjectSize),
            offset > std::numeric_limits<PCRE2_SIZE>::max() - SearchWindow
                ? PCRE2_SIZE(subjectSize) : offset + SearchWindow);
        pcre2_set_offset_limit(matchContext, offsetLimit);
        const int result = pcre2_match(
            impl_->code, reinterpret_cast<PCRE2_SPTR>(subject),
            PCRE2_SIZE(subjectSize), offset, matchOptions, matchData, matchContext);
        if (result == PCRE2_ERROR_NOMATCH) {
            if (retryEmptyAtSameOffset) {
                const qint64 advanced = nextUtf8Offset(subject, subjectSize, qint64(offset));
                if (advanced > subjectSize)
                    break;
                offset = PCRE2_SIZE(advanced);
                matchOptions = PCRE2_NO_UTF_CHECK;
                retryEmptyAtSameOffset = false;
                continue;
            }
            if (offsetLimit < PCRE2_SIZE(subjectSize)) {
                qint64 advanced = qint64(offsetLimit) + 1;
                while (advanced < subjectSize
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
        if (start > end || end > PCRE2_SIZE(subjectSize)
            || end - start > PCRE2_SIZE(std::numeric_limits<qint32>::max())) {
            if (error)
                *error = QObject::tr("单个正则匹配超过 2 GiB，无法在结果模型中表示。");
            return ScanResult::Failed;
        }
        if (onMatch && !onMatch(qint64(start), qint64(end - start)))
            return ScanResult::Cancelled;
        if (reportProgress)
            reportProgress(qint64(end));

        offset = end;
        if (start == end) {
            // Perl-compatible global matching: retry a non-empty alternative at
            // the same position before advancing one complete UTF-8 code point.
            matchOptions = PCRE2_NO_UTF_CHECK | PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
            retryEmptyAtSameOffset = true;
        } else {
            matchOptions = PCRE2_NO_UTF_CHECK;
            retryEmptyAtSameOffset = false;
        }
    }
    if (reportProgress)
        reportProgress(subjectSize);
    return ScanResult::Complete;
}
