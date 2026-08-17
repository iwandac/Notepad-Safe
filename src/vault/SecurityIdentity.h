#pragma once

#include "VaultTypes.h"

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QUuid>

struct SecurityIdentityParameters {
    QString identityId;
    int keyVersion = 1;
    QByteArray kdfSalt;
    quint64 memoryKiB = 0;
    int iterations = 0;
    int parallelism = 0;
};

// Self-contained security identity stored in KDBX4 PublicCustomData.
// The random database credential never depends on AppData or a sidecar file.
// Password and recovery slots independently wrap it and travel with the KDBX.
class SecurityIdentity final {
public:
    static constexpr quint64 DefaultMemoryKiB = 64 * 1024;
    static constexpr int DefaultIterations = 3;
    static constexpr int DefaultParallelism = 4;

    static QString publicDataKey();
    static bool hasEnvelope(const QVariantMap &publicCustomData);

    static SecurityIdentityParameters newIdentityParameters();
    static VaultError describe(const QVariantMap &publicCustomData,
                               SecurityIdentityParameters *parameters,
                               QString *documentType = nullptr,
                               QUuid *documentId = nullptr);
    static VaultError deriveMasterUnlockKey(const SecureBuffer &passphraseUtf8,
                                            const SecurityIdentityParameters &parameters,
                                            SecureBuffer *masterUnlockKey);
    static QString recoveryKeyText(const SecureBuffer &recoverySecret);
    static VaultError parseRecoveryKey(const QString &text,
                                       SecureBuffer *recoverySecret);

    static VaultError createForIdentity(
        const SecurityIdentityParameters &parameters,
        const SecureBuffer &masterUnlockKey,
        const SecureBuffer &recoverySecret,
        SecureBuffer *databaseCredentialUtf8,
        QVariantMap *publicCustomData,
        const QString &documentType = QStringLiteral("SecureDocument"));

    static VaultError create(const SecureBuffer &passphraseUtf8,
                             SecureBuffer *databaseCredentialUtf8,
                             QString *recoveryKey,
                             QVariantMap *publicCustomData,
                             const QString &documentType = QStringLiteral("Accounts"));

    static VaultError unlock(const QVariantMap &publicCustomData,
                             const SecureBuffer &passphraseUtf8,
                             SecureBuffer *databaseCredentialUtf8);
    static VaultError unlockWithMasterKey(const QVariantMap &publicCustomData,
                                          const SecureBuffer &masterUnlockKey,
                                          SecureBuffer *databaseCredentialUtf8);

    static VaultError recover(const QVariantMap &publicCustomData,
                              const QString &recoveryKey,
                              SecureBuffer *databaseCredentialUtf8);
    static VaultError recoverWithSecret(const QVariantMap &publicCustomData,
                                        const SecureBuffer &recoverySecret,
                                        SecureBuffer *databaseCredentialUtf8);

    // Rewrap only the password slot. The recovery slot and unknown public data
    // remain unchanged; the caller persists the returned map with the KDBX.
    static VaultError rebuildPasswordSlot(const QVariantMap &publicCustomData,
                                          const SecureBuffer &passphraseUtf8,
                                          const SecureBuffer &databaseCredentialUtf8,
                                          QVariantMap *updatedPublicCustomData);

private:
    SecurityIdentity() = delete;
};
