#include "EditorTab.h"
#include "AppComposition.h"
#include "FindOverlay.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SecureBuffer.h"
#include "SecureDocumentService.h"
#include "SecuritySessionService.h"
#include "SessionManager.h"
#include "StartupCoordinator.h"
#include "TitleBar.h"
#include "VaultDock.h"
#include "VirtualFileView.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
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
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QSplitter>
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

QByteArray readFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool isOwnedByWindow(const QObject *object, const MainWindow *window)
{
    if (!window) return false;
    // QObject ownership crosses the top-level QDialog window boundary.
    for (const QObject *owner = object; owner; owner = owner->parent()) {
        if (owner == window) return true;
    }
    return false;
}

QString openThroughFileMenu(MainWindow *window, const QString &path)
{
    auto *action = window ? window->findChild<QAction *>(QStringLiteral("file.open")) : nullptr;
    if (!action || !action->isEnabled() || !QFileInfo(path).isFile())
        return QStringLiteral("explicit Open fixture/action is unavailable");
    const bool previouslyNonNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const QPointer<MainWindow> owner(window);
    QPointer<QFileDialog> chooser;
    bool clickedOpen = false;
    QString error;
    QElapsedTimer elapsed;
    QTimer driver;
    driver.setInterval(20);
    const auto cleanup = qScopeGuard([&] {
        driver.stop();
        if (chooser && chooser->isVisible()) chooser->reject();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previouslyNonNative);
    });
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!owner || elapsed.elapsed() >= 8000) {
            if (error.isEmpty()) error = QStringLiteral("explicit Open workflow exceeded its 8 s fixture deadline");
            // Keep retiring only this owner's unfinished dialogs after timeout,
            // including a subsequent product error box. Never close another window.
            if (modal && isOwnedByWindow(modal, owner.data())) modal->reject();
            return;
        }
        if (clickedOpen || !modal) return;
        auto *candidate = qobject_cast<QFileDialog *>(modal);
        if (!candidate || candidate->parentWidget() != owner.data()) return;
        chooser = candidate;
        chooser->selectFile(path);
        const QStringList selected = chooser->selectedFiles();
        if (selected.size() != 1
            || QFileInfo(selected.front()).absoluteFilePath() != QFileInfo(path).absoluteFilePath()) return;
        auto *buttons = chooser->findChild<QDialogButtonBox *>();
        auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
        if (!open || !open->isVisible() || !open->isEnabled()) return;
        clickedOpen = true;
        QTest::mouseClick(open, Qt::LeftButton);
    });
    elapsed.start();
    driver.start();
    action->trigger(); // Public user command -> real Qt chooser -> normal interactive route.
    driver.stop();
    if (!clickedOpen && error.isEmpty()) error = QStringLiteral("owned QFileDialog Open button was not clicked");
    return error;
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

// Answers the 登录 Notepad Safe prompt for an identity created outside the
// window. The driver stops itself after the first login or after 8 seconds.
void driveLoginDialog(MainWindow *window)
{
    auto *driver = new QTimer(window);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    driver->setInterval(25);
    QObject::connect(driver, &QTimer::timeout, window, [driver, elapsed]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        auto *password = dialog ? dialog->findChild<QLineEdit *>(
            QStringLiteral("securitySessionPassword")) : nullptr;
        auto *buttons = dialog ? dialog->findChild<QDialogButtonBox *>() : nullptr;
        auto *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
        if (password && ok && !dialog->findChild<QLineEdit *>(
                QStringLiteral("securitySessionConfirmation"))) {
            password->setText(QStringLiteral("correct horse battery staple"));
            driver->stop();
            driver->deleteLater();
            ok->click();
            return;
        }
        if (elapsed->elapsed() > 8000) {
            driver->stop();
            driver->deleteLater();
        }
    });
    driver->start();
}

// Waits for the current tab to be a secure binary; on timeout returns what
// was on screen instead (tab state or the blocking modal's title and text).
QString waitForSecureBinaryTab(MainWindow *window)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QString seen;
    while (elapsed.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        EditorTab *tab = currentEditor(window);
        if (tab && tab->isSecureBinaryDocument())
            return {};
        if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            auto *box = qobject_cast<QMessageBox *>(modal);
            seen = QStringLiteral("modal '%1': %2").arg(
                modal->windowTitle(), box ? box->text() : QString());
        } else if (tab) {
            seen = QStringLiteral("tab '%1' secure=%2 binary=%3").arg(
                tab->filePath()).arg(tab->isSecureDocument()).arg(tab->isBinaryDocument());
        } else {
            seen = QStringLiteral("no tab");
        }
    }
    if (auto *modal = QApplication::activeModalWidget())
        modal->close();
    return seen;
}

void driveIdentityCreationDialog(MainWindow *window)
{
    constexpr auto ResultProperty = "notepadsafeTestIdentityDriverResult";
    window->setProperty(ResultProperty, QStringLiteral("pending"));
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto *driver = new QTimer(window);
    driver->setInterval(25);
    driver->setProperty("stage", 0);
    QObject::connect(driver, &QTimer::timeout, window,
                     [window, driver, elapsed]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        const int stage = driver->property("stage").toInt();

        if (dialog && stage == 0) {
            auto *password = dialog->findChild<QLineEdit *>(
                QStringLiteral("securitySessionPassword"));
            auto *confirmation = dialog->findChild<QLineEdit *>(
                QStringLiteral("securitySessionConfirmation"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            auto *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            if (password && confirmation && ok) {
                password->setText(QStringLiteral("correct horse battery staple"));
                confirmation->setText(QStringLiteral("correct horse battery staple"));
                driver->setProperty("stage", 1);
                ok->click();
                return;
            }
        } else if (dialog && stage == 1) {
            auto *seedView = dialog->findChild<QPlainTextEdit *>(
                QStringLiteral("securityRecoveryKey"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            auto *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            if (seedView && ok) {
                auto *verify = dialog->findChild<QLineEdit *>(
                    QStringLiteral("securitySessionRecoveryVerify"));
                if (!verify)
                    verify = dialog->findChild<QLineEdit *>();
                auto *confirmed = dialog->findChild<QCheckBox *>(
                    QStringLiteral("securityRecoveryConfirmed"));
                if (verify)
                    verify->setText(seedView->toPlainText().right(8));
                else if (confirmed)
                    confirmed->setChecked(true);
                window->setProperty("notepadsafeTestIdentityDriverResult",
                                    QStringLiteral("complete"));
                driver->stop();
                driver->deleteLater();
                ok->click();
                return;
            }
        }

        if (elapsed->elapsed() < 8000)
            return;
        const QString title = dialog ? dialog->windowTitle()
                                     : QStringLiteral("<no active modal>");
        window->setProperty(
            "notepadsafeTestIdentityDriverResult",
            QStringLiteral("timed out in stage %1; active modal: %2")
                .arg(stage).arg(title));
        driver->stop();
        driver->deleteLater();
        if (dialog)
            dialog->reject();
    });
    driver->start();
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

        driveIdentityCreationDialog(window.get());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));
        QCOMPARE(window->property("notepadsafeTestIdentityDriverResult").toString(),
                 QStringLiteral("complete"));

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

    void plainBinarySaveAsOffersNoSecureType()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());
        const QString binaryPath = dataDirectory.filePath(QStringLiteral("plain.bin"));
        QByteArray payload(64, '\0');
        payload[3] = char(0x7f);
        QVERIFY(writeFixture(binaryPath, payload));

        auto window = std::make_unique<MainWindow>();
        window->resize(980, 680);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->openFilesFromExternal({binaryPath});
        EditorTab *tab = currentEditor(window.get());
        QVERIFY(tab && tab->isBinaryDocument() && !tab->isSecureDocument());

        QStringList filters;
        bool inspected = false;
        QTimer::singleShot(80, window.get(), [&]() {
            auto *dialog = window->findChild<QDialog *>(QStringLiteral("saveAsDialog"));
            auto *fileDialog = dialog ? dialog->findChild<QFileDialog *>(
                QStringLiteral("saveAsFileDialog")) : nullptr;
            if (fileDialog) {
                filters = fileDialog->nameFilters();
                inspected = true;
            }
            if (dialog)
                dialog->reject();
        });
        QAction *saveAsAction = window->findChild<QAction *>(QStringLiteral("file.saveAs"));
        QVERIFY(saveAsAction);
        saveAsAction->trigger();

        QVERIFY(inspected);
        QCOMPARE(filters, QStringList{QStringLiteral("未加密文件 (*)")});
        QVERIFY(!tab->isSecureDocument());
        QCOMPARE(readFileBytes(binaryPath), payload);

        window->close();
        window.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void existingSecureBinaryStillSavesAndExports()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());
        const QString securePath = dataDirectory.filePath(QStringLiteral("payload.nsafe"));
        const QString exportPath = dataDirectory.filePath(QStringLiteral("payload-export.bin"));
        QByteArray payload(64, '\0');
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = char(i * 7 + 1);
        {
            const QByteArray passwordBytes("correct horse battery staple");
            SecureBuffer password(passwordBytes.constData(), passwordBytes.size());
            QString recoveryKey;
            SecuritySessionService session(dataDirectory.path());
            VaultError error = session.createIdentity(password, &recoveryKey);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
            SecureDocumentService documents(&session, dataDirectory.path());
            SecureDocumentSnapshot snapshot;
            error = documents.saveAsSecureBinary(
                payload, QStringLiteral("payload"), securePath, &snapshot);
            QVERIFY2(!error.isError(), qPrintable(error.userMessage));
            password.clear();
        }
        QVERIFY(!readFileBytes(securePath).contains(payload));

        QByteArray edited = payload;
        edited[0] = char(0xAB);
        {
            auto window = std::make_unique<MainWindow>();
            window->resize(980, 680);
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));
            driveLoginDialog(window.get());
            {
                // External opens never unlock secure files; use 文件 › 打开.
                const QString openError = openThroughFileMenu(window.get(), securePath);
                QVERIFY2(openError.isEmpty(), qPrintable(openError));
            }
            {
                const QString seen = waitForSecureBinaryTab(window.get());
                QVERIFY2(seen.isEmpty(), qPrintable(seen));
            }
            EditorTab *tab = currentEditor(window.get());
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, payload.size()),
                                      payload, 3000);
            tab->gotoByteOffset(0);
            tab->editor()->setFocus(Qt::OtherFocusReason);
            QTest::keyClicks(tab->editor(), QStringLiteral("AB"));
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, payload.size()),
                                      edited, 1000);
            QVERIFY(QMetaObject::invokeMethod(window.get(), "saveFile",
                                              Qt::DirectConnection));
            QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 3000);
            window->close();
        }
        QVERIFY(!readFileBytes(securePath).contains(edited));

        auto window = std::make_unique<MainWindow>();
        window->resize(980, 680);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        driveLoginDialog(window.get());
        {
            // External opens never unlock secure files; use 文件 › 打开.
            const QString openError = openThroughFileMenu(window.get(), securePath);
            QVERIFY2(openError.isEmpty(), qPrintable(openError));
        }
        {
            const QString seen = waitForSecureBinaryTab(window.get());
            QVERIFY2(seen.isEmpty(), qPrintable(seen));
        }
        EditorTab *tab = currentEditor(window.get());
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, edited.size()),
                                  edited, 3000);

        QStringList filters;
        bool accepted = false;
        QString selectedFilter;
        QTimer::singleShot(80, window.get(), [&]() {
            auto *dialog = window->findChild<QDialog *>(QStringLiteral("saveAsDialog"));
            auto *fileDialog = dialog ? dialog->findChild<QFileDialog *>(
                QStringLiteral("saveAsFileDialog")) : nullptr;
            if (!fileDialog) {
                if (dialog)
                    dialog->reject();
                return;
            }
            filters = fileDialog->nameFilters();
            // Choose the plain type the way a user does: through the type combo.
            for (QComboBox *candidate : fileDialog->findChildren<QComboBox *>()) {
                if (candidate->count() == filters.size()
                    && candidate->itemText(0).contains(QStringLiteral("安全文档"))) {
                    candidate->setCurrentIndex(0);
                    candidate->setFocus();
                    QTest::keyClick(candidate, Qt::Key_Down);
                    break;
                }
            }
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            selectedFilter = fileDialog->selectedNameFilter();
            fileDialog->selectFile(exportPath);
            accepted = QMetaObject::invokeMethod(fileDialog, "accept",
                                                 Qt::DirectConnection);
        });
        QTimer::singleShot(5000, window.get(), [window = window.get()]() {
            if (auto *dialog = window->findChild<QDialog *>(
                    QStringLiteral("saveAsDialog")); dialog && dialog->isVisible()) {
                dialog->reject();
            }
        });
        QAction *saveAsAction = window->findChild<QAction *>(QStringLiteral("file.saveAs"));
        QVERIFY(saveAsAction);
        saveAsAction->trigger();

        QVERIFY(accepted);
        QCOMPARE(filters.size(), 2);
        QCOMPARE(selectedFilter, QStringLiteral("未加密文件 (*)"));
        QCOMPARE(readFileBytes(exportPath), edited);

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
        recoveryDialogTimer.setInterval(20);
        connect(&recoveryDialogTimer, &QTimer::timeout, &recoveryDialogTimer, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->parentWidget() != window.get()
                || dialog->objectName() != QStringLiteral("fileCredentialDialog")) return;
            recoveryDialogTimer.stop();
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
        recoveryDialogTimer.start();
        const QString accountOpenError = openThroughFileMenu(window.get(), foreignAccountsPath);
        recoveryDialogTimer.stop();
        QVERIFY2(accountOpenError.isEmpty(), qPrintable(accountOpenError));
        QVERIFY(recoveryDialogWasShown);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 3000);
        auto *vault = window->findChild<VaultDock *>(QStringLiteral("VaultDock"));
        auto *tree = window->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        QVERIFY(vault && tree && vault->isSecureStoreOpen());
        auto *statusPosition = window->findChild<QLabel *>(
            QStringLiteral("status.position"));
        auto *statusEncoding = window->findChild<QAbstractButton *>(
            QStringLiteral("status.encoding"));
        auto *statusContext = window->findChild<QAbstractButton *>(
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
        auto *restartedAccounts = restartedWindow->findChild<QAction *>(
            QStringLiteral("tools.accounts"));
        QVERIFY(restartedStack && restartedAccounts);
        // Accounts widgets are a first-use adapter and intentionally do not
        // exist before the first-paint/startup boundary.
        QVERIFY(restartedWindow->findChild<VaultDock *>(QStringLiteral("VaultDock"))
                == nullptr);

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
        auto *restartedVault = restartedWindow->findChild<VaultDock *>(
            QStringLiteral("VaultDock"));
        auto *restartedTree = restartedWindow->findChild<QTreeWidget *>(
            QStringLiteral("vaultEntryTree"));
        QVERIFY(restartedVault && restartedTree);
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
        bool credentialDriverTimedOut = false;
        int credentialStage = 0;
        QTimer credentialDriver;
        credentialDriver.setInterval(20);
        connect(&credentialDriver, &QTimer::timeout, window.get(),
                [&, recoveryKey = foreignRecoveryKey]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->parentWidget() != window.get()
                || dialog->objectName() != QStringLiteral("fileCredentialDialog"))
                return;
            if (credentialStage == 0) {
                if (!dialog->windowTitle().contains(QStringLiteral("其他密码")))
                    return;
                auto *password = dialog->findChild<QLineEdit *>(
                    QStringLiteral("fileCredentialPassphrase"));
                auto *buttons = dialog->findChild<QDialogButtonBox *>(
                    QStringLiteral("fileCredentialButtons"));
                if (!password || !buttons)
                    return;
                password->setText(QStringLiteral("definitely wrong password"));
                credentialStage = 1;
                QTest::mouseClick(buttons->button(QDialogButtonBox::Open),
                                  Qt::LeftButton);
                return;
            }

            auto *errorLabel = dialog->findChild<QLabel *>(
                QStringLiteral("fileCredentialError"));
            auto *kind = dialog->findChild<QComboBox *>(
                QStringLiteral("fileCredentialKind"));
            auto *recovery = dialog->findChild<QLineEdit *>(
                QStringLiteral("fileCredentialRecoveryKey"));
            auto *retryButtons = dialog->findChild<QDialogButtonBox *>(
                QStringLiteral("fileCredentialButtons"));
            if (!errorLabel || !kind || !recovery || !retryButtons
                || errorLabel->text().trimmed().isEmpty()) {
                return;
            }
            rejectedPasswordWasShown = !errorLabel->text().trimmed().isEmpty();
            const int recoveryIndex = kind->findText(QStringLiteral("恢复密钥"));
            if (recoveryIndex < 0)
                return;
            kind->setCurrentIndex(recoveryIndex);
            recovery->setText(recoveryKey);
            recoveryDialogWasShown = true;
            const QString screenshot = QDir::current().filePath(
                QStringLiteral("gui-file-credential-recovery.png"));
            if (!dialog->grab().save(screenshot))
                return;
            credentialStage = 2;
            credentialDriver.stop();
            QTest::mouseClick(retryButtons->button(QDialogButtonBox::Open),
                              Qt::LeftButton);
        });
        QTimer credentialWatchdog;
        credentialWatchdog.setSingleShot(true);
        connect(&credentialWatchdog, &QTimer::timeout, window.get(), [&]() {
            credentialDriverTimedOut = true;
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog && isOwnedByWindow(dialog, window.get())) dialog->reject();
        });
        credentialDriver.start();
        credentialWatchdog.start(8000);

        const QString firstOpenError = openThroughFileMenu(window.get(), firstPath);
        credentialDriver.stop();
        credentialWatchdog.stop();
        QVERIFY2(firstOpenError.isEmpty(), qPrintable(firstOpenError));
        QVERIFY(!credentialDriverTimedOut);
        QVERIFY(rejectedPasswordWasShown);
        QVERIFY(recoveryDialogWasShown);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QCOMPARE(tabs->count(), initialTabCount + 1);
        auto *first = currentEditor(window.get());
        QVERIFY(first && first->isSecureDocument());
        QCOMPARE(first->editor()->text(), QStringLiteral("FOREIGN_RECOVERY_ONE"));
        auto *payloadSize = window->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(payloadSize);
        QCOMPARE(payloadSize->text(), QStringLiteral("20 B · 文本 · 文本视图 · 可编辑"));
        QVERIFY(QFileInfo(firstPath).size() > first->editor()->length());
        first->editor()->append(QStringLiteral("!"));
        QTRY_COMPARE_WITH_TIMEOUT(
            payloadSize->text(), QStringLiteral("21 B · 文本 · 文本视图 · 可编辑"), 1000);

        // The recovery secret is remembered by identityId + keyVersion in the
        // device session. A second document from that identity must therefore
        // behave exactly like an ordinary file and open without another dialog.
        bool secondUnexpectedDialog = false;
        QTimer secondDialogGuard;
        secondDialogGuard.setInterval(20);
        connect(&secondDialogGuard, &QTimer::timeout, &secondDialogGuard, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || !isOwnedByWindow(dialog, window.get()) || qobject_cast<QFileDialog *>(dialog)
                || dialog->objectName() == QStringLiteral("securityOperationProgress")) return;
            secondUnexpectedDialog = true;
            dialog->reject();
        });
        secondDialogGuard.start();
        const QString secondOpenError = openThroughFileMenu(window.get(), secondPath);
        secondDialogGuard.stop();
        QVERIFY2(secondOpenError.isEmpty(), qPrintable(secondOpenError));
        QVERIFY(!secondUnexpectedDialog);
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
        QString createdDocumentId;
        {
            auto window = std::make_unique<MainWindow>();
            window->resize(980, 680);
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));

            driveIdentityCreationDialog(window.get());
            QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile",
                                              Qt::DirectConnection));
            QCOMPARE(window->property("notepadsafeTestIdentityDriverResult").toString(),
                     QStringLiteral("complete"));
            EditorTab *created = currentEditor(window.get());
            QVERIFY(created && created->isSecureDocument());
            QVERIFY(created->filePath().isEmpty());
            createdDocumentId = created->captureSessionTab().documentId;
            QVERIFY(!createdDocumentId.isEmpty());
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
        QSignalSpy restoreCompleted(restarted.get(), &MainWindow::sessionRestoreCompleted);
        QVERIFY(restoreCompleted.isValid());
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
        QTRY_COMPARE_WITH_TIMEOUT(restoreCompleted.count(), 1, 2000);
        promptWatcher.stop();
        QVERIFY(!unexpectedPrompt);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        EditorTab *restored = currentEditor(restarted.get());
        QCOMPARE(QFileInfo(restored->autosavePath()).absoluteFilePath(),
                 QFileInfo(encryptedDraftPath).absoluteFilePath());
        const SessionTab restoredState = restored->captureSessionTab();
        QVERIFY(restoredState.secureDocument);
        QCOMPARE(restoredState.documentId, createdDocumentId);
        // UI restoration is not a replacement durable publication. A fresh
        // reader must still discover this exact encrypted draft after finish.
        SessionManager freshSession(dataDirectory.path());
        const SessionData persisted = freshSession.loadSession();
        QCOMPARE(persisted.tabs.size(), qsizetype(1));
        QCOMPARE(persisted.activeTab, 0);
        QVERIFY(persisted.tabs.first().secureDocument);
        QCOMPARE(persisted.tabs.first().documentId, restoredState.documentId);
        QCOMPARE(persisted.tabs.first().autosavePath, restoredState.autosavePath);
        QCOMPARE(restoreCompleted.count(), 1);
        QVERIFY(restarted->grab().save(QDir::current().filePath(
            QStringLiteral("gui-secure-draft-restart.png"))));

        restarted->close();
        restarted.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void laterSecureRestoreKeepsOrdinaryCurrentDocument_data()
    {
        QTest::addColumn<bool>("missingSecure");
        QTest::addColumn<bool>("enterAccounts");
        QTest::newRow("success") << false << false;
        QTest::newRow("missing-secure") << true << false;
        QTest::newRow("enter-accounts") << false << true;
    }

    void laterSecureRestoreKeepsOrdinaryCurrentDocument()
    {
        QFETCH(bool, missingSecure);
        QFETCH(bool, enterAccounts);
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        const bool hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousProfile = qgetenv("NOTEPADSAFE_DATADIR");
        const auto restoreProfile = qScopeGuard([&] {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", previousProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        QVERIFY(qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8()));
        const auto readBytes = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const auto hash = [](const QByteArray &bytes) {
            return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        };
        const QString ordinaryPath = dataDirectory.filePath(QStringLiteral("preferred.txt"));
        const QByteArray ordinaryBody("ordinary active before later secure restore\n");
        const QString secondPath = dataDirectory.filePath(QStringLiteral("background.txt"));
        const QByteArray secondBody("second ordinary document stays in the background\n");
        const QString secureBody = QStringLiteral("SECURE_LATER_RESTORE_CANARY\n真实安全正文\n");
        QVERIFY(writeFixture(ordinaryPath, ordinaryBody));
        QVERIFY(writeFixture(secondPath, secondBody));
        QCOMPARE(readBytes(ordinaryPath), ordinaryBody);
        QCOMPARE(readBytes(secondPath), secondBody);
        const QByteArray ordinaryHash = hash(ordinaryBody);
        const QByteArray secondHash = hash(secondBody);
        QString securePath, secureDocumentId;
        {
            auto seed = std::make_unique<MainWindow>();
            seed->resize(980, 680);
            seed->show();
            seed->raise();
            seed->activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(seed.get()));
            seed->openFilesFromExternal({ordinaryPath});
            auto *ordinary = currentEditor(seed.get());
            QVERIFY(ordinary && !ordinary->isSecureDocument());
            QCOMPARE(ordinary->filePath(), ordinaryPath);
            QCOMPARE(ordinary->editor()->text().toUtf8(), ordinaryBody);
            seed->openFilesFromExternal({secondPath});
            auto *second = currentEditor(seed.get());
            QVERIFY(second && second != ordinary && !second->isSecureDocument());
            QCOMPARE(second->filePath(), secondPath);
            QCOMPARE(second->editor()->text().toUtf8(), secondBody);
            driveIdentityCreationDialog(seed.get());
            auto *newAction = seed->findChild<QAction *>(QStringLiteral("file.new"));
            QVERIFY(newAction && newAction->isEnabled());
            newAction->trigger();
            QCOMPARE(seed->property("notepadsafeTestIdentityDriverResult").toString(),
                     QStringLiteral("complete"));
            auto *secure = currentEditor(seed.get());
            QVERIFY(secure && secure->isSecureDocument());
            secureDocumentId = secure->captureSessionTab().documentId;
            QVERIFY(!secureDocumentId.isEmpty());
            secure->editor()->setText(secureBody);
            QTRY_VERIFY_WITH_TIMEOUT(!secure->autosavePath().isEmpty()
                                         && QFileInfo::exists(secure->autosavePath()), 5000);
            securePath = secure->autosavePath();
            auto *tabs = seed->findChild<QTabWidget *>();
            QVERIFY(tabs);
            QCOMPARE(tabs->count(), 3);
            QCOMPARE(tabs->widget(0), ordinary);
            QCOMPARE(tabs->widget(1), second);
            QCOMPARE(tabs->widget(2), secure);
            tabs->setCurrentWidget(ordinary);
            // Normal product close checkpoints Secure content and publishes the
            // real session in visible order, with the ordinary item active.
            QVERIFY(seed->close());
            QVERIFY(!seed->isVisible());
        }
        SessionManager freshSession(dataDirectory.path());
        const SessionData before = freshSession.loadSession();
        QCOMPARE(before.tabs.size(), qsizetype(3));
        QCOMPARE(before.activeTab, 0);
        QCOMPARE(before.tabs[0].filePath, ordinaryPath);
        QVERIFY(!before.tabs[0].secureDocument && !before.tabs[0].modified);
        QCOMPARE(before.tabs[1].filePath, secondPath);
        QVERIFY(!before.tabs[1].secureDocument && !before.tabs[1].modified);
        QVERIFY(before.tabs[2].secureDocument);
        QCOMPARE(before.tabs[2].documentId, secureDocumentId);
        QCOMPARE(before.tabs[2].autosavePath, securePath);
        const QByteArray ciphertext = readBytes(securePath);
        QCOMPARE(ciphertext.left(8).toHex().toUpper(), QByteArray("03D9A29A67FB4BB5"));
        QVERIFY(!ciphertext.contains(secureBody.toUtf8()));
        const QByteArray secureHash = hash(ciphertext);
        QCOMPARE(hash(readBytes(ordinaryPath)), ordinaryHash);
        QCOMPARE(hash(readBytes(secondPath)), secondHash);
        QVERIFY(QFileInfo::exists(dataDirectory.filePath(QStringLiteral("security-session-v1.bin"))));
        const QString backupPath = dataDirectory.filePath(QStringLiteral("retained-secure-backup.nsafe"));
        if (missingSecure) {
            // Move only this fixture's sealed file; retain its bytes for the
            // failure cut, rather than deleting a draft or rewriting metadata.
            QVERIFY(QFileInfo(securePath).canonicalFilePath().startsWith(
                QDir(dataDirectory.path()).canonicalPath() + QLatin1Char('/')));
            QVERIFY(!QFileInfo::exists(backupPath));
            QVERIFY(QFile::rename(securePath, backupPath));
            QVERIFY(!QFileInfo::exists(securePath));
            QCOMPARE(hash(readBytes(backupPath)), secureHash);
        }

        auto restarted = std::make_unique<MainWindow>();
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        QVERIFY(completed.isValid());
        auto *tabs = restarted->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QPointer<EditorTab> ordinary;
        QUuid ordinaryId;
        quint64 ordinaryRevision = 0;
        int cuts = 0;
        QString cutError;
        bool unexpectedPrompt = false;
        bool enteringAccounts = false, accountsEntered = false;
        int workspaceDepartures = 0;
        auto *workspace = restarted->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        QPointer<QWidget> accountsPage;
        QPointer<VaultDock> accountsVault;
        QPointer<QLineEdit> accountsFilter;
        int documentFocusTransfers = 0;
        const QString filterPrefix = QStringLiteral("recovery-intent-");
        const QString filterSuffix = QStringLiteral("still-here");
        const QString accountsPath = dataDirectory.filePath(QStringLiteral("accounts.kdbx"));
        QByteArray accountsHash;
        QTimer promptWatcher;
        StartupCoordinator startup;
        QObject observation; // Revoke all callbacks before their captured locals.
        connect(&promptWatcher, &QTimer::timeout, &observation, [&] {
            if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                // Real account creation may display its normal crypto progress
                // window. This is not an unexpected credential/identity prompt.
                if (enterAccounts && enteringAccounts && qobject_cast<QProgressDialog *>(modal)
                    && modal->objectName() == QStringLiteral("securityOperationProgress")) return;
                unexpectedPrompt = true;
                modal->reject();
            }
        });
        connect(restarted->statusBar(), &QStatusBar::messageChanged, &observation,
                [&](const QString &message) {
            if (message != QStringLiteral("正在恢复会话… 1/3 个标签页")) return;
            if (++cuts != 1) return;
            ordinary = currentEditor(restarted.get());
            if (completed.count() != 0 || tabs->count() != 1 || !ordinary
                || ordinary->isSecureDocument() || ordinary->filePath() != ordinaryPath
                || ordinary->editor()->text().toUtf8() != ordinaryBody || ordinary->isModified()) {
                cutError = QStringLiteral("ordinary item was not fully current before later Secure restore");
                return;
            }
            ordinaryId = ordinary->documentState().id;
            ordinaryRevision = ordinary->contentRevision();
            if (!enterAccounts) return;
            auto *accountsAction = restarted->findChild<QAction *>(QStringLiteral("tools.accounts"));
            if (!workspace || !accountsAction || !accountsAction->isEnabled()) {
                cutError = QStringLiteral("real Accounts command is not available at the 1/3 cut");
                return;
            }
            enteringAccounts = true;
            const auto finishEnteringAccounts = qScopeGuard([&] { enteringAccounts = false; });
            accountsAction->trigger(); // Actual synchronous product identity/account flow, not a timing probe.
            accountsPage = workspace->currentWidget();
            accountsVault = restarted->findChild<VaultDock *>(QStringLiteral("VaultDock"));
            if (completed.count() != 0 || tabs->count() != 1 || !accountsPage
                || accountsPage->objectName() != QStringLiteral("AccountsWorkspace")
                || !accountsPage->isVisible() || !accountsVault || !accountsVault->isVisible()
                || !accountsVault->isSecureStoreOpen() || !QFileInfo(accountsPath).isFile()
                || QFileInfo(accountsVault->secureStorePath()).absoluteFilePath()
                       != QFileInfo(accountsPath).absoluteFilePath()) {
                cutError = QStringLiteral("Accounts was not really open while later recovery items remained pending");
                return;
            }
            // An empty store intentionally has no search input. Create one real
            // account through its visible controls before freezing focus/hash.
            auto *create = accountsVault->findChild<QPushButton *>(QStringLiteral("accountsEmptyCreateButton"));
            if (!create || !create->isVisible() || !create->isEnabled()) {
                cutError = QStringLiteral("empty Accounts page has no usable Create button");
                return;
            }
            QTest::mouseClick(create, Qt::LeftButton);
            auto *title = accountsVault->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
            auto *username = accountsVault->findChild<QLineEdit *>(QStringLiteral("vaultUsernameEdit"));
            auto *save = accountsVault->findChild<QPushButton *>(QStringLiteral("vaultSaveButton"));
            if (!title || !title->isVisible() || !title->isEnabled() || title->isReadOnly()
                || !username || !username->isVisible() || !username->isEnabled() || username->isReadOnly()
                || !save || !save->isVisible() || !save->isEnabled()) {
                cutError = QStringLiteral("real Create did not expose editable account fields and Save");
                return;
            }
            const QString accountTitle = filterPrefix + filterSuffix;
            const QString accountUsername = QStringLiteral("recovery-fixture@example.test");
            QTest::mouseClick(title, Qt::LeftButton);
            QTest::keyClick(title, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(title, accountTitle);
            QTest::mouseClick(username, Qt::LeftButton);
            QTest::keyClick(username, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(username, accountUsername);
            QTest::mouseClick(save, Qt::LeftButton);
            auto *accountTree = accountsVault->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
            auto *savedAccount = findTreeItem(accountTree, accountTitle);
            if (completed.count() != 0 || tabs->count() != 1 || workspace->currentWidget() != accountsPage
                || !accountsVault->isSecureStoreOpen() || accountsVault->canSaveActiveEntry()
                || !savedAccount || accountTree->currentItem() != savedAccount
                || savedAccount->text(0) != accountTitle || title->text() != accountTitle
                || username->text() != accountUsername || save->isVisible()) {
                cutError = QStringLiteral("real account Save did not finish/select its entry before recovery resumed");
                return;
            }
            const QByteArray accountsCiphertext = readBytes(accountsPath);
            if (accountsCiphertext.left(8).toHex().toUpper() != QByteArray("03D9A29A67FB4BB5")) {
                cutError = QStringLiteral("real Accounts workflow did not create this fixture's KDBX store");
                return;
            }
            accountsHash = hash(accountsCiphertext);
            accountsEntered = true;
            connect(workspace, &QStackedWidget::currentChanged, &observation, [&](int) {
                if (!accountsPage || workspace->currentWidget() != accountsPage)
                    ++workspaceDepartures;
            }, Qt::DirectConnection);
            accountsFilter = accountsVault->findChild<QLineEdit *>(QStringLiteral("vaultFilterEdit"));
            if (!accountsFilter || !accountsFilter->isVisible() || !accountsFilter->isEnabled()
                || !accountsFilter->text().isEmpty()) {
                cutError = QStringLiteral("Accounts filter is not a usable empty input at the recovery cut");
                return;
            }
            QTest::mouseClick(accountsFilter, Qt::LeftButton);
            if (QApplication::focusWidget() != accountsFilter) {
                cutError = QStringLiteral("real Accounts filter click did not establish actual keyboard focus");
                return;
            }
            QTest::keyClicks(QApplication::focusWidget(), filterPrefix);
            if (accountsFilter->text() != filterPrefix || QApplication::focusWidget() != accountsFilter) {
                cutError = QStringLiteral("prefix did not reach the focused Accounts filter");
                return;
            }
            connect(qApp, &QApplication::focusChanged, &observation, [&](QWidget *, QWidget *now) {
                if (!now) return;
                auto *progress = qobject_cast<QProgressDialog *>(now->window());
                if (progress && progress->objectName() == QStringLiteral("securityOperationProgress")) return;
                for (int i = 0; i < tabs->count(); ++i) {
                    auto *document = qobject_cast<EditorTab *>(tabs->widget(i));
                    if (document && (now == document || document->isAncestorOf(now))) {
                        ++documentFocusTransfers;
                        break;
                    }
                }
            }, Qt::DirectConnection);
            qInfo() << "SECURE_RESTORE_ACCOUNTS_ENTERED_GUARDS_OK" << "completed" << completed.count()
                    << "deliveredDocuments" << tabs->count() << "actualFilterFocus" << (QApplication::focusWidget() == accountsFilter);
        }, Qt::DirectConnection);
        startup.observe(restarted.get(), {});
        promptWatcher.start(20);
        restarted->resize(980, 680);
        restarted->show();
        restarted->raise();
        restarted->activateWindow();
        startup.releasePostPaintWork();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        promptWatcher.stop();
        QCOMPARE(cuts, 1);
        QVERIFY2(cutError.isEmpty(), qPrintable(cutError));
        QVERIFY(ordinary && !ordinaryId.isNull());
        QCOMPARE(tabs->count(), missingSecure ? 2 : 3);
        QCOMPARE(tabs->widget(0), ordinary.data());
        auto *second = qobject_cast<EditorTab *>(tabs->widget(1));
        QVERIFY(second && !second->isSecureDocument() && !second->isModified());
        QVERIFY(!second->isReadOnlyByPolicy());
        QCOMPARE(second->filePath(), secondPath);
        QCOMPARE(second->editor()->text().toUtf8(), secondBody);
        QCOMPARE(second->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
        QVERIFY(second->documentState().capabilities.testFlag(DocumentCapability::Read));
        QVERIFY(second->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QVERIFY(!second->documentState().id.isNull());
        QVERIFY(second->documentState().id != ordinaryId);
        if (missingSecure) {
            QVERIFY(!QFileInfo::exists(securePath));
            QCOMPARE(hash(readBytes(backupPath)), secureHash);
        } else {
            auto *secure = qobject_cast<EditorTab *>(tabs->widget(2));
            QVERIFY(secure && secure->isSecureDocument());
            QCOMPARE(secure->editor()->text(), secureBody);
            QCOMPARE(secure->documentState().securityClass, DocumentSecurityClass::SecureDocument);
            QVERIFY(secure->documentState().capabilities.testFlag(DocumentCapability::Read));
            QVERIFY(secure->documentState().capabilities.testFlag(DocumentCapability::Edit));
            QCOMPARE(secure->captureSessionTab().documentId, secureDocumentId);
            QCOMPARE(secure->autosavePath(), securePath);
            QVERIFY(!secure->documentState().id.isNull());
            QVERIFY(secure->documentState().id != ordinaryId);
            QVERIFY(secure->documentState().id != second->documentState().id);
            QCOMPARE(hash(readBytes(securePath)), secureHash);
        }
        QCOMPARE(ordinary->documentState().id, ordinaryId);
        QCOMPARE(ordinary->contentRevision(), ordinaryRevision);
        QCOMPARE(ordinary->editor()->text().toUtf8(), ordinaryBody);
        QVERIFY(!ordinary->isModified() && !ordinary->isSecureDocument());
        QVERIFY(!ordinary->isReadOnlyByPolicy());
        QCOMPARE(ordinary->documentState().securityClass, DocumentSecurityClass::OrdinaryDocument);
        QVERIFY(ordinary->documentState().capabilities.testFlag(DocumentCapability::Read));
        QVERIFY(ordinary->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QCOMPARE(hash(readBytes(ordinaryPath)), ordinaryHash);
        QCOMPARE(hash(readBytes(secondPath)), secondHash);
        const SessionData after = freshSession.loadSession();
        QCOMPARE(after.tabs.size(), qsizetype(3));
        QCOMPARE(after.activeTab, 0);
        QCOMPARE(after.tabs[0].filePath, ordinaryPath);
        QVERIFY(!after.tabs[0].secureDocument);
        QCOMPARE(after.tabs[1].filePath, secondPath);
        QVERIFY(!after.tabs[1].secureDocument);
        QVERIFY(after.tabs[2].secureDocument);
        QCOMPARE(after.tabs[2].documentId, secureDocumentId);
        QCOMPARE(after.tabs[2].autosavePath, securePath);
        QVERIFY(!unexpectedPrompt);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        if (enterAccounts) {
            QVERIFY(accountsEntered);
            QVERIFY(QFileInfo(accountsPath).isFile());
            QCOMPARE(hash(readBytes(accountsPath)), accountsHash);
        }
        qInfo() << "SECURE_LATER_RESTORE_DATA_GUARDS_OK" << "missingSecure" << missingSecure
                << "enterAccounts" << enterAccounts << "completed" << completed.count()
                << "visibleDocuments" << tabs->count() << "workspaceDepartures" << workspaceDepartures
                << "accountsOpen" << (accountsVault && accountsVault->isSecureStoreOpen())
                << "documentFocusTransfers" << documentFocusTransfers;
        if (enterAccounts) {
            QVERIFY(workspace && accountsPage && accountsVault);
            QCOMPARE(workspace->currentWidget(), accountsPage.data());
            QVERIFY(accountsPage->isVisible() && accountsVault->isVisible());
            QVERIFY(accountsVault->isSecureStoreOpen());
            QCOMPARE(QFileInfo(accountsVault->secureStorePath()).absoluteFilePath(),
                     QFileInfo(accountsPath).absoluteFilePath());
            QCOMPARE(workspaceDepartures, 0); // No final reopen can hide an intermediate workspace takeover.
            QVERIFY(accountsFilter);
            QCOMPARE(accountsFilter->text(), filterPrefix);
            QCOMPARE(QApplication::focusWidget(), accountsFilter.data());
            const QList<QPointer<EditorTab>> documents{ordinary, second,
                qobject_cast<EditorTab *>(tabs->widget(2))};
            QList<QUuid> ids;
            QList<quint64> revisions;
            QList<bool> dirty;
            for (const auto &document : documents) {
                QVERIFY(document);
                ids.append(document->documentState().id);
                revisions.append(document->contentRevision());
                dirty.append(document->isModified());
            }
            // Deliver to the ACTUAL focus owner; never re-click or setFocus here.
            QTest::keyClicks(QApplication::focusWidget(), filterSuffix);
            QCOMPARE(QApplication::focusWidget(), accountsFilter.data());
            QCOMPARE(accountsFilter->text(), filterPrefix + filterSuffix);
            for (int i = 0; i < documents.size(); ++i) {
                QVERIFY(documents[i]);
                QCOMPARE(documents[i]->documentState().id, ids[i]);
                QCOMPARE(documents[i]->contentRevision(), revisions[i]);
                QCOMPARE(documents[i]->isModified(), dirty[i]);
            }
            QCOMPARE(documents[0]->editor()->text().toUtf8(), ordinaryBody);
            QCOMPARE(documents[1]->editor()->text().toUtf8(), secondBody);
            QCOMPARE(documents[2]->editor()->text(), secureBody);
            QCOMPARE(hash(readBytes(ordinaryPath)), ordinaryHash);
            QCOMPARE(hash(readBytes(secondPath)), secondHash);
            QCOMPARE(hash(readBytes(securePath)), secureHash);
            QCOMPARE(hash(readBytes(accountsPath)), accountsHash);
            QCOMPARE(documentFocusTransfers, 0);
            const QString screenshot = QDir::current().filePath(
                QStringLiteral("3a-accounts-focus-text.png"));
            QVERIFY2(restarted->grab().save(screenshot), qPrintable(screenshot));
        } else {
            QCOMPARE(tabs->currentWidget(), ordinary.data());
        }
        // Destruction is cleanup only; do not republish after the frozen cut.
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

        driveIdentityCreationDialog(window.get());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));
        QCOMPARE(window->property("notepadsafeTestIdentityDriverResult").toString(),
                 QStringLiteral("complete"));

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
        auto *findDialog = window->findChild<FindOverlay *>();
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
        // Groups exist from the start; wait until every file has finished.
        auto *cancelSearch = findDialog->findChild<QPushButton *>(
            QStringLiteral("cancelSearchButton"));
        QVERIFY(cancelSearch);
        QTRY_VERIFY_WITH_TIMEOUT(!cancelSearch->isVisible(), 5000);

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

    void externalOpenPreservesAccountsAndSecuritySession()
    {
        QTemporaryDir dataDirectory;
        QVERIFY(dataDirectory.isValid());
        qputenv("NOTEPADSAFE_DATADIR", dataDirectory.path().toUtf8());

        AppComposition composition;
        QVERIFY(composition.inferenceProviders().registerProvider(
            {QStringLiteral("test.remote"), QStringLiteral("1"),
             QStringLiteral("test"), QStringLiteral("test-only"), {},
             QStringLiteral("remote"), QStringLiteral("network"), true},
            []() { return std::unique_ptr<IInferenceProvider>{}; }));
        auto window = std::make_unique<MainWindow>(nullptr, &composition);
        window->resize(1180, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));

        driveIdentityCreationDialog(window.get());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile",
                                          Qt::DirectConnection));
        QCOMPARE(window->property("notepadsafeTestIdentityDriverResult").toString(),
                 QStringLiteral("complete"));
        auto *secureTab = currentEditor(window.get());
        auto *tabs = window->findChild<QTabWidget *>();
        auto *stack = window->findChild<QStackedWidget *>(
            QStringLiteral("WorkspaceStack"));
        QVERIFY(secureTab && secureTab->isSecureDocument() && tabs && stack);
        secureTab->editor()->setText(QStringLiteral("secure session marker"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(secureTab->autosavePath()), 5000);
        const QString secureText = secureTab->editor()->text();
        const QString capsule = dataDirectory.filePath(
            QStringLiteral("security-session-v1.bin"));
        QVERIFY(QFileInfo::exists(capsule));

        auto *tailToggle = window->findChild<QAction *>(QStringLiteral("view.tail.toggle"));
        auto *tailResume = window->findChild<QAction *>(QStringLiteral("view.tail.resume"));
        QVERIFY(tailToggle && tailResume);
        QVERIFY(!tailToggle->isEnabled() && !tailToggle->isChecked() && !tailResume->isEnabled());
        tailToggle->trigger();
        tailResume->trigger();
        QCOMPARE(secureTab->editor()->text(), secureText);
        QVERIFY(!tailToggle->isChecked());

        auto *accountsAction = window->findChild<QAction *>(
            QStringLiteral("tools.accounts"));
        QVERIFY(accountsAction);
        accountsAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(stack->currentWidget()->objectName()
                                     == QStringLiteral("AccountsWorkspace"), 4000);
        auto *vault = window->findChild<VaultDock *>(QStringLiteral("VaultDock"));
        auto *filter = vault ? vault->findChild<QLineEdit *>(
            QStringLiteral("vaultFilterEdit")) : nullptr;
        auto *tree = vault ? vault->findChild<QTreeWidget *>(
            QStringLiteral("vaultEntryTree")) : nullptr;
        QVERIFY(vault && vault->isSecureStoreOpen() && filter && tree);
        filter->setText(QStringLiteral("preserve-filter"));
        if (tree->topLevelItemCount() > 0)
            tree->setCurrentItem(tree->topLevelItem(0));
        QWidget *const accountsPage = stack->currentWidget();
        QTreeWidgetItem *const selectedAccountItem = tree->currentItem();
        const QString vaultPath = vault->secureStorePath();
        const int tabCountBefore = tabs->count();
        const int heavyCallsBefore = composition.heavyFactoryCalls();
        QCOMPARE(composition.inferenceProviders().instantiatedCount(), 0);

        QVERIFY(!tailToggle->isEnabled() && !tailToggle->isChecked() && !tailResume->isEnabled());
        tailToggle->trigger();
        tailResume->trigger();
        QCOMPARE(stack->currentWidget(), accountsPage);
        QCOMPARE(vault->secureStorePath(), vaultPath);
        QCOMPARE(secureTab->editor()->text(), secureText);
        QCOMPARE(composition.heavyFactoryCalls(), heavyCallsBefore);
        QVERIFY(!tailToggle->isChecked());

        const QString plainPath = dataDirectory.filePath(
            QStringLiteral("background-external.txt"));
        QVERIFY(writeFixture(plainPath, QByteArray("ordinary external text\n")));
        window->openFilesFromExternal({plainPath});
        QCOMPARE(stack->currentWidget(), accountsPage);
        QVERIFY(vault->isSecureStoreOpen());
        QCOMPARE(vault->secureStorePath(), vaultPath);
        QCOMPARE(filter->text(), QStringLiteral("preserve-filter"));
        QCOMPARE(tree->currentItem(), selectedAccountItem);
        QCOMPARE(tabs->count(), tabCountBefore + 1);
        QVERIFY(!secureTab->isSecureDocumentMasked());
        QCOMPARE(secureTab->editor()->text(), secureText);
        QVERIFY(QFileInfo::exists(capsule));
        QCOMPARE(composition.heavyFactoryCalls(), heavyCallsBefore);
        QCOMPARE(composition.inferenceProviders().instantiatedCount(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        const QString rejectedSecurePath = dataDirectory.filePath(
            QStringLiteral("external-must-not-unlock.nsafe"));
        QVERIFY(writeFixture(rejectedSecurePath, QByteArray("not inspected")));
        window->openFilesFromExternal({rejectedSecurePath});
        QCOMPARE(stack->currentWidget(), accountsPage);
        QVERIFY(vault->isSecureStoreOpen());
        QCOMPARE(vault->secureStorePath(), vaultPath);
        QCOMPARE(filter->text(), QStringLiteral("preserve-filter"));
        QCOMPARE(tree->currentItem(), selectedAccountItem);
        QCOMPARE(tabs->count(), tabCountBefore + 1);
        QCOMPARE(window->statusBar()->currentMessage(),
                 QStringLiteral("%1 未打开。为保护安全文件，外部请求不会解锁；请在窗口内用文件→打开。")
                     .arg(QFileInfo(rejectedSecurePath).fileName()));
        QVERIFY(!secureTab->isSecureDocumentMasked());
        QCOMPARE(secureTab->editor()->text(), secureText);
        QCOMPARE(composition.heavyFactoryCalls(), heavyCallsBefore);
        QCOMPARE(composition.inferenceProviders().instantiatedCount(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);

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

        driveIdentityCreationDialog(window.get());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "newFile", Qt::DirectConnection));
        QCOMPARE(window->property("notepadsafeTestIdentityDriverResult").toString(),
                 QStringLiteral("complete"));

        EditorTab *tab = currentEditor(window.get());
        QVERIFY(tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked());
        tab->editor()->setText(QStringLiteral("saved clipboard baseline"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(tab->autosavePath())
                                     && !tab->isModified(), 10000);

        // Force the primary KDBX overwrite guard to reject the next save while
        // leaving a valid baseline file. System lock must still mask plaintext
        // and restore the dirty body from the DPAPI recovery point.
        QFile backing(tab->autosavePath());
        QVERIFY(backing.open(QIODevice::ReadWrite));
        QVERIFY(backing.setFileTime(QDateTime::currentDateTimeUtc().addSecs(5),
                                    QFileDevice::FileModificationTime));
        backing.close();
        const QString secret = QStringLiteral("CLIPBOARD_SECRET_74f6d18c_UNSAVED");
        tab->editor()->setText(secret);
        QVERIFY(tab->isModified());
        tab->editor()->selectAll();
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(), secret);
        QTest::keyClick(tab->editor(), Qt::Key_Right);
        QVERIFY(!tab->hasSelectionCommand());
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), secret);

#ifdef Q_OS_WIN
        bool unexpectedModalSeen = false;
        QString unexpectedModalTitle;
        QTimer::singleShot(15000, window.get(), [&]() {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            unexpectedModalSeen = true;
            unexpectedModalTitle = modal->windowTitle();
            modal->close();
        });
        // Exercise the native Windows workstation-lock route: copied secure
        // note text must disappear together with the editor plaintext.
        ::SendMessageW(reinterpret_cast<HWND>(window->winId()), WM_WTSSESSION_CHANGE,
                       WTS_SESSION_LOCK, 0);
        QVERIFY2(!unexpectedModalSeen,
                 qPrintable(QStringLiteral("system-lock path opened an unexpected modal: %1")
                                .arg(unexpectedModalTitle)));
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
        QVERIFY(tab->isModified());

        // Clipboard cleanup is fingerprint-conditional. Never erase content
        // another application/user copied after NotepadSafe's secure copy.
        tab->editor()->selectAll();
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(), secret);
        const QString externalReplacement = QStringLiteral("external clipboard value");
        QApplication::clipboard()->setText(externalReplacement);
        // An open command palette lists the document by title; locking must
        // close it so the title does not stay on screen.
        emit window->titleBar()->commandPaletteRequested();
        auto *palette = window->findChild<QFrame *>(QStringLiteral("commandPalette"));
        QTRY_VERIFY(palette && palette->isVisible());
        QAction *lockAction = actionWithText(window.get(), QStringLiteral("立即锁定"));
        QVERIFY(lockAction);
        lockAction->trigger();
        QVERIFY(!palette->isVisible());
        QVERIFY2(tab->isSecureDocumentMasked(),
                 qPrintable(QStringLiteral("manual lock did not mask; enabled=%1 status=%2")
                                .arg(lockAction->isEnabled())
                                .arg(window->statusBar()->currentMessage())));
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
