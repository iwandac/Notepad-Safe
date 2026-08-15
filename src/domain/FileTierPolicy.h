#pragma once

#include <QtGlobal>

enum class FileTier {
    Invalid,
    StandardEdit,
    LargeEditPreview,
    HugeReadOnly
};

struct FileTierDecision {
    FileTier tier = FileTier::Invalid;
    qint64 sourceBytes = -1;
    qint64 effectiveBytes = -1;

    bool isValid() const { return tier != FileTier::Invalid; }
    bool usesMappedPreview() const {
        return tier == FileTier::LargeEditPreview || tier == FileTier::HugeReadOnly;
    }
    bool mayPromoteToFullEdit() const { return tier == FileTier::LargeEditPreview; }
    bool isReadOnlyByPolicy() const { return tier == FileTier::HugeReadOnly; }
};

// Stable product boundaries. Hardware-dependent decisions belong to
// ResourceAdmission and must never silently move these size thresholds.
class FileTierPolicy final {
public:
    static constexpr qint64 StandardEditLimitBytes = 32LL * 1024 * 1024;
    static constexpr qint64 LargeEditLimitBytes = 2LL * 1024 * 1024 * 1024;

    // estimatedUtf8Bytes is optional. When decoding/transcoding expands the
    // document, classification uses the larger of the source and estimate.
    static FileTierDecision classify(qint64 sourceBytes,
                                     qint64 estimatedUtf8Bytes = -1);
};

enum class ResourceAdmissionReason {
    Admitted,
    NotLargeEditTier,
    InvalidMemorySnapshot,
    PhysicalMemoryBudgetExceeded,
    CommitReserveInsufficient
};

struct ResourceAdmissionInput {
    FileTierDecision file;
    qint64 totalPhysicalBytes = 0;
    qint64 availableCommitBytes = 0;
};

struct ResourceAdmissionDecision {
    bool admitted = false;
    ResourceAdmissionReason reason = ResourceAdmissionReason::NotLargeEditTier;
    qint64 estimatedPeakBytes = 0;
    qint64 physicalBudgetBytes = 0;
    qint64 requiredCommitBytes = 0;
};

class ResourceAdmission final {
public:
    static constexpr qint64 CommitReserveBytes = 2LL * 1024 * 1024 * 1024;

    // Deterministic, conservative estimate for promoting an S2 document from
    // mmap preview into a complete Scintilla document. This does not allocate.
    static ResourceAdmissionDecision evaluate(const ResourceAdmissionInput &input);
};
