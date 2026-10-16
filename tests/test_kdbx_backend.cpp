#include "KeePassXCVaultBackend.h"
#include "VaultService.h"
#include "streams/SymmetricCipherStream.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstring>
#include <cstdio>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

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

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "retention fixture opens for byte comparison");
    return file.isOpen() ? file.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    const bool written = opened && file.write(bytes) == bytes.size() && file.flush();
    check(written, "retention fixture bytes are written");
    return written;
}

QString previousBackupPath(const QString &path)
{
    return QDir(path + QStringLiteral(".backups")).filePath(
        QFileInfo(path).completeBaseName() + QStringLiteral(".previous.kdbx"));
}

bool stageVersion(VaultService &service, const QUuid &entryId, const QString &title)
{
    VaultEntryData entry;
    entry.id = entryId;
    entry.title = title;
    entry.notesUtf8 = password("retention-test-secret");
    const VaultError result = service.upsertEntry(std::move(entry));
    check(!result.isError(), "retention version is staged through the service");
    return !result.isError();
}

void checkNoSaveCandidate(const QString &path)
{
    check(QFileInfo(path).dir().entryList({QStringLiteral(".notepadsafe-*.kdbx")},
                                        QDir::Files | QDir::Hidden).isEmpty(),
          "save candidates are removed after success or failure");
}

#ifdef Q_OS_WIN
class ReadOnlyLease final {
public:
    explicit ReadOnlyLease(const QString &path)
        : handle_(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ReadOnlyLease() { release(); }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    void release()
    {
        if (valid())
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
private:
    Q_DISABLE_COPY(ReadOnlyLease)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

void verifySingleBackupRetention(QTemporaryDir &temporaryDir)
{
    // Dots in the basename also exercise escaping in legacy-name matching.
    const QString path = temporaryDir.filePath(QStringLiteral("retention.with.dots.kdbx"));
    if (!QFile::copy(fixture("Format400.kdbx"), path)) {
        check(false, "retention fixture is copied");
        return;
    }
    VaultService service(std::make_unique<KeePassXCVaultBackend>());
    auto master = password("t");
    if (service.unlock(path, master).isError()) {
        check(false, "retention fixture unlocks");
        return;
    }
    const QUuid entryId = QUuid::createUuid();
    const QDir backupDirectory(path + QStringLiteral(".backups"));
    const QString backupPath = previousBackupPath(path);
    const QString backupName = QFileInfo(backupPath).fileName();
    for (int generation = 0; generation < 3; ++generation) {
        const QByteArray beforeSave = readBytes(path);
        if (!stageVersion(service, entryId, QStringLiteral("generation-%1").arg(generation)))
            return;
        const VaultError result = service.save();
        check(!result.isError(), "three consecutive distinct versions save successfully");
        if (result.isError())
            return;
        check(backupDirectory.entryList(QDir::Files) == QStringList{backupName},
              "every successful save retains exactly one fixed-name backup");
        check(readBytes(backupPath) == beforeSave,
              "retained backup is byte-exactly the immediately preceding original");
        check(readBytes(path) != beforeSave, "successful save replaces the original bytes");
        VaultService previous(std::make_unique<KeePassXCVaultBackend>());
        check(!previous.unlock(backupPath, master).isError(),
              "each retained backup independently unlocks");
        VaultError readError;
        auto entry = previous.readEntry(entryId, &readError);
        if (generation == 0) {
            check(!entry, "first backup predates the inserted test entry");
        } else {
            check(entry && !readError.isError()
                      && entry->title == QStringLiteral("generation-%1").arg(generation - 1),
                  "independent backup contains the previous generation, not current data");
        }
        checkNoSaveCandidate(path);
    }

    QStringList legacyNames = {
        QStringLiteral("retention.with.dots.20260101-120000-000.kdbx"),
        QStringLiteral("retention.with.dots.20260101-120000-001.kdbx"),
        QStringLiteral("retention.with.dots.20260102-120000-000.kdbx")};
#ifdef Q_OS_WIN
    legacyNames.append(QStringLiteral("Retention.With.Dots.20260102-120000-001.KDBX"));
#endif
    for (const QString &name : legacyNames)
        check(QFile::copy(path, backupDirectory.filePath(name)), "legacy snapshot is seeded");
    const QStringList unrelatedNames = {
        QStringLiteral("manual.kdbx"),
        QStringLiteral("other.20260101-120000-000.kdbx"),
        QStringLiteral("retentionXwithXdots.20260101-120000-000.kdbx"),
        QStringLiteral("retention.with.dots.20260101-120000-00.kdbx"),
        QStringLiteral("retention.with.dots.20261301-120000-000.kdbx"),
        QStringLiteral("retention.with.dots.20260101-120000-000.copy.kdbx"),
        QStringLiteral("retention.with.dots.20260101-120000-000.txt")};
    const QByteArray unrelatedBytes("unrelated-user-file\0unchanged", 29);
    for (const QString &name : unrelatedNames)
        writeBytes(backupDirectory.filePath(name), unrelatedBytes);
    const QString directoryPath = backupDirectory.filePath(
        QStringLiteral("retention.with.dots.20260103-120000-000.kdbx"));
    check(QDir().mkpath(directoryPath), "matching-name directory is seeded");
    const QString childPath = QDir(directoryPath).filePath(QStringLiteral("keep.txt"));
    writeBytes(childPath, unrelatedBytes);

    const QByteArray beforeMigration = readBytes(path);
    if (!stageVersion(service, entryId, QStringLiteral("after-migration")))
        return;
    check(!service.save().isError(), "save migrates pre-existing timestamp snapshots");
    check(readBytes(backupPath) == beforeMigration,
          "migration retains the immediately preceding original");
    for (const QString &name : legacyNames)
        check(!QFileInfo::exists(backupDirectory.filePath(name)),
              "strictly matching legacy snapshots are removed");
    for (const QString &name : unrelatedNames)
        check(readBytes(backupDirectory.filePath(name)) == unrelatedBytes,
              "cleanup preserves unrelated and near-matching file bytes");
    check(QFileInfo(directoryPath).isDir() && readBytes(childPath) == unrelatedBytes,
          "cleanup never recursively deletes a matching-name directory");
    check(backupDirectory.entryList(QDir::Files).size() == unrelatedNames.size() + 1,
          "migration leaves one managed backup plus every unrelated file");

#ifdef Q_OS_WIN
    // Share-read leases permit integrity verification while blocking replacement
    // or deletion. Each failure must remain retryable through the SAME service.
    for (int fault = 0; fault < 3; ++fault) {
        const QString legacyPath = backupDirectory.filePath(
            QStringLiteral("retention.with.dots.20260104-120000-000.kdbx"));
        if (fault == 1)
            check(QFile::copy(path, legacyPath), "locked legacy snapshot is seeded");
        const QString blockedPath = fault == 0 ? backupPath : fault == 1 ? legacyPath : path;
        const QByteArray originalBefore = readBytes(path);
        const QByteArray backupBefore = readBytes(backupPath);
        if (!stageVersion(service, entryId, QStringLiteral("retry-%1").arg(fault)))
            return;
        ReadOnlyLease lease(blockedPath);
        check(lease.valid(), "filesystem failure lease is acquired");
        if (!lease.valid())
            return;
        const VaultError failed = service.save();
        check(failed.code == VaultErrorCode::IoError,
              "blocked backup write, legacy cleanup or original commit reports IO failure");
        const QString expectedDiagnostic = fault == 0 ? QStringLiteral("vault.backup.write:")
            : fault == 1 ? QStringLiteral("vault.backup.prune:")
                         : QStringLiteral("vault.save.commit:");
        check(failed.diagnosticCode.startsWith(expectedDiagnostic),
              "filesystem lease reaches the intended failure boundary");
        check(readBytes(path) == originalBefore,
              "every injected pre-commit failure preserves the original bytes");
        if (fault == 0) {
            check(readBytes(backupPath) == backupBefore,
                  "failed backup replacement preserves the existing backup bytes");
        } else {
            check(readBytes(backupPath) == originalBefore,
                  "cleanup or commit failure may advance backup to the intact current original");
        }
        if (fault == 1)
            check(readBytes(legacyPath) == originalBefore,
                  "failed legacy cleanup preserves the locked snapshot");
        checkNoSaveCandidate(path);
        lease.release();
        check(!service.save().isError(),
              "same service retries successfully after each filesystem lease is released");
        check(readBytes(backupPath) == originalBefore,
              "successful retry retains the correct pre-save original");
        check(readBytes(path) != originalBefore, "successful retry publishes staged data");
        check(!QFileInfo::exists(legacyPath), "successful retry completes legacy cleanup");
        check(backupDirectory.entryList(QDir::Files).size() == unrelatedNames.size() + 1,
              "successful retry converges to one managed backup");
        checkNoSaveCandidate(path);
    }
#endif
}

void verifyCorruptOriginalPreservesBackup(QTemporaryDir &temporaryDir)
{
    const QString path = temporaryDir.filePath(QStringLiteral("corrupt-original.kdbx"));
    check(QFile::copy(fixture("Format400.kdbx"), path), "corruption fixture is copied");
    const QString backupPath = previousBackupPath(path);
    check(QDir().mkpath(QFileInfo(backupPath).absolutePath()), "corruption backup directory exists");
    check(QFile::copy(path, backupPath), "corruption fixture has an existing backup");
    const QByteArray backupBefore = readBytes(backupPath);
    KeePassXCVaultBackend backend;
    auto master = password("t");
    // Bypass the service's external-file-change guard to exercise the backend's
    // current-file verification after a successful unlock.
    check(!backend.unlock(path, master, {}).isError(), "corruption fixture unlocks before damage");
    const QByteArray damaged("damaged original database");
    if (!writeBytes(path, damaged))
        return;
    const VaultError result = backend.save();
    check(result.code == VaultErrorCode::CorruptDatabase,
          "backend rejects an original corrupted after unlock");
    check(result.diagnosticCode.startsWith(QStringLiteral("vault.save.current-verify:")),
          "corruption reaches current-file verification rather than an unrelated failure");
    check(readBytes(path) == damaged, "corrupt original is not overwritten");
    check(readBytes(backupPath) == backupBefore,
          "current-file verification fails before changing the existing backup");
    checkNoSaveCandidate(path);
}

void verifyDisposableDatabaseDoesNotRetainCopies(QTemporaryDir &temporaryDir)
{
    const QString path = temporaryDir.filePath(QStringLiteral("disposable.kdbx"));
    check(QFile::copy(fixture("Format400.kdbx"), path), "private staging fixture is copied");
    VaultService service(std::make_unique<KeePassXCVaultBackend>(false));
    auto master = password("t");
    check(!service.unlock(path, master).isError(), "private staging fixture unlocks");
    const QUuid entryId = QUuid::createUuid();
    for (int generation = 0; generation < 2; ++generation) {
        if (!stageVersion(service, entryId, QStringLiteral("private-%1").arg(generation)))
            return;
        check(!service.save().isError(), "private disposable database saves repeatedly");
        check(!QFileInfo::exists(path + QStringLiteral(".backups")),
              "private disposable saves never create a backup directory");
        checkNoSaveCandidate(path);
    }
    service.lock();
    check(!service.unlock(path, master).isError(), "private staging result independently reopens");
    VaultError readError;
    auto entry = service.readEntry(entryId, &readError);
    check(entry && !readError.isError() && entry->title == QStringLiteral("private-1"),
          "no-backup policy still persists the latest staging contents");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    {
        // KDBX4 encrypts through a 1024-byte stream buffer. An exact multiple
        // must reset without attempting to encrypt a zero-length tail block.
        QBuffer encrypted;
        check(encrypted.open(QIODevice::WriteOnly),
              "stream-cipher regression buffer opens");
        SymmetricCipherStream stream(&encrypted);
        check(stream.init(SymmetricCipher::ChaCha20,
                          SymmetricCipher::Encrypt,
                          QByteArray(32, '\x11'), QByteArray(12, '\x22')),
              "stream-cipher regression fixture initializes");
        check(stream.open(QIODevice::WriteOnly),
              "stream-cipher regression stream opens");
        check(stream.write(QByteArray(1024, 'x')) == 1024,
              "stream-cipher exact block is written");
        check(stream.reset(),
              "stream-cipher exact block resets without an empty tail");
        check(encrypted.buffer().size() == 1024,
              "stream-cipher exact block preserves output size");
    }

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
        check(!QFileInfo::exists(newlyCreated + QStringLiteral(".backups")),
              "first database creation has no previous version to retain");
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
        entry.synchronizeExtendedFields = true;
        entry.groupPath = QStringLiteral("Work/Production");
        entry.additionalUrls = {QStringLiteral("https://backup.example.test")};
        VaultCustomField protectedField;
        protectedField.name = QStringLiteral("API token");
        protectedField.valueUtf8 = password("custom-secret");
        protectedField.protectedValue = true;
        entry.customFields.push_back(std::move(protectedField));
        VaultCustomField publicField;
        publicField.name = QStringLiteral("Region");
        publicField.valueUtf8 = password("us-west");
        publicField.protectedValue = false;
        entry.customFields.push_back(std::move(publicField));
        entry.expires = true;
        entry.expiryUtc = QDateTime::currentDateTimeUtc().addDays(30);
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
        updatedEntry.synchronizeExtendedFields = true;
        updatedEntry.groupPath = QStringLiteral("Work/Production");
        updatedEntry.additionalUrls = {QStringLiteral("https://backup.example.test")};
        VaultCustomField updatedProtectedField;
        updatedProtectedField.name = QStringLiteral("API token");
        updatedProtectedField.valueUtf8 = password("custom-secret");
        updatedProtectedField.protectedValue = true;
        updatedEntry.customFields.push_back(std::move(updatedProtectedField));
        VaultCustomField updatedPublicField;
        updatedPublicField.name = QStringLiteral("Region");
        updatedPublicField.valueUtf8 = password("us-west");
        updatedPublicField.protectedValue = false;
        updatedEntry.customFields.push_back(std::move(updatedPublicField));
        updatedEntry.expires = true;
        updatedEntry.expiryUtc = QDateTime::currentDateTimeUtc().addDays(30);
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
        check(!service.clearEntryHistory(insertedId).isError(),
              "entry history cleanup succeeds");
        const auto clearedHistory = service.entryHistory(insertedId, &historyError);
        check(!historyError.isError() && clearedHistory.isEmpty(),
              "entry history cleanup removes every old version");

        VaultPasswordGenerationOptions generatorOptions;
        generatorOptions.length = 32;
        generatorOptions.special = false;
        generatorOptions.excludeLookAlike = true;
        SecureBuffer generated;
        check(!service.generatePassword(generatorOptions, &generated).isError(),
              "configurable KeePassXC password generator succeeds");
        check(generated.size() == 32, "configurable generator honors length");
        generated.clear();

        const QUuid weakOne = QUuid::createUuid();
        const QUuid weakTwo = QUuid::createUuid();
        for (const QUuid &id : {weakOne, weakTwo}) {
            VaultEntryData weak;
            weak.id = id;
            weak.title = id == weakOne ? QStringLiteral("Weak one")
                                       : QStringLiteral("Weak two");
            weak.passwordUtf8 = password("same");
            check(!service.upsertEntry(std::move(weak)).isError(),
                  "health fixture account inserts");
        }
        VaultError healthError;
        const auto health = service.passwordHealthReport(&healthError);
        check(!healthError.isError(), "offline password health scan succeeds");
        check(health.size() >= 4,
              "health scan reports weak and reused findings for both accounts");
        check(!service.save().isError(), "restored history state saves atomically");
    }

    const QDir backupDirectory(writableCopy + QStringLiteral(".backups"));
    const QStringList encryptedSnapshots = backupDirectory.entryList(
        {QStringLiteral("*.kdbx")}, QDir::Files, QDir::Name);
    check(encryptedSnapshots == QStringList{QStringLiteral("roundtrip.previous.kdbx")},
          "repeated saves retain only the fixed-name previous encrypted version");
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
            const QString totpSettings = QString::fromUtf8(
                reinterpret_cast<const char *>(entry->totpSeedUtf8.data()),
                entry->totpSeedUtf8.size());
            check(totpSettings.startsWith(QStringLiteral("otpauth://"))
                      && totpSettings.contains(QStringLiteral("JBSWY3DPEHPK3PXP")),
                  "complete TOTP settings survive encrypted roundtrip");
            check(entry->groupPath == QStringLiteral("Work/Production"),
                  "nested group survives encrypted roundtrip");
            check(entry->additionalUrls.contains(QStringLiteral("https://backup.example.test")),
                  "additional URL survives encrypted roundtrip");
            check(entry->customFields.size() == 2,
                  "protected and public custom fields survive encrypted roundtrip");
            check(entry->expires && entry->expiryUtc.isValid(),
                  "expiry metadata survives encrypted roundtrip");
        }
        auto metadata = service.readEntryMetadata(insertedId, &readError);
        check(metadata && !readError.isError(), "entry metadata is readable");
        if (metadata) {
            check(metadata->hasPassword && metadata->hasTotp,
                  "metadata reports secret presence without returning values");
            check(metadata->passwordUtf8.empty() && metadata->totpSeedUtf8.empty(),
                  "metadata does not retain password or TOTP seed");
            bool protectedValueLeaked = false;
            bool publicValuePresent = false;
            for (const auto &field : metadata->customFields) {
                if (field.protectedValue && !field.valueUtf8.empty())
                    protectedValueLeaked = true;
                if (!field.protectedValue && !field.valueUtf8.empty())
                    publicValuePresent = true;
            }
            check(!protectedValueLeaked,
                  "metadata does not retain protected custom-field values");
            check(publicValuePresent,
                  "metadata keeps explicitly public custom-field values");
        }
        VaultEntrySecrets secrets;
        check(!service.readEntrySecrets(insertedId, &secrets).isError(),
              "short-lived entry secrets are readable on demand");
        check(secrets.passwordUtf8.equals("fixture-entry-password", 22),
              "secret lease returns the password");
        const QString leasedTotp = QString::fromUtf8(
            reinterpret_cast<const char *>(secrets.totpSeedUtf8.data()),
            secrets.totpSeedUtf8.size());
        check(leasedTotp.startsWith(QStringLiteral("otpauth://"))
                  && leasedTotp.contains(QStringLiteral("JBSWY3DPEHPK3PXP")),
              "secret lease returns complete TOTP settings");
        secrets.passwordUtf8.clear();
        secrets.totpSeedUtf8.clear();
    }

    verifySingleBackupRetention(temporaryDir);
    verifyCorruptOriginalPreservesBackup(temporaryDir);
    verifyDisposableDatabaseDoesNotRetainCopies(temporaryDir);

    if (failures == 0)
        qInfo() << "KDBX backend: all checks passed";
    return failures == 0 ? 0 : 1;
}
