#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <atomic>
#include <cstdio>

#include "Pcre2Regex.h"

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QByteArray subject(16 * 1024 * 1024 - 3, 'x');
    const qint64 expectedOffset = subject.size();
    subject += "BEGIN request=42\r\nEND status=ok";
    subject += QByteArray(4096, 'z');

    Pcre2Regex regex;
    QString error;
    check(regex.compile(QStringLiteral("BEGIN[^\\r\\n]*\\r?\\nEND status=ok"),
                        false, true, false, &error),
          "multiline expression compiles");
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
    check(result == Pcre2Regex::ScanResult::Complete, "scan completes");
    check(count == 1, "one cross-window multiline match");
    check(offset == expectedOffset, "match byte offset is exact");
    check(subject.mid(offset, length) == QByteArray("BEGIN request=42\r\nEND status=ok"),
          "match bytes span the newline exactly");

    Pcre2Regex zeroLength;
    check(zeroLength.compile(QStringLiteral("(?=END)"), false, true, false, &error),
          "zero-length expression compiles");
    int zeroCount = 0;
    check(zeroLength.scan(
              reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
              {}, {},
              [&](qint64, qint64 foundLength) {
                  check(foundLength == 0, "lookahead publishes a zero-length match");
                  ++zeroCount;
                  return true;
              }, &error) == Pcre2Regex::ScanResult::Complete,
          "zero-length scan terminates");
    check(zeroCount == 1, "zero-length scan does not loop");

    const QByteArray captureSubject("name=alice id=42");
    Pcre2Regex captures;
    check(captures.compile(
              QStringLiteral("name=(?<who>\\w+) id=(\\d+)(?: tag=(\\w+))?"),
              false, true, false, &error),
          "capture expression compiles");
    QByteArray expanded;
    check(captures.expandReplacementAt(
              reinterpret_cast<const uchar *>(captureSubject.constData()),
              captureSubject.size(), 0, captureSubject.size(),
              QStringLiteral("$2:${who}:$3:$$:\\n:\\1"),
              &expanded, &error),
          "numbered, named, optional and escaped replacement expands");
    check(expanded == QByteArray("42:alice::$:\n:alice"),
          "expanded replacement bytes are exact");
    check(!captures.expandReplacementAt(
              reinterpret_cast<const uchar *>(captureSubject.constData()),
              captureSubject.size(), 0, captureSubject.size(),
              QStringLiteral("${missing}"), &expanded, &error),
          "missing named capture is rejected");
    check(error.contains(QStringLiteral("不存在")),
          "missing capture has visible diagnostic");
    check(!captures.expandReplacementAt(
              reinterpret_cast<const uchar *>(captureSubject.constData()),
              captureSubject.size(), 0, captureSubject.size(),
              QStringLiteral("${who"), &expanded, &error),
          "unterminated named capture is rejected");

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
