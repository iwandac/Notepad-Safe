#include "FileTierPolicy.h"

#include <algorithm>

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
        result.tier = FileTier::LargeReadOnly;
    else
        result.tier = FileTier::HugeReadOnly;
    return result;
}
