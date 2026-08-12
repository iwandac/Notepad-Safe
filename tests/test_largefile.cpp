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
//       直接用 main() + QApplication，与 qt-notepad.exe 主程序相同的初始化方式。

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QScopeGuard>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) fprintf(stderr, "  [RUN] %s\n", name)

#define CHECK(cond, msg) \
    do { \
        ++test_count; \
        if (cond) { ++test_passed; fprintf(stderr, "    [PASS] %s\n", msg); } \
        else { fprintf(stderr, "    [FAIL] %s\n", msg); } \
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

int main(int argc, char *argv[])
{
    // 与 qt-notepad.exe 相同的初始化方式：QApplication 而非 QCoreApplication，
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

    fprintf(stderr, "\n==== 结果: %d/%d 通过 ====\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
