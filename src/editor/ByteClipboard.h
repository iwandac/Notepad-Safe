#pragma once

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QString>

#include "TextEncoding.h"

namespace ByteClipboard {

inline constexpr const char *RawBytesMime =
    "application/x-notepadsafe-raw-bytes";
inline constexpr const char *SearchModeMime =
    "application/x-notepadsafe-search-mode";

inline bool isPrintableAscii(const QByteArray &bytes)
{
    for (const char value : bytes) {
        const uchar byte = uchar(value);
        if (byte < 0x20 || byte > 0x7e)
            return false;
    }
    return true;
}

inline QString hexText(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

// The character pane is an encoding projection of authoritative bytes. Keep
// the raw range for lossless in-app paste while exposing exactly the same
// decoded text contract as Binary Text to ordinary clipboard consumers. A
// Find paste deliberately uses the private bytes: projected replacement
// characters need not identify one unique source byte sequence.
inline void setCharacterSelection(const QByteArray &bytes,
                                  TextEncoding encoding)
{
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(RawBytesMime), bytes);
    mime->setText(decodeTextProjection(bytes, encoding, false, false).text);
    mime->setData(QString::fromLatin1(SearchModeMime), QByteArrayLiteral("hex"));
    QGuiApplication::clipboard()->setMimeData(mime);
}

inline bool rawClipboardBytes(QByteArray *bytes)
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasFormat(QString::fromLatin1(RawBytesMime)))
        return false;
    const QByteArray raw = mime->data(QString::fromLatin1(RawBytesMime));
    if (raw.isEmpty())
        return false;
    if (bytes)
        *bytes = raw;
    return true;
}

inline void setHexSelection(const QByteArray &bytes)
{
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(RawBytesMime), bytes);
    mime->setData(QString::fromLatin1(SearchModeMime), QByteArrayLiteral("hex"));
    mime->setText(hexText(bytes));
    QGuiApplication::clipboard()->setMimeData(mime);
}

inline bool clipboardHexSearch(QByteArray *bytes, QString *visibleText = nullptr)
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasFormat(QString::fromLatin1(RawBytesMime))
        || mime->data(QString::fromLatin1(SearchModeMime))
               != QByteArrayLiteral("hex")) {
        return false;
    }
    const QByteArray raw = mime->data(QString::fromLatin1(RawBytesMime));
    if (raw.isEmpty())
        return false;
    if (bytes)
        *bytes = raw;
    if (visibleText)
        *visibleText = hexText(raw);
    return true;
}

} // namespace ByteClipboard
