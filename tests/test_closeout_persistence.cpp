#include "SessionManager.h"
#include "DraftStore.h"
#include "PlainFilePersistence.h"
#include "DocumentContentPort.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QtTest>
#include <functional>

namespace {
bool put(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QByteArray get(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
class SessionPort final : public ISessionDocumentPort {
public:
    QUuid id = QUuid::createUuid();
    QString path;
    bool exportOk = true;
    quint64 revision = 1;
    QByteArray restored;
    SessionTab captureSessionTab() const override {
        SessionTab s; s.modified = true; s.autosavePath = path; return s;
    }
    QUuid sessionInstanceId() const override { return id; }
    quint64 sessionRevision() const override { return revision; }
    bool needsRecoverySnapshot() const override { return true; }
    bool isEmptyUntitledDocument() const override { return false; }
    bool isLargeRecoveryDocument() const override { return false; }
    QString recoveryPath() const override { return path; }
    void acceptRecoveryPath(const QString &p) override { path = p; }
    bool captureRecoveryPayload(SessionRecoveryPayload *p, QString *) override {
        p->bytes = QByteArrayLiteral("synthetic recovery body ") + QByteArray::number(revision);
        return exportOk;
    }
    bool restoreSessionRecovery(const SessionRestoreRequest &request, QString *) override {
        restored = request.plaintext; return true;
    }
};
class ContentPort final : public IDocumentContentPort {
public:
    QString path;
    std::function<void()> onSuspend;
    bool accepted = false;
    bool resumed = false;
    DocumentState persistenceState() const noexcept override {
        DocumentState s; s.location = path; s.capabilities = DocumentCapability::Save; return s;
    }
    TextEncoding persistenceEncoding() const noexcept override { return TextEncoding::Utf8; }
    DiskFileVersion persistenceBaseline() const override { return {}; }
    bool createPersistenceSnapshot(qint64, DocumentSnapshot *s, QString *) override {
        s->text = QStringLiteral("local edit"); return true;
    }
    bool suspendPersistenceBacking(QString *) override { if (onSuspend) onSuspend(); return true; }
    bool resumePersistenceBacking(const QString &, QString *) override { resumed = true; return true; }
    void acceptPersistenceCommit(const QString &, DiskFileVersion) override { accepted = true; }
    void acceptSecurePersistenceCommit(const QUuid &, const QString &, const QString &, const QString &) override {}
};
}
class CloseoutPersistence : public QObject {
    Q_OBJECT
private slots:
    void snapshotFailure_data() {
        QTest::addColumn<bool>("existing"); QTest::addColumn<bool>("ioFailure");
        QTest::newRow("first-export") << false << false;
        QTest::newRow("stale-export") << true << false;
        QTest::newRow("first-io") << false << true;
        QTest::newRow("stale-io") << true << true;
    }
    void snapshotFailure() {
        QFETCH(bool, existing); QFETCH(bool, ioFailure);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        SessionManager sm(dir.path()); SessionPort port;
        if (existing) QVERIFY(sm.saveSession({&port}, 0, true));
        const auto manifest = get(sm.sessionFilePath());
        const auto oldPath = port.path;
        const auto retained = sm.generateAutosavePath();
        QVERIFY(put(retained, "unreferenced recovery evidence"));
        ++port.revision;
        port.exportOk = ioFailure;
        const auto plan = sm.captureAsyncSave({&port}, 0, true);
#ifdef Q_OS_WIN
        HANDLE lease = INVALID_HANDLE_VALUE;
        if (ioFailure && existing) {
            lease = CreateFileW(reinterpret_cast<LPCWSTR>(port.path.utf16()), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lease != INVALID_HANDLE_VALUE);
        }
#endif
        if (ioFailure && !existing) {
            QVERIFY(QFile::remove(retained));
            QVERIFY(QDir(sm.autosaveDir()).rmdir("."));
            QVERIFY(put(sm.autosaveDir(), "directory blocker"));
        }
        const auto result = sm.writeAsyncSave(plan);
#ifdef Q_OS_WIN
        if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease);
#endif
        QVERIFY2(!sm.asyncSaveSucceeded(result), "Required body failure must veto close");
        sm.applyAsyncSave(result, {&port});
        QCOMPARE(port.path, oldPath);
        QCOMPARE(get(sm.sessionFilePath()), manifest);
        if (!(ioFailure && !existing))
            QCOMPARE(get(retained), QByteArray("unreferenced recovery evidence"));
        if (existing) {
            QByteArray restored; QString error;
            QVERIFY(DraftStore::readSnapshot(oldPath, &restored, &error));
            QCOMPARE(restored, QByteArray("synthetic recovery body 1"));
        }
        if (ioFailure && !existing) {
            QVERIFY(QFile::remove(sm.autosaveDir()));
            QVERIFY(QDir().mkpath(sm.autosaveDir()));
        }
        port.exportOk = true;
        QVERIFY(sm.saveSession({&port}, 0, true));
        QByteArray restored; QString error;
        QVERIFY(DraftStore::readSnapshot(port.path, &restored, &error));
        QCOMPARE(restored, QByteArray("synthetic recovery body 2"));
    }
    void invalidManifest_data() {
        QTest::addColumn<QByteArray>("manifest");
        QTest::newRow("broken") << QByteArray("{broken");
        QTest::newRow("future") << QByteArray("{\"version\":99,\"tabs\":[]}");
        QTest::newRow("wrong-tabs") << QByteArray("{\"version\":2,\"tabs\":{}}");
        QTest::newRow("wrong-entry") << QByteArray("{\"version\":2,\"tabs\":[false]}");
    }
    void invalidManifest() {
        QFETCH(QByteArray, manifest);
        QTemporaryDir dir; QVERIFY(dir.isValid()); SessionManager sm(dir.path());
        const auto draft = sm.generateAutosavePath();
        QVERIFY(put(draft, "synthetic retained draft"));
        {
            QFile aged(draft); QVERIFY(aged.open(QIODevice::ReadWrite));
            QVERIFY(aged.setFileTime(QDateTime::currentDateTime().addDays(-9), QFileDevice::FileModificationTime));
        }
        QVERIFY(put(sm.sessionFilePath(), manifest));
        sm.loadSession();
        QVERIFY2(!sm.saveSession({}, 0, true), "Invalid manifest must not become an empty successful session");
        QCOMPARE(get(sm.sessionFilePath()), manifest);
        QCOMPARE(get(draft), QByteArray("synthetic retained draft"));
        sm.cleanupOldAutosaves(); sm.clearSession();
        QCOMPARE(get(sm.sessionFilePath()), manifest);
        QCOMPARE(get(draft), QByteArray("synthetic retained draft"));
        SessionPort latest; latest.revision = 2;
        const auto result = sm.writeAsyncSave(sm.captureAsyncSaveEntries({&latest}, 0, true, true));
        QVERIFY(sm.asyncSaveSucceeded(result));
        sm.applyAsyncSave(result, {&latest});
        const QString archive = sm.asyncSavePreservedPath(result);
        QVERIFY(!archive.isEmpty());
        QCOMPARE(get(archive + "/session.json"), manifest);
        const QString preservedDraft = archive + "/autosave/" + QFileInfo(draft).fileName();
        QCOMPARE(get(preservedDraft), QByteArray("synthetic retained draft"));
        QVERIFY(QFileInfo::exists(archive + "/recovery-index.json"));
        SessionManager restarted(dir.path());
        QCOMPARE(restarted.loadSession().loadStatus, SessionData::LoadStatus::Loaded);
        QCOMPARE(restarted.loadSession().tabs.size(), 1);
        SessionPort recovered;
        QVERIFY(restarted.restoreAutosave(&recovered, restarted.loadSession().tabs.first().autosavePath));
        QCOMPARE(recovered.restored, QByteArray("synthetic recovery body 2"));
        // Later successful saves, age cleanup and explicit session clearing
        // must never collect the independent preserved archive.
        QVERIFY(restarted.saveSession({}, 0, true));
        restarted.cleanupOldAutosaves(); restarted.clearSession();
        QCOMPARE(get(archive + "/session.json"), manifest);
        QCOMPARE(get(preservedDraft), QByteArray("synthetic retained draft"));
    }
    void preservationFailure_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("archive-directory-blocked") << 0;
        QTest::newRow("draft-copy-unreadable") << 1;
        QTest::newRow("latest-body-export-failed") << 2;
        QTest::newRow("manifest-commit-blocked") << 3;
    }
    void preservationFailure() {
        QFETCH(int, fault);
        QTemporaryDir dir; QVERIFY(dir.isValid()); SessionManager sm(dir.path());
        SessionPort latest; QVERIFY(sm.saveSession({&latest}, 0, true));
        const QString oldPath = latest.path;
        const auto oldBytes = get(oldPath);
        const QByteArray invalid("{invalid-but-preserved");
        QVERIFY(put(sm.sessionFilePath(), invalid));
        latest.revision = 2;
        latest.exportOk = fault != 2;
        if (fault == 0) QVERIFY(put(sm.preservedRecoveryDir(), "directory blocker"));
#ifdef Q_OS_WIN
        HANDLE lease = INVALID_HANDLE_VALUE;
        if (fault == 1 || fault == 3) {
            const QString target = fault == 1 ? oldPath : sm.sessionFilePath();
            lease = CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()), GENERIC_READ,
                fault == 1 ? 0 : FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lease != INVALID_HANDLE_VALUE);
        }
#endif
        const auto failed = sm.writeAsyncSave(sm.captureAsyncSaveEntries({&latest}, 0, true, true));
#ifdef Q_OS_WIN
        if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease);
#endif
        QVERIFY(!sm.asyncSaveSucceeded(failed));
        sm.applyAsyncSave(failed, {&latest});
        QCOMPARE(latest.path, oldPath);
        QCOMPARE(get(sm.sessionFilePath()), invalid);
        QCOMPARE(get(oldPath), oldBytes);
        if (fault < 2) QVERIFY(!sm.asyncSaveError(failed).isEmpty());
        if (fault == 0) QVERIFY(QFile::remove(sm.preservedRecoveryDir()));
        latest.exportOk = true;
        const auto retried = sm.writeAsyncSave(sm.captureAsyncSaveEntries({&latest}, 0, true, true));
        QVERIFY(sm.asyncSaveSucceeded(retried));
        sm.applyAsyncSave(retried, {&latest});
        const auto archive = sm.asyncSavePreservedPath(retried);
        QCOMPARE(get(archive + "/session.json"), invalid);
        QCOMPARE(get(archive + "/autosave/" + QFileInfo(oldPath).fileName()), oldBytes);
        QCOMPARE(get(oldPath), oldBytes); // no same-path rewrite on salvage
        SessionManager archived(archive); SessionPort recovered;
        QVERIFY(archived.restoreAutosave(&recovered, archive + "/autosave/" + QFileInfo(oldPath).fileName()));
        QCOMPARE(recovered.restored, QByteArray("synthetic recovery body 1"));
        SessionManager restarted(dir.path());
        QVERIFY(restarted.restoreAutosave(&recovered, restarted.loadSession().tabs.first().autosavePath));
        QCOMPARE(recovered.restored, QByteArray("synthetic recovery body 2"));
    }
    void preservationRequiresExplicitForcedChoice() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); SessionManager sm(dir.path());
        QVERIFY(put(sm.sessionFilePath(), "{invalid"));
        SessionPort latest;
        QVERIFY(!sm.asyncSaveSucceeded(sm.writeAsyncSave(sm.captureAsyncSaveEntries({&latest}, 0, false, true))));
        QVERIFY(!QFileInfo::exists(sm.preservedRecoveryDir()));
        QCOMPARE(get(sm.sessionFilePath()), QByteArray("{invalid"));
    }
    void qsavefileHasNoConditionalCommit() {
        // Primitive evidence, not a claim that the product exposes a hook in
        // its last instructions before commit. Schedule the competing rename
        // deterministically between a successful check and QSaveFile commit.
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("race.txt");
        QVERIFY(put(path, "baseline")); DiskFileVersion baseline;
        QCOMPARE(captureDiskFileVersion(path, &baseline), DiskFileState::Unchanged);
        QSaveFile local(path); local.setDirectWriteFallback(false);
        QVERIFY(local.open(QIODevice::WriteOnly)); QCOMPARE(local.write("local"), 5);
        QCOMPARE(compareDiskFileVersion(path, baseline), DiskFileState::Unchanged);
        QSaveFile external(path); external.setDirectWriteFallback(false);
        QVERIFY(external.open(QIODevice::WriteOnly)); QCOMPARE(external.write("external"), 8);
        QVERIFY(external.commit()); QCOMPARE(get(path), QByteArray("external"));
        QVERIFY(local.commit()); QCOMPARE(get(path), QByteArray("local"));
    }
    void windowsSharingLeaseTradeoff() {
#ifndef Q_OS_WIN
        QSKIP("Windows sharing semantics");
#else
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const DWORD modes[] = {FILE_SHARE_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE};
        for (DWORD mode : modes) {
            const auto path = QDir::toNativeSeparators(dir.filePath(QString::number(mode) + ".txt"));
            const auto candidate = QDir::toNativeSeparators(dir.filePath(QString::number(mode) + "-native.txt"));
            QVERIFY(put(path, "baseline")); QVERIFY(put(candidate, "external-native"));
            HANDLE lease = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                mode, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lease != INVALID_HANDLE_VALUE);
            const bool directWrite = put(path, "direct-writer");
            bool candidateReady = false, qtReplaced = false;
            {
                // Destroy the failed QSaveFile before testing another API;
                // its internal handles must not contaminate the next probe.
                QSaveFile local(path); local.setDirectWriteFallback(false);
                candidateReady = local.open(QIODevice::WriteOnly) && local.write("local") == 5;
                qtReplaced = candidateReady && local.commit();
            }
            const bool nativeReplaced = MoveFileExW(
                reinterpret_cast<LPCWSTR>(candidate.utf16()),
                reinterpret_cast<LPCWSTR>(path.utf16()), MOVEFILE_REPLACE_EXISTING);
            const DWORD nativeError = nativeReplaced ? ERROR_SUCCESS : GetLastError();
            CloseHandle(lease);
            qInfo("share=%lu directWrite=%d qtReplace=%d nativeReplace=%d nativeError=%lu",
                  mode, directWrite, qtReplaced, nativeReplaced, nativeError);
            QVERIFY(candidateReady);
            QCOMPARE(directWrite, bool(mode & FILE_SHARE_WRITE));
            if (mode == FILE_SHARE_READ) {
                QVERIFY(!qtReplaced); QVERIFY(!nativeReplaced);
            }
            // Replacement behavior with delete sharing is a platform probe,
            // not an assumed capability. Check data against the actual result.
            QCOMPARE(get(path), nativeReplaced ? QByteArray("external-native")
                : qtReplaced ? QByteArray("local")
                : directWrite ? QByteArray("direct-writer") : QByteArray("baseline"));
            QSaveFile unlocked(path); unlocked.setDirectWriteFallback(false);
            QVERIFY(unlocked.open(QIODevice::WriteOnly)); QCOMPARE(unlocked.write("unlocked"), 8);
            QVERIFY(unlocked.commit()); QCOMPARE(get(path), QByteArray("unlocked"));
        }
#endif
    }
    void externalWriteBeforeCommit() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); ContentPort content;
        content.path = dir.filePath("note.txt"); QVERIFY(put(content.path, "baseline"));
        PersistenceSaveRequest request; request.targetPath = content.path;
        request.requireExpectedVersion = true;
        QCOMPARE(captureDiskFileVersion(content.path, &request.expectedVersion), DiskFileState::Unchanged);
        bool wrote = false;
        content.onSuspend = [&] { wrote = put(content.path, "external writer"); };
        const auto result = PlainFilePersistence().save(&content, request);
        QVERIFY(wrote);
        QCOMPARE(result.status, PersistenceStatus::Conflict);
        QCOMPARE(get(content.path), QByteArray("external writer"));
        QVERIFY(!content.accepted); QVERIFY(content.resumed);
        content.onSuspend = {};
        QCOMPARE(captureDiskFileVersion(content.path, &request.expectedVersion), DiskFileState::Unchanged);
        QVERIFY(PlainFilePersistence().save(&content, request).succeeded());
        QCOMPARE(get(content.path), QByteArray("local edit"));
        QVERIFY(content.accepted);
    }
};
QTEST_GUILESS_MAIN(CloseoutPersistence)
#include "test_closeout_persistence.moc"
