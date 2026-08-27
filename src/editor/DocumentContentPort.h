#pragma once

#include "DiskFileVersion.h"
#include "DocumentState.h"
#include "TextEncoding.h"

class IDocumentContentPort {
public:
    virtual ~IDocumentContentPort() = default;

    virtual DocumentState persistenceState() const noexcept = 0;
    virtual TextEncoding persistenceEncoding() const noexcept = 0;
    virtual DiskFileVersion persistenceBaseline() const = 0;
    virtual bool createPersistenceSnapshot(qint64 maximumBytes,
                                           DocumentSnapshot *snapshot,
                                           QString *error) = 0;
    // Atomic replacement on Windows requires mapped providers to release the
    // source handle after the snapshot is complete and before commit. The
    // adapter owns the transaction order; the surface owns its backing store.
    virtual bool suspendPersistenceBacking(QString *error) = 0;
    virtual bool resumePersistenceBacking(const QString &absolutePath,
                                          QString *error) = 0;
    virtual void acceptPersistenceCommit(const QString &absolutePath,
                                         DiskFileVersion version) = 0;
    virtual void acceptSecurePersistenceCommit(const QUuid &documentId,
                                               const QString &title,
                                               const QString &absolutePath,
                                               const QString &backingPath) = 0;
};
