#include "FileContentProbe.h"

#include <QCoreApplication>
#include <QDir>

#include <cstdio>

namespace {

struct TestfileCase {
    const char *name;
    qint64 bytes;
    DocumentContentKind kind;
    FileTier tier;
    TextEncoding encoding;
    bool checkEncoding;
    bool hasBom;
};

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        std::fprintf(stderr, "PASS: %s\n", qPrintable(message));
        return;
    }
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", qPrintable(message));
}

QString kindName(DocumentContentKind kind)
{
    return kind == DocumentContentKind::Binary
        ? QStringLiteral("Binary") : QStringLiteral("Text");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QDir root(QStringLiteral(NOTEPADSAFE_TESTFILES_DIR));
    const TestfileCase cases[] = {
        {"tinytextfile.txt", 11343230, DocumentContentKind::Text,
         FileTier::StandardEdit, TextEncoding::GBK, true, false},
        {"tinytextfile-utf-16be.txt", 12075264, DocumentContentKind::Text,
         FileTier::StandardEdit, TextEncoding::Utf16BE, true, true},
        {"tinytextfile-utf-16be-utf-8-bom.txt", 16648632,
         DocumentContentKind::Text, FileTier::StandardEdit,
         TextEncoding::Utf8Bom, true, true},
        {"tinytestfile", 32670740, DocumentContentKind::Binary,
         FileTier::StandardEdit, TextEncoding::Utf8, false, false},
        {"smalltextfile.txt", 40980710, DocumentContentKind::Text,
         FileTier::LargeReadOnly, TextEncoding::GBK, true, false},
        {"smalltextfile-utf-8-utf-16le.txt", 42092170,
         DocumentContentKind::Text, FileTier::LargeReadOnly,
         TextEncoding::Utf16LE, true, true},
        {"smalltextfile-utf-8.txt", 60915142, DocumentContentKind::Text,
         FileTier::LargeReadOnly, TextEncoding::Utf8, true, false},
        {"smalltestfile", 108385159, DocumentContentKind::Binary,
         FileTier::LargeReadOnly, TextEncoding::Utf8, false, false},
    };

    for (const TestfileCase &test : cases) {
        const QString path = root.filePath(QString::fromLatin1(test.name));
        const FileContentProbeResult probe = probeFileContent(path);
        const QString prefix = QString::fromLatin1(test.name) + QStringLiteral(": ");
        check(probe.exists && probe.readable, prefix + QStringLiteral("sample is readable"));
        check(probe.sourceBytes == test.bytes,
              prefix + QStringLiteral("byte size is the frozen R0 asset"));
        check(probe.tier.tier == test.tier,
              prefix + QStringLiteral("size maps to the expected product tier"));
        check(probe.contentKind == test.kind,
              prefix + QStringLiteral("bounded probe routes as ") + kindName(test.kind));
        if (test.checkEncoding) {
            check(probe.encoding.encoding == test.encoding,
                  prefix + QStringLiteral("encoding is ") + encodingName(test.encoding));
            check(probe.encoding.hasBom == test.hasBom,
                  prefix + QStringLiteral("BOM contract matches"));
        }
    }

    std::fprintf(stderr, "testfiles matrix: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
