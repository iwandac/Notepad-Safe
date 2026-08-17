#include "KeePassXCVaultBackend.h"
#include "VaultService.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstring>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

QString fixture(const char *name)
{
    return QString::fromUtf8(NOTEPADSAFE_KDBX_FIXTURE_DIR) + QLatin1Char('/') +
           QString::fromLatin1(name);
}

SecureBuffer password(const char *text)
{
    return SecureBuffer(text, qsizetype(strlen(text)));
}

void verifyUnlock(const char *name, const char *masterPassword)
{
    VaultService service(std::make_unique<KeePassXCVaultBackend>());
    auto secret = password(masterPassword);
    const VaultError result = service.unlock(fixture(name), secret);
    if (result.isError()) {
        std::fprintf(stderr, "UNLOCK %s: code=%d diagnostic=%s\n", name,
                     int(result.code), result.diagnosticCode.toUtf8().constData());
    }
    check(!result.isError(), name);
    if (!result.isError()) {
        VaultError listError;
        service.listEntries(&listError);
        check(!listError.isError(), "unlocked fixture enumerates entries");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    verifyUnlock("Format300.kdbx", "a");
    verifyUnlock("Format400.kdbx", "t");
    verifyUnlock("ProtectedStrings.kdbx", "masterpw");

    {
        VaultService service(std::make_unique<KeePassXCVaultBackend>());
        auto wrong = password("definitely-wrong");
        const VaultError result = service.unlock(fixture("Format400.kdbx"), wrong);
        check(result.code == VaultErrorCode::InvalidCredentials,
              "wrong password is classified as invalid credentials");
    }

    QTemporaryDir temporaryDir;
    check(temporaryDir.isValid(), "temporary directory is available");

    const QString newlyCreated = temporaryDir.filePath(QStringLiteral("created.kdbx"));
    {
        VaultService service(std::make_unique<KeePassXCVaultBackend>());
        auto master = password("new-master-password");
        const VaultError createResult = service.create(newlyCreated, master);
        if (createResult.isError()) {
            std::fprintf(stderr, "CREATE: code=%d diagnostic=%s\n", int(createResult.code),
                         createResult.diagnosticCode.toUtf8().constData());
        }
        check(!createResult.isError(), "new Argon2id/AES-256 KDBX database is created");
        check(QFile::exists(newlyCreated), "new KDBX file exists after atomic creation");
        check(service.state() == VaultState::Open, "new KDBX remains open after creation");
    }
    {
        VaultService service(std::make_unique<KeePassXCVaultBackend>());
        auto master = password("new-master-password");
        check(!service.unlock(newlyCreated, master).isError(),
              "newly created KDBX reopens with its master password");
        auto wrong = password("wrong-new-password");
        check(service.unlock(newlyCreated, wrong).code == VaultErrorCode::InvalidCredentials,
              "newly created KDBX rejects a wrong password");
    }

    const QString writableCopy = temporaryDir.filePath(QStringLiteral("roundtrip.kdbx"));
    check(QFile::copy(fixture("Format400.kdbx"), writableCopy),
          "fixture copied for non-destructive roundtrip");

    QUuid insertedId;
    {
        VaultService service(std::make_unique<KeePassXCVaultBackend>());
        auto master = password("t");
        check(!service.unlock(writableCopy, master).isError(), "roundtrip copy unlocks");

        VaultEntryData entry;
        entry.id = QUuid::createUuid();
        insertedId = entry.id;
        entry.title = QStringLiteral("NotepadSafe roundtrip");
        entry.username = QStringLiteral("alice@example.test");
        entry.url = QStringLiteral("https://example.test");
        entry.tags = {QStringLiteral("integration"), QStringLiteral("notepadsafe/secure-note")};
        entry.passwordUtf8 = password("fixture-entry-password");
        entry.notesUtf8 = password("encrypted note body");
        entry.totpSeedUtf8 = password("JBSWY3DPEHPK3PXP");
        check(!service.upsertEntry(std::move(entry)).isError(), "entry inserted");
        const VaultError saveResult = service.save();
        if (saveResult.isError()) {
            std::fprintf(stderr, "SAVE: code=%d diagnostic=%s\n", int(saveResult.code),
                         saveResult.diagnosticCode.toUtf8().constData());
        }
        check(!saveResult.isError(), "modified database saves atomically");

        VaultEntryData updatedEntry;
        updatedEntry.id = insertedId;
        updatedEntry.title = QStringLiteral("NotepadSafe roundtrip");
        updatedEntry.username = QStringLiteral("alice@example.test");
        updatedEntry.url = QStringLiteral("https://example.test");
        updatedEntry.tags = {QStringLiteral("integration"),
                             QStringLiteral("notepadsafe/secure-note")};
        updatedEntry.passwordUtf8 = password("updated-entry-password");
        updatedEntry.notesUtf8 = password("encrypted note body");
        updatedEntry.totpSeedUtf8 = password("JBSWY3DPEHPK3PXP");
        check(!service.upsertEntry(std::move(updatedEntry)).isError(),
              "existing entry update succeeds");
        VaultError historyError;
        const auto history = service.entryHistory(insertedId, &historyError);
        check(!historyError.isError(), "entry history is readable");
        check(history.size() == 1, "existing entry update creates one history version");
        if (!history.isEmpty()) {
            check(!service.restoreEntryHistory(insertedId, history.constFirst().index).isError(),
                  "entry history version restores");
            VaultEntrySecrets restoredSecrets;
            check(!service.readEntrySecrets(insertedId, &restoredSecrets).isError(),
                  "restored entry secrets are readable");
            check(restoredSecrets.passwordUtf8.equals("fixture-entry-password", 22),
                  "history restore recovers the previous password");
            restoredSecrets.passwordUtf8.clear();
            restoredSecrets.totpSeedUtf8.clear();
        }
        check(!service.save().isError(), "restored history state saves atomically");
    }

    const QDir backupDirectory(writableCopy + QStringLiteral(".backups"));
    const QStringList encryptedSnapshots = backupDirectory.entryList(
        {QStringLiteral("*.kdbx")}, QDir::Files, QDir::Name);
    check(encryptedSnapshots.size() == 2,
          "each save creates one encrypted pre-replacement snapshot");
    if (!encryptedSnapshots.isEmpty()) {
        VaultService snapshotService(std::make_unique<KeePassXCVaultBackend>());
        auto master = password("t");
        check(!snapshotService.unlock(backupDirectory.filePath(encryptedSnapshots.constFirst()),
                                      master).isError(),
              "encrypted snapshot is independently unlockable");
    }

    {
        VaultService service(std::make_unique<KeePassXCVaultBackend>());
        auto master = password("t");
        check(!service.unlock(writableCopy, master).isError(), "saved database reopens");
        VaultError readError;
        auto entry = service.readEntry(insertedId, &readError);
        check(entry && !readError.isError(), "saved entry is found by UUID");
        if (entry) {
            check(entry->passwordUtf8.equals("fixture-entry-password", 22),
                  "password survives encrypted roundtrip");
            check(entry->notesUtf8.equals("encrypted note body", 19),
                  "secure note survives encrypted roundtrip");
            check(entry->totpSeedUtf8.equals("JBSWY3DPEHPK3PXP", 16),
                  "TOTP seed survives encrypted roundtrip");
        }
        auto metadata = service.readEntryMetadata(insertedId, &readError);
        check(metadata && !readError.isError(), "entry metadata is readable");
        if (metadata) {
            check(metadata->hasPassword && metadata->hasTotp,
                  "metadata reports secret presence without returning values");
            check(metadata->passwordUtf8.empty() && metadata->totpSeedUtf8.empty(),
                  "metadata does not retain password or TOTP seed");
        }
        VaultEntrySecrets secrets;
        check(!service.readEntrySecrets(insertedId, &secrets).isError(),
              "short-lived entry secrets are readable on demand");
        check(secrets.passwordUtf8.equals("fixture-entry-password", 22),
              "secret lease returns the password");
        check(secrets.totpSeedUtf8.equals("JBSWY3DPEHPK3PXP", 16),
              "secret lease returns the TOTP seed");
        secrets.passwordUtf8.clear();
        secrets.totpSeedUtf8.clear();
    }

    if (failures == 0)
        qInfo() << "KDBX backend: all checks passed";
    return failures == 0 ? 0 : 1;
}
