#pragma once

#include "SecurityIdentity.h"

#include <QDateTime>
#include <QByteArray>
#include <QString>

#include <utility>
#include <vector>

// A deliberately short-lived plaintext view of the secrets sealed in the
// seven-day device session.  Callers acquire it only around one cryptographic
// operation; destruction and move-assignment wipe both buffers.
class SecurityOperationLease final {
public:
    SecurityOperationLease() = default;
    ~SecurityOperationLease() = default;

    SecurityOperationLease(const SecurityOperationLease &) = delete;
    SecurityOperationLease &operator=(const SecurityOperationLease &) = delete;
    SecurityOperationLease(SecurityOperationLease &&) noexcept = default;
    SecurityOperationLease &operator=(SecurityOperationLease &&) noexcept = default;

    bool isValid() const noexcept
    {
        return !masterUnlockKey_.empty() && !recoverySecret_.empty();
    }
    const SecureBuffer &masterUnlockKey() const noexcept { return masterUnlockKey_; }
    const SecureBuffer &recoverySecret() const noexcept { return recoverySecret_; }
    void clear() noexcept
    {
        masterUnlockKey_.clear();
        recoverySecret_.clear();
    }

private:
    friend class SecuritySessionService;
    SecurityOperationLease(SecureBuffer &&masterUnlockKey,
                           SecureBuffer &&recoverySecret) noexcept
        : masterUnlockKey_(std::move(masterUnlockKey))
        , recoverySecret_(std::move(recoverySecret))
    {
    }

    SecureBuffer masterUnlockKey_;
    SecureBuffer recoverySecret_;
};

// Application-facing port.  MainWindow and document workflows depend on this
// capability contract, never on whether the KDBX adapter was compiled.
class ISecuritySessionService {
public:
    virtual ~ISecuritySessionService() = default;

    virtual bool available() const noexcept = 0;
    virtual bool hasIdentity() const noexcept = 0;
    virtual bool hasActiveSession() = 0;
    virtual QString identityId() const = 0;
    virtual int keyVersion() const noexcept = 0;
    virtual QDateTime issuedAtUtc() const = 0;
    virtual QDateTime expiresAtUtc() const = 0;
    virtual QDateTime recoveryVerifiedAtUtc() const = 0;
    virtual VaultError createIdentity(const SecureBuffer &passphraseUtf8,
                                      QString *recoveryKey) = 0;
    virtual VaultError unlockIdentity(const SecureBuffer &passphraseUtf8) = 0;
    virtual VaultError changeMasterPassword(
        const SecureBuffer &currentPassphraseUtf8,
        const SecureBuffer &newPassphraseUtf8) = 0;
    virtual VaultError verifyRecoverySecret(const SecureBuffer &recoverySecret) = 0;
    virtual bool restoreSealedSession() = 0;
    virtual void endSession(bool removeDeviceSession = true) noexcept = 0;
    virtual VaultError acquireOperationLease(SecurityOperationLease *lease) = 0;
    virtual VaultError acquireRememberedMasterKey(
        const SecurityIdentityParameters &parameters,
        SecureBuffer *masterUnlockKey) = 0;
    virtual VaultError acquireRememberedRecoverySecret(
        const SecurityIdentityParameters &parameters,
        SecureBuffer *recoverySecret) = 0;
    virtual VaultError rememberMasterKey(
        const SecurityIdentityParameters &parameters,
        const SecureBuffer &masterUnlockKey) = 0;
    virtual VaultError rememberRecoverySecret(
        const SecurityIdentityParameters &parameters,
        const SecureBuffer &recoverySecret) = 0;
    virtual const SecurityIdentityParameters &parameters() const = 0;
};

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
