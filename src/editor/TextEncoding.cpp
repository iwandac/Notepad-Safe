#include "TextEncoding.h"

#include <QStringConverter>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static QString decodeWindowsCodePage(const QByteArray &data, quint32 codePage)
{
#ifdef Q_OS_WIN
    if (data.isEmpty())
        return QString();
    int len = MultiByteToWideChar(codePage, 0, data.constData(), data.size(),
                                  nullptr, 0);
    if (len == 0)
        return QString::fromLatin1(data);
    QVector<wchar_t> wstr(len);
    MultiByteToWideChar(codePage, 0, data.constData(), data.size(),
                        wstr.data(), len);
    return QString::fromWCharArray(wstr.data(), len);
#else
    const char *name = codePage == 936 ? "GBK"
        : codePage == 54936 ? "GB18030"
        : codePage == 950 ? "Big5"
        : codePage == 932 ? "Shift-JIS" : "Windows-1252";
    auto decoder = QStringDecoder(name);
    if (decoder.isValid())
        return decoder(data);
    return QString::fromLatin1(data);
#endif
}

static bool decodeWindowsCodePageStrict(const QByteArray &data, quint32 codePage,
                                        QString *decoded)
{
    if (data.isEmpty()) {
        if (decoded) decoded->clear();
        return true;
    }
#ifdef Q_OS_WIN
    const int length = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                                           data.constData(), data.size(),
                                           nullptr, 0);
    if (length <= 0)
        return false;
    QVector<wchar_t> buffer(length);
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                            data.constData(), data.size(), buffer.data(), length)
        != length) {
        return false;
    }
    if (decoded) *decoded = QString::fromWCharArray(buffer.data(), length);
    return true;
#else
    const char *name = codePage == 936 ? "GBK"
        : codePage == 54936 ? "GB18030"
        : codePage == 950 ? "Big5"
        : codePage == 932 ? "Shift-JIS" : "Windows-1252";
    auto decoder = QStringDecoder(name);
    if (!decoder.isValid())
        return false;
    const QString text = decoder(data);
    if (decoder.hasError())
        return false;
    if (decoded) *decoded = text;
    return true;
#endif
}

static QByteArray encodeWindowsCodePage(const QString &text, quint32 codePage,
                                        bool strict, bool *ok)
{
    if (ok) *ok = true;
#ifdef Q_OS_WIN
    if (text.isEmpty())
        return QByteArray();
    const wchar_t *wptr = reinterpret_cast<const wchar_t *>(text.utf16());
    BOOL usedDefault = FALSE;
    const DWORD flags = strict && codePage != 54936 ? WC_NO_BEST_FIT_CHARS : 0;
    int len = WideCharToMultiByte(codePage, flags, wptr, text.length(),
                                  nullptr, 0, nullptr,
                                  strict && codePage != 54936 ? &usedDefault : nullptr);
    if (len == 0 || usedDefault) {
        if (ok) *ok = false;
        return {};
    }
    QByteArray result(len, '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(codePage, flags, wptr, text.length(),
                            result.data(), len, nullptr,
                            strict && codePage != 54936 ? &usedDefault : nullptr) != len
        || usedDefault) {
        if (ok) *ok = false;
        result.clear();
    }
    if (strict && !result.isEmpty()
        && decodeWindowsCodePage(result, codePage) != text) {
        if (ok) *ok = false;
        result.clear();
    }
    return result;
#else
    const char *name = codePage == 936 ? "GBK"
        : codePage == 54936 ? "GB18030"
        : codePage == 950 ? "Big5"
        : codePage == 932 ? "Shift-JIS" : "Windows-1252";
    auto encoder = QStringEncoder(name);
    if (!encoder.isValid()) {
        if (ok) *ok = false;
        return {};
    }
    QByteArray result = encoder(text);
    if (encoder.hasError()) {
        if (ok) *ok = false;
        result.clear();
    }
    return result;
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
            if (c == 0xF4 && p[i + 1] > 0x8F) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

namespace {

struct EncodingSampleChunk {
    QByteArray bytes;
    qint64 offset = 0;
    bool touchesStart = false;
    bool touchesEnd = false;
};

bool validUtf8Window(const EncodingSampleChunk &chunk)
{
    const int leadingLimit = chunk.touchesStart ? 0 : 3;
    const int trailingLimit = chunk.touchesEnd ? 0 : 3;
    for (int leading = 0; leading <= leadingLimit; ++leading) {
        for (int trailing = 0; trailing <= trailingLimit; ++trailing) {
            if (leading + trailing >= chunk.bytes.size())
                continue;
            if (isValidUtf8(chunk.bytes.sliced(
                    leading, chunk.bytes.size() - leading - trailing))) {
                return true;
            }
        }
    }
    return chunk.bytes.isEmpty();
}

bool hasGb18030FourByteSequence(const QByteArray &sample)
{
    const auto *bytes = reinterpret_cast<const uchar *>(sample.constData());
    for (qsizetype i = 0; i + 3 < sample.size(); ++i) {
        if (bytes[i] >= 0x81 && bytes[i] <= 0xfe
            && bytes[i + 1] >= 0x30 && bytes[i + 1] <= 0x39
            && bytes[i + 2] >= 0x81 && bytes[i + 2] <= 0xfe
            && bytes[i + 3] >= 0x30 && bytes[i + 3] <= 0x39) {
            return true;
        }
    }
    return false;
}

int decodedTextScore(const QString &text, TextEncoding encoding)
{
    int controls = 0;
    int replacement = 0;
    int cjk = 0;
    int kana = 0;
    int latin = 0;
    int simplified = 0;
    int traditional = 0;
    static const QString simplifiedHints = QStringLiteral(
        "这个们来时间国为后里发现应开关数据用户文件");
    static const QString traditionalHints = QStringLiteral(
        "這個們來時間國為後裡發現應開關數據用戶檔案");
    for (const QChar ch : text) {
        const uint value = ch.unicode();
        if (ch == QChar::ReplacementCharacter) ++replacement;
        if (value < 0x20 && ch != QLatin1Char('\n') && ch != QLatin1Char('\r')
            && ch != QLatin1Char('\t') && ch != QLatin1Char('\f')) ++controls;
        if (value >= 0x4e00 && value <= 0x9fff) ++cjk;
        if ((value >= 0x3040 && value <= 0x30ff)
            || (value >= 0xff66 && value <= 0xff9f)) ++kana;
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= 0x00c0 && value <= 0x024f)) ++latin;
        if (simplifiedHints.contains(ch)) ++simplified;
        if (traditionalHints.contains(ch)) ++traditional;
    }
    int score = 1000 - controls * 80 - replacement * 200;
    switch (encoding) {
    case TextEncoding::GBK:
        score += cjk * 3 + simplified * 18 - traditional * 4;
        break;
    case TextEncoding::GB18030:
        score += cjk * 3 + simplified * 18 - traditional * 4;
        break;
    case TextEncoding::Big5:
        score += cjk * 3 + traditional * 18 - simplified * 4;
        break;
    case TextEncoding::ShiftJis:
        score += kana * 24 + cjk * 2;
        if (kana == 0) score -= 40;
        break;
    case TextEncoding::Windows1252:
        score += latin * 2;
        if (cjk || kana) score -= 200;
        break;
    default:
        break;
    }
    return score;
}

bool hasWindows1252TextEvidence(const QString &text)
{
    int asciiLetters = 0;
    int extendedText = 0;
    int controls = 0;
    for (const QChar ch : text) {
        const uint value = ch.unicode();
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'))
            ++asciiLetters;
        if ((value >= 0x00a0 && value <= 0x024f)
            || (value >= 0x2010 && value <= 0x203a))
            ++extendedText;
        if (value < 0x20 && ch != QLatin1Char('\n') && ch != QLatin1Char('\r')
            && ch != QLatin1Char('\t') && ch != QLatin1Char('\f'))
            ++controls;
    }
    // CP1252 can decode nearly every byte stream, so mere round-trip validity
    // is not detection evidence. Require an actual Western-language signal;
    // otherwise an arbitrary invalid UTF-8 tail would be mislabeled CP1252.
    return controls == 0 && asciiLetters >= 4 && extendedText >= 2;
}

bool exactLegacyRoundTripWindow(const EncodingSampleChunk &chunk,
                                TextEncoding encoding, QString *decoded)
{
    quint32 codePage = 0;
    if (!windowsCodePageForEncoding(encoding, &codePage))
        return false;
    const int leadingLimit = chunk.touchesStart ? 0 : 3;
    const int trailingLimit = chunk.touchesEnd ? 0 : 3;
    for (int removed = 0; removed <= leadingLimit + trailingLimit; ++removed) {
        for (int leading = 0; leading <= leadingLimit; ++leading) {
            const int trailing = removed - leading;
            if (trailing < 0 || trailing > trailingLimit
                || leading + trailing >= chunk.bytes.size()) {
                continue;
            }
            const QByteArray candidate = chunk.bytes.sliced(
                leading, chunk.bytes.size() - leading - trailing);
            const QString text = decodeWindowsCodePage(candidate, codePage);
            bool ok = false;
            const QByteArray encoded = encodeWindowsCodePage(text, codePage, true, &ok);
            if (ok && encoded == candidate) {
                if (decoded) *decoded = text;
                return true;
            }
        }
    }
    return chunk.bytes.isEmpty();
}

int encodedUnitLength(const uchar *bytes, qsizetype remaining,
                      TextEncoding encoding, bool *valid)
{
    *valid = true;
    if (remaining <= 0) { *valid = false; return 0; }
    const uchar first = bytes[0];
    switch (encoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom: {
        int length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2
            : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 1;
        if (length > remaining) { *valid = false; return 1; }
        const QByteArray scalar(reinterpret_cast<const char *>(bytes), length);
        if (!isValidUtf8(scalar)) { *valid = false; return 1; }
        return length;
    }
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        if (remaining < 2) { *valid = false; return 1; }
        const quint16 firstUnit = encoding == TextEncoding::Utf16LE
            ? quint16(bytes[0]) | (quint16(bytes[1]) << 8)
            : (quint16(bytes[0]) << 8) | quint16(bytes[1]);
        if (firstUnit >= 0xd800 && firstUnit <= 0xdbff) {
            if (remaining < 4) { *valid = false; return 2; }
            const quint16 secondUnit = encoding == TextEncoding::Utf16LE
                ? quint16(bytes[2]) | (quint16(bytes[3]) << 8)
                : (quint16(bytes[2]) << 8) | quint16(bytes[3]);
            if (secondUnit < 0xdc00 || secondUnit > 0xdfff) {
                *valid = false;
                return 2;
            }
            return 4;
        }
        if (firstUnit >= 0xdc00 && firstUnit <= 0xdfff) *valid = false;
        return 2;
    }
    case TextEncoding::Windows1252:
        return 1;
    case TextEncoding::GB18030:
        if (first >= 0x81 && first <= 0xfe && remaining >= 4
            && bytes[1] >= 0x30 && bytes[1] <= 0x39
            && bytes[2] >= 0x81 && bytes[2] <= 0xfe
            && bytes[3] >= 0x30 && bytes[3] <= 0x39) return 4;
        [[fallthrough]];
    case TextEncoding::GBK:
        if (first >= 0x81 && first <= 0xfe) {
            if (remaining < 2) { *valid = false; return 1; }
            if (bytes[1] < 0x40 || bytes[1] == 0x7f || bytes[1] > 0xfe)
                *valid = false;
            return 2;
        }
        return 1;
    case TextEncoding::Big5:
        if (first >= 0x81 && first <= 0xfe) {
            if (remaining < 2) { *valid = false; return 1; }
            if (!((bytes[1] >= 0x40 && bytes[1] <= 0x7e)
                  || (bytes[1] >= 0xa1 && bytes[1] <= 0xfe)))
                *valid = false;
            return 2;
        }
        return 1;
    case TextEncoding::ShiftJis:
        if ((first >= 0x81 && first <= 0x9f) || (first >= 0xe0 && first <= 0xfc)) {
            if (remaining < 2) { *valid = false; return 1; }
            if (bytes[1] < 0x40 || bytes[1] == 0x7f || bytes[1] > 0xfc)
                *valid = false;
            return 2;
        }
        return 1;
    }
    return 1;
}

QVector<EncodingSampleChunk> memoryEncodingSamples(const QByteArray &data)
{
    if (data.size() <= EncodingDetectionBudgetBytes) {
        return {{data, 0, true, true}};
    }
    const qsizetype side = EncodingDetectionBudgetBytes / 3;
    const qsizetype head = EncodingDetectionBudgetBytes - side * 2;
    qint64 middleOffset = (data.size() - side) / 2;
    middleOffset -= middleOffset % 4;
    const qint64 tailOffset = data.size() - side;
    return {
        {data.first(head), 0, true, false},
        {data.sliced(middleOffset, side), middleOffset, false, false},
        {data.sliced(tailOffset, side), tailOffset, false, true},
    };
}

QVector<EncodingSampleChunk> fileEncodingSamples(qint64 size,
    const std::function<QByteArray(qint64, qsizetype)> &readAt)
{
    if (size <= EncodingDetectionBudgetBytes) {
        return {{readAt(0, qsizetype(size)), 0, true, true}};
    }
    const qint64 side = EncodingDetectionBudgetBytes / 3;
    const qint64 head = EncodingDetectionBudgetBytes - side * 2;
    qint64 middleOffset = (size - side) / 2;
    middleOffset -= middleOffset % 4;
    const qint64 tailOffset = size - side;
    QVector<EncodingSampleChunk> chunks;
    const struct { qint64 offset; qint64 length; bool start; bool end; } ranges[] = {
        {0, head, true, false},
        {middleOffset, side, false, false},
        {tailOffset, side, false, true},
    };
    for (const auto &range : ranges) {
        const QByteArray bytes = readAt(range.offset, qsizetype(range.length));
        if (bytes.size() != range.length)
            return {};
        chunks.push_back({bytes, range.offset, range.start, range.end});
    }
    return chunks;
}

EncodingDetection detectEncodingSamples(const QVector<EncodingSampleChunk> &chunks)
{
    EncodingDetection result;
    if (chunks.isEmpty())
        return result;
    const QByteArray &head = chunks.constFirst().bytes;
    if (head.startsWith("\xef\xbb\xbf"))
        return {TextEncoding::Utf8Bom, EncodingOrigin::Bom,
                EncodingConfidence::Certain, true, false};
    if (head.startsWith("\xff\xfe"))
        return {TextEncoding::Utf16LE, EncodingOrigin::Bom,
                EncodingConfidence::Certain, true, false};
    if (head.startsWith("\xfe\xff"))
        return {TextEncoding::Utf16BE, EncodingOrigin::Bom,
                EncodingConfidence::Certain, true, false};

    bool validUtf8 = true;
    for (const auto &chunk : chunks)
        validUtf8 = validUtf8 && validUtf8Window(chunk);
    if (validUtf8)
        return {TextEncoding::Utf8, EncodingOrigin::Detected,
                EncodingConfidence::High, false, false};

    qint64 evenNul = 0;
    qint64 oddNul = 0;
    qint64 evenBytes = 0;
    qint64 oddBytes = 0;
    qint64 sampledBytes = 0;
    for (const auto &chunk : chunks) {
        sampledBytes += chunk.bytes.size();
        for (qsizetype i = 0; i < chunk.bytes.size(); ++i) {
            const bool odd = ((chunk.offset + i) & 1) != 0;
            if (odd) {
                ++oddBytes;
                oddNul += chunk.bytes[i] == '\0';
            } else {
                ++evenBytes;
                evenNul += chunk.bytes[i] == '\0';
            }
        }
    }
    if (sampledBytes >= 8) {
        if (oddNul * 5 > oddBytes * 2 && evenNul * 20 < qMax<qint64>(1, evenBytes))
            return {TextEncoding::Utf16LE, EncodingOrigin::Detected,
                    EncodingConfidence::Medium, false, false};
        if (evenNul * 5 > evenBytes * 2 && oddNul * 20 < qMax<qint64>(1, oddBytes))
            return {TextEncoding::Utf16BE, EncodingOrigin::Detected,
                    EncodingConfidence::Medium, false, false};
    }

    struct Candidate { TextEncoding encoding; int score; };
    QVector<Candidate> candidates;
    const TextEncoding legacy[] = {TextEncoding::GB18030, TextEncoding::GBK,
        TextEncoding::Big5, TextEncoding::ShiftJis, TextEncoding::Windows1252};
    bool hasFourByte = false;
    for (const auto &chunk : chunks)
        hasFourByte = hasFourByte || hasGb18030FourByteSequence(chunk.bytes);
    for (TextEncoding encoding : legacy) {
        QString decoded;
        bool exact = true;
        for (const auto &chunk : chunks) {
            QString part;
            if (!exactLegacyRoundTripWindow(chunk, encoding, &part)) {
                exact = false;
                break;
            }
            decoded += part;
            decoded += QLatin1Char('\n');
        }
        if (!exact)
            continue;
        if (encoding == TextEncoding::Windows1252
            && !hasWindows1252TextEvidence(decoded)) {
            continue;
        }
        if (encoding == TextEncoding::GB18030 && hasFourByte) {
            return {TextEncoding::GB18030, EncodingOrigin::Detected,
                    EncodingConfidence::High, false, false};
        }
        int score = decodedTextScore(decoded, encoding);
        if (encoding == TextEncoding::GB18030) score += hasFourByte ? 120 : -12;
        if (encoding == TextEncoding::GBK && hasFourByte) score -= 200;
        candidates.push_back({encoding, score});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a,
                                                        const Candidate &b) {
        return a.score > b.score;
    });
    if (!candidates.isEmpty()) {
        const int margin = candidates.size() == 1
            ? (std::numeric_limits<int>::max)()
            : candidates[0].score - candidates[1].score;
        if (margin >= 12 || hasFourByte
            || candidates[0].encoding == TextEncoding::ShiftJis) {
            result.encoding = candidates[0].encoding;
            result.origin = EncodingOrigin::Detected;
            result.confidence = margin >= 80 ? EncodingConfidence::High
                                             : EncodingConfidence::Medium;
            return result;
        }
    }
    result.decodeErrors = !validUtf8;
    return result;
}

bool decodeTextFragmentStrict(const QByteArray &bytes, TextEncoding encoding,
                              QString *decoded);

} // namespace

EncodingDetection detectEncoding(const QByteArray &data)
{
    return detectEncodingSamples(memoryEncodingSamples(data));
}

EncodingDetection detectFileEncoding(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return detectFileEncoding(file.size(), [&file](qint64 offset, qsizetype length) {
        return file.seek(offset) ? file.read(length) : QByteArray{};
    });
}

EncodingDetection detectFileEncoding(qint64 size,
    const std::function<QByteArray(qint64, qsizetype)> &readAt)
{
    return detectEncodingSamples(fileEncodingSamples(size, readAt));
}

DecodeResult decodeBytes(const QByteArray &data)
{
    const EncodingDetection detection = detectEncoding(data);
    DecodeResult result = decodeBytesWithEncoding(data, detection.encoding);
    if (result.decodeErrors && detection.origin != EncodingOrigin::Bom) {
        result = decodeBytesWithEncoding(data, TextEncoding::Utf8);
        result.encoding = TextEncoding::Utf8;
        result.hasBom = false;
        result.origin = EncodingOrigin::Fallback;
        result.confidence = EncodingConfidence::Low;
        result.decodeErrors = true;
        return result;
    }
    result.encoding = detection.encoding;
    result.hasBom = detection.hasBom;
    result.origin = detection.origin;
    result.confidence = detection.confidence;
    return result;
}

DecodeResult decodeBytesWithEncoding(const QByteArray &data,
                                     TextEncoding encoding)
{
    DecodeResult result;
    result.encoding = encoding;
    result.origin = EncodingOrigin::UserSelected;
    result.confidence = EncodingConfidence::Certain;
    QByteArray body = data;
    if (encoding == TextEncoding::Utf8Bom && body.startsWith("\xef\xbb\xbf")) {
        body.remove(0, 3);
        result.hasBom = true;
    } else if (encoding == TextEncoding::Utf16LE && body.startsWith("\xff\xfe")) {
        body.remove(0, 2);
        result.hasBom = true;
    } else if (encoding == TextEncoding::Utf16BE && body.startsWith("\xfe\xff")) {
        body.remove(0, 2);
        result.hasBom = true;
    }
    if (!decodeTextFragmentStrict(body, encoding, &result.text)) {
        const DecodedTextProjection projection = decodeTextProjection(body, encoding);
        result.text = projection.text;
        result.decodeErrors = true;
    }
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
    case TextEncoding::GB18030:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::Windows1252: {
        quint32 codePage = 0;
        windowsCodePageForEncoding(encoding, &codePage);
        return encodeWindowsCodePage(text, codePage, false, nullptr);
    }
    }
    return text.toUtf8();
}

QByteArray encodeTextFragment(const QString &text, TextEncoding encoding,
                              bool *ok)
{
    if (ok) *ok = true;
    switch (encoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        return text.toUtf8();
    case TextEncoding::Utf16LE:
        return QByteArray(reinterpret_cast<const char *>(text.utf16()),
                          text.size() * 2);
    case TextEncoding::Utf16BE: {
        QByteArray result;
        result.reserve(text.size() * 2);
        for (const QChar ch : text) {
            result.append(char(ch.unicode() >> 8));
            result.append(char(ch.unicode() & 0xff));
        }
        return result;
    }
    case TextEncoding::GBK:
    case TextEncoding::GB18030:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::Windows1252: {
        quint32 codePage = 0;
        windowsCodePageForEncoding(encoding, &codePage);
        return encodeWindowsCodePage(text, codePage, true, ok);
    }
    }
    if (ok) *ok = false;
    return {};
}

namespace {

QByteArray encodingBom(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8Bom: return QByteArray("\xef\xbb\xbf", 3);
    case TextEncoding::Utf16LE: return QByteArray("\xff\xfe", 2);
    case TextEncoding::Utf16BE: return QByteArray("\xfe\xff", 2);
    default: return {};
    }
}

qsizetype completeEncodedPrefix(const QByteArray &data, TextEncoding encoding,
                                bool finalChunk)
{
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
    qsizetype position = 0;
    while (position < data.size()) {
        bool valid = false;
        const int length = encodedUnitLength(bytes + position,
                                             data.size() - position,
                                             encoding, &valid);
        if (!valid && !finalChunk && data.size() - position <= 4)
            break;
        position += qMax(1, length);
    }
    return position;
}

bool decodeTextFragmentStrict(const QByteArray &bytes, TextEncoding encoding,
                              QString *decoded)
{
    if (bytes.isEmpty()) {
        if (decoded) decoded->clear();
        return true;
    }
    switch (encoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom:
        if (!isValidUtf8(bytes))
            return false;
        if (decoded) *decoded = QString::fromUtf8(bytes);
        return true;
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            bool valid = false;
            const int length = encodedUnitLength(raw + offset,
                                                 bytes.size() - offset,
                                                 encoding, &valid);
            if (!valid)
                return false;
            offset += length;
        }
        QByteArray native = bytes;
        if (encoding == TextEncoding::Utf16BE) {
            for (qsizetype i = 0; i + 1 < native.size(); i += 2)
                std::swap(native[i], native[i + 1]);
        }
        if (decoded) {
            *decoded = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(native.constData()),
                native.size() / 2);
        }
        return true;
    }
    case TextEncoding::GBK:
    case TextEncoding::GB18030:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::Windows1252: {
        quint32 codePage = 0;
        return windowsCodePageForEncoding(encoding, &codePage)
               && decodeWindowsCodePageStrict(bytes, codePage, decoded);
    }
    }
    return false;
}

bool writeAll(QIODevice *device, const QByteArray &bytes, QString *error)
{
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = device->write(bytes.constData() + offset,
                                             bytes.size() - offset);
        if (written <= 0) {
            if (error) *error = QObject::tr("写入目标文件失败：%1")
                                    .arg(device->errorString());
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

bool writeTextAsEncoding(const QString &path, const QString &text,
                         TextEncoding encoding, QString *error)
{
    bool representable = true;
    const QByteArray body = encodeTextFragment(text, encoding, &representable);
    if (!representable) {
        if (error) {
            *error = QObject::tr("文本包含 %1 无法表示的字符。")
                         .arg(encodingName(encoding));
        }
        return false;
    }
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = QObject::tr("无法创建目标文件：%1")
                                .arg(output.errorString());
        return false;
    }
    if (!writeAll(&output, encodingBom(encoding), error)
        || !writeAll(&output, body, error)) {
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        if (error) *error = QObject::tr("无法提交目标文件：%1")
                                .arg(output.errorString());
        return false;
    }
    return true;
}

bool transcodeFileEncoding(
    const QString &sourcePath, TextEncoding sourceEncoding,
    const QString &targetPath, TextEncoding targetEncoding,
    QString *error, const std::atomic_bool *cancelled,
    const std::function<void(qint64, qint64)> &progress)
{
    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("无法读取源文件：%1").arg(input.errorString());
        return false;
    }
    const QFileInfo sourceBefore(sourcePath);
    const qint64 sourceModifiedBefore = sourceBefore.lastModified().toMSecsSinceEpoch();
    QSaveFile output(targetPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = QObject::tr("无法创建目标文件：%1")
                                .arg(output.errorString());
        return false;
    }
    if (!writeAll(&output, encodingBom(targetEncoding), error))
        return false;

    QByteArray carry;
    qint64 consumed = 0;
    const qint64 total = input.size();
    bool first = true;
    while (!input.atEnd() || !carry.isEmpty()) {
        if (cancelled && cancelled->load()) {
            output.cancelWriting();
            if (error) *error = QObject::tr("转换已取消。");
            return false;
        }
        QByteArray block = carry;
        if (!input.atEnd())
            block += input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError) {
            if (error) *error = QObject::tr("读取源文件失败：%1").arg(input.errorString());
            return false;
        }
        const bool finalChunk = input.atEnd();
        const qsizetype complete = completeEncodedPrefix(block, sourceEncoding,
                                                         finalChunk);
        carry = block.mid(complete);
        QByteArray prefix = block.first(complete);
        if (first) {
            first = false;
            if (sourceEncoding == TextEncoding::Utf8Bom
                && prefix.startsWith("\xef\xbb\xbf")) prefix.remove(0, 3);
            else if (sourceEncoding == TextEncoding::Utf16LE
                     && prefix.startsWith("\xff\xfe")) prefix.remove(0, 2);
            else if (sourceEncoding == TextEncoding::Utf16BE
                     && prefix.startsWith("\xfe\xff")) prefix.remove(0, 2);
        }
        QString decoded;
        if (!decodeTextFragmentStrict(prefix, sourceEncoding, &decoded)) {
            output.cancelWriting();
            if (error) {
                *error = QObject::tr("源文件在约字节 %1 处不符合所选的 %2 编码，未生成目标文件。")
                             .arg(consumed)
                             .arg(encodingName(sourceEncoding));
            }
            return false;
        }
        bool representable = true;
        const QByteArray encoded = encodeTextFragment(decoded, targetEncoding,
                                                      &representable);
        if (!representable) {
            output.cancelWriting();
            if (error) {
                *error = QObject::tr("源文件包含 %1 无法表示的字符（约在源字节 %2）。")
                             .arg(encodingName(targetEncoding))
                             .arg(consumed);
            }
            return false;
        }
        if (!writeAll(&output, encoded, error)) {
            output.cancelWriting();
            return false;
        }
        consumed += complete;
        if (progress) progress(qMin(consumed, total), total);
        if (finalChunk && carry.isEmpty())
            break;
    }
    input.close();
    const QFileInfo sourceAfter(sourcePath);
    if (!sourceAfter.exists() || sourceAfter.size() != total
        || sourceAfter.lastModified().toMSecsSinceEpoch() != sourceModifiedBefore) {
        output.cancelWriting();
        if (error) {
            *error = QObject::tr("转换期间源文件发生变化，目标文件未提交。请重新打开源文件后再试。");
        }
        return false;
    }
    if (!output.commit()) {
        if (error) *error = QObject::tr("无法提交目标文件：%1")
                                .arg(output.errorString());
        return false;
    }
    return true;
}

QString encodingName(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:    return QStringLiteral("UTF-8");
    case TextEncoding::Utf8Bom: return QStringLiteral("UTF-8 BOM");
    case TextEncoding::Utf16LE: return QStringLiteral("UTF-16LE");
    case TextEncoding::Utf16BE: return QStringLiteral("UTF-16BE");
    case TextEncoding::GBK:     return QStringLiteral("GBK");
    case TextEncoding::GB18030: return QStringLiteral("GB18030");
    case TextEncoding::Big5: return QStringLiteral("Big5");
    case TextEncoding::ShiftJis: return QStringLiteral("Shift-JIS");
    case TextEncoding::Windows1252: return QStringLiteral("Windows-1252");
    }
    return QStringLiteral("Unknown");
}

std::optional<TextEncoding> encodingFromName(const QString &name)
{
    static constexpr TextEncoding supported[] = {
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
        if (name == encodingName(encoding))
            return encoding;
    }
    return std::nullopt;
}

bool windowsCodePageForEncoding(TextEncoding encoding, quint32 *codePage)
{
    quint32 value = 0;
    switch (encoding) {
    case TextEncoding::GBK: value = 936; break;
    case TextEncoding::GB18030: value = 54936; break;
    case TextEncoding::Big5: value = 950; break;
    case TextEncoding::ShiftJis: value = 932; break;
    case TextEncoding::Windows1252: value = 1252; break;
    default: return false;
    }
    if (codePage) *codePage = value;
    return true;
}

QString decodeTextFragment(const QByteArray &bytes, TextEncoding encoding)
{
    if (bytes.isEmpty())
        return {};
    switch (encoding) {
    case TextEncoding::Utf8:
        return QString::fromUtf8(bytes);
    case TextEncoding::Utf8Bom:
        return bytes.startsWith("\xEF\xBB\xBF")
            ? QString::fromUtf8(bytes.sliced(3)) : QString::fromUtf8(bytes);
    case TextEncoding::Utf16LE: {
        qsizetype offset = bytes.startsWith("\xFF\xFE") ? 2 : 0;
        QString result;
        result.reserve((bytes.size() - offset) / 2);
        for (qsizetype i = offset; i + 1 < bytes.size(); i += 2) {
            result.append(QChar(quint16(uchar(bytes[i]))
                                | (quint16(uchar(bytes[i + 1])) << 8)));
        }
        return result;
    }
    case TextEncoding::Utf16BE: {
        qsizetype offset = bytes.startsWith("\xFE\xFF") ? 2 : 0;
        QString result;
        result.reserve((bytes.size() - offset) / 2);
        for (qsizetype i = offset; i + 1 < bytes.size(); i += 2) {
            result.append(QChar((quint16(uchar(bytes[i])) << 8)
                                | quint16(uchar(bytes[i + 1]))));
        }
        return result;
    }
    case TextEncoding::GBK:
    case TextEncoding::GB18030:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::Windows1252: {
        quint32 codePage = 0;
        windowsCodePageForEncoding(encoding, &codePage);
        return decodeWindowsCodePage(bytes, codePage);
    }
    }
    return QString::fromUtf8(bytes);
}

DecodedTextProjection decodeTextProjection(const QByteArray &bytes,
                                           TextEncoding encoding,
                                           bool stripLeadingBom,
                                           bool buildProjectedMap)
{
    DecodedTextProjection projection;
    projection.text.reserve(bytes.size());
    if (buildProjectedMap) {
        projection.projectedToSource.reserve(bytes.size() + 1);
        projection.projectedBoundaries.reserve(bytes.size() / 2 + 2);
    }
    projection.sourceBoundaries.reserve(bytes.size() / 2 + 2);

    qsizetype source = 0;
    if (stripLeadingBom) {
        if ((encoding == TextEncoding::Utf8Bom || encoding == TextEncoding::Utf8)
            && bytes.startsWith("\xef\xbb\xbf")) source = 3;
        else if (encoding == TextEncoding::Utf16LE && bytes.startsWith("\xff\xfe")) source = 2;
        else if (encoding == TextEncoding::Utf16BE && bytes.startsWith("\xfe\xff")) source = 2;
    }
    qint32 projected = 0;
    if (buildProjectedMap)
        projection.projectedToSource.append(qint32(source));
    projection.sourceBoundaries.append(qint32(source));
    if (buildProjectedMap)
        projection.projectedBoundaries.append(0);
    while (source < bytes.size()) {
        bool valid = false;
        const int length = encodedUnitLength(
            reinterpret_cast<const uchar *>(bytes.constData() + source),
            bytes.size() - source, encoding, &valid);
        const int consumed = qMax(1, length);
        uint codePoint = QChar::ReplacementCharacter;
        if (valid) {
            const auto *unit = reinterpret_cast<const uchar *>(
                bytes.constData() + source);
            switch (encoding) {
            case TextEncoding::Utf8:
            case TextEncoding::Utf8Bom: {
                codePoint = unit[0];
                if (consumed == 2) {
                    codePoint = ((unit[0] & 0x1fu) << 6)
                        | (unit[1] & 0x3fu);
                } else if (consumed == 3) {
                    codePoint = ((unit[0] & 0x0fu) << 12)
                        | ((unit[1] & 0x3fu) << 6)
                        | (unit[2] & 0x3fu);
                } else if (consumed == 4) {
                    codePoint = ((unit[0] & 0x07u) << 18)
                        | ((unit[1] & 0x3fu) << 12)
                        | ((unit[2] & 0x3fu) << 6)
                        | (unit[3] & 0x3fu);
                }
                if (codePoint <= 0xffff) {
                    projection.text.append(QChar(ushort(codePoint)));
                } else {
                    projection.text.append(QChar::highSurrogate(codePoint));
                    projection.text.append(QChar::lowSurrogate(codePoint));
                }
                break;
            }
            case TextEncoding::Utf16LE:
            case TextEncoding::Utf16BE: {
                ushort first = 0;
                ushort second = 0;
                for (int offset = 0; offset + 1 < consumed; offset += 2) {
                    const ushort value = encoding == TextEncoding::Utf16LE
                        ? ushort(unit[offset] | (ushort(unit[offset + 1]) << 8))
                        : ushort((ushort(unit[offset]) << 8) | unit[offset + 1]);
                    if (offset == 0)
                        first = value;
                    else
                        second = value;
                    projection.text.append(QChar(value));
                }
                codePoint = consumed == 4
                    ? QChar::surrogateToUcs4(
                          QChar(first), QChar(second))
                    : first;
                break;
            }
            default: {
                const QString scalar = decodeTextFragment(
                    bytes.sliced(source, consumed), encoding);
                if (scalar.isEmpty()) {
                    valid = false;
                    break;
                }
                projection.text += scalar;
                codePoint = scalar.at(0).unicode();
                if (scalar.size() >= 2 && scalar.at(0).isHighSurrogate()
                    && scalar.at(1).isLowSurrogate()) {
                    codePoint = QChar::surrogateToUcs4(
                        scalar.at(0), scalar.at(1));
                }
                break;
            }
            }
        }
        if (!valid) {
            projection.text.append(QChar::ReplacementCharacter);
            codePoint = QChar::ReplacementCharacter;
            projection.decodeErrors = true;
        }
        const int projectedBytes = codePoint <= 0x7f ? 1
            : codePoint <= 0x7ff ? 2
            : codePoint <= 0xffff ? 3 : 4;
        if (buildProjectedMap) {
            for (int index = 1; index < projectedBytes; ++index)
                projection.projectedToSource.append(qint32(source));
        }
        source += consumed;
        projected += projectedBytes;
        if (buildProjectedMap)
            projection.projectedToSource.append(qint32(source));
        projection.sourceBoundaries.append(qint32(source));
        if (buildProjectedMap)
            projection.projectedBoundaries.append(projected);
    }
    return projection;
}

qint32 TextProjectionPositionMapper::toSource(
    const QVector<qint32> &projectedToSource,
    qint64 projectedPosition,
    TextBoundaryAffinity affinity)
{
    if (projectedToSource.isEmpty())
        return 0;
    const qsizetype index = static_cast<qsizetype>(std::clamp<qint64>(
        projectedPosition, 0, projectedToSource.size() - 1));
    qint32 source = projectedToSource.at(index);
    if (affinity == TextBoundaryAffinity::End && index > 0
        && projectedToSource.at(index - 1) == source) {
        const auto next = std::upper_bound(
            projectedToSource.cbegin() + index,
            projectedToSource.cend(), source);
        if (next != projectedToSource.cend())
            source = *next;
    }
    return source;
}

qint32 TextProjectionPositionMapper::toProjected(
    const QVector<qint32> &sourceBoundaries,
    const QVector<qint32> &projectedBoundaries,
    qint64 sourceOffset)
{
    if (sourceBoundaries.isEmpty() || projectedBoundaries.isEmpty())
        return 0;
    const qint32 relative = qint32(std::clamp<qint64>(
        sourceOffset, 0, sourceBoundaries.constLast()));
    const auto found = std::lower_bound(sourceBoundaries.cbegin(),
                                        sourceBoundaries.cend(), relative);
    const qsizetype index = found == sourceBoundaries.cend()
        ? sourceBoundaries.size() - 1
        : qsizetype(found - sourceBoundaries.cbegin());
    return projectedBoundaries.value(index, projectedBoundaries.constLast());
}

QString decodeByteGridText(const QByteArray &bytes, TextEncoding encoding,
                           int bytesPerRow)
{
    bytesPerRow = qMax(1, bytesPerRow);
    QString cells(bytes.size(), QLatin1Char(' '));
    const DecodedTextProjection projection = decodeTextProjection(
        bytes, encoding, false, false);
    int utf16 = 0;
    const int units = qMax(0, int(projection.sourceBoundaries.size()) - 1);
    for (int unitIndex = 0; unitIndex < units; ++unitIndex) {
        const int source = projection.sourceBoundaries.at(unitIndex);
        const int consumed = projection.sourceBoundaries.at(unitIndex + 1)
            - source;
        const int scalarUnits = utf16 < projection.text.size()
                && projection.text.at(utf16).isHighSurrogate()
                && utf16 + 1 < projection.text.size()
                && projection.text.at(utf16 + 1).isLowSurrogate()
            ? 2 : 1;
        QString scalar = projection.text.mid(utf16, scalarUnits);
        utf16 += scalarUnits;

        // Control characters must not create extra rows or tabs in the fixed
        // byte grid. They remain exact raw bytes in the Hex pane and use a
        // visible placeholder only for this character projection.
        const bool layoutControl = scalar.contains(QLatin1Char('\r'))
            || scalar.contains(QLatin1Char('\n'))
            || scalar.contains(QLatin1Char('\t'))
            || scalar.contains(QChar(0));
        if (layoutControl)
            scalar = QString(QLatin1Char('.'));

        // Every raw byte boundary in a Hex character grid must remain a valid
        // Qt cursor position. A UTF-16 surrogate pair has no legal cursor
        // boundary between its two code units, so representing it directly
        // would make one source byte unselectable. Keep the complete scalar in
        // the flowing Text view and use the ordinary replacement projection in
        // this byte-addressable pane.
        bool unstableCursorTopology = scalar.size() == 2;
        if (scalar.size() == 1) {
            const QChar value = scalar.at(0);
            switch (value.category()) {
            case QChar::Mark_NonSpacing:
            case QChar::Mark_SpacingCombining:
            case QChar::Mark_Enclosing:
            case QChar::Other_Format:
            case QChar::Other_Surrogate:
                unstableCursorTopology = true;
                break;
            default:
                break;
            }
            switch (value.direction()) {
            case QChar::DirR:
            case QChar::DirAN:
            case QChar::DirAL:
            case QChar::DirRLE:
            case QChar::DirRLO:
            case QChar::DirRLI:
            case QChar::DirLRE:
            case QChar::DirLRO:
            case QChar::DirPDF:
            case QChar::DirNSM:
            case QChar::DirBN:
            case QChar::DirLRI:
            case QChar::DirFSI:
            case QChar::DirPDI:
                unstableCursorTopology = true;
                break;
            default:
                break;
            }
        }
        if (unstableCursorTopology) {
            scalar = QString(QChar::ReplacementCharacter);
        }
        const int units = qMin<int>(scalar.size(), consumed);
        for (int unit = 0; unit < units; ++unit)
            cells[source + unit] = scalar.at(unit);
    }

    QString result;
    result.reserve(cells.size() + cells.size() / bytesPerRow);
    for (qsizetype index = 0; index < cells.size(); ++index) {
        result.append(cells.at(index));
        if ((index + 1) % bytesPerRow == 0 && index + 1 < cells.size())
            result.append(QLatin1Char('\n'));
    }
    return result;
}

TextEncoding defaultEncodingForFile(const QString &fileName)
{
    Q_UNUSED(fileName);
    return TextEncoding::Utf8;
}
