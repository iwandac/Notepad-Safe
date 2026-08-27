#pragma once

#include "DocumentState.h"
#include "DiskFileVersion.h"
#include "SecureDocumentContract.h"
#include "SecurityIdentity.h"
#include "VaultTypes.h"

class IDocumentContentPort;

struct SecurePersistencePayload {
    DocumentState state;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    QString text;
    QByteArray bytes;

    void clear();
};

// Application port for secure-document persistence.  UI code may coordinate
// user prompts through this interface but cannot construct or name the KDBX
// implementation.
class ISecureDocumentPersistence {
public:
    virtual ~ISecureDocumentPersistence() = default;
    virtual bool available() const noexcept = 0;
    virtual VaultError capture(IDocumentContentPort *content,
                               SecurePersistencePayload *payload) const = 0;
    virtual VaultError saveExisting(const SecurePersistencePayload &payload,
                                    const QString &title,
                                    const QString &targetPath = {},
                                    bool explicitRevision = true) = 0;
    virtual VaultError saveAsSecure(const SecurePersistencePayload &payload,
                                    const QString &title,
                                    const QString &targetPath,
                                    SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError checkpoint(const SecurePersistencePayload &payload) = 0;
    virtual VaultError newDraft(SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError open(const QString &path,
                            SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError openWithPassphrase(
        const QString &path, const SecureBuffer &passphrase,
        SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError openWithRecovery(
        const QString &path, const QString &recoveryKey,
        SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError reload(const QUuid &documentId,
                              SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError inspectFile(
        const QString &path, QString *documentType, QUuid *documentId = nullptr,
        SecurityIdentityParameters *parameters = nullptr) const = 0;
    virtual DiskFileState externalFileState(const QUuid &documentId) const = 0;
    virtual QString backingPath(const QUuid &documentId) const = 0;
    virtual void close(const QUuid &documentId) noexcept = 0;
    virtual void closeAll() noexcept = 0;
};
