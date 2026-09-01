#include "EditableTextNavigationScan.h"

#include "EditableTextSearch.h"
#include "Pcre2Regex.h"

EditableTextNavigationResult scanEditableTextNavigation(
    const QByteArray &subject, const QString &pattern, bool regex,
    bool caseSensitive, bool wholeWord, bool forward, bool wrap,
    qint64 boundary, bool currentZeroLengthHitAtEnd,
    const std::function<bool()> &cancelled,
    const std::function<void(qint64, qint64)> &reportProgress)
{
    EditableTextNavigationResult result;
    const auto progress = [reportProgress, total = qint64(subject.size())](qint64 done) {
        reportProgress(done, total);
    };
    bool ok = true;
    if (!regex) {
        auto literal = findEditableTextLiteralHits(
            subject, pattern, caseSensitive, wholeWord, cancelled,
            progress, true, !forward, boundary);
        if (literal.isEmpty() && !cancelled() && wrap) {
            literal = findEditableTextLiteralHits(
                subject, pattern, caseSensitive, wholeWord, cancelled,
                progress, true, !forward, forward ? 0 : subject.size() + 1);
        }
        if (!literal.isEmpty()) {
            result.byteOffset = literal.first().byteOffset;
            result.byteLength = literal.first().byteLength;
        }
        ok = !cancelled();
    } else {
        Pcre2Regex compiled;
        ok = compiled.compile(pattern, false, caseSensitive, wholeWord, &result.error);
        const auto scanForward = [&](qint64 start) {
            const auto scanned = compiled.scan(
                reinterpret_cast<const uchar *>(subject.constData()),
                subject.size(), cancelled, progress,
                [&](qint64 offset, qint64 length) {
                    result.byteOffset = offset;
                    result.byteLength = length;
                    return false;
                }, &result.error, {}, {}, start);
            return scanned != Pcre2Regex::ScanResult::Failed && !cancelled();
        };
        if (ok && forward) {
            if (!currentZeroLengthHitAtEnd)
                ok = scanForward(qBound<qint64>(0, boundary, subject.size()));
            if (ok && result.byteOffset < 0 && wrap && boundary > 0)
                ok = scanForward(0);
        } else if (ok) {
            const auto scanned = compiled.scan(
                reinterpret_cast<const uchar *>(subject.constData()),
                subject.size(), cancelled, progress,
                [&](qint64 offset, qint64 length) {
                    if (offset >= boundary)
                        return false;
                    result.byteOffset = offset;
                    result.byteLength = length;
                    return true;
                }, &result.error);
            ok = scanned != Pcre2Regex::ScanResult::Failed && !cancelled();
            if (ok && result.byteOffset < 0 && wrap && boundary < subject.size()) {
                const auto wrapped = compiled.scan(
                    reinterpret_cast<const uchar *>(subject.constData()),
                    subject.size(), cancelled, {},
                    [&](qint64 offset, qint64 length) {
                        result.byteOffset = offset;
                        result.byteLength = length;
                        return true;
                    }, &result.error, {}, {}, boundary);
                ok = wrapped != Pcre2Regex::ScanResult::Failed && !cancelled();
            }
        }
    }
    if (!ok && result.error.isEmpty() && cancelled())
        result.error = QStringLiteral("已取消。");
    result.ok = ok;
    return result;
}
