#include "AppComposition.h"
#include "EditorTab.h"
#include "FeatureRegistry.h"
#include "MainWindow.h"
#include "StartupCoordinator.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

namespace {

class NoopTool final : public IDocumentTool {
public:
    explicit NoopTool(DocumentToolDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    DocumentToolDescriptor descriptor() const override { return descriptor_; }
    DocumentToolResult run(const DocumentSnapshot &, const DocumentToolContext &) override
    {
        return {DocumentToolResult::Status::Completed};
    }
private:
    DocumentToolDescriptor descriptor_;
};

class NoopProvider final : public IDocumentProvider {
public:
    explicit NoopProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    DocumentProviderDescriptor descriptor() const override { return descriptor_; }
    bool open(IDocumentSurface *, const DocumentSurfaceOpenRequest &,
              QString *) override { return false; }
private:
    DocumentProviderDescriptor descriptor_;
};

class NoopInferenceProvider final : public IInferenceProvider {
public:
    explicit NoopInferenceProvider(InferenceProviderManifest manifest)
        : manifest_(std::move(manifest)) {}
    InferenceProviderManifest manifest() const override { return manifest_; }
    InferenceResponse infer(const InferenceRequest &) override
    {
        return {true, false, QStringLiteral("unused"), {}};
    }
private:
    InferenceProviderManifest manifest_;
};

class NoopPersistenceAdapter final : public IPersistenceAdapter {
public:
    explicit NoopPersistenceAdapter(PersistenceAdapterDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    PersistenceAdapterDescriptor descriptor() const override { return descriptor_; }
    bool recognizes(const QString &) const override { return false; }
    PersistenceResult save(IDocumentContentPort *,
                           const PersistenceSaveRequest &) override
    {
        return {PersistenceStatus::Unsupported, {}, {}};
    }
private:
    PersistenceAdapterDescriptor descriptor_;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir dataDir;
    if (!dataDir.isValid())
        return 2;
    qputenv("NOTEPADSAFE_DATADIR", dataDir.path().toUtf8());

    AppComposition composition;
    for (int i = 0; i < 100; ++i) {
        const DocumentToolDescriptor descriptor{
            QStringLiteral("startup.tool.%1").arg(i), QStringLiteral("Tool"),
            ToolExposurePolicy::LocalOnly, false};
        if (!composition.documentTools().registerTool(descriptor, [descriptor] {
                return std::make_unique<NoopTool>(descriptor);
            }))
            return 3;
    }
    for (int i = 0; i < 20; ++i) {
        const DocumentProviderDescriptor descriptor{
            QStringLiteral("startup.provider.%1").arg(i), QStringLiteral("Provider"),
            DocumentContentKind::Text, false};
        if (!composition.documentProviders().registerProvider(descriptor, [descriptor] {
                return std::make_unique<NoopProvider>(descriptor);
            }))
            return 4;
        const InferenceProviderManifest manifest{
            QStringLiteral("startup.inference.%1").arg(i), QStringLiteral("1"),
            QStringLiteral("test"), QStringLiteral("test-only"), {},
            QStringLiteral("none"), QStringLiteral("none"), false};
        if (!composition.inferenceProviders().registerProvider(manifest, [manifest] {
                return std::make_unique<NoopInferenceProvider>(manifest);
            }))
            return 10;
        const PersistenceAdapterDescriptor persistenceDescriptor{
            QStringLiteral("startup.persistence.%1").arg(i),
            QStringLiteral("Persistence"), -100 - i,
            DocumentPersistence::PlainFile};
        if (!composition.persistenceAdapters().registerAdapter(
                persistenceDescriptor, [persistenceDescriptor] {
                    return std::make_unique<NoopPersistenceAdapter>(
                        persistenceDescriptor);
                }))
            return 11;
    }

    QElapsedTimer timer;
    timer.start();
    MainWindow window(nullptr, &composition);
    if (composition.heavyFactoryCalls() != 0
        || composition.documentTools().instantiatedCount() != 0
        || composition.documentProviders().instantiatedCount() != 0
        || composition.persistenceAdapters().instantiatedCount() != 0
        || composition.inferenceProviders().instantiatedCount() != 0)
        return 5;

    bool painted = false;
    bool idleTurnCompleted = false;
    qint64 firstIdleTurnMs = -1;
    QElapsedTimer postPaintTimer;
    StartupCoordinator startup;
    startup.observe(&window, {});
    QObject::connect(&window, &MainWindow::firstPaintCompleted,
                     [&] {
                         painted = true;
                         postPaintTimer.start();
                     });
    QObject::connect(&startup, &StartupCoordinator::postPaintIdleTurnCompleted,
                     [&] {
                         firstIdleTurnMs = postPaintTimer.elapsed();
                         idleTurnCompleted = true;
                     });
    window.show();
    startup.releasePostPaintWork();
    if (!QTest::qWaitFor([&painted] { return painted; }, 1500))
        return 6;
    const qint64 firstPaintMs = timer.elapsed();
    if (!QTest::qWaitFor([&idleTurnCompleted] { return idleTurnCompleted; }, 250)
        || firstIdleTurnMs < 0 || firstIdleTurnMs >= 250) {
        return 18;
    }

    if (composition.heavyFactoryCalls() != 0
        || composition.documentTools().instantiatedCount() != 0
        || composition.documentProviders().instantiatedCount() != 0
        || composition.persistenceAdapters().instantiatedCount() != 0
        || composition.inferenceProviders().instantiatedCount() != 0)
        return 7;
    const auto actions = window.findChildren<QAction *>();
    QSet<QString> actualCommandIds;
    for (QAction *action : actions) {
        if (!action->objectName().isEmpty()
            && action->property("featureId").toString().isEmpty()) {
            qCritical() << "Command missing FeatureId:" << action->objectName();
            return 9;
        }
        if (!action->objectName().isEmpty())
            actualCommandIds.insert(action->objectName());
    }
    for (const QString &required : FeatureRegistry::requiredStartupCommandIds()) {
        if (!actualCommandIds.contains(required)) {
            qCritical() << "Required startup command disappeared:" << required;
            return 15;
        }
    }
    if (firstPaintMs >= 1500)
        return 8;

    DocumentSnapshot snapshot;
    snapshot.documentId = QUuid::createUuid();
    snapshot.revision = 1;
    snapshot.capabilities = DocumentCapability::Read
        | DocumentCapability::AnalyzeLocally;
    snapshot.text = QStringLiteral("post-paint lazy tool input");
    snapshot.sourceRange = {{0}, {snapshot.text.toUtf8().size()}};
    if (composition.runDocumentTool(QStringLiteral("startup.tool.0"), snapshot).status
            != DocumentToolResult::Status::Completed
        || composition.documentTools().instantiatedCount() != 1) {
        return 12;
    }

    // The provider registry is an executable production route, not descriptor
    // scaffolding: after first paint a selected provider must open the document
    // through the same EditorTab implementation used by MainWindow.
    const QString providerFixture = dataDir.filePath(QStringLiteral("provider.txt"));
    QFile fixture(providerFixture);
    if (!fixture.open(QIODevice::WriteOnly)
        || fixture.write("provider-route\n") != 15
        || !fixture.flush()) {
        return 13;
    }
    fixture.close();
    EditorTab providerTab;
    QString providerError;
    const DocumentSurfaceOpenRequest request{
        providerFixture, DocumentContentKind::Text, false,
        DocumentPresentationKind::Text, std::nullopt};
    if (!composition.documentProviders().openBest(
            &providerTab, request, &providerError)
        || providerTab.filePath() != providerFixture
        || composition.documentProviders().instantiatedCount() != 1) {
        qCritical() << "Production document provider route failed:" << providerError;
        return 14;
    }
    providerTab.setModified(true);
    if (!providerTab.documentState().dirty)
        return 16;
    providerTab.setModified(false);
    if (providerTab.documentState().dirty)
        return 17;

    qInfo("Startup composition: first paint %lld ms, first idle turn %lld ms, heavy factories 0 before paint; lazy tool instantiated after paint",
          static_cast<long long>(firstPaintMs),
          static_cast<long long>(firstIdleTurnMs));
    return 0;
}
