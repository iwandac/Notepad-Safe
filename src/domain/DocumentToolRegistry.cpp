#include "DocumentToolRegistry.h"

bool DocumentToolRegistry::registerTool(DocumentToolDescriptor descriptor,
                                        Factory factory)
{
    if (descriptor.id.isEmpty() || !factory || entries_.contains(descriptor.id))
        return false;
    const QString id = descriptor.id;
    entries_.insert(id, Entry{std::move(descriptor), std::move(factory)});
    return true;
}

QList<DocumentToolDescriptor> DocumentToolRegistry::descriptors() const
{
    QList<DocumentToolDescriptor> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_)
        result.push_back(entry.descriptor);
    return result;
}

std::unique_ptr<IDocumentTool> DocumentToolRegistry::create(const QString &id) const
{
    const auto it = entries_.constFind(id);
    if (it == entries_.cend())
        return {};
    ++instantiatedCount_;
    return it->factory();
}

DocumentToolResult DocumentToolRegistry::run(
    const QString &id, const DocumentSnapshot &snapshot,
    const DocumentToolContext &context) const
{
    const auto it = entries_.constFind(id);
    if (it == entries_.cend()) {
        DocumentToolResult result;
        result.status = DocumentToolResult::Status::Failed;
        result.error = QStringLiteral("Unknown document tool: %1").arg(id);
        return result;
    }
    if (!documentToolInvocationAllowed(snapshot, it->descriptor, context))
        return {DocumentToolResult::Status::Denied};
    if (context.cancellation && context.cancellation->isCancelled())
        return {DocumentToolResult::Status::Cancelled};

    std::unique_ptr<IDocumentTool> tool = create(id);
    if (!tool) {
        DocumentToolResult result;
        result.status = DocumentToolResult::Status::Failed;
        result.error = QStringLiteral("Document tool factory failed: %1").arg(id);
        return result;
    }
    DocumentToolResult result = tool->run(snapshot, context);
    if (result.status != DocumentToolResult::Status::Completed)
        return result;
    if (!result.artifact.documentId.isNull()
        && (result.artifact.documentId != snapshot.documentId
            || result.artifact.sourceRevision != snapshot.revision)) {
        result.status = DocumentToolResult::Status::Failed;
        result.error = QStringLiteral("Document tool returned an artifact for a different snapshot");
        result.editProposal.reset();
    }
    if (result.editProposal
        && (result.editProposal->documentId != snapshot.documentId
            || result.editProposal->baseRevision != snapshot.revision)) {
        result.status = DocumentToolResult::Status::Failed;
        result.error = QStringLiteral("Document tool returned a stale edit proposal");
        result.editProposal.reset();
    }
    return result;
}
