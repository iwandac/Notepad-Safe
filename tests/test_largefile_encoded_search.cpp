#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <limits>

#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "TextEncoding.h"

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool runFindAll(LargeFileSearch *search, const QString &pattern,
                const LargeFileSearch::Options &options, QString *error,
                qint64 *count, qint64 *firstBatchMs = nullptr)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QElapsedTimer elapsed;
    elapsed.start();
    bool done = false;
    bool batchSeen = false;
    const auto partial = QObject::connect(
        search, &LargeFileSearch::partialBatch, &loop,
        [&](const QVector<LargeFileSearchMatch> &) {
            if (!batchSeen && firstBatchMs) {
                *firstBatchMs = elapsed.elapsed();
                batchSeen = true;
            }
        });
    const auto finished = QObject::connect(
        search, &LargeFileSearch::finished, &loop, [&](qint64 total) {
            done = true;
            if (count) *count = total;
            loop.quit();
        });
    const auto failed = QObject::connect(
        search, &LargeFileSearch::failed, &loop, [&](const QString &message) {
            if (error) *error = message;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        if (error) *error = QStringLiteral("timeout");
        search->requestCancel();
        loop.quit();
    });
    timeout.start(15000);
    search->findAll(pattern, options);
    loop.exec();
    QObject::disconnect(partial);
    QObject::disconnect(finished);
    QObject::disconnect(failed);
    search->cancelAndWait();
    return done;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    bool representable = false;
    const QString text = QStringLiteral(
        "甲中文乙\n中文 ERROR id=42\nXERROR id=500\n中文 ERROR id=500\n");
    QByteArray subject = encodeTextFragment(
        text, TextEncoding::GBK, &representable);
    check(representable, "GBK integration fixture is representable");

    bool trailRepresentable = false;
    const QByteArray trailOnly = encodeTextFragment(
        QString(QChar(0x4e02)), TextEncoding::GBK, &trailRepresentable);
    check(trailRepresentable && trailOnly.size() == 2
              && uchar(trailOnly.at(1)) == 0x40,
          "GBK boundary fixture contains @ only as a trail byte");
    subject += trailOnly;
    subject += '\n';

    std::atomic<bool> cancel{false};
    LineIndex lines;
    check(lines.buildFromMmap(
              reinterpret_cast<const uchar *>(subject.constData()),
              subject.size(), {}, &cancel),
          "line index builds for encoded integration subject");
    PatchList patches;
    LargeFileSearch search;
    search.setSource(reinterpret_cast<const uchar *>(subject.constData()),
                     subject.size(), &lines, &patches);

    LargeFileSearch::Options options;
    options.textEncoding = TextEncoding::GBK;
    options.regexMode = true;
    options.caseSensitive = true;
    QString error;
    qint64 count = -1;
    qint64 firstBatchMs = -1;
    check(runFindAll(&search, QStringLiteral("ERROR id=(4\\d|500)"),
                     options, &error, &count, &firstBatchMs),
          "GBK regex Find All completes through LargeFileSearch");
    check(error.isEmpty() && count == 3,
          "GBK regex publishes every authoritative match");
    check(firstBatchMs >= 0,
          "first Find All result is published before final model handoff");

    options.regexMode = false;
    options.wholeWord = true;
    count = -1;
    error.clear();
    check(runFindAll(&search, QStringLiteral("ERROR"), options, &error, &count),
          "encoded whole-word literal search completes");
    check(error.isEmpty() && count == 2,
          "encoded whole-word search excludes XERROR");

    count = -1;
    error.clear();
    check(runFindAll(&search, QStringLiteral("中文"), options, &error, &count),
          "CJK whole-word literal search completes");
    check(error.isEmpty() && count == 3,
          "CJK whole-word option retains exact-phrase semantics");

    options.wholeWord = false;
    count = -1;
    error.clear();
    check(runFindAll(&search, QStringLiteral("@"), options, &error, &count),
          "encoded boundary-only literal search completes");
    check(error.isEmpty() && count == 0,
          "literal search never begins at a GBK trail byte");

    for (bool forward : {true, false}) {
        bool literalNavigationCallback = false;
        QEventLoop literalNavigationLoop;
        QTimer literalNavigationTimeout;
        literalNavigationTimeout.setSingleShot(true);
        QObject::connect(&literalNavigationTimeout, &QTimer::timeout,
                         &literalNavigationLoop, &QEventLoop::quit);
        literalNavigationTimeout.start(10000);
        const qint64 literalTaskId = search.findOneViaTaskManager(
            QStringLiteral("@"), options,
            forward ? qint64(-1) : std::numeric_limits<qint64>::max(),
            forward, false, QStringLiteral("integration-gbk-boundary"), &app,
            [&](bool found, LargeFileSearchMatch, QString navigationError) {
                literalNavigationCallback = true;
                check(!found && navigationError.isEmpty(),
                      forward
                          ? "forward literal navigation rejects a GBK trail byte"
                          : "reverse literal navigation rejects a GBK trail byte");
                literalNavigationLoop.quit();
            });
        check(literalTaskId > 0,
              "encoded literal boundary navigation enters async framework");
        literalNavigationLoop.exec();
        search.cancelAndWait();
        check(literalNavigationCallback,
              "encoded literal boundary navigation callback arrives");
    }

    options.regexMode = true;
    bool navigationCallback = false;
    QEventLoop navigationLoop;
    QTimer navigationTimeout;
    navigationTimeout.setSingleShot(true);
    QObject::connect(&navigationTimeout, &QTimer::timeout,
                     &navigationLoop, &QEventLoop::quit);
    navigationTimeout.start(10000);
    const qint64 taskId = search.findOneViaTaskManager(
        QStringLiteral("ERROR id=500"), options, -1, true, false,
        QStringLiteral("integration-gbk"), &app,
        [&](bool found, LargeFileSearchMatch match, QString navigationError) {
            navigationCallback = true;
            check(found && navigationError.isEmpty(),
                  "directional encoded navigation finds a result");
            const QByteArray expected = encodeTextFragment(
                QStringLiteral("ERROR id=500"), TextEncoding::GBK);
            check(subject.mid(match.byteOffset, match.length) == expected,
                  "directional navigation returns an exact source byte range");
            navigationLoop.quit();
        });
    check(taskId > 0, "directional encoded navigation enters async framework");
    navigationLoop.exec();
    search.cancelAndWait();
    check(navigationCallback, "directional navigation callback arrives");

    // Reproduce the real Binary Text case: only wrap-search semantics are
    // relevant at the UI layer, while the literal itself contains the common
    // replacement-character projection of one malformed source byte. This
    // must remain a bounded linear literal scan, never an implicit regex job
    // and never depend on a global line index.
    QByteArray binarySubject(8 * 1024 * 1024, 'x');
    const qint64 binaryHit = binarySubject.size() / 2 + 17;
    binarySubject[int(binaryHit)] = 'H';
    binarySubject[int(binaryHit + 1)] = char(0xff);
    binarySubject[int(binaryHit + 2)] = 'T';
    binarySubject[int(binaryHit + 3)] = '$';
    LargeFileSearch binarySearch;
    binarySearch.setSource(
        reinterpret_cast<const uchar *>(binarySubject.constData()),
        binarySubject.size(), nullptr, nullptr);
    LargeFileSearch::Options binaryOptions;
    binaryOptions.textEncoding = TextEncoding::Utf8;
    binaryOptions.caseSensitive = true;
    binaryOptions.tolerateInvalidEncoding = true;
    QVector<qint64> binaryProgress;
    const auto progressConnection = QObject::connect(
        &binarySearch, &LargeFileSearch::progress, &app,
        [&](qint64 doneBytes, qint64 totalBytes) {
            check(totalBytes == binarySubject.size(),
                  "binary projected search reports source-byte total");
            if (!binaryProgress.isEmpty()) {
                check(doneBytes >= binaryProgress.constLast(),
                      "binary projected search progress is monotonic");
            }
            binaryProgress.append(doneBytes);
        });
    QElapsedTimer binaryElapsed;
    binaryElapsed.start();
    qint64 binaryCount = -1;
    QString binaryError;
    check(runFindAll(&binarySearch, QStringLiteral("H\ufffdT$"),
                     binaryOptions, &binaryError, &binaryCount),
          "replacement-character literal completes without a line index");
    const qint64 binaryElapsedMs = binaryElapsed.elapsed();
    QObject::disconnect(progressConnection);
    check(binaryError.isEmpty() && binaryCount == 1,
          "replacement-character literal returns one meaningful result");
    check(!binarySearch.memoryMatches().isEmpty()
              && binarySearch.memoryMatches().constFirst().byteOffset == binaryHit
              && binarySearch.memoryMatches().constFirst().length == 4,
          "projected literal result maps to the exact malformed source bytes");
    check(!binarySearch.memoryMatches().isEmpty()
              && binarySearch.memoryMatches().constFirst().line == -1,
          "binary result does not invent a global text line number");
    check(!binaryProgress.isEmpty()
              && binaryProgress.constLast() == binarySubject.size(),
          "binary projected search reaches a visible linear completion point");
    check(binaryElapsedMs < 5000,
          "8 MiB replacement-character literal search has a bounded ROI gate");
    binarySearch.cancelAndWait();

    if (!subject.isEmpty())
        subject.fill(0);
    if (!binarySubject.isEmpty())
        binarySubject.fill(0);
    std::fprintf(stderr, "%s (%d failure(s))\n",
                 failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
