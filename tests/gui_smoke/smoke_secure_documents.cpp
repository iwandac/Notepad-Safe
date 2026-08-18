#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SecureBuffer.h"
#include "SecureDocumentService.h"
#include "SecuritySessionService.h"
#include "VaultDock.h"
#include "VirtualFileView.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QElapsedTimer>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeView>
#include <QtTest>

#include <memory>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <WtsApi32.h>
#endif

namespace {

EditorTab *currentEditor(MainWindow *window)
{
    auto *tabs = window->findChild<QTabWidget *>();
    return tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
}

QAction *actionWithText(QObject *root, const QString &needle)
{
    for (QAction *action : root->findChildren<QAction *>()) {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (text.contains(needle))
            return action;
    }
    return nullptr;
}

QAction *actionWithShortcut(QObject *root, const QKeySequence &shortcut)
{
    for (QAction *action : root->findChildren<QAction *>()) {
        if (action->shortcut().matches(shortcut) == QKeySequence::ExactMatch)
            return action;
    }
    return nullptr;
}

bool writeFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

QTreeWidgetItem *findTreeItem(QTreeWidget *tree, const QString &needle)
{
    if (!tree)
        return nullptr;
    QList<QTreeWidgetItem *> pending;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        pending.push_back(tree->topLevelItem(index));
    while (!pending.isEmpty()) {
        QTreeWidgetItem *item = pending.takeLast();
        if (item->text(0).contains(needle, Qt::CaseInsensitive))
            return item;
        for (int index = 0; index < item->childCount(); ++index)
            pending.push_back(item->child(index));
    }
    return nullptr;
}

bool treeContainsText(QTreeWidget *tree, const QString &needle)
{
    return findTreeItem(tree, needle) != nullptr;
}

void confirmRecoverySeedFromIdentitySetup()
{
    QElapsedTimer wait;
    wait.start();
    QDialog *recoveryDialog = nullptr;
    QPlainTextEdit *seedView = nullptr;
    QLineEdit *verify = nullptr;
    QCheckBox *confirmed = nullptr;
    QDialogButtonBox *buttons = nullptr;
    while (wait.elapsed() < 3000) {
        recoveryDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!recoveryDialog) {
            QTest::qWait(25);
            continue;
        }

        seedView = recoveryDialog->findChild<QPlainTextEdit *>(
            QStringLiteral("securityRecoveryKey"));
        buttons = recoveryDialog->findChild<QDialogButtonBox *>();
        if (!seedView || !buttons) {
            QTest::qWait(25);
            continue;
        }

        verify = recoveryDialog->findChild<QLineEdit *>(QStringLiteral("securitySessionRecoveryVerify"));
        if (!verify)
            verify = recoveryDialog->findChild<QLineEdit *>();
        confirmed = recoveryDialog->findChild<QCheckBox *>(QStringLiteral("securityRecoveryConfirmed"));
        break;
    }

    QVERIFY(recoveryDialog);
    QVERIFY(seedView);
    QVERIFY(buttons);

    if (verify)
        verify->setText(seedView->toPlainText().right(8));
    else if (confirmed)
        confirmed->setChecked(true);

    auto *ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    QTest::mouseClick(ok, Qt::LeftButton);
}

} // namespace

class SecureDocumentsSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void saveAsUsesTwoSecurityChoicesAndRoundTrips()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());

        auto window = std::make_unique<MainWindow>();
        window->resize(1180, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        QTimer::singleShot(80, window.get(), []() {
            auto *identity = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(identity);
            auto *password = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionPassword"));
            auto *confirmation = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionConfirmation"));
            auto *buttons = identity->findChild<QDialogButtonBox *>();
            QVERIFY(password && confirmation && buttons);
            QTest::keyClicks(password, QStringLiteral("correct horse battery staple"));
            QTest::keyClicks(confirmation, QStringLiteral("correct horse battery staple"));
            QTimer::singleShot(120, identity, []() {
                confirmRecoverySeedFromIdentitySetup();
            });
            QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        });
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));

        EditorTab *tab = currentEditor(window.get());
        QVERIFY(tab && tab->isSecureDocument());
        const QString body = QStringLiteral("Save As security boundary\n第二行内容\n");
        tab->editor()->setText(body);

        QAction *saveAsAction = window->findChild<QAction *>(
            QStringLiteral("file.saveAs"));
        QVERIFY(saveAsAction);
        const QString dangerousPlainName = dataDirectory.filePath(
            QStringLiteral("exported.nsafe"));
        const QString normalizedPlainName = dataDirectory.filePath(
            QStringLiteral("exported.txt"));
        bool inspectedPlainChoice = false;
        bool plainDialogAccepted = false;
        bool plainScreenshotSaved = false;
        QStringList plainFilters;
        QString plainNotice;
        QString plainVisibleSuggestion;
        QTimer::singleShot(80, window.get(), [&]() {
            auto *dialog = window->findChild<QDialog *>(QStringLiteral("saveAsDialog"));
            auto *fileDialog = dialog ? dialog->findChild<QFileDialog *>(
                QStringLiteral("saveAsFileDialog")) : nullptr;
            auto *notice = dialog ? dialog->findChild<QLabel *>(
                QStringLiteral("saveAsSecurityNotice")) : nullptr;
            if (!dialog || !fileDialog || !notice) {
                if (dialog)
                    dialog->reject();
                return;
            }
            plainFilters = fileDialog->nameFilters();
            QComboBox *typeCombo = nullptr;
            for (QComboBox *candidate : fileDialog->findChildren<QComboBox *>()) {
                if (candidate->count() == plainFilters.size()
                    && candidate->itemText(0).contains(QStringLiteral("安全文档"))) {
                    typeCombo = candidate;
                    break;
                }
            }
            if (typeCombo) {
                typeCombo->setCurrentIndex(0);
                typeCombo->setFocus();
                QTest::keyClick(typeCombo, Qt::Key_Down);
            }
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            plainNotice = notice->text();
            if (!fileDialog->selectedFiles().isEmpty())
                plainVisibleSuggestion = fileDialog->selectedFiles().constFirst();
            plainScreenshotSaved = dialog->grab().save(QDir::current().filePath(
                QStringLiteral("gui-save-as-plain-choice.png")));
            // Keep the final write boundary defensive even if a caller or user
            // subsequently enters the reserved extension.
            fileDialog->selectFile(dangerousPlainName);
            inspectedPlainChoice = true;
            plainDialogAccepted = QMetaObject::invokeMethod(
                fileDialog, "accept", Qt::DirectConnection);
        });
        QTimer::singleShot(5000, window.get(), [window = window.get()]() {
            if (auto *dialog = window->findChild<QDialog *>(
                    QStringLiteral("saveAsDialog")); dialog && dialog->isVisible()) {
                dialog->reject();
            }
        });
        saveAsAction->trigger();

        QVERIFY(inspectedPlainChoice);
        QVERIFY(plainDialogAccepted);
        QVERIFY(plainScreenshotSaved);
        QCOMPARE(plainFilters.size(), 2);
        QVERIFY(plainFilters.at(0).contains(QStringLiteral("安全文档")));
        QVERIFY(plainFilters.at(1).contains(QStringLiteral("未加密文件")));
        QVERIFY(plainNotice.contains(QStringLiteral("未加密")));
        QVERIFY(plainVisibleSuggestion.endsWith(QStringLiteral(".txt"),
                                                 Qt::CaseInsensitive));
        QVERIFY(!tab->isSecureDocument());
        QCOMPARE(QFileInfo(tab->filePath()).absoluteFilePath(),
                 QFileInfo(normalizedPlainName).absoluteFilePath());
        QVERIFY(QFileInfo::exists(normalizedPlainName));
        QVERIFY(!QFileInfo::exists(dangerousPlainName));
        QFile plain(normalizedPlainName);
        QVERIFY(plain.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(plain.readAll()), body);

        const QString requestedSecureName = dataDirectory.filePath(
            QStringLiteral("roundtrip.txt"));
        const QString normalizedSecureName = requestedSecureName
                                                 + QStringLiteral(".nsafe");
        bool inspectedSecureChoice = false;
        bool secureDialogAccepted = false;
        bool secureScreenshotSaved = false;
        QString secureNotice;
        QString secureVisibleSuggestion;
        QTimer::singleShot(80, window.get(), [&]() {
            auto *dialog = window->findChild<QDialog *>(QStringLiteral("saveAsDialog"));
            auto *fileDialog = dialog ? dialog->findChild<QFileDialog *>(
                QStringLiteral("saveAsFileDialog")) : nullptr;
            auto *notice = dialog ? dialog->findChild<QLabel *>(
                QStringLiteral("saveAsSecurityNotice")) : nullptr;
            if (!dialog || !fileDialog || !notice) {
                if (dialog)
                    dialog->reject();
                return;
            }
            const QStringList filters = fileDialog->nameFilters();
            QComboBox *typeCombo = nullptr;
            for (QComboBox *candidate : fileDialog->findChildren<QComboBox *>()) {
                if (candidate->count() == filters.size()
                    && candidate->itemText(0).contains(QStringLiteral("安全文档"))) {
                    typeCombo = candidate;
                    break;
                }
            }
            if (typeCombo) {
                typeCombo->setCurrentIndex(1);
                typeCombo->setFocus();
                QTest::keyClick(typeCombo, Qt::Key_Up);
            }
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            secureNotice = notice->text();
            if (!fileDialog->selectedFiles().isEmpty())
                secureVisibleSuggestion = fileDialog->selectedFiles().constFirst();
            secureScreenshotSaved = dialog->grab().save(QDir::current().filePath(
                QStringLiteral("gui-save-as-secure-choice.png")));
            fileDialog->selectFile(requestedSecureName);
            inspectedSecureChoice = true;
            secureDialogAccepted = QMetaObject::invokeMethod(
                fileDialog, "accept", Qt::DirectConnection);
        });
        QTimer::singleShot(5000, window.get(), [window = window.get()]() {
            if (auto *dialog = window->findChild<QDialog *>(
                    QStringLiteral("saveAsDialog")); dialog && dialog->isVisible()) {
                dialog->reject();
            }
        });
        saveAsAction->trigger();

        QVERIFY(inspectedSecureChoice);
        QVERIFY(secureDialogAccepted);
        QVERIFY(secureScreenshotSaved);
        QVERIFY(secureNotice.contains(QStringLiteral("离线加密")));
        QVERIFY(secureVisibleSuggestion.endsWith(QStringLiteral(".nsafe"),
                                                  Qt::CaseInsensitive));
        QVERIFY(tab->isSecureDocument());
        QCOMPARE(QFileInfo(tab->filePath()).absoluteFilePath(),
                 QFileInfo(normalizedSecureName).absoluteFilePath());
        QCOMPARE(tab->editor()->text(), body);
        QFile encrypted(normalizedSecureName);
        QVERIFY(encrypted.open(QIODevice::ReadOnly));
        QCOMPARE(encrypted.read(8).toHex().toUpper(), QByteArray("03D9A29A67FB4BB5"));

        window->close();
        window.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void unifiedOpenForeignAccountWithRecovery()
    {
        QTemporaryDir mainIdentityDirectory;
        QTemporaryDir foreignIdentityDirectory;
        QVERIFY(mainIdentityDirectory.isValid());
        QVERIFY(foreignIdentityDirectory.isValid());

        const QByteArray mainPasswordBytes("main account identity password");
        SecureBuffer mainPassword(mainPasswordBytes.constData(), mainPasswordBytes.size());
        QString mainRecoveryKey;
        {
            SecuritySessionService mainSession(mainIdentityDirectory.path());
            const VaultError error = mainSession.createIdentity(mainPassword, &mainRecoveryKey);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
        }
        mainPassword.clear();
        mainRecoveryKey.fill(QChar(0));
        mainRecoveryKey.clear();

        const QString foreignAccountsPath = foreignIdentityDirectory.filePath(
            QStringLiteral("team-accounts.kdbx"));
        const QByteArray foreignPasswordBytes("foreign account identity password");
        SecureBuffer foreignPassword(foreignPasswordBytes.constData(),
                                     foreignPasswordBytes.size());
        QString foreignRecoveryKey;
        qputenv("NOTEPADSAFE_DATADIR", foreignIdentityDirectory.path().toUtf8());
        {
            SecuritySessionService foreignSession(foreignIdentityDirectory.path());
            VaultError error = foreignSession.createIdentity(foreignPassword,
                                                              &foreignRecoveryKey);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
            SecurityOperationLease lease;
            error = foreignSession.acquireOperationLease(&lease);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));

            VaultDock fixtureDock;
            error = fixtureDock.createManagedStoreForIdentity(
                foreignAccountsPath, foreignSession.parameters(),
                lease.masterUnlockKey(), lease.recoverySecret());
            lease.clear();
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
            fixtureDock.createLogin();
            auto *title = fixtureDock.findChild<QLineEdit *>(
                QStringLiteral("vaultTitleEdit"));
            auto *username = fixtureDock.findChild<QLineEdit *>(
                QStringLiteral("vaultUsernameEdit"));
            auto *password = fixtureDock.findChild<QLineEdit *>(
                QStringLiteral("vaultPasswordEdit"));
            QVERIFY(title && username && password);
            title->setText(QStringLiteral("Production database"));
            username->setText(QStringLiteral("ops@example.test"));
            password->setText(QStringLiteral("fixture-only-secret"));
            QVERIFY(fixtureDock.saveActiveEntry());
            QVERIFY(fixtureDock.releaseWorkspaceSecrets());
        }
        foreignPassword.clear();

        qputenv("NOTEPADSAFE_DATADIR", mainIdentityDirectory.path().toUtf8());
        auto window = std::make_unique<MainWindow>();
        window->resize(1180, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto *stack = window->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        QVERIFY(stack);

        bool recoveryDialogWasShown = false;
        QTimer recoveryDialogTimer;
        recoveryDialogTimer.setSingleShot(true);
        connect(&recoveryDialogTimer, &QTimer::timeout, &recoveryDialogTimer, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QStringLiteral("fileCredentialDialog"));
            QVERIFY(dialog->windowTitle().contains(QStringLiteral("账户集合")));
            auto *kind = dialog->findChild<QComboBox *>(
                QStringLiteral("fileCredentialKind"));
            auto *recovery = dialog->findChild<QLineEdit *>(
                QStringLiteral("fileCredentialRecoveryKey"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(
                QStringLiteral("fileCredentialButtons"));
            QVERIFY(kind && recovery && buttons);
            const int recoveryIndex = kind->findText(QStringLiteral("恢复密钥"));
            QVERIFY(recoveryIndex >= 0);
            kind->setCurrentIndex(recoveryIndex);
            recovery->setText(foreignRecoveryKey);
            recoveryDialogWasShown = true;
            QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        });
        recoveryDialogTimer.start(80);
        window->openFilesFromExternal({foreignAccountsPath});
        recoveryDialogTimer.stop();
        QVERIFY(recoveryDialogWasShown);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 3000);
        auto *vault = window->findChild<VaultDock *>(QStringLiteral("VaultDock"));
        auto *tree = window->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        QVERIFY(vault && tree && vault->isSecureStoreOpen());
        auto *statusPosition = window->findChild<QLabel *>(
            QStringLiteral("status.position"));
        auto *statusEncoding = window->findChild<QLabel *>(
            QStringLiteral("status.encoding"));
        auto *statusContext = window->findChild<QLabel *>(
            QStringLiteral("status.context"));
        auto *statusSize = window->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(statusPosition && statusEncoding && statusContext && statusSize);
        QVERIFY(statusPosition->isHidden());
        QVERIFY(statusEncoding->isHidden());
        QVERIFY(statusSize->isHidden());
        QVERIFY(statusContext->isVisible());
        QCOMPARE(statusContext->text(), QStringLiteral("账户"));
        QCOMPARE(QFileInfo(vault->secureStorePath()).absoluteFilePath(),
                 QFileInfo(foreignAccountsPath).absoluteFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(treeContainsText(tree,
                                                  QStringLiteral("Production database")),
                                 3000);
        QTreeWidgetItem *accountItem = findTreeItem(
            tree, QStringLiteral("Production database"));
        QVERIFY(accountItem);
        tree->scrollToItem(accountItem);
        const QRect accountRect = tree->visualItemRect(accountItem);
        QVERIFY(accountRect.isValid());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, accountRect.center());
        auto *visibleTitle = window->findChild<QLineEdit *>(
            QStringLiteral("vaultTitleEdit"));
        auto *visibleUsername = window->findChild<QLineEdit *>(
            QStringLiteral("vaultUsernameEdit"));
        auto *visiblePassword = window->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        QVERIFY(visibleTitle && visibleUsername && visiblePassword);
        QTRY_COMPARE_WITH_TIMEOUT(visibleTitle->text(),
                                  QStringLiteral("Production database"), 3000);
        QCOMPARE(visibleUsername->text(), QStringLiteral("ops@example.test"));
        QVERIFY(visiblePassword->text().isEmpty());
        auto *copyPassword = window->findChild<QPushButton *>(
            QStringLiteral("vaultCopyPasswordButton"));
        QVERIFY(copyPassword && copyPassword->isVisible() && copyPassword->isEnabled());
        QTest::mouseClick(copyPassword, Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("fixture-only-secret"));
        QTRY_VERIFY_WITH_TIMEOUT(window->statusBar()->currentMessage().contains(
                                     QStringLiteral("30 秒后")), 1000);
        auto *detailHeader = window->findChild<QWidget *>(
            QStringLiteral("vaultDetailHeader"));
        auto *editAccount = window->findChild<QPushButton *>(
            QStringLiteral("vaultEditButton"));
        auto *deleteAccount = window->findChild<QPushButton *>(
            QStringLiteral("vaultDeleteButton"));
        QVERIFY(detailHeader && editAccount && deleteAccount);
        QCOMPARE(visibleTitle->parentWidget(), detailHeader);
        QCOMPARE(editAccount->parentWidget(), detailHeader);
        QCOMPARE(deleteAccount->parentWidget(), detailHeader);
        QVERIFY(editAccount->isVisible() && deleteAccount->isVisible());
        QVERIFY(editAccount->geometry().center().y() < detailHeader->height());
        QVERIFY(deleteAccount->geometry().right()
                <= detailHeader->contentsRect().right());
        const QString screenshot = QDir::current().filePath(
            QStringLiteral("gui-foreign-account-recovery.png"));
        QVERIFY2(window->grab().save(screenshot), qPrintable(screenshot));

        window->resize(940, 680);
        QTest::qWait(50);
        QVERIFY(deleteAccount->geometry().right()
                <= detailHeader->contentsRect().right());
        const QString narrowScreenshot = QDir::current().filePath(
            QStringLiteral("gui-foreign-account-recovery-narrow.png"));
        QVERIFY2(window->grab().save(narrowScreenshot), qPrintable(narrowScreenshot));

        // Global editor commands keep their expected meaning in the Accounts
        // workspace: Ctrl+F focuses account search and Ctrl+S saves an edit.
        auto *accountFilter = window->findChild<QLineEdit *>(
            QStringLiteral("vaultFilterEdit"));
        auto *findAction = window->findChild<QAction *>(QStringLiteral("search.find"));
        auto *saveAction = window->findChild<QAction *>(QStringLiteral("file.save"));
        QVERIFY(accountFilter && findAction && saveAction);
        QVERIFY(findAction->isEnabled());
        QTest::keyClick(window.get(), Qt::Key_F, Qt::ControlModifier);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(accountFilter));
        QTest::mouseClick(editAccount, Qt::LeftButton);
        QTRY_VERIFY(visibleUsername->isEnabled() && !visibleUsername->isReadOnly());
        visibleUsername->setText(QStringLiteral("ops-updated@example.test"));
        QTRY_VERIFY(saveAction->isEnabled());
        QTest::keyClick(visibleUsername, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(editAccount->isVisible() && editAccount->isEnabled());
        QCOMPARE(visibleUsername->text(), QStringLiteral("ops-updated@example.test"));
        QVERIFY(!saveAction->isEnabled());

        auto *back = window->findChild<QPushButton *>(
            QStringLiteral("accountsBackToDocuments"));
        auto *accountsAction = window->findChild<QAction *>(QStringLiteral("tools.accounts"));
        QVERIFY(back && accountsAction);
        QTest::mouseClick(back, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!vault->isSecureStoreOpen(), 3000);

        // The normal Accounts command must now reuse the remembered foreign
        // key without exposing another credentials dialog.
        bool unexpectedDialog = false;
        QTimer sameWindowWatchdog;
        sameWindowWatchdog.setSingleShot(true);
        connect(&sameWindowWatchdog, &QTimer::timeout, &sameWindowWatchdog, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            unexpectedDialog = true;
            dialog->reject();
        });
        sameWindowWatchdog.start(300);
        accountsAction->trigger();
        sameWindowWatchdog.stop();
        QVERIFY(!unexpectedDialog);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(vault->isSecureStoreOpen(), 3000);
        QVERIFY(treeContainsText(tree, QStringLiteral("Production database")));
        QTest::mouseClick(back, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!vault->isSecureStoreOpen(), 3000);

        window->close();
        window.reset();

        // Remembered keys live inside the DPAPI-sealed seven-day capsule, not
        // MainWindow. Reconstructing the application window must remain silent.
        auto restartedWindow = std::make_unique<MainWindow>();
        restartedWindow->resize(1180, 760);
        restartedWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restartedWindow.get()));
        auto *restartedStack = restartedWindow->findChild<QStackedWidget *>(
            QStringLiteral("WorkspaceStack"));
        auto *restartedVault = restartedWindow->findChild<VaultDock *>(
            QStringLiteral("VaultDock"));
        auto *restartedTree = restartedWindow->findChild<QTreeWidget *>(
            QStringLiteral("vaultEntryTree"));
        auto *restartedAccounts = restartedWindow->findChild<QAction *>(
            QStringLiteral("tools.accounts"));
        QVERIFY(restartedStack && restartedVault && restartedTree && restartedAccounts);

        unexpectedDialog = false;
        QTimer restartedWindowWatchdog;
        restartedWindowWatchdog.setSingleShot(true);
        connect(&restartedWindowWatchdog, &QTimer::timeout,
                &restartedWindowWatchdog, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            unexpectedDialog = true;
            dialog->reject();
        });
        restartedWindowWatchdog.start(300);
        restartedAccounts->trigger();
        restartedWindowWatchdog.stop();
        QVERIFY(!unexpectedDialog);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(restartedStack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 3000);
        QVERIFY(restartedVault->isSecureStoreOpen());
        QVERIFY(treeContainsText(restartedTree, QStringLiteral("Production database")));

        // Destructive actions stay recoverable by default and report their
        // result in the same unobtrusive status channel as secure copy.
        auto *restartedItem = findTreeItem(
            restartedTree, QStringLiteral("Production database"));
        auto *restartedDelete = restartedWindow->findChild<QPushButton *>(
            QStringLiteral("vaultDeleteButton"));
        auto *restartedRestore = restartedWindow->findChild<QPushButton *>(
            QStringLiteral("vaultRestoreButton"));
        QVERIFY(restartedItem && restartedDelete && restartedRestore);
        restartedTree->setCurrentItem(restartedItem);
        QTRY_VERIFY_WITH_TIMEOUT(restartedDelete->isVisible(), 1000);

        QTimer confirmRecycleTimer;
        confirmRecycleTimer.setSingleShot(true);
        connect(&confirmRecycleTimer, &QTimer::timeout, &confirmRecycleTimer, []() {
            auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (message && message->button(QMessageBox::Yes))
                QTest::mouseClick(message->button(QMessageBox::Yes), Qt::LeftButton);
        });
        confirmRecycleTimer.start(50);
        QTest::mouseClick(restartedDelete, Qt::LeftButton);
        confirmRecycleTimer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(restartedWindow->statusBar()->currentMessage().contains(
                                     QStringLiteral("已移到回收站")), 1000);

        restartedItem = findTreeItem(restartedTree,
                                     QStringLiteral("Production database"));
        QVERIFY(restartedItem);
        restartedTree->setCurrentItem(restartedItem);
        QTRY_VERIFY_WITH_TIMEOUT(restartedRestore->isVisible(), 1000);
        QTest::mouseClick(restartedRestore, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(restartedWindow->statusBar()->currentMessage().contains(
                                     QStringLiteral("已从回收站恢复")), 1000);
        QVERIFY(treeContainsText(restartedTree, QStringLiteral("Production database")));

        foreignRecoveryKey.fill(QChar(0));
        foreignRecoveryKey.clear();
        restartedWindow->close();
        restartedWindow.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void unifiedOpenForeignDocumentWithRecovery()
    {
        QTemporaryDir mainIdentityDirectory;
        QTemporaryDir foreignIdentityDirectory;
        QVERIFY(mainIdentityDirectory.isValid());
        QVERIFY(foreignIdentityDirectory.isValid());

        const QByteArray mainPasswordBytes("main identity password");
        SecureBuffer mainPassword(mainPasswordBytes.constData(), mainPasswordBytes.size());
        QString mainRecoveryKey;
        {
            SecuritySessionService mainSession(mainIdentityDirectory.path());
            const VaultError error = mainSession.createIdentity(mainPassword, &mainRecoveryKey);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
        }
        mainPassword.clear();
        mainRecoveryKey.fill(QChar(0));
        mainRecoveryKey.clear();

        const QByteArray foreignPasswordBytes("foreign identity password");
        SecureBuffer foreignPassword(foreignPasswordBytes.constData(),
                                     foreignPasswordBytes.size());
        QString foreignRecoveryKey;
        const QString firstPath = foreignIdentityDirectory.filePath(
            QStringLiteral("foreign-one.nsafe"));
        const QString secondPath = foreignIdentityDirectory.filePath(
            QStringLiteral("foreign-two.nsafe"));
        {
            SecuritySessionService foreignSession(foreignIdentityDirectory.path());
            VaultError error = foreignSession.createIdentity(foreignPassword,
                                                              &foreignRecoveryKey);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));

            SecureDocumentService foreignDocuments(&foreignSession,
                                                    foreignIdentityDirectory.path());
            SecureDocumentSnapshot snapshot;
            error = foreignDocuments.saveAsSecure(
                QStringLiteral("FOREIGN_RECOVERY_ONE"), QStringLiteral("Foreign one"),
                firstPath, &snapshot);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
            error = foreignDocuments.saveAsSecure(
                QStringLiteral("FOREIGN_RECOVERY_TWO"), QStringLiteral("Foreign two"),
                secondPath, &snapshot);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
        }
        foreignPassword.clear();

        qputenv("NOTEPADSAFE_DATADIR", mainIdentityDirectory.path().toUtf8());
        auto window = std::make_unique<MainWindow>();
        window->resize(900, 620);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        const int initialTabCount = tabs->count();

        bool rejectedPasswordWasShown = false;
        bool recoveryDialogWasShown = false;
        QTimer::singleShot(80, window.get(), [&, recoveryKey = foreignRecoveryKey]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QStringLiteral("fileCredentialDialog"));
            QVERIFY(dialog->windowTitle().contains(QStringLiteral("其他密码")));
            auto *password = dialog->findChild<QLineEdit *>(
                QStringLiteral("fileCredentialPassphrase"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>(
                QStringLiteral("fileCredentialButtons"));
            QVERIFY(password && buttons);
            password->setText(QStringLiteral("definitely wrong password"));

            QTimer::singleShot(50, window.get(), [&, recoveryKey]() {
                auto *retryDialog = qobject_cast<QDialog *>(
                    QApplication::activeModalWidget());
                QVERIFY(retryDialog);
                QCOMPARE(retryDialog->objectName(),
                         QStringLiteral("fileCredentialDialog"));
                auto *errorLabel = retryDialog->findChild<QLabel *>(
                    QStringLiteral("fileCredentialError"));
                auto *kind = retryDialog->findChild<QComboBox *>(
                    QStringLiteral("fileCredentialKind"));
                auto *recovery = retryDialog->findChild<QLineEdit *>(
                    QStringLiteral("fileCredentialRecoveryKey"));
                auto *retryButtons = retryDialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("fileCredentialButtons"));
                QVERIFY(errorLabel && kind && recovery && retryButtons);
                rejectedPasswordWasShown = !errorLabel->text().trimmed().isEmpty();
                QVERIFY(rejectedPasswordWasShown);

                const int recoveryIndex = kind->findText(QStringLiteral("恢复密钥"));
                QVERIFY(recoveryIndex >= 0);
                kind->setCurrentIndex(recoveryIndex);
                recovery->setText(recoveryKey);
                recoveryDialogWasShown = true;
                const QString screenshot = QDir::current().filePath(
                    QStringLiteral("gui-file-credential-recovery.png"));
                QVERIFY2(retryDialog->grab().save(screenshot), qPrintable(screenshot));
                QTest::mouseClick(retryButtons->button(QDialogButtonBox::Open),
                                  Qt::LeftButton);
            });

            QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        });

        window->openFilesFromExternal({firstPath});
        QVERIFY(rejectedPasswordWasShown);
        QVERIFY(recoveryDialogWasShown);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QCOMPARE(tabs->count(), initialTabCount + 1);
        auto *first = currentEditor(window.get());
        QVERIFY(first && first->isSecureDocument());
        QCOMPARE(first->editor()->text(), QStringLiteral("FOREIGN_RECOVERY_ONE"));
        auto *payloadSize = window->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(payloadSize);
        QCOMPARE(payloadSize->text(), QStringLiteral("20 B · S1"));
        QVERIFY(QFileInfo(firstPath).size() > first->editor()->length());
        first->editor()->append(QStringLiteral("!"));
        QTRY_COMPARE_WITH_TIMEOUT(payloadSize->text(), QStringLiteral("21 B · S1"), 1000);

        // The recovery secret is remembered by identityId + keyVersion in the
        // device session. A second document from that identity must therefore
        // behave exactly like an ordinary file and open without another dialog.
        window->openFilesFromExternal({secondPath});
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QCOMPARE(tabs->count(), initialTabCount + 2);
        auto *second = currentEditor(window.get());
        QVERIFY(second && second->isSecureDocument());
        QCOMPARE(second->editor()->text(), QStringLiteral("FOREIGN_RECOVERY_TWO"));

        foreignRecoveryKey.fill(QChar(0));
        foreignRecoveryKey.clear();
        window->close();
        window.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void encryptedUnnamedDraftRestoresSilentlyAcrossRestart()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());

        const QString body = QStringLiteral(
            "Restart-safe private note\r\n不会写入明文会话\r\n");
        QString encryptedDraftPath;
        {
            auto window = std::make_unique<MainWindow>();
            window->resize(980, 680);
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));

            QTimer::singleShot(80, window.get(), []() {
                auto *identity = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(identity);
                auto *password = identity->findChild<QLineEdit *>(
                    QStringLiteral("securitySessionPassword"));
                auto *confirmation = identity->findChild<QLineEdit *>(
                    QStringLiteral("securitySessionConfirmation"));
                auto *buttons = identity->findChild<QDialogButtonBox *>();
                QVERIFY(password && confirmation && buttons);
                password->setText(QStringLiteral("correct horse battery staple"));
                confirmation->setText(QStringLiteral("correct horse battery staple"));
                QTimer::singleShot(120, identity, []() {
                    confirmRecoverySeedFromIdentitySetup();
                });
                QTest::mouseClick(buttons->button(QDialogButtonBox::Ok),
                                  Qt::LeftButton);
            });
            QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile",
                                              Qt::DirectConnection));
            EditorTab *created = currentEditor(window.get());
            QVERIFY(created && created->isSecureDocument());
            QVERIFY(created->filePath().isEmpty());
            created->editor()->setText(body);
            QTRY_VERIFY_WITH_TIMEOUT(!created->autosavePath().isEmpty()
                                         && QFileInfo::exists(created->autosavePath()),
                                     5000);
            encryptedDraftPath = created->autosavePath();
            QFile encrypted(encryptedDraftPath);
            QVERIFY(encrypted.open(QIODevice::ReadOnly));
            const QByteArray diskBytes = encrypted.readAll();
            QCOMPARE(diskBytes.left(8).toHex().toUpper(),
                     QByteArray("03D9A29A67FB4BB5"));
            QVERIFY(!diskBytes.contains(body.toUtf8()));

            window->close();
            QVERIFY(!window->isVisible());
        }

        QVERIFY(QFileInfo::exists(encryptedDraftPath));
        QVERIFY(QFileInfo::exists(dataDirectory.filePath(
            QStringLiteral("security-session-v1.bin"))));
        QVERIFY(QFileInfo::exists(dataDirectory.filePath(
            QStringLiteral("session.json"))));

        auto restarted = std::make_unique<MainWindow>();
        restarted->resize(980, 680);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        bool unexpectedPrompt = false;
        QTimer promptWatcher;
        connect(&promptWatcher, &QTimer::timeout, restarted.get(), [&]() {
            if (auto *modal = qobject_cast<QDialog *>(
                    QApplication::activeModalWidget())) {
                unexpectedPrompt = true;
                modal->reject();
            }
        });
        promptWatcher.start(20);
        restarted->deferredRestoreSession();
        auto *tabs = restarted->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(currentEditor(restarted.get())
                                     && currentEditor(restarted.get())->isSecureDocument(),
                                 5000);
        QTRY_COMPARE_WITH_TIMEOUT(currentEditor(restarted.get())->editor()->text(),
                                  body, 5000);
        promptWatcher.stop();
        QVERIFY(!unexpectedPrompt);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        EditorTab *restored = currentEditor(restarted.get());
        QCOMPARE(QFileInfo(restored->autosavePath()).absoluteFilePath(),
                 QFileInfo(encryptedDraftPath).absoluteFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(dataDirectory.filePath(
                                     QStringLiteral("session.json"))), 2000);
        QVERIFY(restarted->grab().save(QDir::current().filePath(
            QStringLiteral("gui-secure-draft-restart.png"))));

        restarted->close();
        restarted.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void operationLeaseWorkspaceLifecycle()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());

        auto window = std::make_unique<MainWindow>();
        window->resize(1180, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        QTimer::singleShot(80, window.get(), []() {
            auto *identity = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(identity);
            auto *password = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionPassword"));
            auto *confirmation = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionConfirmation"));
            auto *buttons = identity->findChild<QDialogButtonBox *>();
            QVERIFY(password && confirmation && buttons);
            QTest::keyClicks(password, QStringLiteral("correct horse battery staple"));
            QTest::keyClicks(confirmation, QStringLiteral("correct horse battery staple"));
            QTimer::singleShot(120, identity, []() {
                confirmRecoverySeedFromIdentitySetup();
            });
            QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        });
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));

        auto *tabs = window->findChild<QTabWidget *>();
        auto *stack = window->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        auto *secureTab = currentEditor(window.get());
        QVERIFY(tabs && stack && secureTab);
        QVERIFY(secureTab->isSecureDocument());
        QVERIFY(!tabs->tabText(tabs->currentIndex()).contains(QStringLiteral("🔒")));

        secureTab->editor()->setFocus();
        QTest::keyClicks(secureTab->editor(), QStringLiteral("Daily note"));
        QTest::keyPress(secureTab->editor(), Qt::Key_Return);
        QTest::keyClicks(secureTab->editor(), QStringLiteral("MARKER_SHARED secure text"));
        QTest::keyPress(secureTab->editor(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(secureTab->autosavePath()), 5000);
        QFile encryptedDraft(secureTab->autosavePath());
        QVERIFY(encryptedDraft.open(QIODevice::ReadOnly));
        QCOMPARE(encryptedDraft.read(8).toHex().toUpper(), QByteArray("03D9A29A67FB4BB5"));

        const QString firstPlain = dataDirectory.filePath(QStringLiteral("one.log"));
        const QString secondPlain = dataDirectory.filePath(QStringLiteral("two.log"));
        QVERIFY(writeFixture(firstPlain, QByteArray("INFO start\nMARKER_SHARED first\n")));
        QVERIFY(writeFixture(secondPlain,
                             QByteArray("WARN before\nMARKER_SHARED second\nERROR end\n")));
        window->openFilesFromExternal({firstPlain, secondPlain});
        QCOMPARE(tabs->count(), 3);

        QVERIFY(QMetaObject::invokeMethod(window.get(), "showFindDialog", Qt::DirectConnection));
        auto *findDialog = window->findChild<FindReplaceDialog *>();
        auto *pattern = findDialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = findDialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *findAll = findDialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        QVERIFY(findDialog && pattern && scope && findAll);
        pattern->setText(QStringLiteral("MARKER_SHARED"));
        scope->setCurrentIndex(scope->findData(1));
        QTest::mouseClick(findAll, Qt::LeftButton);

        auto *results = window->findChild<SearchResultsDock *>(
            QStringLiteral("SearchResultsDock"));
        QVERIFY(results);
        QTRY_VERIFY_WITH_TIMEOUT(results->isVisible(), 2000);
        auto *tree = results->findChild<QTreeView *>(
            QStringLiteral("searchResultsTree"));
        QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 3, 5000);

        bool keywordHighlightedInDock = false;
        QModelIndex secondFileMatch;
        for (int groupIndex = 0; groupIndex < tree->model()->rowCount(); ++groupIndex) {
            const QModelIndex group = tree->model()->index(groupIndex, 0);
            const bool isSecondFile = group.data(Qt::DisplayRole).toString().contains(
                QStringLiteral("two.log"));
            for (int childIndex = 0;
                 childIndex < tree->model()->rowCount(group); ++childIndex) {
                const QModelIndex context = tree->model()->index(
                    childIndex, SearchResultsModel::COL_CONTEXT, group);
                const QString text = context.data(SearchResultsModel::ContextRole).toString();
                const QVariantList ranges = context.data(
                    SearchResultsModel::MatchPositionsRole).toList();
                keywordHighlightedInDock |= text.contains(QStringLiteral("MARKER_SHARED"))
                                            && !ranges.isEmpty();
                if (isSecondFile)
                    secondFileMatch = context;
            }
        }
        QVERIFY(keywordHighlightedInDock);
        QVERIFY(secondFileMatch.isValid());

        tree->expand(secondFileMatch.parent());
        tree->scrollTo(secondFileMatch);
        const QRect matchRect = tree->visualRect(secondFileMatch);
        QVERIFY(matchRect.isValid());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, matchRect.center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, matchRect.center());
        QTRY_VERIFY(currentEditor(window.get())->filePath().endsWith(QStringLiteral("two.log")));
        QTRY_COMPARE_WITH_TIMEOUT(currentEditor(window.get())->editor()->selectedText(),
                                  QStringLiteral("MARKER_SHARED"), 2000);

        const QString searchScreenshot = QDir::current().filePath(
            QStringLiteral("gui-maturity-search.png"));
        QVERIFY2(window->grab().save(searchScreenshot), qPrintable(searchScreenshot));

        QAction *accountsAction = window->findChild<QAction *>(QStringLiteral("tools.accounts"));
        QVERIFY(accountsAction);
        accountsAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 4000);
        auto *vault = window->findChild<VaultDock *>(QStringLiteral("VaultDock"));
        QVERIFY(vault && vault->isVisible());
        QCOMPARE(window->dockWidgetArea(vault), Qt::NoDockWidgetArea);
        QVERIFY(QFileInfo::exists(dataDirectory.filePath(QStringLiteral("accounts.kdbx"))));

        const QString accountsScreenshot = QDir::current().filePath(
            QStringLiteral("gui-maturity-accounts.png"));
        QVERIFY2(window->grab().save(accountsScreenshot), qPrintable(accountsScreenshot));

        // Returning to ordinary documents must release the account database's
        // resident secrets without ending the seven-day session or masking an
        // unrelated encrypted document. Re-entering Accounts is then silent.
        auto *backToDocuments = window->findChild<QPushButton *>(
            QStringLiteral("accountsBackToDocuments"));
        QVERIFY(backToDocuments);
        QTest::mouseClick(backToDocuments, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     != QStringLiteral("AccountsWorkspace"), 3000);
        QVERIFY(!vault->isSecureStoreOpen());
        QVERIFY(!secureTab->isSecureDocumentMasked());
        QVERIFY(secureTab->editor()->text().contains(QStringLiteral("MARKER_SHARED")));
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        accountsAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(vault->isSecureStoreOpen(), 4000);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        const QString leaseScreenshot = QDir::current().filePath(
            QStringLiteral("gui-account-session-reopen.png"));
        QVERIFY2(window->grab().save(leaseScreenshot), qPrintable(leaseScreenshot));

#ifdef Q_OS_WIN
        // Drive the real native-event path used by Windows workstation lock.
        const QString capsule = dataDirectory.filePath(
            QStringLiteral("security-session-v1.bin"));
        QVERIFY(QFileInfo::exists(capsule));
        ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_WTSSESSION_CHANGE,
                       WTS_SESSION_LOCK, 0);
        QTRY_VERIFY_WITH_TIMEOUT(secureTab->isSecureDocumentMasked(), 3000);
        QVERIFY(!secureTab->editor()->text().contains(QStringLiteral("MARKER_SHARED")));
        QVERIFY(!results->isVisible());
        QVERIFY(!tree->model() || tree->model()->rowCount() == 0);
        QCOMPARE(pattern->text(), QString());
        QVERIFY(QFileInfo::exists(capsule)); // system lock keeps the seven-day capsule

        // Undo must not resurrect plaintext from Scintilla after masking.
        secureTab->editor()->SendScintilla(QsciScintillaBase::SCI_UNDO);
        QVERIFY(!secureTab->editor()->text().contains(QStringLiteral("MARKER_SHARED")));

        ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_WTSSESSION_CHANGE,
                       WTS_SESSION_UNLOCK, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!secureTab->isSecureDocumentMasked(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(secureTab->editor()->text().contains(
                                     QStringLiteral("MARKER_SHARED")), 5000);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 5000);
#endif

        QAction *lockAction = actionWithText(window.get(), QStringLiteral("立即锁定"));
        QVERIFY(lockAction);
        lockAction->trigger();
        QVERIFY(secureTab->isSecureDocumentMasked());
        QVERIFY(!secureTab->editor()->text().contains(QStringLiteral("MARKER_SHARED")));
        QVERIFY(!QFileInfo::exists(dataDirectory.filePath(
            QStringLiteral("security-session-v1.bin"))));

        window->close();
        window.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void secureClipboardIsConditionalAndClearedOnLock()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());

        auto window = std::make_unique<MainWindow>();
        window->resize(1180, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        QTimer::singleShot(80, window.get(), []() {
            auto *identity = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(identity);
            auto *password = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionPassword"));
            auto *confirmation = identity->findChild<QLineEdit *>(
                QStringLiteral("securitySessionConfirmation"));
            auto *buttons = identity->findChild<QDialogButtonBox *>();
            QVERIFY(password && confirmation && buttons);
            QTest::keyClicks(password, QStringLiteral("correct horse battery staple"));
            QTest::keyClicks(confirmation, QStringLiteral("correct horse battery staple"));
            QTimer::singleShot(120, identity, []() {
                confirmRecoverySeedFromIdentitySetup();
            });
            QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        });
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));

        EditorTab *tab = currentEditor(window.get());
        QVERIFY(tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked());
        const QString secret = QStringLiteral("CLIPBOARD_SECRET_74f6d18c");
        tab->editor()->setText(secret);
        tab->editor()->selectAll();
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(), secret);
        QTest::keyClick(tab->editor(), Qt::Key_Right);
        QVERIFY(!tab->hasSelectionCommand());
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), secret);

#ifdef Q_OS_WIN
        // Exercise the native Windows workstation-lock route: copied secure
        // note text must disappear together with the editor plaintext.
        ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_WTSSESSION_CHANGE,
                       WTS_SESSION_LOCK, 0);
        QTRY_VERIFY_WITH_TIMEOUT(tab->isSecureDocumentMasked(), 3000);
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(tabs->indexOf(tab)), QStringLiteral("已锁定文档"));
        QVERIFY(!tabs->tabText(tabs->indexOf(tab)).contains(secret));
        const bool clipboardWasCleared = QApplication::clipboard()->text().isEmpty();
        QApplication::clipboard()->clear();
        QVERIFY2(clipboardWasCleared,
                 "secure document plaintext remained in the clipboard after system lock");
        QVERIFY(window->grab().save(QDir::current().filePath(
            QStringLiteral("gui-secure-clipboard-locked.png"))));

        ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_WTSSESSION_CHANGE,
                       WTS_SESSION_UNLOCK, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isSecureDocumentMasked(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(), secret, 5000);

        // Clipboard cleanup is fingerprint-conditional. Never erase content
        // another application/user copied after NotepadSafe's secure copy.
        tab->editor()->selectAll();
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(), secret);
        const QString externalReplacement = QStringLiteral("external clipboard value");
        QApplication::clipboard()->setText(externalReplacement);
        QAction *lockAction = actionWithText(window.get(), QStringLiteral("立即锁定"));
        QVERIFY(lockAction);
        lockAction->trigger();
        QVERIFY(tab->isSecureDocumentMasked());
        QCOMPARE(QApplication::clipboard()->text(), externalReplacement);
#endif

        QApplication::clipboard()->clear();
        window->close();
        window.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }
};

QTEST_MAIN(SecureDocumentsSmokeTest)
#include "smoke_secure_documents.moc"
