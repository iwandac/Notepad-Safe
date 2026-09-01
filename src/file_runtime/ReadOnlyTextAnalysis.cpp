#include "ReadOnlyTextAnalysis.h"
#include "SharedReadOnlyFile.h"
#include <QStringConverter>
#include <climits>

namespace ReadOnlyTextAnalysis {
namespace {

constexpr qint64 ScanReadBytes = 16LL * 1024 * 1024;

qint64 unicodeScalarCount(QStringView text)
{
    qint64 count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar current = text.at(i);
        if (current.isHighSurrogate() && i + 1 < text.size()
            && text.at(i + 1).isLowSurrogate()) {
            ++i;
        }
        ++count;
    }
    return count;
}

} // namespace

qint64 countPhysicalLineBreaks(QByteArrayView bytes, TextEncoding encoding,
                              bool previousWasCr, bool *endsWithCr)
{
    qint64 breaks = 0;
    bool priorCr = previousWasCr;
    if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) {
        for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
            const quint16 unit = encoding == TextEncoding::Utf16LE
                ? quint16(uchar(bytes.at(i))) | (quint16(uchar(bytes.at(i + 1))) << 8)
                : (quint16(uchar(bytes.at(i))) << 8) | quint16(uchar(bytes.at(i + 1)));
            if (unit == '\r') {
                ++breaks;
                priorCr = true;
            } else {
                if (unit == '\n' && !priorCr) ++breaks;
                priorCr = false;
            }
        }
    } else {
        for (const char raw : bytes) {
            const uchar value = uchar(raw);
            if (value == '\r') {
                ++breaks;
                priorCr = true;
            } else {
                if (value == '\n' && !priorCr) ++breaks;
                priorCr = false;
            }
        }
    }
    if (endsWithCr) *endsWithCr = priorCr;
    return breaks;
}

Result analyze(const QString &path, const PersistenceReadPolicy &policy,
               const DiskFileStamp &expected, TextEncoding encoding,
               const std::atomic_bool *cancel,
               const std::function<void(qint64, qint64)> &progress)
{
    Result result;
    if (cancel && cancel->load(std::memory_order_acquire))
        return {Result::Status::Canceled};
    if (!expected.valid || expected.size < 0) {
        result.source.status = OrdinaryReadStatus::Changed;
        result.source.error = QStringLiteral("Text analysis requires a valid source stamp.");
        return result;
    }
    result.characterCount = 0;
    result.lineBreakPrefix = {0};
    result.lineBreakPrefixEndsWithCr = {0};
    const qint64 size = expected.size;
    result.lineBreakPrefix.reserve(int(qMin<qint64>(size / PrefixChunkBytes + 2, INT_MAX)));
    result.lineBreakPrefixEndsWithCr.reserve(result.lineBreakPrefix.capacity());
    const auto canceled = [&] { return cancel && cancel->load(std::memory_order_acquire); };
    qint64 totalLineBreaks = 0;
    bool previousWasCr = false;
    bool firstDecodedChunk = true;
    QStringDecoder utf8Decoder("UTF-8");
    qint64 offset = 0;
    do {
        if (canceled()) return {Result::Status::Canceled};
        const qint64 wanted = qMin(ScanReadBytes, size - offset);
        SharedReadOnlyFile reader;
        result.source = openOrdinaryReadSource(reader, path, policy, &expected);
        if (!result.source.isReady()) return result;
        const QByteArray chunk = reader.readAt(offset, wanted, &result.source.error);
        reader.captureStamp(&result.source.stamp);
        reader.close();
        if (!sameDiskFileStamp(result.source.stamp, expected))
            result.source.status = OrdinaryReadStatus::Changed;
        else if (chunk.size() != wanted)
            result.source.status = OrdinaryReadStatus::Unreadable;
        if (!result.source.isReady()) return result;
        qsizetype local = 0;
        while (local < chunk.size()) {
            if (canceled()) return {Result::Status::Canceled};
            const qsizetype partSize = qMin<qsizetype>(PrefixChunkBytes, chunk.size() - local);
            const QByteArrayView part(chunk.constData() + local, partSize);
            totalLineBreaks += countPhysicalLineBreaks(part, encoding, previousWasCr, &previousWasCr);
            const QByteArray partBytes = QByteArray::fromRawData(part.data(), part.size());
            QString decoded = (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf8Bom)
                ? utf8Decoder(part) : decodeTextFragment(partBytes, encoding);
            qint64 decodedCount = unicodeScalarCount(decoded);
            if (firstDecodedChunk && !decoded.isEmpty()
                && decoded.front() == QChar::ByteOrderMark) {
                --decodedCount;
            }
            firstDecodedChunk = false;
            result.characterCount += qMax<qint64>(0, decodedCount);
            local += partSize;
            offset += partSize;
            result.lineBreakPrefix.append(totalLineBreaks);
            result.lineBreakPrefixEndsWithCr.append(previousWasCr ? 1 : 0);
        }
        if (progress) progress(offset, size);
    } while (offset < size);
    result.status = canceled() ? Result::Status::Canceled : Result::Status::Completed;
    return result;
}

} // namespace ReadOnlyTextAnalysis
