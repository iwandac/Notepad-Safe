#include "IVaultBackend.h"
#include "VaultBackendFactory.h"
#include "VaultDock.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QtTest>

namespace {

QString textFrom(const SecureBuffer &value)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(value.data()), value.size());
}

struct Record {
    QUuid id;
    QString title;
    QString username;
    QString url;
    QStringList tags;
    QString password;
    QString notes;
    QString totp;
};

class MemoryVaultBackend final : public IVaultBackend {
public:
    VaultError create(const QString &, const SecureBuffer &password, const QString &keyFile) override
    {
        return unlock({}, password, keyFile);
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
            result.push_back({record.id, record.title, record.username, record.url, record.tags,
                              record.tags.contains(VaultSecureNoteTag), !record.password.isEmpty()});
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

    VaultError removeEntry(const QUuid &id) override
    {
        for (qsizetype i = 0; i < records.size(); ++i) {
            if (records[i].id == id) {
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
        QCOMPARE(tree->topLevelItemCount(), 2);
        QCOMPARE(tree->topLevelItem(0)->childCount() + tree->topLevelItem(1)->childCount(), 0);
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
        auto *lock = dock->findChild<QPushButton *>(QStringLiteral("vaultLockButton"));
        auto *title = dock->findChild<QLineEdit *>(QStringLiteral("vaultTitleEdit"));
        auto *username = dock->findChild<QLineEdit *>(QStringLiteral("vaultUsernameEdit"));
        auto *url = dock->findChild<QLineEdit *>(QStringLiteral("vaultUrlEdit"));
        auto *password = dock->findChild<QLineEdit *>(QStringLiteral("vaultPasswordEdit"));
        auto *totp = dock->findChild<QLineEdit *>(QStringLiteral("vaultTotpEdit"));
        auto *copyPassword = dock->findChild<QPushButton *>(QStringLiteral("vaultCopyPasswordButton"));
        auto *notes = dock->findChild<QTextEdit *>(QStringLiteral("vaultNotesEdit"));
        auto *filter = dock->findChild<QLineEdit *>(QStringLiteral("vaultFilterEdit"));
        QVERIFY(tree && newLogin && newNote && save && lock && title && username
                && url && password && totp && copyPassword && notes && filter);
        QVERIFY(newLogin->isEnabled() && newNote->isEnabled() && lock->isEnabled());
        QCOMPARE(tree->topLevelItemCount(), 2);

        QTest::mouseClick(newLogin, Qt::LeftButton);
        title->setText(QStringLiteral("Mail account"));
        username->setText(QStringLiteral("bob@example.test"));
        url->setText(QStringLiteral("https://mail.example.test"));
        password->setText(QStringLiteral("correct horse"));
        totp->setText(QStringLiteral("JBSWY3DPEHPK3PXP"));
        notes->setPlainText(QStringLiteral("recovery codes stored offline"));
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(backendView->saveCount, 1);
        QCOMPARE(backendView->records.size(), 2);
        QCOMPARE(backendView->records.last().password, QStringLiteral("correct horse"));
        QCOMPARE(backendView->records.last().totp, QStringLiteral("JBSWY3DPEHPK3PXP"));

        QTest::mouseClick(copyPassword, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("correct horse"));
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
        int visibleChildren = 0;
        for (int root = 0; root < tree->topLevelItemCount(); ++root) {
            auto *rootItem = tree->topLevelItem(root);
            for (int child = 0; child < rootItem->childCount(); ++child)
                visibleChildren += !rootItem->child(child)->isHidden();
        }
        QCOMPARE(visibleChildren, 1);

        QTest::mouseClick(lock, Qt::LeftButton);
        QTRY_COMPARE(dock->vaultState(), VaultState::Locked);
        QVERIFY(title->text().isEmpty());
        QVERIFY(password->text().isEmpty());
        QVERIFY(notes->toPlainText().isEmpty());
        QVERIFY(!backendView->unlocked);
        QVERIFY(!newLogin->isEnabled() && !newNote->isEnabled() && !lock->isEnabled());
    }

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
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
