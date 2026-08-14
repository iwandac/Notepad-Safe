#include "TextEncoding.h"

#include <QStringConverter>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Windows API 解码 GBK（CP_ACP 在中文系统上是 GBK）
static QString fromSystemCodepage(const QByteArray &data)
{
#ifdef Q_OS_WIN
    if (data.isEmpty())
        return QString();
    int len = MultiByteToWideChar(CP_ACP, 0, data.constData(), data.size(),
                                  nullptr, 0);
    if (len == 0)
        return QString::fromLatin1(data);
    QVector<wchar_t> wstr(len);
    MultiByteToWideChar(CP_ACP, 0, data.constData(), data.size(),
                        wstr.data(), len);
    return QString::fromWCharArray(wstr.data(), len);
#else
    auto decoder = QStringDecoder("GB18030");
    if (decoder.isValid())
        return decoder(data);
    return QString::fromLatin1(data);
#endif
}

// Windows API 编码为 GBK
static QByteArray toSystemCodepage(const QString &text)
{
#ifdef Q_OS_WIN
    if (text.isEmpty())
        return QByteArray();
    const wchar_t *wptr = reinterpret_cast<const wchar_t *>(text.utf16());
    int len = WideCharToMultiByte(CP_ACP, 0, wptr, text.length(),
                                  nullptr, 0, nullptr, nullptr);
    if (len == 0)
        return text.toUtf8();
    QByteArray result(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, wptr, text.length(),
                        result.data(), len, nullptr, nullptr);
    return result;
#else
    auto encoder = QStringEncoder("GB18030");
    if (encoder.isValid())
        return encoder(text);
    return text.toUtf8();
#endif
}

bool isValidUtf8(const QByteArray &data)
{
    int i = 0;
    const int size = data.size();
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data.constData());
    while (i < size) {
        unsigned char c = p[i];
        if (c <= 0x7F) {
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= size) return false;
            if ((p[i + 1] & 0xC0) != 0x80) return false;
            // 检查 overlong
            if ((c & 0x1E) == 0) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= size) return false;
            if ((p[i + 1] & 0xC0) != 0x80) return false;
            if ((p[i + 2] & 0xC0) != 0x80) return false;
            // 检查 overlong 和代理区
            if ((c & 0x0F) == 0 && (p[i + 1] & 0x20) == 0) return false;
            if (c == 0xED && (p[i + 1] & 0xA0) == 0xA0) return false; // 代理区
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= size) return false;
            if ((p[i + 1] & 0xC0) != 0x80) return false;
            if ((p[i + 2] & 0xC0) != 0x80) return false;
            if ((p[i + 3] & 0xC0) != 0x80) return false;
            // 检查 overlong 和超出范围
            if ((c & 0x07) == 0 && (p[i + 1] & 0x30) == 0) return false;
            if (c > 0xF4) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

DecodeResult decodeBytes(const QByteArray &data)
{
    DecodeResult result;
    result.hasBom = false;

    // 1. UTF-8 BOM
    if (data.size() >= 3 &&
        (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        result.encoding = TextEncoding::Utf8Bom;
        result.hasBom = true;
        result.text = QString::fromUtf8(data.mid(3));
        return result;
    }

    // 2. UTF-16LE BOM
    if (data.size() >= 2 &&
        (unsigned char)data[0] == 0xFF &&
        (unsigned char)data[1] == 0xFE) {
        result.encoding = TextEncoding::Utf16LE;
        result.hasBom = true;
        const int charCount = (data.size() - 2) / 2;
        result.text = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(data.constData() + 2),
            charCount);
        return result;
    }

    // 3. UTF-16BE BOM
    if (data.size() >= 2 &&
        (unsigned char)data[0] == 0xFE &&
        (unsigned char)data[1] == 0xFF) {
        result.encoding = TextEncoding::Utf16BE;
        result.hasBom = true;
        QByteArray swapped = data.mid(2);
        for (int i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        const int charCount = swapped.size() / 2;
        result.text = QString::fromUtf16(
            reinterpret_cast<const char16_t *>(swapped.constData()),
            charCount);
        return result;
    }

    // 4. 尝试 UTF-8 严格解码
    if (isValidUtf8(data)) {
        result.encoding = TextEncoding::Utf8;
        result.text = QString::fromUtf8(data);
        return result;
    }

    // 5. 回退到系统代码页（中文 Windows 是 GBK）
    result.encoding = TextEncoding::GBK;
    result.text = fromSystemCodepage(data);
    return result;
}

QByteArray encodeText(const QString &text, TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:
        return text.toUtf8();
    case TextEncoding::Utf8Bom: {
        QByteArray r = QByteArray("\xEF\xBB\xBF");
        r += text.toUtf8();
        return r;
    }
    case TextEncoding::Utf16LE: {
        QByteArray r = QByteArray("\xFF\xFE");
        const int len = text.length();
        r += QByteArray(reinterpret_cast<const char *>(text.utf16()), len * 2);
        return r;
    }
    case TextEncoding::Utf16BE: {
        QByteArray r = QByteArray("\xFE\xFF");
        const ushort *data = text.utf16();
        const int len = text.length();
        for (int i = 0; i < len; ++i) {
            r.append((char)(data[i] >> 8));
            r.append((char)(data[i] & 0xFF));
        }
        return r;
    }
    case TextEncoding::GBK:
        return toSystemCodepage(text);
    }
    return text.toUtf8();
}

QString encodingName(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:    return QStringLiteral("UTF-8");
    case TextEncoding::Utf8Bom: return QStringLiteral("UTF-8 BOM");
    case TextEncoding::Utf16LE: return QStringLiteral("UTF-16LE");
    case TextEncoding::Utf16BE: return QStringLiteral("UTF-16BE");
    case TextEncoding::GBK:     return QStringLiteral("GBK");
    }
    return QStringLiteral("Unknown");
}

TextEncoding defaultEncodingForFile(const QString &fileName)
{
    Q_UNUSED(fileName);
    return TextEncoding::Utf8;
}
