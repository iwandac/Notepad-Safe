#pragma once

#include <QString>
#include <QByteArray>
#include <QVector>
#include <atomic>
#include <functional>

// 文本编码类型
enum class TextEncoding {
    Utf8,          // UTF-8 无 BOM
    Utf8Bom,       // UTF-8 with BOM
    Utf16LE,       // UTF-16 Little Endian
    Utf16BE,       // UTF-16 Big Endian
    GBK,           // Windows CP936 / GBK
    GB18030,       // Windows CP54936 / GB18030
    Big5,          // Windows CP950 / Big5
    ShiftJis,      // Windows CP932 / Shift-JIS
    Windows1252,   // Western European Windows code page
};

enum class EncodingOrigin {
    Bom,
    Detected,
    UserSelected,
    Fallback,
};

enum class EncodingConfidence {
    Certain,
    High,
    Medium,
    Low,
};

struct EncodingDetection {
    TextEncoding encoding = TextEncoding::Utf8;
    EncodingOrigin origin = EncodingOrigin::Fallback;
    EncodingConfidence confidence = EncodingConfidence::Low;
    bool hasBom = false;
    bool decodeErrors = false;
};

// 解码结果
struct DecodeResult {
    QString text;
    TextEncoding encoding = TextEncoding::Utf8;
    bool hasBom = false;
    EncodingOrigin origin = EncodingOrigin::Fallback;
    EncodingConfidence confidence = EncodingConfidence::Low;
    bool decodeErrors = false;
};

// Encoding detection is deliberately independent of the document size tier.
// Both memory and file entry points use the same fixed total budget distributed
// across the beginning, middle and end of the document. Detection is bounded;
// exact source validation is performed while decoding/saving/converting.
constexpr qsizetype EncodingDetectionBudgetBytes = 1024 * 1024;
EncodingDetection detectEncoding(const QByteArray &data);
EncodingDetection detectFileEncoding(const QString &path);

// 从字节流解码文本（自动检测编码）
// 检测顺序：BOM → UTF-8 严格验证 → UTF-16 启发式 → 受支持代码页评分
// → 无法可靠判断时以 UTF-8 回退并保留非法字节状态。
DecodeResult decodeBytes(const QByteArray &data);
DecodeResult decodeBytesWithEncoding(const QByteArray &data,
                                     TextEncoding encoding);

// 将文本编码为字节流
QByteArray encodeText(const QString &text, TextEncoding encoding);

// Encode content inserted into an existing document. Unlike encodeText(),
// this never emits a BOM. When ok is supplied, false means the source contains
// characters that the requested legacy encoding cannot represent exactly.
QByteArray encodeTextFragment(const QString &text, TextEncoding encoding,
                              bool *ok = nullptr);

// Write a complete document with the requested encoding and BOM policy.
// Legacy encodings are strict: unrepresentable characters fail instead of
// being silently replaced. QSaveFile keeps an existing destination intact.
bool writeTextAsEncoding(const QString &path, const QString &text,
                         TextEncoding encoding, QString *error = nullptr);

// Bounded-memory file conversion for mapped/large documents. The source is
// decoded with sourceEncoding and atomically written as targetEncoding.
bool transcodeFileEncoding(
    const QString &sourcePath, TextEncoding sourceEncoding,
    const QString &targetPath, TextEncoding targetEncoding,
    QString *error = nullptr, const std::atomic_bool *cancelled = nullptr,
    const std::function<void(qint64, qint64)> &progress = {});

// Decode a raw fragment for display. Invalid input is represented, never
// dropped, and the source byte array is not modified.
QString decodeTextFragment(const QByteArray &bytes, TextEncoding encoding);

// A decoded window plus the only byte mapping the GUI needs. Mapping entries
// are relative to the supplied raw window and Scintilla's projected UTF-8
// byte positions; no whole-file position table is built.
struct DecodedTextProjection {
    QString text;
    QVector<qint32> projectedToSource;
    QVector<qint32> sourceBoundaries;
    QVector<qint32> projectedBoundaries;
    bool decodeErrors = false;
};

// The one authority for translating half-open selections between Scintilla's
// UTF-8 projection positions and source-byte positions.  A position exactly
// on a scalar boundary maps to that boundary for either affinity; a position
// inside a projected scalar rounds outward according to the requested edge.
enum class TextBoundaryAffinity { Start, End };

class TextProjectionPositionMapper final
{
public:
    static qint32 toSource(
        const QVector<qint32> &projectedToSource,
        qint64 projectedPosition,
        TextBoundaryAffinity affinity);
    static qint32 toProjected(
        const QVector<qint32> &sourceBoundaries,
        const QVector<qint32> &projectedBoundaries,
        qint64 sourceOffset);
};

DecodedTextProjection decodeTextProjection(const QByteArray &bytes,
                                           TextEncoding encoding,
                                           bool stripLeadingBom = false,
                                           bool buildProjectedMap = true);

// Encoding-aware character cells for the right side of a Hex view. The
// returned text has exactly one UTF-16 cell per source byte (plus row breaks):
// a decoded scalar occupies the first cell(s) of its encoded byte range and
// the remaining continuation cells are blank. Invalid input is U+FFFD. This
// keeps byte hit-testing stable while using the same multibyte decoder as the
// flowing Binary Text projection.
QString decodeByteGridText(const QByteArray &bytes, TextEncoding encoding,
                           int bytesPerRow);

// True for encodings whose characters are represented by a stateless Windows
// byte code page. The number is explicit so the machine ANSI setting can never
// change file meaning.
bool windowsCodePageForEncoding(TextEncoding encoding, quint32 *codePage);

// 编码名称（用于 UI 显示）
QString encodingName(TextEncoding encoding);

// 严格 UTF-8 验证（不含 BOM）
bool isValidUtf8(const QByteArray &data);

// 从文件后缀猜测默认编码（.txt 可能是 GBK，其他默认 UTF-8）
TextEncoding defaultEncodingForFile(const QString &fileName);
