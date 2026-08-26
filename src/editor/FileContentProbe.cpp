#include "FileContentProbe.h"

#include <QFile>
#include <QFileInfo>

FileContentProbeResult probeFileContent(const QString &path)
{
    FileContentProbeResult result;
    const QFileInfo info(path);
    result.exists = info.exists() && info.isFile();
    result.sourceBytes = result.exists ? info.size() : -1;
    result.tier = FileTierPolicy::classify(result.sourceBytes);
    if (!result.exists)
        return result;

    result.encoding = detectFileEncoding(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    result.readable = true;

    if (result.encoding.encoding == TextEncoding::Utf16LE
        || result.encoding.encoding == TextEncoding::Utf16BE) {
        return result;
    }

    const QByteArray sample = file.read(64 * 1024);
    if (sample.size() >= 2) {
        const auto first = static_cast<unsigned char>(sample[0]);
        const auto second = static_cast<unsigned char>(sample[1]);
        if ((first == 0xff && second == 0xfe)
            || (first == 0xfe && second == 0xff)) {
            return result;
        }
    }

    qsizetype controls = 0;
    for (const unsigned char byte : sample) {
        if (byte == 0) {
            result.contentKind = DocumentContentKind::Binary;
            return result;
        }
        if (byte < 0x20 && byte != '\n' && byte != '\r'
            && byte != '\t' && byte != '\f') {
            ++controls;
        }
    }
    if (!sample.isEmpty() && controls * 100 > sample.size() * 3)
        result.contentKind = DocumentContentKind::Binary;
    return result;
}
