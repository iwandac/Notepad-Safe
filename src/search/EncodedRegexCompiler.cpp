#include "EncodedRegexCompiler.h"

#include <QChar>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

constexpr qsizetype MaximumGeneratedPatternBytes = 256 * 1024;

QByteArray hexByte(uchar value)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    QByteArray result("\\x00", 4);
    result[2] = digits[value >> 4];
    result[3] = digits[value & 0x0f];
    return result;
}

QByteArray escapedBytes(const QByteArray &bytes)
{
    QByteArray result;
    result.reserve(bytes.size() * 4);
    for (uchar value : bytes)
        result += hexByte(value);
    return result;
}

QString scalarString(char32_t scalar)
{
    const uint value = uint(scalar);
    return QString::fromUcs4(&value, 1);
}

bool nextScalar(const QString &text, int *position, char32_t *scalar)
{
    if (!position || !scalar || *position < 0 || *position >= text.size())
        return false;
    const QChar first = text.at(*position);
    if (first.isHighSurrogate() && *position + 1 < text.size()
        && text.at(*position + 1).isLowSurrogate()) {
        *scalar = char32_t(QChar::surrogateToUcs4(first, text.at(*position + 1)));
        *position += 2;
    } else {
        *scalar = char32_t(first.unicode());
        ++*position;
    }
    return true;
}

QByteArray encodedScalar(char32_t scalar, TextEncoding encoding, bool *ok)
{
    bool encodedExactly = false;
    QByteArray bytes = encodeTextFragment(scalarString(scalar), encoding,
                                          &encodedExactly);
    if (ok)
        *ok = encodedExactly;
    return bytes;
}

QByteArray validDbcsTrail(const QByteArray &ranges)
{
    return QByteArrayLiteral("(?:") + ranges + QByteArrayLiteral(")");
}

QByteArray encodedUnitPattern(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Windows1252:
        return QByteArrayLiteral("[\\x00-\\xFF]");
    case TextEncoding::GBK:
        return QByteArrayLiteral(
            "(?>[\\x81-\\xFE](?:[\\x40-\\x7E]|[\\x80-\\xFE])|[\\x00-\\xFF])");
    case TextEncoding::GB18030:
        return QByteArrayLiteral(
            "(?>[\\x81-\\xFE][\\x30-\\x39][\\x81-\\xFE][\\x30-\\x39]"
            "|[\\x81-\\xFE](?:[\\x40-\\x7E]|[\\x80-\\xFE])|[\\x00-\\xFF])");
    case TextEncoding::Big5:
        return QByteArrayLiteral(
            "(?>[\\x81-\\xFE](?:[\\x40-\\x7E]|[\\xA1-\\xFE])|[\\x00-\\xFF])");
    case TextEncoding::ShiftJis:
        return QByteArrayLiteral(
            "(?>(?:[\\x81-\\x9F]|[\\xE0-\\xFC])"
            "(?:[\\x40-\\x7E]|[\\x80-\\xFC])|[\\x00-\\xFF])");
    case TextEncoding::Utf16LE:
        return QByteArrayLiteral(
            "(?>[\\x00-\\xFF][\\xD8-\\xDB][\\x00-\\xFF][\\xDC-\\xDF]"
            "|[\\x00-\\xFF]{2}|[\\x00-\\xFF])");
    case TextEncoding::Utf16BE:
        return QByteArrayLiteral(
            "(?>[\\xD8-\\xDB][\\x00-\\xFF][\\xDC-\\xDF][\\x00-\\xFF]"
            "|[\\x00-\\xFF]{2}|[\\x00-\\xFF])");
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        break;
    }
    return {};
}

int encodedUnitWidth(const uchar *subject, qint64 remaining,
                     TextEncoding encoding)
{
    if (!subject || remaining <= 0)
        return 0;
    const uchar first = subject[0];
    switch (encoding) {
    case TextEncoding::Windows1252:
        return 1;
    case TextEncoding::GBK:
        if (remaining >= 2 && first >= 0x81 && first <= 0xfe
            && ((subject[1] >= 0x40 && subject[1] <= 0x7e)
                || (subject[1] >= 0x80 && subject[1] <= 0xfe)))
            return 2;
        return 1;
    case TextEncoding::GB18030:
        if (remaining >= 4 && first >= 0x81 && first <= 0xfe
            && subject[1] >= 0x30 && subject[1] <= 0x39
            && subject[2] >= 0x81 && subject[2] <= 0xfe
            && subject[3] >= 0x30 && subject[3] <= 0x39)
            return 4;
        if (remaining >= 2 && first >= 0x81 && first <= 0xfe
            && ((subject[1] >= 0x40 && subject[1] <= 0x7e)
                || (subject[1] >= 0x80 && subject[1] <= 0xfe)))
            return 2;
        return 1;
    case TextEncoding::Big5:
        if (remaining >= 2 && first >= 0x81 && first <= 0xfe
            && ((subject[1] >= 0x40 && subject[1] <= 0x7e)
                || (subject[1] >= 0xa1 && subject[1] <= 0xfe)))
            return 2;
        return 1;
    case TextEncoding::ShiftJis:
        if (remaining >= 2
            && ((first >= 0x81 && first <= 0x9f)
                || (first >= 0xe0 && first <= 0xfc))
            && ((subject[1] >= 0x40 && subject[1] <= 0x7e)
                || (subject[1] >= 0x80 && subject[1] <= 0xfc)))
            return 2;
        return 1;
    case TextEncoding::Utf16LE:
        if (remaining < 2)
            return 1;
        if (remaining >= 4 && subject[1] >= 0xd8 && subject[1] <= 0xdb
            && subject[3] >= 0xdc && subject[3] <= 0xdf)
            return 4;
        return 2;
    case TextEncoding::Utf16BE:
        if (remaining < 2)
            return 1;
        if (remaining >= 4 && first >= 0xd8 && first <= 0xdb
            && subject[2] >= 0xdc && subject[2] <= 0xdf)
            return 4;
        return 2;
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        return 1;
    }
    return 1;
}

class Compiler
{
public:
    Compiler(const QString &pattern, TextEncoding encoding, bool caseSensitive)
        : pattern_(pattern), encoding_(encoding), caseSensitive_(caseSensitive)
    {
        unit_ = encodedUnitPattern(encoding_);
        bool ok = false;
        lf_ = escapedBytes(encodedScalar(U'\n', encoding_, &ok));
        if (!ok)
            lf_.clear();
        cr_ = escapedBytes(encodedScalar(U'\r', encoding_, &ok));
        if (!ok)
            cr_.clear();
    }

    bool run(EncodedRegexCompiler::Output *output, QString *error)
    {
        if (!output || pattern_.isEmpty())
            return fail(QObject::tr("正则表达式为空。"), error);
        if (unit_.isEmpty())
            return fail(QObject::tr("当前编码不需要字节正则适配。"), error);
        if (!parseLeadingFlags(error))
            return false;
        const QString requiredPrefix = leadingAsciiLiteral();
        QByteArray translated;
        if (!parseSequence(&translated, false, error))
            return false;
        if (position_ != pattern_.size())
            return failAt(QObject::tr("缺少匹配的括号。"), position_, error);

        // Keep the user's expression free of a whole-subject scanner prefix.
        // LargeFileSearch validates each PCRE2 candidate against a monotonic
        // source-character boundary cursor. Prefixing the expression with a
        // scan-from-BOF atom would make a distant first match interpret every
        // preceding character inside one backtracking match and is unusable
        // for multi-GiB logs.
        QByteArray generated = QByteArrayLiteral("(?:") + translated
            + QByteArrayLiteral(")");
        if (generated.size() > MaximumGeneratedPatternBytes)
            return fail(QObject::tr("正则展开后过大；请缩小字符类或简化表达式。"), error);
        output->bytePattern = std::move(generated);
        // Case variants are expanded per scalar above. Enabling PCRE2's
        // single-byte caseless table here could fold a multibyte trail byte
        // and produce a match that is invalid in the source encoding.
        output->caseSensitive = true;
        output->requiredLiteral.clear();
        output->requiredLiteralCaseSensitive = caseSensitive_;
        if (requiredPrefix.size() >= 3) {
            bool prefixOk = false;
            output->requiredLiteral = encodeTextFragment(
                requiredPrefix, encoding_, &prefixOk);
            if (!prefixOk)
                output->requiredLiteral.clear();
        }
        return true;
    }

private:
    QString leadingAsciiLiteral() const
    {
        QString prefix;
        int at = position_;
        if (at < pattern_.size() && pattern_.at(at) == QLatin1Char('^'))
            ++at;
        else if (pattern_.mid(at, 2) == QStringLiteral("\\A"))
            at += 2;

        while (at < pattern_.size()) {
            QChar value = pattern_.at(at);
            if (value == QLatin1Char('\\')) {
                if (at + 1 >= pattern_.size())
                    break;
                const QChar escaped = pattern_.at(at + 1);
                if (QStringLiteral("\\.^$|()[]{}*+?-=!:<>").contains(escaped)) {
                    value = escaped;
                    at += 2;
                } else if (escaped == QLatin1Char('n')
                           || escaped == QLatin1Char('r')
                           || escaped == QLatin1Char('t')) {
                    value = escaped == QLatin1Char('n') ? QLatin1Char('\n')
                        : escaped == QLatin1Char('r') ? QLatin1Char('\r')
                                                     : QLatin1Char('\t');
                    at += 2;
                } else {
                    break;
                }
            } else {
                if (value.unicode() > 0x7f
                    || QStringLiteral(".^$|()[]{}*+?").contains(value)) {
                    break;
                }
                ++at;
            }
            prefix.append(value);
            if (at < pattern_.size()
                && QStringLiteral("*+?{").contains(pattern_.at(at))) {
                prefix.chop(1);
                break;
            }
        }
        return prefix;
    }

    bool fail(const QString &message, QString *error)
    {
        if (error)
            *error = message;
        return false;
    }

    bool failAt(const QString &message, int at, QString *error)
    {
        if (error)
            *error = QObject::tr("第 %1 个字符附近：%2").arg(at + 1).arg(message);
        return false;
    }

    bool parseLeadingFlags(QString *error)
    {
        while (position_ + 3 < pattern_.size()
               && pattern_.at(position_) == QLatin1Char('(')
               && pattern_.at(position_ + 1) == QLatin1Char('?')) {
            const QChar construct = pattern_.at(position_ + 2);
            if (construct == QLatin1Char('=') || construct == QLatin1Char('!')
                || construct == QLatin1Char('<') || construct == QLatin1Char(':')) {
                break;
            }
            const int close = pattern_.indexOf(QLatin1Char(')'), position_ + 2);
            if (close < 0)
                return failAt(QObject::tr("缺少 `)`。"), position_, error);
            const QString flags = pattern_.mid(position_ + 2,
                                               close - position_ - 2);
            if (flags.isEmpty() || flags.contains(QLatin1Char(':'))
                || flags.contains(QLatin1Char('-')))
                break;
            for (QChar flag : flags) {
                if (flag == QLatin1Char('i'))
                    caseSensitive_ = false;
                else if (flag == QLatin1Char('m'))
                    multiline_ = true;
                else if (flag == QLatin1Char('s'))
                    dotAll_ = true;
                else
                    return failAt(
                        QObject::tr("当前编码不支持内联选项 `%1`。").arg(flag),
                        position_, error);
            }
            position_ = close + 1;
        }
        return true;
    }

    bool parseSequence(QByteArray *out, bool stopAtClose, QString *error)
    {
        while (position_ < pattern_.size()) {
            const QChar ch = pattern_.at(position_);
            if (ch == QLatin1Char(')')) {
                if (!stopAtClose)
                    return failAt(QObject::tr("多余的 `)`。"), position_, error);
                return true;
            }
            if (ch == QLatin1Char('(')) {
                if (!parseGroup(out, error))
                    return false;
            } else if (ch == QLatin1Char('[')) {
                if (!parseClass(out, error))
                    return false;
            } else if (ch == QLatin1Char('\\')) {
                if (!parseEscape(out, error))
                    return false;
            } else if (ch == QLatin1Char('.')) {
                ++position_;
                *out += dotAll_ ? unit_ : QByteArrayLiteral("(?!") + lf_
                    + QByteArrayLiteral(")") + unit_;
            } else if (ch == QLatin1Char('^')) {
                ++position_;
                *out += multiline_
                    ? QByteArrayLiteral("(?:\\A|(?<=") + lf_ + QByteArrayLiteral("))")
                    : QByteArrayLiteral("\\A");
            } else if (ch == QLatin1Char('$')) {
                ++position_;
                const QByteArray newline = QByteArrayLiteral("(?:(?:") + cr_
                    + QByteArrayLiteral(")?") + lf_ + QByteArrayLiteral(")");
                *out += multiline_
                    ? QByteArrayLiteral("(?=\\z|") + newline + QByteArrayLiteral(")")
                    : QByteArrayLiteral("(?=\\z|") + newline + QByteArrayLiteral("\\z)");
            } else if (ch == QLatin1Char('{')) {
                const int close = pattern_.indexOf(QLatin1Char('}'), position_ + 1);
                if (close < 0)
                    return failAt(QObject::tr("量词缺少 `}`。"), position_, error);
                const QString body = pattern_.mid(position_ + 1,
                                                  close - position_ - 1);
                for (QChar value : body) {
                    if (!value.isDigit() && value != QLatin1Char(','))
                        return failAt(QObject::tr("量词只允许数字和逗号。"),
                                      position_, error);
                }
                out->append(pattern_.mid(position_, close - position_ + 1).toLatin1());
                position_ = close + 1;
            } else if (QStringLiteral("*+?|").contains(ch)) {
                out->append(char(ch.unicode()));
                ++position_;
            } else {
                char32_t scalar = 0;
                const int at = position_;
                if (!nextScalar(pattern_, &position_, &scalar)
                    || !appendLiteralScalar(out, scalar, at, error))
                    return false;
            }
            if (out->size() > MaximumGeneratedPatternBytes)
                return fail(QObject::tr("正则展开后过大。"), error);
        }
        if (stopAtClose)
            return failAt(QObject::tr("缺少 `)`。"), position_, error);
        return true;
    }

    bool appendLiteralScalar(QByteArray *out, char32_t scalar, int at,
                             QString *error)
    {
        QSet<QByteArray> variants;
        const auto add = [&](char32_t value) -> bool {
            bool ok = false;
            const QByteArray bytes = encodedScalar(value, encoding_, &ok);
            if (!ok)
                return false;
            variants.insert(escapedBytes(bytes));
            return true;
        };
        if (!add(scalar)) {
            return failAt(QObject::tr("字符 U+%1 无法用当前编码表示。")
                              .arg(uint(scalar), 4, 16, QLatin1Char('0')).toUpper(),
                          at, error);
        }
        if (!caseSensitive_) {
            add(char32_t(QChar::toLower(uint(scalar))));
            add(char32_t(QChar::toUpper(uint(scalar))));
            add(char32_t(QChar::toCaseFolded(uint(scalar))));
        }
        if (variants.size() == 1) {
            *out += *variants.constBegin();
        } else {
            *out += QByteArrayLiteral("(?:");
            bool first = true;
            for (const QByteArray &variant : variants) {
                if (!first)
                    out->append('|');
                *out += variant;
                first = false;
            }
            *out += ')';
        }
        return true;
    }

    bool parseGroup(QByteArray *out, QString *error)
    {
        const int groupStart = position_++;
        QByteArray prefix("(");
        if (position_ < pattern_.size() && pattern_.at(position_) == QLatin1Char('?')) {
            if (pattern_.mid(position_, 2) == QStringLiteral("?:")
                || pattern_.mid(position_, 2) == QStringLiteral("?=")
                || pattern_.mid(position_, 2) == QStringLiteral("?!")) {
                prefix += pattern_.mid(position_, 2).toLatin1();
                position_ += 2;
            } else if (pattern_.mid(position_, 3) == QStringLiteral("?<=")
                       || pattern_.mid(position_, 3) == QStringLiteral("?<!")) {
                const bool positive = pattern_.at(position_ + 2) == QLatin1Char('=');
                position_ += 3;
                QByteArray body;
                while (position_ < pattern_.size()
                       && pattern_.at(position_) != QLatin1Char(')')) {
                    const QChar value = pattern_.at(position_);
                    if (value.unicode() > 0x7f
                        || QStringLiteral("|*+?{}[]()").contains(value)) {
                        return failAt(QObject::tr(
                            "当前编码的后瞻只支持固定长度 ASCII 文本。"),
                            position_, error);
                    }
                    if (value == QLatin1Char('\\')) {
                        if (position_ + 1 >= pattern_.size())
                            return failAt(QObject::tr("转义不完整。"), position_, error);
                        const QChar escaped = pattern_.at(position_ + 1);
                        if (escaped == QLatin1Char('w')) {
                            body += words();
                            position_ += 2;
                            continue;
                        }
                        if (!QStringLiteral("\\.^$|()[]{}*+?-=!:<>").contains(escaped))
                            return failAt(QObject::tr(
                                "后瞻中只允许转义 ASCII 标点。"), position_, error);
                        if (!appendLiteralScalar(&body, escaped.unicode(), position_, error))
                            return false;
                        position_ += 2;
                    } else {
                        if (!appendLiteralScalar(&body, value.unicode(), position_, error))
                            return false;
                        ++position_;
                    }
                }
                if (position_ >= pattern_.size())
                    return failAt(QObject::tr("后瞻缺少 `)`。"), groupStart, error);
                ++position_;
                *out += positive ? QByteArrayLiteral("(?<=")
                                 : QByteArrayLiteral("(?<!");
                *out += body;
                *out += ')';
                return true;
            } else if (pattern_.mid(position_, 2) == QStringLiteral("?<")) {
                const int nameEnd = pattern_.indexOf(QLatin1Char('>'), position_ + 2);
                if (nameEnd < 0)
                    return failAt(QObject::tr("命名捕获缺少 `>`。"), groupStart, error);
                const QString name = pattern_.mid(position_ + 2,
                                                  nameEnd - position_ - 2);
                if (name.isEmpty())
                    return failAt(QObject::tr("捕获组名称为空。"), groupStart, error);
                for (QChar value : name) {
                    if (!(value.isLetterOrNumber() || value == QLatin1Char('_'))
                        || value.unicode() > 0x7f)
                        return failAt(QObject::tr("捕获组名称只允许 ASCII 字母、数字和下划线。"),
                                      groupStart, error);
                }
                prefix += QByteArrayLiteral("?<") + name.toLatin1() + '>';
                position_ = nameEnd + 1;
            } else {
                return failAt(QObject::tr("当前编码不支持这个分组结构。"),
                              groupStart, error);
            }
        }
        *out += prefix;
        if (!parseSequence(out, true, error))
            return false;
        ++position_; // closing ')'
        *out += ')';
        return true;
    }

    bool parseClassScalar(char32_t *scalar, QString *error)
    {
        if (position_ >= pattern_.size())
            return false;
        if (pattern_.at(position_) != QLatin1Char('\\'))
            return nextScalar(pattern_, &position_, scalar);
        const int at = position_++;
        if (position_ >= pattern_.size())
            return failAt(QObject::tr("字符类中的转义不完整。"), at, error);
        const QChar escaped = pattern_.at(position_++);
        if (escaped == QLatin1Char('n')) *scalar = U'\n';
        else if (escaped == QLatin1Char('r')) *scalar = U'\r';
        else if (escaped == QLatin1Char('t')) *scalar = U'\t';
        else if (QStringLiteral("\\-]^.").contains(escaped))
            *scalar = escaped.unicode();
        else
            return failAt(QObject::tr("字符类中不支持 `\\%1`。需使用显式字符。")
                              .arg(escaped), at, error);
        return true;
    }

    bool parseClass(QByteArray *out, QString *error)
    {
        const int classStart = position_++;
        bool negated = false;
        if (position_ < pattern_.size() && pattern_.at(position_) == QLatin1Char('^')) {
            negated = true;
            ++position_;
        }
        QVector<char32_t> scalars;
        bool firstItem = true;
        while (position_ < pattern_.size()
               && (pattern_.at(position_) != QLatin1Char(']') || firstItem)) {
            char32_t first = 0;
            const int itemAt = position_;
            if (!parseClassScalar(&first, error))
                return false;
            firstItem = false;
            if (position_ + 1 < pattern_.size()
                && pattern_.at(position_) == QLatin1Char('-')
                && pattern_.at(position_ + 1) != QLatin1Char(']')) {
                ++position_;
                char32_t last = 0;
                if (!parseClassScalar(&last, error))
                    return false;
                if (first > 0x7f || last > 0x7f || last < first)
                    return failAt(QObject::tr("当前编码只支持 ASCII 字符范围。"),
                                  itemAt, error);
                if (last - first > 128)
                    return failAt(QObject::tr("字符范围过大。"), itemAt, error);
                for (char32_t value = first; value <= last; ++value)
                    scalars.append(value);
            } else {
                scalars.append(first);
            }
            if (scalars.size() > 256)
                return failAt(QObject::tr("字符类项目过多。"), classStart, error);
        }
        if (position_ >= pattern_.size())
            return failAt(QObject::tr("字符类缺少 `]`。"), classStart, error);
        ++position_;
        if (scalars.isEmpty())
            return failAt(QObject::tr("空字符类不受支持。"), classStart, error);

        QByteArray positive("(?:");
        bool firstAlternative = true;
        QSet<QByteArray> emitted;
        for (char32_t scalar : scalars) {
            QVector<char32_t> variants{scalar};
            if (!caseSensitive_) {
                variants.append(char32_t(QChar::toLower(uint(scalar))));
                variants.append(char32_t(QChar::toUpper(uint(scalar))));
                variants.append(char32_t(QChar::toCaseFolded(uint(scalar))));
            }
            for (char32_t variant : variants) {
                bool ok = false;
                const QByteArray bytes = encodedScalar(variant, encoding_, &ok);
                if (!ok)
                    continue;
                const QByteArray encoded = escapedBytes(bytes);
                if (emitted.contains(encoded))
                    continue;
                emitted.insert(encoded);
                if (!firstAlternative)
                    positive += '|';
                positive += encoded;
                firstAlternative = false;
            }
        }
        positive += ')';
        if (firstAlternative)
            return failAt(QObject::tr("字符类在当前编码中没有可表示字符。"),
                          classStart, error);
        if (negated)
            *out += QByteArrayLiteral("(?!") + positive + QByteArrayLiteral(")") + unit_;
        else
            *out += positive;
        return true;
    }

    QByteArray scalarSet(const QVector<char32_t> &scalars)
    {
        QByteArray result("(?:");
        bool first = true;
        QSet<QByteArray> unique;
        for (char32_t scalar : scalars) {
            bool ok = false;
            const QByteArray item = escapedBytes(encodedScalar(scalar, encoding_, &ok));
            if (!ok || unique.contains(item))
                continue;
            unique.insert(item);
            if (!first)
                result += '|';
            result += item;
            first = false;
        }
        result += ')';
        return result;
    }

    QByteArray digits() { QVector<char32_t> v; for (char32_t c=U'0';c<=U'9';++c)v<<c; return scalarSet(v); }
    QByteArray spaces() { return scalarSet({U'\t',U'\n',U'\v',U'\f',U'\r',U' '}); }
    QByteArray words()
    {
        QVector<char32_t> v;
        for (char32_t c=U'0';c<=U'9';++c)v<<c;
        for (char32_t c=U'A';c<=U'Z';++c)v<<c;
        for (char32_t c=U'a';c<=U'z';++c)v<<c;
        v<<U'_';
        return scalarSet(v);
    }

    bool parseHexScalar(char32_t *scalar, int escapeAt, QString *error)
    {
        if (position_ < pattern_.size() && pattern_.at(position_) == QLatin1Char('{')) {
            const int close = pattern_.indexOf(QLatin1Char('}'), position_ + 1);
            if (close < 0)
                return failAt(QObject::tr("十六进制转义缺少 `}`。"), escapeAt, error);
            bool ok = false;
            const uint value = pattern_.mid(position_ + 1, close - position_ - 1)
                                   .toUInt(&ok, 16);
            if (!ok || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
                return failAt(QObject::tr("Unicode 转义无效。"), escapeAt, error);
            *scalar = char32_t(value);
            position_ = close + 1;
            return true;
        }
        if (position_ + 2 > pattern_.size())
            return failAt(QObject::tr("`\\x` 后需要两个十六进制数字。"), escapeAt, error);
        bool ok = false;
        const uint value = pattern_.mid(position_, 2).toUInt(&ok, 16);
        if (!ok)
            return failAt(QObject::tr("十六进制转义无效。"), escapeAt, error);
        if (value > 0x7f)
            return failAt(QObject::tr("非 ASCII 字符请使用 `\\x{...}` 或直接输入字符。"),
                          escapeAt, error);
        *scalar = char32_t(value);
        position_ += 2;
        return true;
    }

    bool parseEscape(QByteArray *out, QString *error)
    {
        const int escapeAt = position_++;
        if (position_ >= pattern_.size())
            return failAt(QObject::tr("转义不完整。"), escapeAt, error);
        const QChar escaped = pattern_.at(position_++);
        if (escaped == QLatin1Char('d') || escaped == QLatin1Char('D')
            || escaped == QLatin1Char('s') || escaped == QLatin1Char('S')
            || escaped == QLatin1Char('w') || escaped == QLatin1Char('W')) {
            const bool negative = escaped.isUpper();
            const QByteArray positive = escaped.toLower() == QLatin1Char('d')
                ? digits() : escaped.toLower() == QLatin1Char('s') ? spaces() : words();
            *out += negative
                ? QByteArrayLiteral("(?!") + positive + QByteArrayLiteral(")") + unit_
                : positive;
            return true;
        }
        if (escaped == QLatin1Char('b') || escaped == QLatin1Char('B')) {
            const QByteArray word = words();
            if (escaped == QLatin1Char('b')) {
                *out += QByteArrayLiteral("(?:(?<!") + word + QByteArrayLiteral(")(?=")
                    + word + QByteArrayLiteral(")|(?<=") + word
                    + QByteArrayLiteral(")(?!") + word + QByteArrayLiteral("))");
            } else {
                *out += QByteArrayLiteral("(?:(?<=") + word + QByteArrayLiteral(")(?=")
                    + word + QByteArrayLiteral(")|(?<!") + word
                    + QByteArrayLiteral(")(?!") + word + QByteArrayLiteral("))");
            }
            return true;
        }
        if (escaped == QLatin1Char('n') || escaped == QLatin1Char('r')
            || escaped == QLatin1Char('t')) {
            const char32_t scalar = escaped == QLatin1Char('n') ? U'\n'
                : escaped == QLatin1Char('r') ? U'\r' : U'\t';
            return appendLiteralScalar(out, scalar, escapeAt, error);
        }
        if (escaped == QLatin1Char('R')) {
            *out += QByteArrayLiteral("(?:") + cr_ + lf_ + '|' + lf_ + '|'
                + cr_ + ')';
            return true;
        }
        if (escaped == QLatin1Char('x')) {
            char32_t scalar = 0;
            return parseHexScalar(&scalar, escapeAt, error)
                && appendLiteralScalar(out, scalar, escapeAt, error);
        }
        if (escaped == QLatin1Char('A') || escaped == QLatin1Char('z')) {
            *out += '\\';
            out->append(char(escaped.unicode()));
            return true;
        }
        if (escaped.isDigit() && escaped != QLatin1Char('0')) {
            out->append('\\');
            out->append(char(escaped.unicode()));
            while (position_ < pattern_.size() && pattern_.at(position_).isDigit())
                out->append(char(pattern_.at(position_++).unicode()));
            return true;
        }
        if (escaped == QLatin1Char('k')) {
            if (position_ >= pattern_.size()
                || pattern_.at(position_) != QLatin1Char('<'))
                return failAt(QObject::tr("命名引用必须使用 `\\k<name>`。"), escapeAt, error);
            const int close = pattern_.indexOf(QLatin1Char('>'), position_ + 1);
            if (close < 0)
                return failAt(QObject::tr("命名引用缺少 `>`。"), escapeAt, error);
            *out += pattern_.mid(escapeAt, close - escapeAt + 1).toLatin1();
            position_ = close + 1;
            return true;
        }
        if (QStringLiteral("\\.^$|()[]{}*+?-=").contains(escaped))
            return appendLiteralScalar(out, escaped.unicode(), escapeAt, error);
        if (QStringLiteral("pPXCKgGZNQEhHvV").contains(escaped))
            return failAt(QObject::tr("当前编码不支持 `\\%1`。").arg(escaped),
                          escapeAt, error);
        return failAt(QObject::tr("不支持的转义 `\\%1`。").arg(escaped),
                      escapeAt, error);
    }

    const QString &pattern_;
    TextEncoding encoding_;
    bool caseSensitive_ = true;
    bool multiline_ = false;
    bool dotAll_ = false;
    int position_ = 0;
    QByteArray unit_;
    QByteArray lf_;
    QByteArray cr_;
};

} // namespace

bool EncodedRegexCompiler::compile(const QString &pattern,
                                   TextEncoding encoding,
                                   bool caseSensitive, Output *output,
                                   QString *error)
{
    if (output)
        *output = {};
    Compiler compiler(pattern, encoding, caseSensitive);
    return compiler.run(output, error);
}

qint64 EncodedRegexCompiler::nextUnitOffset(const uchar *subject,
                                            qint64 subjectSize,
                                            qint64 offset,
                                            TextEncoding encoding)
{
    if (!subject || offset < 0 || offset >= subjectSize)
        return subjectSize + 1;
    return offset + encodedUnitWidth(subject + offset, subjectSize - offset,
                                     encoding);
}

bool EncodedRegexCompiler::isUtf8SubjectEncoding(TextEncoding encoding)
{
    return encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf8Bom;
}
