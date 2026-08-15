#include "FileTierPolicy.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << "FAIL:" << message;
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    constexpr qint64 MiB = 1024LL * 1024;
    constexpr qint64 GiB = 1024LL * MiB;

    check(FileTierPolicy::classify(-1).tier == FileTier::Invalid,
          "negative byte count is invalid");
    check(FileTierPolicy::classify(0).tier == FileTier::StandardEdit,
          "empty document is S1");
    check(FileTierPolicy::classify(32 * MiB - 1).tier == FileTier::StandardEdit,
          "32 MiB - 1 is S1");
    check(FileTierPolicy::classify(32 * MiB).tier == FileTier::LargeEditPreview,
          "32 MiB is S2");
    check(FileTierPolicy::classify(2 * GiB).tier == FileTier::LargeEditPreview,
          "2 GiB is S2");
    check(FileTierPolicy::classify(2 * GiB + 1).tier == FileTier::HugeReadOnly,
          "2 GiB + 1 is S3");

    const auto expanded = FileTierPolicy::classify(24 * MiB, 40 * MiB);
    check(expanded.tier == FileTier::LargeEditPreview &&
              expanded.effectiveBytes == 40 * MiB,
          "UTF-8 expansion estimate participates in classification");

    ResourceAdmissionInput healthy;
    healthy.file = FileTierPolicy::classify(100 * MiB);
    healthy.totalPhysicalBytes = 16 * GiB;
    healthy.availableCommitBytes = 12 * GiB;
    const auto admitted = ResourceAdmission::evaluate(healthy);
    check(admitted.admitted && admitted.reason == ResourceAdmissionReason::Admitted,
          "100 MiB promotion is admitted with a healthy 16 GiB snapshot");

    ResourceAdmissionInput lowCommit = healthy;
    lowCommit.availableCommitBytes = admitted.requiredCommitBytes - 1;
    const auto commitRejected = ResourceAdmission::evaluate(lowCommit);
    check(!commitRejected.admitted &&
              commitRejected.reason == ResourceAdmissionReason::CommitReserveInsufficient,
          "commit reserve is enforced exactly");

    ResourceAdmissionInput constrained;
    constrained.file = FileTierPolicy::classify(2 * GiB);
    constrained.totalPhysicalBytes = 4 * GiB;
    constrained.availableCommitBytes = 16 * GiB;
    const auto memoryRejected = ResourceAdmission::evaluate(constrained);
    check(!memoryRejected.admitted &&
              memoryRejected.reason == ResourceAdmissionReason::PhysicalMemoryBudgetExceeded,
          "2 GiB promotion is rejected on a constrained machine");

    ResourceAdmissionInput huge = healthy;
    huge.file = FileTierPolicy::classify(2 * GiB + 1);
    const auto hugeRejected = ResourceAdmission::evaluate(huge);
    check(!hugeRejected.admitted &&
              hugeRejected.reason == ResourceAdmissionReason::NotLargeEditTier,
          "S3 can never promote to full edit");

    if (failures == 0)
        qInfo() << "FileTierPolicy: all checks passed";
    return failures == 0 ? 0 : 1;
}
