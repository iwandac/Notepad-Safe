#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdio>

#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"
#include "TextEncoding.h"

namespace {

int failures = 0;

const char *encodingLabel(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf16LE: return "UTF-16LE";
    case TextEncoding::Utf16BE: return "UTF-16BE";
    case TextEncoding::GBK: return "GBK";
    case TextEncoding::GB18030: return "GB18030";
    case TextEncoding::Big5: return "Big5";
    case TextEncoding::ShiftJis: return "Shift-JIS";
    case TextEncoding::Windows1252: return "Windows-1252";
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Utf8Bom: return "UTF-8 BOM";
    }
    return "unknown";
}

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

QVector<QPair<qint64, qint64>> scanEncoded(const QString &pattern,
                                           const QByteArray &subject,
                                           TextEncoding encoding,
                                           QString *error,
                                           bool caseSensitive = true)
{
    QVector<QPair<qint64, qint64>> matches;
    EncodedRegexCompiler::Output output;
    if (!EncodedRegexCompiler::compile(pattern, encoding, caseSensitive,
                                       &output, error))
        return matches;
    Pcre2Regex regex;
    if (!regex.compileBytePattern(output.bytePattern, output.caseSensitive, error))
        return matches;
    const auto *bytes = reinterpret_cast<const uchar *>(subject.constData());
    qint64 boundaryCursor = 0;
    const auto result = regex.scan(
        bytes, subject.size(), {}, {},
        [&](qint64 offset, qint64 length) {
            matches.append({offset, length});
            return true;
        }, error, {},
        [bytes, size = qint64(subject.size()), encoding](qint64 offset) {
            return EncodedRegexCompiler::nextUnitOffset(bytes, size, offset,
                                                        encoding);
        }, 0,
        [bytes, size = qint64(subject.size()), encoding,
         &boundaryCursor](qint64 candidate) {
            while (boundaryCursor < candidate) {
                boundaryCursor = EncodedRegexCompiler::nextUnitOffset(
                    bytes, size, boundaryCursor, encoding);
            }
            return boundaryCursor;
        });
    check(result == Pcre2Regex::ScanResult::Complete,
          "encoded PCRE2 scan completes");
    return matches;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;

    QByteArray subject(16 * 1024 * 1024 - 3, 'x');
    const qint64 expectedOffset = subject.size();
    subject += "BEGIN request=42\r\nEND status=ok";
    subject += QByteArray(4096, 'z');

    Pcre2Regex regex;
    check(regex.compile(QStringLiteral("BEGIN[^\\r\\n]*\\r?\\nEND status=ok"),
                        false, true, false, &error),
          "UTF-8 multiline expression compiles");
    check(regex.startByteFilter().asciiBytes == QByteArray("B"),
          "PCRE2 exposes the proven fixed ASCII start byte");
    Pcre2Regex caselessPrefix;
    check(caselessPrefix.compile(QStringLiteral("s3_regex_([0-9]+)_end"),
                                 false, false, false, &error),
          "caseless UTF-8 prefix expression compiles");
    const auto caselessFilter = caselessPrefix.startByteFilter();
    check(caselessFilter.asciiBytes.contains('s')
              && caselessFilter.asciiBytes.contains('S')
              && caselessFilter.anyNonAscii,
          "PCRE2 exposes every caseless ASCII and Unicode start candidate");
    qint64 offset = -1;
    qint64 length = -1;
    int count = 0;
    const auto result = regex.scan(
        reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
        {}, {},
        [&](qint64 foundOffset, qint64 foundLength) {
            offset = foundOffset;
            length = foundLength;
            ++count;
            return true;
        }, &error);
    check(result == Pcre2Regex::ScanResult::Complete, "UTF-8 scan completes");
    check(count == 1, "one UTF-8 cross-window multiline match");
    check(offset == expectedOffset, "UTF-8 match byte offset is exact");
    check(subject.mid(offset, length) == QByteArray("BEGIN request=42\r\nEND status=ok"),
          "UTF-8 match bytes span the newline exactly");

    const TextEncoding encodings[] = {
        TextEncoding::Utf16LE, TextEncoding::Utf16BE,
        TextEncoding::GBK, TextEncoding::GB18030, TextEncoding::Big5,
        TextEncoding::ShiftJis, TextEncoding::Windows1252,
    };
    for (TextEncoding encoding : encodings) {
        const QString text = encoding == TextEncoding::ShiftJis
            ? QStringLiteral("日本 ERROR id=42 日本 ERROR id=500")
            : encoding == TextEncoding::Windows1252
                ? QStringLiteral("café ERROR id=42 tail ERROR id=500")
                : QStringLiteral("中文 ERROR id=42 中文 ERROR id=500");
        bool representable = false;
        const QByteArray encoded = encodeTextFragment(text, encoding, &representable);
        if (!representable)
            std::fprintf(stderr, "fixture encoding failed: %s\n",
                         encodingLabel(encoding));
        check(representable, "cross-encoding fixture is representable");
        error.clear();
        const auto matches = scanEncoded(
            QStringLiteral("ERROR id=(4\\d|500)"), encoded, encoding, &error);
        check(error.isEmpty(), "encoded pattern has no error");
        check(matches.size() == 2, "encoded pattern finds both log records");
        if (matches.size() == 2) {
            bool firstOk = false;
            bool secondOk = false;
            const QByteArray first = encodeTextFragment(
                QStringLiteral("ERROR id=42"), encoding, &firstOk);
            const QByteArray second = encodeTextFragment(
                QStringLiteral("ERROR id=500"), encoding, &secondOk);
            check(firstOk && secondOk, "expected encoded matches are representable");
            check(encoded.mid(matches[0].first, matches[0].second) == first,
                  "first encoded raw byte range is exact");
            check(encoded.mid(matches[1].first, matches[1].second) == second,
                  "second encoded raw byte range is exact");
        }
    }

    bool gbOk = false;
    const QString gbText = QStringLiteral("甲中乙\n中文\nEND");
    const QByteArray gb = encodeTextFragment(gbText, TextEncoding::GB18030, &gbOk);
    check(gbOk, "GB18030 scalar fixture encodes");
    const auto scalarMatches = scanEncoded(
        QStringLiteral("."), gb, TextEncoding::GB18030, &error);
    check(scalarMatches.size() == gbText.size() - 2,
          "dot consumes complete GB18030 characters and excludes newlines");
    for (const auto &match : scalarMatches)
        check(match.second != 0, "encoded dot never publishes empty match");

    const auto zeroMatches = scanEncoded(
        QStringLiteral("(?=END)"), gb, TextEncoding::GB18030, &error);
    if (zeroMatches.size() != 1 || zeroMatches[0].second != 0) {
        std::fprintf(stderr, "zero-length: count=%lld error=%s\n",
                     static_cast<long long>(zeroMatches.size()),
                     error.toUtf8().constData());
        for (const auto &match : zeroMatches) {
            std::fprintf(stderr, "  offset=%lld length=%lld\n",
                         static_cast<long long>(match.first),
                         static_cast<long long>(match.second));
        }
    }
    check(zeroMatches.size() == 1 && zeroMatches[0].second == 0,
          "encoded zero-length lookahead terminates at one unit boundary");
    bool repeatedZeroOk = false;
    const QByteArray repeatedZeroSubject = encodeTextFragment(
        QStringLiteral("END END"), TextEncoding::GB18030, &repeatedZeroOk);
    const auto repeatedZeroMatches = scanEncoded(
        QStringLiteral("(?=END)"), repeatedZeroSubject,
        TextEncoding::GB18030, &error);
    check(repeatedZeroOk && repeatedZeroMatches.size() == 2
              && repeatedZeroMatches[0].first == 0
              && repeatedZeroMatches[1].first > repeatedZeroMatches[0].first,
          "encoded zero-length matching advances and finds later boundaries");

    bool utf16Ok = false;
    const QString utf16Text = QStringLiteral(
        "ERROR id=7\r\nXERROR id=8\r\nerror id=9\r\n中文");
    const QByteArray utf16 = encodeTextFragment(
        utf16Text, TextEncoding::Utf16LE, &utf16Ok);
    check(utf16Ok, "UTF-16LE feature fixture encodes");
    error.clear();
    const auto anchored = scanEncoded(
        QStringLiteral("(?im)^ERROR id=\\d+$"), utf16,
        TextEncoding::Utf16LE, &error);
    check(error.isEmpty() && anchored.size() == 2,
          "UTF-16 anchors, flags and ASCII classes preserve text semantics");
    const auto wholeWord = scanEncoded(
        QStringLiteral("(?<!\\w)(?:ERROR)(?!\\w)"), utf16,
        TextEncoding::Utf16LE, &error, false);
    check(error.isEmpty() && wholeWord.size() == 2,
          "encoded ASCII whole-word boundaries exclude embedded words");

    bool sjisOk = false;
    const QByteArray sjis = encodeTextFragment(
        QStringLiteral("日本 ABAB 試験 ABAC"), TextEncoding::ShiftJis,
        &sjisOk);
    check(sjisOk, "Shift-JIS capture fixture encodes");
    const auto captured = scanEncoded(
        QStringLiteral("(AB)\\1"), sjis, TextEncoding::ShiftJis, &error);
    check(error.isEmpty() && captured.size() == 1,
          "encoded capture and numeric backreference work");
    const auto lookbehind = scanEncoded(
        QStringLiteral("(?<=AB)AB"), sjis, TextEncoding::ShiftJis, &error);
    check(error.isEmpty() && lookbehind.size() == 1,
          "fixed ASCII lookbehind works in a DBCS subject");

    QByteArray distant(2 * 1024 * 1024 + 3, 'x');
    distant += "ERROR id=77";
    const auto distantMatches = scanEncoded(
        QStringLiteral("ERROR id=\\d+"), distant,
        TextEncoding::Windows1252, &error);
    check(error.isEmpty() && distantMatches.size() == 1
              && distantMatches[0].first == 2 * 1024 * 1024 + 3,
          "encoded scanner remains exact beyond an offset-progress window");

    EncodedRegexCompiler::Output rejected;
    check(!EncodedRegexCompiler::compile(
              QStringLiteral("\\p{Han}+"), TextEncoding::GBK, true,
              &rejected, &error),
          "Unicode properties are rejected for non-UTF subjects");
    check(error.contains(QStringLiteral("不支持")),
          "unsupported property has a short diagnostic");
    check(!EncodedRegexCompiler::compile(
              QStringLiteral("[一-龥]"), TextEncoding::GB18030, true,
              &rejected, &error),
          "non-ASCII ranges are rejected");
    check(!EncodedRegexCompiler::compile(
              QStringLiteral("(?<=中文)ERROR"), TextEncoding::GBK, true,
              &rejected, &error),
          "multibyte lookbehind is rejected");

    Pcre2Regex zeroLength;
    check(zeroLength.compile(QStringLiteral("(?=END)"), false, true, false, &error),
          "UTF-8 zero-length expression compiles");
    int zeroCount = 0;
    check(zeroLength.scan(
              reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
              {}, {},
              [&](qint64, qint64 foundLength) {
                  check(foundLength == 0, "UTF-8 lookahead publishes zero length");
                  ++zeroCount;
                  return true;
              }, &error) == Pcre2Regex::ScanResult::Complete,
          "UTF-8 zero-length scan terminates");
    check(zeroCount == 1, "UTF-8 zero-length scan does not loop");

    std::atomic<bool> cancelled{true};
    check(regex.scan(
              reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
              [&] { return cancelled.load(); }, {}, {}, &error)
              == Pcre2Regex::ScanResult::Cancelled,
          "pre-cancelled scan exits without matching");

    std::fprintf(stderr, "%s (%d failure(s))\n",
                 failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
