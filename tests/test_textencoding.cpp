// 编码检测单元测试
#include "TextEncoding.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QString>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) fprintf(stderr, "  [RUN] %s\n", name)

#define CHECK(cond, msg) \
    do { \
        ++test_count; \
        if (cond) { ++test_passed; fprintf(stderr, "    [PASS] %s\n", msg); } \
        else { fprintf(stderr, "    [FAIL] %s\n", msg); } \
    } while (0)

static QByteArray toUtf8(const QString &s) { return s.toUtf8(); }

static QByteArray withBom(const QByteArray &data) {
    QByteArray r = QByteArray("\xEF\xBB\xBF");
    r += data;
    return r;
}

static QByteArray toUtf16LE(const QString &s) {
    QByteArray r = QByteArray("\xFF\xFE");
    const ushort *data = s.utf16();
    const int len = s.length();
    for (int i = 0; i < len; ++i) {
        r.append((char)(data[i] & 0xFF));
        r.append((char)(data[i] >> 8));
    }
    return r;
}

static QByteArray toUtf16BE(const QString &s) {
    QByteArray r = QByteArray("\xFE\xFF");
    const ushort *data = s.utf16();
    const int len = s.length();
    for (int i = 0; i < len; ++i) {
        r.append((char)(data[i] >> 8));
        r.append((char)(data[i] & 0xFF));
    }
    return r;
}

void test_utf8_no_bom()
{
    TEST("UTF-8 无 BOM 检测");
    QString original = QStringLiteral("Hello World 123");
    QByteArray data = toUtf8(original);
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf8, "应为 UTF-8");
    CHECK(result.text == original, "文本应一致");
    CHECK(!result.hasBom, "不应有 BOM");
}

void test_utf8_with_bom()
{
    TEST("UTF-8 BOM 检测");
    QString original = QStringLiteral("BOM test");
    QByteArray data = withBom(toUtf8(original));
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf8Bom, "应为 UTF-8 BOM");
    CHECK(result.text == original, "文本应一致");
    CHECK(result.hasBom, "应有 BOM");
}

void test_utf16le()
{
    TEST("UTF-16LE BOM 检测");
    QString original = QStringLiteral("UTF-16LE test ABC123");
    QByteArray data = toUtf16LE(original);
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf16LE, "应为 UTF-16LE");
    CHECK(result.text == original, "文本应一致");
    CHECK(result.hasBom, "应有 BOM");
}

void test_utf16be()
{
    TEST("UTF-16BE BOM 检测");
    QString original = QStringLiteral("UTF-16BE test XYZ789");
    QByteArray data = toUtf16BE(original);
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf16BE, "应为 UTF-16BE");
    CHECK(result.text == original, "文本应一致");
    CHECK(result.hasBom, "应有 BOM");
}

void test_empty_data()
{
    TEST("空数据检测");
    QByteArray data;
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf8, "空数据应为 UTF-8");
    CHECK(result.text.isEmpty(), "文本应为空");
}

void test_pure_ascii()
{
    TEST("纯 ASCII 检测");
    QByteArray data = "Hello World 123";
    DecodeResult result = decodeBytes(data);
    CHECK(result.encoding == TextEncoding::Utf8, "纯 ASCII 应为 UTF-8");
    CHECK(result.text == QStringLiteral("Hello World 123"), "文本应一致");
}

void test_isValidUtf8_ascii()
{
    TEST("isValidUtf8: 纯 ASCII");
    CHECK(isValidUtf8("Hello"), "ASCII 应为有效 UTF-8");
}

void test_isValidUtf8_valid()
{
    TEST("isValidUtf8: 有效 UTF-8");
    CHECK(isValidUtf8(toUtf8(QStringLiteral("test"))), "应为有效 UTF-8");
}

void test_isValidUtf8_invalid()
{
    TEST("isValidUtf8: 无效 UTF-8");
    QByteArray bad;
    bad.append('\xFF');
    bad.append('\xFE');
    CHECK(!isValidUtf8(bad), "0xFFFE 应为无效 UTF-8");
}

void test_isValidUtf8_overlong()
{
    TEST("isValidUtf8: overlong 编码");
    QByteArray bad;
    bad.append('\xC0');
    bad.append('\x81');
    CHECK(!isValidUtf8(bad), "overlong 应为无效");
}

void test_encode_decode_roundtrip_utf8()
{
    TEST("UTF-8 编解码往返");
    QString original = QStringLiteral("Roundtrip test");
    QByteArray encoded = encodeText(original, TextEncoding::Utf8);
    DecodeResult decoded = decodeBytes(encoded);
    CHECK(decoded.encoding == TextEncoding::Utf8, "应为 UTF-8");
    CHECK(decoded.text == original, "往返文本应一致");
}

void test_encode_decode_roundtrip_utf8bom()
{
    TEST("UTF-8 BOM 编解码往返");
    QString original = QStringLiteral("BOM roundtrip");
    QByteArray encoded = encodeText(original, TextEncoding::Utf8Bom);
    DecodeResult decoded = decodeBytes(encoded);
    CHECK(decoded.encoding == TextEncoding::Utf8Bom, "应为 UTF-8 BOM");
    CHECK(decoded.text == original, "往返文本应一致");
}

void test_encode_decode_roundtrip_utf16le()
{
    TEST("UTF-16LE 编解码往返");
    QString original = QStringLiteral("LE roundtrip ABC");
    QByteArray encoded = encodeText(original, TextEncoding::Utf16LE);
    DecodeResult decoded = decodeBytes(encoded);
    CHECK(decoded.encoding == TextEncoding::Utf16LE, "应为 UTF-16LE");
    CHECK(decoded.text == original, "往返文本应一致");
}

void test_encode_decode_roundtrip_utf16be()
{
    TEST("UTF-16BE 编解码往返");
    QString original = QStringLiteral("BE roundtrip XYZ");
    QByteArray encoded = encodeText(original, TextEncoding::Utf16BE);
    DecodeResult decoded = decodeBytes(encoded);
    CHECK(decoded.encoding == TextEncoding::Utf16BE, "应为 UTF-16BE");
    CHECK(decoded.text == original, "往返文本应一致");
}

void test_encoding_name()
{
    TEST("编码名称");
    CHECK(encodingName(TextEncoding::Utf8) == QStringLiteral("UTF-8"), "UTF-8 名称");
    CHECK(encodingName(TextEncoding::Utf8Bom) == QStringLiteral("UTF-8 BOM"), "UTF-8 BOM 名称");
    CHECK(encodingName(TextEncoding::Utf16LE) == QStringLiteral("UTF-16LE"), "UTF-16LE 名称");
    CHECK(encodingName(TextEncoding::Utf16BE) == QStringLiteral("UTF-16BE"), "UTF-16BE 名称");
    CHECK(encodingName(TextEncoding::GBK) == QStringLiteral("GBK"), "GBK 名称");
    CHECK(encodingName(TextEncoding::GB18030) == QStringLiteral("GB18030"), "GB18030 名称");
    CHECK(encodingName(TextEncoding::Big5) == QStringLiteral("Big5"), "Big5 名称");
    CHECK(encodingName(TextEncoding::ShiftJis) == QStringLiteral("Shift-JIS"), "Shift-JIS 名称");
    CHECK(encodingName(TextEncoding::Windows1252) == QStringLiteral("Windows-1252"), "Windows-1252 名称");
}

void test_encoding_name_roundtrip()
{
    TEST("九种受支持编码名称精确往返");
    const TextEncoding supported[] = {
        TextEncoding::Utf8,
        TextEncoding::Utf8Bom,
        TextEncoding::Utf16LE,
        TextEncoding::Utf16BE,
        TextEncoding::GBK,
        TextEncoding::GB18030,
        TextEncoding::Big5,
        TextEncoding::ShiftJis,
        TextEncoding::Windows1252,
    };
    for (const TextEncoding encoding : supported) {
        const QString name = encodingName(encoding);
        CHECK(encodingFromName(name) == encoding, "保存的精确名称恢复原编码");
        CHECK(!encodingFromName(QStringLiteral(" ") + name), "拒绝前导空白别名");
        CHECK(!encodingFromName(name + QStringLiteral(" ")), "拒绝尾随空白别名");
        if (name.toLower() != name)
            CHECK(!encodingFromName(name.toLower()), "拒绝小写别名");
        if (name.toUpper() != name)
            CHECK(!encodingFromName(name.toUpper()), "拒绝大写别名");
    }
}

void test_encoding_name_rejects_unsupported_values()
{
    TEST("空值及未知编码名称不猜测代码页");
    const QString unsupported[] = {
        QString(),
        QStringLiteral("Unknown"),
        QStringLiteral("UTF8"),
        QStringLiteral("UTF-8-BOM"),
        QStringLiteral("UTF-16"),
        QStringLiteral("Shift_JIS"),
        QStringLiteral("CP1252"),
        QStringLiteral("1252"),
        QStringLiteral("CP936"),
        QStringLiteral("ANSI"),
        QStringLiteral("GB 18030"),
        QStringLiteral("UTF-8\t"),
        QStringLiteral("UTF-8\n"),
        QStringLiteral("UTF-8\u00a0"),
        QString(4096, QLatin1Char('x')),
    };
    for (const QString &name : unsupported)
        CHECK(!encodingFromName(name), "未知持久化名称返回 nullopt");
}

void test_multibyte_byte_grid_projection()
{
    TEST("Hex 字符区使用多字节投影并保持字节网格");
    const QByteArray bytes = QStringLiteral("A中B").toUtf8();
    const QString grid = decodeByteGridText(bytes, TextEncoding::Utf8, 16);
    CHECK(grid.size() == bytes.size(), "字符区每个源字节保留一个可定位单元");
    CHECK(grid == QStringLiteral("A中  B"), "UTF-8 中文只显示一次且续字节保留为空位");
    const QByteArray supplementary = QStringLiteral("A😀B").toUtf8();
    CHECK(decodeByteGridText(supplementary, TextEncoding::Utf8, 16)
              == QStringLiteral("A�   B"),
          "补充平面字符不能用代理对吞掉一个可定位字节边界");
    CHECK(decodeByteGridText(QByteArray::fromHex("DEADBE"),
                             TextEncoding::Utf8, 16)
              == QStringLiteral("� �"),
          "RTL 多字节标量不能用双向排版破坏字节光标拓扑");
}

void test_replacement_character_projection_truth()
{
    TEST("真实与投影 U+FFFD 统一显示但保留字节边界");
    QByteArray bytes("H", 1);
    bytes.append(char(0xff));
    bytes.append("T$");
    bytes.append("\xef\xbf\xbd", 3);
    const DecodedTextProjection projection = decodeTextProjection(
        bytes, TextEncoding::Utf8, false, false);
    CHECK(projection.text == QStringLiteral("H\ufffdT$\ufffd"),
          "非法字节与真实 U+FFFD 使用相同 UI 字符");
    CHECK(projection.decodeErrors, "非法 UTF-8 在内部仍有解码错误事实");
    CHECK(projection.sourceBoundaries
              == QVector<qint32>({0, 1, 2, 3, 4, 7}),
          "两种 U+FFFD 的源字节边界保持精确");
    CHECK(projection.projectedToSource.isEmpty()
              && projection.projectedBoundaries.isEmpty(),
          "只需文本/源边界时不分配高成本投影映射");
    CHECK(decodeByteGridText(bytes, TextEncoding::Utf8, 16)
              == QStringLiteral("H\ufffdT$\ufffd  "),
          "固定字节网格同样不暴露低价值的 U+FFFD 来源差异");
}

void test_projection_position_mapper_half_open_contract()
{
    TEST("投影位置统一保持半开 source-byte 边界");
    const QString sample = QStringLiteral("A中😀B");
    const TextEncoding encodings[] = {
        TextEncoding::Utf8,
        TextEncoding::Utf16LE,
        TextEncoding::Utf16BE,
    };
    for (const TextEncoding encoding : encodings) {
        bool encodedExactly = false;
        const QByteArray bytes = encodeTextFragment(sample, encoding,
                                                    &encodedExactly);
        CHECK(encodedExactly, "测试文本可被目标编码精确表示");
        const DecodedTextProjection projection = decodeTextProjection(
            bytes, encoding, false, true);
        CHECK(!projection.sourceBoundaries.isEmpty()
                  && projection.sourceBoundaries.size()
                         == projection.projectedBoundaries.size(),
              "源边界与投影边界一一对应");

        for (qsizetype i = 0; i < projection.sourceBoundaries.size(); ++i) {
            const qint32 source = projection.sourceBoundaries.at(i);
            const qint32 projected = projection.projectedBoundaries.at(i);
            CHECK(TextProjectionPositionMapper::toProjected(
                      projection.sourceBoundaries,
                      projection.projectedBoundaries, source) == projected,
                  "每个源字符边界可精确映射到投影边界");
            CHECK(TextProjectionPositionMapper::toSource(
                      projection.projectedToSource, projected,
                      TextBoundaryAffinity::Start) == source,
                  "投影边界的起点 affinity 不向前漂移");
            CHECK(TextProjectionPositionMapper::toSource(
                      projection.projectedToSource, projected,
                      TextBoundaryAffinity::End) == source,
                  "投影边界的终点 affinity 不多复制一个字符");
        }

        for (qint32 projected = 1;
             projected < projection.projectedBoundaries.constLast();
             ++projected) {
            const auto upper = std::lower_bound(
                projection.projectedBoundaries.cbegin(),
                projection.projectedBoundaries.cend(), projected);
            if (upper != projection.projectedBoundaries.cend()
                && *upper == projected) {
                continue;
            }
            const qsizetype upperIndex = qsizetype(
                upper - projection.projectedBoundaries.cbegin());
            CHECK(TextProjectionPositionMapper::toSource(
                      projection.projectedToSource, projected,
                      TextBoundaryAffinity::Start)
                      == projection.sourceBoundaries.at(upperIndex - 1),
                  "字符内部的选择起点向前扩展到完整源字符");
            CHECK(TextProjectionPositionMapper::toSource(
                      projection.projectedToSource, projected,
                      TextBoundaryAffinity::End)
                      == projection.sourceBoundaries.at(upperIndex),
                  "字符内部的选择终点向后扩展到完整源字符");
        }
    }
}

void test_admitted_reader_encoding_sampling()
{
    TEST("已准入读取器保留首中尾编码采样预算");
    constexpr qint64 budget = 1024 * 1024;
    CHECK(EncodingDetectionBudgetBytes == budget, "编码总采样预算保持 1 MiB");
    QTemporaryDir directory;
    CHECK(directory.isValid(), "临时编码夹具目录可用");
    if (!directory.isValid()) return;
    for (const qint64 size : {0LL, 127LL, budget, budget * 4}) {
        QByteArray bytes(size, 'a');
        if (size > 3) bytes.replace(size - 3, 3, QByteArray::fromHex("e4b8ad"));
        const QString path = directory.filePath(QString::number(size) + ".txt");
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == size;
        CHECK(written, "写入真实编码采样夹具");
        file.close();
        if (!written) continue;
        QVector<QPair<qint64, qsizetype>> reads;
        qint64 total = 0;
        const auto actual = detectFileEncoding(size, [&](qint64 offset, qsizetype length) {
            reads.append({offset, length});
            total += length;
            return bytes.mid(offset, length);
        });
        CHECK(total == qMin(size, budget), "读取字节总量不超过原固定预算");
        if (size <= budget) {
            CHECK(reads.size() == 1 && reads[0].first == 0 && reads[0].second == size,
                  "预算内只读取完整的一段（包括空文件）");
        } else {
            const qint64 side = budget / 3;
            qint64 middle = (size - side) / 2;
            middle -= middle % 4;
            CHECK(reads.size() == 3
                      && reads[0] == qMakePair(qint64(0), qsizetype(budget - 2 * side))
                      && reads[1] == qMakePair(middle, qsizetype(side))
                      && reads[2] == qMakePair(size - side, qsizetype(side)),
                  "大文件首中尾三段范围准确，不能退化为只读首段");
        }
        for (const auto expected : {detectFileEncoding(path), detectEncoding(bytes)}) {
            CHECK(actual.encoding == expected.encoding && actual.origin == expected.origin
                      && actual.confidence == expected.confidence
                      && actual.hasBom == expected.hasBom
                      && actual.decodeErrors == expected.decodeErrors,
                  "已打开读取器与原路径/内存入口的完整编码判定一致");
        }
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    fprintf(stderr, "\n=== TextEncoding Tests ===\n\n");

    test_utf8_no_bom();
    test_utf8_with_bom();
    test_utf16le();
    test_utf16be();
    test_empty_data();
    test_pure_ascii();
    test_isValidUtf8_ascii();
    test_isValidUtf8_valid();
    test_isValidUtf8_invalid();
    test_isValidUtf8_overlong();
    test_encode_decode_roundtrip_utf8();
    test_encode_decode_roundtrip_utf8bom();
    test_encode_decode_roundtrip_utf16le();
    test_encode_decode_roundtrip_utf16be();
    test_encoding_name();
    test_encoding_name_roundtrip();
    test_encoding_name_rejects_unsupported_values();
    test_multibyte_byte_grid_projection();
    test_replacement_character_projection_truth();
    test_projection_position_mapper_half_open_contract();
    test_admitted_reader_encoding_sampling();

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
