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

class SecuritySessionService final {
public:
    explicit SecuritySessionService(const QString &dataDirectory = {});
    ~SecuritySessionService();

    SecuritySessionService(const SecuritySessionService &) = delete;
    SecuritySessionService &operator=(const SecuritySessionService &) = delete;

    bool hasIdentity() const noexcept { return identityLoaded_; }
    bool hasActiveSession();
    QString identityId() const { return parameters_.identityId; }
    int keyVersion() const noexcept { return parameters_.keyVersion; }
    QDateTime issuedAtUtc() const { return issuedAtUtc_; }
    QDateTime expiresAtUtc() const { return expiresAtUtc_; }
    QDateTime recoveryVerifiedAtUtc() const { return recoveryVerifiedAtUtc_; }

    VaultError createIdentity(const SecureBuffer &passphraseUtf8,
                              QString *recoveryKey);
    VaultError unlockIdentity(const SecureBuffer &passphraseUtf8);
    // Creates a new keyVersion for future files without rewriting existing
    // KDBX envelopes. The previous master/recovery keys remain in the sealed
    // session ring so old files continue to open silently during this session.
    VaultError changeMasterPassword(const SecureBuffer &currentPassphraseUtf8,
                                    const SecureBuffer &newPassphraseUtf8);
    // Verifies an independently supplied recovery secret against the active
    // identity and persists only a non-secret verification timestamp.
    VaultError verifyRecoverySecret(const SecureBuffer &recoverySecret);
    bool restoreSealedSession();
    void endSession(bool removeDeviceSession = true) noexcept;

    // Unseals the DPAPI capsule only for the lifetime of the returned lease.
    // The service itself never owns a long-lived plaintext key after the call.
    VaultError acquireOperationLease(SecurityOperationLease *lease);

    // Open secure documents retain only a DPAPI blob for their random KDBX
    // credential.  These helpers are intentionally session-gated and keep the
    // plaintext in a caller-owned SecureBuffer for one operation.
    VaultError sealDocumentCredential(const SecureBuffer &credential,
                                      QByteArray *sealedCredential);
    VaultError unsealDocumentCredential(const QByteArray &sealedCredential,
                                        SecureBuffer *credential);

    VaultError acquireRememberedMasterKey(
        const SecurityIdentityParameters &parameters, SecureBuffer *masterUnlockKey);
    VaultError acquireRememberedRecoverySecret(
        const SecurityIdentityParameters &parameters, SecureBuffer *recoverySecret);
    VaultError rememberMasterKey(const SecurityIdentityParameters &parameters,
                                 const SecureBuffer &masterUnlockKey);
    VaultError rememberRecoverySecret(const SecurityIdentityParameters &parameters,
                                      const SecureBuffer &recoverySecret);

    const SecurityIdentityParameters &parameters() const { return parameters_; }

    bool matches(const SecurityIdentityParameters &parameters) const noexcept;
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
