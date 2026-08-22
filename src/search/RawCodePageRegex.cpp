#include "RawCodePageRegex.h"

#include <cstddef>

namespace boost {
inline std::size_t hash_value(char32_t const &value) { return value; }
}

#include <boost/regex.hpp>

#include <QObject>
#include <QChar>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

struct ScanCancelled final {};

struct IterationControl {
    const std::function<bool()> *isCancelled = nullptr;
    const std::function<void(qint64)> *reportProgress = nullptr;
    mutable quint64 operations = 0;
    mutable qint64 furthestOffset = 0;
    mutable qint64 nextProgressOffset = Q_INT64_C(16) * 1024 * 1024;

    void checkpoint(qint64 offset) const
    {
        if (offset > furthestOffset)
            furthestOffset = offset;
        if (furthestOffset >= nextProgressOffset) {
            if (reportProgress && *reportProgress)
                (*reportProgress)(furthestOffset);
            nextProgressOffset = furthestOffset + Q_INT64_C(16) * 1024 * 1024;
        }
        // Iterator activity occurs inside Boost.Regex's backtracking loop, so
        // this check also makes a single expensive match cancellable.
        if (((++operations & 0x3fffu) == 0u)
            && isCancelled && *isCancelled && (*isCancelled)()) {
            throw ScanCancelled{};
        }
    }
};

struct BoundaryAnchors {
    static constexpr qint64 Stride = Q_INT64_C(64) * 1024;
    std::vector<const uchar *> offsets;

    explicit BoundaryAnchors(const uchar *begin)
    {
        offsets.push_back(begin);
    }

    void observe(const uchar *position)
    {
        if (position - offsets.back() >= Stride)
            offsets.push_back(position);
    }

    const uchar *nearestBefore(const uchar *position) const
    {
        const auto found = std::lower_bound(offsets.begin(), offsets.end(), position);
        return found == offsets.begin() ? offsets.front() : *std::prev(found);
    }
};

struct UnicodeRegexTraits
{
    using char_type = char32_t;
    using size_type = std::size_t;
    using string_type = std::vector<char32_t>;
    using locale_type = int;
    using char_class_type = std::uint32_t;

    static size_type length(const char_type *value)
    {
        size_type length = 0;
        while (*value++) ++length;
        return length;
    }
    char_type translate(char_type value) const { return value; }
    char_type translate_nocase(char_type value) const
    {
        return QChar::toCaseFolded(value);
    }
    static string_type transform(const char_type *first, const char_type *last)
    {
        return string_type(first, last);
    }
    static string_type transform_primary(const char_type *first,
                                         const char_type *last)
    {
        return string_type(first, last);
    }
    static char_class_type lookup_classname(const char_type *first,
                                            const char_type *last)
    {
        std::string name;
        for (; first != last; ++first) {
            if (*first > 0x7f) return 0;
            name.push_back(char(*first));
        }
        return boost::c_regex_traits<char>::lookup_classname(
            name.data(), name.data() + name.size());
    }
    static string_type lookup_collatename(const char_type *first,
                                          const char_type *last)
    {
        return string_type(first, last);
    }
    static bool isctype(char_type value, char_class_type mask)
    {
        using namespace boost::BOOST_REGEX_DETAIL_NS;
        const auto category = QChar::category(value);
        const bool control = category == QChar::Other_Control
            || category == QChar::Other_Format
            || category == QChar::Other_Surrogate;
        const bool punctuation = category >= QChar::Punctuation_Connector
            && category <= QChar::Punctuation_Other;
        const bool separator = value == U'\n' || value == U'\r'
            || value == 0x85 || value == 0x2028 || value == 0x2029;
        return ((mask & char_class_space) && QChar::isSpace(value))
            || ((mask & char_class_print) && !control
                && category != QChar::Other_NotAssigned)
            || ((mask & char_class_cntrl) && control)
            || ((mask & char_class_upper) && QChar::isUpper(value))
            || ((mask & char_class_lower) && QChar::isLower(value))
            || ((mask & char_class_alpha) && QChar::isLetter(value))
            || ((mask & char_class_digit) && QChar::isDigit(value))
            || ((mask & char_class_punct) && punctuation)
            || ((mask & char_class_xdigit)
                && ((value >= U'0' && value <= U'9')
                    || (value >= U'a' && value <= U'f')
                    || (value >= U'A' && value <= U'F')))
            || ((mask & char_class_blank) && QChar::isSpace(value) && !separator)
            || ((mask & char_class_word)
                && (value == U'_' || QChar::isLetterOrNumber(value)
                    || category == QChar::Mark_NonSpacing
                    || category == QChar::Mark_SpacingCombining))
            || ((mask & char_class_unicode) && value > 0xff)
            || ((mask & char_class_vertical) && (separator || value == U'\v'))
            || ((mask & char_class_horizontal)
                && QChar::isSpace(value) && !separator && value != U'\v');
    }
    static int value(char_type value, int radix)
    {
        if (value >= U'0' && value <= U'9') {
            const int digit = int(value - U'0');
            return digit < radix ? digit : -1;
        }
        if (radix == 16 && value >= U'a' && value <= U'f')
            return int(value - U'a') + 10;
        if (radix == 16 && value >= U'A' && value <= U'F')
            return int(value - U'A') + 10;
        return -1;
    }
    locale_type imbue(locale_type value) { return value; }
    locale_type getloc() const { return 0; }
};

struct DecodedScalar {
    char32_t value = 0xfffd;
    int width = 1;
};

class EncodedIterator
{
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = char32_t;
    using difference_type = std::ptrdiff_t;
    using pointer = const char32_t *;
    using reference = char32_t;

    EncodedIterator() = default;
    EncodedIterator(const uchar *begin, const uchar *position, const uchar *end,
                TextEncoding encoding,
                BoundaryAnchors *anchors, const IterationControl *control)
        : begin_(begin), position_(position), end_(end), anchors_(anchors),
          control_(control), encoding_(encoding)
    {
    }

    char32_t operator*() const
    {
        checkpoint();
        if (!position_ || position_ >= end_)
            return 0;
        return scalarAt(position_).value;
    }

    EncodedIterator &operator++()
    {
        checkpoint();
        if (position_ < end_) {
            position_ += scalarAt(position_).width;
            if (anchors_)
                anchors_->observe(position_);
        }
        return *this;
    }

    EncodedIterator operator++(int)
    {
        EncodedIterator copy(*this);
        ++*this;
        return copy;
    }

    EncodedIterator &operator--()
    {
        checkpoint();
        if (!position_ || position_ <= begin_)
            return *this;
        const uchar *cursor = anchors_ ? anchors_->nearestBefore(position_) : begin_;
        const uchar *previous = cursor;
        while (cursor < position_) {
            previous = cursor;
            cursor += scalarAt(cursor).width;
        }
        position_ = previous;
        return *this;
    }

    EncodedIterator operator--(int)
    {
        EncodedIterator copy(*this);
        --*this;
        return copy;
    }

    friend bool operator==(const EncodedIterator &left, const EncodedIterator &right)
    {
        return left.begin_ == right.begin_ && left.position_ == right.position_;
    }

    friend bool operator!=(const EncodedIterator &left, const EncodedIterator &right)
    {
        return !(left == right);
    }

    qint64 byteOffset() const
    {
        return position_ && begin_ ? position_ - begin_ : 0;
    }

private:
    DecodedScalar scalarAt(const uchar *cursor) const
    {
        if (!cursor || cursor >= end_)
            return {0, 0};
        if (encoding_ == TextEncoding::Utf8
            || encoding_ == TextEncoding::Utf8Bom) {
            const uchar lead = *cursor;
            if (lead <= 0x7f) return {lead, 1};
            int width = lead >= 0xc2 && lead <= 0xdf ? 2
                : lead >= 0xe0 && lead <= 0xef ? 3
                : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
            if (cursor + width > end_) return {0xfffd, 1};
            char32_t value = lead & (0x7f >> width);
            for (int i = 1; i < width; ++i) {
                if ((cursor[i] & 0xc0) != 0x80) return {0xfffd, 1};
                value = (value << 6) | (cursor[i] & 0x3f);
            }
            const char32_t minimum = width == 2 ? 0x80
                : width == 3 ? 0x800 : 0x10000;
            if (value < minimum || value > 0x10ffff
                || (value >= 0xd800 && value <= 0xdfff)) {
                return {0xfffd, 1};
            }
            return {value, width};
        }
        if (encoding_ == TextEncoding::Utf16LE
            || encoding_ == TextEncoding::Utf16BE) {
            if (cursor + 2 > end_) return {0xfffd, 1};
            const auto readUnit = [&](const uchar *at) -> quint16 {
                return encoding_ == TextEncoding::Utf16LE
                    ? quint16(at[0]) | (quint16(at[1]) << 8)
                    : (quint16(at[0]) << 8) | quint16(at[1]);
            };
            const quint16 first = readUnit(cursor);
            if (first >= 0xd800 && first <= 0xdbff && cursor + 4 <= end_) {
                const quint16 second = readUnit(cursor + 2);
                if (second >= 0xdc00 && second <= 0xdfff) {
                    return {char32_t(0x10000
                            + ((first - 0xd800) << 10) + (second - 0xdc00)), 4};
                }
            }
            if (first >= 0xd800 && first <= 0xdfff) return {0xfffd, 2};
            return {first, 2};
        }

        quint32 codePage = 0;
        if (!windowsCodePageForEncoding(encoding_, &codePage))
            return {0xfffd, 1};
        int width = 1;
        const uchar lead = *cursor;
        if (encoding_ == TextEncoding::GBK && lead >= 0x81 && lead <= 0xfe)
            width = 2;
        else if (encoding_ == TextEncoding::GB18030
                 && lead >= 0x81 && lead <= 0xfe) {
            width = cursor + 4 <= end_ && cursor[1] >= 0x30 && cursor[1] <= 0x39
                    && cursor[2] >= 0x81 && cursor[2] <= 0xfe
                    && cursor[3] >= 0x30 && cursor[3] <= 0x39 ? 4 : 2;
        } else if (encoding_ == TextEncoding::Big5
                   && lead >= 0x81 && lead <= 0xfe) {
            width = 2;
        } else if (encoding_ == TextEncoding::ShiftJis
                   && ((lead >= 0x81 && lead <= 0x9f)
                       || (lead >= 0xe0 && lead <= 0xfc))) {
            width = 2;
        }
        if (cursor + width > end_) return {0xfffd, 1};
#ifdef Q_OS_WIN
        wchar_t decoded[2] = {};
        const int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                                        reinterpret_cast<const char *>(cursor),
                                        width, decoded, 2);
        if (count == 1)
            return {char32_t(decoded[0]), width};
        if (count == 2 && decoded[0] >= 0xd800 && decoded[0] <= 0xdbff
            && decoded[1] >= 0xdc00 && decoded[1] <= 0xdfff) {
            return {char32_t(0x10000 + ((decoded[0] - 0xd800) << 10)
                    + (decoded[1] - 0xdc00)), width};
        }
#endif
        return {0xfffd, 1};
    }

    void checkpoint() const
    {
        if (control_)
            control_->checkpoint(byteOffset());
    }

    const uchar *begin_ = nullptr;
    const uchar *position_ = nullptr;
    const uchar *end_ = nullptr;
    BoundaryAnchors *anchors_ = nullptr;
    const IterationControl *control_ = nullptr;
    TextEncoding encoding_ = TextEncoding::Utf8;
};

std::vector<char32_t> toScalarPattern(const QString &pattern)
{
    std::vector<char32_t> result;
    result.reserve(size_t(pattern.size()));
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const quint16 first = pattern.at(i).unicode();
        if (QChar::isHighSurrogate(first) && i + 1 < pattern.size()
            && QChar::isLowSurrogate(pattern.at(i + 1).unicode())) {
            result.push_back(QChar::surrogateToUcs4(
                first, pattern.at(++i).unicode()));
        } else if (QChar::isSurrogate(first)) {
            result.push_back(0xfffd);
        } else {
            result.push_back(first);
        }
    }
    return result;
}

} // namespace

struct RawCodePageRegex::Impl {
    boost::basic_regex<char32_t, UnicodeRegexTraits> expression;
    TextEncoding encoding = TextEncoding::Utf8;
    bool valid = false;
    std::unique_ptr<BoundaryAnchors> anchors;
    const uchar *anchorSubject = nullptr;
    qint64 anchorSubjectSize = 0;
};

RawCodePageRegex::RawCodePageRegex() : impl_(std::make_unique<Impl>()) {}
RawCodePageRegex::~RawCodePageRegex() = default;
RawCodePageRegex::RawCodePageRegex(RawCodePageRegex &&) noexcept = default;
RawCodePageRegex &RawCodePageRegex::operator=(RawCodePageRegex &&) noexcept = default;

bool RawCodePageRegex::compile(const QString &pattern, bool caseSensitive,
                               TextEncoding encoding, QString *error)
{
    impl_->valid = false;
    impl_->anchors.reset();
    impl_->anchorSubject = nullptr;
    impl_->anchorSubjectSize = 0;
    if (pattern.isEmpty()) {
        if (error)
            *error = QObject::tr("查找文本为空。");
        return false;
    }
#ifndef Q_OS_WIN
    quint32 codePage = 0;
    if (windowsCodePageForEncoding(encoding, &codePage)) {
        if (error)
            *error = QObject::tr("传统代码页正则后端当前仅支持 Windows。");
        return false;
    }
#else
    try {
        boost::regex_constants::syntax_option_type flags = boost::regex::perl;
        if (!caseSensitive)
            flags |= boost::regex::icase;
        const auto scalarPattern = toScalarPattern(pattern);
        impl_->expression.assign(scalarPattern.data(),
                                 scalarPattern.data() + scalarPattern.size(), flags);
        impl_->encoding = encoding;
        impl_->valid = true;
        return true;
    } catch (const boost::regex_error &failure) {
        if (error) {
            *error = QObject::tr("正则无效（字符 %1）：%2")
                         .arg(qulonglong(failure.position()))
                         .arg(QString::fromLocal8Bit(failure.what()));
        }
        return false;
    } catch (const std::exception &failure) {
        if (error)
            *error = QObject::tr("无法编译原始编码正则：%1")
                         .arg(QString::fromLocal8Bit(failure.what()));
        return false;
    }
#endif
}

bool RawCodePageRegex::isValid() const
{
    return impl_ && impl_->valid;
}

bool RawCodePageRegex::expandReplacementAt(
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
            *error = QObject::tr("原始编码正则替换的匹配范围无效。");
        return false;
    }
    try {
        const uchar *endBytes = subject + subjectSize;
        BoundaryAnchors localAnchors(subject);
        BoundaryAnchors *anchors = impl_->anchorSubject == subject
                && impl_->anchorSubjectSize == subjectSize
                && impl_->anchors
            ? impl_->anchors.get() : &localAnchors;
        IterationControl control;
        const EncodedIterator matchStart(
            subject, subject + matchOffset, endBytes, impl_->encoding, anchors, &control);
        const EncodedIterator subjectEnd(
            subject, endBytes, endBytes, impl_->encoding, anchors, &control);
        boost::match_results<EncodedIterator> match;
        auto flags = boost::match_continuous;
        if (matchOffset > 0)
            flags |= boost::match_prev_avail | boost::match_not_bob;
        if (!boost::regex_search(
                matchStart, subjectEnd, match, impl_->expression, flags)
            || match[0].first.byteOffset() != matchOffset
            || match[0].second.byteOffset() != matchOffset + matchLength) {
            if (error)
                *error = QObject::tr("正文已变化，当前范围不再是同一个正则匹配。");
            return false;
        }

        const auto appendCapture = [&](int number) -> bool {
            if (number < 0 || size_t(number) >= match.size()) {
                if (error)
                    *error = QObject::tr("替换文本引用了不存在的捕获组 %1。")
                                 .arg(number);
                return false;
            }
            const auto &capture = match[size_t(number)];
            if (!capture.matched)
                return true;
            const qint64 start = capture.first.byteOffset();
            const qint64 end = capture.second.byteOffset();
            if (start < 0 || end < start || end > subjectSize) {
                if (error)
                    *error = QObject::tr("捕获组 %1 的原始字节范围无效。")
                                 .arg(number);
                return false;
            }
            expanded->append(reinterpret_cast<const char *>(subject + start),
                             qsizetype(end - start));
            return true;
        };
        const auto captureNumber = [&](const QString &token, int *number) -> bool {
            bool numeric = false;
            const int parsed = token.toInt(&numeric);
            if (numeric) {
                *number = parsed;
                return true;
            }
            const auto name = toScalarPattern(token);
            const int resolved = match.named_subexpression_index(
                name.data(), name.data() + name.size());
            if (resolved < 0) {
                if (error) {
                    *error = QObject::tr("替换文本引用了不存在的命名捕获组“%1”。")
                                 .arg(token);
                }
                return false;
            }
            *number = resolved;
            return true;
        };
        const auto appendLiteral = [&](const QString &text) -> bool {
            QByteArray encoded;
            bool ok = true;
            encoded = encodeTextFragment(text, impl_->encoding, &ok);
            if (!ok) {
                if (error) {
                    *error = QObject::tr("替换文本包含 %1 无法表示的字符。")
                                 .arg(encodingName(impl_->encoding));
                }
                return false;
            }
            expanded->append(encoded);
            if (!encoded.isEmpty())
                encoded.fill(0);
            return true;
        };

        for (qsizetype i = 0; i < replacement.size(); ++i) {
            const QChar ch = replacement.at(i);
            if (ch == QLatin1Char('$') && i + 1 < replacement.size()) {
                const QChar next = replacement.at(i + 1);
                if (next == QLatin1Char('$')) {
                    if (!appendLiteral(QString(QLatin1Char('$'))))
                        return false;
                    ++i;
                    continue;
                }
                QString token;
                if (next == QLatin1Char('{')) {
                    const qsizetype close = replacement.indexOf(
                        QLatin1Char('}'), i + 2);
                    if (close < 0) {
                        if (error)
                            *error = QObject::tr("替换文本中的 ${...} 引用缺少右花括号。");
                        return false;
                    }
                    token = replacement.mid(i + 2, close - i - 2);
                    i = close;
                } else if (next.isDigit()) {
                    qsizetype end = i + 1;
                    while (end < replacement.size()
                           && replacement.at(end).isDigit()) {
                        ++end;
                    }
                    token = replacement.mid(i + 1, end - i - 1);
                    i = end - 1;
                } else {
                    if (!appendLiteral(QString(ch)))
                        return false;
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
                if (next.isDigit()) {
                    QString token(next);
                    while (i + 1 < replacement.size()
                           && replacement.at(i + 1).isDigit()) {
                        token.append(replacement.at(++i));
                    }
                    int number = -1;
                    if (!captureNumber(token, &number) || !appendCapture(number))
                        return false;
                } else if (next == QLatin1Char('n')) {
                    if (!appendLiteral(QString(QLatin1Char('\n')))) return false;
                } else if (next == QLatin1Char('r')) {
                    if (!appendLiteral(QString(QLatin1Char('\r')))) return false;
                } else if (next == QLatin1Char('t')) {
                    if (!appendLiteral(QString(QLatin1Char('\t')))) return false;
                } else if (next == QLatin1Char('\\')) {
                    if (!appendLiteral(QString(QLatin1Char('\\')))) return false;
                } else {
                    if (!appendLiteral(QString(QLatin1Char('\\')) + next))
                        return false;
                }
                continue;
            }
            if (!appendLiteral(QString(ch)))
                return false;
        }
        return true;
    } catch (const std::exception &failure) {
        if (error) {
            *error = QObject::tr("无法展开原始编码正则替换：%1")
                         .arg(QString::fromLocal8Bit(failure.what()));
        }
        return false;
    }
}

RawCodePageRegex::ScanResult RawCodePageRegex::scan(
    const uchar *subject, qint64 subjectSize,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(qint64, qint64)> &onMatch,
    QString *error) const
{
    if (!isValid() || !subject || subjectSize < 0
        || quint64(subjectSize) > quint64(std::numeric_limits<std::ptrdiff_t>::max())) {
        if (error)
            *error = QObject::tr("原始编码正则扫描器或输入无效。");
        return ScanResult::Failed;
    }
    if (isCancelled && isCancelled())
        return ScanResult::Cancelled;

    try {
        const uchar *endBytes = subject + subjectSize;
        impl_->anchors = std::make_unique<BoundaryAnchors>(subject);
        impl_->anchorSubject = subject;
        impl_->anchorSubjectSize = subjectSize;
        BoundaryAnchors *anchors = impl_->anchors.get();
        IterationControl control{&isCancelled, &reportProgress};
        EncodedIterator searchStart(subject, subject, endBytes, impl_->encoding,
                                    anchors, &control);
        const EncodedIterator searchEnd(subject, endBytes, endBytes,
                                        impl_->encoding, anchors, &control);
        boost::match_results<EncodedIterator> match;
        boost::regex_constants::match_flag_type flags = boost::match_default;
        bool retryNonEmptyAtSamePosition = false;
        bool searchedEndPosition = false;

        while (searchStart != searchEnd || !searchedEndPosition
               || retryNonEmptyAtSamePosition) {
            if (searchStart == searchEnd && !retryNonEmptyAtSamePosition) {
                if (searchedEndPosition)
                    break;
                searchedEndPosition = true;
            }
            auto currentFlags = flags;
            if (retryNonEmptyAtSamePosition)
                currentFlags |= boost::match_not_null | boost::match_continuous;
            const bool matched = boost::regex_search(
                searchStart, searchEnd, match, impl_->expression, currentFlags);
            if (!matched) {
                if (retryNonEmptyAtSamePosition) {
                    if (searchStart == searchEnd)
                        break;
                    ++searchStart;
                    retryNonEmptyAtSamePosition = false;
                    flags |= boost::match_prev_avail | boost::match_not_bob;
                    continue;
                }
                break;
            }

            const qint64 start = match[0].first.byteOffset();
            const qint64 end = match[0].second.byteOffset();
            if (start < 0 || end < start || end > subjectSize
                || end - start > std::numeric_limits<qint32>::max()) {
                if (error)
                    *error = QObject::tr("原始编码正则返回了无效或过大的字节范围。");
                return ScanResult::Failed;
            }
            if (onMatch && !onMatch(start, end - start))
                return ScanResult::Cancelled;
            if (reportProgress)
                reportProgress(end);

            searchStart = match[0].second;
            flags |= boost::match_prev_avail | boost::match_not_bob;
            retryNonEmptyAtSamePosition = start == end;
        }
        if (reportProgress)
            reportProgress(subjectSize);
        return ScanResult::Complete;
    } catch (const ScanCancelled &) {
        return ScanResult::Cancelled;
    } catch (const std::runtime_error &failure) {
        if (error) {
            *error = QObject::tr(
                "正则表达式达到安全资源上限：%1")
                         .arg(QString::fromLocal8Bit(failure.what()));
        }
        return ScanResult::Failed;
    } catch (const std::exception &failure) {
        if (error) {
            *error = QObject::tr("原始编码正则扫描失败：%1")
                         .arg(QString::fromLocal8Bit(failure.what()));
        }
        return ScanResult::Failed;
    }
}
