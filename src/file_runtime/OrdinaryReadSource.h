#pragma once

#include "DiskFileVersion.h"
#include "PersistenceReadPolicy.h"

class SharedReadOnlyFile;

enum class OrdinaryReadStatus {
    Ready,
    Missing,
    Unreadable,
    Changed,
    SecureType,
    PolicyUnavailable,
};

struct OrdinaryReadResult {
    OrdinaryReadStatus status = OrdinaryReadStatus::Unreadable;
    DiskFileStamp stamp;
    QString adapterId;
    QString error;
    bool isReady() const noexcept { return status == OrdinaryReadStatus::Ready; }
};

// One open, then metadata + bounded format header + metadata from that same
// handle. Ready leaves reader open for the caller's readAt/map; every rejection
// closes it. Policy/expected are immutable values, never UI-registry callbacks.
OrdinaryReadResult openOrdinaryReadSource(
    SharedReadOnlyFile &reader, const QString &path,
    const PersistenceReadPolicy &policy, const DiskFileStamp *expected = nullptr);

// Cheap observation only: no header, content samples, registry or mapping.
// Its typed stamp must not replace a complete DiskFileVersion save baseline.
DiskFileState captureDiskFileStamp(const QString &path, DiskFileStamp *stamp,
                                  QString *error = nullptr);
