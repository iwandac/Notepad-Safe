#pragma once

#include <QString>
#include <QByteArray>

// 文本编码类型
enum class TextEncoding {
    Utf8,          // UTF-8 无 BOM
    Utf8Bom,       // UTF-8 with BOM
    Utf16LE,       // UTF-16 Little Endian
    Utf16BE,       // UTF-16 Big Endian
    GBK,           // GBK / GB18030 / 系统默认
};

// 解码结果
struct DecodeResult {
    QString text;
    TextEncoding encoding = TextEncoding::Utf8;
    bool hasBom = false;
};

// 从字节流解码文本（自动检测编码）
// 检测顺序：BOM → UTF-8 严格验证 → GBK（Windows API）→ Latin-1 回退
DecodeResult decodeBytes(const QByteArray &data);

// 将文本编码为字节流
QByteArray encodeText(const QString &text, TextEncoding encoding);

// 编码名称（用于 UI 显示）
QString encodingName(TextEncoding encoding);

// 严格 UTF-8 验证（不含 BOM）
bool isValidUtf8(const QByteArray &data);

// 从文件后缀猜测默认编码（.txt 可能是 GBK，其他默认 UTF-8）
TextEncoding defaultEncodingForFile(const QString &fileName);
