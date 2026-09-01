#include "KdbxFormatProbe.h"

#include <QFile>

namespace {

quint32 littleEndian32(const char *bytes)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

} // namespace

KdbxHeaderInfo KdbxFormatProbe::inspectFile(const QString &filePath)
{
    KdbxHeaderInfo result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法读取 KDBX 文件头。 ");
        return result;
    }

    return inspectHeader(file.read(12));
}

KdbxHeaderInfo KdbxFormatProbe::inspectHeader(QByteArrayView header)
{
    KdbxHeaderInfo result;
    if (header.size() < 12) {
        result.error = QStringLiteral("KDBX 文件头被截断。 ");
        return result;
    }

    constexpr quint32 KeePassSignature1 = 0x9AA2D903U;
    constexpr quint32 KeePassSignature2 = 0xB54BFB67U;
    if (littleEndian32(header.data()) != KeePassSignature1 ||
        littleEndian32(header.data() + 4) != KeePassSignature2) {
        result.error = QStringLiteral("文件不是 KDBX 数据库。 ");
        return result;
    }

    const quint32 version = littleEndian32(header.data() + 8);
    result.minorVersion = quint16(version & 0xFFFFU);
    result.majorVersion = quint16(version >> 16);
    result.recognized = result.majorVersion == 3 || result.majorVersion == 4;
    if (!result.recognized)
        result.error = QStringLiteral("KDBX 主版本不受支持。 ");
    return result;
}
