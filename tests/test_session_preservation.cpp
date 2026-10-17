// Session publication tests: real S1 editor/draft storage, not a MainWindow
// journey. Windows commit failure is caused only by a no-delete-sharing lease
// on the actual session.json; no filesystem calls or crypto are replaced.
#include "DraftStore.h"
#include "EditorTab.h"
#include "SessionManager.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

// Independent field oracle: no product serializer is used to compare records.
QJsonObject recordFields(const SessionTab &s)
{
    return {{"filePath", s.filePath}, {"encoding", s.encoding}, {"syntax", s.syntax},
            {"cursorLine", s.cursorLine}, {"cursorIndex", s.cursorIndex},
            {"cursorByteOffset", QString::number(s.cursorByteOffset)},
            {"selectionStartByte", QString::number(s.selectionStartByte)},
            {"selectionEndByte", QString::number(s.selectionEndByte)},
            {"modified", s.modified}, {"autosavePath", s.autosavePath},
            {"secureDocument", s.secureDocument}, {"documentId", s.documentId},
            {"presentation", s.presentation}, {"hexInsertMode", s.hexInsertMode}};
}

SessionTab retainedRecord(const QString &source, const QString &draft)
{
    SessionTab s;
    s.filePath = source;
    s.encoding = QStringLiteral("Windows-1252");
    s.syntax = SessionTab::MaximumPersistedSyntaxValue;
    s.cursorLine = 17;
    s.cursorIndex = 9;
    s.cursorByteOffset = 9007199254740993LL;
    s.selectionStartByte = 9007199254740991LL;
    s.selectionEndByte = 9007199254740995LL;
    s.modified = true;
    s.autosavePath = draft;
    s.documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.presentation = QStringLiteral("hex");
    s.hexInsertMode = true;
    return s;
}

// Metadata-only unit double, deliberately never used as the real I/O fixture.
class MetadataPort final : public ISessionDocumentPort {
public:
    SessionTab state;
    int payloadCalls = 0;
    const QUuid instance = QUuid::createUuid();
    SessionTab captureSessionTab() const override { return state; }
    QUuid sessionInstanceId() const override { return instance; }
    quint64 sessionRevision() const override { return 1; }
    bool needsRecoverySnapshot() const override { return false; }
    bool isEmptyUntitledDocument() const override { return false; }
    bool isLargeRecoveryDocument() const override { return false; }
    QString recoveryPath() const override { return state.autosavePath; }
    void acceptRecoveryPath(const QString &path) override { state.autosavePath = path; }
    bool captureRecoveryPayload(SessionRecoveryPayload *, QString *) override
    { ++payloadCalls; return false; }
    bool restoreSessionRecovery(const SessionRestoreRequest &, QString *) override
    { return false; }
};

// Real EditorTab with two observation counters, not a replacement session port.
// Every observed call immediately runs the unmodified EditorTab implementation.
class ObservedEditor final : public EditorTab {
public:
    int payloadCalls = 0;
    int acceptCalls = 0;
    void acceptRecoveryPath(const QString &path) override
    { ++acceptCalls; EditorTab::acceptRecoveryPath(path); }
    bool captureRecoveryPayload(SessionRecoveryPayload *payload, QString *error) override
    { ++payloadCalls; return EditorTab::captureRecoveryPayload(payload, error); }
};

#ifdef Q_OS_WIN
class ManifestReadLease final {
public:
    explicit ManifestReadLease(const QString &path)
        : handle_(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ManifestReadLease() { release(); }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    void release()
    {
        if (valid()) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
private:
    Q_DISABLE_COPY(ManifestReadLease)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif
} // namespace

class TestSessionPreservation final : public QObject {
    Q_OBJECT
private slots:
    void retainedEntriesKeepOrderAndOpaqueMetadata();
    void invalidRetainedPathRejectsPublication_data();
    void invalidRetainedPathRejectsPublication();
    void manifestCommitFailureDoesNotApplyOrRememberRevision();
    void legacyMigrationKeepsPreviousDraftUntilManifestCommit();
    void legacySplitFieldsAreIgnoredAndNotRewritten();
};

void TestSessionPreservation::legacySplitFieldsAreIgnoredAndNotRewritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionManager manager(dir.filePath(QStringLiteral("profile")));
    SessionTab seed;
    seed.filePath = dir.filePath(QStringLiteral("notes.txt"));
    seed.encoding = QStringLiteral("UTF-8");
    seed.cursorLine = 12;
    QVERIFY(manager.asyncSaveSucceeded(
        manager.writeAsyncSave(manager.captureAsyncSaveEntries({seed}, 0))));

    // An older build wrote split-view state into each tab record.
    QJsonDocument manifest = QJsonDocument::fromJson(fileBytes(manager.sessionFilePath()));
    QJsonObject root = manifest.object();
    QJsonArray tabs = root.value(QStringLiteral("tabs")).toArray();
    QCOMPARE(tabs.size(), 1);
    QJsonObject legacy = tabs.first().toObject();
    legacy.insert(QStringLiteral("splitView"), true);
    legacy.insert(QStringLiteral("splitOrientation"), QStringLiteral("vertical"));
    tabs[0] = legacy;
    root.insert(QStringLiteral("tabs"), tabs);
    QVERIFY(writeBytes(manager.sessionFilePath(), QJsonDocument(root).toJson()));

    const SessionData loaded = manager.loadSession();
    QCOMPARE(loaded.loadStatus, SessionData::LoadStatus::Loaded);
    QCOMPARE(loaded.tabs.size(), 1);
    QCOMPARE(loaded.tabs.first().filePath, seed.filePath);
    QCOMPARE(loaded.tabs.first().cursorLine, 12);

    QVERIFY(manager.asyncSaveSucceeded(
        manager.writeAsyncSave(manager.captureAsyncSaveEntries({loaded.tabs.first()}, 0))));
    const QJsonObject rewritten = QJsonDocument::fromJson(fileBytes(manager.sessionFilePath()))
                                      .object().value(QStringLiteral("tabs")).toArray()
                                      .first().toObject();
    QVERIFY(!rewritten.contains(QStringLiteral("splitView")));
    QVERIFY(!rewritten.contains(QStringLiteral("splitOrientation")));
    QCOMPARE(rewritten.value(QStringLiteral("filePath")).toString(), seed.filePath);
    QCOMPARE(rewritten.value(QStringLiteral("cursorLine")).toInt(), 12);
}

void TestSessionPreservation::retainedEntriesKeepOrderAndOpaqueMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionManager manager(dir.filePath(QStringLiteral("profile")));
    QString error;
    const QString opaquePath = manager.generateAutosavePath();
    QVERIFY2(DraftStore::writeSnapshot(opaquePath, QByteArrayLiteral("OPAQUE_PENDING_DRAFT"), &error), qPrintable(error));
    QByteArray corrupt = fileBytes(opaquePath);
    QVERIFY(!corrupt.isEmpty());
    corrupt[corrupt.size() - 1] ^= 0x55;
    QVERIFY(writeBytes(opaquePath, corrupt));
    QByteArray rejected;
    QVERIFY(!DraftStore::readSnapshot(opaquePath, &rejected, &error));
    const QByteArray opaqueHash = fileHash(opaquePath);

    const auto opaque = retainedRecord(dir.filePath(QStringLiteral("absent-source.bin")), opaquePath);
    auto original = retainedRecord(dir.filePath(QStringLiteral("missing-original.txt")), {});
    original.modified = false;
    auto missing = retainedRecord(dir.filePath(QStringLiteral("other-source.bin")), manager.generateAutosavePath());
    QVERIFY(!QFileInfo::exists(missing.autosavePath));
    auto secureMetadata = retainedRecord(dir.filePath(QStringLiteral("secure-store.nsafe")), {});
    secureMetadata.secureDocument = true; // metadata only, not a vault/decryption test
    secureMetadata.autosavePath = secureMetadata.filePath;
    MetadataPort live;
    live.state.filePath = dir.filePath(QStringLiteral("live-metadata-only.txt"));
    live.state.encoding = QStringLiteral("UTF-8");
    const SessionTab liveExpected = live.state;
    const QVector<SessionCaptureEntry> entries{opaque, static_cast<ISessionDocumentPort *>(&live), original, missing, secureMetadata};
    const auto plan = manager.captureAsyncSaveEntries(entries, 3);
    const auto result = manager.writeAsyncSave(plan);
    QVERIFY(manager.asyncSaveSucceeded(result));
    manager.applyAsyncSave(result, {&live});
    const SessionData saved = manager.loadSession();
    QCOMPARE(saved.tabs.size(), 5);
    QCOMPARE(saved.activeTab, 3);
    QCOMPARE(recordFields(saved.tabs[0]), recordFields(opaque));
    QCOMPARE(recordFields(saved.tabs[1]), recordFields(liveExpected));
    QCOMPARE(recordFields(saved.tabs[2]), recordFields(original));
    QCOMPARE(recordFields(saved.tabs[3]), recordFields(missing));
    QCOMPARE(recordFields(saved.tabs[4]), recordFields(secureMetadata));
    QCOMPARE(live.payloadCalls, 0);
    QCOMPARE(fileHash(opaquePath), opaqueHash);
    QVERIFY(!QFileInfo::exists(missing.autosavePath));
    QVERIFY(!DraftStore::readSnapshot(opaquePath, &rejected, &error));
}

void TestSessionPreservation::invalidRetainedPathRejectsPublication_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("outside-managed-directory") << 0;
    QTest::newRow("unmanaged-basename") << 1;
    QTest::newRow("relative-path") << 2;
}

void TestSessionPreservation::invalidRetainedPathRejectsPublication()
{
    QFETCH(int, kind);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SessionManager manager(dir.filePath(QStringLiteral("profile")));
    const QString keptPath = manager.generateAutosavePath();
    QString error;
    QVERIFY2(DraftStore::writeSnapshot(keptPath, QByteArrayLiteral("PREVIOUS_MANIFEST_RECOVERY"), &error), qPrintable(error));
    const auto kept = retainedRecord({}, keptPath);
    auto initial = manager.writeAsyncSave(manager.captureAsyncSaveEntries({kept}, 0));
    QVERIFY(manager.asyncSaveSucceeded(initial));
    const QByteArray oldManifest = fileBytes(manager.sessionFilePath());
    const QByteArray keptHash = fileHash(keptPath);
    QVERIFY(!oldManifest.isEmpty());

    QString invalidPath;
    if (kind == 0) invalidPath = dir.filePath(QStringLiteral("autosave_outside.ndraft"));
    else if (kind == 1) invalidPath = QDir(manager.autosaveDir()).filePath(QStringLiteral("unmanaged.ndraft"));
    else invalidPath = QStringLiteral("autosave_relative.ndraft");
    if (kind != 2) QVERIFY(writeBytes(invalidPath, QByteArrayLiteral("UNRELATED_FILE_MUST_REMAIN")));
    const QByteArray unrelatedHash = kind != 2 ? fileHash(invalidPath) : QByteArray{};
    const auto invalid = retainedRecord({}, invalidPath);
    const auto result = manager.writeAsyncSave(manager.captureAsyncSaveEntries({invalid}, 0));
    QVERIFY(!manager.asyncSaveSucceeded(result));
    manager.applyAsyncSave(result, {});
    QCOMPARE(fileBytes(manager.sessionFilePath()), oldManifest);
    QCOMPARE(fileHash(keptPath), keptHash);
    QCOMPARE(manager.loadSession().tabs.size(), 1);
    if (kind != 2) QCOMPARE(fileHash(invalidPath), unrelatedHash);
}

void TestSessionPreservation::manifestCommitFailureDoesNotApplyOrRememberRevision()
{
#ifndef Q_OS_WIN
    QSKIP("Real no-delete-sharing QSaveFile failure is a Windows-only acceptance case.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("ordinary.txt"));
    const QByteArray original = QByteArrayLiteral("ORIGINAL_DISK_CONTENT\n");
    QVERIFY(writeBytes(source, original));
    SessionManager manager(dir.filePath(QStringLiteral("profile")));
    ObservedEditor editor;
    QVERIFY(editor.loadFile(source));
    editor.editor()->append(QStringLiteral("UNSAVED_A\n"));
    QVERIFY(editor.isModified());
    const QVector<ISessionDocumentPort *> ports{&editor};
    QVERIFY(manager.saveSession(ports, 0, true));
    const QString previousPath = editor.recoveryPath();
    QVERIFY(DraftStore::isEncryptedDraft(previousPath));
    const QByteArray manifestBytes = fileBytes(manager.sessionFilePath());
    const QByteArray manifestHash = fileHash(manager.sessionFilePath());
    const QByteArray sourceHash = fileHash(source);
    const quint64 previousRevision = editor.sessionRevision();
    editor.editor()->append(QStringLiteral("UNSAVED_B\n"));
    QVERIFY(editor.sessionRevision() > previousRevision);
    const quint64 pendingRevision = editor.sessionRevision();
    const QByteArray pendingBody = editor.editor()->text().toUtf8();
    const int capturesBefore = editor.payloadCalls;
    const int acceptsBefore = editor.acceptCalls;
    const auto plan = manager.captureAsyncSave(ports, 0, false);
    QCOMPARE(editor.payloadCalls, capturesBefore + 1);
    ManifestReadLease lease(manager.sessionFilePath());
    QVERIFY2(lease.valid(), "Could not establish the real manifest-only no-delete-sharing lease.");
    const auto failed = manager.writeAsyncSave(plan);
    QVERIFY(!manager.asyncSaveSucceeded(failed));
    QCOMPARE(fileBytes(manager.sessionFilePath()), manifestBytes);
    QCOMPARE(fileHash(manager.sessionFilePath()), manifestHash);
    QCOMPARE(manager.loadSession().tabs.size(), 1);
    QCOMPARE(manager.loadSession().tabs.first().autosavePath, previousPath);
    QByteArray decoded;
    QString error;
    // Same-path resealing remains existing behavior: the draft must still be
    // valid, but this test does not invent transactional old-ciphertext semantics.
    QVERIFY2(DraftStore::readSnapshot(previousPath, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded, pendingBody); // proves draft I/O succeeded before manifest failure
    manager.applyAsyncSave(failed, ports);
    QCOMPARE(editor.acceptCalls, acceptsBefore);
    QCOMPARE(editor.recoveryPath(), previousPath);
    QCOMPARE(editor.sessionRevision(), pendingRevision);
    QCOMPARE(editor.editor()->text().toUtf8(), pendingBody);
    QVERIFY(editor.isModified());
    QCOMPARE(fileHash(source), sourceHash);

    lease.release();
    const auto retry = manager.captureAsyncSave(ports, 0, false);
    QCOMPARE(editor.payloadCalls, capturesBefore + 2); // failure did not remember revision
    const auto committed = manager.writeAsyncSave(retry);
    QVERIFY(manager.asyncSaveSucceeded(committed));
    manager.applyAsyncSave(committed, ports);
    QCOMPARE(editor.acceptCalls, acceptsBefore + 1);
    QVERIFY2(DraftStore::readSnapshot(editor.recoveryPath(), &decoded, &error), qPrintable(error));
    QCOMPARE(decoded, pendingBody);
    QCOMPARE(fileBytes(source), original);
    const int capturedAfterCommit = editor.payloadCalls;
    manager.captureAsyncSave(ports, 0, false);
    QCOMPARE(editor.payloadCalls, capturedAfterCommit); // successful commit remembers revision
#endif
}

void TestSessionPreservation::legacyMigrationKeepsPreviousDraftUntilManifestCommit()
{
#ifndef Q_OS_WIN
    QSKIP("Real no-delete-sharing QSaveFile migration failure is Windows-only.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("migration-source.txt"));
    const QByteArray original = QByteArrayLiteral("UNCHANGED_SOURCE\n");
    QVERIFY(writeBytes(source, original));
    SessionManager manager(dir.filePath(QStringLiteral("profile")));
    // Explicit legacy input fixture only; production must write a sealed ndraft.
    const QString legacy = QDir(manager.autosaveDir()).filePath(QStringLiteral("autosave_legacy.tmp"));
    const QByteArray legacyBody = QByteArrayLiteral("LEGACY_RECOVERABLE_BODY\n");
    QVERIFY(writeBytes(legacy, legacyBody));
    const auto retained = retainedRecord(source, legacy);
    const auto seed = manager.writeAsyncSave(manager.captureAsyncSaveEntries({retained}, 0));
    QVERIFY(manager.asyncSaveSucceeded(seed));
    const QByteArray manifestBytes = fileBytes(manager.sessionFilePath());
    const QByteArray legacyHash = fileHash(legacy);
    ObservedEditor editor;
    QVERIFY(editor.loadFile(source));
    editor.editor()->append(QStringLiteral("MIGRATED_UNSAVED_CONTENT\n"));
    QVERIFY(editor.isModified());
    editor.acceptRecoveryPath(legacy);
    const QByteArray pendingBody = editor.editor()->text().toUtf8();
    const int acceptsBefore = editor.acceptCalls;
    const QVector<ISessionDocumentPort *> ports{&editor};
    const auto plan = manager.captureAsyncSave(ports, 0, false);
    ManifestReadLease lease(manager.sessionFilePath());
    QVERIFY(lease.valid());
    const auto failed = manager.writeAsyncSave(plan);
    QVERIFY(!manager.asyncSaveSucceeded(failed));
    const auto sealedFiles = QDir(manager.autosaveDir()).entryInfoList({QStringLiteral("autosave_*.ndraft")}, QDir::Files);
    QCOMPARE(sealedFiles.size(), 1);
    QByteArray decoded;
    QString error;
    QVERIFY2(DraftStore::readSnapshot(sealedFiles.first().absoluteFilePath(), &decoded, &error), qPrintable(error));
    QCOMPARE(decoded, pendingBody);
    QCOMPARE(fileBytes(manager.sessionFilePath()), manifestBytes);
    QCOMPARE(fileHash(legacy), legacyHash);
    QCOMPARE(fileBytes(legacy), legacyBody);
    manager.applyAsyncSave(failed, ports);
    QCOMPARE(editor.acceptCalls, acceptsBefore);
    QCOMPARE(editor.recoveryPath(), legacy);
    QCOMPARE(manager.loadSession().tabs.first().autosavePath, legacy);
    QCOMPARE(fileBytes(source), original);

    lease.release();
    const auto committed = manager.writeAsyncSave(manager.captureAsyncSave(ports, 0, false));
    QVERIFY(manager.asyncSaveSucceeded(committed));
    manager.applyAsyncSave(committed, ports);
    QCOMPARE(editor.acceptCalls, acceptsBefore + 1);
    QVERIFY(editor.recoveryPath() != legacy);
    QVERIFY(!QFileInfo::exists(legacy));
    QCOMPARE(manager.loadSession().tabs.first().autosavePath, editor.recoveryPath());
    QVERIFY2(DraftStore::readSnapshot(editor.recoveryPath(), &decoded, &error), qPrintable(error));
    QCOMPARE(decoded, pendingBody);
    QCOMPARE(fileBytes(source), original);
#endif
}

QTEST_MAIN(TestSessionPreservation)
#include "test_session_preservation.moc"
