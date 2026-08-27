#pragma once

#include "SecuritySessionPort.h"

#include <QDateTime>
#include <QByteArray>
#include <QString>

#include <vector>

class SecuritySessionService final : public ISecuritySessionService {
public:
    explicit SecuritySessionService(const QString &dataDirectory = {});
    ~SecuritySessionService();

    SecuritySessionService(const SecuritySessionService &) = delete;
    SecuritySessionService &operator=(const SecuritySessionService &) = delete;

    bool available() const noexcept override { return true; }
    bool hasIdentity() const noexcept override { return identityLoaded_; }
    bool hasActiveSession() override;
    QString identityId() const override { return parameters_.identityId; }
    int keyVersion() const noexcept override { return parameters_.keyVersion; }
    QDateTime issuedAtUtc() const override { return issuedAtUtc_; }
    QDateTime expiresAtUtc() const override { return expiresAtUtc_; }
    QDateTime recoveryVerifiedAtUtc() const override { return recoveryVerifiedAtUtc_; }

    VaultError createIdentity(const SecureBuffer &passphraseUtf8,
                              QString *recoveryKey) override;
    VaultError unlockIdentity(const SecureBuffer &passphraseUtf8) override;
    // Creates a new keyVersion for future files without rewriting existing
    // KDBX envelopes. The previous master/recovery keys remain in the sealed
    // session ring so old files continue to open silently during this session.
    VaultError changeMasterPassword(const SecureBuffer &currentPassphraseUtf8,
                                    const SecureBuffer &newPassphraseUtf8) override;
    // Verifies an independently supplied recovery secret against the active
    // identity and persists only a non-secret verification timestamp.
    VaultError verifyRecoverySecret(const SecureBuffer &recoverySecret) override;
    VaultError exportRecoveryKey(QString *recoveryKey) override;
    VaultError verifyRecoveryKey(const QString &recoveryKey) override;
    VaultError decodeRecoveryKey(const QString &recoveryKey,
                                 SecureBuffer *recoverySecret) override;
    VaultError deriveMasterUnlockKey(
        const SecureBuffer &passphraseUtf8,
        const SecurityIdentityParameters &parameters,
        SecureBuffer *masterUnlockKey) override;
    bool restoreSealedSession() override;
    void endSession(bool removeDeviceSession = true) noexcept override;

    // Unseals the DPAPI capsule only for the lifetime of the returned lease.
    // The service itself never owns a long-lived plaintext key after the call.
    VaultError acquireOperationLease(SecurityOperationLease *lease) override;

    // Open secure documents retain only a DPAPI blob for their random KDBX
    // credential.  These helpers are intentionally session-gated and keep the
    // plaintext in a caller-owned SecureBuffer for one operation.
    VaultError sealDocumentCredential(const SecureBuffer &credential,
                                      QByteArray *sealedCredential);
    VaultError unsealDocumentCredential(const QByteArray &sealedCredential,
                                        SecureBuffer *credential);

    VaultError acquireRememberedMasterKey(
        const SecurityIdentityParameters &parameters,
        SecureBuffer *masterUnlockKey) override;
    VaultError acquireRememberedRecoverySecret(
        const SecurityIdentityParameters &parameters,
        SecureBuffer *recoverySecret) override;
    VaultError rememberMasterKey(const SecurityIdentityParameters &parameters,
                                 const SecureBuffer &masterUnlockKey) override;
    VaultError rememberRecoverySecret(const SecurityIdentityParameters &parameters,
                                      const SecureBuffer &recoverySecret) override;

    const SecurityIdentityParameters &parameters() const override { return parameters_; }

    bool matches(const SecurityIdentityParameters &parameters);
    QString descriptorPath() const;
    QString capsulePath() const;

private:
    enum class RememberedKeyKind : quint8 { Master = 1, Recovery = 2 };
    struct RememberedKey {
        SecurityIdentityParameters parameters;
        RememberedKeyKind kind = RememberedKeyKind::Master;
        SecureBuffer secret;
    };

    bool loadDescriptor();
    VaultError writeDescriptor();
    VaultError persistDescriptor(const SecureBuffer &masterUnlockKey,
                                 const SecureBuffer &recoverySecret);
    VaultError decryptRecoverySeed(const SecureBuffer &masterUnlockKey,
                                   SecureBuffer *recoverySecret) const;
    VaultError sealSession(const SecureBuffer &masterUnlockKey,
                           const SecureBuffer &recoverySecret,
                           const std::vector<RememberedKey> &rememberedKeys = {});
    VaultError unsealSession(SecureBuffer *masterUnlockKey,
                             SecureBuffer *recoverySecret,
                             std::vector<RememberedKey> *rememberedKeys,
                             bool removeInvalidCapsule);
    VaultError acquireRememberedKey(const SecurityIdentityParameters &parameters,
                                    RememberedKeyKind kind, SecureBuffer *secret);
    VaultError rememberKey(const SecurityIdentityParameters &parameters,
                           RememberedKeyKind kind, const SecureBuffer &secret);
    bool validateActiveSession();
    void clearMemory() noexcept;

    QString dataDirectory_;
    SecurityIdentityParameters parameters_;
    QByteArray recoveryNonce_;
    QByteArray encryptedRecoverySeed_;
    QDateTime issuedAtUtc_;
    QDateTime expiresAtUtc_;
    QDateTime lastObservedUtc_;
    QDateTime recoveryVerifiedAtUtc_;
    bool identityLoaded_ = false;
    bool active_ = false;
};
