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
            captured.fileIdentity = QByteArray::number(identity.dwVolumeSerialNumber)
                + ':' + QByteArray::number(identity.nFileIndexHigh)
                + ':' + QByteArray::number(identity.nFileIndexLow);
        }
        ::CloseHandle(handle);
    }
#endif

    constexpr qint64 sampleBytes = 64 * 1024;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(captured.size));
    const auto addSample = [&file, &hash](qint64 offset, qint64 count) -> bool {
        if (!file.seek(offset))
            return false;
        const QByteArray sample = file.read(count);
        if (sample.size() != count)
            return false;
        hash.addData(QByteArray::number(offset));
        hash.addData(":", 1);
        hash.addData(sample);
        return true;
    };

    if (captured.size <= sampleBytes * 3) {
        if (!addSample(0, captured.size))
            return DiskFileState::Unreadable;
    } else {
        const qint64 middle = qMax<qint64>(0, captured.size / 2 - sampleBytes / 2);
        if (!addSample(0, sampleBytes)
            || !addSample(middle, sampleBytes)
            || !addSample(captured.size - sampleBytes, sampleBytes)) {
            return DiskFileState::Unreadable;
        }
    }
    if (file.error() != QFileDevice::NoError)
        return DiskFileState::Unreadable;

    captured.sampleHash = hash.result();
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
