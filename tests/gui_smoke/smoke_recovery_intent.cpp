// 3A-2 behavior probes: real SessionManager/DPAPI records and StartupCoordinator.
// The runner must impose a process deadline: a QtTest wait cannot interrupt a
// nested production recovery operation. These are not input-latency benchmarks.
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <Qsci/qsciscintilla.h>

#include <array>
#include <memory>

#include "AsyncSerialQueue.h"
#include "DraftStore.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "SessionManager.h"
#include "StartupCoordinator.h"
#include "VirtualFileView.h"

namespace {
QByteArray fileHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray{};
}

bool writeSource(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QTabWidget *tabsOf(MainWindow *window)
{
    return window ? window->findChild<QTabWidget *>() : nullptr;
}

void destroyWithoutSaving(MainWindow *window)
{
    if (!window) return;
    if (auto *tabs = tabsOf(window)) {
        for (int i = 0; i < tabs->count(); ++i)
            if (auto *tab = qobject_cast<EditorTab *>(tabs->widget(i))) tab->shutdownL2BeforeClose();
    }
    delete window; // Cleanup only: neither a graceful save nor a process-crash proof.
}
using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutSaving)>;

class UnexpectedDialogs final : public QObject {
public:
    UnexpectedDialogs() { qApp->installEventFilter(this); }
    ~UnexpectedDialogs() override { qApp->removeEventFilter(this); }
    QStringList seen;
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(object);
            if (dialog && dialog->isModal()
                && dialog->objectName() != QStringLiteral("sessionCommitProgress")) {
                seen.append(dialog->objectName() + QLatin1Char(':') + dialog->windowTitle());
                const QPointer<QDialog> guarded(dialog);
                QTimer::singleShot(0, this, [guarded] { if (guarded) guarded->reject(); });
            }
        }
        return QObject::eventFilter(object, event);
    }
};

struct Item {
    QString path;
    QByteArray original;
    QByteArray body;
    QByteArray sourceHash;
    QByteArray draftHash;
    QByteArray draftPlaintext;
    SessionTab record;
    bool binary = false;
    bool dirty = true;
};

class IntentFixture final {
public:
    ~IntentFixture()
    {
        // Observation contexts and StartupCoordinator are later locals and have
        // already disconnected. Do not destroy a manager still owned by a job.
        if (!QTest::qWaitFor([] { return !AsyncSerialQueue::session().busy(); }, 5000))
            qFatal("INTENT_CLEANUP_TIMEOUT: session worker still owns its manager");
        restarted.reset();
        manager.reset();
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", oldProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool prepare(bool mixed)
    {
        if (!root.isValid()) return fail(root.errorString());
        profile = root.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile)) return fail(QStringLiteral("cannot create isolated profile"));
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        profileSet_ = qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        if (!profileSet_ || AsyncSerialQueue::session().busy())
            return fail(QStringLiteral("profile isolation/session lane prerequisite failed"));
        manager = std::make_unique<SessionManager>(profile);
        WindowOwner seed(new MainWindow, destroyWithoutSaving);
        seed->resize(1100, 750);
        seed->show();
        seed->raise();
        seed->activateWindow();
        if (!QTest::qWaitForWindowExposed(seed.get())) return fail(QStringLiteral("seed not exposed"));
        QVector<ISessionDocumentPort *> documents;
        for (int i = 0; i < 3; ++i) {
            Item &item = items[i];
            item.binary = mixed && i == 2;
            item.dirty = !mixed || i != 0;
            item.path = root.filePath(QStringLiteral("source-%1.%2").arg(i)
                .arg(item.binary ? QStringLiteral("bin") : QStringLiteral("txt")));
            if (item.binary) {
                item.original.resize(4096); // Actual editable B1, not a large-file timing workload.
                for (int byte = 0; byte < item.original.size(); ++byte)
                    item.original[byte] = char((byte * 73 + 19) & 0xff);
                item.original[0] = '\0';
            } else {
                item.original = QByteArrayLiteral("original ordinary source ") + QByteArray::number(i) + '\n';
            }
            item.body = item.original;
            if (!writeSource(item.path, item.original)) return fail(QStringLiteral("source write failed"));
            item.sourceHash = fileHash(item.path);
            seed->openFilesFromExternal({item.path});
            auto *tabs = tabsOf(seed.get());
            auto *tab = tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
            if (!tab || tab->filePath() != item.path || tab->isSecureDocument())
                return fail(QStringLiteral("real ordinary open failed"));
            if (item.binary) {
                if (!QTest::qWaitFor([&] {
                        return tab->isBinaryDocument() && tab->l2View() && tab->l2View()->isOpen()
                            && tab->viewModeL2() == ViewMode::Hex;
                    }, 5000) || !tab->l2View()->replaceBytes(64, 4, QByteArrayLiteral("R3A!")))
                    return fail(QStringLiteral("real B1 byte edit failed"));
                item.body.replace(64, 4, QByteArrayLiteral("R3A!"));
                tab->l2View()->selectBytes(64, 68);
            } else {
                if (item.dirty) {
                    const QByteArray addition = QByteArrayLiteral("UNSAVED_INTENT_") + QByteArray::number(i) + '\n';
                    item.body += addition;
                    tab->editor()->append(QString::fromLatin1(addition));
                }
                tab->editor()->setSelection(0, 1, 0, 4);
            }
            if (tab->isModified() != item.dirty || tab->isReadOnlyByPolicy()
                || tab->documentState().id.isNull()) return fail(QStringLiteral("seed capability/dirty/id mismatch"));
            documents.append(tab);
        }
        // Public capture -> encrypted DraftStore -> real QSaveFile publication.
        // No handwritten manifest and no test port substituted for an editor.
        if (!manager->saveSession(documents, 2, true)) return fail(QStringLiteral("session save failed"));
        const SessionData saved = manager->loadSession();
        if (saved.tabs.size() != 3 || saved.activeTab != 2)
            return fail(QStringLiteral("three ordered records/active tail not persisted"));
        for (int i = 0; i < 3; ++i) {
            Item &item = items[i];
            item.record = saved.tabs.at(i);
            if (item.record.filePath != item.path || item.record.modified != item.dirty
                || item.record.secureDocument) return fail(QStringLiteral("captured metadata mismatch"));
            if (item.dirty) {
                DraftStore::SnapshotInfo info;
                if (!DraftStore::isEncryptedDraft(item.record.autosavePath)
                    || !DraftStore::inspect(item.record.autosavePath, &info)
                    || !DraftStore::readSnapshot(item.record.autosavePath, &item.draftPlaintext))
                    return fail(QStringLiteral("real DPAPI recovery is not readable"));
                if (item.binary ? info.kind != DraftStore::PayloadKind::BinaryPatchJournal
                                : item.draftPlaintext != item.body)
                    return fail(QStringLiteral("encrypted payload kind/body mismatch"));
                item.draftHash = fileHash(item.record.autosavePath);
                if (item.draftHash.isEmpty() || item.draftPlaintext.isEmpty())
                    return fail(QStringLiteral("empty encrypted recovery evidence"));
            } else if (!item.record.autosavePath.isEmpty()) {
                return fail(QStringLiteral("clean record unexpectedly has a recovery draft"));
            }
            const QString guard = documentGuard(dynamic_cast<EditorTab *>(documents[i]), i);
            if (!guard.isEmpty()) return fail(guard);
        }
        seed.reset();
        const QString diskError = diskGuard();
        return diskError.isEmpty() || fail(diskError);
    }

    int itemIndex(EditorTab *tab) const
    {
        for (int i = 0; tab && i < 3; ++i)
            if (tab->filePath() == items[i].path) return i;
        return -1;
    }

    QString documentGuard(EditorTab *tab, int index) const
    {
        if (!tab || index < 0 || index >= 3) return QStringLiteral("no matching completed document");
        const Item &item = items[index];
        const auto &state = tab->documentState();
        if (tab->filePath() != item.path || state.id.isNull()
            || tab->isModified() != item.dirty || state.dirty != item.dirty
            || tab->isSecureDocument() || state.securityClass != DocumentSecurityClass::OrdinaryDocument
            || !state.capabilities.testFlag(DocumentCapability::Read)
            || !state.capabilities.testFlag(DocumentCapability::Edit) || tab->isReadOnlyByPolicy())
            return QStringLiteral("document path/id/dirty/ordinary capabilities changed");
        QByteArray body;
        if (item.binary) {
            if (!tab->isBinaryDocument() || !tab->l2View() || !tab->l2View()->isOpen()
                || tab->viewModeL2() != ViewMode::Hex
                || tab->l2View()->virtualFileSize() != item.body.size())
                return QStringLiteral("B1 journal not fully projected");
            body = tab->l2View()->readByteRange(0, item.body.size());
        } else {
            if (tab->isL2Mode() || tab->isBinaryDocument()) return QStringLiteral("S1 changed surface");
            body = tab->editor()->text().toUtf8();
        }
        if (body != item.body) return QStringLiteral("complete restored bytes differ");
        const SessionTab capture = tab->captureSessionTab();
        if (capture.selectionStartByte != item.record.selectionStartByte
            || capture.selectionEndByte != item.record.selectionEndByte)
            return QStringLiteral("restored byte selection differs");
        return {};
    }

    QString diskGuard() const
    {
        const SessionData disk = manager->loadSession();
        if (disk.tabs.size() != 3 || disk.activeTab != 2)
            return QStringLiteral("published responsibility count/active changed");
        for (int i = 0; i < 3; ++i) {
            const Item &item = items[i];
            const auto &record = disk.tabs.at(i);
            if (fileHash(item.path) != item.sourceHash || item.sourceHash.isEmpty()
                || record.filePath != item.path || record.modified != item.dirty
                || record.autosavePath != item.record.autosavePath || record.secureDocument)
                return QStringLiteral("source hash or ordered manifest responsibility changed");
            if (item.dirty) {
                QByteArray decoded;
                if (fileHash(record.autosavePath) != item.draftHash
                    || !DraftStore::isEncryptedDraft(record.autosavePath)
                    || !DraftStore::readSnapshot(record.autosavePath, &decoded)
                    || decoded != item.draftPlaintext)
                    return QStringLiteral("complete encrypted recovery changed");
            }
        }
        return {};
    }

    QTemporaryDir root;
    UnexpectedDialogs dialogs;
    QString profile;
    QString error;
    std::array<Item, 3> items;
    std::unique_ptr<SessionManager> manager;
    WindowOwner restarted{nullptr, destroyWithoutSaving};
private:
    bool fail(const QString &reason) { error = reason; return false; }
    QByteArray oldProfile_;
    bool hadProfile_ = false;
    bool profileSet_ = false;
};
} // namespace

class RecoveryIntentSmoke final : public QObject {
    Q_OBJECT
    bool quitOnLastWindow_ = true;
private slots:
    void initTestCase()
    {
        QVERIFY2(QGuiApplication::platformName() == QStringLiteral("windows"), "Real Windows DPAPI/native Qt required");
        quitOnLastWindow_ = qApp->quitOnLastWindowClosed();
        qApp->setQuitOnLastWindowClosed(false);
    }
    void cleanupTestCase() { qApp->setQuitOnLastWindowClosed(quitOnLastWindow_); }

    void previousActiveB1RestoresFirstWithoutReordering();
    void userTabClickDuringRecoveryKeepsCurrentDocument();
    void userReorderDuringRecoverySurvivesFinishAndSave();
};

void RecoveryIntentSmoke::previousActiveB1RestoresFirstWithoutReordering()
{
    IntentFixture fixture;
    QVERIFY2(fixture.prepare(true), qPrintable(fixture.error));
    fixture.restarted.reset(new MainWindow);
    auto *window = fixture.restarted.get();
    int completed = 0, cuts = 0, firstIndex = -1;
    QString cutError;
    QPointer<EditorTab> first;
    QUuid firstId;
    quint64 firstRevision = 0;
    StartupCoordinator startup;
    QObject observation; // Destroy connections before the captured evidence/fixture.
    connect(window, &MainWindow::sessionRestoreCompleted, &observation, [&] { ++completed; });
    connect(window->statusBar(), &QStatusBar::messageChanged, &observation, [&](const QString &message) {
        if (message != QStringLiteral("正在恢复会话… 1/3 个标签页")) return;
        if (++cuts != 1) return;
        auto *tabs = tabsOf(window);
        if (completed != 0 || !tabs || tabs->count() != 1) {
            cutError = QStringLiteral("1/3 did not expose exactly one complete item before completion");
            return;
        }
        first = qobject_cast<EditorTab *>(tabs->widget(0));
        firstIndex = fixture.itemIndex(first);
        cutError = fixture.documentGuard(first, firstIndex);
        if (cutError.isEmpty()) cutError = fixture.diskGuard();
        if (!cutError.isEmpty()) return;
        firstId = first->documentState().id;
        firstRevision = first->contentRevision();
        qInfo() << "INTENT_ACTIVE_FIRST_CUT_GUARDS_OK" << "recordIndex" << firstIndex;
    }, Qt::DirectConnection);
    startup.observe(window, {});
    window->resize(1100, 750);
    window->show();
    window->raise();
    window->activateWindow();
    startup.releasePostPaintWork();
    QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 10000);
    QCOMPARE(cuts, 1);
    QVERIFY2(cutError.isEmpty(), qPrintable(cutError));
    QVERIFY(first);
    QCOMPARE(first->documentState().id, firstId); // In-session identity, not a cross-restart UUID assertion.
    QCOMPARE(first->contentRevision(), firstRevision);
    auto *tabs = tabsOf(window);
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 3);
    QSet<QUuid> ids;
    for (int i = 0; i < 3; ++i) {
        auto *tab = qobject_cast<EditorTab *>(tabs->widget(i));
        const QString guard = fixture.documentGuard(tab, i);
        QVERIFY2(guard.isEmpty(), qPrintable(guard));
        ids.insert(tab->documentState().id);
    }
    QCOMPARE(ids.size(), 3);
    const QString diskError = fixture.diskGuard();
    QVERIFY2(diskError.isEmpty(), qPrintable(diskError));
    QVERIFY2(fixture.dialogs.seen.isEmpty(), qPrintable(fixture.dialogs.seen.join(QLatin1Char(';'))));
    qInfo() << "INTENT_ACTIVE_FIRST_FINAL_DATA_GUARDS_OK";
    QCOMPARE(firstIndex, 2); // Product red only after complete bytes/selection/identity/disk guards.
    QCOMPARE(tabs->currentWidget(), tabs->widget(2));
}

void RecoveryIntentSmoke::userTabClickDuringRecoveryKeepsCurrentDocument()
{
    IntentFixture fixture;
    QVERIFY2(fixture.prepare(false), qPrintable(fixture.error));
    const QString externalPath = fixture.root.filePath(QStringLiteral("explicit-preferred.txt"));
    const QByteArray externalBody = QByteArrayLiteral("explicit ordinary file remains unchanged\n");
    QVERIFY(writeSource(externalPath, externalBody));
    const QByteArray externalHash = fileHash(externalPath);
    QVERIFY(!externalHash.isEmpty());
    fixture.restarted.reset(new MainWindow);
    auto *window = fixture.restarted.get();
    // Actual public external open before recovery captures the preferred tab.
    window->openFilesFromExternal({externalPath});
    auto *tabs = tabsOf(window);
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 1);
    QPointer<EditorTab> preferred = qobject_cast<EditorTab *>(tabs->currentWidget());
    QVERIFY(preferred);
    QCOMPARE(preferred->filePath(), externalPath);
    QCOMPARE(preferred->editor()->text().toUtf8(), externalBody);
    QVERIFY(!preferred->isModified());
    preferred->editor()->setSelection(0, 1, 0, 4);
    const SessionTab preferredView = preferred->captureSessionTab();
    const QUuid preferredId = preferred->documentState().id;
    QVERIFY(!preferredId.isNull());
    const quint64 preferredRevision = preferred->contentRevision();
    int completed = 0, cuts = 0, clickedIndex = -1;
    bool clickChangedCurrent = false;
    QString cutError;
    QPointer<EditorTab> clicked;
    QUuid clickedId;
    quint64 clickedRevision = 0;
    StartupCoordinator startup;
    QObject observation;
    connect(window, &MainWindow::sessionRestoreCompleted, &observation, [&] { ++completed; });
    connect(window->statusBar(), &QStatusBar::messageChanged, &observation, [&](const QString &message) {
        if (message != QStringLiteral("正在恢复会话… 1/3 个标签页")) return;
        if (++cuts != 1) return;
        if (completed != 0 || tabs->count() != 2 || tabs->currentWidget() != preferred) {
            cutError = QStringLiteral("preferred plus one fully restored item prerequisite failed");
            return;
        }
        clicked = qobject_cast<EditorTab *>(tabs->widget(tabs->widget(0) == preferred ? 1 : 0));
        clickedIndex = fixture.itemIndex(clicked);
        cutError = fixture.documentGuard(clicked, clickedIndex);
        if (cutError.isEmpty()) cutError = fixture.diskGuard();
        if (!cutError.isEmpty()) return;
        clickedId = clicked->documentState().id;
        clickedRevision = clicked->contentRevision();
        auto *bar = tabs->tabBar();
        const QRect rect = bar->tabRect(tabs->indexOf(clicked));
        if (!bar->isVisible() || !bar->isEnabled() || !bar->rect().contains(rect.center())) {
            cutError = QStringLiteral("restored tab has no actual clickable native tab-bar rectangle");
            return;
        }
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, rect.center());
        clickChangedCurrent = tabs->currentWidget() == clicked && clicked != preferred;
        if (!clickChangedCurrent) cutError = QStringLiteral("actual QTest tab click did not change current document");
        qInfo() << "INTENT_USER_CLICK_CUT_GUARDS_OK" << "changed" << clickChangedCurrent
                << "recordIndex" << clickedIndex << "completed" << completed;
    }, Qt::DirectConnection);
    startup.observe(window, {});
    window->resize(1100, 750);
    window->show();
    window->raise();
    window->activateWindow();
    startup.releasePostPaintWork();
    QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 10000);
    QCOMPARE(cuts, 1);
    QVERIFY2(cutError.isEmpty(), qPrintable(cutError));
    QVERIFY(clickChangedCurrent && clicked && preferred);
    QCOMPARE(tabs->count(), 4);
    QCOMPARE(clicked->documentState().id, clickedId);
    QCOMPARE(clicked->contentRevision(), clickedRevision);
    QCOMPARE(preferred->documentState().id, preferredId);
    QCOMPARE(preferred->contentRevision(), preferredRevision);
    QCOMPARE(preferred->editor()->text().toUtf8(), externalBody);
    QVERIFY(!preferred->isModified() && !preferred->isSecureDocument());
    QCOMPARE(preferred->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
    QVERIFY(preferred->documentState().capabilities.testFlag(DocumentCapability::Read));
    QVERIFY(preferred->documentState().capabilities.testFlag(DocumentCapability::Edit));
    QCOMPARE(preferred->captureSessionTab().selectionStartByte, preferredView.selectionStartByte);
    QCOMPARE(preferred->captureSessionTab().selectionEndByte, preferredView.selectionEndByte);
    QCOMPARE(fileHash(externalPath), externalHash);
    QSet<QUuid> ids;
    std::array<int, 3> occurrences{};
    QStringList restoredOrder;
    int externalOccurrences = 0;
    for (int i = 0; i < tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(tabs->widget(i));
        QVERIFY(tab);
        ids.insert(tab->documentState().id);
        if (tab->filePath() == externalPath) {
            ++externalOccurrences;
            QCOMPARE(tab, preferred.data());
            continue;
        }
        const int index = fixture.itemIndex(tab);
        const QString guard = fixture.documentGuard(tab, index);
        QVERIFY2(guard.isEmpty(), qPrintable(guard));
        ++occurrences[index];
        restoredOrder.append(tab->filePath());
    }
    QCOMPARE(ids.size(), 4);
    QCOMPARE(externalOccurrences, 1);
    for (int count : occurrences) QCOMPARE(count, 1);
    QCOMPARE(restoredOrder, QStringList({fixture.items[0].path, fixture.items[1].path, fixture.items[2].path}));
    const QString diskError = fixture.diskGuard();
    QVERIFY2(diskError.isEmpty(), qPrintable(diskError));
    QVERIFY2(fixture.dialogs.seen.isEmpty(), qPrintable(fixture.dialogs.seen.join(QLatin1Char(';'))));
    qInfo() << "INTENT_USER_CLICK_FINAL_DATA_GUARDS_OK";
    QCOMPARE(tabs->currentWidget(), clicked.data()); // A previous preference cannot overwrite this actual user choice.
    QVERIFY(tabs->currentWidget() != preferred);
}

void RecoveryIntentSmoke::userReorderDuringRecoverySurvivesFinishAndSave()
{
    IntentFixture fixture;
    QVERIFY2(fixture.prepare(false), qPrintable(fixture.error));
    fixture.restarted.reset(new MainWindow);
    QPointer<MainWindow> window(fixture.restarted.get());
    auto *tabs = tabsOf(window);
    QVERIFY(tabs && tabs->tabBar());
    int completed = 0, cuts = 0, currentSignalsAfterClick = 0, unexpectedCurrentChanges = 0;
    bool watchCurrent = false, clickChangedCurrent = false;
    QString cutError;
    std::array<QPointer<EditorTab>, 2> delivered;
    std::array<int, 2> itemIndices{{-1, -1}};
    std::array<QUuid, 2> deliveredIds;
    std::array<quint64, 2> deliveredRevisions{};
    QPointer<EditorTab> chosen;
    StartupCoordinator startup;
    QObject observation; // Disconnect before all captured state and fixture cleanup.
    connect(window.data(), &QObject::destroyed, &observation, [&fixture](QObject *object) {
        if (fixture.restarted.get() == object) fixture.restarted.release();
    });
    connect(window.data(), &MainWindow::sessionRestoreCompleted, &observation,
            [&] { ++completed; }, Qt::DirectConnection);
    connect(tabs, &QTabWidget::currentChanged, &observation, [&](int) {
        if (!watchCurrent) return;
        ++currentSignalsAfterClick;
        if (!chosen || tabs->currentWidget() != chosen)
            ++unexpectedCurrentChanges;
    }, Qt::DirectConnection);
    connect(window->statusBar(), &QStatusBar::messageChanged, &observation, [&](const QString &message) {
        if (message != QStringLiteral("正在恢复会话… 2/3 个标签页")) return;
        if (++cuts != 1) return;
        if (completed != 0 || tabs->count() != 2) {
            cutError = QStringLiteral("2/3 did not expose two complete items before finish");
            return;
        }
        for (int index = 0; index < 2; ++index) {
            delivered[index] = qobject_cast<EditorTab *>(tabs->widget(index));
            itemIndices[index] = fixture.itemIndex(delivered[index]);
            cutError = fixture.documentGuard(delivered[index], itemIndices[index]);
            if (!cutError.isEmpty()) return;
            deliveredIds[index] = delivered[index]->documentState().id;
            deliveredRevisions[index] = delivered[index]->contentRevision();
        }
        if (itemIndices[0] == itemIndices[1] || deliveredIds[0] == deliveredIds[1]) {
            cutError = QStringLiteral("two delivered pages do not have distinct real identities");
            return;
        }
        cutError = fixture.diskGuard();
        if (!cutError.isEmpty()) return;
        // Do not assume which original records arrived first: both the old
        // sequential order and active-first candidate can reach this cut.
        auto *bar = tabs->tabBar();
        bar->moveTab(0, 1); // Public ordering API, not OS drag/drop latency evidence.
        if (tabs->widget(0) != delivered[1] || tabs->widget(1) != delivered[0]) {
            cutError = QStringLiteral("public moveTab did not reverse the delivered pair");
            return;
        }
        chosen = delivered[0];
        QWidget *beforeClick = tabs->currentWidget();
        const QRect rect = bar->tabRect(1);
        if (beforeClick == chosen || !bar->isVisible() || !bar->isEnabled()
            || !bar->rect().contains(rect.center())) {
            cutError = QStringLiteral("moved first page is not a distinct clickable choice");
            return;
        }
        watchCurrent = true; // Include the real click's synchronous currentChanged.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, rect.center());
        clickChangedCurrent = tabs->currentWidget() == chosen && beforeClick != chosen;
        if (!clickChangedCurrent || completed != 0 || tabs->count() != 2) {
            cutError = QStringLiteral("real click did not change current inside the complete 2/3 cut");
            return;
        }
        for (int index = 0; index < 2; ++index) {
            cutError = fixture.documentGuard(delivered[index], itemIndices[index]);
            if (!cutError.isEmpty()) return;
        }
        qInfo() << "INTENT_REORDER_CUT_GUARDS_OK" << "oldFirst" << itemIndices[0]
                << "oldSecond" << itemIndices[1] << "changed" << clickChangedCurrent;
    }, Qt::DirectConnection);
    startup.observe(window, {});
    window->resize(1100, 750);
    window->show();
    window->raise();
    window->activateWindow();
    startup.releasePostPaintWork();
    QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 10000);
    QCOMPARE(cuts, 1);
    QVERIFY2(cutError.isEmpty(), qPrintable(cutError));
    QVERIFY(window && chosen && delivered[0] && delivered[1]);
    QVERIFY(clickChangedCurrent);
    QCOMPARE(tabs->count(), 3);
    for (int index = 0; index < 2; ++index) {
        QCOMPARE(delivered[index]->documentState().id, deliveredIds[index]);
        QCOMPARE(delivered[index]->contentRevision(), deliveredRevisions[index]);
    }
    QSet<QUuid> ids;
    std::array<int, 3> occurrences{};
    QStringList visibleOrder;
    QVector<SessionTab> visibleStates;
    for (int index = 0; index < tabs->count(); ++index) {
        auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
        const int itemIndex = fixture.itemIndex(tab);
        const QString guard = fixture.documentGuard(tab, itemIndex);
        QVERIFY2(guard.isEmpty(), qPrintable(guard));
        ++occurrences[itemIndex];
        ids.insert(tab->documentState().id);
        visibleOrder.append(tab->filePath());
        visibleStates.append(tab->captureSessionTab());
    }
    QCOMPARE(ids.size(), 3);
    for (int count : occurrences) QCOMPARE(count, 1);
    for (const auto &item : fixture.items)
        QCOMPARE(fileHash(item.path), item.sourceHash);
    // Record business outcomes before normal close; close may destroy every
    // widget. A later inserted page may legally use either neighbor as anchor.
    const bool relativeOrderKept = tabs->indexOf(delivered[1]) < tabs->indexOf(delivered[0]);
    const bool chosenStillCurrent = tabs->currentWidget() == chosen;
    const int visibleActive = tabs->currentIndex();
    watchCurrent = false;
    qInfo() << "INTENT_REORDER_FINAL_DATA_GUARDS_OK" << "currentSignals" << currentSignalsAfterClick
            << "unexpectedCurrentChanges" << unexpectedCurrentChanges;
    const bool accepted = window->close(); // Actual forced session publication path.
    // No tab/editor access or later cleanup sampling after this business cut.
    SessionManager fresh(fixture.profile);
    const SessionData published = fresh.loadSession();
    QVERIFY(accepted);
    QVERIFY(!window || !window->isVisible());
    QVERIFY(!AsyncSerialQueue::session().busy());
    QCOMPARE(published.tabs.size(), 3);
    QCOMPARE(published.activeTab, visibleActive);
    QStringList publishedOrder;
    for (int index = 0; index < published.tabs.size(); ++index) {
        const SessionTab &record = published.tabs.at(index);
        const SessionTab &visible = visibleStates.at(index);
        publishedOrder.append(record.filePath);
        QCOMPARE(record.filePath, visible.filePath);
        QCOMPARE(record.documentId, visible.documentId);
        QVERIFY(record.modified && !record.secureDocument);
        QCOMPARE(record.selectionStartByte, visible.selectionStartByte);
        QCOMPARE(record.selectionEndByte, visible.selectionEndByte);
        QVERIFY(DraftStore::isEncryptedDraft(record.autosavePath));
        QByteArray body;
        QVERIFY(DraftStore::readSnapshot(record.autosavePath, &body));
        int itemIndex = -1;
        for (int candidate = 0; candidate < 3; ++candidate)
            if (fixture.items[candidate].path == record.filePath) itemIndex = candidate;
        QVERIFY(itemIndex >= 0);
        QCOMPARE(body, fixture.items[itemIndex].body);
        QCOMPARE(fileHash(record.filePath), fixture.items[itemIndex].sourceHash);
        // Successful publication may legitimately reseal or change draft paths;
        // compare recoverable bytes, never demand the old ciphertext hash.
    }
    QCOMPARE(publishedOrder, visibleOrder);
    QCOMPARE(completed, 1);
    QVERIFY2(fixture.dialogs.seen.isEmpty(), qPrintable(fixture.dialogs.seen.join(QLatin1Char(';'))));
    qInfo() << "INTENT_REORDER_PUBLISHED_DATA_GUARDS_OK";
    QVERIFY2(relativeOrderKept, "Later restore/finish reversed the user's already-delivered pair");
    QVERIFY2(chosenStillCurrent, "Later restore/finish replaced the user's actual clicked choice");
    QCOMPARE(unexpectedCurrentChanges, 0);
}

QTEST_MAIN(RecoveryIntentSmoke)
#include "smoke_recovery_intent.moc"
