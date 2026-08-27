#pragma once

#include "DocumentState.h"
#include "PersistenceAdapter.h"
#include "SecureDocumentService.h"
#include "SecureDocumentPersistencePort.h"

#include <memory>

class IDocumentContentPort;

// Application-layer adapter for the secure-document lifecycle. It owns no UI
// and never asks for credentials; callers schedule operations and provide an
// already-authorized secure service.
class SecureDocumentPersistence final : public IPersistenceAdapter,
                                        public ISecureDocumentPersistence {
public:
    explicit SecureDocumentPersistence(
        std::unique_ptr<ISecureDocumentService> service)
        : service_(std::move(service))
    {
    }

    bool available() const noexcept override;
    PersistenceAdapterDescriptor descriptor() const override;
    bool recognizes(const QString &path) const override;
    PersistenceResult save(IDocumentContentPort *content,
                           const PersistenceSaveRequest &request) override;
    VaultError capture(IDocumentContentPort *content,
                       SecurePersistencePayload *payload) const override;
    VaultError saveExisting(const SecurePersistencePayload &payload,
                            const QString &title,
                            const QString &targetPath = {},
                            bool explicitRevision = true) override;
    VaultError saveAsSecure(const SecurePersistencePayload &payload,
                            const QString &title,
                            const QString &targetPath,
                            SecureDocumentSnapshot *snapshot) override;
    VaultError checkpoint(const SecurePersistencePayload &payload) override;

    VaultError newDraft(SecureDocumentSnapshot *snapshot) override;
    VaultError open(const QString &path, SecureDocumentSnapshot *snapshot) override;
    VaultError openWithPassphrase(const QString &path,
                                  const SecureBuffer &passphrase,
                                  SecureDocumentSnapshot *snapshot) override;
    VaultError openWithRecovery(const QString &path, const QString &recoveryKey,
                                SecureDocumentSnapshot *snapshot) override;
    VaultError reload(const QUuid &documentId,
                      SecureDocumentSnapshot *snapshot) override;
    VaultError inspectFile(const QString &path, QString *documentType,
                           QUuid *documentId = nullptr,
                           SecurityIdentityParameters *parameters = nullptr) const override;
    DiskFileState externalFileState(const QUuid &documentId) const override;
    QString backingPath(const QUuid &documentId) const override;
    void close(const QUuid &documentId) noexcept override;
    void closeAll() noexcept override;

private:
    static VaultError unavailableError();
    std::unique_ptr<ISecureDocumentService> service_;
};
