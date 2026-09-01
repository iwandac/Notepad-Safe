#include "OrdinaryReadSource.h"

#include "SharedReadOnlyFile.h"

OrdinaryReadResult openOrdinaryReadSource(
    SharedReadOnlyFile &reader, const QString &path,
    const PersistenceReadPolicy &policy, const DiskFileStamp *expected)
{
    reader.close();
    OrdinaryReadResult result;
    const auto reject = [&](OrdinaryReadStatus status, const QString &error) {
        result.status = status;
        result.error = error;
        reader.close();
        return result;
    };
    if (!policy.isAvailable())
        return reject(OrdinaryReadStatus::PolicyUnavailable,
                      QStringLiteral("Registered secure-format read policy is unavailable"));

    QString error;
    if (!openSharedReadOnlyFile(reader, path, &error)) {
        return reject(reader.openFailureState() == DiskFileState::Missing
                          ? OrdinaryReadStatus::Missing : OrdinaryReadStatus::Unreadable,
                      error);
    }
    if (!reader.captureStamp(&result.stamp, &error))
        return reject(OrdinaryReadStatus::Unreadable, error);
    if (result.stamp.size != reader.size())
        return reject(OrdinaryReadStatus::Changed,
                      QStringLiteral("Source size changed while opening"));

    // A short ordinary file, including an empty file, is valid. The registered
    // rule also sees its original path so secure extensions remain protected.
    const qsizetype prefixBytes = static_cast<qsizetype>(
        qMin<qint64>(result.stamp.size, policy.requiredPrefixBytes()));
    const QByteArray prefix = reader.readAt(0, prefixBytes, &error);
    DiskFileStamp afterHeader;
    QString stampError;
    if (!reader.captureStamp(&afterHeader, &stampError))
        return reject(OrdinaryReadStatus::Unreadable, stampError);
    if (!sameDiskFileStamp(result.stamp, afterHeader)) {
        result.stamp = std::move(afterHeader);
        return reject(OrdinaryReadStatus::Changed,
                      QStringLiteral("Source changed while inspecting its format header"));
    }
    if (prefix.size() != prefixBytes)
        return reject(OrdinaryReadStatus::Unreadable, error.isEmpty()
            ? QStringLiteral("Short read while inspecting source format header") : error);

    // Secure classification takes precedence over an old snapshot mismatch:
    // it gives the UI the correct explicit-open next step, without unlocking.
    result.adapterId = policy.secureAdapterFor(path, prefix);
    if (!result.adapterId.isEmpty())
        return reject(OrdinaryReadStatus::SecureType,
                      QStringLiteral("Source belongs to registered secure format %1")
                          .arg(result.adapterId));
    if (expected && !sameDiskFileStamp(*expected, result.stamp))
        return reject(OrdinaryReadStatus::Changed,
                      QStringLiteral("Opened source does not match the admitted snapshot"));

    result.status = OrdinaryReadStatus::Ready;
    return result;
}

DiskFileState captureDiskFileStamp(const QString &path, DiskFileStamp *stamp, QString *error)
{
    if (stamp) *stamp = {};
    if (error) error->clear();
    if (path.trimmed().isEmpty()) return DiskFileState::NotTracked;
    SharedReadOnlyFile reader;
    if (!openSharedReadOnlyFile(reader, path, error))
        return reader.openFailureState();
    return reader.captureStamp(stamp, error)
        ? DiskFileState::Unchanged : DiskFileState::Unreadable;
}
