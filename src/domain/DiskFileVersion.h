#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <utility>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

// Shared, bounded file-version evidence used by plaintext, mmap/Hex and KDBX
// writers. It deliberately lives below all document providers so every save
// path makes the same conservative overwrite decision.
enum class DiskFileState {
    NotTracked,
    Unchanged,
    Changed,
    Missing,
    Unreadable,
};

struct DiskFileVersion {
    bool valid = false;
    QString absolutePath;
    qint64 size = -1;
    qint64 modifiedMs = -1;
    QByteArray fileIdentity;
    QByteArray sampleHash;
};

// Metadata-only evidence for a particular opened source. This is deliberately
// not a DiskFileVersion: it must never replace a save-conflict fingerprint.
struct DiskFileStamp {
    bool valid = false;
    qint64 size = -1;
    qint64 modifiedMs = -1;
    QByteArray fileIdentity;
};

inline bool sameDiskFileStamp(const DiskFileStamp &left, const DiskFileStamp &right)
{
    return left.valid && right.valid && !left.fileIdentity.isEmpty()
        && left.fileIdentity == right.fileIdentity
        && left.size == right.size && left.modifiedMs == right.modifiedMs;
}

#ifdef Q_OS_WIN
inline QByteArray diskFileIdentity(const BY_HANDLE_FILE_INFORMATION &identity)
{
    return QByteArray::number(identity.dwVolumeSerialNumber)
        + ':' + QByteArray::number(identity.nFileIndexHigh)
        + ':' + QByteArray::number(identity.nFileIndexLow);
}
#else
inline QByteArray diskFileIdentity(const struct stat &identity)
{
    return QByteArray::number(static_cast<qulonglong>(identity.st_dev))
        + ':' + QByteArray::number(static_cast<qulonglong>(identity.st_ino));
}
#endif

// Keep the existing byte-for-byte bounded fingerprint algorithm shared by
// path-based save checks and same-handle snapshots taken after read admission.
template <typename ReadSample>
inline bool captureDiskFileSampleHash(qint64 size, ReadSample readSample, QByteArray *result)
{
    if (!result || size < 0) return false;
    result->clear();
    constexpr qint64 sampleBytes = 64 * 1024;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(size));
    const auto addSample = [&readSample, &hash](qint64 offset, qint64 count) {
        const QByteArray sample = readSample(offset, count);
        if (sample.size() != count) return false;
        hash.addData(QByteArray::number(offset));
        hash.addData(":", 1);
        hash.addData(sample);
        return true;
    };
    if (size <= sampleBytes * 3) {
        if (!addSample(0, size)) return false;
    } else {
        const qint64 middle = qMax<qint64>(0, size / 2 - sampleBytes / 2);
        if (!addSample(0, sampleBytes) || !addSample(middle, sampleBytes)
            || !addSample(size - sampleBytes, sampleBytes)) return false;
    }
    *result = hash.result();
    return true;
}

inline DiskFileState captureDiskFileVersion(const QString &path,
                                            DiskFileVersion *version)
{
    if (version)
        *version = {};
    if (path.trimmed().isEmpty())
        return DiskFileState::NotTracked;

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return DiskFileState::Missing;

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return DiskFileState::Unreadable;

    DiskFileVersion captured;
    captured.absolutePath = info.absoluteFilePath();
    captured.size = file.size();
    captured.modifiedMs = info.lastModified().toMSecsSinceEpoch();

#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(captured.absolutePath).toStdWString();
    HANDLE handle = ::CreateFileW(nativePath.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION identity{};
        if (::GetFileInformationByHandle(handle, &identity)) {
            captured.fileIdentity = diskFileIdentity(identity);
        }
        ::CloseHandle(handle);
    }
#else
    struct stat identity{};
    if (::fstat(file.handle(), &identity) == 0)
        captured.fileIdentity = diskFileIdentity(identity);
#endif

    bool readSucceeded = true;
    if (!captureDiskFileSampleHash(captured.size,
            [&file, &readSucceeded](qint64 offset, qint64 count) {
                if (!file.seek(offset)) {
                    readSucceeded = false;
                    return QByteArray{};
                }
                return file.read(count);
            }, &captured.sampleHash)
        || !readSucceeded || file.error() != QFileDevice::NoError)
        return DiskFileState::Unreadable;

    captured.valid = true;
    if (version)
        *version = std::move(captured);
    return DiskFileState::Unchanged;
}

inline DiskFileState compareDiskFileVersion(const QString &path,
                                            const DiskFileVersion &baseline)
{
    if (path.trimmed().isEmpty())
        return DiskFileState::NotTracked;
    DiskFileVersion current;
    const DiskFileState probe = captureDiskFileVersion(path, &current);
    if (probe != DiskFileState::Unchanged)
        return probe;
    if (!baseline.valid)
        return DiskFileState::Unreadable;
    if (baseline.fileIdentity != current.fileIdentity
        || baseline.size != current.size
        || baseline.modifiedMs != current.modifiedMs
        || baseline.sampleHash != current.sampleHash) {
        return DiskFileState::Changed;
    }
    return DiskFileState::Unchanged;
}

inline bool sameDiskFilePath(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty())
        return false;
    const QString normalizedLeft = QDir::cleanPath(QFileInfo(left).absoluteFilePath());
    const QString normalizedRight = QDir::cleanPath(QFileInfo(right).absoluteFilePath());
#ifdef Q_OS_WIN
    return normalizedLeft.compare(normalizedRight, Qt::CaseInsensitive) == 0;
#else
    return normalizedLeft == normalizedRight;
#endif
}

// Compare the identity of two successful probes.  On Windows the volume/file
// index pair also recognizes hard links and alternate path spellings.  Path is
// the conservative portable fallback when the platform cannot expose a stable
// file id.
inline bool sameDiskFile(const DiskFileVersion &left,
                         const DiskFileVersion &right)
{
    if (!left.valid || !right.valid)
        return false;
    if (!left.fileIdentity.isEmpty() && !right.fileIdentity.isEmpty())
        return left.fileIdentity == right.fileIdentity;
    return sameDiskFilePath(left.absolutePath, right.absolutePath);
}
