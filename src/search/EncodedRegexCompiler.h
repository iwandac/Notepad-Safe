#pragma once

#include "TextEncoding.h"

#include <QByteArray>
#include <QString>

// Compiles the deliberately bounded log-regex subset to an equivalent PCRE2
// 8-bit pattern over the original encoded bytes.  The generated pattern owns
// character-boundary scanning, so PCRE2 never starts a match in the middle of
// a GB18030/DBCS/UTF-16 unit.  UTF-8 subjects do not use this adapter.
class EncodedRegexCompiler
{
public:
    struct Output {
        QByteArray bytePattern;
        bool caseSensitive = true;
        QByteArray requiredLiteral;
        bool requiredLiteralCaseSensitive = true;
    };

    static bool compile(const QString &pattern, TextEncoding encoding,
                        bool caseSensitive, Output *output,
                        QString *error = nullptr);

    // Used after a zero-length match to preserve the same deterministic unit
    // boundaries as the generated scanner prefix.
    static qint64 nextUnitOffset(const uchar *subject, qint64 subjectSize,
                                 qint64 offset, TextEncoding encoding);

    static bool isUtf8SubjectEncoding(TextEncoding encoding);
};
