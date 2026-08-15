// 编码检测单元测试
#include "TextEncoding.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QString>

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

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
