#include "SecurityServiceFactory.h"

#include "SecureDocumentService.h"
#include "SecuritySessionService.h"

namespace {

VaultError unavailableError()
{
    return {VaultErrorCode::BackendUnavailable,
            QStringLiteral("此构建未启用 KDBX 安全存储后端。"),
            QStringLiteral("kdbx-backend-unavailable")};
}

#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
class UnavailableSecuritySessionService final : public ISecuritySessionService {
public:
    bool available() const noexcept override { return false; }
    bool hasIdentity() const noexcept override { return false; }
    bool hasActiveSession() override { return false; }
    QString identityId() const override { return {}; }
    int keyVersion() const noexcept override { return 0; }
    QDateTime issuedAtUtc() const override { return {}; }
    QDateTime expiresAtUtc() const override { return {}; }
    QDateTime recoveryVerifiedAtUtc() const override { return {}; }
    VaultError createIdentity(const SecureBuffer &, QString *) override
    { return unavailableError(); }
    VaultError unlockIdentity(const SecureBuffer &) override
    { return unavailableError(); }
    VaultError changeMasterPassword(const SecureBuffer &,
                                    const SecureBuffer &) override
    { return unavailableError(); }
    VaultError verifyRecoverySecret(const SecureBuffer &) override
    { return unavailableError(); }
    VaultError exportRecoveryKey(QString *) override
    { return unavailableError(); }
    VaultError verifyRecoveryKey(const QString &) override
    { return unavailableError(); }
    VaultError decodeRecoveryKey(const QString &, SecureBuffer *) override
    { return unavailableError(); }
    VaultError deriveMasterUnlockKey(const SecureBuffer &,
                                     const SecurityIdentityParameters &,
                                     SecureBuffer *) override
    { return unavailableError(); }
    bool restoreSealedSession() override { return false; }
    void endSession(bool) noexcept override {}
    VaultError acquireOperationLease(SecurityOperationLease *) override
    { return unavailableError(); }
    VaultError acquireRememberedMasterKey(const SecurityIdentityParameters &,
                                          SecureBuffer *) override
    { return unavailableError(); }
    VaultError acquireRememberedRecoverySecret(const SecurityIdentityParameters &,
                                               SecureBuffer *) override
    { return unavailableError(); }
    VaultError rememberMasterKey(const SecurityIdentityParameters &,
                                 const SecureBuffer &) override
    { return unavailableError(); }
    VaultError rememberRecoverySecret(const SecurityIdentityParameters &,
                                      const SecureBuffer &) override
    { return unavailableError(); }
    const SecurityIdentityParameters &parameters() const override
    { return parameters_; }

private:
    SecurityIdentityParameters parameters_;
};

class UnavailableSecureDocumentService final : public ISecureDocumentService {
public:
    bool available() const noexcept override { return false; }
    VaultError newDraft(SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError open(const QString &, SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError openWithPassphrase(const QString &, const SecureBuffer &,
                                  SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError openWithRecovery(const QString &, const QString &,
                                SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError reload(const QUuid &, SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError save(const QUuid &, const QString &, const QString &,
                    const QString &, bool) override
    { return unavailableError(); }
    VaultError saveBinary(const QUuid &, const QByteArray &, const QString &,
                          const QString &, bool) override
    { return unavailableError(); }
    VaultError saveAsSecure(const QString &, const QString &, const QString &,
                            SecureDocumentSnapshot *) override
    { return unavailableError(); }
    VaultError saveAsSecureBinary(const QByteArray &, const QString &,
                                  const QString &,
                                  SecureDocumentSnapshot *) override
    { return unavailableError(); }
    void close(const QUuid &) noexcept override {}
    void closeAll() noexcept override {}
    QString backingPath(const QUuid &) const override { return {}; }
    DiskFileState externalFileState(const QUuid &) const override
    { return DiskFileState::NotTracked; }
    VaultError checkpointText(const QUuid &, const QString &) override
    { return unavailableError(); }
    VaultError checkpointBinary(const QUuid &, const QByteArray &) override
    { return unavailableError(); }
    VaultError inspectFile(const QString &, QString *, QUuid *,
                           SecurityIdentityParameters *) const override
    { return unavailableError(); }
};
#endif

} // namespace

SecurityServiceBundle createSecurityServiceBundle()
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    auto session = std::make_unique<SecuritySessionService>();
    auto documents = std::make_unique<SecureDocumentService>(session.get());
    return {std::move(session), std::move(documents)};
#else
    return {std::make_unique<UnavailableSecuritySessionService>(),
            std::make_unique<UnavailableSecureDocumentService>()};
#endif
}
