#include "FileTierPolicy.h"

#include <algorithm>
#include <limits>

namespace {

qint64 saturatingAdd(qint64 left, qint64 right)
{
    if (left < 0 || right < 0)
        return std::numeric_limits<qint64>::max();
    if (left > std::numeric_limits<qint64>::max() - right)
        return std::numeric_limits<qint64>::max();
    return left + right;
}

qint64 clampBytes(qint64 value, qint64 lower, qint64 upper)
{
    return std::max(lower, std::min(value, upper));
}

qint64 estimatePromotionPeak(qint64 documentBytes, qint64 totalPhysicalBytes)
{
    // The production preview is a bounded random-read projection and does not
    // keep a second full source document resident. Scintilla stores the UTF-8
    // document plus gap/metadata, line/style indexes, bounded search/save
    // scratch space, and a bounded initial undo allowance.
    const qint64 previewAndDecode = 32LL * 1024 * 1024;
    const qint64 scintillaDocument = saturatingAdd(documentBytes, documentBytes / 3);
    const qint64 lineAndStyle = saturatingAdd(documentBytes / 6, 8LL * 1024 * 1024);
    const qint64 saveAndSearch = clampBytes(documentBytes / 4,
                                            64LL * 1024 * 1024,
                                            512LL * 1024 * 1024);
    const qint64 undoAllowance = std::min(512LL * 1024 * 1024,
                                         totalPhysicalBytes / 10);

    qint64 peak = saturatingAdd(previewAndDecode, scintillaDocument);
    peak = saturatingAdd(peak, lineAndStyle);
    peak = saturatingAdd(peak, saveAndSearch);
    return saturatingAdd(peak, undoAllowance);
}

} // namespace

FileTierDecision FileTierPolicy::classify(qint64 sourceBytes,
                                          qint64 estimatedUtf8Bytes)
{
    FileTierDecision result;
    result.sourceBytes = sourceBytes;
    if (sourceBytes < 0)
        return result;

    result.effectiveBytes = sourceBytes;
    if (estimatedUtf8Bytes >= 0)
        result.effectiveBytes = std::max(sourceBytes, estimatedUtf8Bytes);

    if (result.effectiveBytes < StandardEditLimitBytes)
        result.tier = FileTier::StandardEdit;
    else if (result.effectiveBytes <= LargeEditLimitBytes)
        result.tier = FileTier::LargeEditPreview;
    else
        result.tier = FileTier::HugeReadOnly;
    return result;
}

ResourceAdmissionDecision ResourceAdmission::evaluate(const ResourceAdmissionInput &input)
{
    ResourceAdmissionDecision result;
    if (input.file.tier != FileTier::LargeEditPreview) {
        result.reason = ResourceAdmissionReason::NotLargeEditTier;
        return result;
    }
    if (input.totalPhysicalBytes <= 0 || input.availableCommitBytes <= 0 ||
        input.file.effectiveBytes < 0) {
        result.reason = ResourceAdmissionReason::InvalidMemorySnapshot;
        return result;
    }

    result.estimatedPeakBytes = estimatePromotionPeak(input.file.effectiveBytes,
                                                      input.totalPhysicalBytes);
    // Leave at least 40% of physical RAM outside this promotion estimate.
    result.physicalBudgetBytes = (input.totalPhysicalBytes / 10) * 6;
    result.requiredCommitBytes = saturatingAdd(result.estimatedPeakBytes,
                                               CommitReserveBytes);

    if (result.estimatedPeakBytes > result.physicalBudgetBytes) {
        result.reason = ResourceAdmissionReason::PhysicalMemoryBudgetExceeded;
        return result;
    }
    if (input.availableCommitBytes < result.requiredCommitBytes) {
        result.reason = ResourceAdmissionReason::CommitReserveInsufficient;
        return result;
    }

    result.admitted = true;
    result.reason = ResourceAdmissionReason::Admitted;
    return result;
}
