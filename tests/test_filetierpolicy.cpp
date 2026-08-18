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
    check(FileTierPolicy::classify(32 * MiB).tier == FileTier::LargeReadOnly,
          "32 MiB is S2");
    check(FileTierPolicy::classify(2 * GiB).tier == FileTier::LargeReadOnly,
          "2 GiB is S2");
    check(FileTierPolicy::classify(2 * GiB + 1).tier == FileTier::HugeReadOnly,
          "2 GiB + 1 is S3");

    const auto expanded = FileTierPolicy::classify(24 * MiB, 40 * MiB);
    check(expanded.tier == FileTier::LargeReadOnly &&
              expanded.effectiveBytes == 40 * MiB,
          "UTF-8 expansion estimate participates in classification");

    check(!FileTierPolicy::LargeFileEditingEnabled,
          "large-file editing is explicitly disabled");
    check(FileTierPolicy::classify(100 * MiB).isReadOnlyByPolicy(),
          "S2 is read-only regardless of machine resources");

    if (failures == 0)
        qInfo() << "FileTierPolicy: all checks passed";
    return failures == 0 ? 0 : 1;
}
