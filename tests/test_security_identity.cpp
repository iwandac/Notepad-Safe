#include "SecurityIdentity.h"
#include "SecuritySessionService.h"
#include "SecureDocumentService.h"
#include "VaultBackendFactory.h"
#include "VaultService.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QDebug>

#include <cstdio>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << "FAIL:" << message;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

SecureBuffer secret(const char *text)
{
    return SecureBuffer(text, qsizetype(strlen(text)));
}

void testMasterPasswordRotation()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "rotation temporary directory is available");
    if (!temporary.isValid())
        return;

    SecuritySessionService session(temporary.path());
    auto oldPassphrase = secret("old correct horse battery staple");
    auto wrongPassphrase = secret("wrong current password");
    auto newPassphrase = secret("new correct horse battery staple");
    QString recoveryKey;
    check(!session.createIdentity(oldPassphrase, &recoveryKey).isError(),
          "rotation fixture creates v1 identity");
    const SecurityIdentityParameters oldParameters = session.parameters();

    SecurityOperationLease oldLease;
    check(!session.acquireOperationLease(&oldLease).isError(),
          "rotation fixture acquires old operation lease");
    SecureBuffer oldDocumentCredential;
    QVariantMap oldEnvelope;
    check(!SecurityIdentity::createForIdentity(
               oldParameters, oldLease.masterUnlockKey(), oldLease.recoverySecret(),
               &oldDocumentCredential, &oldEnvelope).isError(),
          "old keyVersion document envelope is created");
    oldLease.clear();

    check(session.changeMasterPassword(wrongPassphrase, newPassphrase).code
              == VaultErrorCode::InvalidCredentials,
          "wrong current password cannot rotate the identity");
    check(session.keyVersion() == oldParameters.keyVersion,
          "failed rotation leaves keyVersion unchanged");

    check(!session.changeMasterPassword(oldPassphrase, newPassphrase).isError(),
          "current password rotates the identity");
    check(session.keyVersion() == oldParameters.keyVersion + 1,
          "successful rotation increments keyVersion");
    check(session.identityId() == oldParameters.identityId,
          "rotation preserves the logical identity");

    SecureBuffer rememberedOldMaster;
    check(!session.acquireRememberedMasterKey(
               oldParameters, &rememberedOldMaster).isError(),
          "old master key remains in the sealed session ring");
    SecureBuffer reopenedOldCredential;
    check(!SecurityIdentity::unlockWithMasterKey(
               oldEnvelope, rememberedOldMaster, &reopenedOldCredential).isError()
              && reopenedOldCredential.equals(oldDocumentCredential.data(),
                                               oldDocumentCredential.size()),
          "old keyVersion document remains silently unlockable");
    rememberedOldMaster.clear();
    reopenedOldCredential.clear();

    SecurityOperationLease newLease;
    check(!session.acquireOperationLease(&newLease).isError(),
          "rotated identity yields a new operation lease");
    SecureBuffer newDocumentCredential;
    QVariantMap newEnvelope;
    check(!SecurityIdentity::createForIdentity(
               session.parameters(), newLease.masterUnlockKey(),
               newLease.recoverySecret(), &newDocumentCredential,
               &newEnvelope).isError(),
          "new files bind to the incremented keyVersion");
    newLease.clear();
    SecureBuffer unlockedNewCredential;
    check(!SecurityIdentity::unlock(newEnvelope, newPassphrase,
                                    &unlockedNewCredential).isError()
              && unlockedNewCredential.equals(newDocumentCredential.data(),
                                               newDocumentCredential.size()),
          "new password unlocks new keyVersion files");
    unlockedNewCredential.clear();
    check(SecurityIdentity::unlock(newEnvelope, oldPassphrase,
                                   &unlockedNewCredential).code
              == VaultErrorCode::InvalidCredentials,
          "old password does not unlock new keyVersion files");

    session.endSession(false);
    SecuritySessionService restarted(temporary.path());
    check(restarted.hasActiveSession()
              && restarted.keyVersion() == oldParameters.keyVersion + 1,
          "rotated seven-day session survives process restart");
    check(!restarted.acquireRememberedMasterKey(
               oldParameters, &rememberedOldMaster).isError(),
          "historical key ring survives process restart");
    rememberedOldMaster.clear();
    restarted.endSession(true);
    check(restarted.unlockIdentity(oldPassphrase).code
              == VaultErrorCode::InvalidCredentials,
          "local identity rejects the previous password after rotation");
    check(!restarted.unlockIdentity(newPassphrase).isError(),
          "local identity accepts the replacement password after rotation");
    SecureBuffer identityRecovery;
    check(!restarted.acquireRememberedRecoverySecret(
               oldParameters, &identityRecovery).isError(),
          "fresh new-password session reuses the identity recovery seed for old versions");
    check(!SecurityIdentity::recoverWithSecret(
               oldEnvelope, identityRecovery, &reopenedOldCredential).isError()
              && reopenedOldCredential.equals(oldDocumentCredential.data(),
                                               oldDocumentCredential.size()),
          "old document opens silently after explicit lock and new-password login");
    identityRecovery.clear();
    reopenedOldCredential.clear();
    restarted.endSession(true);

    oldDocumentCredential.clear();
    newDocumentCredential.clear();
    oldPassphrase.clear();
    wrongPassphrase.clear();
    newPassphrase.clear();
    recoveryKey.fill(QChar(0));
}

void testRecoveryVerification()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "recovery verification temporary directory is available");
    if (!temporary.isValid())
        return;

    SecuritySessionService session(temporary.path());
    auto passphrase = secret("correct horse battery staple");
    QString recoveryKey;
    check(!session.createIdentity(passphrase, &recoveryKey).isError(),
          "recovery verification fixture creates an identity");
    check(!session.recoveryVerifiedAtUtc().isValid(),
          "new identity does not claim an independently verified backup");

    QByteArray wrongBytes(32, '\x5a');
    SecureBuffer wrongRecovery(wrongBytes.constData(), wrongBytes.size());
    wrongBytes.fill(0);
    check(session.verifyRecoverySecret(wrongRecovery).code
              == VaultErrorCode::InvalidRecoveryKey,
          "a different well-formed recovery secret is rejected");
    check(!session.recoveryVerifiedAtUtc().isValid(),
          "failed verification does not write a verification time");
    wrongRecovery.clear();

    SecureBuffer recoverySecret;
    check(!SecurityIdentity::parseRecoveryKey(recoveryKey, &recoverySecret).isError(),
          "the printed recovery key parses for verification");
    check(!session.verifyRecoverySecret(recoverySecret).isError(),
          "the identity recovery secret verifies");
    const QDateTime verifiedAt = session.recoveryVerifiedAtUtc();
    check(verifiedAt.isValid(),
          "successful verification records a non-secret timestamp");
    recoverySecret.clear();

    SecuritySessionService restarted(temporary.path());
    check(restarted.recoveryVerifiedAtUtc() == verifiedAt,
          "the verification timestamp survives descriptor reload");

    passphrase.clear();
    recoveryKey.fill(QChar(0));
    recoveryKey.clear();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--master-password-rotation"))) {
        testMasterPasswordRotation();
        if (failures == 0)
            qInfo() << "Master password rotation: all checks passed";
        return failures == 0 ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("--recovery-verification"))) {
        testRecoveryVerification();
        if (failures == 0)
            qInfo() << "Recovery verification: all checks passed";
        return failures == 0 ? 0 : 1;
    }
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory is available");
    if (!temporary.isValid())
        return 1;

    const QString kdbxPath = temporary.filePath(QStringLiteral("default-secure-store.kdbx"));
    auto passphrase = secret("correct horse battery staple");
    SecureBuffer databaseCredential;
    QString recoveryKey;
    QVariantMap publicCustomData;
    publicCustomData.insert(QStringLiteral("Future.Field"), QByteArray("preserved"));
    const VaultError created = SecurityIdentity::create(
        passphrase, &databaseCredential, &recoveryKey, &publicCustomData);
    check(!created.isError(), "security identity is created");
    check(databaseCredential.size() == 32, "database credential is a random 256-bit value");
    check(recoveryKey.startsWith(QStringLiteral("NSR1-")), "recovery key is versioned");
    check(SecurityIdentity::hasEnvelope(publicCustomData),
          "key slots are present in KDBX public custom data");
    check(publicCustomData.value(QStringLiteral("Future.Field")).toByteArray() == "preserved",
          "unknown public data survives identity creation");

    SecureBuffer unlockedCredential;
    check(!SecurityIdentity::unlock(publicCustomData, passphrase, &unlockedCredential).isError(),
          "correct passphrase unlocks random database credential");
    check(unlockedCredential.equals(databaseCredential.data(), databaseCredential.size()),
          "password slot unwraps the generated credential");

    auto wrongPassphrase = secret("wrong passphrase");
    SecureBuffer wrongOutput;
    check(SecurityIdentity::unlock(publicCustomData, wrongPassphrase, &wrongOutput).code
              == VaultErrorCode::InvalidCredentials,
          "wrong passphrase is rejected by authenticated envelope");
    check(wrongOutput.empty(), "failed unlock exposes no credential");

    SecureBuffer recoveredCredential;
    check(!SecurityIdentity::recover(publicCustomData, recoveryKey, &recoveredCredential).isError(),
          "recovery slot works without AppData or a sidecar");
    check(recoveredCredential.equals(databaseCredential.data(), databaseCredential.size()),
          "recovery slot restores the original database credential");
    const QByteArray recoveryText = recoveryKey.toLatin1();
    check(!databaseCredential.equals(recoveryText.constData(), recoveryText.size()),
          "recovery text is independent from the KDBX database password");

    QString damagedRecovery = recoveryKey;
    damagedRecovery[damagedRecovery.size() - 1] =
        damagedRecovery.endsWith(QLatin1Char('0')) ? QLatin1Char('1') : QLatin1Char('0');
    SecureBuffer damagedOutput;
    check(SecurityIdentity::recover(publicCustomData, damagedRecovery, &damagedOutput).code
              == VaultErrorCode::InvalidRecoveryKey,
          "recovery checksum detects transcription damage");

    VaultService service(createProductionVaultBackend());
    const VaultError storeCreated = service.create(kdbxPath, databaseCredential, {},
                                                   publicCustomData);
    if (storeCreated.isError())
        qCritical().noquote() << "CREATE ERROR:" << storeCreated.userMessage
                              << storeCreated.diagnosticCode;
    check(!storeCreated.isError(),
          "credential and key slots create one self-contained KDBX 4 file");
    check(QFile::exists(kdbxPath), "KDBX file exists");
    check(!QFile::exists(kdbxPath + QStringLiteral(".nsk")), "no key sidecar is created");

    QVariantMap roundTripData;
    check(!service.readPublicCustomData(kdbxPath, &roundTripData).isError(),
          "public key slots are readable before KDBX unlock");
    check(SecurityIdentity::hasEnvelope(roundTripData),
          "key slots survive the KDBX writer-reader round trip");
    check(roundTripData.value(QStringLiteral("Future.Field")).toByteArray() == "preserved",
          "unknown KDBX public data survives the round trip");

    service.lock();
    check(!service.unlock(kdbxPath, unlockedCredential).isError(),
          "password-unwrapped credential opens the KDBX");
    service.lock();
    check(!service.unlock(kdbxPath, recoveredCredential).isError(),
          "recovery-unwrapped credential opens the same KDBX");
    service.lock();

    QVariantMap rebuiltData;
    auto replacementPassphrase = secret("replacement passphrase");
    check(!SecurityIdentity::rebuildPasswordSlot(roundTripData, replacementPassphrase,
                                                 recoveredCredential, &rebuiltData).isError(),
          "recovery credential can rebuild the password slot");
    check(rebuiltData.value(QStringLiteral("Future.Field")).toByteArray() == "preserved",
          "password-slot rebuild preserves unknown public data");
    SecureBuffer afterRebuild;
    check(!SecurityIdentity::unlock(rebuiltData, replacementPassphrase, &afterRebuild).isError(),
          "rebuilt password slot unlocks normally");
    check(afterRebuild.equals(databaseCredential.data(), databaseCredential.size()),
          "rebuilt slot still unwraps the original database credential");

    QVariantMap damagedData = roundTripData;
    damagedData.insert(SecurityIdentity::publicDataKey(), QByteArray("{damaged"));
    SecureBuffer afterDamage;
    check(SecurityIdentity::unlock(damagedData, passphrase, &afterDamage).code
              == VaultErrorCode::CorruptKeyEnvelope,
          "corrupt embedded key data is distinguished from a wrong password");

    // A seven-day session keeps only a DPAPI capsule in steady state.  Plain
    // keys exist in a move-only lease around one operation and are wiped when
    // that lease is cleared or destroyed.
    static_assert(!std::is_copy_constructible_v<SecurityOperationLease>);
    static_assert(!std::is_copy_assignable_v<SecurityOperationLease>);
    const QString sessionDir = temporary.filePath(QStringLiteral("session-data"));
    SecuritySessionService session(sessionDir);
    auto sessionPassphrase = secret("correct horse battery staple");
    QString sessionRecoveryKey;
    check(!session.createIdentity(sessionPassphrase, &sessionRecoveryKey).isError(),
          "security session identity is created");
    check(session.hasActiveSession(),
          "new identity establishes a seven-day device session");

    SecurityOperationLease firstLease;
    check(!session.acquireOperationLease(&firstLease).isError() && firstLease.isValid(),
          "active device session yields a short-lived operation lease");
    check(firstLease.masterUnlockKey().size() == 32
              && firstLease.recoverySecret().size() == 32,
          "operation lease owns both 256-bit identity secrets");
    check(SecurityIdentity::recoveryKeyText(firstLease.recoverySecret())
              == sessionRecoveryKey,
          "leased recovery material belongs to the active identity");
    firstLease.clear();
    check(!firstLease.isValid() && firstLease.masterUnlockKey().empty()
              && firstLease.recoverySecret().empty(),
          "clearing a lease immediately releases both plaintext keys");

    QByteArray documentCredentialBytes(32, '\0');
    for (int i = 0; i < documentCredentialBytes.size(); ++i)
        documentCredentialBytes[i] = char(i + 1);
    SecureBuffer documentCredential(documentCredentialBytes.constData(),
                                    documentCredentialBytes.size());
    QByteArray sealedDocumentCredential;
    check(!session.sealDocumentCredential(documentCredential,
                                          &sealedDocumentCredential).isError(),
          "open document credential is DPAPI-sealed for steady-state storage");
    check(!sealedDocumentCredential.contains(documentCredentialBytes),
          "sealed document credential does not contain its plaintext bytes");
    documentCredential.clear();
    SecureBuffer unsealedDocumentCredential;
    check(!session.unsealDocumentCredential(sealedDocumentCredential,
                                            &unsealedDocumentCredential).isError()
              && unsealedDocumentCredential.equals(documentCredentialBytes.constData(),
                                                    documentCredentialBytes.size()),
          "document credential is available only in a caller-owned short lease");
    unsealedDocumentCredential.clear();
    documentCredentialBytes.fill(0);

    session.endSession(false);
    check(!session.hasActiveSession() && QFile::exists(session.capsulePath()),
          "system-lock suspension drops active state but preserves the sealed capsule");
    SecurityOperationLease blockedLease;
    check(session.acquireOperationLease(&blockedLease).code == VaultErrorCode::Locked
              && !blockedLease.isValid(),
          "suspended sessions cannot lease plaintext keys");
    check(session.restoreSealedSession(),
          "same Windows user restores the unexpired session without a password prompt");
    SecurityOperationLease restoredLease;
    check(!session.acquireOperationLease(&restoredLease).isError()
              && restoredLease.isValid(),
          "restored session can lease keys again");
    restoredLease.clear();

    // The sealed capsule is the authority for an active device session. If it
    // disappears (cleanup, profile damage, or external tampering), a stale
    // in-memory active_ flag must not continue to match document identities.
    const SecurityIdentityParameters activeParameters = session.parameters();
    check(session.matches(activeParameters),
          "live device session matches its current identity");
    check(QFile::remove(session.capsulePath()),
          "session capsule can be removed to simulate external loss");
    check(!session.matches(activeParameters) && !session.hasActiveSession(),
          "missing capsule immediately invalidates identity matching and active state");
    SecurityOperationLease missingCapsuleLease;
    check(session.acquireOperationLease(&missingCapsuleLease).code
              == VaultErrorCode::Locked
              && !missingCapsuleLease.isValid(),
          "missing capsule cannot yield an operation lease");

    check(!session.unlockIdentity(sessionPassphrase).isError(),
          "master password can establish a fresh session after capsule loss");

    // Only the exact app-owned drafts/<document-id>.nsafe-draft path is an
    // internal disposable backing file. A user file elsewhere may share the
    // suffix and must remain a normal named document across Save As.
    SecureDocumentService documents(&session,
        temporary.filePath(QStringLiteral("document-data")));
    SecureDocumentSnapshot draft;
    check(!documents.newDraft(&draft).isError(),
          "secure document boundary fixture creates an internal draft");
    const QString externalDraft = temporary.filePath(
        QStringLiteral("user-owned.nsafe-draft"));
    const QString finalDocument = temporary.filePath(
        QStringLiteral("saved-as.nsafe"));
    check(!documents.save(draft.documentId, QStringLiteral("first body"),
                          QStringLiteral("User file"), externalDraft).isError()
              && QFileInfo::exists(externalDraft),
          "a user-selected .nsafe-draft path is saved as a named document");
    check(!documents.save(draft.documentId, QStringLiteral("second body"),
                          QStringLiteral("Saved As"), finalDocument).isError(),
          "external same-suffix document can be saved elsewhere");
    check(QFileInfo::exists(externalDraft),
          "Save As never deletes an external file merely because of its suffix");
    documents.closeAll();

    session.endSession(true);
    check(!QFile::exists(session.capsulePath()),
          "explicit lock removes the seven-day device capsule");
    sessionRecoveryKey.fill(QChar(0));

    recoveryKey.fill(QChar(0));
    if (failures == 0)
        qInfo() << "Security identity: all checks passed";
    return failures == 0 ? 0 : 1;
}
