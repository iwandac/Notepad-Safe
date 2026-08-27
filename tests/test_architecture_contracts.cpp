#include "DocumentProviderRegistry.h"
#include "DocumentToolRegistry.h"
#include "ReadOnlySelectionModel.h"
#include "FeatureRegistry.h"
#include "InferenceProviderRegistry.h"
#include "PersistenceAdapterRegistry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>

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
    bool open(EditorTab *, const QString &, std::optional<TextEncoding>,
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
private:
    PersistenceAdapterDescriptor descriptor_;
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

    check(FeatureRegistry::featureIdForCommand(QStringLiteral("edit.copy"))
              == QLatin1String("NSP-EDIT"),
          "command id maps to a stable FeatureId");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("tools.accounts"))
              == QLatin1String("NSP-ACCOUNTS"),
          "accounts remain a distinct product feature boundary");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("unknown.command")).isEmpty(),
          "unknown commands fail closed instead of disappearing from the ledger");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("file.svae")).isEmpty()
              && FeatureRegistry::featureIdForCommand(
                     QStringLiteral("edit.indentation.tabWidth.3")).isEmpty()
              && FeatureRegistry::featureIdForCommand(
                     QStringLiteral("language.unregistered")).isEmpty(),
          "known command namespaces reject typo and unregistered suffixes");

    InferenceProviderRegistry inferenceProviders;
    check(inferenceProviders.manifests().isEmpty()
              && inferenceProviders.instantiatedCount() == 0,
          "inference is optional and core offline editing has no model dependency");

    if (failures == 0)
        qInfo() << "Architecture contracts: all checks passed; descriptor registration"
                << descriptorRegistrationMs << "ms";
    return failures == 0 ? 0 : 1;
}
