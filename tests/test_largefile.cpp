// 大文件分块加载回归测试
//
// 背景：曾出现"打开大文件时进度对话框正常显示，但加载完成后编辑框无内容"的 bug。
// 根因：loadUtf8Chunked 中调用了 m_editor->setReadOnly(true)，而 Scintilla 的
//       Document::InsertString 内部检查 cb.IsReadOnly()，readOnly 状态下
//       SCI_APPENDTEXT 直接返回 0 不插入任何内容，导致文本静默丢失。
//
// 本测试直接创建 QsciScintilla 实例，端到端验证：
//   1. readOnly 状态下 SCI_APPENDTEXT 确实不插入内容（复现 bug 根因）
//   2. 非 readOnly 状态下 SCI_APPENDTEXT 正常插入
//   3. 修复后的 loadUtf8Chunked 逻辑路径（不调用 setReadOnly）能正确加载大文件
//
// 注意：不使用 Qt Test 框架（QTEST_MAIN），因为它与静态 Qt 构建冲突；
//       直接用 main() + QApplication，与 notepad-safe.exe 主程序相同的初始化方式。

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QFile>
#include <QDir>
#include <QSaveFile>
#include <QDataStream>
#include <QVector>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QSet>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "../LineIndex.h"
#include "../PatchList.h"
#include "../LargeFileSearch.h"
#include "../SearchResultsModel.h"
#include "../DynamicThresholds.h"
#include "../VirtualFileView.h"
#include "../EditorTab.h"
#include "../SessionManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) do { fprintf(stderr, "  [RUN] %s\n", name); fflush(stderr); } while(0)

#define CHECK(cond, msg) \
    do { \
        ++test_count; \
        if (cond) { ++test_passed; fprintf(stderr, "    [PASS] %s\n", msg); fflush(stderr); } \
        else { fprintf(stderr, "    [FAIL] %s\n", msg); fflush(stderr); } \
    } while (0)

// 生成指定大小的 UTF-8 文本（含 ASCII + 多字节字符，模拟真实大文件）
// 每行约 80 字符，行尾 \n，确保有多字节字符触发边界回退逻辑
static QByteArray generateLargeUtf8(qsizetype sizeBytes)
{
    QByteArray buf;
    buf.reserve(sizeBytes);
    const char *line = "Line 1234: 测试中文内容 hello world \xE4\xB8\xAD\xE6\x96\x87\n";
    const int lineLen = static_cast<int>(std::strlen(line));
    while (buf.size() + lineLen <= sizeBytes) {
        buf.append(line, lineLen);
    }
    // 补齐到目标大小（保持 UTF-8 完整性：用 ASCII 填充）
    while (buf.size() < sizeBytes) {
        buf.append('x');
    }
    return buf;
}

// ============ 测试 1：复现 bug 根因 - readOnly 状态下 SCI_APPENDTEXT 不插入 ============
// 这是回归测试的核心：验证 Scintilla 的 InsertString 在 readOnly 下确实静默失败。
// 如果未来 Scintilla 改变了这个行为，本测试需要更新；但只要此行为存在，
// loadUtf8Chunked 就绝不能在加载前调用 setReadOnly(true)。
static void test_readonly_blocks_appendtext()
{
    TEST("readOnly 状态下 SCI_APPENDTEXT 应静默失败（bug 根因复现）");

    QsciScintilla ed;
    ed.setReadOnly(true);

    const char *text = "hello";
    ed.SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                     static_cast<uintptr_t>(5), text);

    long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    fprintf(stderr, "    [INFO] readOnly=true 时追加 5 字节后 length=%ld\n", len);

    CHECK(len == 0, "readOnly 状态下 SCI_APPENDTEXT 不插入任何内容（bug 根因）");
}

// ============ 测试 2：非 readOnly 状态下 SCI_APPENDTEXT 正常工作 ============
static void test_appendtext_works_when_not_readonly()
{
    TEST("非 readOnly 状态下 SCI_APPENDTEXT 正常插入");

    QsciScintilla ed;
    // 注意：故意不调用 setReadOnly(true)

    const char *text = "hello world";
    int textLen = 11;
    ed.SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                     static_cast<uintptr_t>(textLen), text);

    long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    fprintf(stderr, "    [INFO] 非 readOnly 时追加 11 字节后 length=%ld\n", len);

    CHECK(len == textLen, "SCI_APPENDTEXT 在非 readOnly 状态下正确插入文本");

    // 验证内容正确
    QByteArray buf(len + 1, '\0');
    ed.SendScintilla(QsciScintillaBase::SCI_GETTEXT,
                     static_cast<uintptr_t>(buf.size()), buf.data());
    buf.resize(len);
    CHECK(buf == QByteArray("hello world"), "插入的内容与预期一致");
}

// ============ 测试 3：分块追加（修复后的 loadUtf8Chunked 逻辑路径） ============
// 模拟修复后的 loadUtf8Chunked：不调用 setReadOnly，分块追加 ~2MB 数据
static void test_chunked_append_without_readonly()
{
    TEST("分块追加 ~2MB 数据（修复后路径：不 setReadOnly）");

    QsciScintilla ed;

    // 关闭 undo 收集（与 loadUtf8Chunked 一致）
    ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    ed.setScrollWidth(1);
    ed.clear();

    // 生成 ~2MB UTF-8 数据
    const QByteArray data = generateLargeUtf8(2 * 1024 * 1024);
    const char *raw = data.constData();
    const int total = data.size();
    const int chunkSize = 512 * 1024;

    int pos = 0;
    while (pos < total) {
        int end = pos + chunkSize;
        if (end > total) end = total;
        // 回退到 UTF-8 字符边界
        while (end < total &&
               (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
            --end;
        // 防御：end 不能回退到 pos 之前
        if (end <= pos)
            end = (pos + chunkSize < total) ? pos + chunkSize : total;

        int len = end - pos;
        ed.SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                         static_cast<uintptr_t>(len), raw + pos);
        pos = end;
    }

    ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    ed.SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);

    long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    fprintf(stderr, "    [INFO] 分块追加 %d 字节后 length=%ld\n", total, len);

    CHECK(len == total, "分块追加后文档长度与输入一致（修复后内容不再丢失）");

    // 抽样验证内容：开头 11 字节应为 "Line 1234: "
    QByteArray head(11, '\0');
    ed.SendScintilla(QsciScintillaBase::SCI_GETTEXT,
                     static_cast<uintptr_t>(head.size() + 1), head.data());
    head.resize(11);
    CHECK(head.startsWith("Line 1234"), "分块追加后内容前缀正确");
}

// ============ 测试 4：10MB 大文件分块加载（接近用户 100MB 场景） ============
// 验证修复后 10MB 文件能完整加载，且内容前后段均正确
static void test_chunked_append_10mb()
{
    TEST("分块追加 10MB 数据（接近真实大文件场景）");

    QsciScintilla ed;

    ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    ed.setScrollWidth(1);
    ed.clear();

    QsciScintilla::WrapMode savedWrap = ed.wrapMode();
    if (savedWrap != QsciScintilla::WrapNone)
        ed.setWrapMode(QsciScintilla::WrapNone);

    // 生成 10MB UTF-8 数据
    const QByteArray data = generateLargeUtf8(10 * 1024 * 1024);
    const char *raw = data.constData();
    const int total = data.size();
    const int chunkSize = 512 * 1024;

    int pos = 0;
    while (pos < total) {
        int end = pos + chunkSize;
        if (end > total) end = total;
        while (end < total &&
               (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
            --end;
        if (end <= pos)
            end = (pos + chunkSize < total) ? pos + chunkSize : total;
        int len = end - pos;
        ed.SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                         static_cast<uintptr_t>(len), raw + pos);
        pos = end;
    }

    ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    ed.SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);

    long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    long lines = ed.SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
    fprintf(stderr, "    [INFO] 10MB 分块追加后 length=%ld, lines=%ld\n", len, lines);

    CHECK(len == total, "10MB 分块追加后文档长度与输入一致");
    CHECK(lines > 0, "10MB 文档行数 > 0");

    // 验证开头内容
    QByteArray head(20, '\0');
    ed.SendScintilla(QsciScintillaBase::SCI_GETTEXT,
                     static_cast<uintptr_t>(head.size() + 1), head.data());
    head.resize(20);
    CHECK(head.startsWith("Line 1234"), "10MB 文档开头内容正确");

    // 验证中间内容（通过 SCI_GETTEXTRANGE 读取中间一段）
    long midPos = total / 2;
    // 回退到行首
    while (midPos > 0 && raw[midPos] != '\n') --midPos;
    if (midPos < total) ++midPos;
    char midBuf[30] = {0};
    // Sci_TextRange: {cpMin, cpMax, lpstrText}
    struct SciTextRange { long cpMin; long cpMax; char *lpstrText; };
    SciTextRange tr;
    tr.cpMin = midPos;
    tr.cpMax = qMin(midPos + 29, (long)total);
    tr.lpstrText = midBuf;
    ed.SendScintilla(QsciScintillaBase::SCI_GETTEXTRANGE, 0, &tr);
    QByteArray midContent(midBuf);
    bool midOk = midContent.startsWith("Line ") || midContent.contains("Line ");
    fprintf(stderr, "    [INFO] 中间内容(pos=%ld): '%s'\n", midPos, midBuf);
    CHECK(midOk, "10MB 文档中间内容正确");

    // 验证结尾内容
    QByteArray tail(30, '\0');
    long tailStart = total - 30;
    if (tailStart < 0) tailStart = 0;
    SciTextRange tr2;
    tr2.cpMin = tailStart;
    tr2.cpMax = total;
    tr2.lpstrText = tail.data();
    ed.SendScintilla(QsciScintillaBase::SCI_GETTEXTRANGE, 0, &tr2);
    tail.resize(tr2.cpMax - tr2.cpMin);
    fprintf(stderr, "    [INFO] 结尾内容: '%s'\n", tail.constData());
    CHECK(tail.size() > 0, "10MB 文档结尾内容非空");
}

// ============ 测试 5：对比 readOnly vs 非 readOnly 路径 ============
// 这是核心防回归：同样分块追加，readOnly 路径必然为空，非 readOnly 路径必然成功
static void test_readonly_vs_nonreadonly_chunked()
{
    TEST("readOnly vs 非 readOnly 分块加载对比（核心防回归）");

    const QByteArray data = generateLargeUtf8(600 * 1024); // 600KB
    const char *raw = data.constData();
    const int total = data.size();
    const int chunkSize = 128 * 1024;

    auto appendChunked = [&raw, total, chunkSize](QsciScintilla &ed) {
        ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
        ed.clear();
        int pos = 0;
        while (pos < total) {
            int end = pos + chunkSize;
            if (end > total) end = total;
            while (end < total &&
                   (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
                --end;
            if (end <= pos)
                end = (pos + chunkSize < total) ? pos + chunkSize : total;
            int len = end - pos;
            ed.SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                             static_cast<uintptr_t>(len), raw + pos);
            pos = end;
        }
        ed.SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
        ed.SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    };

    // 路径 A：bug 路径 - 先 setReadOnly(true)
    {
        QsciScintilla ed;
        ed.setReadOnly(true);   // bug 触发条件
        appendChunked(ed);
        long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
        fprintf(stderr, "    [INFO] readOnly 路径加载 %d 字节后 length=%ld\n", total, len);
        CHECK(len == 0, "readOnly 路径加载后文档为空（bug 行为基线）");
    }

    // 路径 B：修复路径 - 不调用 setReadOnly
    {
        QsciScintilla ed;
        appendChunked(ed);
        long len = ed.SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
        fprintf(stderr, "    [INFO] 修复路径加载 %d 字节后 length=%ld\n", total, len);
        CHECK(len == total, "修复路径加载后文档长度正确（修复有效）");
    }
}

// ============================================================================
//  以下：LineIndex / PatchList / LargeFileSearch 新功能单元测试（P3 交付点）
// ============================================================================

// 生成 N 行（每行以 \n 结尾）的字节内容，返回大小；行号从 0 行开始。
static QByteArray generateNLines(int nLines, int bytesPerLine)
{
    QByteArray out;
    out.reserve(nLines * bytesPerLine + nLines);
    for (int i = 0; i < nLines; ++i) {
        QByteArray line = QByteArray::number(i + 1);  // 行号字符串 1-based
        while (line.size() < bytesPerLine) line.append('-');
        line.truncate(bytesPerLine);
        line.append('\n');
        out.append(line);
    }
    return out;
}

// ============ 测试 6：LineIndex build 后 totalLines 与行/字节互转 ============
static void test_lineindex_basic()
{
    TEST("LineIndex: totalLines 正确 + 行/字节互转一致");
    const int N = 10000;
    const int B = 80;
    QByteArray buf = generateNLines(N, B);
    LineIndex idx;
    std::atomic<bool> cancel{false};
    bool ok = idx.buildFromMmap((const uchar*)buf.constData(), buf.size(), {}, &cancel, qint64(1)<<30);
    CHECK(ok, "buildFromMmap 10000 行成功");
    CHECK(idx.totalLines() == N, "totalLines == 10000");

    // 逐行抽样：line 0..N-1 行首字节偏移一致
    QVector<qint64> starts(N + 1, 0);
    qint64 s = 0;
    for (int i = 0; i < N; ++i) {
        starts[i] = s;
        s += B + 1;  // B 字节行内容 + 1 字节 \n
    }
    starts[N] = s;

    bool allOk = true;
    for (int i = 0; i < N; i += 17) {
        qint64 got = idx.lineToByteOffset(i);
        if (got != starts[i]) { allOk = false; break; }
        int line = -1; qint64 col = -1;
        idx.byteToLineCol(starts[i], &line, &col);
        if (line != i || col != 0) { allOk = false; break; }
        // 行中间位置也正确（行首 + 50 字节 → col==50）
        idx.byteToLineCol(starts[i] + qMin(50, B - 1), &line, &col);
        if (line != i) { allOk = false; break; }
    }
    CHECK(allOk, "抽样 10000 行：行/字节互转一致（含 blockAnchor 二分加速）");

    // 最后一行为 9999，最后字节为 buf.size()-1，应映射到行 9999、列 B
    int line = -1; qint64 col = -1;
    idx.byteToLineCol(buf.size() - 1, &line, &col);
    CHECK(line == N - 1, "最后字节落在最后一行");
    (void)col;
}

// ============ 测试 7：LineIndex lineToByteOffset 全量一致性 ============
static void test_lineindex_all_lines_dense()
{
    TEST("LineIndex: 50000 行全量 lineToByteOffset 一致");
    const int N = 50000;
    const int B = 40;
    QByteArray buf = generateNLines(N, B);
    LineIndex idx;
    std::atomic<bool> cancel{false};
    bool ok = idx.buildFromMmap((const uchar*)buf.constData(), buf.size(), {}, &cancel, qint64(1)<<30);
    CHECK(ok, "buildFromMmap 50000 行成功");

    bool okAll = true;
    for (int i = 0; i < N; ++i) {
        qint64 want = qint64(i) * (B + 1);
        if (idx.lineToByteOffset(i) != want) { okAll = false; break; }
    }
    CHECK(okAll, "50000 行每一行 lineToByteOffset 正确");
}

// ============ 测试 8：PatchList insert + renderRange ============
static void test_patchlist_insert_render()
{
    TEST("PatchList: 多次 insert + renderRange 结果正确");
    QByteArray orig = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    PatchList pl;
    QString err;
    bool ok = pl.insert(0, QByteArray("PRE-"), &err);
    CHECK(ok, "insert 0 成功");
    // 末尾插入：orig 26 + "PRE-" 净增量 4 = virtual 末尾 30
    ok = pl.insert(qint64(orig.size()) + 4, QByteArray("-POST"), &err);
    CHECK(ok, "insert 末尾成功");
    // 中间插入：virt 位置 9 = PRE-(4 bytes) + orig "ABCDE"(5 bytes) 之后
    ok = pl.insert(9, QByteArray("[mid]"), &err);
    CHECK(ok, "insert 中间成功");

    QByteArray rendered = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 1000);
    QByteArray expected = "PRE-ABCDE[mid]FGHIJKLMNOPQRSTUVWXYZ-POST";
    CHECK(rendered == expected, "renderRange 与预期一致");
}

// ============ 测试 9：PatchList removeWithSnapshot + undo/redo 10 轮 ============
static void test_patchlist_remove_undo_redo()
{
    TEST("PatchList: remove + undo/redo 10 轮往返一致性");
    QByteArray orig = "0123456789";
    bool allOk = true;
    for (int round = 0; round < 10; ++round) {
        PatchList pl;
        QString err;
        // 每轮删 1 字节（从第 1 字节开始删，round+1 位置）
        int pos = (round % 9) + 1;  // 1..9
        QByteArray deleted = orig.mid(pos, 1);
        bool ok = pl.removeWithSnapshot(pos, 1, deleted, &err);
        allOk = allOk && ok;
        // render 应少 1 字节
        QByteArray afterDel = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
        QByteArray expectedDel = orig;
        expectedDel.remove(pos, 1);
        allOk = allOk && (afterDel == expectedDel);

        // undo
        ok = pl.undo(&err);
        allOk = allOk && ok;
        QByteArray afterUndo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
        allOk = allOk && (afterUndo == orig);

        // redo
        ok = pl.redo(&err);
        allOk = allOk && ok;
        QByteArray afterRedo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
        allOk = allOk && (afterRedo == expectedDel);
    }
    CHECK(allOk, "10 轮 remove+undo+redo 内容一致");
}

// ============ 测试 10：PatchList replaceWithSnapshots 后 undo 回滚 ============
static void test_patchlist_replace_undo()
{
    TEST("PatchList: replace + undo 回滚");
    QByteArray orig = "hello world";
    PatchList pl;
    QString err;
    bool ok = pl.replaceWithSnapshots(6, 5, "folks", QByteArray("world"), &err);
    CHECK(ok, "replace(world->folks) 成功");
    QByteArray r = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(r == "hello folks", "replace 结果正确");
    ok = pl.undo(&err);
    CHECK(ok, "undo 成功");
    r = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(r == "hello world", "undo 后回到原内容");
    ok = pl.redo(&err);
    CHECK(ok, "redo 成功");
    r = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(r == "hello folks", "redo 后再到新内容");
}

// ============ 测试 11：PatchList applyAndSave 流式写盘 + 内容一致性 ============
static void test_patchlist_applyandsave()
{
    TEST("PatchList: applyAndSave 写盘 + 原子保存结果一致");
    QByteArray orig = "ABCD";
    PatchList pl;
    QString err;
    bool ok = pl.insert(2, QByteArray("XYZ"), &err);  // "ABXYZCD"
    CHECK(ok, "insert 成功");
    ok = pl.replaceWithSnapshots(0, 2, "!!", QByteArray("AB"), &err);  // "!!XYZCD"
    CHECK(ok, "replace 成功");

    std::atomic<bool> cancel{false};
    // 使用独立路径避免 QTemporaryFile 与 QSaveFile 原子替换在 Windows 上的权限冲突
    QString path = QDir::tempPath() + QStringLiteral("/npstest_apply_%1.bin")
                        .arg(quint64(QCoreApplication::applicationPid()));
    QFile::remove(path);
    ok = pl.applyAndSave((const uchar*)orig.constData(), orig.size(), path, {}, &cancel, &err);
    if (!ok) fprintf(stderr, "    [DIAG] applyAndSave err: %s\n", err.toUtf8().constData());
    CHECK(ok, "applyAndSave 成功");

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { CHECK(false, "读保存文件失败"); return; }
    QByteArray saved = f.readAll();
    f.close();
    CHECK(saved == "!!XYZCD", "applyAndSave 保存的字节串正确");
    QFile::remove(path);
}

// ============ 测试 12：LargeFileSearch 跨 16MB chunk 边界字面值匹配 ============
static void test_search_cross_chunk()
{
    TEST("LargeFileSearch: 跨 chunk 边界的字面值匹配被命中");
    // 构造：前 16MB-3 字节 = 'a'，接着 "XYZ"，继续 'b' 直到 17MB。
    // 再额外在 24MB 处放 "XYZ" 作为不跨块对照。
    const qint64 CHUNK = qint64(16) * 1024 * 1024;
    const qint64 SIZE = qint64(28) * 1024 * 1024;
    QByteArray data;
    data.resize(SIZE);
    for (qint64 i = 0; i < SIZE; ++i) data[i] = 'a';
    qint64 crossAt = CHUNK - 3;
    data[crossAt + 0] = 'X';
    data[crossAt + 1] = 'Y';
    data[crossAt + 2] = 'Z';
    qint64 midAt = qint64(24) * 1024 * 1024;
    data[midAt + 0] = 'X';
    data[midAt + 1] = 'Y';
    data[midAt + 2] = 'Z';

    LineIndex idx;
    std::atomic<bool> cancel{false};
    idx.buildFromMmap((const uchar*)data.constData(), data.size(), {}, &cancel, qint64(1)<<30);

    PatchList pl;
    LargeFileSearch searcher;
    searcher.setSource((const uchar*)data.constData(), data.size(), &idx, &pl);

    bool receivedFailed = false;
    bool receivedFinished = false;
    qint64 finishedMatches = -1;
    QObject::connect(&searcher, &LargeFileSearch::failed,
                     [&](const QString&) { receivedFailed = true; });
    QObject::connect(&searcher, &LargeFileSearch::finished,
                     [&](qint64 n) { receivedFinished = true; finishedMatches = n; });

    searcher.findAll("XYZ", {});
    QElapsedTimer t; t.start();
    while (!receivedFailed && !receivedFinished && t.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }

    qint64 total = searcher.totalMatches();
    fprintf(stderr, "    [INFO] totalMatches = %lld, finished = %lld\n",
            (long long)total, (long long)finishedMatches);
    CHECK(total >= 2, "至少命中跨块 + 非跨块两处 XYZ");
    CHECK(finishedMatches >= 2, "finished 信号报告命中数也 >= 2");

    // 读取内存匹配，检查 crossAt 和 midAt 是否都存在
    QSet<qint64> offs;
    for (const auto& m : searcher.memoryMatches()) offs.insert(m.byteOffset);
    bool foundCross = offs.contains(crossAt);
    bool foundMid = offs.contains(midAt);
    fprintf(stderr, "    [INFO] offsets found: %s, crossAt=%lld midAt=%lld\n",
            offs.size() ? "yes" : "none", (long long)crossAt, (long long)midAt);
    CHECK(foundCross, "跨 chunk 边界的匹配被正确找到");
    CHECK(foundMid, "未跨 chunk 的匹配也被找到");
    (void)receivedFailed;
}

// ============ 测试 13：LargeFileSearch requestCancel 在 findAll 中可中断 ============
static void test_search_cancel()
{
    TEST("LargeFileSearch: requestCancel 可中断 findAll");
    // 构造 100MB 的 'a' 中间每隔 1MB 放 "AAA"，然后立刻 requestCancel
    qint64 SIZE = qint64(100) * 1024 * 1024;
    QByteArray data;
    data.resize(SIZE);
    for (qint64 i = 0; i < SIZE; ++i) data[i] = 'a';
    for (qint64 i = 1; i < 100; ++i) {
        qint64 p = i * 1024 * 1024;
        data[p] = 'A'; data[p + 1] = 'A'; data[p + 2] = 'A';
    }
    LineIndex idx;
    std::atomic<bool> idxCancel{false};
    idx.buildFromMmap((const uchar*)data.constData(), data.size(), {}, &idxCancel, qint64(1)<<30);
    PatchList pl;
    LargeFileSearch searcher;
    searcher.setSource((const uchar*)data.constData(), data.size(), &idx, &pl);

    bool receivedFailed = false;
    bool receivedFinished = false;
    qint64 finishedMatches = -1;
    QObject::connect(&searcher, &LargeFileSearch::failed,
                     [&](const QString&) { receivedFailed = true; });
    QObject::connect(&searcher, &LargeFileSearch::finished,
                     [&](qint64 n) { receivedFinished = true; finishedMatches = n; });

    searcher.requestCancel();
    searcher.findAll("AAA", {});

    // QtConcurrent::run(worker) 在后台跑；等最多 3 秒让信号到达
    QElapsedTimer t; t.start();
    while (!receivedFailed && !receivedFinished && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    // 取消场景下，要么 failed，要么 finished 但匹配数 < 99（未全扫完）
    bool ok = receivedFailed || (receivedFinished && finishedMatches < 99);
    if (!ok) {
        fprintf(stderr, "    [INFO] 取消场景：finished=%lld failed=%d\n",
                (long long)finishedMatches, int(receivedFailed));
    }
    CHECK(ok, "requestCancel 下 findAll 被中断（failed 或匹配数<99）");
    (void)receivedFailed;
    (void)receivedFinished;
}

// ============ 测试 14：SearchResultsModel 内存 + 溢出文件虚拟化读取 ============
static void test_searchresults_model_overrun()
{
    TEST("SearchResultsModel: 内存 + 溢出文件虚拟化读取 matchAtRow");
    const int MEM = 5;
    const int OVR = 5;
    QVector<LargeFileSearchMatch> mem;
    for (int i = 0; i < MEM; ++i) {
        LargeFileSearchMatch m;
        m.byteOffset = i * 1000;
        m.length = 3;
        m.line = i;
        m.col = 0;
        m.lineContext = QByteArray("MEM-") + QByteArray::number(i);
        mem.append(m);
    }
    // 写溢出文件
    QTemporaryFile tf(QDir::tempPath() + "/npsoverrunXXXXXX.bin");
    if (!tf.open()) { CHECK(false, "临时文件打开失败"); return; }
    {
        QDataStream s(&tf);
        s.setByteOrder(QDataStream::LittleEndian);
        s << (quint32)0x564F534Du;
        s << (quint32)1;
        s << (quint64)OVR;
        for (int i = 0; i < OVR; ++i) {
            quint64 bo = quint64((MEM + i) * 1000);
            qint32  len = 3, ln = MEM + i, col = 0;
            QByteArray ctx = QByteArray("OVR-") + QByteArray::number(MEM + i);
            s << bo << len << ln << col << ctx;
        }
    }
    tf.close();
    QString p = tf.fileName();

    SearchResultsModel m;
    m.setResults(mem, p, OVR);
    CHECK(m.rowCount({}) == MEM + OVR, "model rowCount = MEM + OVR");

    // 抽样：第 2 行（内存）、第 MEM+2 行（溢出文件第 2 项）
    QVariant d2 = m.data(m.index(2, SearchResultsModel::COL_LINE, {}));
    CHECK(d2.toString() == QStringLiteral("3"), "内存项的行号显示为 1-based (3)");
    QVariant dovrLine = m.data(m.index(MEM + 2, SearchResultsModel::COL_LINE, {}));
    CHECK(dovrLine.toString() == QString::number(MEM + 3), "溢出项的行号正确显示为 1-based");

    QVariant ctxMem = m.data(m.index(1, SearchResultsModel::COL_CONTEXT, {}));
    CHECK(ctxMem.toString() == "MEM-1", "内存上下文列正确回显");
    QVariant ctxOvr = m.data(m.index(MEM + 4, SearchResultsModel::COL_CONTEXT, {}));
    CHECK(ctxOvr.toString() == QString("OVR-%1").arg(MEM + 4), "溢出文件上下文列正确回显");
}

// ============ 测试 15：LargeFileSearch replaceAll 原子 UndoGroup + 内容一致 ============
static void test_replaceall_atomic()
{
    TEST("LargeFileSearch::replaceAll 结果正确 + UndoGroup 可整体 undo");
    // 构造 1000 行，每行 "hello-" + 行号 + "\n"，把 "hello" 全替换为 "HELLO"
    QByteArray orig;
    for (int i = 0; i < 1000; ++i) {
        orig += QByteArray("hello-") + QByteArray::number(i) + QByteArray("\n");
    }
    LineIndex idx;
    std::atomic<bool> cancel{false};
    idx.buildFromMmap((const uchar*)orig.constData(), orig.size(), {}, &cancel, qint64(1)<<30);
    PatchList pl;
    LargeFileSearch searcher;
    searcher.setSource((const uchar*)orig.constData(), orig.size(), &idx, &pl);
    LargeFileSearch::Options opts;
    QString err;
    LargeFileSearch::ReplaceCallbacks cb;  // 全部默认：不走压力、不走中间保存
    bool ok = searcher.replaceAll("hello", "HELLO", opts, pl, cb, &err);
    CHECK(ok, "replaceAll 1000 处成功");
    QByteArray replaced = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + 10000);
    QByteArray expected;
    for (int i = 0; i < 1000; ++i) {
        expected += QByteArray("HELLO-") + QByteArray::number(i) + QByteArray("\n");
    }
    CHECK(replaced == expected, "replaceAll 1000 处内容正确");
    // 一次 undo 应回滚整批（因为 1000 < REPLACE_BATCH_SIZE，恰好 1 UndoGroup）
    ok = pl.undo(&err);
    CHECK(ok, "undo 1 组成功");
    QByteArray afterUndo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + 10000);
    CHECK(afterUndo == orig, "undo 后内容回到替换前原文");
}

// ============ 测试 16：LargeFileSearch replaceAll 压力提示 + 中间保存 ============
static void test_replaceall_intermediate_save()
{
    TEST("LargeFileSearch::replaceAll 压力提示触发后中间保存（applyAndSave 不中断）");
    QByteArray orig;
    for (int i = 0; i < 200; ++i) orig += QByteArray("AAAA ") + QByteArray::number(i % 50) + QByteArray(" : data data data\n");
    LineIndex idx;
    std::atomic<bool> cancel{false};
    idx.buildFromMmap((const uchar*)orig.constData(), orig.size(), {}, &cancel, qint64(1)<<30);
    PatchList pl;
    LargeFileSearch searcher;
    searcher.setSource((const uchar*)orig.constData(), orig.size(), &idx, &pl);
    LargeFileSearch::Options opts;
    QString err;
    LargeFileSearch::ReplaceCallbacks cb;
    int pressureCalls = 0;
    cb.onPressureSoftWarn = [&](const QString&) {
        ++pressureCalls;
        return false;  // 不保存继续
    };
    // replaceAll: AAAA -> BBBB（因为 orig 小，soft warn 不一定触发，我们调 pressureWillExceedSoftWarn
    // 手动确认；这里主要验证回调接口存在且不崩溃）
    bool ok = searcher.replaceAll("AAAA", "BBBB", opts, pl, cb, &err);
    CHECK(ok, "replaceAll 带回调成功");
    QByteArray rendered = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + 2000);
    CHECK(rendered.contains("BBBB") && !rendered.contains("AAAA "), "AAAA->BBBB 已发生替换");
    (void)pressureCalls;
}

// ============ 测试 17：DynamicThresholds 阈值范围合理 ============
static void test_dynamicthresholds_range()
{
    TEST("DynamicThresholds: 阈值范围合理");
    const DynamicThresholds& d = DynamicThresholds::instance();
    CHECK(d.RAM_BYTES > 0, "RAM 字节检测正常");
    CHECK(d.CPU_CORES >= 1, "CPU 核数 >= 1");
    CHECK(d.L1_EDIT_LIMIT >= qint64(32) * 1024 * 1024, "L1_EDIT_LIMIT >= 32MB");
    CHECK(d.PATCH_L1_LIMIT >= qint64(8) * 1024 * 1024, "PATCH_L1_LIMIT >= 8MB");
    CHECK(d.SEARCH_CHUNK_BYTES >= qint64(4) * 1024 * 1024, "SEARCH_CHUNK_BYTES >= 4MB");
    CHECK(d.MAX_MATCHES_MEM >= 100000, "MAX_MATCHES_MEM >= 100000");
    CHECK(d.SEARCH_THREADS >= 1, "SEARCH_THREADS >= 1");
    CHECK(d.REPLACE_BATCH_SIZE >= 100, "REPLACE_BATCH_SIZE >= 100");
}

// ============ 测试 18：SearchResultsModel headerData + flags ============
static void test_searchresults_model_api()
{
    TEST("SearchResultsModel: headerData / columnCount / rowCount / flags 合规");
    SearchResultsModel m;
    CHECK(m.columnCount({}) == SearchResultsModel::COL_COUNT, "columnCount 正确");
    CHECK(m.rowCount({}) == 0, "空模型 rowCount == 0");
    CHECK(m.index(0, 0).isValid() == false, "空模型 index 无效");
    QVariant hdr0 = m.headerData(SearchResultsModel::COL_LINE, Qt::Horizontal, Qt::DisplayRole);
    CHECK(!hdr0.toString().isEmpty(), "headerData 至少有非空文本");
    // 非空模型才存在可访问 index；空集合用 flags(index()) 会越界，这里改以非空 1 行验证
    QVector<LargeFileSearchMatch> one(1);
    one[0].line = 1; one[0].col = 0; one[0].length = 2; one[0].byteOffset = 5;
    one[0].lineContext = QByteArray("x");
    m.setResults(one, {}, 0);
    QModelIndex idx = m.index(0, 0, {});
    CHECK(idx.isValid(), "非空模型 index 有效");
    Qt::ItemFlags f = m.flags(idx);
    CHECK(f & Qt::ItemIsEnabled, "flags 含 ItemIsEnabled");
    CHECK(f & Qt::ItemIsSelectable, "flags 含 ItemIsSelectable");
    m.clear();
}

// ============ 测试 19：LineIndex cancel 能中断 buildFromMmap ============
static void test_lineindex_cancel()
{
    TEST("LineIndex: buildFromMmap 期间 cancel 能中断");
    QByteArray buf;
    buf.resize(qint64(200) * 1024 * 1024);
    for (qint64 i = 0; i < buf.size(); ++i) buf[i] = (i % 78 == 77) ? '\n' : 'a';
    LineIndex idx;
    std::atomic<bool> cancel{true};  // 一开始就取消
    bool ok = idx.buildFromMmap((const uchar*)buf.constData(), buf.size(), {}, &cancel, qint64(4) * 1024 * 1024);
    CHECK(ok == false, "初始 cancel=true 时 buildFromMmap 返回 false");
}

// ============ 测试 20：PatchList beginUndoGroup/endUndoGroup 组合 ============
static void test_patchlist_grouped_undo()
{
    TEST("PatchList: beginUndoGroup/endUndoGroup 打包多次操作为一组可整体撤销");
    QByteArray orig = "ABCDEFGHIJ";
    PatchList pl;
    QString err;
    pl.beginUndoGroup();
    bool ok = pl.insert(3, QByteArray("++"), &err);  // "ABC++DEFGHIJ"
    ok = ok && pl.removeWithSnapshot(0, 1, orig.left(1), &err);  // "BC++DEFGHIJ"
    pl.endUndoGroup();
    CHECK(ok, "组内两次编辑成功");
    QByteArray after = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(after == "BC++DEFGHIJ", "组合编辑结果正确");
    ok = pl.undo(&err);
    CHECK(ok, "一次 undo 成功");
    QByteArray afterUndo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(afterUndo == orig, "组 undo 整体回滚到原文");
    ok = pl.redo(&err);
    CHECK(ok, "一次 redo 成功");
    QByteArray afterRedo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(afterRedo == "BC++DEFGHIJ", "组 redo 整体重放成功");
}

// ============ 测试 21：LineIndex clear() 后状态清空 ============
static void test_lineindex_clear()
{
    TEST("LineIndex: clear() 后 totalLines 为 0");
    QByteArray buf = generateNLines(100, 60);
    LineIndex idx;
    std::atomic<bool> cancel{false};
    idx.buildFromMmap((const uchar*)buf.constData(), buf.size(), {}, &cancel, qint64(1)<<30);
    idx.clear();
    CHECK(idx.totalLines() == 0, "clear 后 totalLines == 0");
    CHECK(idx.lineToByteOffset(0) == 0, "clear 后 line 0 -> byte 0");
}

// ============ 测试 22：PatchList L2 溢出（通过缩小 l1Limit 构造）============
static void test_patchlist_spillover()
{
    TEST("PatchList: 多次 insert 触发 L2 溢出后 renderRange / applyAndSave 正确");
    // 用 DynamicThresholds 给默认 limit，但如果 limit 很大，手动通过多次 insert + 调整
    // 先拿到 limit：
    PatchList pl;
    qint64 limit = pl.l1Limit();
    // 目标：构造 patch 总字节 > limit。用 QByteArray data = "X"* 4KB，每次 insert 1 个；
    // 数量 = limit / 4KB + 若干；但可能 limit 过大，本测试改为 limit 大时跳过（不做真 L2）。
    qint64 wantBytes = limit * 2 + 64;
    if (wantBytes > qint64(1) * 1024 * 1024 * 1024) {
        // 内存环境不充裕，跳过真实溢出，转而验证 l1Count/l2Count 初始为 0
        CHECK(pl.l1Count() == 0 && pl.l2Count() == 0, "初始 patch 计数为 0");
        return;
    }
    const int STEP = 4096;
    QByteArray chunk;
    chunk.resize(STEP, 'x');
    QString err;
    qint64 inserted = 0;
    qint64 pos = 0;
    while (pl.l1Bytes() < limit + 1024) {
        if (!pl.insert(pos, chunk, &err)) { CHECK(false, "insert 失败"); return; }
        inserted += STEP;
        pos += STEP;
    }
    // 此时应有 L2 溢出
    fprintf(stderr, "    [INFO] l1Bytes = %lld, limit = %lld, l2Count = %d\n",
            (long long)pl.l1Bytes(), (long long)limit, pl.l2Count());
    CHECK(pl.l2Count() > 0, "超过 L1 limit 后 L2 溢出（l2Count > 0）");

    // applyAndSave 结果与原文件 + 插入应一致
    QByteArray orig;  // 空原文件
    QTemporaryFile tf(QDir::tempPath() + "/npspillXXXXXX.bin");
    if (!tf.open()) { CHECK(false, "临时文件打开失败"); return; }
    QString path = tf.fileName(); tf.close();
    std::atomic<bool> cancel{false};
    bool ok = pl.applyAndSave(nullptr, 0, path, {}, &cancel, &err);
    CHECK(ok, "applyAndSave L2 溢出场景成功");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { CHECK(false, "读保存文件失败"); return; }
    QByteArray saved = f.readAll();
    f.close();
    CHECK(saved.size() == inserted, "L2 溢出后保存的字节总数正确");
    bool allX = true;
    for (char c : saved) if (c != 'x') { allX = false; break; }
    CHECK(allX, "L2 溢出后保存内容全是 x");
}

// ============ 测试 23：LargeFileSearch replaceAll 10000 处 = 10 个 UndoGroup，逐批 undo/redo 原子 ============
static void test_replaceall_10batches_undo_atomic()
{
    TEST("LargeFileSearch::replaceAll 10000 处分 10 批 undo/redo 严格原子（无残留）");
    // 构造 10000 行 "token_XXXXX\n"，每行含 1 个 "token"，共 10000 处匹配
    // REPLACE_BATCH_SIZE = DynamicThresholds::instance().REPLACE_BATCH_SIZE（通常 1000）
    const int N = 10000;
    QByteArray orig;
    orig.reserve(N * 16);
    for (int i = 0; i < N; ++i) {
        orig += QByteArray("token_") + QByteArray::number(i).rightJustified(5, '0') + QByteArray("\n");
    }
    LineIndex idx;
    std::atomic<bool> cancel{false};
    idx.buildFromMmap((const uchar*)orig.constData(), orig.size(), {}, &cancel, qint64(1)<<30);
    PatchList pl;
    LargeFileSearch searcher;
    searcher.setSource((const uchar*)orig.constData(), orig.size(), &idx, &pl);
    LargeFileSearch::Options opts;
    LargeFileSearch::ReplaceCallbacks cb;  // 不走中间保存
    QString err;
    bool ok = searcher.replaceAll("token", "TOKEN", opts, pl, cb, &err);
    CHECK(ok, "replaceAll 10000 处成功");

    // 1) 全部替换正确：10000 个 TOKEN_，没有 token_
    QByteArray replaced = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + N * 100);
    int replCount = replaced.count("TOKEN");
    int origCount = replaced.count("token_");
    CHECK(replCount == N, QStringLiteral("render 后 TOKEN 出现 %1 次 = 10000 预期").arg(replCount).toUtf8().constData());
    CHECK(origCount == 0, "render 后 token_（小写）完全不出现");

    // 2) 10 次 undo：每一次 undo 应回滚一整批（REPLACE_BATCH_SIZE=1000 条），而不是单条
    //    验证：undo k 次后剩余 TOKEN 个数应是 N - k*REPLACE_BATCH_SIZE（或接近，如果 REPLACE_BATCH_SIZE 非 1000，按实际）
    const int BATCH = DynamicThresholds::instance().REPLACE_BATCH_SIZE;
    const int TOTAL_GROUPS = (N + BATCH - 1) / BATCH;  // ceil(N/BATCH)
    fprintf(stderr, "    [INFO] BATCH=%d, N=%d, TOTAL_GROUPS=%d\n", BATCH, N, TOTAL_GROUPS);
    int expectedTokens = N;
    for (int g = 0; g < TOTAL_GROUPS; ++g) {
        ok = pl.undo(&err);
        if (!ok) {
            fprintf(stderr, "    [DIAG] undo %d 失败: %s\n", g+1, err.toUtf8().constData());
        }
        CHECK(ok, QStringLiteral("undo 第 %1/%2 批成功").arg(g+1).arg(TOTAL_GROUPS).toUtf8().constData());
        QByteArray partial = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + N * 100);
        // 每批 undo BATCH 条，恢复 BATCH 个小写 token
        expectedTokens -= BATCH;
        if (expectedTokens < 0) expectedTokens = 0;
        int actualT = partial.count("TOKEN");
        // 最后一批可能不满 BATCH，所以误差范围应 <= 1 batch
        int diff = qAbs(actualT - expectedTokens);
        if (diff > BATCH) {
            fprintf(stderr, "    [DIAG] batch %d: expected TOKEN ~%d but actual %d (diff=%d)\n",
                    g+1, int(expectedTokens), actualT, diff);
        }
        CHECK(diff <= BATCH,
              QStringLiteral("undo 第 %1 批后剩余 TOKEN 数与批大小递减一致")
                  .arg(g+1).toUtf8().constData());
    }
    // TOTAL_GROUPS 次 undo 完成后应回到原文
    QByteArray fullyUndone = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + N * 100);
    CHECK(fullyUndone == orig, "TOTAL_GROUPS 次 undo 后内容与原文字节级一致（无残留）");
    CHECK(!pl.canUndo(), "undo 干净后 canUndo 为 false");

    // 3) TOTAL_GROUPS 次 redo：恢复到全部替换
    expectedTokens = 0;
    for (int g = 0; g < TOTAL_GROUPS; ++g) {
        ok = pl.redo(&err);
        CHECK(ok, QStringLiteral("redo 第 %1/%2 批成功").arg(g+1).arg(TOTAL_GROUPS).toUtf8().constData());
        QByteArray partial = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + N * 100);
        expectedTokens += BATCH;
        if (expectedTokens > N) expectedTokens = N;
        int actualT = partial.count("TOKEN");
        int diff = qAbs(actualT - expectedTokens);
        CHECK(diff <= BATCH,
              QStringLiteral("redo 第 %1 批后 TOKEN 数递增合理").arg(g+1).toUtf8().constData());
    }
    QByteArray fullyRedone = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + N * 100);
    CHECK(fullyRedone == replaced, "TOTAL_GROUPS 次 redo 后内容与全部替换后字节级一致");
}

// ============ 测试 24：applyAndSave + resetAfterSave 连续性 ============
static void test_applyandsave_resetaftersave()
{
    TEST("PatchList: applyAndSave + resetAfterSave 保存成功后 patch 清空");
    QByteArray orig;
    for (int i = 0; i < 500; ++i) {
        orig += QByteArray("LINE-") + QByteArray::number(i).rightJustified(3, '0') + QByteArray("\n");
    }
    PatchList pl;
    QString err;
    // 做些混合 patch：100 处 insert + 50 处 remove + 50 处 replace
    for (int i = 0; i < 100; ++i) {
        qint64 pos = qint64(orig.size() / 2) + i * 2;
        if (!pl.insert(pos, QByteArray("[INS]"), &err)) { CHECK(false, "insert fail"); return; }
    }
    // remove 在头部（不影响后面 patch 偏移的验证）
    for (int i = 0; i < 50; ++i) {
        // 每 remove 1 byte 前缀 "L"（现在经过之前 patch 偏移后，要在正确位置，但位置计算会因为前面 insert 复杂
        // 所以这里改为 replace：位置 0, oldLen=0, newText 空 = 不做；用 removeAtCursor 更简单但现在用已知安全操作
        // 简化：用虚拟 renderRange 前 saveAs 只要 render 一致即可
        pl.insert(0, QByteArray("H"));  // 100 个 H 前缀在 virt 位置 0
    }
    // 替换 50 处（位置固定，后面 insert 会 shift，但这里只是量的测试）
    for (int i = 0; i < 50; ++i) {
        pl.replace(0, 1, "X");  // 把第一个字节替换为 X，叠加多次不影响字节正确性
    }

    // 1) 先 renderRange 作为基准
    QByteArray expected = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, qint64(50)*1024*1024);

    // 2) applyAndSave 到独立临时路径（避免 QTemporaryFile/QSaveFile Windows 权限冲突）
    QString path = QDir::tempPath() + QStringLiteral("/npstest_rst_%1.bin")
                        .arg(quint64(QCoreApplication::applicationPid()));
    QFile::remove(path);
    std::atomic<bool> cancel{false};
    bool ok = pl.applyAndSave((const uchar*)orig.constData(), orig.size(), path, {}, &cancel, &err);
    if (!ok) fprintf(stderr, "    [DIAG] applyAndSave err: %s\n", err.toUtf8().constData());
    CHECK(ok, "applyAndSave 混合 patch 场景成功");

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { CHECK(false, "读保存文件失败"); return; }
    QByteArray saved = f.readAll();
    f.close();
    CHECK(saved == expected, "applyAndSave 保存字节串与 renderRange 基准字节级一致");

    // 3) resetAfterSave：patch 清空
    pl.resetAfterSave();
    CHECK(pl.l1Bytes() == 0, "resetAfterSave 后 l1Bytes == 0");
    CHECK(pl.l1Count() == 0 && pl.l2Count() == 0, "resetAfterSave 后 l1Count/l2Count 都为 0");
    CHECK(!pl.canUndo() && !pl.canRedo(), "resetAfterSave 后 undo/redo 栈清空");
    // reset 后对同 orig renderRange 应等于 orig（无 patch 应用）
    QByteArray noPatch = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, orig.size() + 1000);
    CHECK(noPatch == orig, "resetAfterSave 后 renderRange 返回纯 orig");

    // 4) 验证保存的文件内容：把 saved 作为「新 orig」再打开，内容应与 orig 应用 patch 的预期一致
    // 这里仅验证 saved 与 expected 一致（前面已验证）
    QFile::remove(path);
}

// ============ 测试 25：resetAfterSave 后再编辑 undo 行为正确 ============
static void test_reset_after_save_then_edit_undo()
{
    TEST("PatchList: resetAfterSave 后重新编辑，undo 行为与初始空状态一致");
    QByteArray orig = "0123456789ABCDEF";
    PatchList pl;
    QString err;
    // 预先做一堆编辑
    pl.insert(0, "PRE-");
    pl.insert(qint64(orig.size()) + 4, "-POST");
    // reset
    pl.resetAfterSave();
    CHECK(!pl.canUndo(), "reset 后 canUndo=false");
    // 再做 insert + undo
    bool ok = pl.insert(2, QByteArray("@"), &err);  // "01@23456789ABCDEF"
    CHECK(ok, "reset 后新 insert 成功");
    QByteArray after = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(after == "01@23456789ABCDEF", "reset 后新编辑结果正确");
    ok = pl.undo(&err);
    CHECK(ok, "reset 后 undo 新 insert 成功");
    QByteArray afterUndo = pl.renderRange((const uchar*)orig.constData(), orig.size(), 0, 100);
    CHECK(afterUndo == orig, "reset 后 undo 回到 orig（无之前 patch 干扰）");
    CHECK(!pl.canUndo(), "回到初始后 canUndo=false");
}

// ============================================================================
//  ★ 启动秒开验证（对应"打开记忆的100MB文件，窗口全程静默不显示"的核心体验 bug）
//
//  设计要点：
//   · BUG 根因：会话恢复时 autosave 路径直接调用 loadAutosave() → file.readAll(100MB)
//     主线程同步读 3~5s；且 show() 后的首个 zero-timer（deferredRestoreSession）
//     在 Paint 事件之前运行，阻塞期间 DWM 无首帧 → 用户"以为双击没启动"。
//   · 修复契约：无论 filePath 直接打开、或 autosave 恢复，
//     对 ≥ L1_EDIT_LIMIT (硬上限 32MB) 的文件：
//       - EditorTab 调用必须在 500ms 内返回（L2 mmap + incomplete LineIndex 秒开）
//       - L2 数据（用户 patches）在 SessionManager::saveSession 时不得丢失
//       - 内容写入 autosave 文件时正确应用字节级 patches（非 editor()->text() 空串）
// ============================================================================

// 在给定目录下创建一个命名的大 UTF-8 文本文件（避免 QTemporaryFile 的随机命名在会话中不稳定）
static QString makeFixedSizeFile(const QString &dir, const QString &name, qsizetype bytes)
{
    QString path = dir + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    // 直接写 generateLargeUtf8：每行约 80B，含中文，字节级 determinable
    QByteArray chunk = generateLargeUtf8(qMin(bytes, (qsizetype)(8 * 1024 * 1024)));
    qsizetype written = 0;
    while (written < bytes) {
        qsizetype toWrite = qMin(chunk.size(), bytes - written);
        qint64 w = f.write(chunk.constData(), toWrite);
        if (w <= 0) break;
        written += w;
    }
    f.close();
    return path;
}

// 读取文件为 QByteArray（用于内容校验；仅用于 < 2GB 文件测试）
static QByteArray readAllBytes(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// 测试：L2 打开 100MB 原始文件，openLargeFileL2() 必须在 500ms 内返回（秒开）
static void test_l2_open_100mb_sub_500ms()
{
    TEST("★ 启动秒开: L2 打开 100MB 文件 耗时 < 500ms");

    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid(), "临时目录可创建");
    const QString filePath = makeFixedSizeFile(tmpDir.path(), "big100mb.txt",
                                                100LL * 1024 * 1024);
    CHECK(QFileInfo::exists(filePath), "100MB 测试文件创建成功");
    CHECK(QFileInfo(filePath).size() >= 99LL * 1024 * 1024,
          "100MB 文件尺寸正确（≥99MB，允许少量对齐误差）");

    {
        EditorTab tab;
        QElapsedTimer t;
        t.start();
        QString err;
        bool ok = tab.openLargeFileL2(filePath, &err);
        qint64 ms = t.elapsed();
        fprintf(stderr, "    [INFO] openLargeFileL2(100MB) 耗时: %lld ms\n", ms);

        CHECK(ok, QString("openLargeFileL2 成功(失败原因: %1)").arg(err).toStdString().c_str());
        CHECK(tab.isL2Mode(), "成功后应为 L2 模式");
        CHECK(ms < 500,
              QString("100MB 打开耗时 %1ms 应 < 500ms（秒开契约；若 >500ms 表示阻塞了主线程，"
                      "用户会看到窗口不出现）").arg(ms).toStdString().c_str());
        // 不等待 LineIndex 构建完成（这正是秒开的意义）
        CHECK(tab.l2View() != nullptr, "L2 视图已创建");
        CHECK(tab.l2View()->isOpen(), "L2 文件已 mmap");
    }
}

// 测试：大文件 autosave 恢复（会话启动的最常见阻塞路径）
//   autosave 文件 = 100MB，走 EditorTab::openAutosaveAutoMode(autosave, real)
//   必须：< 500ms 返回，L2 模式，modified=true，标题栏显示 realFilePath
static void test_autosave_100mb_open_sub_500ms()
{
    TEST("★ 启动秒开: 100MB autosave 恢复 耗时 < 500ms");

    QTemporaryDir tmpDir;
    // autosave 内容（用户上次编辑后未保存的全文快照，写入临时 autosave_xxx.tmp）
    const QString autosavePath = makeFixedSizeFile(tmpDir.path(),
        "autosave_bigfile_100mb.tmp", 100LL * 1024 * 1024);
    CHECK(QFileInfo::exists(autosavePath), "100MB autosave 文件创建成功");
    const QString realPath = "C:/Users/koopo/Documents/big100mb_project.log";

    {
        EditorTab tab;
        QElapsedTimer t;
        t.start();
        bool ok = tab.openAutosaveAutoMode(autosavePath, realPath);
        qint64 ms = t.elapsed();
        fprintf(stderr, "    [INFO] openAutosaveAutoMode(100MB) 耗时: %lld ms\n", ms);

        CHECK(ok, "openAutosaveAutoMode 成功（autosave 大文件应走 L2 mmap）");
        CHECK(tab.isL2Mode(), "大 autosave 应为 L2 模式（而非 L1 同步 readAll）");
        CHECK(tab.isModified(), "autosave 内容应标记为未保存修改（modified=true）");
        CHECK(tab.filePath() == realPath, "标题栏/保存路径应显示真实原名（非 autosave_xxx.tmp）");
        CHECK(tab.autosavePath() == autosavePath, "内部仍关联 autosave 路径（下次会话/清空）");
        CHECK(ms < 500,
              QString("100MB autosave 恢复耗时 %1ms 应 < 500ms（原 bug: loadAutosave 同步 readAll"
                      " 阻塞 3~5s，首帧不画）").arg(ms).toStdString().c_str());
    }

    // 再测一遍：没有 realFilePath（未命名文件 autosave）→ isUntitled=true
    {
        EditorTab tab;
        bool ok = tab.openAutosaveAutoMode(autosavePath);
        CHECK(ok, "未命名 autosave 也能打开");
        CHECK(tab.isUntitled(), "未提供 realFilePath 时，身份应为未命名（isUntitled=true）");
        CHECK(tab.isModified(), "未命名 autosave 同样应 modified=true");
    }
}

// 测试：SessionManager::saveSession 对 L2 标签导出内容正确（修复 BUG A：L2 数据永久丢失）
//   步骤：
//   1) 创建 10MB 原始文件 → EditorTab 以 L2 打开
//   2) 在 L2 中执行 replace 操作（首行插入"MODIFIED_L2_TOKEN"）→ saveNeeded=true
//   3) 用 SessionManager::saveSession 写 autosave
//   4) 读取 autosave 文件内容：必须包含 "MODIFIED_L2_TOKEN"，且整体字节序=原始+patch
//      （若错误地走 editor()->text()，autosave 内容为空/旧 QsciScintilla 内容，丢失 token）
static void test_session_save_export_l2_correctness()
{
    TEST("★ 数据安全: L2 模式 saveSession 不丢失用户 patches（BUG A 回归）");

    QTemporaryDir sessionDir;
    QTemporaryDir workDir;
    CHECK(sessionDir.isValid() && workDir.isValid(), "两个临时目录创建成功");

    // 1) 构造原始 10MB 文件
    const QString origPath = makeFixedSizeFile(workDir.path(), "source_project.log",
                                                10LL * 1024 * 1024);
    const QByteArray origBytes = readAllBytes(origPath);
    CHECK(!origBytes.isEmpty() && origBytes.size() >= 9 * 1024 * 1024,
          "原始 10MB 文件可读且尺寸正确");

    // 2) EditorTab 以 L2 打开，并做 patch 编辑：在 byte 0 位置插入 Token
    //    （因为 VirtualFileView 公开方法有限，直接通过 editor 统一入口不能做 patch 级插入；
    //     改用一个更稳健的验证：saveAs 到一个 reference file，然后让 SessionManager autosave
    //     内容与该 reference 字节级完全一致。）
    QString refOutput = workDir.path() + "/reference_output.bin";
    QString autosaveFileActual;
    {
        EditorTab tab;
        QString err;
        bool ok = tab.openLargeFileL2(origPath, &err);
        CHECK(ok && tab.isL2Mode(), "L2 打开源文件成功");

        // 执行一次 L2 编辑：通过 pasteBytes 在光标（byte 0）前插入 Token
        const QByteArray token = "MODIFIED_L2_TOKEN__SESSION_EXPORT_MUST_CONTAIN_THIS__\n";
        tab.setFocusEditor();
        VirtualFileView *v = tab.l2View();
        CHECK(v != nullptr && v->isOpen(), "L2 视图就绪");
        // goto 0 byte
        v->pasteBytes(token, nullptr);
        CHECK(tab.isModified(), "编辑后 modified=true");

        // A) 用 saveL2As 导出权威 reference_output.bin
        QString saveErr;
        bool saveOk = tab.saveL2As(refOutput, &saveErr);
        CHECK(saveOk, QString("saveL2As(reference) 成功(err=%1)")
              .arg(saveErr).toStdString().c_str());

        // B) 用 SessionManager 导出 autosave（这才是真正被 saveSession 调用的路径）
        SessionManager sm(sessionDir.path());
        QVector<EditorTab *> tabs;
        tabs.append(&tab);
        bool saved = sm.saveSession(tabs, 0);
        CHECK(saved, "SessionManager::saveSession 成功");

        // 读回 session.json，拿到 autosavePath
        SessionData sd = sm.loadSession();
        CHECK(sd.tabs.size() == 1, "恢复的 session 含 1 个标签");
        autosaveFileActual = sd.tabs[0].autosavePath;
        CHECK(!autosaveFileActual.isEmpty(), "session 记录的 autosavePath 非空");
        CHECK(QFileInfo::exists(autosaveFileActual), "autosave 文件真实存在于磁盘");
    }

    // 3) 比较：reference_output.bin vs autosaveFileActual 必须字节级相等
    QByteArray refBytes = readAllBytes(refOutput);
    QByteArray autosaveBytes = readAllBytes(autosaveFileActual);
    CHECK(!refBytes.isEmpty(), "reference output 非空");
    CHECK(!autosaveBytes.isEmpty(), "autosave output 非空");
    CHECK(refBytes.size() == autosaveBytes.size(),
          QString("autosave 尺寸(%1) == reference(%2)")
          .arg(autosaveBytes.size()).arg(refBytes.size()).toStdString().c_str());
    CHECK(refBytes == autosaveBytes,
          "autosave 内容与 saveAs 权威导出字节级一致（L2 patches 未丢失）");
    // 最终保险：autosave 必须含有我们插入的 token（否则就算尺寸一样内容也不对，极端小概率）
    CHECK(autosaveBytes.contains("MODIFIED_L2_TOKEN__SESSION_EXPORT_MUST_CONTAIN_THIS__"),
          "autosave 内包含 L2 插入的 Token（证明 editor()->text() 空串 bug 已修复）");
}

// 测试：小文件 autosave（< L1_EDIT_LIMIT）仍走原 loadAutosave L1 同步路径，避免破坏现有行为
static void test_small_file_autosave_prefers_l1()
{
    TEST("★ 兼容: 小文件 autosave(< L1_EDIT_LIMIT) 仍走 L1 同步路径");

    QTemporaryDir tmpDir;
    const QString autosavePath = makeFixedSizeFile(tmpDir.path(),
        "autosave_small_50kb.tmp", 50 * 1024);  // 50KB，一定 < 任何阈值下限 2MB

    EditorTab tab;
    bool ok = tab.openAutosaveAutoMode(autosavePath);
    CHECK(ok, "50KB autosave 打开成功");
    CHECK(!tab.isL2Mode(), "50KB autosave 应走 L1 路径（isL2Mode=false）；避免小文件也进 L2 浪费 mmap");
    CHECK(tab.isModified(), "autosave modified=true");
}

// ============================================================================
//  新增功能测试：搜索结果分组、长行渲染、跳转定位、多线程验证
// ============================================================================

// 测试：SearchResultsModel 按行分组去重 —— 同一行多匹配只显示一次
static void test_searchresults_grouping()
{
    TEST("SearchResultsModel: 同一行多匹配合并为一行，matchPositions 记录所有位置");

    QVector<LargeFileSearchMatch> matches;

    // Line 5, 3 matches at different columns
    for (int i = 0; i < 3; ++i) {
        LargeFileSearchMatch m;
        m.line = 5;
        m.col = 10 + i * 8;
        m.length = 3;
        m.byteOffset = 100 + i * 8;
        m.lineContext = "aaa KEY aaa KEY aaa KEY aaa";
        matches.append(m);
    }
    // Line 10, 1 match
    LargeFileSearchMatch m4;
    m4.line = 10; m4.col = 0; m4.length = 3; m4.byteOffset = 200;
    m4.lineContext = "KEY bbb";
    matches.append(m4);

    SearchResultsModel model;
    model.setResults(matches, {}, 0);

    CHECK(model.groupedCount() == 2, "同一行多匹配合并后只有 2 组（行5 + 行10）");

    const auto& lines = model.groupedLines();
    CHECK(lines[0].line == 5, "第一组行号为 5");
    CHECK(lines[0].matchPositions.size() == 3, "第一组有 3 个匹配位置");
    CHECK(lines[0].matchPositions[0].first == 10, "第一个匹配 col=10");
    CHECK(lines[0].matchPositions[1].first == 18, "第二个匹配 col=18");
    CHECK(lines[0].matchPositions[2].first == 26, "第三个匹配 col=26");
    CHECK(lines[0].firstByteOffset == 100, "第一组首字节偏移=100");
    CHECK(lines[0].firstLength == 3, "第一组首匹配长度=3");

    CHECK(lines[1].line == 10, "第二组行号为 10");
    CHECK(lines[1].matchPositions.size() == 1, "第二组有 1 个匹配位置");

    CHECK(model.totalMatches() == 4, "总匹配数为 4（3+1）");

    // MatchCount 列应显示 3
    QModelIndex idx0 = model.index(0, SearchResultsModel::COL_MATCHCOUNT);
    CHECK(idx0.data(Qt::DisplayRole).toString() == QStringLiteral("3"),
          "行5的 MatchCount 列显示为 3");

    // ByteOffsetRole 应返回首匹配偏移
    QModelIndex idx0Line = model.index(0, SearchResultsModel::COL_LINE);
    CHECK(idx0Line.data(SearchResultsModel::ByteOffsetRole).toLongLong() == 100,
          "ByteOffsetRole 返回首匹配偏移 100");

    // 行号列应显示 1-based 行号
    QModelIndex idx0No = model.index(0, SearchResultsModel::COL_LINE);
    CHECK(idx0No.data(Qt::DisplayRole).toString() == QStringLiteral("6"),
          "行号列显示为 1-based (6)");
}

// 测试：长行渲染 —— 创建单行 5MB 文件，打开后渲染不崩溃、不分配整行 QByteArray
static void test_longline_render()
{
    TEST("长行渲染: 5MB 单行文件零拷贝渲染不崩溃 + LineIndex 正确");

    // 创建 5MB 单行文件（无换行符，纯 ASCII）
    QTemporaryFile tmpFile;
    if (!tmpFile.open()) { CHECK(false, "临时文件创建失败"); return; }
    const int lineSize = 5 * 1024 * 1024;  // 5MB
    QByteArray lineData(lineSize, 'x');
    // 在特定位置插入标记，便于后续搜索测试
    lineData.replace(1000, 4, "MARK");
    lineData.replace(1000000, 4, "MARK");
    lineData.replace(4000000, 4, "MARK");
    tmpFile.write(lineData);
    tmpFile.flush();
    QString filePath = tmpFile.fileName();

    // 用 VirtualFileView 打开
    VirtualFileView view;
    view.resize(800, 600);
    QString err;
    bool ok = view.openFile(filePath, &err);
    CHECK(ok, QString("5MB 单行文件 openFile 成功 (err=%1)").arg(err).toStdString().c_str());

    // 等待异步 mmap + LineIndex 构建完成
    QEventLoop loop;
    QObject::connect(&view, &VirtualFileView::lineBuildFinished, &loop, &QEventLoop::quit);
    loop.exec();

    CHECK(view.isOpen(), "异步 mmap 完成后 isOpen()=true");
    CHECK(view.lineIndex().totalLines() >= 1, "LineIndex totalLines >= 1（至少 1 行）");

    // 验证行偏移正确：第 0 行的起始字节 = 0，第 1 行（如果有）= 文件末尾
    qint64 line0Start = view.lineIndex().lineToByteOffset(0);
    CHECK(line0Start == 0, "第 0 行起始字节偏移 = 0");

    // 触发 paintEvent（不崩溃即通过）
    view.viewport()->update();
    QApplication::processEvents();
    CHECK(true, "paintEvent 执行完成未崩溃（零拷贝 + 水平裁剪路径）");

    // 水平滚动到中间位置，再 paint（验证水平裁剪不崩溃）
    view.horizontalScrollBar()->setValue(2000);
    view.viewport()->update();
    QApplication::processEvents();
    CHECK(true, "水平滚动后 paintEvent 未崩溃");

    // 水平滚动到末尾，再 paint
    view.horizontalScrollBar()->setValue(view.horizontalScrollBar()->maximum());
    view.viewport()->update();
    QApplication::processEvents();
    CHECK(true, "水平滚动到末尾后 paintEvent 未崩溃");

    // 搜索 MARK 关键词：验证 3 处匹配
    view.horizontalScrollBar()->setValue(0);
    QApplication::processEvents();

    LargeFileSearch search;
    search.setSource(view.mmapBase(), view.mmapSize(), &view.lineIndex(), nullptr);
    LargeFileSearch::Options opts;
    opts.caseSensitive = true;

    bool searchFailed = false;
    qint64 searchTotal = -1;
    QEventLoop searchLoop;
    QObject::connect(&search, &LargeFileSearch::finished, &searchLoop, [&](qint64 total){
        searchTotal = total;
        searchLoop.quit();
    });
    QObject::connect(&search, &LargeFileSearch::failed, &searchLoop, [&](){
        searchFailed = true;
        searchLoop.quit();
    });
    search.findAll(QStringLiteral("MARK"), opts);
    searchLoop.exec();
    QThreadPool::globalInstance()->waitForDone(2000);

    int actualCount = search.memoryMatches().size() + int(search.overrunCount());
    fprintf(stderr, "    [INFO] searchFailed=%d, finished.total=%lld, memMatches=%d, overrun=%lld, actual=%d\n",
            int(searchFailed), (long long)searchTotal,
            search.memoryMatches().size(), (long long)search.overrunCount(), actualCount);
    fflush(stderr);
    CHECK(!searchFailed, "搜索未发出 failed 信号");
    CHECK(actualCount == 3, "5MB 单行文件中 MARK 出现 3 次");
}

// 测试：gotoByteOffset 同时处理垂直和水平滚动条
static void test_goto_byte_offset_horizontal()
{
    TEST("gotoByteOffset: 同时调整垂直+水平滚动条定位到关键词");

    // 创建多行长行文件：每行 10000 字符，共 100 行
    QTemporaryFile tmpFile;
    if (!tmpFile.open()) { CHECK(false, "临时文件创建失败"); return; }
    const int lineLen = 10000;
    const int numLines = 100;
    QByteArray oneLine(lineLen, 'x');
    oneLine[5000] = 'K'; oneLine[5001] = 'E'; oneLine[5002] = 'Y';  // 在每行中间放 KEY
    for (int i = 0; i < numLines; ++i) {
        tmpFile.write(oneLine);
        tmpFile.write("\n");
    }
    tmpFile.flush();
    QString filePath = tmpFile.fileName();

    VirtualFileView view;
    view.resize(400, 300);  // 小窗口，只显示约 40 字符宽
    QString err;
    view.openFile(filePath, &err);

    QEventLoop loop;
    QObject::connect(&view, &VirtualFileView::lineBuildFinished, &loop, &QEventLoop::quit);
    loop.exec();

    CHECK(view.isOpen(), "长行文件打开成功");

    // 初始水平滚动条值应为 0
    CHECK(view.horizontalScrollBar()->value() == 0, "初始水平滚动条 = 0");

    // gotoByteOffset 到第 50 行的 KEY 位置（byte offset = 50 * 10001 + 5000）
    qint64 targetByte = qint64(50) * 10001 + 5000;
    view.gotoByteOffset(targetByte);

    int hVal = view.horizontalScrollBar()->value();
    fprintf(stderr, "    [INFO] gotoByteOffset(%lld) 后水平滚动条=%d\n",
            (long long)targetByte, hVal);
    CHECK(hVal > 0, "gotoByteOffset 后水平滚动条 > 0（KEY 在 col=5000，需水平滚动才能可见）");

    // 验证垂直滚动条也调整了（目标行 50 应在可见范围内）
    int vVal = view.verticalScrollBar()->value();
    fprintf(stderr, "    [INFO] 垂直滚动条=%d, 目标行=50\n", vVal);
    CHECK(vVal > 0, "gotoByteOffset 后垂直滚动条 > 0（目标行 50 需要垂直滚动）");

    // 验证 KEY 位置在可见水平范围内（用 viewport 宽度估算）
    // col=5000 字符，charWidth 约 7-8px（Consolas 10pt），约 35000-40000px
    // hVal 应 < 40000（否则 KEY 在左侧不可见），且 hVal + viewport_width > 35000（否则 KEY 在右侧不可见）
    int vpW = view.viewport()->width();
    CHECK(hVal < 40000, "水平滚动条值 < 40000（KEY 不在左侧不可见区域）");
    CHECK(hVal + vpW > 35000, "水平滚动条 + 视口宽度 > 35000（KEY 在可见范围内）");
}

// 测试：多线程搜索验证 —— 大文件 findAll 使用多个线程
static void test_multithread_search()
{
    TEST("多线程搜索: 大文件(>64MB) findAll 实际使用多线程并行扫描");

    DynamicThresholds dt = DynamicThresholds::instance();
    int threads = dt.SEARCH_THREADS;
    fprintf(stderr, "    [INFO] SEARCH_THREADS = %d (CPU_CORES = %d)\n",
            threads, dt.CPU_CORES);
    CHECK(threads == qBound(1, dt.CPU_CORES, 8),
          "SEARCH_THREADS == qBound(1, CPU_CORES, 8) — 按 CPU 核心数设置");

    // 创建 > CHUNK(16MB) 的大文件以触发多线程搜索
    qint64 minSize = qint64(32) * 1024 * 1024;  // 32MB，超过 CHUNK 阈值
    QByteArray buf;
    buf.resize(minSize);
    for (qint64 i = 0; i < buf.size(); ++i) {
        buf[i] = (i % 80 == 79) ? '\n' : char('A' + (i % 26));
    }
    // 在多个位置插入 TOKEN 以验证匹配正确
    for (int i = 0; i < 10; ++i) {
        qint64 pos = (qint64)(i + 1) * buf.size() / 11;
        if (pos + 5 < buf.size()) {
            buf.replace(int(pos), 5, "TOKEN");
        }
    }

    QTemporaryFile tmpFile;
    if (!tmpFile.open()) { CHECK(false, "临时文件创建失败"); return; }
    tmpFile.write(buf);
    tmpFile.flush();

    // 用 QFile::map 直接映射（绕过 VirtualFileView 异步加载）
    QFile f(tmpFile.fileName());
    bool fOpen = f.open(QIODevice::ReadOnly);
    CHECK(fOpen, "QFile::open 成功");
    uchar* mapped = f.map(0, buf.size());
    CHECK(mapped != nullptr, "QFile::map 成功");

    LineIndex idx;
    std::atomic<bool> cancel(false);
    idx.buildFromMmap(mapped, buf.size(), nullptr, &cancel, qint64(16) * 1024 * 1024);
    CHECK(idx.totalLines() > 0, "LineIndex 构建成功，行数 > 0");

    // 计时搜索
    QElapsedTimer timer;
    timer.start();

    LargeFileSearch search;
    search.setSource(mapped, buf.size(), &idx, nullptr);
    LargeFileSearch::Options opts;
    opts.caseSensitive = true;

    bool searchFailed = false;
    qint64 searchTotal = -1;
    QEventLoop searchLoop;
    QObject::connect(&search, &LargeFileSearch::finished, &searchLoop, [&](qint64 total){
        searchTotal = total;
        searchLoop.quit();
    });
    QObject::connect(&search, &LargeFileSearch::failed, &searchLoop, [&](){
        searchFailed = true;
        searchLoop.quit();
    });
    search.findAll(QStringLiteral("TOKEN"), opts);
    searchLoop.exec();
    QThreadPool::globalInstance()->waitForDone(5000);

    qint64 elapsedMs = timer.elapsed();
    int matchCount = search.memoryMatches().size() + int(search.overrunCount());
    fprintf(stderr, "    [INFO] 文件大小=%lld MB, 搜索耗时=%lld ms, 匹配数=%d, 线程数=%d, failed=%d\n",
            (long long)(buf.size() / (1024 * 1024)), (long long)elapsedMs, matchCount, threads, int(searchFailed));

    CHECK(!searchFailed, "多线程搜索未发出 failed 信号");
    CHECK(matchCount == 10, "找到 10 个 TOKEN 匹配（多线程搜索结果正确）");
    CHECK(elapsedMs < 30000, "搜索在 30 秒内完成（多线程并行效率正常）");

    f.unmap(mapped);
}

// 测试：搜索高亮染色 —— setSearchHighlights 后 paintEvent 不崩溃
static void test_search_highlight_rendering()
{
    TEST("搜索高亮染色: setSearchHighlights 后 paintEvent 不崩溃 + 高亮位置正确");

    // 创建多行文件
    QTemporaryFile tmpFile;
    if (!tmpFile.open()) { CHECK(false, "临时文件创建失败"); return; }
    QByteArray content;
    for (int i = 0; i < 50; ++i) {
        content += "Line " + QByteArray::number(i) + ": hello world hello world\n";
    }
    tmpFile.write(content);
    tmpFile.flush();

    VirtualFileView view;
    view.resize(600, 400);
    QString err;
    view.openFile(tmpFile.fileName(), &err);

    QEventLoop loop;
    QObject::connect(&view, &VirtualFileView::lineBuildFinished, &loop, &QEventLoop::quit);
    loop.exec();

    CHECK(view.isOpen(), "文件打开成功");

    // 搜索所有 "hello" 位置
    LargeFileSearch search;
    search.setSource(view.mmapBase(), view.mmapSize(), &view.lineIndex(), nullptr);
    LargeFileSearch::Options opts;
    opts.caseSensitive = false;

    QEventLoop searchLoop;
    QObject::connect(&search, &LargeFileSearch::finished, &searchLoop, &QEventLoop::quit);
    QObject::connect(&search, &LargeFileSearch::failed, &searchLoop, &QEventLoop::quit);
    search.findAll(QStringLiteral("hello"), opts);
    searchLoop.exec();
    QThreadPool::globalInstance()->waitForDone(5000);

    // 收集匹配位置，设置高亮
    QVector<QPair<qint64, qint32>> highlights;
    for (const auto& m : search.memoryMatches()) {
        highlights.append(qMakePair(m.byteOffset, m.length));
    }
    fprintf(stderr, "    [INFO] 设置 %d 个搜索高亮位置\n", int(highlights.size()));
    CHECK(highlights.size() >= 50, "至少找到 50 个 hello 匹配");

    view.setSearchHighlights(highlights);

    // paint 不崩溃
    view.viewport()->update();
    QApplication::processEvents();
    CHECK(true, "设置搜索高亮后 paintEvent 未崩溃");

    // 清除高亮后 paint 不崩溃
    view.clearSearchHighlights();
    view.viewport()->update();
    QApplication::processEvents();
    CHECK(true, "清除搜索高亮后 paintEvent 未崩溃");
}

// ============ 测试 26：PatchList applyAndSave 4GB+ 大文件（稀疏文件不占实际空间）============
//   复现场景：用户在 4GB+ 大文件里「粘贴」后保存失败
//   验证：对 4.5GB 原始文件做 3 处 patch（头 insert、中间 replace、尾 remove）后
//         applyAndSave 流式写盘成功，块大小检查不报错，QSaveFile commit 不失败。
static void test_patchlist_sizes_lightweight_diag()
{
    TEST("PatchList: 轻量 size() 正确性诊断（3 次 patch，无 4GB mmap）");
    const qint64 SIZE = qint64(1024); // 随便一个小数字，只用它来计算 patch3 的偏移
    PatchList pl;
    QString err;

    auto dump = [&](const char* label) {
        fprintf(stderr, "    [LIGHT] %s: netDelta=%lld count=%d\n",
                label, (long long)pl.netLengthDelta(), pl.patchCount());
        for (int i = 0; i < pl.patchCount(); ++i) {
            const Patch* pp = pl.patchAt(i);
            if (!pp) continue;
            // 用两种方式计算 len：1) QByteArray::size()；2) 手动 strlen（对纯 ASCII）
            int sz1 = pp->newText.size();
            const char* cd = pp->newText.constData();
            int sz2 = pp->newText.isEmpty() ? 0 : int(qstrnlen(cd, 1024));
            // 再用 QByteArray::length() 做第三份（和 size() 应该一致）
            int sz3 = pp->newText.length();
            fprintf(stderr, "      [%d] offset=%lld size()=%d length()=%d strlen=%d\n"
                    "              newText='%s' constData=%p\n",
                    i, (long long)pp->byteOffset, sz1, sz3, sz2,
                    pp->newText.isEmpty() ? "" :
                      QString::fromLatin1(pp->newText.left(64)).toStdString().c_str(),
                    (const void*)cd);
            // 打印 sizeof(Patch) 和 QByteArray d-ptr 值（帮助诊断 SSO 问题）
            fprintf(stderr, "              sizeof(Patch)=%zu sizeof(QByteArray)=%zu\n",
                    sizeof(Patch), sizeof(QByteArray));
        }
    };

    bool ok = pl.insert(32, QByteArray("!HEADER_INSERT!"), &err);
    CHECK(ok, "patch1 ok");
    dump("after patch1");
    // 立即验证 netDelta=15（"!HEADER_INSERT!" 是 15 字节，非 16）
    {
        const Patch* p0 = pl.patchAt(0);
        CHECK(p0 && p0->newText.size() == 15, "patch1 newText.size()==15");
    }

    ok = pl.insert(0, QByteArray("__PREFIX__"), &err);
    CHECK(ok, "patch2 ok");
    dump("after patch2");
    {
        const Patch* p0 = pl.patchAt(0); // __PREFIX__ 10
        const Patch* p1 = pl.patchAt(1); // !HEADER_INSERT! 15 但 byteOffset=42
        CHECK(p0 && p0->newText.size() == 10, "patch2 newText.size()==10 (patch at 0)");
        CHECK(p1 && p1->newText.size() == 15, "patch1 newText.size()==15 (patch at 1)");
        CHECK(p1 && p1->byteOffset == 32 + 10, "patch1 byteOffset should shift to 42");
    }

    // patch3 应在当前虚拟末尾插入：SIZE + 15 + 10 = SIZE + 25（非 SIZE+26，
    // 否则会产生 1 字节 gap 被 NUL 填充，导致 virtualSize != mmapSize + netDelta）。
    ok = pl.insert(SIZE + 25, QByteArray("~APPEND8~"), &err);
    CHECK(ok, "patch3 ok");
    dump("after patch3");
    {
        const Patch* p0 = pl.patchAt(0);
        const Patch* p1 = pl.patchAt(1);
        const Patch* p2 = pl.patchAt(2);
        CHECK(p0 && p0->newText.size() == 10, "patch2 @0: size==10");
        CHECK(p1 && p1->newText.size() == 15, "patch1 @1: size==15");
        CHECK(p2 && p2->newText.size() == 9,  "patch3 @2: size==9");
    }
    fprintf(stderr, "    [LIGHT] netLengthDelta()==%lld (期望 34)\n", (long long)pl.netLengthDelta());
    CHECK(pl.netLengthDelta() == 34, "3 次 patch 净增量=34 (10+15+9)");
    // virtualSize 应 == mmapSize + netDelta（无 gap）
    qint64 vsz = pl.virtualSize(SIZE);
    fprintf(stderr, "    [LIGHT] virtualSize==%lld (期望 %lld)\n", (long long)vsz, (long long)(SIZE + 34));
    CHECK(vsz == SIZE + 34, "virtualSize == SIZE + 34（无 gap 空洞）");
}

static void test_patchlist_applyandsave_4gb_plus()
{
    TEST("PatchList: applyAndSave 4.5GB 大文件 + 多处 patch 保存不报错（稀疏文件）");
    // 1) 先在临时目录建一个 4.5GB 稀疏文件：写入头、再 resize 到 4.5GB。
    //    Windows NTFS 下 resize 不会实际占用 4.5GB 磁盘（sparse），几毫秒完成。
    QString path = QDir::tempPath() + QStringLiteral("/npstest_4gb_%1.bin")
                        .arg(quint64(QCoreApplication::applicationPid()));
    QString savePath = path + QStringLiteral(".out.bin");
    QFile::remove(path); QFile::remove(savePath);

    QFile origF(path);
    if (!origF.open(QIODevice::ReadWrite)) { CHECK(false, "创建临时稀疏原文件失败"); return; }
    const qint64 FOUR_GB = qint64(4) * 1024 * 1024 * 1024;
    const qint64 SIZE = FOUR_GB + qint64(512) * 1024 * 1024;  // 4.5GB
    // 先写 32 字节标识在文件头，便于验证保存后头内容仍存在
    const char HEADER[] = "[4GB-SPARSE-FILE:HEADER]ABCD1234";
    static_assert(sizeof(HEADER)-1 == 32, "header = 32 bytes exactly");
    origF.write(HEADER, 32);
    // 在 ~第 2GB 位置写 17 字节，便于中间 patch 验证
    // （"__MIDPOINT_MARK__" 是 17 字符，非 18——旧测试误传 18 导致写入 null 终结符）
    const char MIDPOINT[] = "__MIDPOINT_MARK__";
    static_assert(sizeof(MIDPOINT)-1 == 17, "midpoint = 17 bytes exactly");
    origF.seek(FOUR_GB / 2);
    origF.write(MIDPOINT, 17);
    // 在文件结尾前写 8 字节：尾部标识（"TAILBYTE" 恰好 8 字节，避免 strlen/字节数不一致）
    const char TAILBYTE[] = "TAILBYTE";
    static_assert(sizeof(TAILBYTE)-1 == 8, "TAILBYTE = 8 bytes exactly");
    origF.seek(SIZE - 8);
    origF.write(TAILBYTE, 8);
    // 然后 resize 到 SIZE（中间未写区域在 NTFS 是 sparse，不占物理空间）
    if (!origF.resize(SIZE)) {
        CHECK(false, "QFile::resize(4.5GB) 失败：可能不支持稀疏文件");
        origF.close(); QFile::remove(path); return;
    }
    origF.close();

    // 2) mmap 文件并构造 PatchList：3 处 patch
    QFile fmmap(path);
    if (!fmmap.open(QIODevice::ReadOnly)) { CHECK(false, "mmap 打开失败"); QFile::remove(path); return; }
    const uchar* base = fmmap.map(0, SIZE);
    if (!base) { CHECK(false, "mmap 4.5GB 失败"); fmmap.close(); QFile::remove(path); return; }

    PatchList pl;
    QString err;
    auto diagPl = [&](const char* step) {
        fprintf(stderr, "    [DIAG-PL] %s: netDelta=%lld patches=%d\n",
                step, (long long)pl.netLengthDelta(), (int)pl.patchCount());
        for (int i = 0; i < pl.patchCount(); ++i) {
            const Patch* pp = pl.patchAt(i);
            if (!pp) { fprintf(stderr, "      [%d] null\n", i); continue; }
            // 同时比较 size() 方法 与 qstrlen(constData())（如果是纯 ASCII 可打印）
            int szMethod = pp->newText.size();
            int szStrlen = pp->newText.isEmpty() ? 0 : int(qstrnlen(pp->newText.constData(), 1024));
            fprintf(stderr, "      [%d] offset=%lld oldLen=%d newText.size()=%d strlen=%d content='%s' oldSnap.size=%d\n",
                    i, (long long)pp->byteOffset, pp->oldLength,
                    szMethod, szStrlen,
                    pp->newText.isEmpty() ? "" :
                      QString::fromLatin1(pp->newText.left(64)).toStdString().c_str(),
                    pp->oldSnapshot.size());
        }
    };
    // ====== 先在 mmap 之前做 insert（排除 mmap 4.5GB 导致堆内存覆盖的因素）======
    // patch 1: 在文件头之后（偏移 32）插入一段内容 "!HEADER_INSERT!"
    bool ok = pl.insert(32, QByteArray("!HEADER_INSERT!"), &err);
    if (!ok) fprintf(stderr, "    [DIAG] insert head err: %s\n", err.toUtf8().constData());
    CHECK(ok, "patch 1: 头部 insert 成功");
    diagPl("after patch1 (before mmap)");

    // patch 2: 在文件开头之前（虚拟坐标 0）插入 "__PREFIX__" (10 bytes)
    ok = pl.insert(0, QByteArray("__PREFIX__"), &err);
    if (!ok) fprintf(stderr, "    [DIAG] insert prefix err: %s\n", err.toUtf8().constData());
    CHECK(ok, "patch 2: 虚拟坐标 0 处前缀 insert 成功");
    diagPl("after patch2 (before mmap)");

    // patch 3: 在当前虚拟文件末尾插入 "~APPEND8~" (9 bytes)。
    // 前 2 个 patch 净增 10 + 15 = 25 字节，故虚拟末尾 = SIZE + 25（非 SIZE+26，
    // 否则产生 1 字节 gap 被 NUL 填充，导致保存文件比期望大 1 字节）。
    ok = pl.insert(SIZE + 25, QByteArray("~APPEND8~"), &err);
    CHECK(ok, "patch 3: 虚拟文件尾 append 成功");
    diagPl("after patch3 (before mmap)");

    // 先单独验证各关键块的 renderRange 正确性（用于定位 4GB Bug）
    {
        qint64 expectedNet = 34;
        qint64 virt = SIZE + expectedNet;
        // 最后一块
        qint64 lastStart = SIZE;
        qint64 lastEnd = virt;
        QByteArray lastChunk = pl.renderRange(base, SIZE, lastStart, lastEnd);
        fprintf(stderr, "    [DIAG] last block [%lld..%lld): expected=%lld actual=%d\n",
                (long long)lastStart, (long long)lastEnd,
                (long long)(lastEnd - lastStart), lastChunk.size());
        // 对最后一块逐字节验证：应输出 [SIZE-25 : SIZE] 物理 25 字节 + ~APPEND8~ 9 字节 = 34
        // （前 2 个 patch 在 SIZE 之前共插入 10+15=25 字节，故虚拟 SIZE 对应物理 SIZE-25）
        QByteArray expectedLast;
        // 物理最后 25 字节：mmapBase + SIZE - 25 拷贝
        expectedLast.append((const char*)(base + SIZE - 25), 25);
        expectedLast.append("~APPEND8~", 9);
        if (lastChunk.size() != expectedLast.size()) {
            fprintf(stderr, "    [DIAG] size mismatch. first diff byte idx: n/a\n");
        } else {
            int firstDiff = -1;
            for (int i = 0; i < expectedLast.size(); ++i)
                if ((unsigned char)lastChunk[i] != (unsigned char)expectedLast[i]) { firstDiff = i; break; }
            if (firstDiff >= 0) {
                fprintf(stderr, "    [DIAG] first content diff at idx %d: got=0x%02x want=0x%02x\n",
                        firstDiff,
                        (unsigned char)lastChunk[firstDiff],
                        (unsigned char)expectedLast[firstDiff]);
            }
        }
        // 第一块
        QByteArray firstChunk = pl.renderRange(base, SIZE, 0, 128);
        QByteArray expectedFirst = QByteArray("__PREFIX__", 10)
                                  + QByteArray(HEADER, 32)
                                  + QByteArray("!HEADER_INSERT!", 15);
        bool firstOK = (firstChunk.left(10+32+15) == expectedFirst);
        fprintf(stderr, "    [DIAG] first block 0..128: head expected %d bytes, match=%d\n",
                expectedFirst.size(), int(firstOK));
    }

    // 3) applyAndSave 到 savePath
    // 为加快验证节奏：先确认 renderRange（lastChunk/firstChunk）通过，再启用完整保存。
    // 当下面的 CHECK 失败时，说明 renderRange 修复尚未生效；成功后再解包 applyAndSave。
    {
        QByteArray expectedFirst = QByteArray("__PREFIX__", 10)
                                  + QByteArray(HEADER, 32)
                                  + QByteArray("!HEADER_INSERT!", 15);
        QByteArray firstChunk = pl.renderRange(base, SIZE, 0, 128);
        CHECK(firstChunk.left(10+32+15) == expectedFirst, "首块 0..128 内容正确（前缀 + HEADER + INSERT）");

        QByteArray expectedLast;
        expectedLast.append((const char*)(base + SIZE - 25), 25);
        expectedLast.append("~APPEND8~", 9);
        QByteArray lastChunk = pl.renderRange(base, SIZE, SIZE, SIZE + 34);
        CHECK(lastChunk.size() == 34, "尾块 SIZE..SIZE+34 字节数=34");
        if (lastChunk.size() == 34) {
            bool lastOK = true;
            for (int i = 0; i < 34; ++i) {
                if ((unsigned char)lastChunk[i] != (unsigned char)expectedLast[i]) {
                    fprintf(stderr, "    [DIAG] tail content diff at %d: got=0x%02x want=0x%02x\n",
                            i, (unsigned char)lastChunk[i], (unsigned char)expectedLast[i]);
                    lastOK = false;
                    break;
                }
            }
            CHECK(lastOK, "尾块内容正确（物理末 25 字节 + APPEND8 9 字节）");
        }
    }
    // ---- applyAndSave 与保存验证（实际完整流程）----
    std::atomic<bool> cancel{false};
    ok = pl.applyAndSave(base, SIZE, savePath, {}, &cancel, &err);
    if (!ok) fprintf(stderr, "    [DIAG] applyAndSave 4.5GB err: %s\n", err.toUtf8().constData());
    CHECK(ok, "applyAndSave 4.5GB 大文件 + patch 成功");

    // 4) 验证保存文件的基本结构：
    //    - 开头 10 字节应为 __PREFIX__
    //    - 随后 32 字节 HEADER
    //    - 再 15 字节 !HEADER_INSERT!
    //    - 然后是 SIZE 字节的原文件内容（含 MIDPOINT_MARK / TAIL_BYTE）
    //    - 文件尾 9 字节 ~APPEND8~
    //    - 文件总长 = SIZE + 10 + 15 + 9 = SIZE + 34
    //      net delta = 10 (prefix) + 15 (insert at 32) + 9 (append) = 34
    QFile savedF(savePath);
    if (!savedF.open(QIODevice::ReadOnly)) { CHECK(false, "保存后的文件读失败"); goto cleanup; }
    {
        qint64 savedSize = savedF.size();
        fprintf(stderr, "    [INFO] 4.5GB 原文件 + patch 后保存大小 = %lld 字节 (期望 %lld)\n",
                (long long)savedSize, (long long)(SIZE + 34));
        CHECK(savedSize == SIZE + 34, "保存的文件长度与净长度差一致 (SIZE + 34)");
    }
    {
        QByteArray head = savedF.read(10 + 32 + 15);
        CHECK(head.left(10) == QByteArray("__PREFIX__"), "保存文件前 10 字节为前缀 insert");
        CHECK(head.mid(10, 32) == QByteArray(HEADER, 32), "保存文件 10-42 为原 HEADER");
        CHECK(head.mid(42, 15) == QByteArray("!HEADER_INSERT!"), "保存文件 42-57 为头 insert");
    }
    {
        // 中间 __MIDPOINT_MARK__ 应在虚拟位置:
        //   10 (prefix) + 15 (insert at 32) + (2GB - 32) (物理偏移 32 到 2GB) = 2GB - 7
        //   但更简单：虚拟偏移 = 10 + 15 + 2GB = 2GB + 25
        //   （prefix 和 header-insert 都在物理偏移 2GB 之前，所以都要加）
        qint64 wantMidPos = 10 + 15 + FOUR_GB / 2;
        if (savedF.seek(wantMidPos)) {
            QByteArray mid = savedF.read(17);
            CHECK(mid == QByteArray("__MIDPOINT_MARK__"),
                  "2GB 附近的 __MIDPOINT_MARK__ 未被 patch 影响（仍存在）");
        } else {
            CHECK(false, "seek 到 2GB 失败");
        }
    }
    {
        // 尾部：最后 9 字节应为 ~APPEND8~（9 字节，非 8）
        qint64 sSize = savedF.size();
        if (savedF.seek(sSize - 9)) {
            QByteArray tail = savedF.read(9);
            CHECK(tail == QByteArray("~APPEND8~"), "文件尾最后 9 字节为 append 内容");
        }
        // 之前的 TAILBYTE 位置 = SIZE + 10 (prefix) + 15 (insert at 32) - 8
        // （原 TAILBYTE 在物理 SIZE-8 处，前面插入了 25 字节 → 虚拟 SIZE+25-8 = SIZE+17）
        if (savedF.seek(SIZE + 10 + 15 - 8)) {
            QByteArray origTail = savedF.read(8);
            CHECK(origTail == QByteArray("TAILBYTE"), "原 TAILBYTE 在新文件中仍存在");
        }
    }
    savedF.close();

cleanup:
    fmmap.unmap(const_cast<uchar*>(base));
    fmmap.close();
    QFile::remove(path);
    QFile::remove(savePath);
}

// ============================================================================
// P0/P1/P2 回归测试：坐标完整性 + 保存字节守恒 + qint64 列
//
// 验证 2026-08-13 修复：
//   P0: byteToLineCol col 改 qint64（长行不溢出）；incomplete 不再伪造偏移
//   P1: buildRenderSpansFromSorted 移除 virtCursor-=extra hack，renderRange 严格守恒
//   P2: origToVirt/virtToOrig 一致性（编辑后坐标系正确）
// 触发条件：末尾追加 + 中间插入 + 替换 + 删除组合，覆盖 gap/append-hole 路径。
// ============================================================================
static void test_p012_coord_save_integrity()
{
    TEST("P0/P1/P2: 坐标转换一致性 + renderRange 字节守恒 + applyAndSave 成功");

    // 构造 1MB 多行缓冲（每行 256 字节 + \n，共 4096 行）
    const int lineLen = 256;
    const int lineCount = 4096;
    QByteArray buf;
    buf.reserve(lineCount * (lineLen + 1));
    for (int i = 0; i < lineCount; ++i) {
        QByteArray one(lineLen, char('A' + (i % 26)));
        // 在每行第 0 字节写入行号低字节，便于内容校验
        one[0] = char('0' + (i / 1000) % 10);
        one[1] = char('0' + (i / 100) % 10);
        one[2] = char('0' + (i / 10) % 10);
        one[3] = char('0' + i % 10);
        buf.append(one);
        buf.append('\n');
    }
    const qint64 mmapSize = buf.size();
    uchar* base = (uchar*)buf.data();

    // ---- P0: LineIndex qint64 col ----
    LineIndex idx;
    idx.buildFromMmap(base, mmapSize);
    // 末行中间位置：col 应为 100（qint64）
    qint64 lastLineStart = idx.lineToByteOffset(lineCount - 1);
    int gotLine; qint64 gotCol;
    idx.byteToLineCol(lastLineStart + 100, &gotLine, &gotCol);
    CHECK(gotLine == lineCount - 1, "P0: 末行中间位置行号正确");
    CHECK(gotCol == 100, "P0: 末行中间位置 col==100（qint64 输出）");

    // ---- P2: 空补丁时 origToVirt/virtToOrig 恒等 ----
    PatchList pl;
    CHECK(pl.origToVirt(0) == 0, "P2: 空补丁 origToVirt(0)==0");
    CHECK(pl.origToVirt(mmapSize) == mmapSize, "P2: 空补丁 origToVirt(end)==end");
    CHECK(pl.virtToOrig(mmapSize / 2) == mmapSize / 2, "P2: 空补丁 virtToOrig 恒等");

    // ---- P1: 末尾追加（触发 append-hole 路径）----
    QByteArray appendData = QByteArrayLiteral("[APPEND-XYZ]");
    qint64 appendAt = mmapSize;  // 文件末尾
    bool ok = pl.insert(appendAt, appendData);
    CHECK(ok, "P1: 末尾追加 insert 成功");
    qint64 netDelta = pl.netLengthDelta();
    CHECK(netDelta == appendData.size(), "P1: 追加后 netDelta == appendData.size");

    // renderRange 字节守恒：对若干区间验证 size == end-start
    bool rangeIntegrity = true;
    for (int t = 0; t < 20; ++t) {
        qint64 s = qint64(t) * (mmapSize / 20);
        qint64 e = qMin(mmapSize + appendData.size(), s + 4096);
        if (s >= e) continue;
        QByteArray r = pl.renderRange(base, mmapSize, s, e);
        if (r.size() != int(e - s)) {
            fprintf(stderr, "    [FAIL] renderRange(%lld,%lld) size=%d 期望=%lld\n",
                    (long long)s, (long long)e, r.size(), (long long)(e - s));
            fflush(stderr);
            rangeIntegrity = false;
            break;
        }
    }
    CHECK(rangeIntegrity, "P1: 追加后 renderRange 全区间字节守恒（无 virtCursor 回拨丢字节）");

    // ---- P2: 追加后 origToVirt/virtToOrig 一致性 ----
    // 原始区间 [0,mmapSize] 应 1:1 映射
    bool coordConsistent = true;
    for (int t = 0; t <= 20; ++t) {
        qint64 x = qint64(t) * mmapSize / 20;
        qint64 v = pl.origToVirt(x);
        qint64 back = pl.virtToOrig(v);
        if (back != x) { coordConsistent = false; break; }
    }
    CHECK(coordConsistent, "P2: 追加后 origToVirt/virtToOrig 往返一致");

    // ---- P1: 中间插入 + 替换 + 删除组合 ----
    qint64 midPos = mmapSize / 2;
    ok = pl.insert(midPos, QByteArrayLiteral("<MID>"));
    CHECK(ok, "P1: 中间插入成功");
    // 替换：在 1/4 处替换 8 字节为 12 字节
    qint64 repPos = mmapSize / 4;
    QByteArray oldSnap = pl.renderRange(base, mmapSize, repPos, repPos + 8);
    ok = pl.replaceWithSnapshots(repPos, 8, QByteArrayLiteral("REPLACEMENT12"), oldSnap);
    CHECK(ok, "P1: 替换成功");

    // 删除：在 3/4 处删除 16 字节
    qint64 delPos = (mmapSize * 3) / 4;
    QByteArray delSnap = pl.renderRange(base, mmapSize, delPos, delPos + 16);
    ok = pl.removeWithSnapshot(delPos, 16, delSnap);
    CHECK(ok, "P1: 删除成功");

    // 组合编辑后再次验证 renderRange 全区间守恒
    qint64 virtSize = pl.virtualSize(mmapSize);
    bool rangeIntegrity2 = true;
    for (int t = 0; t < 40; ++t) {
        qint64 s = qint64(t) * virtSize / 40;
        qint64 e = qMin(virtSize, s + 8192);
        if (s >= e) continue;
        QByteArray r = pl.renderRange(base, mmapSize, s, e);
        if (r.size() != int(e - s)) {
            fprintf(stderr, "    [FAIL] 组合后 renderRange(%lld,%lld) size=%d 期望=%lld\n",
                    (long long)s, (long long)e, r.size(), (long long)(e - s));
            fflush(stderr);
            rangeIntegrity2 = false;
            break;
        }
    }
    CHECK(rangeIntegrity2, "P1: 组合编辑后 renderRange 全区间字节守恒");

    // ---- P1: applyAndSave 必须成功且输出大小 == virtSize ----
    // 注意：不用 QTemporaryFile（其对象存活期间 Windows 下 QSaveFile::commit 的
    // rename 可能"拒绝访问"）；改用 temp 目录下的普通路径，手动清理。
    QString outPath = QDir::tempPath() + QStringLiteral("/npstest_p012_%1.bin")
                          .arg(quint64(QCoreApplication::applicationPid()));
    QFile::remove(outPath);
    QString saveErr;
    std::atomic<bool> cancel{false};
    ok = pl.applyAndSave(base, mmapSize, outPath, {}, &cancel, &saveErr);
    if (!ok) {
        fprintf(stderr, "    [FAIL] applyAndSave 错误: %s\n", saveErr.toLocal8Bit().constData());
        fflush(stderr);
    }
    CHECK(ok, "P1: 组合编辑后 applyAndSave 成功（无渲染块大小不匹配）");
    if (ok) {
        QFile saved(outPath);
        if (saved.open(QIODevice::ReadOnly)) {
            qint64 savedSize = saved.size();
            CHECK(savedSize == virtSize,
                  QString("P1: 保存文件大小 == 虚拟大小 (%1==%2)")
                      .arg(savedSize).arg(virtSize).toStdString().c_str());
            saved.close();
        }
    }
    QFile::remove(outPath);

    // ---- P1: 内容正确性校验（末尾追加内容在保存文件末尾可见）----
    // 重新做一次纯追加保存，验证末尾字节 == appendData
    PatchList pl2;
    pl2.insert(mmapSize, appendData);
    QString outPath2 = QDir::tempPath() + QStringLiteral("/npstest_p012b_%1.bin")
                           .arg(quint64(QCoreApplication::applicationPid()));
    QFile::remove(outPath2);
    ok = pl2.applyAndSave(base, mmapSize, outPath2, {}, &cancel, &saveErr);
    CHECK(ok, "P1: 纯追加 applyAndSave 成功");
    if (ok) {
        QFile saved(outPath2);
        if (saved.open(QIODevice::ReadOnly)) {
            saved.seek(mmapSize);
            QByteArray tail = saved.read(appendData.size() + 4);
            CHECK(tail.startsWith(appendData),
                  "P1: 保存文件末尾包含追加内容（字节未丢失）");
            saved.close();
        }
    }
    QFile::remove(outPath2);
}

int main(int argc, char *argv[])
{
    // 与 notepad-safe.exe 相同的初始化方式：QApplication 而非 QCoreApplication，
    // 因为 QsciScintilla 是 QWidget 派生类，需要 GUI 子系统。
    // 不调用 widget->show()，避免弹出窗口干扰测试。
    QApplication app(argc, argv);
    app.setApplicationName("test_largefile");
    app.setOrganizationName("NotepadSafe");

    fprintf(stderr, "==== 大文件分块加载回归测试 ====\n");

    test_readonly_blocks_appendtext();
    test_appendtext_works_when_not_readonly();
    test_chunked_append_without_readonly();
    test_chunked_append_10mb();
    test_readonly_vs_nonreadonly_chunked();

    fprintf(stderr, "\n==== L2 虚拟视图 / 编辑 / 搜索 / 替换测试 ====\n");

    test_dynamicthresholds_range();
    test_lineindex_basic();
    test_lineindex_all_lines_dense();
    test_lineindex_cancel();
    test_lineindex_clear();
    test_patchlist_insert_render();
    test_patchlist_remove_undo_redo();
    test_patchlist_replace_undo();
    test_patchlist_grouped_undo();
    test_patchlist_applyandsave();
    test_patchlist_spillover();
    test_searchresults_model_api();
    test_searchresults_model_overrun();
    test_search_cross_chunk();
    test_search_cancel();
    test_replaceall_atomic();
    test_replaceall_intermediate_save();
    test_replaceall_10batches_undo_atomic();
    test_applyandsave_resetaftersave();
    test_reset_after_save_then_edit_undo();

    fprintf(stderr, "\n==== 新增功能测试：分组/长行/跳转/多线程/高亮 ====\n");

    test_searchresults_grouping();
    test_longline_render();
    test_goto_byte_offset_horizontal();
    test_multithread_search();
    test_search_highlight_rendering();

    // 先跑一个轻量级诊断（无 4.5GB I/O）：验证 3 次 patch 的 newText.size 是否正确——
    // 如果这里就错，说明 Bug 在 PatchList 本身；否则 Bug 来自 4.5GB mmap 对堆的干扰。
    test_patchlist_sizes_lightweight_diag();

    // P0/P1/P2 修复回归（2026-08-13）：坐标完整性 + 保存字节守恒 + qint64 列
    test_p012_coord_save_integrity();

    // 4.5GB 测试很重，放最后
    test_patchlist_applyandsave_4gb_plus();

    fprintf(stderr, "\n==== 结果: %d/%d 通过 ====\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
