#pragma once

#include <QtGlobal>

enum class FileTier {
    Invalid,
    StandardEdit,
    // 32 MiB..2 GiB is intentionally a read-only mapped analysis tier.
    // It currently has exactly the same editing capability as HugeReadOnly;
    // the separate tier only preserves the product size boundary.
    LargeReadOnly,
    HugeReadOnly
};

struct FileTierDecision {
    FileTier tier = FileTier::Invalid;
    qint64 sourceBytes = -1;
    qint64 effectiveBytes = -1;

    bool isValid() const { return tier != FileTier::Invalid; }
    bool usesMappedPreview() const {
        return tier == FileTier::LargeReadOnly || tier == FileTier::HugeReadOnly;
    }
    bool isReadOnlyByPolicy() const { return usesMappedPreview(); }
};

// Stable product boundaries. Hardware-dependent state must never silently move
// these thresholds or make a mapped tier editable.
class FileTierPolicy final {
public:
    static constexpr qint64 StandardEditLimitBytes = 32LL * 1024 * 1024;
    static constexpr qint64 LargeEditLimitBytes = 2LL * 1024 * 1024 * 1024;
    // Do not infer an editable S2 path from the 2 GiB classification boundary.
    // The former background ILoader promotion proved unusable and is disabled.
    static constexpr bool LargeFileEditingEnabled = false;

    // estimatedUtf8Bytes is optional. When decoding/transcoding expands the
    // document, classification uses the larger of the source and estimate.
    static FileTierDecision classify(qint64 sourceBytes,
                                     qint64 estimatedUtf8Bytes = -1);
};
