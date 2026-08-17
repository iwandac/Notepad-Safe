#pragma once

#include "SecuritySessionService.h"
#include "VaultTypes.h"
#include "DiskFileVersion.h"

#include <QByteArray>
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
};

class SecureDocumentService final {
public:
    static constexpr qsizetype PayloadMaximumExclusive = 32 * 1024 * 1024;

    explicit SecureDocumentService(SecuritySessionService *securitySession,
                                   const QString &dataDirectory = {});
    ~SecureDocumentService();

    VaultError newDraft(SecureDocumentSnapshot *snapshot);
    VaultError open(const QString &filePath, SecureDocumentSnapshot *snapshot);
    VaultError openWithPassphrase(const QString &filePath,
                                  const SecureBuffer &passphraseUtf8,
                                  SecureDocumentSnapshot *snapshot);
    VaultError openWithRecovery(const QString &filePath,
                                const QString &recoveryKey,
                                SecureDocumentSnapshot *snapshot);
    VaultError reload(const QUuid &documentId, SecureDocumentSnapshot *snapshot);
    VaultError save(const QUuid &documentId, const QString &text,
                    const QString &title, const QString &targetPath = {});
    VaultError saveBinary(const QUuid &documentId, const QByteArray &binary,
                          const QString &title, const QString &targetPath = {});
    VaultError saveAsSecure(const QString &text, const QString &title,
                            const QString &targetPath,
                            SecureDocumentSnapshot *snapshot);
    VaultError saveAsSecureBinary(const QByteArray &binary, const QString &title,
                                  const QString &targetPath,
                                  SecureDocumentSnapshot *snapshot);
    void close(const QUuid &documentId) noexcept;
    void closeAll() noexcept;
    QString backingPath(const QUuid &documentId) const;
    DiskFileState externalFileState(const QUuid &documentId) const;

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
    };

    Handle *find(const QUuid &documentId) const;
    VaultError createHandle(const QString &path, bool draft, Handle **handle);
    VaultError openWithCredential(const QString &filePath,
                                  QVariantMap publicCustomData,
                                  SecureBuffer &&databaseCredential,
                                  SecureDocumentSnapshot *snapshot);
    VaultError persist(Handle *handle, SecureDocumentPayloadKind payloadKind,
                       const QString &text, const QByteArray &binary,
                       const QString &title, const QString &targetPath);
    QString draftPath(const QUuid &documentId) const;
    static VaultError validatePayload(const QString &text);
    static VaultError validatePayload(const QByteArray &binary);

    SecuritySessionService *securitySession_ = nullptr;
    QString dataDirectory_;
    std::vector<std::unique_ptr<Handle>> handles_;
};
