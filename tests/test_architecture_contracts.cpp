#include "DocumentProviderRegistry.h"
#include "DocumentSession.h"
#include "DocumentContentPort.h"
#include "DocumentToolRegistry.h"
#include "ReadOnlySelectionModel.h"
#include "SearchSession.h"
#include "FeatureRegistry.h"
#include "InferenceProviderRegistry.h"
#include "PersistenceAdapterRegistry.h"
#include "PlainFilePersistence.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << "FAIL:" << message;
        ++failures;
    }
}

class EchoDocumentTool final : public IDocumentTool {
public:
    DocumentToolDescriptor descriptor() const override
    {
        return {QStringLiteral("test.echo"), QStringLiteral("Echo"),
                ToolExposurePolicy::LocalOnly, false};
    }

    DocumentToolResult run(const DocumentSnapshot &snapshot,
                           const DocumentToolContext &context) override
    {
        if (!documentToolInvocationAllowed(snapshot, descriptor(), context))
            return {DocumentToolResult::Status::Denied};
        if (context.cancellation && context.cancellation->isCancelled())
            return {DocumentToolResult::Status::Cancelled};
        if (context.progress)
            context.progress(100);
        DocumentToolResult result;
        result.status = DocumentToolResult::Status::Completed;
        result.artifact = {snapshot.documentId, snapshot.revision,
                           QStringLiteral("text"), snapshot.text, {}};
        return result;
    }
};

class StaticGraphTool final : public IDocumentTool {
public:
    DocumentToolDescriptor descriptor() const override
    {
        return {QStringLiteral("test.static-graph"), QStringLiteral("Static graph"),
                ToolExposurePolicy::LocalOnly, false};
    }

    DocumentToolResult run(const DocumentSnapshot &snapshot,
                           const DocumentToolContext &context) override
    {
        if (!documentToolInvocationAllowed(snapshot, descriptor(), context))
            return {DocumentToolResult::Status::Denied};
        DocumentToolResult result;
        result.status = DocumentToolResult::Status::Completed;
        result.artifact = {snapshot.documentId, snapshot.revision,
                           QStringLiteral("graph"), {},
                           {{QStringLiteral("nodes"), 2},
                            {QStringLiteral("edges"), 1}}};
        return result;
    }
};

class CountingProvider final : public IDocumentProvider {
public:
    explicit CountingProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    DocumentProviderDescriptor descriptor() const override { return descriptor_; }
    bool open(IDocumentSurface *, const DocumentSurfaceOpenRequest &,
              QString *) override { return false; }
private:
    DocumentProviderDescriptor descriptor_;
};

class CountingPersistenceAdapter final : public IPersistenceAdapter {
public:
    explicit CountingPersistenceAdapter(PersistenceAdapterDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    PersistenceAdapterDescriptor descriptor() const override { return descriptor_; }
    bool recognizes(const QString &path) const override
    {
        return path.contains(descriptor_.id);
    }
    PersistenceResult save(IDocumentContentPort *,
                           const PersistenceSaveRequest &) override
    {
        return {PersistenceStatus::Success, {}, {}};
    }
private:
    PersistenceAdapterDescriptor descriptor_;
};

class MemoryContentPort final : public IDocumentContentPort {
public:
    DocumentState persistenceState() const noexcept override { return state; }
    TextEncoding persistenceEncoding() const noexcept override { return encoding; }
    DiskFileVersion persistenceBaseline() const override { return baseline; }
    bool createPersistenceSnapshot(qint64 maximumBytes, DocumentSnapshot *out,
                                   QString *error) override
    {
        const QByteArray encoded = text.toUtf8();
        if (!out || encoded.size() > maximumBytes) {
            if (error) *error = QStringLiteral("snapshot refused");
            return false;
        }
        *out = {};
        out->documentId = state.id;
        out->revision = state.revision;
        out->contentKind = DocumentContentKind::Text;
        out->securityClass = state.securityClass;
        out->capabilities = state.capabilities;
        out->text = text;
        out->sourceRange = {{0}, {encoded.size()}};
        ++snapshotCount;
        return true;
    }
    bool suspendPersistenceBacking(QString *) override { return true; }
    bool resumePersistenceBacking(const QString &, QString *) override
    {
        return true;
    }
    void acceptPersistenceCommit(const QString &path,
                                 DiskFileVersion version) override
    {
        state.location = path;
        state.persistence = DocumentPersistence::PlainFile;
        baseline = std::move(version);
        ++plainCommitCount;
    }
    void acceptSecurePersistenceCommit(const QUuid &, const QString &,
                                       const QString &, const QString &) override
    {
        ++secureCommitCount;
    }

    DocumentState state;
    TextEncoding encoding = TextEncoding::Utf8;
    DiskFileVersion baseline;
    QString text;
    int snapshotCount = 0;
    int plainCommitCount = 0;
    int secureCommitCount = 0;
};

DocumentSnapshot snapshot(DocumentSecurityClass security)
{
    DocumentSnapshot result;
    result.documentId = QUuid::createUuid();
    result.revision = 7;
    result.securityClass = security;
    result.capabilities = DocumentCapability::Read
        | DocumentCapability::AnalyzeLocally
        | DocumentCapability::AnalyzeRemotelyWithConsent;
    result.text = QStringLiteral("const answer = 42;");
    result.sourceRange = {{0}, {result.text.toUtf8().size()}};
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    ReadOnlySelectionModel selection;
    selection.select({{2}, {5}});
    const auto firstGeneration = selection.generation();
    selection.select({{10}, {14}});
    check(selection.generation() == firstGeneration + 1,
          "second selection supersedes the first owner generation");
    check(selection.selection() && selection.selection()->start.value == 10,
          "repeated copy observes the second selection, not a stale first range");
    selection.select({{9}, {3}});
    check(!selection.selection(), "invalid owner transition clears selection");
    selection.selectEndpoints({18}, {12});
    check(selection.selection()
              && selection.selection()->start.value == 12
              && selection.selection()->end.value == 18
              && selection.anchor().value == 18
              && selection.caret().value == 12,
          "reverse selection preserves direction and exposes a normalized range");

    SearchSession searchSession;
    SearchQuery firstQuery{QStringLiteral("first"),
                           SearchScope::AllOpenDocuments,
                           false, true, false, false};
    const int firstSearchGeneration = searchSession.begin(std::move(firstQuery));
    searchSession.scheduleTarget();
    searchSession.scheduleTarget();
    searchSession.settleTarget();
    searchSession.requestCancel();
    check(searchSession.pendingTargets() == 1
              && searchSession.cancelRequested(),
          "search session owns target count and cancellation together");
    SearchQuery secondQuery{QStringLiteral("second"),
                            SearchScope::AllOpenDocuments,
                            true, false, false, false};
    const int secondSearchGeneration = searchSession.begin(std::move(secondQuery));
    check(secondSearchGeneration != firstSearchGeneration
              && !searchSession.isCurrent(firstSearchGeneration)
              && searchSession.pendingTargets() == 0
              && !searchSession.cancelRequested()
              && searchSession.query().pattern == QLatin1String("second"),
          "a new search atomically invalidates all prior generation state");
    searchSession.settleTarget();
    check(searchSession.pendingTargets() == 0,
          "late or duplicate target completion cannot underflow session state");

    DocumentToolRegistry tools;
    check(tools.registerTool(EchoDocumentTool().descriptor(), [] {
        return std::make_unique<EchoDocumentTool>();
    }), "register EchoDocumentTool");
    check(tools.registerTool(StaticGraphTool().descriptor(), [] {
        return std::make_unique<StaticGraphTool>();
    }), "register StaticGraphTool");
    check(tools.instantiatedCount() == 0, "tool descriptors are lazy");

    const auto plain = snapshot(DocumentSecurityClass::OrdinaryDocument);
    auto secure = plain;
    secure.securityClass = DocumentSecurityClass::SecureDocument;
    const auto plainResult = tools.run(QStringLiteral("test.echo"), plain);
    const auto secureResult = tools.run(QStringLiteral("test.echo"), secure);
    check(plainResult.status == DocumentToolResult::Status::Completed
              && secureResult.status == DocumentToolResult::Status::Completed
              && plainResult.artifact.text == secureResult.artifact.text,
          "plain and unlocked secure documents use the same tool contract");
    const auto accountResult = tools.run(
        QStringLiteral("test.static-graph"),
        snapshot(DocumentSecurityClass::AccountSecret));
    check(accountResult.status == DocumentToolResult::Status::Denied,
          "account secrets are denied before entering a document tool");
    const int instancesBeforeCapabilityDenial = tools.instantiatedCount();
    auto inaccessible = plain;
    inaccessible.capabilities = DocumentCapability::None;
    check(tools.run(QStringLiteral("test.echo"), inaccessible).status
              == DocumentToolResult::Status::Denied
              && tools.instantiatedCount() == instancesBeforeCapabilityDenial,
          "missing document capability is denied before tool instantiation");
    const DocumentToolDescriptor remoteDescriptor{
        QStringLiteral("test.remote"), QStringLiteral("Remote"),
        ToolExposurePolicy::RemoteRequiresPerInvocationConsent, false};
    DocumentToolContext remoteContext;
    check(!documentToolInvocationAllowed(plain, remoteDescriptor, remoteContext),
          "remote document exposure requires per-invocation consent");
    remoteContext.remoteExposureApproved = true;
    check(documentToolInvocationAllowed(plain, remoteDescriptor, remoteContext),
          "remote document exposure requires both capability and consent");

    ToolCancellation cancellation;
    cancellation.cancel();
    DocumentToolContext cancelledContext;
    cancelledContext.cancellation = &cancellation;
    check(tools.run(QStringLiteral("test.echo"), plain, cancelledContext).status
              == DocumentToolResult::Status::Cancelled,
          "tool cancellation is a stable port contract");

    DocumentState state;
    state.id = plain.documentId;
    state.revision = plain.revision;
    state.capabilities = DocumentCapability::Edit;
    EditProposal proposal{state.id, state.revision, QStringLiteral("replacement")};
    check(proposal.appliesTo(state), "edit proposal applies to its baseline revision");
    ++state.revision;
    check(!proposal.appliesTo(state), "stale edit proposal is rejected");
    check(plainResult.artifact.isStaleFor(state),
          "analysis artifact becomes stale on revision change");

    DocumentProviderRegistry providers;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 100; ++i) {
        const DocumentProviderDescriptor descriptor{
            QStringLiteral("test.provider.%1").arg(i), QStringLiteral("Test"),
            DocumentContentKind::Text, false};
        check(providers.registerProvider(descriptor, [descriptor] {
            return std::make_unique<CountingProvider>(descriptor);
        }), "register scale provider descriptor");
    }
    const qint64 descriptorRegistrationMs = timer.elapsed();
    check(providers.descriptors().size() == 100,
          "one hundred provider descriptors are discoverable");
    check(providers.instantiatedCount() == 0,
          "one hundred unused providers instantiate no implementation");
    check(descriptorRegistrationMs < 100,
          "descriptor-only extension registration stays inside startup budget");

    PersistenceAdapterRegistry persistenceAdapters;
    for (int i = 0; i < 20; ++i) {
        const PersistenceAdapterDescriptor descriptor{
            QStringLiteral("test.persistence.%1").arg(i), QStringLiteral("Test"),
            i, DocumentPersistence::PlainFile};
        check(persistenceAdapters.registerAdapter(descriptor, [descriptor] {
            return std::make_unique<CountingPersistenceAdapter>(descriptor);
        }), "register scale persistence adapter descriptor");
    }
    check(persistenceAdapters.descriptors().size() == 20,
          "twenty persistence adapters are discoverable without UI changes");
    check(persistenceAdapters.instantiatedCount() == 0,
          "unused persistence adapters add no startup construction");
    const auto resolvedPersistence = persistenceAdapters.resolve(
        QStringLiteral("test.persistence.19"));
    check(resolvedPersistence
              && resolvedPersistence->id == QLatin1String("test.persistence.19"),
          "highest-priority matching persistence adapter wins");
    check(persistenceAdapters.instantiatedCount() == 1,
          "resolving a high-priority adapter does not instantiate lower adapters");
    check(!persistenceAdapters.registerAdapter(
              {QStringLiteral("test.persistence.19"), QStringLiteral("Duplicate"),
               999, DocumentPersistence::PlainFile},
              [] { return std::unique_ptr<IPersistenceAdapter>{}; }),
          "duplicate persistence adapter ids fail closed");

    PersistenceAdapterRegistry routedPersistence;
    check(routedPersistence.registerRoute(
              {QStringLiteral("test.secure"), QStringLiteral("Secure"), 100,
               DocumentPersistence::SecureFile},
              [](const QString &path) {
                  return path.endsWith(QLatin1String(".safe"));
              }),
          "register descriptor-only persistence route");
    const auto secureRoute = routedPersistence.resolve(QStringLiteral("note.safe"));
    check(secureRoute
              && secureRoute->persistence == DocumentPersistence::SecureFile
              && routedPersistence.instantiatedCount() == 0,
          "format routing is available without constructing a heavy adapter");
    auto routedAdapter = std::make_unique<CountingPersistenceAdapter>(
        PersistenceAdapterDescriptor{QStringLiteral("test.secure"),
                                     QStringLiteral("Secure"), 100,
                                     DocumentPersistence::SecureFile});
    IPersistenceAdapter *routedAdapterAddress = routedAdapter.get();
    check(routedPersistence.registerAdapter(std::move(routedAdapter))
              && routedPersistence.adapter(QStringLiteral("test.secure"))
                  == routedAdapterAddress
              && routedPersistence.instantiatedCount() == 1,
          "lazy route is completed by its runtime adapter without duplicate registration");

    DocumentSession lifecycle;
    const QUuid originalId = lifecycle.state().id;
    lifecycle.bindPlainFile(QStringLiteral("plain.txt"));
    check(lifecycle.state().id == originalId
              && lifecycle.state().persistence == DocumentPersistence::PlainFile
              && lifecycle.state().securityClass
                  == DocumentSecurityClass::OrdinaryDocument,
          "plain binding is one atomic lifecycle transition");
    const QUuid secureId = QUuid::createUuid();
    lifecycle.setRecoveryPath(QStringLiteral("draft.recovery"));
    lifecycle.setSecureBackingPath(QStringLiteral("cipher.nsafe"));
    lifecycle.bindSecureFile(secureId, QStringLiteral("note.nsafe"),
                             DocumentContentKind::Text);
    lifecycle.clearRecoveryPath();
    check(lifecycle.state().id == secureId
              && lifecycle.state().persistence == DocumentPersistence::SecureFile
              && lifecycle.recoveryPath().isEmpty()
              && lifecycle.secureBackingPath() == QLatin1String("cipher.nsafe"),
          "discarding recovery never discards the secure backing identity");
    lifecycle.bindPlainFile(QStringLiteral("export.txt"), true);
    check(lifecycle.state().id != secureId
              && lifecycle.state().securityClass
                  == DocumentSecurityClass::OrdinaryDocument,
          "secure-to-plain conversion renews identity and clears classification");

    QTemporaryDir persistenceDir;
    check(persistenceDir.isValid(), "persistence contract temp directory exists");
    MemoryContentPort memory;
    memory.state.capabilities = DocumentCapability::Read
        | DocumentCapability::Edit | DocumentCapability::Save;
    memory.text = QStringLiteral("first\n");
    const QString persistencePath = persistenceDir.filePath(
        QStringLiteral("twice.txt"));
    PersistenceAdapterRegistry realPersistence;
    check(realPersistence.registerAdapter(
              std::make_unique<PlainFilePersistence>()),
          "register the production plain persistence adapter");
    PersistenceSaveRequest firstSave{persistencePath, QString(), {}, false};
    const PersistenceResult firstCommit = realPersistence.saveFor(
        DocumentPersistence::PlainFile, &memory, firstSave);
    check(firstCommit.succeeded() && memory.plainCommitCount == 1
              && memory.snapshotCount == 1,
          "first persistence operation snapshots and commits exactly once");
    memory.text = QStringLiteral("second\n");
    ++memory.state.revision;
    PersistenceSaveRequest secondSave{
        persistencePath, QString(), firstCommit.committedVersion, true};
    const PersistenceResult secondCommit = realPersistence.saveFor(
        DocumentPersistence::PlainFile, &memory, secondSave);
    QFile committedFile(persistencePath);
    check(secondCommit.succeeded() && memory.plainCommitCount == 2
              && memory.snapshotCount == 2 && committedFile.open(QIODevice::ReadOnly)
              && committedFile.readAll() == QByteArray("second\n"),
          "second persistence operation observes new revision and replaces atomically");
    committedFile.close();
    QFile externalWriter(persistencePath);
    check(externalWriter.open(QIODevice::WriteOnly | QIODevice::Truncate)
              && externalWriter.write("external\n") == 9,
          "conflict fixture changes the committed file");
    externalWriter.close();
    memory.text = QStringLiteral("must-not-overwrite\n");
    const int commitsBeforeConflict = memory.plainCommitCount;
    const PersistenceResult conflict = realPersistence.saveFor(
        DocumentPersistence::PlainFile, &memory,
        {persistencePath, QString(), secondCommit.committedVersion, true});
    QFile conflictFile(persistencePath);
    check(conflict.status == PersistenceStatus::Conflict
              && memory.plainCommitCount == commitsBeforeConflict
              && conflictFile.open(QIODevice::ReadOnly)
              && conflictFile.readAll() == QByteArray("external\n"),
          "conflict fails closed without committing state or overwriting bytes");

    check(FeatureRegistry::featureIdForCommand(QStringLiteral("edit.copy"))
              == QLatin1String("NSP-EDIT"),
          "command id maps to a stable FeatureId");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("tools.accounts"))
              == QLatin1String("NSP-ACCOUNTS"),
          "accounts remain a distinct product feature boundary");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("unknown.command")).isEmpty(),
          "unknown commands fail closed instead of disappearing from the ledger");
    for (const QString &commandId : FeatureRegistry::requiredStartupCommandIds()) {
        check(!FeatureRegistry::featureIdForCommand(commandId).isEmpty(),
              qPrintable(QStringLiteral("required command is registered: %1")
                             .arg(commandId)));
    }
    check(FeatureRegistry::isKnownFeature(QStringLiteral("NSP-LARGEFILE"))
              && FeatureRegistry::isKnownFeature(QStringLiteral("NSP-SECURE")),
          "GUI coverage labels belong to the feature ledger");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("file.svae")).isEmpty()
              && FeatureRegistry::featureIdForCommand(
                     QStringLiteral("edit.indentation.tabWidth.3")).isEmpty()
              && FeatureRegistry::featureIdForCommand(
                     QStringLiteral("language.unregistered")).isEmpty(),
          "known command namespaces reject typo and unregistered suffixes");
    for (const char *removed : {"view.split", "view.split.horizontal", "view.split.vertical",
                                "view.presentation", "view.presentation.text",
                                "view.presentation.hex", "view.presentation.toggle"}) {
        check(FeatureRegistry::featureIdForCommand(QString::fromLatin1(removed)).isEmpty(),
              qPrintable(QStringLiteral("removed command is no longer registered: %1")
                             .arg(QString::fromLatin1(removed))));
    }

    InferenceProviderRegistry inferenceProviders;
    check(inferenceProviders.manifests().isEmpty()
              && inferenceProviders.instantiatedCount() == 0,
          "inference is optional and core offline editing has no model dependency");

    if (failures == 0)
        qInfo() << "Architecture contracts: all checks passed; descriptor registration"
                << descriptorRegistrationMs << "ms";
    return failures == 0 ? 0 : 1;
}
