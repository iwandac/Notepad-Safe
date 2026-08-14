// ============================================================================
//  可视窗口 / 行跳转 诊断工具
//  (1) 打开 smalltestfile 对比 LineIndex 行数 vs PowerShell/Notepad++
//  (2) 对 bigtestfile 验证：
//       · 能跳行 0 / N/4 / N/2 / 3N/4 / N-1
//       · 对应 byteOffset 可通过 mmap 读回真实字节
//       · byteOffset → line 转换 round-trip 一致性
//       · LineIndex totalBytes == file size
// ============================================================================
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <cmath>

#include "LineIndex.h"

#define P(fmt, ...) printf(fmt "\n", ##__VA_ARGS__); fflush(stdout)

static int total = 0, passed = 0;
#define CHECK(cond, name) do { \
    ++total; \
    if (cond) { ++passed; P("  [PASS] %s", name); } \
    else { P("  [FAIL] %s", name); } \
} while(0)

static std::unique_ptr<uchar, void(*)(void*)> mmapFile(const QString& path, qint64& outSize) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        P("  cannot open: %s", qPrintable(f.errorString()));
        outSize = 0;
        return {nullptr, free};
    }
    qint64 sz = f.size();
    uchar* buf = (uchar*)malloc(size_t(sz + 16));
    if (!buf) { outSize = 0; return {nullptr, free}; }
    qint64 got = f.read((char*)buf, sz);
    if (got != sz) {
        P("  warning: read %lld != size %lld", got, sz);
    }
    outSize = sz;
    return {buf, free};
}

// 参考 LineIndex 扫描 LF+CRLF+CR，获取「独立参考行数」
static qint64 refCountNewlines(const uchar* d, qint64 size) {
    qint64 n = 0;
    qint64 i = 0;
    while (i < size) {
        uchar c = d[i];
        if (c == '\r') {
            // 看下一个是否是 '\n'（CRLF），若是跳过两个字符计 1 换行
            if (i + 1 < size && d[i + 1] == '\n') i += 2;
            else i += 1;
            ++n;
        } else if (c == '\n') {
            i += 1;
            ++n;
        } else {
            i += 1;
        }
    }
    // 末尾非换行符 = 最后一行
    if (size > 0) {
        uchar last = d[size - 1];
        if (last != '\r' && last != '\n') ++n;
    }
    return n;
}

static void testFile(const QString& tag, const QString& path) {
    P("\n=== %s ===", qPrintable(tag));
    QFileInfo fi(path);
    if (!fi.exists()) { P("  SKIP: not found: %s", qPrintable(path)); return; }
    qint64 size = 0;
    auto data = mmapFile(path, size);
    if (!data || size <= 0) return;
    P("  file size = %lld bytes", size);

    qint64 refLines = refCountNewlines(data.get(), size);
    P("  reference line count (LF/CRLF/CR all counted) = %lld", refLines);

    LineIndex idx;
    bool ok = idx.buildFromMmap(data.get(), size, {}, nullptr);
    CHECK(ok, "buildFromMmap");
    if (!ok) return;

    qint64 gotLines = idx.totalLines();
    qint64 gotBytes = idx.totalBytes();
    P("  LineIndex: totalLines=%lld totalBytes=%lld maxLineBytes=%lld realCoverage=%lld",
      gotLines, gotBytes, idx.maxLineBytes(), idx.realCoverageBytes());
    CHECK(gotBytes == size, "totalBytes == file.size");

    QString lineCountInfo = QString("totalLines %1 vs reference %2 (delta %3)")
        .arg(gotLines).arg(refLines).arg(gotLines - refLines);
    CHECK(abs(gotLines - refLines) < 100,   // 容忍非常少量的估计偏差（<100）
          qPrintable(lineCountInfo));

    if (gotLines < 2) {
        P("  too few lines to test scrolling, skip jump tests");
        return;
    }

    // 行跳转采样点
    auto testJump = [&](const char* label, qint64 lineNo) {
        qint64 off = idx.lineToByteOffset(int(lineNo));
        bool offOk = (off >= 0 && off <= size);
        char labelbuf[160];
        std::snprintf(labelbuf, sizeof(labelbuf),
                      "lineToByteOffset(%s=%lld) -> %lld %s",
                      label, lineNo, off, offOk ? "IN-RANGE" : "OUT-OF-RANGE!");
        CHECK(offOk, labelbuf);

        // 再反查 lineNo
        int lineBack = -1;
        qint64 colBack = 0;
        idx.byteToLineCol(off, &lineBack, &colBack);
        // 允许 ±1 误差（不同换行拆分实现边界）
        bool roundOk = (std::abs(lineBack - lineNo) <= 1);
        std::snprintf(labelbuf, sizeof(labelbuf),
                      "  round-trip byte(%lld) -> line %d vs expected %lld (%s)",
                      off, lineBack, lineNo, roundOk ? "ok" : "MISMATCH");
        CHECK(roundOk, labelbuf);

        if (offOk && off < size) {
            // 读出跳转后的前 16 字节预览
            qint64 readN = std::min<qint64>(16, size - off);
            QByteArray preview((const char*)(data.get() + off), int(readN));
            P("    preview bytes@%lld: %s", off,
              qPrintable(QString::fromLatin1(preview.toHex())));
        }
    };

    testJump("LINE_0",           0);
    testJump("LINE_1000",        std::min<qint64>(gotLines - 1, 1000));
    testJump("LINE_1/4",         gotLines / 4);
    testJump("LINE_1/2",         gotLines / 2);
    testJump("LINE_3/4",         3 * gotLines / 4);
    testJump("LINE_LAST-1",      gotLines - 1);

    // 100 次随机跳转 + 验证
    P("\n  100 次随机跳转 round-trip ...");
    int badRandom = 0;
    std::mt19937 rng(12345);
    for (int k = 0; k < 100; ++k) {
        qint64 targetLine = std::uniform_int_distribution<qint64>(0, gotLines - 1)(rng);
        qint64 off = idx.lineToByteOffset(int(targetLine));
        if (off < 0 || off > size) { ++badRandom; continue; }
        int lineBack = -1;
        qint64 colBack = 0;
        idx.byteToLineCol(off, &lineBack, &colBack);
        if (std::abs(lineBack - targetLine) > 1) {
            ++badRandom;
            if (badRandom <= 3) {
                P("    [warn] random rountrip k=%d targetLine=%lld -> off=%lld -> lineBack=%d",
                  k, targetLine, off, lineBack);
            }
        }
    }
    char labelbuf[128];
    std::snprintf(labelbuf, sizeof(labelbuf),
                  "random 100 line jumps roundtrip ok (%d errors)", badRandom);
    CHECK(badRandom == 0, labelbuf);

    // byteOffset → line 尾部边界
    {
        qint64 off = size - 1;
        int lastLine = -1;
        qint64 colBack = 0;
        idx.byteToLineCol(off, &lastLine, &colBack);
        bool ok_ = (lastLine >= 0 && lastLine < gotLines);
        P("  byteOffset(size-1)=%lld -> line %d (totalLines=%lld)", off, lastLine, gotLines);
        CHECK(ok_, "last byte maps to a valid line");
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    P("=== LineIndex 可视窗口跳转 / 行数诊断工具 ===");

    // 默认测试路径（相对工作目录 f:\projects\credentials），
    // 用户也可通过 argv 传入自定义文件路径
    QString smallTestFile = QCoreApplication::applicationDirPath() + "/../../../smalltestfile";
    QString bigTestFile   = QCoreApplication::applicationDirPath() + "/../../../bigtestfile";

    if (argc >= 2) smallTestFile = QString::fromUtf8(argv[1]);
    if (argc >= 3) bigTestFile   = QString::fromUtf8(argv[2]);

    testFile("smalltestfile", smallTestFile);
    testFile("bigtestfile",   bigTestFile);

    P("\n=== 总计 %d/%d 通过 ===", passed, total);
    return (passed == total) ? 0 : 1;
}
