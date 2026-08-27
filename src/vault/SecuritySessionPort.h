#pragma once

#include "SecurityIdentity.h"
#include "VaultTypes.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <utility>

// Short-lived plaintext access to secrets sealed in the device session.
// Application code receives only this capability and never the concrete
// session implementation.
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
    virtual VaultError exportRecoveryKey(QString *recoveryKey) = 0;
    virtual VaultError verifyRecoveryKey(const QString &recoveryKey) = 0;
    virtual VaultError decodeRecoveryKey(const QString &recoveryKey,
                                         SecureBuffer *recoverySecret) = 0;
    virtual VaultError deriveMasterUnlockKey(
        const SecureBuffer &passphraseUtf8,
        const SecurityIdentityParameters &parameters,
        SecureBuffer *masterUnlockKey) = 0;
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
