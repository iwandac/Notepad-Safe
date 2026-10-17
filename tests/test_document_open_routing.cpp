// Session restore routing: which provider request each stored presentation
// produces. Core-only: providers record the request instead of opening widgets.
#include "DocumentOpenWorkflow.h"
#include "DocumentProviderRegistry.h"
#include "DocumentSurface.h"
#include "SessionDocumentPort.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <memory>
#include <optional>

namespace {
std::optional<DocumentSurfaceOpenRequest> g_lastRequest;

class RecordingProvider final : public IDocumentProvider {
public:
    explicit RecordingProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    DocumentProviderDescriptor descriptor() const override { return descriptor_; }
    bool open(IDocumentSurface *, const DocumentSurfaceOpenRequest &request,
              QString *) override
    {
        g_lastRequest = request;
        return true;
    }
private:
    DocumentProviderDescriptor descriptor_;
};

class NullSurface final : public IDocumentSurface {
public:
    bool openSurface(const DocumentSurfaceOpenRequest &, QString *) override { return true; }
};

void registerAllShapes(DocumentProviderRegistry &providers)
{
    int n = 0;
    for (auto kind : {DocumentContentKind::Text, DocumentContentKind::Binary}) {
        for (bool windowed : {false, true}) {
            for (auto presentation : {DocumentPresentationKind::Text,
                                      DocumentPresentationKind::Hex}) {
                const DocumentProviderDescriptor descriptor{
                    QStringLiteral("record.%1").arg(n++), QStringLiteral("Record"),
                    kind, windowed, presentation, 0};
                providers.registerProvider(descriptor, [descriptor] {
                    return std::make_unique<RecordingProvider>(descriptor);
                });
            }
        }
    }
}

// Content probing reads only the first 64 KiB, so a short prefix followed by
// QFile::resize zero padding gives a large file of the wanted kind quickly.
QString makeFile(const QTemporaryDir &dir, const QString &name,
                 const QByteArray &prefix, qint64 size)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(prefix);
    file.close();
    if (size > prefix.size()) QFile::resize(path, size);
    return path;
}

QByteArray textPrefix()
{
    QByteArray text;
    while (text.size() < 70 * 1024)
        text += "2026-10-17 INFO request served\n";
    return text;
}

constexpr qint64 Mib40 = 40LL * 1024 * 1024;
} // namespace

class TestDocumentOpenRouting final : public QObject {
    Q_OBJECT
private slots:
    void init() { g_lastRequest.reset(); }
    void binaryTextSessionRestoresAsHex();
    void smallBinaryTextSessionRestoresAsHex();
    void windowedTextHexSessionKeepsTextContent();
    void smallTextHexSessionOpensAsText();
};

void TestDocumentOpenRouting::binaryTextSessionRestoresAsHex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionTab state;
    state.filePath = makeFile(dir, QStringLiteral("big.bin"), QByteArray(16, '\0'), Mib40);
    state.presentation = QStringLiteral("text");
    DocumentProviderRegistry providers;
    registerAllShapes(providers);
    NullSurface surface;
    QVERIFY(openSessionDocument(providers, &surface, state));
    QVERIFY(g_lastRequest.has_value());
    QCOMPARE(g_lastRequest->contentKind, DocumentContentKind::Binary);
    QCOMPARE(g_lastRequest->presentation, DocumentPresentationKind::Hex);
}

void TestDocumentOpenRouting::smallBinaryTextSessionRestoresAsHex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionTab state;
    state.filePath = makeFile(dir, QStringLiteral("small.bin"), QByteArray::fromHex("00414243"), 4);
    state.presentation = QStringLiteral("text");
    DocumentProviderRegistry providers;
    registerAllShapes(providers);
    NullSurface surface;
    QVERIFY(openSessionDocument(providers, &surface, state));
    QVERIFY(g_lastRequest.has_value());
    QCOMPARE(g_lastRequest->contentKind, DocumentContentKind::Binary);
    QCOMPARE(g_lastRequest->presentation, DocumentPresentationKind::Hex);
}

void TestDocumentOpenRouting::windowedTextHexSessionKeepsTextContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionTab state;
    state.filePath = makeFile(dir, QStringLiteral("big.log"), textPrefix(), Mib40);
    state.presentation = QStringLiteral("hex");
    DocumentProviderRegistry providers;
    registerAllShapes(providers);
    NullSurface surface;
    QVERIFY(openSessionDocument(providers, &surface, state));
    QVERIFY(g_lastRequest.has_value());
    QCOMPARE(g_lastRequest->contentKind, DocumentContentKind::Text);
    QCOMPARE(g_lastRequest->presentation, DocumentPresentationKind::Hex);
    QVERIFY(g_lastRequest->windowed);
}

void TestDocumentOpenRouting::smallTextHexSessionOpensAsText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionTab state;
    state.filePath = makeFile(dir, QStringLiteral("notes.txt"), "hello\nworld\n", 12);
    state.presentation = QStringLiteral("hex");
    DocumentProviderRegistry providers;
    registerAllShapes(providers);
    NullSurface surface;
    QVERIFY(openSessionDocument(providers, &surface, state));
    QVERIFY(g_lastRequest.has_value());
    QCOMPARE(g_lastRequest->contentKind, DocumentContentKind::Text);
    QCOMPARE(g_lastRequest->presentation, DocumentPresentationKind::Text);
    QVERIFY(!g_lastRequest->windowed);
}

QTEST_GUILESS_MAIN(TestDocumentOpenRouting)
#include "test_document_open_routing.moc"
