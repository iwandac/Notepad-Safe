// ============================================================================
//  PatchList 编辑功能诊断工具
//  验证：insert / removeWithSnapshot / undo / redo / applyAndSave
// ============================================================================
#include <QCoreApplication>
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QDir>
#include <QElapsedTimer>
#include <QDateTime>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <random>

#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"

#define P(fmt, ...) printf(fmt "\n", ##__VA_ARGS__); fflush(stdout)

static int total = 0, passed = 0;
#define CHECK(cond, name) do { \
    ++total; \
    if (cond) { ++passed; P("  [PASS] %s", name); } \
    else { P("  [FAIL] %s", name); } \
} while(0)

static void renderCheck(const char* tag, const QByteArray& actual, const QByteArray& expected) {
    char buf[80];
    if (actual == expected) {
        ++total; ++passed;
        P("  [PASS] renderCheck(%s): %lld bytes match", tag, (long long)actual.size());
    } else {
        ++total;
        int limit = qMin(60, (int)actual.size());
        QString a = QString::fromUtf8(actual.left(limit).toHex());
        QString e = QString::fromUtf8(expected.left(limit).toHex());
        P("  [FAIL] renderCheck(%s): actual(%lld) != expected(%lld)\n"
          "    actual_hex:   %s\n"
          "    expected_hex: %s",
          tag, (long long)actual.size(), (long long)expected.size(),
          a.toUtf8().constData(), e.toUtf8().constData());
        // 逐字节定位第一个不匹配
        qint64 minlen = qMin<qint64>(actual.size(), expected.size());
        for (qint64 k = 0; k < minlen; ++k) {
            if (actual[(int)k] != expected[(int)k]) {
                P("    FIRST MISMATCH at byte[%lld]: actual=0x%02X expected=0x%02X",
                  k, (unsigned char)actual[(int)k], (unsigned char)expected[(int)k]);
                break;
            }
        }
        Q_UNUSED(buf);
    }
}

static void test_basic_edit() {
    P("\n=== test_basic_edit ===");
    QByteArray orig("0123456789ABCDEF");   // 16 bytes
    PatchList pl;

    QByteArray r = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 16);
    renderCheck("empty patch → full", r, orig);

    QString err;
    bool ok;

    // Insert at position 10: "@@"
    ok = pl.insert(10, QByteArrayLiteral("@@"), &err);
    CHECK(ok, "insert(10, '@@')");
    QByteArray exp1("0123456789@@ABCDEF");   // 18 bytes
    renderCheck("after insert",
        pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 18), exp1);

    // Undo insert
    ok = pl.undo(&err);
    if (!ok) P("    undo err: %s", err.toUtf8().constData());
    CHECK(ok, "undo(insert)");
    renderCheck("after undo insert",
        pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 16), orig);

    // Redo insert
    ok = pl.redo(&err);
    if (!ok) P("    redo err: %s", err.toUtf8().constData());
    CHECK(ok, "redo(insert)");
    renderCheck("after redo insert",
        pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 18), exp1);

    // Remove bytes at pos 10, len=2 (the @@ we just inserted via redo)
    QByteArray oldSnap = pl.renderRange((const uchar*)orig.constData(), orig.size(), 10, 12); // 2 bytes
    CHECK(oldSnap == QByteArrayLiteral("@@"), "removed snapshot is '@@'");
    {
        P("    [dbg] BEFORE remove: l1Hot_ patches (count=%d):", pl.patchCount());
        for (int idx = 0; idx < pl.patchCount(); ++idx) {
            const Patch* p = pl.patchAt(idx);
            P("      [%d] byteOffset=%lld oldLength=%lld newText.size=%lld hex=%s",
              idx,
              (long long)p->byteOffset, (long long)p->oldLength, (long long)p->newText.size(),
              p->newText.toHex().constData());
        }
    }
    ok = pl.removeWithSnapshot(10, 2, oldSnap, &err);
    if (!ok) P("    remove err: %s", err.toUtf8().constData());
    CHECK(ok, "remove(10, 2, '@@')");
    {
        P("    [dbg] AFTER remove: l1Hot_ patches (count=%d):", pl.patchCount());
        for (int idx = 0; idx < pl.patchCount(); ++idx) {
            const Patch* p = pl.patchAt(idx);
            P("      [%d] byteOffset=%lld oldLength=%lld newText.size=%lld hex=%s",
              idx,
              (long long)p->byteOffset, (long long)p->oldLength, (long long)p->newText.size(),
              p->newText.toHex().constData());
        }
        P("    [dbg] virtualSize=%lld netDelta=%lld", (long long)pl.virtualSize(orig.size()), (long long)pl.netDelta());
    }
    renderCheck("after remove(10,2)",
        pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 16), orig);

    // Undo remove → should restore the "@@" at 10
    ok = pl.undo(&err);
    if (!ok) P("    undo(remove) err: %s", err.toUtf8().constData());
    CHECK(ok, "undo(remove)");
    renderCheck("after undo remove → back to insert",
        pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 18), exp1);
}

static void test_save() {
    P("\n=== test_save ===");
    QByteArray orig("HELLO\nWORLD\nLINE3\n");
    PatchList pl;
    QString err;
    bool ok = pl.insert(6, QByteArrayLiteral("INSERTED_"), &err);
    CHECK(ok, "insert for save");

    QByteArray expected("HELLO\nINSERTED_WORLD\nLINE3\n");
    // 不用 QTemporaryFile（Windows 上 QTemporaryFile 创建的文件带 FILE_ATTRIBUTE_TEMPORARY，
    // QSaveFile 原子提交经常会拒绝访问），改用手写的唯一 temp 路径。
    QString baseName = QStringLiteral("diag_save_%1").arg(quint64(QDateTime::currentMSecsSinceEpoch()));
    QString outPath = QDir::tempPath() + "/" + baseName + ".txt";
    QFile::remove(outPath);  // 确保不存在旧同名

    std::atomic<bool> cancel(false);
    QElapsedTimer t; t.start();
    ok = pl.applyAndSave((const uchar*)orig.constData(), orig.size(), outPath, {}, &cancel, &err);
    if (!ok) P("    save err: %s", err.toUtf8().constData());
    CHECK(ok, "applyAndSave");
    P("    save took %lld ms, size=%d  path=%s", t.elapsed(), (int)QFileInfo(outPath).size(),
      qPrintable(QDir::toNativeSeparators(outPath)));

    QFile f(outPath);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray saved = f.readAll();
        f.close();
        renderCheck("saved file content", saved, expected);
    } else {
        P("  [SKIP] cannot open saved file: %s", qPrintable(f.errorString()));
    }
    QFile::remove(outPath);
}

static void test_render_range_consistency() {
    P("\n=== test_render_range_consistency (large synthetic file) ===");
    QByteArray orig;
    const int LINES = 100000;
    orig.reserve(LINES * 12);
    for (int i = 0; i < LINES; ++i) {
        char line[64]; int n = snprintf(line, sizeof(line), "LINE_%06d\n", i);
        orig.append(line, n);
    }
    P("synthetic file: %d lines, %lld bytes", LINES, (long long)orig.size());

    PatchList pl;
    QString err;
    std::mt19937 rng(12345);

    // Apply 1000 random edits
    for (int i = 0; i < 1000; ++i) {
        qint64 pos = (qint64)(rng() % (orig.size() + 1));
        QByteArray data;
        int mode = (int)(rng() % 3);
        if (mode == 0) { // insert
            data = QStringLiteral("X%1Y").arg(i).toUtf8();
            pl.insert(pos, data, &err);
        } else if (mode == 1) { // remove short stretch
            qint32 len = qint32(qMin<qint64>(10, orig.size() - pos));
            if (len > 0) {
                QByteArray snap = pl.renderRange((const uchar*)orig.constData(), orig.size(), pos, pos + len);
                if (snap.size() < len) snap.resize(len, '\0');
                pl.removeWithSnapshot(pos, len, snap, &err);
            }
        } else { // replace
            qint32 oldLen = qint32(qMin<qint64>(5, orig.size() - pos));
            QByteArray newData = QStringLiteral("R%1").arg(i).toUtf8();
            QByteArray snap = pl.renderRange((const uchar*)orig.constData(), orig.size(), pos, pos + oldLen);
            if (snap.size() < oldLen) snap.resize(oldLen, '\0');
            pl.replaceWithSnapshots(pos, oldLen, newData, snap, &err);
        }
    }
    P("after 1000 edits: l1Count=%d l2Count=%d l1Bytes=%lld",
      pl.l1Count(), pl.l2Count(), pl.l1Bytes());

    // Verify: chunked renderRange matches full renderRange
    qint64 virtSize = pl.virtualSize(orig.size());
    P("virtualSize = %lld", virtSize);

    QByteArray full = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, virtSize);
    CHECK((long long)full.size() == virtSize, "full render size == virtualSize");

    // Chunked reads: every 1MB
    QByteArray chunked;
    const qint64 CHUNK = 1024 * 1024;
    for (qint64 s = 0; s < virtSize; s += CHUNK) {
        qint64 e = qMin(s + CHUNK, virtSize);
        QByteArray c = pl.renderRange((const uchar*)orig.constData(), orig.size(), s, e);
        chunked.append(c);
    }
    CHECK((long long)chunked.size() == (long long)full.size(), "chunked read size matches full read");
    if (chunked.size() == full.size()) {
        CHECK(chunked == full, "chunked content == full content");
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    DynamicThresholds::instance();  // 初始化阈值（避免首次动态分配）

    P("=== PatchList 编辑功能诊断工具 ===");
    P("DynamicThresholds: PATCH_L1_LIMIT=%lld, RAM_BYTES=%lld",
      DynamicThresholds::instance().PATCH_L1_LIMIT,
      DynamicThresholds::instance().RAM_BYTES);

    QElapsedTimer t; t.start();
    test_basic_edit();
    test_save();
    test_render_range_consistency();
    P("\n=== 总计 %d/%d 通过（%lld ms）===", passed, total, t.elapsed());
    return (passed == total) ? 0 : 1;
}
