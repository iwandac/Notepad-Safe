#include "EditableTextSearch.h"

#include <QStringView>

#include <algorithm>

namespace {

bool wordCharacter(const QString &text, int index)
{
    if (index < 0 || index >= text.size())
        return false;
    uint scalar = text.at(index).unicode();
    if (text.at(index).isLowSurrogate() && index > 0
        && text.at(index - 1).isHighSurrogate()) {
        scalar = QChar::surrogateToUcs4(text.at(index - 1), text.at(index));
    } else if (text.at(index).isHighSurrogate() && index + 1 < text.size()
               && text.at(index + 1).isLowSurrogate()) {
        scalar = QChar::surrogateToUcs4(text.at(index), text.at(index + 1));
    }
    return scalar == uint('_') || QChar::isLetterOrNumber(scalar);
}

bool wholeWordAccepted(const QString &text, int start, int length)
{
    return (start == 0 || !wordCharacter(text, start - 1))
        && (start + length >= text.size()
            || !wordCharacter(text, start + length));
}

QVector<qint32> utf16ToUtf8Boundaries(const QString &text)
{
    QVector<qint32> mapping(text.size() + 1, 0);
    qint32 bytes = 0;
    for (int index = 0; index < text.size();) {
        mapping[index] = bytes;
        const int units = text.at(index).isHighSurrogate()
                && index + 1 < text.size()
                && text.at(index + 1).isLowSurrogate()
            ? 2 : 1;
        if (units == 2)
            mapping[index + 1] = bytes;
        bytes += QStringView(text).mid(index, units).toString().toUtf8().size();
        index += units;
        mapping[index] = bytes;
    }
    return mapping;
}

} // namespace

QVector<EditableTextHit> findEditableTextLiteralHits(
    const QByteArray &subject, const QString &pattern, bool caseSensitive,
    bool wholeWord, const std::function<bool()> &cancelled,
    const std::function<void(qint64)> &progress, bool firstOnly,
    bool reverse, qint64 byteBoundary, qsizetype maximumHits)
{
    QVector<EditableTextHit> hits;
    if (pattern.isEmpty())
        return hits;
    const QString text = QString::fromUtf8(subject);
    const QVector<qint32> bytesAt = utf16ToUtf8Boundaries(text);
    const Qt::CaseSensitivity sensitivity = caseSensitive
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    int from = reverse ? text.size() : 0;
    if (byteBoundary >= 0) {
        const auto boundary = std::lower_bound(
            bytesAt.cbegin(), bytesAt.cend(), qint32(qMin<qint64>(
                byteBoundary, std::numeric_limits<qint32>::max())));
        from = int(std::distance(bytesAt.cbegin(), boundary));
    }
    int hit = -1;
    if (reverse) {
        if (from > 0)
            hit = text.lastIndexOf(pattern, qMin(from - 1, text.size()),
                                   sensitivity);
    } else {
        hit = text.indexOf(pattern, qBound(0, from, text.size()), sensitivity);
    }
    qint64 lastReported = 0;
    while (hit >= 0) {
        if (cancelled && cancelled()) {
            hits.clear();
            return hits;
        }
        const int end = qMin(text.size(), hit + pattern.size());
        if (!wholeWord || wholeWordAccepted(text, hit, end - hit)) {
            EditableTextHit value;
            value.byteOffset = bytesAt.at(hit);
            value.byteLength = bytesAt.at(end) - bytesAt.at(hit);
            value.utf16Offset = hit;
            value.utf16Length = end - hit;
            hits.append(value);
            if (firstOnly || hits.size() >= maximumHits)
                break;
        }
        const qint64 examined = reverse
            ? qint64(subject.size()) - bytesAt.at(hit) : bytesAt.at(hit);
        if (progress && examined >= lastReported + 256 * 1024) {
            lastReported = examined;
            progress(qMin<qint64>(examined, subject.size()));
        }
        hit = reverse
            ? text.lastIndexOf(pattern, hit - 1, sensitivity)
            : text.indexOf(pattern, hit + 1, sensitivity);
    }
    if (progress)
        progress(subject.size());
    return hits;
}
