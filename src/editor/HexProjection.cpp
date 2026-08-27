
#include "HexProjection.h"
#include <QObject>

QString formatHexBytes(const QByteArray &bytes, int bytesPerRow)
{
    bytesPerRow = qMax(1, bytesPerRow);
    static constexpr char Digits[] = "0123456789ABCDEF";
    QString text;
    text.reserve(bytes.size() * 3 + bytes.size() / bytesPerRow + 1);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes.at(i));
        text.append(QLatin1Char(Digits[value >> 4]));
        text.append(QLatin1Char(Digits[value & 0x0f]));
        const bool endOfLine = ((i + 1) % bytesPerRow) == 0;
        if (i + 1 < bytes.size())
            text.append(endOfLine ? QLatin1Char('\n') : QLatin1Char(' '));
    }
    return text;
}

int hexNibble(QChar ch)
{
    const ushort value = ch.unicode();
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parseHexBytes(const QString &text, QByteArray *bytes, QString *error)
{
    if (!bytes)
        return false;
    bytes->clear();
    bytes->reserve(text.size() / 3 + 1);
    int highNibble = -1;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace()) {
            if (highNibble >= 0) {
                if (error) {
                    *error = QObject::tr(
                        "第 %1 个字符处的十六进制字节只有一位；每个字节必须是两位。")
                                 .arg(i + 1);
                }
                bytes->clear();
                return false;
            }
            continue;
        }
        const int nibble = hexNibble(ch);
        if (nibble < 0) {
            if (error)
                *error = QObject::tr("第 %1 个字符“%2”不是十六进制数字。")
                             .arg(i + 1).arg(ch);
            bytes->clear();
            return false;
        }
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            bytes->append(char((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    if (highNibble >= 0) {
        if (error)
            *error = QObject::tr("末尾的十六进制字节只有一位；每个字节必须是两位。");
        bytes->clear();
        return false;
    }
    return true;
}
