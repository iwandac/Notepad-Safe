#include "KdbxFormatProbe.h"
#include "VaultService.h"

#include <QCoreApplication>
#include <QDebug>

#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << "FAIL:" << message;
        ++failures;
    }
}

class FakeVaultBackend final : public IVaultBackend {
public:
    VaultError create(const QString &, const SecureBuffer &password,
                      const QString &keyFilePath,
                      const QVariantMap &publicCustomData) override
    {
        storedPublicCustomData = publicCustomData;
        return unlock({}, password, keyFilePath);
    }

    VaultError readPublicCustomData(const QString &, QVariantMap *publicCustomData) const override
    {
        if (!publicCustomData)
            return {VaultErrorCode::InternalError, QStringLiteral("missing output"), {}};
        *publicCustomData = storedPublicCustomData;
        return VaultError::success();
    }

    VaultError unlock(const QString &, const SecureBuffer &password,
                      const QString &) override
    {
        static constexpr char Expected[] = "correct horse";
        if (!password.equals(Expected, sizeof(Expected) - 1)) {
            return {VaultErrorCode::InvalidCredentials,
                    QStringLiteral("密码错误。"),
                    QStringLiteral("vault.credentials.invalid")};
        }
        open = true;
        return VaultError::success();
    }

    void lock() noexcept override { open = false; }

    QList<VaultEntrySummary> listEntries(VaultError *error) const override
    {
        if (error)
            *error = VaultError::success();
        return {{QUuid::createUuid(), QStringLiteral("Example"), {}, {}, {}, false, true}};
    }

    QList<VaultEntrySummary> listRecycledEntries(VaultError *error) const override
    {
        if (error)
            *error = VaultError::success();
        return {};
    }
    QSet<QUuid> searchEntryIds(const QString &, bool, VaultError *error) const override
    {
        if (error)
            *error = VaultError::success();
        return {};
    }

    std::unique_ptr<VaultEntryData> readEntry(const QUuid &, VaultError *) const override
    {
        return {};
    }
    VaultError currentTotp(const QUuid &, VaultTotpSnapshot *snapshot) const override
    {
        if (snapshot)
            *snapshot = {QStringLiteral("123456"), 12};
        return VaultError::success();
    }
    VaultError generatePassword(int length, SecureBuffer *passwordUtf8) const override
    {
        const QByteArray generated(length, 'A');
        passwordUtf8->assign(generated.constData(), generated.size());
        return VaultError::success();
    }
    VaultError assessPassword(const SecureBuffer &passwordUtf8,
                              VaultPasswordAssessment *assessment) const override
    {
        *assessment = {passwordUtf8.empty() ? VaultPasswordQuality::Empty
                                            : VaultPasswordQuality::Good,
                       passwordUtf8.empty() ? 0.0 : 80.0};
        return VaultError::success();
    }
    VaultError upsertEntry(VaultEntryData &&) override { return VaultError::success(); }
    VaultError recycleEntry(const QUuid &) override { return VaultError::success(); }
    VaultError restoreEntry(const QUuid &) override { return VaultError::success(); }
    VaultError removeEntry(const QUuid &) override { return VaultError::success(); }
    VaultError save() override { return VaultError::success(); }

    bool open = false;
    QVariantMap storedPublicCustomData;
};

QString fixture(const char *name)
{
    return QString::fromUtf8(NOTEPADSAFE_KDBX_FIXTURE_DIR) + QLatin1Char('/') +
           QString::fromLatin1(name);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    static_assert(!std::is_copy_constructible_v<SecureBuffer>);
    static_assert(!std::is_copy_assignable_v<SecureBuffer>);
    static_assert(std::is_move_constructible_v<SecureBuffer>);

    const char secret[] = "correct horse";
    SecureBuffer password(secret, sizeof(secret) - 1);
    check(password.equals(secret, sizeof(secret) - 1), "secure buffer preserves bytes");
    SecureBuffer moved(std::move(password));
    check(password.empty() && moved.equals(secret, sizeof(secret) - 1),
          "secure buffer move transfers ownership without a logical copy");

    auto backend = std::make_unique<FakeVaultBackend>();
    VaultService service(std::move(backend));
    VaultError lockedError;
    check(service.listEntries(&lockedError).isEmpty() &&
              lockedError.code == VaultErrorCode::Locked,
          "entry enumeration is denied while locked");
    check(!service.unlock(QStringLiteral("fixture.kdbx"), moved).isError() &&
              service.state() == VaultState::Open,
          "valid credentials transition the service to Open");
    check(service.listEntries().size() == 1,
          "open service delegates summary-only enumeration");
    service.lock();
    check(service.state() == VaultState::Locked && service.filePath().isEmpty(),
          "lock clears session path and state");
    check(!service.create(QStringLiteral("new.kdbx"), moved).isError() &&
              service.state() == VaultState::Open &&
              service.filePath() == QStringLiteral("new.kdbx"),
          "create transitions the service to Open with the new path");
    service.lock();

    const KdbxHeaderInfo kdbx3 = KdbxFormatProbe::inspectFile(fixture("Format300.kdbx"));
    const KdbxHeaderInfo kdbx4 = KdbxFormatProbe::inspectFile(fixture("Format400.kdbx"));
    const KdbxHeaderInfo protectedStrings =
        KdbxFormatProbe::inspectFile(fixture("ProtectedStrings.kdbx"));
    check(kdbx3.recognized && kdbx3.majorVersion == 3,
          "KeePassXC Format300 fixture is recognized as KDBX 3");
    check(kdbx4.recognized && kdbx4.majorVersion == 4,
          "KeePassXC Format400 fixture is recognized as KDBX 4");
    check(protectedStrings.recognized,
          "KeePassXC protected-string fixture has a recognized KDBX header");

    if (failures == 0)
        qInfo() << "Vault contract: all checks passed";
    return failures == 0 ? 0 : 1;
}
