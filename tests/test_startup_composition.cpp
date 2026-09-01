#include "AppComposition.h"
#include "EditorTab.h"
#include "FeatureRegistry.h"
#include "MainWindow.h"
#include "StartupCoordinator.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QShowEvent>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>
#include <cstring>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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

int registerUnusedProvidersAndTools(AppComposition &composition)
{
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
    return 0;
}

qint64 qpcNow()
{
#ifdef Q_OS_WIN
    LARGE_INTEGER counter{};
    if (::QueryPerformanceCounter(&counter))
        return counter.QuadPart;
#endif
    return -1;
}

qint64 qpcFrequency()
{
#ifdef Q_OS_WIN
    LARGE_INTEGER frequency{};
    if (::QueryPerformanceFrequency(&frequency))
        return frequency.QuadPart;
#endif
    return -1;
}

qint64 qpcElapsedNanoseconds(qint64 start, qint64 end, qint64 frequency)
{
    if (start < 0 || end < start || frequency <= 0)
        return -1;
    constexpr qint64 billion = 1000000000;
    constexpr qint64 maximum = (std::numeric_limits<qint64>::max)();
    const qint64 ticks = end - start;
    const qint64 seconds = ticks / frequency;
    const qint64 remainder = ticks % frequency;
    // Divide before multiplying. Fail closed on an unrepresentable clock,
    // rather than overflowing or converting large ticks through a double.
    if (seconds > maximum / billion || remainder > maximum / billion)
        return -1;
    const qint64 whole = seconds * billion;
    const qint64 fraction = remainder * billion / frequency;
    return fraction <= maximum - whole ? whole + fraction : -1;
}

struct FactoryCounts {
    int heavy = 0;
    int tools = 0;
    int documents = 0;
    int persistence = 0;
    int inference = 0;

    bool empty() const
    {
        return heavy == 0 && tools == 0 && documents == 0
            && persistence == 0 && inference == 0;
    }

    QJsonObject json() const
    {
        return {{"heavy", heavy}, {"tools", tools}, {"documents", documents},
                {"persistence", persistence}, {"inference", inference}};
    }
};

struct StartupMeasurement {
    QString mode;
    qint64 frequency = -1;
    qint64 start = -1;
    qint64 firstPaint = -1;
    qint64 elapsedNs = -1;
    qint64 compositionReady = -1;
    qint64 registrationReady = -1;
    qint64 windowReady = -1;
    qint64 showBegin = -1;
    qint64 showReturned = -1;
    qint64 showEventEnter = -1;
    qint64 showEventReturned = -1;
    int showEventCount = 0;
    bool painted = false;
    FactoryCounts counts;
    QString result = QStringLiteral("not_started");
    int exitCode = 20;

    int writeResult() const
    {
        // Decimal strings preserve every QPC tick in JSON consumers that use
        // doubles. Unobserved values are null, never zero-valued successes.
        const auto ticks = [](qint64 value) -> QJsonValue {
            return value < 0 ? QJsonValue(QJsonValue::Null)
                             : QJsonValue(QString::number(value));
        };
        const QJsonObject record{
            {"schema", "startup-composition-measurement-v3"}, {"mode", mode},
            {"pid", QCoreApplication::applicationPid()},
            {"qpc_frequency", ticks(frequency)}, {"qpc_start", ticks(start)},
            {"qpc_first_paint", ticks(firstPaint)},
            {"first_paint_elapsed_ns", ticks(elapsedNs)},
            {"stage_qpc", QJsonObject{
                {"composition_ready", ticks(compositionReady)},
                {"registration_ready", ticks(registrationReady)},
                {"window_ready", ticks(windowReady)},
                {"show_begin", ticks(showBegin)},
                {"show_returned", ticks(showReturned)}}},
            {"show_event_qpc", QJsonObject{
                {"enter", ticks(showEventEnter)},
                {"returned", ticks(showEventReturned)}}},
            {"show_event_count", showEventCount},
            {"paint_observed", painted},
            {"counts", painted ? QJsonValue(counts.json())
                               : QJsonValue(QJsonValue::Null)},
            {"success", exitCode == 0}, {"result", result}, {"exit_code", exitCode}};
        const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (std::fwrite(line.constData(), 1, size_t(line.size()), stdout) != size_t(line.size())
            || std::fflush(stdout) != 0)
            return 26;
        return exitCode;
    }
};

class MeasuredMainWindow final : public MainWindow {
public:
    MeasuredMainWindow(AppComposition &composition, StartupMeasurement &sample)
        : MainWindow(nullptr, &composition), sample_(sample) {}

protected:
    void showEvent(QShowEvent *event) override
    {
        const bool first = ++sample_.showEventCount == 1;
        if (first)
            sample_.showEventEnter = qpcNow();
        MainWindow::showEvent(event);
        if (first)
            sample_.showEventReturned = qpcNow();
    }

private:
    StartupMeasurement &sample_;
};

void measureFirstPaint(StartupMeasurement &sample)
{
    AppComposition composition;
    sample.compositionReady = qpcNow();
    if (sample.mode == QLatin1String("stress")) {
        const int registrationResult = registerUnusedProvidersAndTools(composition);
        if (registrationResult != 0) {
            sample.result = QStringLiteral("registration_failed");
            sample.exitCode = registrationResult;
            return;
        }
    }
    sample.registrationReady = qpcNow();
    MeasuredMainWindow window(composition, sample);
    sample.windowReady = qpcNow();
    // This mode deliberately measures only the real composition root, unused
    // registration and shell paint. It does not schedule session restoration,
    // exercise a synthetic input probe, or represent the formal product EXE.
    QObject observationScope;
    QObject::connect(&window, &MainWindow::firstPaintCompleted, &observationScope,
                     [&] {
                         if (sample.painted)
                             return;
                         sample.firstPaint = qpcNow();
                         sample.counts = {composition.heavyFactoryCalls(),
                             composition.documentTools().instantiatedCount(),
                             composition.documentProviders().instantiatedCount(),
                             composition.persistenceAdapters().instantiatedCount(),
                             composition.inferenceProviders().instantiatedCount()};
                         sample.elapsedNs = qpcElapsedNanoseconds(
                             sample.start, sample.firstPaint, sample.frequency);
                         sample.painted = true;
                     }, Qt::DirectConnection);
    sample.showBegin = qpcNow();
    window.show();
    sample.showReturned = qpcNow();
    const bool observed = QTest::qWaitFor([&sample] { return sample.painted; }, 1500);
    // show() may deliver the first paint synchronously. Its return timestamp
    // must follow show_begin, but need not precede the first paint timestamp.
    if (sample.compositionReady < sample.start
        || sample.registrationReady < sample.compositionReady
        || sample.windowReady < sample.registrationReady
        || sample.showBegin < sample.windowReady
        || sample.showReturned < sample.showBegin
        || sample.showEventCount != 1
        || sample.showEventEnter < sample.showBegin
        || sample.showEventReturned < sample.showEventEnter
        || sample.showEventReturned > sample.showReturned
        || (sample.painted && (sample.firstPaint < sample.showBegin || sample.elapsedNs < 0))) {
        sample.result = QStringLiteral("clock_failed");
        sample.exitCode = 21;
    } else if (!observed) {
        sample.result = QStringLiteral("first_paint_timeout");
        sample.exitCode = 6;
    } else if (!sample.counts.empty()) {
        sample.result = QStringLiteral("factory_before_paint");
        sample.exitCode = 5;
    } else if (sample.elapsedNs >= 1500000000LL) {
        sample.result = QStringLiteral("first_paint_budget_exceeded");
        sample.exitCode = 8;
    } else {
        // This is sample validity plus the existing 1500 ms smoke ceiling,
        // not a verdict on the 30-sample 500 ms / registration-delta gates.
        sample.result = QStringLiteral("first_paint_observed");
        sample.exitCode = 0;
    }
}

int runMeasurement(int argc, char **argv, const QString &mode)
{
    StartupMeasurement sample;
    sample.mode = mode;
    sample.frequency = qpcFrequency();
    if (mode != QLatin1String("baseline") && mode != QLatin1String("stress")) {
        sample.mode = QStringLiteral("invalid");
        sample.result = QStringLiteral("invalid_mode");
        return sample.writeResult();
    }
    if (sample.frequency <= 0) {
        sample.result = QStringLiteral("qpc_unavailable");
        sample.exitCode = 21;
        return sample.writeResult();
    }
    // Prepare the identical isolated profile outside both measured intervals.
    // Each invocation is one sample, never a second QApplication in a process.
    QTemporaryDir dataDir;
    if (!dataDir.isValid()
        || !qputenv("NOTEPADSAFE_DATADIR", dataDir.path().toUtf8())) {
        sample.result = QStringLiteral("profile_setup_failed");
        sample.exitCode = 2;
        return sample.writeResult();
    }
    {
        QApplication app(argc, argv);
        sample.start = qpcNow();
        if (sample.start < 0) {
            sample.result = QStringLiteral("clock_failed");
            sample.exitCode = 21;
        } else {
            measureFirstPaint(sample);
        }
    }
    if (!dataDir.remove()) {
        sample.result = QStringLiteral("profile_cleanup_failed");
        sample.exitCode = 27;
    }
    // Publish after normal window/application/profile teardown; an external
    // owner must still require normal exit and retain crash/timeout samples.
    return sample.writeResult();
}

} // namespace

int main(int argc, char **argv)
{
    QString measurementMode;
    bool measurementRequested = false;
    bool invalidMeasurement = false;
    constexpr char measurementPrefix[] = "--measure-startup=";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], measurementPrefix, sizeof(measurementPrefix) - 1) == 0) {
            invalidMeasurement |= measurementRequested;
            measurementRequested = true;
            measurementMode = QString::fromLatin1(argv[i] + sizeof(measurementPrefix) - 1);
        } else if (std::strcmp(argv[i], "--measure-startup") == 0) {
            invalidMeasurement = measurementRequested = true;
        }
    }
    if (measurementRequested)
        return runMeasurement(argc, argv, invalidMeasurement ? QStringLiteral("invalid")
                                                           : measurementMode);

    QApplication app(argc, argv);
    QTemporaryDir dataDir;
    if (!dataDir.isValid())
        return 2;
    qputenv("NOTEPADSAFE_DATADIR", dataDir.path().toUtf8());

    AppComposition composition;
    const int registrationResult = registerUnusedProvidersAndTools(composition);
    if (registrationResult != 0)
        return registrationResult;

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
    qint64 firstPaintMs = -1;
    qint64 firstIdleTurnMs = -1;
    QElapsedTimer postPaintTimer;
    StartupCoordinator startup;
    startup.observe(&window, {});
    QObject::connect(&window, &MainWindow::firstPaintCompleted,
                     &startup,
                     [&] {
                         firstPaintMs = timer.elapsed();
                         painted = true;
                         postPaintTimer.start();
                     }, Qt::DirectConnection);
    QObject::connect(&startup, &StartupCoordinator::postPaintIdleTurnCompleted,
                     [&] {
                         firstIdleTurnMs = postPaintTimer.elapsed();
                         idleTurnCompleted = true;
                     });
    window.show();
    startup.releasePostPaintWork();
    if (!QTest::qWaitFor([&painted] { return painted; }, 1500))
        return 6;
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
