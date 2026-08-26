#pragma once

#include "SecuritySessionService.h"
#include "VaultTypes.h"
#include "DiskFileVersion.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>

#include <memory>
#include <vector>

enum class SecureDocumentPayloadKind {
    Text,
    Binary,
};

struct SecureDocumentSnapshot {
    QUuid documentId;
    QString filePath;
    QString backingPath;
    QString title;
    QString text;
    QByteArray binary;
    SecureDocumentPayloadKind payloadKind = SecureDocumentPayloadKind::Text;
    bool draft = false;
    bool recoveredUnsaved = false;
};

class ISecureDocumentService {
public:
    virtual ~ISecureDocumentService() = default;

    virtual bool available() const noexcept = 0;
    virtual VaultError newDraft(SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError open(const QString &filePath,
                            SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError openWithPassphrase(
        const QString &filePath, const SecureBuffer &passphraseUtf8,
        SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError openWithRecovery(const QString &filePath,
                                        const QString &recoveryKey,
                                        SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError reload(const QUuid &documentId,
                              SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError save(const QUuid &documentId, const QString &text,
                            const QString &title, const QString &targetPath = {},
                            bool explicitRevision = true) = 0;
    virtual VaultError saveBinary(const QUuid &documentId, const QByteArray &binary,
                                  const QString &title,
                                  const QString &targetPath = {},
                                  bool explicitRevision = true) = 0;
    virtual VaultError saveAsSecure(const QString &text, const QString &title,
                                    const QString &targetPath,
                                    SecureDocumentSnapshot *snapshot) = 0;
    virtual VaultError saveAsSecureBinary(
        const QByteArray &binary, const QString &title, const QString &targetPath,
        SecureDocumentSnapshot *snapshot) = 0;
    virtual void close(const QUuid &documentId) noexcept = 0;
    virtual void closeAll() noexcept = 0;
    virtual QString backingPath(const QUuid &documentId) const = 0;
    virtual DiskFileState externalFileState(const QUuid &documentId) const = 0;
    virtual VaultError checkpointText(const QUuid &documentId,
                                      const QString &text) = 0;
    virtual VaultError checkpointBinary(const QUuid &documentId,
                                        const QByteArray &binary) = 0;
    virtual VaultError inspectFile(
        const QString &filePath, QString *documentType,
        QUuid *documentId = nullptr,
        SecurityIdentityParameters *parameters = nullptr) const = 0;
};

class SecureDocumentService final : public ISecureDocumentService {
public:
    static constexpr qsizetype PayloadMaximumExclusive = 32 * 1024 * 1024;

    explicit SecureDocumentService(SecuritySessionService *securitySession,
                                   const QString &dataDirectory = {});
    ~SecureDocumentService();

    bool available() const noexcept override { return true; }
    VaultError newDraft(SecureDocumentSnapshot *snapshot) override;
    VaultError open(const QString &filePath, SecureDocumentSnapshot *snapshot) override;
    VaultError openWithPassphrase(const QString &filePath,
                                  const SecureBuffer &passphraseUtf8,
                                  SecureDocumentSnapshot *snapshot) override;
    VaultError openWithRecovery(const QString &filePath,
                                const QString &recoveryKey,
                                SecureDocumentSnapshot *snapshot) override;
    VaultError reload(const QUuid &documentId,
                      SecureDocumentSnapshot *snapshot) override;
    VaultError save(const QUuid &documentId, const QString &text,
                    const QString &title, const QString &targetPath = {},
                    bool explicitRevision = true) override;
    VaultError saveBinary(const QUuid &documentId, const QByteArray &binary,
                          const QString &title, const QString &targetPath = {},
                          bool explicitRevision = true) override;
    VaultError saveAsSecure(const QString &text, const QString &title,
                            const QString &targetPath,
                            SecureDocumentSnapshot *snapshot) override;
    VaultError saveAsSecureBinary(const QByteArray &binary, const QString &title,
                                  const QString &targetPath,
                                  SecureDocumentSnapshot *snapshot) override;
    void close(const QUuid &documentId) noexcept override;
    void closeAll() noexcept override;
    QString backingPath(const QUuid &documentId) const override;
    DiskFileState externalFileState(const QUuid &documentId) const override;
    VaultError checkpointText(const QUuid &documentId,
                              const QString &text) override;
    VaultError checkpointBinary(const QUuid &documentId,
                                const QByteArray &binary) override;
    VaultError inspectFile(const QString &filePath, QString *documentType,
                           QUuid *documentId = nullptr,
                           SecurityIdentityParameters *parameters = nullptr) const override;
    bool hasRecoverySnapshot(const QUuid &documentId) const;
    void clearRecoverySnapshot(const QUuid &documentId) noexcept;

    static VaultError inspect(const QString &filePath,
                              QString *documentType,
                              QUuid *documentId = nullptr,
                              SecurityIdentityParameters *parameters = nullptr);

private:
    struct Handle {
        QUuid documentId;
        QUuid entryId;
        QString filePath;
        QString title;
        QVariantMap publicCustomData;
        // The random KDBX credential is never retained as plaintext for the
        // lifetime of an open tab.  It is DPAPI-sealed and leased per I/O.
        QByteArray sealedDatabaseCredential;
        bool persisted = false;
        bool draft = false;
        SecureDocumentPayloadKind payloadKind = SecureDocumentPayloadKind::Text;
        DiskFileVersion diskVersion;
        QDateTime lastHistoryAtUtc;
    };

    Handle *find(const QUuid &documentId) const;
    VaultError createHandle(const QString &path, bool draft, Handle **handle);
    VaultError openWithCredential(const QString &filePath,
                                  QVariantMap publicCustomData,
                                  SecureBuffer &&databaseCredential,
                                  SecureDocumentSnapshot *snapshot);
    VaultError persist(Handle *handle, SecureDocumentPayloadKind payloadKind,
                       const QString &text, const QByteArray &binary,
                       const QString &title, const QString &targetPath,
                       bool explicitRevision);
    VaultError writeRecoverySnapshot(const QUuid &documentId,
                                     SecureDocumentPayloadKind payloadKind,
                                     const QByteArray &payload);
    VaultError applyRecoverySnapshot(const QUuid &documentId,
                                     SecureDocumentPayloadKind *payloadKind,
                                     QString *text, QByteArray *binary,
                                     bool *recovered);
    QString recoveryPath(const QUuid &documentId) const;
    QString draftPath(const QUuid &documentId) const;
    bool isManagedDraftPath(const QString &path, const QUuid &documentId) const;
    static VaultError validatePayload(const QString &text);
    static VaultError validatePayload(const QByteArray &binary);

    SecuritySessionService *securitySession_ = nullptr;
    QString dataDirectory_;
    std::vector<std::unique_ptr<Handle>> handles_;
};
