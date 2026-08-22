#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <atomic>
#include <cstdio>

#include "LineIndex.h"
#include "DynamicThresholds.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QString path = "f:/projects/notepadsafe/bigtestfile";
    if (argc > 1) path = argv[1];

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "Failed to open: %s\n", path.toUtf8().constData());
        return 1;
    }

    qint64 size = file.size();
    fprintf(stderr, "File size: %lld bytes (%lld MB)\n", (long long)size, (long long)(size/1024/1024));

    // Map the file
    uchar* data = file.map(0, size);
    if (!data) {
        fprintf(stderr, "mmap failed!\n");
        return 1;
    }
    fprintf(stderr, "mmap OK, data=%p\n", (void*)data);

    // Verify first and last bytes are accessible
    fprintf(stderr, "First byte: 0x%02x\n", (unsigned)data[0]);
    fprintf(stderr, "Last byte: 0x%02x\n", (unsigned)data[size-1]);

    // Count newlines in first and last 1MB for verification
    {
        qint64 firstMlLF = 0, lastMlLF = 0;
        for (qint64 i = 0; i < qMin<qint64>(1024*1024, size); i++)
            if (data[i] == '\n') firstMlLF++;
        qint64 tailStart = qMax<qint64>(0, size - 1024*1024);
        for (qint64 i = tailStart; i < size; i++)
            if (data[i] == '\n') lastMlLF++;
        fprintf(stderr, "First 1MB newlines: %lld, Last 1MB newlines: %lld\n",
                (long long)firstMlLF, (long long)lastMlLF);
    }

    // Build line index with progress
    LineIndex index;
    std::atomic<bool> cancel{false};
    QElapsedTimer timer;
    timer.start();

    qint64 lastReported = 0;
    qint64 sizeCopy = size;
    bool ok = index.buildFromMmap(data, size,
        [&lastReported, &timer, sizeCopy](qint64 lines, qint64 bytesDone) {
            if (bytesDone - lastReported >= 64 * 1024 * 1024) { // every 64MB
                fprintf(stderr, "  progress: %lld / %lld bytes (%lld lines, %lld ms)\n",
                        (long long)bytesDone, (long long)sizeCopy, (long long)lines,
                        (long long)timer.elapsed());
                lastReported = bytesDone;
            }
        },
        &cancel, 16 * 1024 * 1024);  // 16MB step

    qint64 elapsed = timer.elapsed();
    fprintf(stderr, "buildFromMmap returned: %d, elapsed: %lld ms\n", ok, (long long)elapsed);
    fprintf(stderr, "totalLines=%lld, totalBytes=%lld, maxLineBytes=%lld, realCoverageBytes=%lld, incomplete=%d\n",
            (long long)index.totalLines(), (long long)index.totalBytes(),
            (long long)index.maxLineBytes(), (long long)index.realCoverageBytes(),
            index.isIncomplete());

    // Verify line count by scanning
    qint64 actualLines = 0;
    for (qint64 i = 0; i < size; i++)
        if (data[i] == '\n') actualLines++;
    if (size > 0 && data[size-1] != '\n') actualLines++;
    fprintf(stderr, "Actual line count (by scanning): %lld\n", (long long)actualLines);

    // Verify first 10 line offsets
    fprintf(stderr, "\nFirst 10 line offsets:\n");
    for (int i = 0; i < qMin(10, (int)index.totalLines()); i++) {
        qint64 offset = index.lineToByteOffset(i);
        qint64 nextOffset = (i+1 < index.totalLines()) ? index.lineToByteOffset(i+1) : size;
        qint64 lineLen = nextOffset - offset;
        fprintf(stderr, "  line %d: byteOffset=%lld, lineLen=%lld\n", i, (long long)offset, (long long)lineLen);
    }

    // Verify some line offsets near the end
    if (index.totalLines() > 10) {
        fprintf(stderr, "\nLast 5 line offsets:\n");
        for (int i = qMax((qint64)0, index.totalLines() - 5); i < index.totalLines(); i++) {
            qint64 offset = index.lineToByteOffset(i);
            qint64 nextOffset = (i+1 < index.totalLines()) ? index.lineToByteOffset(i+1) : size;
            qint64 lineLen = nextOffset - offset;
            fprintf(stderr, "  line %lld: byteOffset=%lld, lineLen=%lld\n",
                    (long long)i, (long long)offset, (long long)lineLen);
        }
    }

    // Verify byteToLineCol for some key positions
    fprintf(stderr, "\nbyteToLineCol checks:\n");
    qint64 testPositions[] = {0, 100, 1024, 1024*1024, 1024*1024*100, size/2, size-1};
    for (qint64 pos : testPositions) {
        if (pos >= 0 && pos < size) {
            int line = 0; qint64 col = 0;
            index.byteToLineCol(pos, &line, &col);
            qint64 roundTrip = index.lineToByteOffset(line);
            fprintf(stderr, "  pos=%lld -> line=%d col=%lld -> lineStart=%lld (diff=%lld)\n",
                    (long long)pos, line, (long long)col, (long long)roundTrip, (long long)(pos - roundTrip));
        }
    }

    file.unmap(data);
    file.close();

    fprintf(stderr, "\nDone.\n");
    return 0;
}
