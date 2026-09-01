// R3 recovery preservation baseline: real encrypted drafts and public UI cuts.
// Future runner must impose an external per-case process deadline (e.g. 30 s):
// QTRY cannot interrupt a production closeEvent's nested synchronous save loop.
// No private invocations, timer changes, replacement recovery ports, or plaintext autosaves.
// This covers ordinary encrypted recovery via normal CLOSE save, not the 30 s
// automatic timer, OS termination/restart, mixed secure recovery, or timing budgets.

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <Qsci/qsciscintilla.h>

#include <array>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "AsyncSerialQueue.h"
#include "AsyncTaskManager.h"
#include "DraftStore.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "SessionManager.h"
#include "StartupCoordinator.h"

namespace {
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QByteArray hashFile(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}

void destroyWithoutSaving(MainWindow *window)
{
    if (!window) return;
    if (auto *tabs = window->findChild<QTabWidget *>()) {
        for (int index = 0; index < tabs->count(); ++index)
            if (auto *tab = qobject_cast<EditorTab *>(tabs->widget(index))) tab->shutdownL2BeforeClose();
    }
    // Test cleanup only, NOT evidence of a killed/restarted process. No closeEvent save.
    delete window;
}
using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutSaving)>;

class DialogGuard final : public QObject {
public:
    DialogGuard() { qApp->installEventFilter(this); }
    ~DialogGuard() override { qApp->removeEventFilter(this); }
    QStringList unexpected;
    QString cancelSaveForFile;
    int cancelSavePrompts = 0;
    int cancelClicks = 0;
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(object);
            auto *box = qobject_cast<QMessageBox *>(dialog);
            if (box && !cancelSaveForFile.isEmpty()
                && box->standardButtons() == (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel)
                && box->text().contains(QLatin1Char('"') + cancelSaveForFile + QLatin1Char('"'))) {
                ++cancelSavePrompts;
                const QPointer<QMessageBox> guard(box);
                QTimer::singleShot(0, this, [this, guard] {
                    if (!guard) return;
                    auto *cancel = guard->button(QMessageBox::Cancel);
                    if (!cancel || !cancel->isVisible() || !cancel->isEnabled()) {
                        unexpected.append(QStringLiteral("expected Save/Discard/Cancel has no usable Cancel"));
                        guard->reject();
                        return;
                    }
                    ++cancelClicks;
                    QTest::mouseClick(cancel, Qt::LeftButton);
                });
                return QObject::eventFilter(object, event);
            }
            if (dialog && dialog->isModal()
                && dialog->objectName() != QStringLiteral("sessionCommitProgress")) {
                unexpected.append(dialog->objectName() + QLatin1Char(':') + dialog->windowTitle());
                const QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, this, [guard] { if (guard) guard->reject(); });
            }
        }
        return QObject::eventFilter(object, event);
    }
};

#ifdef Q_OS_WIN
class ManifestReadLease final {
public:
    explicit ManifestReadLease(const QString &path)
        : handle_(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }

    ~ManifestReadLease() { release(); }

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

    void release()
    {
        if (valid())
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    Q_DISABLE_COPY(ManifestReadLease)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

struct Item {
    QString sourcePath;
    QByteArray original;
    QByteArray canary;
    QByteArray dirtyBody;
    QByteArray sourceHash;
    SessionTab record;
    QByteArray draftHash;
};

struct DiskCut {
    bool manifestExists = false;
    QByteArray manifestBytes;
    SessionData session;
    std::array<QByteArray, 3> sourceHashes;
    std::array<QByteArray, 3> draftHashes;
    std::array<bool, 3> sealedAndDecryptable{};
    std::array<int, 3> recoverableManifestReferences{};
    int extraRecoverableReference = 0;
    bool plaintextRecoveryLeak = false;
};

QByteArray inflightDirtyBody(const QByteArray &original, const QByteArray &canary,
                            qsizetype totalBytes)
{
    const QByteArray suffix = '\n' + canary + '\n';
    const qsizetype padding = totalBytes - original.size() - suffix.size();
    if (padding <= 0) return {};
    // Short ASCII lines avoid testing giant-line layout or UTF-8 conversion.
    const QByteArray line = QByteArray(127, 'x') + '\n';
    QByteArray result = original;
    result.reserve(totalBytes);
    result += line.repeated(padding / line.size());
    result += line.left(padding % line.size());
    result += suffix; // Unique canary exists only at the end of the complete draft.
    return result;
}

class RecoveryFixture final {
public:
    ~RecoveryFixture()
    {
        // All business cuts are captured before this destructor; cleanup cannot
        // turn a lost manifest into a successful observation.
        // Each case's later-declared observationScope has already disconnected.
        // A valid finish may start a fresh publication whose worker captures the
        // window's SessionManager. Drain that lane before destroying either owner;
        // never reread the disk cut after this cleanup wait.
        if (!QTest::qWaitFor([] { return !AsyncSerialQueue::session().busy(); }, 5000))
            qFatal("RECOVERY_DRAFT_CLEANUP_TIMEOUT: session worker still owns its manager; refusing teardown");
        restarted.reset();
        manager.reset();
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", oldProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool prepare(qsizetype firstDirtyBytes = 0)
    {
        if (!temporary.isValid()) return fail(temporary.errorString());
        profile = temporary.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile)) return fail(QStringLiteral("cannot create isolated profile"));
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        profileSet_ = qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        if (!profileSet_) return fail(QStringLiteral("cannot isolate profile"));
        if (AsyncSerialQueue::session().busy()) return fail(QStringLiteral("session lane is not initially idle"));
        manager = std::make_unique<SessionManager>(profile);
        WindowOwner seed(new MainWindow, destroyWithoutSaving); // Real default AppComposition.
        seed->resize(1100, 750);
        seed->show();
        if (!QTest::qWaitForWindowExposed(seed.get())) return fail(QStringLiteral("seed window not exposed"));
        QVector<ISessionDocumentPort *> documents;
        for (int index = 0; index < 3; ++index) {
            auto &item = items[index];
            item.sourcePath = temporary.filePath(QStringLiteral("ordinary-%1.txt").arg(index));
            item.original = QByteArrayLiteral("original ordinary source ") + QByteArray::number(index) + '\n';
            item.canary = QByteArrayLiteral("UNSAVED_3A_DRAFT_") + QByteArray::number(index) + QByteArrayLiteral("_CANARY");
            item.dirtyBody = item.original + item.canary + '\n';
            if (index == 0 && firstDirtyBytes > 0)
                item.dirtyBody = inflightDirtyBody(item.original, item.canary, firstDirtyBytes);
            if (item.dirtyBody.isEmpty()) return fail(QStringLiteral("invalid inflight fixture size"));
            QFile source(item.sourcePath);
            if (!source.open(QIODevice::WriteOnly) || source.write(item.original) != item.original.size()
                || !source.flush()) return fail(QStringLiteral("cannot write source fixture"));
            source.close();
            item.sourceHash = hashFile(item.sourcePath);
            seed->openFilesFromExternal({item.sourcePath});
            auto *tabs = seed->findChild<QTabWidget *>();
            auto *tab = tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
            if (!tab || tab->filePath() != item.sourcePath || tab->isSecureDocument() || tab->isL2Mode())
                return fail(QStringLiteral("real ordinary S1 open failed"));
            tab->editor()->append(QString::fromLatin1(item.dirtyBody.mid(item.original.size())));
            if (!tab->isModified() || tab->editor()->text().toUtf8() != item.dirtyBody)
                return fail(QStringLiteral("real editable dirty fixture not established"));
            documents.append(tab);
        }
        // Product capture -> DraftStore DPAPI seal -> QSaveFile manifest publication.
        // No hand-written session JSON or plaintext .tmp recovery file.
        if (!manager->saveSession(documents, 0, true)) return fail(QStringLiteral("product session save failed"));
        const SessionData session = manager->loadSession();
        if (session.tabs.size() != 3 || session.activeTab != 0) return fail(QStringLiteral("manifest fixture is not three items"));
        for (int index = 0; index < 3; ++index) {
            auto &item = items[index];
            item.record = session.tabs.at(index);
            item.draftHash = hashFile(item.record.autosavePath);
            QByteArray decoded;
            if (item.record.filePath != item.sourcePath || !item.record.modified || item.record.secureDocument
                || item.draftHash.isEmpty() || !DraftStore::isEncryptedDraft(item.record.autosavePath)
                || !DraftStore::readSnapshot(item.record.autosavePath, &decoded) || decoded != item.dirtyBody)
                return fail(QStringLiteral("product did not create a recoverable encrypted dirty draft"));
        }
        seed.reset(); // Fixture persistence was already performed by SessionManager.
        const DiskCut baseline = cut();
        if (baseline.plaintextRecoveryLeak) return fail(QStringLiteral("fixture leaked unsaved plaintext into recovery files"));
        for (int index = 0; index < 3; ++index)
            if (baseline.sourceHashes[index] != items[index].sourceHash || !baseline.sealedAndDecryptable[index])
                return fail(QStringLiteral("seed teardown changed source or encrypted recovery"));
        return true;
    }

    bool showRestarted()
    {
        restarted.reset(new MainWindow);
        // Normal close may acquire deferred destruction in a future implementation.
        // Relinquish only that exact object; never delete it again during cleanup.
        QObject::connect(restarted.get(), &QObject::destroyed, [this](QObject *object) {
            if (restarted.get() == object) restarted.release();
        });
        restarted->resize(1100, 750);
        restarted->show();
        return QTest::qWaitForWindowExposed(restarted.get()) || fail(QStringLiteral("recovery window not exposed"));
    }

    DiskCut cut(const QString &extraSource = {}, const QByteArray &extraBody = {}) const
    {
        DiskCut result;
        result.manifestExists = QFileInfo::exists(manager->sessionFilePath());
        result.manifestBytes = readFile(manager->sessionFilePath());
        // Fresh reader, not an old in-memory SessionData supplied by the coordinator.
        SessionManager diskReader(profile);
        result.session = diskReader.loadSession();
        for (int index = 0; index < 3; ++index) {
            const auto &item = items[index];
            result.sourceHashes[index] = hashFile(item.sourcePath);
            result.draftHashes[index] = hashFile(item.record.autosavePath);
            QByteArray decoded;
            result.sealedAndDecryptable[index] = DraftStore::isEncryptedDraft(item.record.autosavePath)
                && DraftStore::readSnapshot(item.record.autosavePath, &decoded) && decoded == item.dirtyBody;
            for (const auto &record : result.session.tabs) {
                if (record.filePath != item.sourcePath || !record.modified || record.secureDocument
                    || record.autosavePath.isEmpty()) continue;
                QByteArray publishedBody;
                if (DraftStore::isEncryptedDraft(record.autosavePath)
                    && DraftStore::readSnapshot(record.autosavePath, &publishedBody)
                    && publishedBody == item.dirtyBody) ++result.recoverableManifestReferences[index];
            }
        }
        if (!extraSource.isEmpty()) {
            for (const auto &record : result.session.tabs) {
                if (record.filePath != extraSource || !record.modified || record.secureDocument) continue;
                QByteArray body;
                if (DraftStore::isEncryptedDraft(record.autosavePath)
                    && DraftStore::readSnapshot(record.autosavePath, &body) && body == extraBody)
                    ++result.extraRecoverableReference;
            }
        }
        QDirIterator files(profile, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QByteArray bytes = readFile(files.next());
            for (const auto &item : items)
                if (bytes.contains(item.canary)) result.plaintextRecoveryLeak = true;
            if (!extraBody.isEmpty() && bytes.contains(extraBody)) result.plaintextRecoveryLeak = true;
        }
        return result;
    }

    QString restoredGuard(int count) const
    {
        auto *tabs = restarted ? restarted->findChild<QTabWidget *>() : nullptr;
        if (!tabs || tabs->count() != count) return QStringLiteral("wrong number of delivered tabs");
        for (int index = 0; index < count; ++index) {
            auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
            if (!tab || tab->filePath() != items[index].sourcePath
                || tab->editor()->text().toUtf8() != items[index].dirtyBody
                || !tab->isModified() || tab->isSecureDocument()
                || tab->documentState().securityClass != DocumentSecurityClass::OrdinaryDocument
                || !tab->documentState().capabilities.testFlag(DocumentCapability::Edit))
                return QStringLiteral("delivered dirty document/body/recovery responsibility mismatch");
        }
        return {};
    }

    QTemporaryDir temporary;
    DialogGuard dialogs;
    QString profile;
    QString error;
    std::array<Item, 3> items;
    std::unique_ptr<SessionManager> manager;
    WindowOwner restarted{nullptr, destroyWithoutSaving};
private:
    bool fail(const QString &message) { error = message; return false; }
    QByteArray oldProfile_;
    bool hadProfile_ = false;
    bool profileSet_ = false;
};

QStringList retainedManifestIssues(const RecoveryFixture &fixture, const DiskCut &cut,
                                   int expectedRecords = 3)
{
    QStringList issues;
    if (!cut.manifestExists || cut.session.tabs.size() != expectedRecords)
        issues.append(QStringLiteral("fresh loadSession cannot discover all three dirty responsibilities"));
    for (int index = 0; index < 3; ++index) {
        const auto &item = fixture.items[index];
        int count = 0;
        for (const auto &record : cut.session.tabs)
            if (record.filePath == item.sourcePath && record.modified && !record.secureDocument) ++count;
        if (count != 1) issues.append(QStringLiteral("dirty manifest responsibility missing or duplicated"));
        if (cut.recoverableManifestReferences[index] != 1)
            issues.append(QStringLiteral("published manifest does not reference exactly one decryptable dirty body"));
    }
    return issues;
}
} // namespace

class RecoveryPreservationSmoke final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY2(QGuiApplication::platformName() == QStringLiteral("windows"), "Real Windows DPAPI/native Qt required");
        quitOnLastWindow_ = qApp->quitOnLastWindowClosed();
        qApp->setQuitOnLastWindowClosed(false); // Keep QtTest alive after the real closeEvent.
    }
    void cleanupTestCase() { qApp->setQuitOnLastWindowClosed(quitOnLastWindow_); }

    void closingDuringFirstDraftProjectionRetainsCompleteDrafts();
    void cancelingCloseAfterFirstRestoredItemContinuesExactlyOnce();
    void completedRecoverySavesReorderedTabsAndActiveIndex();
    void closingAfterFirstPaintBeforeRecoveryStartsRetainsDrafts();
    void forcedSessionCommitFailureKeepsWindowOpenAndRetryable();

    void closingAfterFirstRestoredItemRetainsUndeliveredDrafts()
    {
        RecoveryFixture fixture;
        QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
        QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
        QPointer<MainWindow> window(fixture.restarted.get());
        bool intercepted = false, closing = false, closeAccepted = false, closeReturned = false;
        QString beforeGuard;
        DiskCut before, after;
        QList<qint64> snapshotTasks;
        int snapshotTerminals = 0, restoreCompleted = 0;
        AsyncTaskState snapshotState = AsyncTaskState::Queued;
        QList<QPointer<QWidget>> pagesAtClose;
        int completedAtClose = -1;
        auto &tasks = AsyncTaskManager::instance();
        QObject observationScope; // Disconnect before captured locals and fixture are destroyed.
        QObject::connect(&tasks, &AsyncTaskManager::taskAdded, &observationScope, [&](qint64 id) {
            if (closing && tasks.taskById(id).type == AsyncTaskType::SessionSnapshot) snapshotTasks.append(id);
        }, Qt::DirectConnection);
        QObject::connect(&tasks, &AsyncTaskManager::taskFinished, &observationScope, [&](qint64 id, AsyncTaskState state) {
            if (snapshotTasks.contains(id)) { ++snapshotTerminals; snapshotState = state; }
        }, Qt::DirectConnection);
        QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted, &observationScope, [&] { ++restoreCompleted; });
        // Existing public UI observation: emitted after restoreDraft and restoreDocumentSessionView,
        // and before the next restoreNextSessionTab is queued. No timer cancellation or blocked worker.
        QObject::connect(window->statusBar(), &QStatusBar::messageChanged, &observationScope, [&](const QString &message) {
            if (intercepted || message != QStringLiteral("正在恢复会话… 1/3 个标签页")) return;
            intercepted = true;
            beforeGuard = fixture.restoredGuard(1);
            before = fixture.cut();
            auto *tabs = window->findChild<QTabWidget *>();
            auto *first = tabs ? qobject_cast<EditorTab *>(tabs->widget(0)) : nullptr;
            if (!beforeGuard.isEmpty() || !first || restoreCompleted != 0) return;
            if (first->autosavePath().isEmpty() || !QFileInfo::exists(first->autosavePath())) {
                beforeGuard = QStringLiteral("restored draft does not establish the real no-prompt close branch");
                return;
            }
            // Public normal close -> maybeSaveTab -> saveSessionNow(true) -> real session lane.
            // maybeSaveTab(closingApp)'s existing-autosave branch allows this restored dirty
            // ordinary tab without a prompt. No Discard selection or private closeEvent call.
            closing = true;
            closeAccepted = window->close();
            closing = false;
            if (!window) fixture.restarted.release();
            // Normal close may release its widgets; do not dereference first or require live tabs.
            after = fixture.cut(); // Cut before callback returns / before another queued restore.
            if (auto *remainingTabs = window ? window->findChild<QTabWidget *>() : nullptr)
                for (int index = 0; index < remainingTabs->count(); ++index)
                    pagesAtClose.append(remainingTabs->widget(index));
            completedAtClose = restoreCompleted;
            closeReturned = true;
            qInfo() << "RECOVERY_CLOSE_CUT" << "delivered" << 1 << "total" << 3
                    << "snapshotTasks" << snapshotTasks.size() << "manifestItems" << after.session.tabs.size();
        }, Qt::DirectConnection);
        window->deferredRestoreSession();
        QTRY_VERIFY_WITH_TIMEOUT(intercepted, 5000);
        QVERIFY2(beforeGuard.isEmpty(), qPrintable(beforeGuard));
        QVERIFY(closeReturned && closeAccepted);
        QCOMPARE(snapshotTasks.size(), 1);
        QCOMPARE(snapshotTerminals, 1);
        QCOMPARE(snapshotState, AsyncTaskState::Finished);
        QVERIFY(!AsyncSerialQueue::session().busy());
        QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
        QVERIFY(!before.plaintextRecoveryLeak && !after.plaintextRecoveryLeak);
        const auto beforeIssues = retainedManifestIssues(fixture, before);
        QVERIFY2(beforeIssues.isEmpty(), qPrintable(beforeIssues.join(QLatin1Char('\n'))));
        for (int index = 0; index < 3; ++index) {
            QCOMPARE(before.sourceHashes[index], fixture.items[index].sourceHash);
            QCOMPARE(after.sourceHashes[index], fixture.items[index].sourceHash);
            if (index > 0) {
                QCOMPARE(before.draftHashes[index], fixture.items[index].draftHash);
                QVERIFY(before.sealedAndDecryptable[index]);
            }
        }
        QStringList issues = retainedManifestIssues(fixture, after);
        for (int index = 1; index < 3; ++index)
            if (after.draftHashes[index] != fixture.items[index].draftHash || !after.sealedAndDecryptable[index])
                issues.append(QStringLiteral("undelivered encrypted draft was deleted or changed"));
        // A delivered draft may be resealed or replaced by the legitimate forced save;
        // retainedManifestIssues validates the published encrypted replacement, not its nonce/path.
        QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
        // Deliberately observe already-queued callbacks after the immutable disk
        // cut. This is not a preparation delay or a substitute for that cut.
        QTest::qWait(100);
        QVERIFY(!window || !window->isVisible());
        QCOMPARE(completedAtClose, 0);
        QCOMPARE(restoreCompleted, completedAtClose);
        if (auto *remainingTabs = window ? window->findChild<QTabWidget *>() : nullptr) {
            QVERIFY(remainingTabs->count() <= pagesAtClose.size());
            for (int index = 0; index < remainingTabs->count(); ++index)
                QVERIFY(pagesAtClose.contains(QPointer<QWidget>(remainingTabs->widget(index))));
        }
        const DiskCut late = fixture.cut();
        QCOMPARE(late.manifestBytes, after.manifestBytes);
        const auto lateIssues = retainedManifestIssues(fixture, late);
        QVERIFY2(lateIssues.isEmpty(), qPrintable(lateIssues.join(QLatin1Char('\n'))));
        for (int index = 0; index < 3; ++index)
            QCOMPARE(late.sourceHashes[index], fixture.items[index].sourceHash);
        QVERIFY(!late.plaintextRecoveryLeak);
    }

    void completedRecoveryImmediatelyRemainsDiscoverable()
    {
        RecoveryFixture fixture;
        QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
        QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
        auto *window = fixture.restarted.get();
        DiskCut completionCut;
        QString liveGuard;
        int completed = 0, snapshotTasks = 0;
        auto &tasks = AsyncTaskManager::instance();
        QObject observationScope; // Outlives callbacks, dies before all their captured locals.
        QObject::connect(&tasks, &AsyncTaskManager::taskAdded, &observationScope, [&](qint64 id) {
            if (tasks.taskById(id).type == AsyncTaskType::SessionSnapshot) ++snapshotTasks;
        }, Qt::DirectConnection);
        QObject::connect(window, &MainWindow::sessionRestoreCompleted, &observationScope, [&] {
            ++completed;
            liveGuard = fixture.restoredGuard(3);
            completionCut = fixture.cut(); // Immediate disk read, before periodic save or teardown.
            qInfo() << "RECOVERY_FINISH_CUT" << "delivered" << 3
                    << "manifestItems" << completionCut.session.tabs.size() << "snapshotTasks" << snapshotTasks;
        }, Qt::DirectConnection);
        window->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 5000);
        QVERIFY2(liveGuard.isEmpty(), qPrintable(liveGuard));
        // A valid implementation may publish a new complete recovery point before this signal.
        QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
        QVERIFY(!completionCut.plaintextRecoveryLeak);
        for (int index = 0; index < 3; ++index) {
            QCOMPARE(completionCut.sourceHashes[index], fixture.items[index].sourceHash);
        }
        const auto issues = retainedManifestIssues(fixture, completionCut);
        QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
    }
private:
    bool quitOnLastWindow_ = true;
};

void RecoveryPreservationSmoke::forcedSessionCommitFailureKeepsWindowOpenAndRetryable()
{
#ifndef Q_OS_WIN
    QSKIP("Real manifest replacement failure uses a Windows no-delete-sharing lease.");
#else
    RecoveryFixture fixture;
    QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
    QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
    QPointer<MainWindow> window(fixture.restarted.get());
    int restored = 0;
    int committed = 0;
    QObject observationScope;
    QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted,
                     &observationScope, [&] { ++restored; }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::shutdownCommitted,
                     &observationScope, [&] { ++committed; }, Qt::DirectConnection);
    window->deferredRestoreSession();
    QTRY_COMPARE_WITH_TIMEOUT(restored, 1, 5000);
    QVERIFY2(fixture.restoredGuard(3).isEmpty(),
             qPrintable(fixture.restoredGuard(3)));

    const DiskCut before = fixture.cut();
    const auto beforeIssues = retainedManifestIssues(fixture, before);
    QVERIFY2(beforeIssues.isEmpty(), qPrintable(beforeIssues.join(QLatin1Char('\n'))));
    QVERIFY(!before.plaintextRecoveryLeak);
    const QByteArray manifestHash = hashFile(fixture.manager->sessionFilePath());
    QVERIFY(!manifestHash.isEmpty());

    ManifestReadLease lease(fixture.manager->sessionFilePath());
    QVERIFY2(lease.valid(),
             "Could not establish the real manifest no-delete-sharing lease.");

    auto &tasks = AsyncTaskManager::instance();
    QList<qint64> snapshotTasks;
    QList<QPair<qint64, AsyncTaskState>> terminals;
    QObject::connect(&tasks, &AsyncTaskManager::taskAdded, &observationScope,
                     [&](qint64 id) {
        if (tasks.taskById(id).type == AsyncTaskType::SessionSnapshot)
            snapshotTasks.append(id);
    }, Qt::DirectConnection);
    QObject::connect(&tasks, &AsyncTaskManager::taskFinished, &observationScope,
                     [&](qint64 id, AsyncTaskState state) {
        if (snapshotTasks.contains(id))
            terminals.append(qMakePair(id, state));
    }, Qt::DirectConnection);

    const bool firstAccepted = window->close();
    const DiskCut failed = fixture.cut();
    qInfo() << "RECOVERY_SESSION_COMMIT_FAILURE_CUT"
            << "accepted" << firstAccepted
            << "visible" << bool(window && window->isVisible())
            << "shutdownCommitted" << committed
            << "snapshotTasks" << snapshotTasks
            << "terminalCount" << terminals.size()
            << "terminalState"
            << (terminals.isEmpty() ? -1 : int(terminals.front().second))
            << "manifestHash" << hashFile(fixture.manager->sessionFilePath()).toHex();

    QVERIFY(!firstAccepted);
    QVERIFY(window && window->isVisible());
    QCOMPARE(committed, 0);
    QCOMPARE(snapshotTasks.size(), 1);
    QCOMPARE(terminals.size(), 1);
    QCOMPARE(terminals.front().first, snapshotTasks.front());
    QCOMPARE(terminals.front().second, AsyncTaskState::Failed);
    QVERIFY(!AsyncSerialQueue::session().busy());
    QCOMPARE(failed.manifestBytes, before.manifestBytes);
    QCOMPARE(hashFile(fixture.manager->sessionFilePath()), manifestHash);
    const auto failedIssues = retainedManifestIssues(fixture, failed);
    QVERIFY2(failedIssues.isEmpty(), qPrintable(failedIssues.join(QLatin1Char('\n'))));
    QVERIFY(!failed.plaintextRecoveryLeak);

    auto *tabs = window->findChild<QTabWidget *>();
    QVERIFY(tabs && tabs->count() == 3);
    auto *first = qobject_cast<EditorTab *>(tabs->widget(0));
    QVERIFY(first && first->editor() && first->isModified());
    const QString originalText = first->editor()->text();
    const int lastLine = first->editor()->lines() - 1;
    const int lastIndex = first->editor()->lineLength(lastLine);
    first->editor()->insertAt(QStringLiteral("POST_FAILURE_WINDOW_STILL_EDITABLE"),
                              lastLine, lastIndex);
    QVERIFY(first->editor()->text().endsWith(
        QStringLiteral("POST_FAILURE_WINDOW_STILL_EDITABLE")));
    QVERIFY(first->editor()->isUndoAvailable());
    first->editor()->undo();
    QCOMPARE(first->editor()->text(), originalText);
    QVERIFY(first->isModified());

    lease.release();
    snapshotTasks.clear();
    terminals.clear();
    const bool retryAccepted = window->close();
    if (!window)
        fixture.restarted.release();
    const DiskCut closed = fixture.cut();
    qInfo() << "RECOVERY_SESSION_COMMIT_RETRY_CUT"
            << "accepted" << retryAccepted
            << "shutdownCommitted" << committed
            << "snapshotTasks" << snapshotTasks
            << "terminalCount" << terminals.size()
            << "terminalState"
            << (terminals.isEmpty() ? -1 : int(terminals.front().second));

    QVERIFY(retryAccepted);
    QVERIFY(!window || !window->isVisible());
    QCOMPARE(committed, 1);
    QCOMPARE(snapshotTasks.size(), 1);
    QCOMPARE(terminals.size(), 1);
    QCOMPARE(terminals.front().first, snapshotTasks.front());
    QCOMPARE(terminals.front().second, AsyncTaskState::Finished);
    QVERIFY(!AsyncSerialQueue::session().busy());
    const auto closedIssues = retainedManifestIssues(fixture, closed);
    QVERIFY2(closedIssues.isEmpty(), qPrintable(closedIssues.join(QLatin1Char('\n'))));
    QVERIFY(!closed.plaintextRecoveryLeak);
    QVERIFY2(fixture.dialogs.unexpected.isEmpty(),
             qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
#endif
}

void RecoveryPreservationSmoke::closingDuringFirstDraftProjectionRetainsCompleteDrafts()
{
    constexpr qsizetype fullBytes = 8 * 1024 * 1024;
    RecoveryFixture fixture;
    QVERIFY2(fixture.prepare(fullBytes), qPrintable(fixture.error));
    QCOMPARE(fixture.items[0].dirtyBody.size(), fullBytes);
    QVERIFY(fixture.items[0].dirtyBody.endsWith(fixture.items[0].canary + '\n'));
    QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
    QPointer<MainWindow> window(fixture.restarted.get());
    bool observed = false, queued = false, attempted = false;
    bool closing = false, closeAccepted = false, closeReturned = false;
    int restoreCompleted = 0, committedSignals = 0;
    qsizetype observedLength = -1, dispatchLength = -1;
    QString guardError, progressText;
    DiskCut before, after;
    QList<qint64> snapshotTasks;
    QList<QPair<qint64, AsyncTaskState>> snapshotTerminals;
    auto &tasks = AsyncTaskManager::instance();
    const auto partialGuard = [&](qsizetype *length) -> QString {
        auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
        auto *tab = tabs && tabs->count() == 1
            ? qobject_cast<EditorTab *>(tabs->widget(0)) : nullptr;
        if (!tab || !tab->editor() || restoreCompleted != 0)
            return QStringLiteral("cut is not inside the sole first restoring tab");
        // A safe implementation may temporarily prohibit editing during restore.
        // The ordinary security domain and exact partial body are the contract,
        // not an Edit capability or read-only flag before recovery commits.
        if (tab->isSecureDocument() || tab->isL2Mode()
            || tab->documentState().securityClass != DocumentSecurityClass::OrdinaryDocument)
            return QStringLiteral("inflight target is not an ordinary S1 document");
        if (!tab->isUntitled() || !tab->filePath().isEmpty() || !tab->recoveryPath().isEmpty())
            return QStringLiteral("draft has already acquired its final source/recovery binding");
        *length = static_cast<qsizetype>(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETLENGTH));
        const QByteArray actual = tab->editor()->text().toUtf8();
        if (*length <= 0 || *length >= fullBytes || actual.size() != *length
            || actual != fixture.items[0].dirtyBody.left(*length)
            || actual.contains(fixture.items[0].canary))
            return QStringLiteral("exact partial prefix / missing EOF canary not established");
        return {};
    };
    QObject observationScope; // Dies/disconnects before every captured local and fixture.
    QObject::connect(&tasks, &AsyncTaskManager::taskAdded, &observationScope, [&](qint64 id) {
        if (closing && tasks.taskById(id).type == AsyncTaskType::SessionSnapshot)
            snapshotTasks.append(id);
    }, Qt::DirectConnection);
    QObject::connect(&tasks, &AsyncTaskManager::taskFinished, &observationScope,
                     [&](qint64 id, AsyncTaskState state) {
        if (snapshotTasks.contains(id)) snapshotTerminals.append(qMakePair(id, state));
    }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted, &observationScope,
                     [&] { ++restoreCompleted; }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::shutdownCommitted, &observationScope,
                     [&] { ++committedSignals; }, Qt::DirectConnection);
    QObject::connect(window->statusBar(), &QStatusBar::messageChanged, &observationScope,
                     [&](const QString &message) {
        // Do not parse integer percentages or displayed MB: the real byte prefix
        // is the oracle. Current code first reports about 2 MiB, not a timer seam.
        if (observed || !message.startsWith(QStringLiteral("正在加载文件…"))) return;
        observed = true;
        progressText = message;
        guardError = partialGuard(&observedLength);
        if (!guardError.isEmpty()) return;
        // NEVER close directly in a Scintilla/status signal. This queued public
        // operation runs in loadUtf8Chunked's existing processEvents, after the
        // completed append, while restoreSessionRecovery has not committed.
        queued = QMetaObject::invokeMethod(&observationScope, [&] {
            attempted = true;
            guardError = partialGuard(&dispatchLength);
            if (!guardError.isEmpty()) return;
            before = fixture.cut();
            const auto baselineIssues = retainedManifestIssues(fixture, before);
            if (!baselineIssues.isEmpty()) {
                guardError = baselineIssues.join(QLatin1Char('\n'));
                return;
            }
            for (int index = 0; index < 3; ++index) {
                if (before.sourceHashes[index] != fixture.items[index].sourceHash
                    || before.draftHashes[index] != fixture.items[index].draftHash
                    || !before.sealedAndDecryptable[index] || before.plaintextRecoveryLeak) {
                    guardError = QStringLiteral("source / complete encrypted draft changed before close");
                    return;
                }
            }
            closing = true;
            closeAccepted = window->close(); // Real QWidget API -> closeEvent, never private invocation.
            closing = false;
            if (!window) fixture.restarted.release(); // Avoid cleanup double-delete if ownership changes.
            // No tab/window access, event pumping, or waiting after close before
            // this business cut. Safe refusal/defer is allowed if full durable
            // references remain; accepting close is NOT itself required.
            after = fixture.cut();
            closeReturned = true;
            qInfo() << "RECOVERY_INFLIGHT_CLOSE_CUT" << "progress" << progressText
                    << "observedBytes" << observedLength << "partialBytes" << dispatchLength
                    << "fullBytes" << fullBytes
                    << "accepted" << closeAccepted << "shutdownCommitted" << committedSignals
                    << "snapshotTaskIds" << snapshotTasks << "terminals" << snapshotTerminals.size()
                    << "manifestItems" << after.session.tabs.size();
            for (const auto &terminal : snapshotTerminals)
                qInfo() << "RECOVERY_INFLIGHT_SNAPSHOT_TERMINAL" << terminal.first << int(terminal.second);
        }, Qt::QueuedConnection);
        if (!queued) guardError = QStringLiteral("public close could not be queued");
    }, Qt::DirectConnection);

    window->deferredRestoreSession();
    QTRY_VERIFY_WITH_TIMEOUT(observed, 5000);
    QVERIFY2(guardError.isEmpty(), qPrintable(guardError));
    QVERIFY(queued);
    QTRY_VERIFY_WITH_TIMEOUT(attempted, 5000);
    QVERIFY2(guardError.isEmpty(), qPrintable(guardError));
    QVERIFY(closeReturned);
    QVERIFY(dispatchLength > 0 && dispatchLength < fullBytes);
    QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
    QVERIFY(!before.plaintextRecoveryLeak && !after.plaintextRecoveryLeak);
    for (int index = 0; index < 3; ++index) {
        QCOMPARE(before.sourceHashes[index], fixture.items[index].sourceHash);
        QCOMPARE(after.sourceHashes[index], fixture.items[index].sourceHash);
        QCOMPARE(before.draftHashes[index], fixture.items[index].draftHash);
        QVERIFY(before.sealedAndDecryptable[index]);
    }
    // Do not demand one snapshot, a particular ciphertext nonce, or successful
    // close: retaining the complete old durable responsibility is a safe fix.
    // A newly sealed PARTIAL untitled snapshot cannot satisfy these full-body,
    // original-source, exactly-one-responsibility DPAPI checks.
    const auto issues = retainedManifestIssues(fixture, after);
    QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
    // The original close request may safely defer while projection unwinds, but
    // must eventually commit normal shutdown. Permanently ignoring close fails.
    QTRY_VERIFY_WITH_TIMEOUT(!window || !window->isVisible(), 5000);
    QVERIFY(committedSignals > 0);
    if (!window) fixture.restarted.release();
    QTRY_VERIFY_WITH_TIMEOUT(!AsyncSerialQueue::session().busy(), 5000);
    const DiskCut closed = fixture.cut();
    const auto closedIssues = retainedManifestIssues(fixture, closed);
    QVERIFY2(closedIssues.isEmpty(), qPrintable(closedIssues.join(QLatin1Char('\n'))));
    for (int index = 0; index < 3; ++index)
        QCOMPARE(closed.sourceHashes[index], fixture.items[index].sourceHash);
    QVERIFY(!closed.plaintextRecoveryLeak);
}

void RecoveryPreservationSmoke::cancelingCloseAfterFirstRestoredItemContinuesExactlyOnce()
{
    RecoveryFixture fixture;
    QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
    QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
    QPointer<MainWindow> window(fixture.restarted.get());
    const QString userPath = fixture.temporary.filePath(QStringLiteral("user-cancel-close.txt"));
    const QByteArray userOriginal = QByteArrayLiteral("ordinary file opened by the user\n");
    const QByteArray userAdded = QByteArrayLiteral("USER_UNSAVED_CANCEL_CLOSE_CANARY\n");
    const QByteArray userDirty = userOriginal + userAdded;
    QFile userSource(userPath);
    QVERIFY(userSource.open(QIODevice::WriteOnly));
    QCOMPARE(userSource.write(userOriginal), userOriginal.size());
    QVERIFY(userSource.flush());
    userSource.close();
    const QByteArray userHash = hashFile(userPath);
    QVERIFY(!userHash.isEmpty());
    QPointer<EditorTab> userTab;
    bool intercepted = false, closeReturned = false, closeAccepted = true;
    int restored = 0, committed = 0;
    QString guardError;
    DiskCut before, canceled, completion;
    QObject observationScope;
    QObject::connect(window.data(), &MainWindow::shutdownCommitted, &observationScope,
                     [&] { ++committed; }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted, &observationScope, [&] {
        ++restored;
        completion = fixture.cut(userPath, userDirty);
    }, Qt::DirectConnection);
    QObject::connect(window->statusBar(), &QStatusBar::messageChanged, &observationScope,
                     [&](const QString &message) {
        if (intercepted || message != QStringLiteral("正在恢复会话… 1/3 个标签页")) return;
        intercepted = true;
        guardError = fixture.restoredGuard(1);
        if (!guardError.isEmpty() || restored != 0) return;
        before = fixture.cut();
        const auto baselineIssues = retainedManifestIssues(fixture, before);
        if (!baselineIssues.isEmpty()) {
            guardError = baselineIssues.join(QLatin1Char('\n'));
            return;
        }
        for (int index = 0; index < 3; ++index) {
            if (before.sourceHashes[index] != fixture.items[index].sourceHash
                || before.draftHashes[index] != fixture.items[index].draftHash
                || !before.sealedAndDecryptable[index]) {
                guardError = QStringLiteral("original draft responsibility changed before user cancel");
                return;
            }
        }
        window->openFilesFromExternal({userPath});
        auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
        userTab = tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
        if (!userTab || tabs->count() != 2 || userTab->filePath() != userPath
            || userTab->isSecureDocument() || userTab->isL2Mode()
            || userTab->editor()->text().toUtf8() != userOriginal) {
            guardError = QStringLiteral("real user ordinary open did not establish the second tab");
            return;
        }
        userTab->editor()->append(QString::fromLatin1(userAdded));
        // Exact production prompt branch: named, modified, no autosave fallback.
        if (!userTab || !userTab->isModified() || userTab->isUntitled() || !userTab->autosavePath().isEmpty()
            || userTab->editor()->text().toUtf8() != userDirty || hashFile(userPath) != userHash) {
            guardError = QStringLiteral("named dirty no-autosave prompt prerequisite not established");
            return;
        }
        fixture.dialogs.cancelSaveForFile = QFileInfo(userPath).fileName();
        closeAccepted = window->close();
        fixture.dialogs.cancelSaveForFile.clear();
        if (!window) fixture.restarted.release();
        canceled = fixture.cut(userPath, userDirty); // Before canceled close callback returns to recovery.
        closeReturned = true;
        qInfo() << "RECOVERY_USER_CANCEL_CUT" << "accepted" << closeAccepted
                << "prompts" << fixture.dialogs.cancelSavePrompts
                << "clicks" << fixture.dialogs.cancelClicks
                << "manifestItems" << canceled.session.tabs.size();
    }, Qt::DirectConnection);

    window->deferredRestoreSession();
    QTRY_VERIFY_WITH_TIMEOUT(intercepted, 5000);
    QVERIFY2(guardError.isEmpty(), qPrintable(guardError));
    QVERIFY(closeReturned);
    QVERIFY(!closeAccepted);
    QCOMPARE(fixture.dialogs.cancelSavePrompts, 1);
    QCOMPARE(fixture.dialogs.cancelClicks, 1);
    QCOMPARE(committed, 0);
    QVERIFY(window && window->isVisible());
    const auto canceledIssues = retainedManifestIssues(fixture, canceled);
    QVERIFY2(canceledIssues.isEmpty(), qPrintable(canceledIssues.join(QLatin1Char('\n'))));
    QTRY_COMPARE_WITH_TIMEOUT(restored, 1, 5000);
    // Bounded post-completion event observation catches duplicate queue restart.
    QTest::qWait(100);
    QCOMPARE(restored, 1);
    QCOMPARE(committed, 0);
    QVERIFY(window && window->isVisible());
    auto *tabs = window->findChild<QTabWidget *>();
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 4);
    QVERIFY(userTab && tabs->indexOf(userTab) >= 0);
    QCOMPARE(userTab->filePath(), userPath);
    QCOMPARE(userTab->editor()->text().toUtf8(), userDirty);
    QVERIFY(userTab->isModified() && !userTab->isSecureDocument());
    QCOMPARE(hashFile(userPath), userHash);
    QCOMPARE(readFile(userPath), userOriginal);
    for (int index = 0; index < 3; ++index) {
        int matches = 0;
        for (int tabIndex = 0; tabIndex < tabs->count(); ++tabIndex) {
            auto *tab = qobject_cast<EditorTab *>(tabs->widget(tabIndex));
            if (!tab || tab->filePath() != fixture.items[index].sourcePath) continue;
            ++matches;
            QCOMPARE(tab->editor()->text().toUtf8(), fixture.items[index].dirtyBody);
            QVERIFY(tab->isModified() && !tab->isSecureDocument());
        }
        QCOMPARE(matches, 1);
        QCOMPARE(canceled.sourceHashes[index], fixture.items[index].sourceHash);
    }
    const DiskCut late = fixture.cut(userPath, userDirty);
    // Completion may legitimately publish the user tab as a fourth dirty item;
    // neither that publication nor a still-valid old three-item point is forbidden.
    for (const DiskCut *cut : std::array<const DiskCut *, 2>{&completion, &late}) {
        QVERIFY(cut->session.tabs.size() == 3 || cut->session.tabs.size() == 4);
        const auto issues = retainedManifestIssues(fixture, *cut, cut->session.tabs.size());
        QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
        if (cut->session.tabs.size() == 4) {
            int userRecords = 0;
            for (const auto &record : cut->session.tabs) {
                if (record.filePath != userPath) continue;
                ++userRecords;
                QVERIFY(record.modified && !record.secureDocument);
            }
            QCOMPARE(userRecords, 1);
            QCOMPARE(cut->extraRecoverableReference, 1);
        }
        for (int index = 0; index < 3; ++index)
            QCOMPARE(cut->sourceHashes[index], fixture.items[index].sourceHash);
        QVERIFY(!cut->plaintextRecoveryLeak);
    }
    QVERIFY(!before.plaintextRecoveryLeak && !canceled.plaintextRecoveryLeak);
    QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
}

void RecoveryPreservationSmoke::completedRecoverySavesReorderedTabsAndActiveIndex()
{
    RecoveryFixture fixture;
    QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
    QVERIFY2(fixture.showRestarted(), qPrintable(fixture.error));
    QPointer<MainWindow> window(fixture.restarted.get());
    int completed = 0;
    QObject observationScope;
    QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted, &observationScope,
                     [&] { ++completed; }, Qt::DirectConnection);
    window->deferredRestoreSession();
    QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 5000);
    const QString guard = fixture.restoredGuard(3);
    QVERIFY2(guard.isEmpty(), qPrintable(guard));
    QVERIFY(window);
    auto *tabs = window->findChild<QTabWidget *>();
    QVERIFY(tabs && tabs->tabBar());
    QPointer<EditorTab> moved(qobject_cast<EditorTab *>(tabs->widget(2)));
    QVERIFY(moved);
    QCOMPARE(moved->filePath(), fixture.items[2].sourcePath);
    // Public ordering API, not a native drag-and-drop or mouse-geometry test.
    tabs->tabBar()->moveTab(2, 0);
    tabs->setCurrentWidget(moved);
    QCOMPARE(tabs->widget(0), moved.data());
    QCOMPARE(tabs->currentWidget(), moved.data());
    const int activeIndex = tabs->currentIndex();
    QCOMPARE(activeIndex, 0);
    QStringList visibleOrder;
    for (int index = 0; index < tabs->count(); ++index) {
        auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
        QVERIFY(tab);
        visibleOrder.append(tab->filePath());
    }
    const QStringList expectedOrder{fixture.items[2].sourcePath, fixture.items[0].sourcePath,
                                    fixture.items[1].sourcePath};
    QCOMPARE(visibleOrder, expectedOrder);
    // Do not require a pre-close manifest: older finish removes it, while the
    // public normal close still must persist the actual visible order correctly.
    for (int index = 0; index < 3; ++index)
        QCOMPARE(hashFile(fixture.items[index].sourcePath), fixture.items[index].sourceHash);
    const bool accepted = window->close();
    const DiskCut closed = fixture.cut();
    // No raw tab/widget access after close. This is a forced normal-close save,
    // not a direct SessionManager call with a test-supplied desired ordering.
    QVERIFY(accepted);
    QVERIFY(!window || !window->isVisible());
    QVERIFY(!AsyncSerialQueue::session().busy());
    QCOMPARE(closed.session.activeTab, activeIndex);
    QStringList persistedOrder;
    for (const auto &record : closed.session.tabs)
        persistedOrder.append(record.filePath);
    QCOMPARE(persistedOrder, visibleOrder);
    const auto issues = retainedManifestIssues(fixture, closed);
    QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
    for (int index = 0; index < 3; ++index)
        QCOMPARE(closed.sourceHashes[index], fixture.items[index].sourceHash);
    QVERIFY(!closed.plaintextRecoveryLeak);
    QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
}

void RecoveryPreservationSmoke::closingAfterFirstPaintBeforeRecoveryStartsRetainsDrafts()
{
    RecoveryFixture fixture;
    QVERIFY2(fixture.prepare(), qPrintable(fixture.error));
    // This case must connect before show. showRestarted waits for exposure, but
    // a successful early close can hide this window before that wait returns.
    fixture.restarted.reset(new MainWindow);
    QPointer<MainWindow> window(fixture.restarted.get());
    QObject::connect(window.data(), &QObject::destroyed, [&fixture](QObject *object) {
        if (fixture.restarted.get() == object) fixture.restarted.release();
    });
    StartupCoordinator startup;
    int painted = 0, completed = 0, completedAtCut = -1, tabsAtCut = -1;
    int committed = 0;
    bool reached = false, closing = false, accepted = false, closeReturned = false;
    QString guardError;
    DiskCut before, closed;
    QList<qint64> snapshotTasks;
    QList<QPair<qint64, AsyncTaskState>> terminals;
    auto &tasks = AsyncTaskManager::instance();
    QObject observationScope;
    QObject::connect(window.data(), &MainWindow::firstPaintCompleted, &observationScope,
                     [&] { ++painted; }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::sessionRestoreCompleted, &observationScope,
                     [&] { ++completed; }, Qt::DirectConnection);
    QObject::connect(window.data(), &MainWindow::shutdownCommitted, &observationScope,
                     [&] { if (closing) ++committed; }, Qt::DirectConnection);
    QObject::connect(&tasks, &AsyncTaskManager::taskAdded, &observationScope, [&](qint64 id) {
        if (closing && tasks.taskById(id).type == AsyncTaskType::SessionSnapshot) snapshotTasks.append(id);
    }, Qt::DirectConnection);
    QObject::connect(&tasks, &AsyncTaskManager::taskFinished, &observationScope,
                     [&](qint64 id, AsyncTaskState state) {
        if (snapshotTasks.contains(id)) terminals.append(qMakePair(id, state));
    }, Qt::DirectConnection);
    // Actual startup orchestration: this public signal is emitted on a clean
    // queued turn after firstPaintCompleted. Its second queued callback (which
    // calls deferredRestoreSession) is not posted until this receiver returns.
    QObject::connect(&startup, &StartupCoordinator::postPaintIdleTurnCompleted,
                     &observationScope, [&] {
        reached = true;
        auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
        if (painted != 1 || completed != 0 || !tabs || tabs->count() != 0) {
            guardError = QStringLiteral("not the real first-painted, zero-body, pre-recovery cut");
            return;
        }
        tabsAtCut = tabs->count();
        completedAtCut = completed;
        before = fixture.cut();
        const auto baselineIssues = retainedManifestIssues(fixture, before);
        if (!baselineIssues.isEmpty()) {
            guardError = baselineIssues.join(QLatin1Char('\n'));
            return;
        }
        for (int index = 0; index < 3; ++index) {
            if (before.sourceHashes[index] != fixture.items[index].sourceHash
                || before.draftHashes[index] != fixture.items[index].draftHash
                || !before.sealedAndDecryptable[index] || before.plaintextRecoveryLeak) {
                guardError = QStringLiteral("complete original recovery point changed before early close");
                return;
            }
        }
        closing = true;
        accepted = window->close();
        closing = false;
        closed = fixture.cut(); // Before startup can enqueue its deferred recovery callback.
        closeReturned = true;
        qInfo() << "RECOVERY_PRE_START_CLOSE_CUT" << "paints" << painted
                << "bodies" << tabsAtCut << "completed" << completedAtCut
                << "accepted" << accepted << "committed" << committed
                << "snapshotTaskIds" << snapshotTasks << "manifestItems" << closed.session.tabs.size();
    }, Qt::DirectConnection);
    startup.observe(window.data(), {});
    window->resize(1100, 750);
    window->show();
    startup.releasePostPaintWork();
    QTRY_VERIFY_WITH_TIMEOUT(reached, 5000);
    QVERIFY2(guardError.isEmpty(), qPrintable(guardError));
    QVERIFY(closeReturned && accepted);
    QCOMPARE(tabsAtCut, 0);
    QCOMPARE(completedAtCut, 0);
    QCOMPARE(committed, 1);
    QCOMPARE(snapshotTasks.size(), 1);
    QCOMPARE(terminals.size(), 1);
    QCOMPARE(terminals.front().first, snapshotTasks.front());
    QCOMPARE(terminals.front().second, AsyncTaskState::Finished);
    const auto issues = retainedManifestIssues(fixture, closed);
    QVERIFY2(issues.isEmpty(), qPrintable(issues.join(QLatin1Char('\n'))));
    for (int index = 0; index < 3; ++index)
        QCOMPARE(closed.sourceHashes[index], fixture.items[index].sourceHash);
    QVERIFY(!closed.plaintextRecoveryLeak);
    QVERIFY2(fixture.dialogs.unexpected.isEmpty(), qPrintable(fixture.dialogs.unexpected.join(QLatin1Char(';'))));
}

QTEST_MAIN(RecoveryPreservationSmoke)
#include "smoke_recovery_preservation.moc"
