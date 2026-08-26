#include "AppComposition.h"
#include "MainWindow.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
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
    }

    QElapsedTimer timer;
    timer.start();
    MainWindow window(nullptr, &composition);
    if (composition.heavyFactoryCalls() != 0
        || composition.documentTools().instantiatedCount() != 0
        || composition.documentProviders().instantiatedCount() != 0
        || composition.inferenceProviders().instantiatedCount() != 0)
        return 5;

    bool painted = false;
    QObject::connect(&window, &MainWindow::firstPaintCompleted,
                     [&painted] { painted = true; });
    window.show();
    if (!QTest::qWaitFor([&painted] { return painted; }, 1500))
        return 6;
    const qint64 firstPaintMs = timer.elapsed();

    if (composition.heavyFactoryCalls() != 0
        || composition.documentTools().instantiatedCount() != 0
        || composition.documentProviders().instantiatedCount() != 0
        || composition.inferenceProviders().instantiatedCount() != 0)
        return 7;
    const auto actions = window.findChildren<QAction *>();
    for (QAction *action : actions) {
        if (!action->objectName().isEmpty()
            && action->property("featureId").toString().isEmpty()) {
            qCritical() << "Command missing FeatureId:" << action->objectName();
            return 9;
        }
    }
    if (firstPaintMs >= 1500)
        return 8;

    qInfo("Startup composition: first paint %lld ms, heavy factories 0, tools 100, document providers 24, inference providers 20",
          static_cast<long long>(firstPaintMs));
    return 0;
}
