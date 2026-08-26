#include "DocumentProviderRegistry.h"
#include "DocumentToolRegistry.h"
#include "ReadOnlySelectionModel.h"
#include "FeatureRegistry.h"
#include "InferenceProviderRegistry.h"

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
private:
    DocumentProviderDescriptor descriptor_;
};

DocumentSnapshot snapshot(DocumentSecurityClass security)
{
    DocumentSnapshot result;
    result.documentId = QUuid::createUuid();
    result.revision = 7;
    result.securityClass = security;
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

    DocumentToolRegistry tools;
    check(tools.registerTool(EchoDocumentTool().descriptor(), [] {
        return std::make_unique<EchoDocumentTool>();
    }), "register EchoDocumentTool");
    check(tools.registerTool(StaticGraphTool().descriptor(), [] {
        return std::make_unique<StaticGraphTool>();
    }), "register StaticGraphTool");
    check(tools.instantiatedCount() == 0, "tool descriptors are lazy");

    auto echo = tools.create(QStringLiteral("test.echo"));
    auto graph = tools.create(QStringLiteral("test.static-graph"));
    const auto plain = snapshot(DocumentSecurityClass::OrdinaryDocument);
    auto secure = plain;
    secure.securityClass = DocumentSecurityClass::SecureDocument;
    const auto plainResult = echo->run(plain, {});
    const auto secureResult = echo->run(secure, {});
    check(plainResult.status == DocumentToolResult::Status::Completed
              && secureResult.status == DocumentToolResult::Status::Completed
              && plainResult.artifact.text == secureResult.artifact.text,
          "plain and unlocked secure documents use the same tool contract");
    const auto accountResult = graph->run(
        snapshot(DocumentSecurityClass::AccountSecret), {});
    check(accountResult.status == DocumentToolResult::Status::Denied,
          "account secrets are denied before entering a document tool");

    ToolCancellation cancellation;
    cancellation.cancel();
    DocumentToolContext cancelledContext;
    cancelledContext.cancellation = &cancellation;
    check(echo->run(plain, cancelledContext).status
              == DocumentToolResult::Status::Cancelled,
          "tool cancellation is a stable port contract");

    DocumentState state;
    state.id = plain.documentId;
    state.revision = plain.revision;
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

    check(FeatureRegistry::featureIdForCommand(QStringLiteral("edit.copy"))
              == QLatin1String("NSP-EDIT"),
          "command id maps to a stable FeatureId");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("tools.accounts"))
              == QLatin1String("NSP-ACCOUNTS"),
          "accounts remain a distinct product feature boundary");
    check(FeatureRegistry::featureIdForCommand(QStringLiteral("unknown.command")).isEmpty(),
          "unknown commands fail closed instead of disappearing from the ledger");

    InferenceProviderRegistry inferenceProviders;
    check(inferenceProviders.manifests().isEmpty()
              && inferenceProviders.instantiatedCount() == 0,
          "inference is optional and core offline editing has no model dependency");

    if (failures == 0)
        qInfo() << "Architecture contracts: all checks passed; descriptor registration"
                << descriptorRegistrationMs << "ms";
    return failures == 0 ? 0 : 1;
}
