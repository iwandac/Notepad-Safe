#pragma once

#include "DocumentState.h"

#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

enum class ToolExposurePolicy {
    LocalOnly,
    RemoteRequiresPerInvocationConsent,
};

struct DocumentToolDescriptor {
    QString id;
    QString displayName;
    ToolExposurePolicy exposure = ToolExposurePolicy::LocalOnly;
    bool producesEditProposal = false;
};

class ToolCancellation final {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> cancelled_{false};
};

struct EditProposal {
    QUuid documentId;
    quint64 baseRevision = 0;
    QString replacementText;

    bool appliesTo(const DocumentState &state) const noexcept
    {
        return state.id == documentId && state.revision == baseRevision
            && state.capabilities.testFlag(DocumentCapability::Edit);
    }
};

struct AnalysisArtifact {
    QUuid documentId;
    quint64 sourceRevision = 0;
    QString kind;
    QString text;
    QJsonObject payload;

    bool isStaleFor(const DocumentState &state) const noexcept
    {
        return state.id != documentId || state.revision != sourceRevision;
    }
};

struct DocumentToolResult {
    enum class Status { Completed, Cancelled, Denied, Failed };
    Status status = Status::Failed;
    QString error;
    AnalysisArtifact artifact;
    std::optional<EditProposal> editProposal;
};

struct DocumentToolContext {
    ToolCancellation *cancellation = nullptr;
    std::function<void(int)> progress;
    bool remoteExposureApproved = false;
};

class IDocumentTool {
public:
    virtual ~IDocumentTool() = default;
    virtual DocumentToolDescriptor descriptor() const = 0;
    virtual DocumentToolResult run(const DocumentSnapshot &snapshot,
                                   const DocumentToolContext &context) = 0;
};

inline bool documentToolInvocationAllowed(const DocumentSnapshot &snapshot,
                                          const DocumentToolDescriptor &tool,
                                          const DocumentToolContext &context)
{
    if (!snapshot.mayEnterDocumentTools())
        return false;
    return tool.exposure != ToolExposurePolicy::RemoteRequiresPerInvocationConsent
        || context.remoteExposureApproved;
}
