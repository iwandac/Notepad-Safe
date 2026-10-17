#include <QToolButton>
#include "TitleBar.h"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>

#include <memory>
#include <utility>

#include "AppComposition.h"
#include "AsyncSerialQueue.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "SessionManager.h"
#include "StartupCoordinator.h"

namespace {
QByteArray fileHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result();
}

class ShellFixture final : public QObject {
public:
    ~ShellFixture() override
    {
        qApp->removeEventFilter(this);
        startup.reset(); // Revoke queued startup work before fixture teardown.
        if (!QTest::qWaitFor([] {
                return !AsyncSerialQueue::session().busy() && !AsyncSerialQueue::security().busy();
            }, 5000))
            qFatal("STARTUP_SHELL_CLEANUP_TIMEOUT: refusing to delete managers used by live tasks");
        window.reset();
        composition.reset();
        if (profileSet) {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", oldProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool prepare(const QString &cleanSeedPath = {})
    {
        if (!temporary.isValid()) return fail(QStringLiteral("temporary directory unavailable"));
        const QString profile = temporary.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile) || !QDir(profile).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            return fail(QStringLiteral("profile is not genuinely empty"));
        hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldProfile = qgetenv("NOTEPADSAFE_DATADIR");
        profileSet = qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        if (!profileSet) return fail(QStringLiteral("profile isolation failed"));
        qApp->installEventFilter(this);
        composition = std::make_unique<AppComposition>();
        if (!cleanSeedPath.isEmpty()) {
            EditorTab seed;
            if (!seed.loadFile(cleanSeedPath) || seed.isModified() || seed.isSecureDocument()
                || seed.filePath() != cleanSeedPath)
                return fail(QStringLiteral("real clean ordinary seed did not load"));
            SessionManager manager(profile);
            if (!manager.saveSession({&seed}, 0, true))
                return fail(QStringLiteral("real clean session seed did not save"));
            const SessionData saved = manager.loadSession();
            if (saved.tabs.size() != 1 || saved.activeTab != 0
                || saved.tabs.first().filePath != cleanSeedPath || saved.tabs.first().modified
                || saved.tabs.first().secureDocument || !saved.tabs.first().autosavePath.isEmpty())
                return fail(QStringLiteral("published manifest is not one clean ordinary source"));
        }
        window = std::make_unique<MainWindow>(nullptr, composition.get());
        startup = std::make_unique<StartupCoordinator>();
        connect(window.get(), &MainWindow::firstPaintCompleted, this,
                [this] { ++firstPaintCount; }, Qt::DirectConnection);
        connect(startup.get(), &StartupCoordinator::postPaintIdleTurnCompleted, this,
                [this] { ++idleCount; }, Qt::DirectConnection);
        connect(window.get(), &MainWindow::sessionRestoreCompleted, this,
                [this] { ++restoreCount; }, Qt::DirectConnection);
        startup->observe(window.get(), {});
        return true;
    }

    bool start() { return prepare() && showAndRestore(); }

    bool showAndRestore()
    {
        window->resize(1100, 740);
        window->show();
        window->raise();
        window->activateWindow();
        startup->releasePostPaintWork();
        if (!QTest::qWaitForWindowExposed(window.get(), 3000))
            return fail(QStringLiteral("native main window not exposed"));
        if (!QTest::qWaitFor([this] { return restoreCount != 0; }, 5000))
            return fail(QStringLiteral("real startup restoration did not settle"));
        qInfo("STARTUP_SHELL paint=%d idle=%d restored=%d modal=%d identity=%d cancelClicks=%d documents=%lld heavy=%d",
              firstPaintCount, idleCount, restoreCount, modalCount, identityCount, cancelClicks,
              static_cast<long long>(documents().size()), composition->heavyFactoryCalls());
        return true;
    }

    QList<EditorTab *> documents() const
    {
        return window ? window->findChildren<EditorTab *>() : QList<EditorTab *>{};
    }

    QString emptyShellError() const
    {
        if (firstPaintCount != 1 || idleCount != 1 || restoreCount != 1)
            return QStringLiteral("startup phase signals were missing or duplicated");
        // Never erase this history after canceling a blocking dialog. Old ON
        // must fail here even though the cleanup Cancel allowed restore to end.
        if (modalCount != 0)
            return QStringLiteral("IMPLICIT_STARTUP_MODAL: observed %1 modal(s), %2 identity dialog(s); cancellation was cleanup, not an input pass")
                .arg(modalCount).arg(identityCount);
        if (QApplication::activeModalWidget()) return QStringLiteral("startup remains modal");
        if (!documents().isEmpty()) return QStringLiteral("IMPLICIT_STARTUP_DOCUMENT: empty session created a document");
        if (composition->heavyFactoryCalls() != 0
            || composition->documentTools().instantiatedCount() != 0
            || composition->documentProviders().instantiatedCount() != 0
            || composition->persistenceAdapters().instantiatedCount() != 0
            || composition->inferenceProviders().instantiatedCount() != 0)
            return QStringLiteral("empty startup instantiated a backend/provider/tool");
        return {};
    }

    bool openFileMenu()
    {
        // The menu bar is hidden; menus open from the title bar's main menu button.
        auto *action = window->findChild<QAction *>(QStringLiteral("file.new"));
        QMenu *mainMenu = window->mainMenu();
        QToolButton *menuButton = window->titleBar() ? window->titleBar()->menuButton() : nullptr;
        if (!action || !action->isEnabled() || !mainMenu || !menuButton || !menuButton->isVisible())
            return fail(QStringLiteral("real File/New action or main menu button unavailable"));
        fileMenu = nullptr;
        for (QAction *top : mainMenu->actions()) {
            if (top->menu() && top->menu()->actions().contains(action)) {
                fileMenu = top->menu();
                fileMenuAction = top;
                break;
            }
        }
        if (!fileMenu) return fail(QStringLiteral("New is not in a top-level production menu"));
        if (!QTest::qWaitForWindowActive(window.get(), 2000))
            return fail(QStringLiteral("owned native main window could not become active"));
        QTest::mouseClick(menuButton, Qt::LeftButton);
        if (!QTest::qWaitFor([mainMenu] { return mainMenu->isVisible(); }, 2000))
            return fail(QStringLiteral("main menu button did not open the main menu"));
        const QRect area = mainMenu->actionGeometry(fileMenuAction);
        if (area.isEmpty() || !mainMenu->rect().contains(area.center()))
            return fail(QStringLiteral("File menu has no real clickable geometry"));
        menuPaintCount = menuPaintTailCount = 0;
        QTest::mouseClick(mainMenu, Qt::LeftButton, Qt::NoModifier, area.center());
        if (!QTest::qWaitFor([this] {
                return fileMenu && fileMenu->isVisible() && menuPaintCount > 0 && menuPaintTailCount > 0;
            }, 2000)) return fail(QStringLiteral("real menu input did not produce a visible menu and Paint delivery"));
        qInfo("STARTUP_MENU input=mouse menuPaint=%d afterPaintTurn=%d visible=1", menuPaintCount, menuPaintTailCount);
        return screenshot(QStringLiteral("file-menu"), fileMenu);
    }

    bool clickNew()
    {
        if (!openFileMenu()) return false;
        QAction *action = window->findChild<QAction *>(QStringLiteral("file.new"));
        const QRect area = fileMenu->actionGeometry(action);
        if (area.isEmpty() || !fileMenu->rect().contains(area.center()))
            return fail(QStringLiteral("New action has no real clickable menu geometry"));
        phase = QStringLiteral("explicit-new");
        QTest::mouseClick(fileMenu, Qt::LeftButton, Qt::NoModifier, area.center());
        return true;
    }

    bool screenshot(const QString &name, QWidget *target)
    {
        const QString directory = qEnvironmentVariable("NOTEPADSAFE_STARTUP_SHELL_SCREENSHOTS");
        if (directory.isEmpty()) return true;
        if (!QDir::isAbsolutePath(directory) || !QDir().mkpath(directory))
            return fail(QStringLiteral("screenshot output must be an absolute writable directory"));
        const QString filename = QString::fromLatin1(QTest::currentTestFunction()) + QLatin1Char('-') + name + QStringLiteral(".png");
        if (!target || !target->isVisible() || !target->grab().save(QDir(directory).filePath(filename)))
            return fail(QStringLiteral("owned widget screenshot failed"));
        return true;
    }

    QTemporaryDir temporary;
    std::unique_ptr<AppComposition> composition;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<StartupCoordinator> startup;
    QString error;
    QString phase = QStringLiteral("startup");
    int firstPaintCount = 0;
    int idleCount = 0;
    int restoreCount = 0;
    int modalCount = 0;
    int identityCount = 0;
    int cancelClicks = 0;
    int menuPaintCount = 0;
    int menuPaintTailCount = 0;
    QPointer<QMenu> fileMenu;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == fileMenu && event->type() == QEvent::Paint) {
            ++menuPaintCount;
            const QPointer<QMenu> menu = fileMenu;
            // Observation only: let the real QMenu process Paint. The queued
            // marker is not claimed to be DWM composition or a latency probe.
            QTimer::singleShot(0, this, [this, menu] {
                if (menu && menu == fileMenu) ++menuPaintTailCount;
            });
        }
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(watched);
            bool owned = false;
            // QDialog is a top-level widget even with MainWindow as its parent.
            // Follow QObject ownership explicitly, not visual widget ancestry
            // across a window boundary, so the real modal can be observed and
            // safely canceled without erasing the startup failure evidence.
            for (QObject *owner = dialog; owner; owner = owner->parent()) {
                if (owner == window.get()) {
                    owned = true;
                    break;
                }
            }
            if (dialog)
                qInfo("STARTUP_DIALOG_SHOW phase=%s modal=%d owned=%d windowReady=%d",
                      qPrintable(phase), int(dialog->isModal()), int(owned), int(bool(window)));
            if (dialog && dialog->isModal() && owned) {
                ++modalCount;
                const bool creatingIdentity = dialog->findChild<QLineEdit *>(QStringLiteral("securitySessionPassword"))
                    && dialog->findChild<QLineEdit *>(QStringLiteral("securitySessionConfirmation"));
                if (creatingIdentity) ++identityCount;
                qInfo("STARTUP_MODAL phase=%s creatingIdentity=%d; queued real Cancel is cleanup only",
                      qPrintable(phase), int(creatingIdentity));
                const QPointer<QDialog> guarded(dialog);
                QTimer::singleShot(0, this, [this, guarded] {
                    if (!guarded) return;
                    auto *buttons = guarded->findChild<QDialogButtonBox *>();
                    auto *cancel = buttons ? buttons->button(QDialogButtonBox::Cancel) : nullptr;
                    if (cancel && cancel->isVisible() && cancel->isEnabled()) {
                        ++cancelClicks;
                        QTest::mouseClick(cancel, Qt::LeftButton);
                    } else {
                        guarded->reject(); // Only a bounded failure cleanup fallback.
                    }
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    bool fail(QString message) { error = std::move(message); return false; }
    bool hadProfile = false;
    bool profileSet = false;
    QByteArray oldProfile;
    QAction *fileMenuAction = nullptr;
};
} // namespace

class SmokeStartupShell final : public QObject {
    Q_OBJECT
private slots:
    void emptySessionCompletesWithoutImplicitDocumentOrModal()
    {
        ShellFixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        const QString guard = fixture.emptyShellError();
        QVERIFY2(guard.isEmpty(), qPrintable(guard));
        QVERIFY2(fixture.openFileMenu(), qPrintable(fixture.error));
        QVERIFY(fixture.menuPaintCount > 0);
        QVERIFY(fixture.menuPaintTailCount > 0);
        QTest::keyClick(fixture.fileMenu, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.fileMenu->isVisible(), 1000);
        QVERIFY2(fixture.screenshot(QStringLiteral("empty-shell"), fixture.window.get()), qPrintable(fixture.error));
        QVERIFY2(fixture.emptyShellError().isEmpty(), qPrintable(fixture.emptyShellError()));
        QVERIFY(fixture.window->statusBar()->isVisible());
        QStringList visibleDocumentLabels;
        for (const QString &name : {QStringLiteral("status.position"), QStringLiteral("status.encoding"),
                                   QStringLiteral("status.context"), QStringLiteral("status.size"),
                                   QStringLiteral("status.characters")}) {
            auto *label = fixture.window->findChild<QWidget *>(name);
            QVERIFY2(label, qPrintable(QStringLiteral("Missing document status label: ") + name));
            if (label->isVisible()) visibleDocumentLabels.append(name);
        }
        QVERIFY2(visibleDocumentLabels.isEmpty(),
                 qPrintable(QStringLiteral("EMPTY_SHELL_DOCUMENT_STATUS_VISIBLE: ")
                            + visibleDocumentLabels.join(QLatin1Char(','))));
    }

    void explicitNewKeepsOriginalSecurityBoundary()
    {
        ShellFixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        QVERIFY2(fixture.emptyShellError().isEmpty(), qPrintable(fixture.emptyShellError()));
        QVERIFY2(fixture.clickNew(), qPrintable(fixture.error));
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
        QTRY_COMPARE_WITH_TIMEOUT(fixture.identityCount, 1, 2000);
        QCOMPARE(fixture.modalCount, 1);
        QCOMPARE(fixture.cancelClicks, 1);
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY2(fixture.documents().isEmpty(), "Canceling real identity creation must not create a plaintext alternative");
        QVERIFY(fixture.composition->heavyFactoryCalls() > 0);
#else
        QTRY_COMPARE_WITH_TIMEOUT(fixture.documents().size(), qsizetype(1), 2000);
        QCOMPARE(fixture.modalCount, 0);
        EditorTab *tab = fixture.documents().first();
        QVERIFY(!tab->isSecureDocument());
        QCOMPARE(tab->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
        QVERIFY(tab->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QVERIFY(tab->editor()->text().isEmpty());
#endif
        QCOMPARE(fixture.restoreCount, 1);
    }

    void publicOpenFromEmptyShellPreservesSmallFileBytes()
    {
        ShellFixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        QVERIFY2(fixture.emptyShellError().isEmpty(), qPrintable(fixture.emptyShellError()));
        const QString startupHint = fixture.window->statusBar()->currentMessage();
        QVERIFY2(!startupHint.isEmpty(), "Empty startup must provide its real starting hint");
        const QByteArray bytes = QStringLiteral("STARTUP_OPEN_SOURCE\n真实普通文件，不创建安全身份。\nEND\n").toUtf8();
        const QString path = fixture.temporary.filePath(QStringLiteral("source.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        QVERIFY(file.flush());
        file.close();
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        fixture.phase = QStringLiteral("public-open");
        fixture.window->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(fixture.documents().size(), qsizetype(1), 3000);
        EditorTab *tab = fixture.documents().first();
        QCOMPARE(tab->editor()->text().toUtf8(), bytes);
        QCOMPARE(tab->filePath(), path);
        QVERIFY(!tab->documentState().id.isNull());
        QCOMPARE(tab->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
        QVERIFY(tab->documentState().capabilities.testFlag(DocumentCapability::Read));
        QVERIFY(tab->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QVERIFY(!tab->isSecureDocument());
        QVERIFY(!tab->isModified());
        QVERIFY(!tab->documentState().dirty);
        QCOMPARE(fileHash(path), expectedHash);
        QCOMPARE(fixture.modalCount, 0);
        QCOMPARE(fixture.composition->heavyFactoryCalls(), 0);
        QCOMPARE(fixture.restoreCount, 1);
        // Match only the hint captured from this real empty startup. A later
        // legitimate error/status message need not be empty or be discarded.
        QVERIFY2(fixture.window->statusBar()->currentMessage() != startupHint,
                 "STALE_EMPTY_STARTUP_HINT: source opened correctly but the empty-shell hint remains");
        QVERIFY2(fixture.screenshot(QStringLiteral("ordinary-open"), fixture.window.get()), qPrintable(fixture.error));
    }

    void closingOnlyRestoredTabBeforeFinishKeepsEmptyShell()
    {
        ShellFixture fixture;
        const QByteArray bytes("ONLY_CLEAN_RESTORED_SOURCE\nThe user closes this tab before finish.\n");
        const QString path = fixture.temporary.filePath(QStringLiteral("clean-restored.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        QVERIFY(file.flush());
        file.close();
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        QVERIFY2(fixture.prepare(path), qPrintable(fixture.error));

        int progressCount = 0;
        int closeTriggered = 0;
        bool queuedCloseRan = false;
        bool closeReturnedBeforeFinish = false;
        QPointer<EditorTab> restored;
        DocumentState beforeClose;
        QString callbackError;
        auto *closeAction = fixture.window->findChild<QAction *>(QStringLiteral("file.close"));
        QVERIFY(closeAction);
        // Destroy this context before captured locals and fixture teardown.
        QObject observationScope;
        connect(closeAction, &QAction::triggered, &observationScope,
                [&] { ++closeTriggered; }, Qt::DirectConnection);
        connect(fixture.window->statusBar(), &QStatusBar::messageChanged, &observationScope,
                [&](const QString &message) {
            if (message != QStringLiteral("正在恢复会话… 1/1 个标签页")) return;
            if (++progressCount != 1) return;
            const auto documents = fixture.documents();
            if (fixture.restoreCount != 0 || documents.size() != 1) {
                callbackError = QStringLiteral("1/1 progress was not before finish with one real target");
                return;
            }
            restored = documents.first();
            beforeClose = restored->documentState();
            if (restored->filePath() != path || restored->editor()->text().toUtf8() != bytes
                || restored->isModified() || beforeClose.dirty || restored->isSecureDocument()
                || beforeClose.securityClass != DocumentSecurityClass::OrdinaryDocument
                || beforeClose.id.isNull() || !restored->autosavePath().isEmpty()
                || !beforeClose.capabilities.testFlag(DocumentCapability::Read)
                || !beforeClose.capabilities.testFlag(DocumentCapability::Edit)
                || fileHash(path) != expectedHash) {
                callbackError = QStringLiteral("restored source guard failed before queuing Close");
                return;
            }
            // restoreNext emits this public progress synchronously, before it
            // queues its next turn. No private Coordinator or fabricated state.
            QTimer::singleShot(0, &observationScope, [&] {
                queuedCloseRan = true;
                if (fixture.restoreCount != 0 || !restored || fixture.documents().size() != 1
                    || !closeAction->isEnabled()) {
                    callbackError = QStringLiteral("queued Close missed the before-finish window");
                    return;
                }
                const auto current = restored->documentState();
                if (current.id != beforeClose.id || current.revision != beforeClose.revision
                    || current.capabilities != beforeClose.capabilities || current.dirty
                    || restored->isModified() || restored->editor()->text().toUtf8() != bytes
                    || fileHash(path) != expectedHash) {
                    callbackError = QStringLiteral("source identity/revision/body changed before real Close");
                    return;
                }
                closeAction->trigger(); // Existing public command; its real handler deletes the tab.
                closeReturnedBeforeFinish = fixture.restoreCount == 0;
                if (!closeReturnedBeforeFinish || restored || !fixture.documents().isEmpty()
                    || fileHash(path) != expectedHash)
                    callbackError = QStringLiteral("real Close did not leave zero tabs before finish");
            });
        }, Qt::DirectConnection);
        QVERIFY2(fixture.showAndRestore(), qPrintable(fixture.error));
        QVERIFY2(callbackError.isEmpty(), qPrintable(callbackError));
        QCOMPARE(progressCount, 1);
        QVERIFY(queuedCloseRan);
        QVERIFY(closeReturnedBeforeFinish);
        QCOMPARE(closeTriggered, 1);
        QVERIFY(restored.isNull());
        QCOMPARE(fileHash(path), expectedHash);
        QCOMPARE(fixture.firstPaintCount, 1);
        QCOMPARE(fixture.idleCount, 1);
        QCOMPARE(fixture.restoreCount, 1);
        QVERIFY2(fixture.documents().isEmpty(), "Recovery finish recreated a user-closed document");
        QCOMPARE(fixture.modalCount, 0);
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(fixture.composition->heavyFactoryCalls(), 0);
        qInfo("STARTUP_FINISH_EMPTY progress=1 closeTriggered=1 closeBeforeFinish=1 restored=1 documents=0 sourceGuard=1");
        // A real ordinary Provider was instantiated. Do not require zero
        // providers or force finish to delete the preservation manifest.
    }

    void openingFilePreservesReplacementStatusMessage()
    {
        ShellFixture fixture;
        QVERIFY2(fixture.start(), qPrintable(fixture.error));
        QVERIFY2(fixture.emptyShellError().isEmpty(), qPrintable(fixture.emptyShellError()));
        const QString startupHint = fixture.window->statusBar()->currentMessage();
        QVERIFY(!startupHint.isEmpty());
        const QString replacement = QStringLiteral("TEST_STATUS_REPLACEMENT_KEEP");
        QVERIFY(replacement != startupHint);
        // Pure presentation regression: this explicit test status is NOT a
        // simulated I/O error or evidence of a real failed-open journey.
        fixture.window->statusBar()->showMessage(replacement, 0);
        QCOMPARE(fixture.window->statusBar()->currentMessage(), replacement);

        const QByteArray bytes("KEEP_NEWER_STATUS\nActual ordinary file content.\n");
        const QString path = fixture.temporary.filePath(QStringLiteral("status-source.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        QVERIFY(file.flush());
        file.close();
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        fixture.phase = QStringLiteral("open-with-replacement-status");
        fixture.window->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(fixture.documents().size(), qsizetype(1), 3000);
        EditorTab *tab = fixture.documents().first();
        QCOMPARE(tab->filePath(), path);
        QCOMPARE(tab->editor()->text().toUtf8(), bytes);
        QVERIFY(!tab->isModified());
        QVERIFY(!tab->documentState().dirty);
        QVERIFY(!tab->documentState().id.isNull());
        QCOMPARE(tab->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
        QVERIFY(tab->documentState().capabilities.testFlag(DocumentCapability::Read));
        QVERIFY(tab->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QVERIFY(!tab->isSecureDocument());
        QCOMPARE(fileHash(path), expectedHash);
        QCOMPARE(fixture.modalCount, 0);
        QCOMPARE(fixture.composition->heavyFactoryCalls(), 0);
        QCOMPARE(fixture.restoreCount, 1);
        QCOMPARE(fixture.window->statusBar()->currentMessage(), replacement);
    }
};

QTEST_MAIN(SmokeStartupShell)
#include "smoke_startup_shell.moc"
