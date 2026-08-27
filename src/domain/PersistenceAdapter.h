#pragma once

#include "DiskFileVersion.h"
#include "DocumentState.h"

#include <QString>

class IDocumentContentPort;

enum class PersistenceStatus {
    Success,
    Conflict,
    Unsupported,
    Error,
};

struct PersistenceSaveRequest {
    QString targetPath;
    QString displayTitle;
    DiskFileVersion expectedVersion;
    bool requireExpectedVersion = false;
};

struct PersistenceResult {
    PersistenceStatus status = PersistenceStatus::Error;
    QString error;
    DiskFileVersion committedVersion;

    bool succeeded() const noexcept { return status == PersistenceStatus::Success; }
};

struct PersistenceAdapterDescriptor {
    QString id;
    QString displayName;
    int priority = 0;
    DocumentPersistence persistence = DocumentPersistence::PlainFile;
};

class IPersistenceAdapter {
public:
    virtual ~IPersistenceAdapter() = default;
    virtual PersistenceAdapterDescriptor descriptor() const = 0;
    virtual bool recognizes(const QString &path) const = 0;
    virtual PersistenceResult save(IDocumentContentPort *content,
                                   const PersistenceSaveRequest &request) = 0;
};
