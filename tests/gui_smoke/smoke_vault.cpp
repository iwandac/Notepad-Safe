#include "IVaultBackend.h"
#include "VaultBackendFactory.h"
#include "VaultDock.h"

#include <QClipboard>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUrl>
#include <QtTest>

namespace {

QString textFrom(const SecureBuffer &value)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(value.data()), value.size());
}

QTreeWidgetItem *rootNamed(QTreeWidget *tree, const QString &name)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        if (tree->topLevelItem(index)->text(0) == name)
            return tree->topLevelItem(index);
    }
    return nullptr;
}

void acceptNextQuestion()
{
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *message = qobject_cast<QMessageBox *>(widget);
            if (message && message->isVisible()) {
                auto *yes = message->button(QMessageBox::Yes);
                if (yes)
                    QTest::mouseClick(yes, Qt::LeftButton);
                return;
            }
        }
    });
}

class UrlCapture final : public QObject {
    Q_OBJECT
public slots:
    void handle(const QUrl &url) { opened = url; }
public:
    QUrl opened;
};

struct Record {
    QUuid id;
    QString title;
    QString username;
    QString url;
    QStringList tags;
    QString password;
    QString notes;
    QString totp;
    bool recycled = false;
};

class MemoryVaultBackend final : public IVaultBackend {
public:
    VaultError create(const QString &, const SecureBuffer &password, const QString &keyFile,
                      const QVariantMap &publicCustomData) override
    {
        storedPublicCustomData = publicCustomData;
        return unlock({}, password, keyFile);
    }

    VaultError readPublicCustomData(const QString &, QVariantMap *publicCustomData) const override
    {
        if (!publicCustomData)
            return {VaultErrorCode::InternalError, QStringLiteral("missing output"), {}};
        *publicCustomData = storedPublicCustomData;
        return VaultError::success();
    }

    VaultError unlock(const QString &, const SecureBuffer &password, const QString &) override
    {
        unlocked = password.equals("master", 6);
        return unlocked ? VaultError::success()
                        : VaultError{VaultErrorCode::InvalidCredentials,
                                     QStringLiteral("bad password"),
                                     QStringLiteral("test.invalid")};
    }

    void lock() noexcept override { unlocked = false; }

    QList<VaultEntrySummary> listEntries(VaultError *error) const override
    {
        if (!unlocked) {
            if (error)
                *error = {VaultErrorCode::Locked, QStringLiteral("locked"), {}};
            return {};
        }
        QList<VaultEntrySummary> result;
        for (const auto &record : records) {
            if (record.recycled)
                continue;
            result.push_back({record.id, record.title, record.username, record.url, record.tags,
                              record.tags.contains(VaultSecureNoteTag), !record.password.isEmpty()});
        }
        if (error)
            *error = VaultError::success();
        return result;
    }

    QList<VaultEntrySummary> listRecycledEntries(VaultError *error) const override
    {
        if (!unlocked) {
            if (error)
                *error = {VaultErrorCode::Locked, QStringLiteral("locked"), {}};
            return {};
        }
        QList<VaultEntrySummary> result;
        for (const auto &record : records) {
            if (!record.recycled)
                continue;
            result.push_back({record.id, record.title, record.username, record.url, record.tags,
                              record.tags.contains(VaultSecureNoteTag), !record.password.isEmpty()});
        }
        if (error)
            *error = VaultError::success();
        return result;
    }

    QSet<QUuid> searchEntryIds(const QString &query, bool includeRecycled,
                               VaultError *error) const override
    {
        QSet<QUuid> result;
        for (const auto &record : records) {
            if (record.recycled && !includeRecycled)
                continue;
            const bool matches = record.title.contains(query, Qt::CaseInsensitive)
                                 || record.username.contains(query, Qt::CaseInsensitive)
                                 || record.url.contains(query, Qt::CaseInsensitive)
                                 || record.notes.contains(query, Qt::CaseInsensitive)
                                 || record.tags.join(QLatin1Char(',')).contains(
                                     query, Qt::CaseInsensitive);
            if (matches)
                result.insert(record.id);
        }
        if (error)
            *error = VaultError::success();
        return result;
    }

    std::unique_ptr<VaultEntryData> readEntry(const QUuid &id, VaultError *error) const override
    {
        for (const auto &record : records) {
            if (record.id != id)
                continue;
            auto data = std::make_unique<VaultEntryData>();
            data->id = record.id;
            data->title = record.title;
            data->username = record.username;
            data->url = record.url;
            data->tags = record.tags;
            const QByteArray password = record.password.toUtf8();
            data->passwordUtf8.assign(password.constData(), password.size());
            const QByteArray notes = record.notes.toUtf8();
            data->notesUtf8.assign(notes.constData(), notes.size());
            const QByteArray totp = record.totp.toUtf8();
            data->totpSeedUtf8.assign(totp.constData(), totp.size());
            if (error)
                *error = VaultError::success();
            return data;
        }
        if (error)
            *error = {VaultErrorCode::InternalError, QStringLiteral("missing"), {}};
        return {};
    }

    VaultError currentTotp(const QUuid &id, VaultTotpSnapshot *snapshot) const override
    {
        if (!snapshot)
            return {VaultErrorCode::InternalError, QStringLiteral("missing output"), {}};
        for (const auto &record : records) {
            if (record.id == id) {
                *snapshot = record.totp.isEmpty()
                    ? VaultTotpSnapshot{}
                    : VaultTotpSnapshot{QStringLiteral("123456"), 18};
                return VaultError::success();
            }
        }
        return {VaultErrorCode::InternalError, QStringLiteral("missing"), {}};
    }
    VaultError generatePassword(int length, SecureBuffer *passwordUtf8) const override
    {
        const QByteArray base("Aa1!Bb2@Cc3#Dd4$Ee5%Ff6&");
        QByteArray generated;
        while (generated.size() < length)
            generated.append(base);
        generated.truncate(length);
        passwordUtf8->assign(generated.constData(), generated.size());
        return VaultError::success();
    }
    VaultError assessPassword(const SecureBuffer &passwordUtf8,
                              VaultPasswordAssessment *assessment) const override
    {
        if (!assessment)
            return {VaultErrorCode::InternalError, QStringLiteral("missing output"), {}};
        *assessment = passwordUtf8.size() >= 20
            ? VaultPasswordAssessment{VaultPasswordQuality::Excellent, 120.0}
            : VaultPasswordAssessment{VaultPasswordQuality::Weak, 45.0};
        return VaultError::success();
    }

    VaultError upsertEntry(VaultEntryData &&entry) override
    {
        Record replacement{entry.id, entry.title, entry.username, entry.url, entry.tags,
                           textFrom(entry.passwordUtf8), textFrom(entry.notesUtf8),
                           textFrom(entry.totpSeedUtf8)};
        for (auto &record : records) {
            if (record.id == replacement.id) {
                record = std::move(replacement);
                return VaultError::success();
            }
        }
        records.push_back(std::move(replacement));
        return VaultError::success();
    }

    VaultError recycleEntry(const QUuid &id) override
    {
        for (auto &record : records) {
            if (record.id == id && !record.recycled) {
                record.recycled = true;
                return VaultError::success();
            }
        }
        return {VaultErrorCode::InternalError, QStringLiteral("missing"), {}};
    }

    VaultError restoreEntry(const QUuid &id) override
    {
        for (auto &record : records) {
            if (record.id == id && record.recycled) {
                record.recycled = false;
                return VaultError::success();
            }
        }
        return {VaultErrorCode::InternalError, QStringLiteral("missing"), {}};
    }

    VaultError removeEntry(const QUuid &id) override
    {
        for (qsizetype i = 0; i < records.size(); ++i) {
            if (records[i].id == id && records[i].recycled) {
                records.removeAt(i);
                return VaultError::success();
            }
        }
        return {VaultErrorCode::InternalError, QStringLiteral("missing"), {}};
    }

    VaultError save() override
    {
        ++saveCount;
        return VaultError::success();
    }

    bool unlocked = false;
    int saveCount = 0;
    QVariantMap storedPublicCustomData;
    QList<Record> records;
};

} // namespace

class VaultSmokeTest final : public QObject {
    Q_OBJECT
private slots:
    void createVaultOpensEmptyWorkspace()
    {
        auto backend = std::make_unique<MemoryVaultBackend>();
        auto *backendView = backend.get();
        QMainWindow window;
        auto *dock = new VaultDock(std::move(backend), &window);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        SecureBuffer master("master", 6);
        const VaultError created = dock->createVault(QStringLiteral("new-memory.kdbx"), master);
        QVERIFY2(!created.isError(), qPrintable(created.diagnosticCode));
        QCOMPARE(dock->vaultState(), VaultState::Open);
        QVERIFY(backendView->unlocked);
        auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->childCount() + tree->topLevelItem(1)->childCount()
                     + tree->topLevelItem(2)->childCount(), 0);
    }

    void accountNoteSaveFilterAndLock()
    {
        auto backend = std::make_unique<MemoryVaultBackend>();
        auto *backendView = backend.get();
        backendView->records.push_back({QUuid::createUuid(), QStringLiteral("Existing"),
                                        QStringLiteral("alice"), QStringLiteral("https://example.test"),
                                        {}, QStringLiteral("old-secret"), QString(), QString()});

        QMainWindow window;
        window.resize(900, 760);
        auto *dock = new VaultDock(std::move(backend), &window);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        SecureBuffer master("master", 6);
        const VaultError unlock = dock->unlockVault(QStringLiteral("memory.kdbx"), master);
        QVERIFY2(!unlock.isError(), qPrintable(unlock.diagnosticCode));
        QCOMPARE(dock->vaultState(), VaultState::Open);

        auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        auto *newLogin = dock->findChild<QPushButton *>(QStringLiteral("vaultNewLoginButton"));
        auto *newNote = dock->findChild<QPushButton *>(QStringLiteral("vaultNewNoteButton"));
        auto *save = dock->findChild<QPushButton *>(QStringLiteral("vaultSaveButton"));
        auto *edit = dock->findChild<QPushButton *>(QStringLiteral("vaultEditButton"));
        auto *cancel = dock->findChild<QPushButton *>(QStringLiteral("vaultCancelButton"));
        auto *copyUsername = dock->findChild<QPushButton *>(QStringLiteral("vaultCopyUsernameButton"));
        auto *openUrl = dock->findChild<QPushButton *>(QStringLiteral("vaultOpenUrlButton"));
        auto *lock = dock->findChild<QPushButton *>(QStringLiteral("vaultLockButton"));
        auto *title = dock->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
        auto *username = dock->findChild<QLineEdit *>(QStringLiteral("vaultUsernameEdit"));
        auto *url = dock->findChild<QLineEdit *>(QStringLiteral("vaultUrlEdit"));
        auto *tags = dock->findChild<QLineEdit *>(QStringLiteral("vaultTagsEdit"));
        auto *password = dock->findChild<QLineEdit *>(QStringLiteral("vaultPasswordEdit"));
        auto *totp = dock->findChild<QLineEdit *>(QStringLiteral("vaultTotpEdit"));
        auto *totpCode = dock->findChild<QLabel *>(QStringLiteral("vaultTotpCodeLabel"));
        auto *copyTotp = dock->findChild<QPushButton *>(QStringLiteral("vaultCopyTotpButton"));
        auto *copyPassword = dock->findChild<QPushButton *>(QStringLiteral("vaultCopyPasswordButton"));
        auto *generatePassword = dock->findChild<QPushButton *>(QStringLiteral("vaultGeneratePasswordButton"));
        auto *passwordStrength = dock->findChild<QLabel *>(QStringLiteral("vaultPasswordStrengthLabel"));
        auto *showPassword = dock->findChild<QCheckBox *>(QStringLiteral("vaultShowPasswordCheck"));
        auto *revealTimer = dock->findChild<QTimer *>(QStringLiteral("vaultPasswordRevealTimer"));
        auto *notes = dock->findChild<QTextEdit *>(QStringLiteral("vaultNotesEdit"));
        auto *filter = dock->findChild<QLineEdit *>(QStringLiteral("vaultFilterEdit"));
        QVERIFY(tree && newLogin && newNote && save && edit && cancel && copyUsername
                && openUrl && lock && title && username
                && url && tags && password && totp && totpCode && copyTotp && copyPassword
                && generatePassword && passwordStrength
                && showPassword && revealTimer && notes && filter);
        QVERIFY(newLogin->isEnabled() && newNote->isEnabled() && lock->isEnabled());
        QCOMPARE(tree->topLevelItemCount(), 3);

        QTest::mouseClick(newLogin, Qt::LeftButton);
        title->setText(QStringLiteral("Mail account"));
        username->setText(QStringLiteral("bob@example.test"));
        url->setText(QStringLiteral("https://mail.example.test"));
        tags->setText(QStringLiteral("production, email"));
        password->setFocus();
        QTest::keyClicks(password, QStringLiteral("abc"));
        QCOMPARE(passwordStrength->text(), QStringLiteral("一般 · 约 45 bit"));
        QTest::mouseClick(generatePassword, Qt::LeftButton);
        const QString generatedPassword = password->text();
        QCOMPARE(generatedPassword.size(), 24);
        QCOMPARE(passwordStrength->text(), QStringLiteral("很强 · 约 120 bit"));
        QVERIFY(window.grab().save(QStringLiteral("gui-account-generator.png")));
        totp->setText(QStringLiteral("JBSWY3DPEHPK3PXP"));
        notes->setPlainText(QStringLiteral("recovery codes stored offline"));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(backendView->saveCount, 1);
        QCOMPARE(backendView->records.size(), 2);
        QCOMPARE(backendView->records.last().password, generatedPassword);
        QCOMPARE(backendView->records.last().totp, QStringLiteral("JBSWY3DPEHPK3PXP"));
        QVERIFY(backendView->records.last().tags.contains(QStringLiteral("production")));
        QVERIFY(!save->isVisible() && edit->isVisible() && edit->isEnabled());
        QVERIFY(!generatePassword->isVisible());
        QVERIFY(!showPassword->isChecked());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(password->text().isEmpty());
        QVERIFY(title->isReadOnly() && notes->isReadOnly());
        QVERIFY(!totp->isVisible());
        QVERIFY(totp->text().isEmpty());
        QCOMPARE(tags->text(), QStringLiteral("production, email"));
        QVERIFY(tags->isReadOnly());
        QCOMPARE(totp->echoMode(), QLineEdit::Password);
        QCOMPARE(totpCode->text(), QStringLiteral("123 456  ·  18 秒"));
        QVERIFY(copyTotp->isEnabled());
        QVERIFY(window.grab().save(QStringLiteral("gui-account-totp.png")));

        QTest::mouseClick(copyUsername, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("bob@example.test"));
        UrlCapture urlCapture;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &urlCapture, "handle");
        QTest::mouseClick(openUrl, Qt::LeftButton);
        QTRY_COMPARE(urlCapture.opened, QUrl(QStringLiteral("https://mail.example.test")));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));

        QTest::mouseClick(edit, Qt::LeftButton);
        QVERIFY(save->isVisible() && cancel->isVisible());
        QVERIFY(!title->isReadOnly() && !notes->isReadOnly());
        QVERIFY(totp->isVisible());
        QCOMPARE(totp->text(), QStringLiteral("JBSWY3DPEHPK3PXP"));
        notes->setPlainText(QStringLiteral("discard this edit"));
        QTest::mouseClick(cancel, Qt::LeftButton);
        QCOMPARE(notes->toPlainText(), QStringLiteral("recovery codes stored offline"));
        QVERIFY(notes->isReadOnly() && edit->isVisible());
        QVERIFY(totp->text().isEmpty());

        const auto visibleEntryCount = [tree] {
            int count = 0;
            for (int root = 0; root < tree->topLevelItemCount(); ++root) {
                auto *rootItem = tree->topLevelItem(root);
                for (int child = 0; child < rootItem->childCount(); ++child)
                    count += !rootItem->child(child)->isHidden();
            }
            return count;
        };
        filter->setText(QStringLiteral("offline"));
        QCoreApplication::processEvents();
        QCOMPARE(visibleEntryCount(), 1);
        filter->setText(QStringLiteral("production"));
        QCoreApplication::processEvents();
        QCOMPARE(visibleEntryCount(), 1);
        filter->setText(generatedPassword);
        QCoreApplication::processEvents();
        QCOMPARE(visibleEntryCount(), 0); // password values are deliberately excluded
        filter->clear();

        QTest::mouseClick(copyTotp, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("123456"));
        QVERIFY(QMetaObject::invokeMethod(dock, "clearProtectedClipboard",
                                          Qt::DirectConnection));
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());

        QTest::mouseClick(showPassword, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QVERIFY(revealTimer->isActive());
        QCOMPARE(revealTimer->interval(), 30000);
        revealTimer->setInterval(20);
        QTRY_COMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(!showPassword->isChecked());
        QVERIFY(password->text().isEmpty());

        QTest::mouseClick(showPassword, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        password->setFocus();
        username->setFocus();
        QTRY_COMPARE(password->echoMode(), QLineEdit::Password);

        QTest::mouseClick(copyPassword, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), generatedPassword);
        QGuiApplication::clipboard()->setText(QStringLiteral("new clipboard value"));
        QVERIFY(QMetaObject::invokeMethod(dock, "clearProtectedClipboard",
                                          Qt::DirectConnection));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("new clipboard value"));
        QTest::mouseClick(copyPassword, Qt::LeftButton);
        QVERIFY(QMetaObject::invokeMethod(dock, "clearProtectedClipboard",
                                          Qt::DirectConnection));
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());

        QTest::mouseClick(newNote, Qt::LeftButton);
        title->setText(QStringLiteral("Recovery plan"));
        notes->setPlainText(QStringLiteral("Encrypted note body"));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(backendView->saveCount, 2);
        QCOMPARE(backendView->records.size(), 3);
        QVERIFY(backendView->records.last().tags.contains(VaultSecureNoteTag));
        QCOMPARE(backendView->records.last().notes, QStringLiteral("Encrypted note body"));

        filter->setText(QStringLiteral("Recovery"));
        QCoreApplication::processEvents();
        QCOMPARE(visibleEntryCount(), 2); // account notes + legacy-note title

        QTest::mouseClick(lock, Qt::LeftButton);
        QTRY_COMPARE(dock->vaultState(), VaultState::Locked);
        QVERIFY(title->text().isEmpty());
        QVERIFY(password->text().isEmpty());
        QVERIFY(notes->toPlainText().isEmpty());
        QVERIFY(!backendView->unlocked);
        QVERIFY(!newLogin->isEnabled() && !newNote->isEnabled() && !lock->isEnabled());
    }

    void recoverableDeleteRestoreAndPermanentDelete()
    {
        auto backend = std::make_unique<MemoryVaultBackend>();
        auto *backendView = backend.get();
        const QUuid accountId = QUuid::createUuid();
        backendView->records.push_back({accountId, QStringLiteral("Operations account"),
                                        QStringLiteral("operator"),
                                        QStringLiteral("https://ops.example.test"), {},
                                        QStringLiteral("offline-secret"),
                                        QStringLiteral("production access"), QString()});

        QMainWindow window;
        window.resize(980, 760);
        auto *dock = new VaultDock(std::move(backend), &window);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        SecureBuffer master("master", 6);
        QVERIFY(!dock->unlockVault(QStringLiteral("memory.kdbx"), master).isError());

        auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        auto *title = dock->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
        auto *save = dock->findChild<QPushButton *>(QStringLiteral("vaultSaveButton"));
        auto *restore = dock->findChild<QPushButton *>(QStringLiteral("vaultRestoreButton"));
        auto *remove = dock->findChild<QPushButton *>(QStringLiteral("vaultDeleteButton"));
        QVERIFY(tree && title && save && restore && remove);

        auto *accounts = rootNamed(tree, QStringLiteral("账号"));
        QVERIFY(accounts && accounts->childCount() == 1);
        tree->setCurrentItem(accounts->child(0));
        QTRY_COMPARE(title->text(), QStringLiteral("Operations account"));
        QCOMPARE(remove->text(), QStringLiteral("移到回收站"));

        acceptNextQuestion();
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_VERIFY(backendView->records.first().recycled);
        QCOMPARE(backendView->saveCount, 1);

        auto *recycle = rootNamed(tree, QStringLiteral("回收站"));
        QVERIFY(recycle && recycle->childCount() == 1 && !recycle->isHidden());
        recycle->setExpanded(true);
        tree->setCurrentItem(recycle->child(0));
        QTRY_VERIFY(restore->isVisible());
        QVERIFY(restore->isEnabled());
        QVERIFY(!save->isVisible());
        QVERIFY(title->isReadOnly() || !title->isEnabled());
        QCOMPARE(remove->text(), QStringLiteral("永久删除"));
        QVERIFY(window.grab().save(QStringLiteral("gui-account-recycle.png")));

        QTest::mouseClick(restore, Qt::LeftButton);
        QTRY_VERIFY(!backendView->records.first().recycled);
        QCOMPARE(backendView->saveCount, 2);
        auto *edit = dock->findChild<QPushButton *>(QStringLiteral("vaultEditButton"));
        QVERIFY(edit);
        QTRY_VERIFY(edit->isVisible() && edit->isEnabled());
        QVERIFY(!save->isVisible());
        QCOMPARE(remove->text(), QStringLiteral("移到回收站"));

        acceptNextQuestion();
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_VERIFY(backendView->records.first().recycled);
        recycle = rootNamed(tree, QStringLiteral("回收站"));
        QVERIFY(recycle && recycle->childCount() == 1);
        tree->setCurrentItem(recycle->child(0));
        QTRY_COMPARE(remove->text(), QStringLiteral("永久删除"));

        acceptNextQuestion();
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_VERIFY(backendView->records.isEmpty());
        QCOMPARE(backendView->saveCount, 4);
        recycle = rootNamed(tree, QStringLiteral("回收站"));
        QVERIFY(recycle && recycle->childCount() == 0 && recycle->isHidden());
    }

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    void productionAccountHistoryCanRestorePassword()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("history.kdbx"));

        QMainWindow window;
        window.resize(980, 720);
        auto *dock = new VaultDock(createProductionVaultBackend(), &window);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        SecureBuffer master("history-test-master", 19);
        QVERIFY(!dock->createVault(path, master).isError());

        auto *newLogin = dock->findChild<QPushButton *>(
            QStringLiteral("vaultNewLoginButton"));
        auto *title = dock->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
        auto *username = dock->findChild<QLineEdit *>(
            QStringLiteral("vaultUsernameEdit"));
        auto *password = dock->findChild<QLineEdit *>(
            QStringLiteral("vaultPasswordEdit"));
        auto *save = dock->findChild<QPushButton *>(QStringLiteral("vaultSaveButton"));
        auto *edit = dock->findChild<QPushButton *>(QStringLiteral("vaultEditButton"));
        auto *history = dock->findChild<QPushButton *>(
            QStringLiteral("vaultHistoryButton"));
        auto *copyPassword = dock->findChild<QPushButton *>(
            QStringLiteral("vaultCopyPasswordButton"));
        QVERIFY(newLogin && title && username && password && save && edit
                && history && copyPassword);

        QTest::mouseClick(newLogin, Qt::LeftButton);
        title->setText(QStringLiteral("History account"));
        username->setText(QStringLiteral("operator@example.test"));
        password->setText(QStringLiteral("old-password-value"));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(edit->isVisible());

        QTest::mouseClick(edit, Qt::LeftButton);
        password->setText(QStringLiteral("new-password-value"));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(history->isVisible() && history->isEnabled());

        bool inspected = false;
        bool driverError = false;
        int dialogPhase = 0;
        QTimer dialogDriver;
        dialogDriver.setInterval(20);
        connect(&dialogDriver, &QTimer::timeout, &window, [&]() {
            QWidget *active = QApplication::activeModalWidget();
            if (dialogPhase == 0) {
                auto *dialog = qobject_cast<QDialog *>(active);
                if (!dialog || dialog->objectName() != QStringLiteral("vaultHistoryDialog"))
                    return;
                auto *list = dialog->findChild<QListWidget *>(
                    QStringLiteral("vaultHistoryList"));
                auto *restore = dialog->findChild<QPushButton *>(
                    QStringLiteral("vaultHistoryRestoreButton"));
                if (!list || !restore || list->count() != 1) {
                    driverError = true;
                    dialog->reject();
                    return;
                }
                const QString row = list->item(0)->text();
                driverError = !row.contains(QStringLiteral("History account"))
                              || row.contains(QStringLiteral("old-password-value"));
                inspected = dialog->grab().save(QStringLiteral("gui-account-history.png"));
                dialogPhase = 1;
                QTimer::singleShot(50, &window, [&]() {
                    auto *question = qobject_cast<QMessageBox *>(
                        QApplication::activeModalWidget());
                    auto *yes = question ? question->button(QMessageBox::Yes) : nullptr;
                    if (!yes) {
                        driverError = true;
                        if (question)
                            question->reject();
                        return;
                    }
                    dialogPhase = 2;
                    QTest::mouseClick(yes, Qt::LeftButton);
                });
                QTest::mouseClick(restore, Qt::LeftButton);
                return;
            }
        });
        dialogDriver.start();
        QTest::mouseClick(history, Qt::LeftButton);
        dialogDriver.stop();
        QVERIFY(inspected);
        QVERIFY(!driverError);
        QCOMPARE(dialogPhase, 2);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTRY_VERIFY(copyPassword->isVisible() && copyPassword->isEnabled());
        QTest::mouseClick(copyPassword, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("old-password-value"));

        // Restore itself creates a new history item for the superseded current
        // version, so users can move both directions without data loss.
        QTimer::singleShot(80, &window, [&]() {
            auto *dialog = window.findChild<QDialog *>(
                QStringLiteral("vaultHistoryDialog"));
            auto *list = dialog ? dialog->findChild<QListWidget *>(
                QStringLiteral("vaultHistoryList")) : nullptr;
            QVERIFY(dialog && list);
            QCOMPARE(list->count(), 2);
            dialog->reject();
        });
        QTest::mouseClick(history, Qt::LeftButton);
        dock->lockVault();
        QVERIFY(password->text().isEmpty());
    }

    void productionKdbxFixtureUnlocksIntoEditor()
    {
        auto backend = createProductionVaultBackend();
        QVERIFY(backend != nullptr);

        QMainWindow window;
        window.resize(900, 760);
        auto *dock = new VaultDock(std::move(backend), &window);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QString fixture = QStringLiteral(NOTEPADSAFE_KDBX_FIXTURE_DIR)
                                + QStringLiteral("/Format400.kdbx");
        SecureBuffer password("t", 1);
        const VaultError unlock = dock->unlockVault(fixture, password);
        QVERIFY2(!unlock.isError(), qPrintable(unlock.diagnosticCode));

        auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("vaultEntryTree"));
        auto *title = dock->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
        QVERIFY(tree && title);
        int entryCount = 0;
        QTreeWidgetItem *firstEntry = nullptr;
        for (int root = 0; root < tree->topLevelItemCount(); ++root) {
            auto *rootItem = tree->topLevelItem(root);
            entryCount += rootItem->childCount();
            if (!firstEntry && rootItem->childCount() > 0)
                firstEntry = rootItem->child(0);
        }
        QVERIFY(entryCount > 0);
        QVERIFY(firstEntry != nullptr);
        tree->setCurrentItem(firstEntry);
        QTRY_VERIFY(!title->text().isEmpty());

        dock->lockVault();
        QCOMPARE(dock->vaultState(), VaultState::Locked);
        QVERIFY(title->text().isEmpty());
    }
#endif
};

QTEST_MAIN(VaultSmokeTest)
#include "smoke_vault.moc"
