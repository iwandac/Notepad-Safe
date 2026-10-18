#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <memory>

#include "AppComposition.h"
#include "DraftStore.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "RecoveryIssuePage.h"
#include "SessionManager.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

namespace {

class RejectingOpenProbe final : public IDocumentProvider {
public:
    RejectingOpenProbe(DocumentProviderDescriptor descriptor, int *calls)
        : descriptor_(std::move(descriptor)), calls_(calls) {}
    DocumentProviderDescriptor descriptor() const override { return descriptor_; }
    bool open(IDocumentSurface *, const DocumentSurfaceOpenRequest &, QString *error) override
    {
        ++*calls_;
        if (error)
            *error = QStringLiteral("test negative provider must never be called");
        return false;
    }
private:
    DocumentProviderDescriptor descriptor_;
    int *calls_;
};

bool installRejectingOpenProbes(AppComposition &composition, int *calls)
{
    const auto descriptors = composition.documentProviders().descriptors();
    for (DocumentProviderDescriptor descriptor : descriptors) {
        descriptor.id.prepend(QStringLiteral("2b1-negative."));
        descriptor.priority = 10000;
        if (!composition.documentProviders().registerProvider(descriptor,
                [descriptor, calls] {
                    return std::make_unique<RejectingOpenProbe>(descriptor, calls);
                })) {
            return false;
        }
    }
    return true;
}

void rejectUnexpectedDialogs(int *count)
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *dialog = qobject_cast<QDialog *>(widget);
            dialog && dialog->isVisible() && dialog->isModal()) {
            ++*count;
            dialog->reject();
        }
    }
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray sha256File(const QString &path)
{
    return QCryptographicHash::hash(readAll(path), QCryptographicHash::Sha256);
}

QTabWidget *tabsOf(MainWindow *window)
{
    return window ? window->findChild<QTabWidget *>() : nullptr;
}

EditorTab *findEditor(MainWindow *window, const QString &path = {})
{
    auto *tabs = tabsOf(window);
    if (!tabs)
        return nullptr;
    for (int index = 0; index < tabs->count(); ++index) {
        auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
        if (tab && (path.isEmpty()
                    || QFileInfo(tab->filePath()).absoluteFilePath()
                           == QFileInfo(path).absoluteFilePath())) {
            return tab;
        }
    }
    return nullptr;
}

EditorTab *waitForHex(MainWindow *window, const QString &path, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        EditorTab *tab = findEditor(window, path);
        if (tab && tab->isBinaryDocument() && tab->l2View()
            && tab->l2View()->isOpen() && tab->viewModeL2() == ViewMode::Hex) {
            return tab;
        }
        QTest::qWait(10);
    }
    return nullptr;
}

QWidget *waitForIssue(MainWindow *window, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (auto *issue = window->findChild<QWidget *>(
                QStringLiteral("RecoveryIssuePage"))) {
            return issue;
        }
        QTest::qWait(10);
    }
    return nullptr;
}

void destroyWithoutClose(MainWindow *window)
{
    if (!window)
        return;
    if (auto *tabs = tabsOf(window)) {
        for (int index = 0; index < tabs->count(); ++index) {
            if (auto *tab = qobject_cast<EditorTab *>(tabs->widget(index)))
                tab->shutdownL2BeforeClose();
        }
    }
    delete window;
    QApplication::processEvents(QEventLoop::AllEvents, 100);
}

QByteArray binaryFixture()
{
    QByteArray bytes(4096, '\0');
    for (int index = 0; index < bytes.size(); ++index)
        bytes[index] = char((index * 73 + 19) & 0xff);
    bytes[0] = '\0';
    return bytes;
}

struct RecoveryFixture {
    QString dataDir;
    QString sourcePath;
    QDateTime sourceModified;
    QByteArray original;
    QByteArray edited;
    QString journalPath;
};

RecoveryFixture createBinaryRecoveryFixture(const QString &root)
{
    RecoveryFixture fixture;
    fixture.dataDir = QDir(root).filePath(QStringLiteral("app-data"));
    fixture.sourcePath = QDir(root).filePath(QStringLiteral("source.bin"));
    QDir().mkpath(fixture.dataDir);
    fixture.original = binaryFixture();
    fixture.edited = fixture.original;
    fixture.edited.replace(64, 4, QByteArrayLiteral("R1C!"));
    if (!writeAll(fixture.sourcePath, fixture.original))
        return {};
    fixture.sourceModified = QFileInfo(fixture.sourcePath).lastModified();

    qputenv("NOTEPADSAFE_DATADIR", fixture.dataDir.toUtf8());
    auto *window = new MainWindow;
    window->resize(1050, 700);
    window->show();
    if (!QTest::qWaitForWindowExposed(window)) {
        destroyWithoutClose(window);
        return {};
    }
    window->openFilesFromExternal({fixture.sourcePath});
    EditorTab *tab = waitForHex(window, fixture.sourcePath);
    if (!tab || !tab->l2View()->replaceBytes(64, 4, QByteArrayLiteral("R1C!"))) {
        destroyWithoutClose(window);
        return {};
    }
    tab->l2View()->selectBytes(64, 68);
    QVector<ISessionDocumentPort *> documents{tab};
    SessionManager manager(fixture.dataDir);
    if (!manager.saveSession(documents, 0, true)) {
        destroyWithoutClose(window);
        return {};
    }
    const SessionData saved = manager.loadSession();
    if (saved.tabs.size() != 1) {
        destroyWithoutClose(window);
        return {};
    }
    fixture.journalPath = saved.tabs.first().autosavePath;
    destroyWithoutClose(window);
    return fixture;
}

bool recoveryTreeContains(const QString &root, const QByteArray &needle)
{
    QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        if (readAll(files.next()).contains(needle))
            return true;
    }
    return false;
}

} // namespace

class RecoveryIssueSmoke final : public QObject {
    Q_OBJECT

    QTemporaryDir temp_;
    QByteArray originalDataDir_;
    bool hadDataDir_ = false;

private slots:
    void initTestCase()
    {
        QVERIFY(temp_.isValid());
        hadDataDir_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        originalDataDir_ = qgetenv("NOTEPADSAFE_DATADIR");
    }

    void cleanupTestCase()
    {
        if (hadDataDir_)
            qputenv("NOTEPADSAFE_DATADIR", originalDataDir_);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void tc_2b1_binary_text_encoding_clean_restore_data()
    {
        QTest::addColumn<qint64>("sourceSize");
        QTest::newRow("B1") << qint64(4096);
        QTest::newRow("B2-readonly") << qint64(32 * 1024 * 1024);
    }

    void tc_2b1_manual_syntax_clean_restore()
    {
        const QString root = temp_.filePath(QStringLiteral("clean-syntax"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("manual-language.txt"));
        QVERIFY(writeAll(path, QByteArrayLiteral("const message = 'keep source';\n")));
        const QByteArray sourceHash = sha256File(path);
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner first(new MainWindow, destroyWithoutClose);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first.get()));
        first->openFilesFromExternal({path});
        EditorTab *source = findEditor(first.get(), path);
        QVERIFY(source);
        auto *language = first->findChild<QAction *>(QStringLiteral("language.javascript"));
        QVERIFY(language && language->isEnabled());
        language->trigger();
        QCOMPARE(source->syntax(), Syntax::JavaScript);
        QVERIFY(!source->isModified());
        SessionManager manager(dataDir);
        QVERIFY(manager.saveSession({source}, 0, false));
        QCOMPARE(manager.loadSession().tabs.first().syntax, int(Syntax::JavaScript));
        first.reset();

        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        EditorTab *restored = findEditor(restarted.get(), path);
        QVERIFY(restored);
        QCOMPARE(sha256File(path), sourceHash);
        QVERIFY(!restored->isModified());
        QVERIFY(!restored->isL2Mode());
        QCOMPARE(restored->syntax(), Syntax::JavaScript);
    }

    void tc_2b1_manual_syntax_recovery_retry()
    {
        const QString root = temp_.filePath(QStringLiteral("retry-syntax"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("manual-language.txt"));
        const QByteArray original("const original = 'disk';\n");
        const QString edited = QString::fromUtf8(original)
                               + QStringLiteral("const unsaved = 'recovered';\n");
        QVERIFY(writeAll(path, original));
        const QByteArray sourceHash = sha256File(path);
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner first(new MainWindow, destroyWithoutClose);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first.get()));
        first->openFilesFromExternal({path});
        EditorTab *source = findEditor(first.get(), path);
        QVERIFY(source && !source->isL2Mode());
        auto *language = first->findChild<QAction *>(QStringLiteral("language.javascript"));
        QVERIFY(language && language->isEnabled());
        language->trigger();
        QCOMPARE(source->syntax(), Syntax::JavaScript);
        source->editor()->append(QStringLiteral("const unsaved = 'recovered';\n"));
        QCOMPARE(source->editor()->text(), edited);
        QVERIFY(source->isModified());
        SessionManager manager(dataDir);
        QVERIFY(manager.saveSession({source}, 0, true));
        const SessionData saved = manager.loadSession();
        QCOMPARE(saved.tabs.size(), 1);
        QCOMPARE(saved.tabs.first().syntax, int(Syntax::JavaScript));
        QVERIFY(saved.tabs.first().modified);
        const QString draftPath = saved.tabs.first().autosavePath;
        QVERIFY(DraftStore::isEncryptedDraft(draftPath));
        first.reset();
        const QByteArray intactDraft = readAll(draftPath);
        QVERIFY(intactDraft.size() > 80);
        QByteArray corrupt = intactDraft;
        corrupt.chop(1);
        QVERIFY(writeAll(draftPath, corrupt));
        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        restarted->deferredRestoreSession();
        QWidget *issue = waitForIssue(restarted.get());
        QVERIFY(issue);
        auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        QVERIFY(retry);
        QCOMPARE(sha256File(path), sourceHash);
        QVERIFY(writeAll(draftPath, intactDraft));
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(restarted->findChild<QWidget *>(
            QStringLiteral("RecoveryIssuePage")) == nullptr, 10000);
        EditorTab *restored = findEditor(restarted.get(), path);
        QVERIFY(restored);
        QCOMPARE(sha256File(path), sourceHash);
        QVERIFY(QFileInfo::exists(draftPath));
        QVERIFY(DraftStore::isEncryptedDraft(draftPath));
        QCOMPARE(restored->editor()->text(), edited);
        QVERIFY(!restored->isL2Mode());
        QVERIFY(restored->isModified()); // Existing recovered dirty content, not a new edit.
        QCOMPARE(restored->syntax(), Syntax::JavaScript);
    }

    void tc_2b1_clean_restore_failure_is_visible_data()
    {
        QTest::addColumn<int>("failureKind");
        QTest::newRow("missing") << 0;
        QTest::newRow("secure-adapter-contradicts-old-flag") << 1;
#ifdef Q_OS_WIN
        QTest::newRow("unreadable-exclusive-handle") << 2;
#endif
    }

    void tc_2b1_clean_restore_failure_is_visible()
    {
        QFETCH(int, failureKind);
        const QString root = temp_.filePath(QStringLiteral("clean-failure-%1").arg(failureKind));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("previously-plain.dat"));
        QVERIFY(writeAll(path, QByteArrayLiteral("previously ordinary text\n")));
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner first(new MainWindow, destroyWithoutClose);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first.get()));
        first->openFilesFromExternal({path});
        EditorTab *source = findEditor(first.get(), path);
        QVERIFY(source && !source->isSecureDocument());
        SessionManager manager(dataDir);
        QVERIFY(manager.saveSession({source}, 0, false));
        QVERIFY(!manager.loadSession().tabs.first().secureDocument);
        first.reset();
        QByteArray currentHash;
        if (failureKind == 1) {
            // The exact registered KDBX header is enough to establish the
            // persistence boundary; this is not a fake successful unlock.
            QVERIFY(writeAll(path, QByteArray::fromHex("03d9a29a67fb4bb501000400")
                                   + QByteArray(256, '\0')));
            AppComposition composition;
            const auto adapter = composition.persistenceAdapters().resolve(path);
            QVERIFY(adapter && adapter->persistence == DocumentPersistence::SecureFile);
            currentHash = sha256File(path);
        } else if (failureKind == 0) {
            QVERIFY(QFile::remove(path));
        } else {
            currentHash = sha256File(path);
        }
#ifdef Q_OS_WIN
        std::unique_ptr<void, decltype(&CloseHandle)> exclusive(nullptr, CloseHandle);
        if (failureKind == 2) {
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(handle != INVALID_HANDLE_VALUE);
            exclusive.reset(handle);
            QFile readable(path);
            QVERIFY(!readable.open(QIODevice::ReadOnly));
        }
#endif
        int providerCalls = 0;
        AppComposition composition;
        QVERIFY(installRejectingOpenProbes(composition, &providerCalls));
        WindowOwner restarted(new MainWindow(nullptr, &composition), destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        int unexpectedDialogs = 0;
        QTimer dialogGuard;
        connect(&dialogGuard, &QTimer::timeout, this,
                [&]() { rejectUnexpectedDialogs(&unexpectedDialogs); });
        dialogGuard.start(20);
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
#ifdef Q_OS_WIN
        exclusive.reset();
#endif
        if (failureKind != 0)
            QCOMPARE(sha256File(path), currentHash);
        else
            QVERIFY(!QFileInfo::exists(path));
        QString visible = restarted->statusBar()->currentMessage();
        for (QLabel *label : restarted->findChildren<QLabel *>()) {
            if (label->isVisible())
                visible += QLatin1Char('\n') + label->text();
        }
        qInfo().noquote() << "2B1 clean failure visible state:" << visible;
        QCOMPARE(providerCalls, 0);
        QCOMPARE(unexpectedDialogs, 0);
        QVERIFY2(!findEditor(restarted.get(), path),
                 "Missing or currently secure paths must not enter a plain provider during clean restore");
        for (int i = 0; i < tabsOf(restarted.get())->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(tabsOf(restarted.get())->widget(i)))
                QVERIFY(!tab->isModified());
        }
        QVERIFY2(!visible.contains(QStringLiteral("已恢复 1 个标签页")), qPrintable(visible));
        QVERIFY2(visible.contains(QFileInfo(path).fileName()), qPrintable(visible));
        QVERIFY2(visible.contains(QStringLiteral("原文件"))
                     && (visible.contains(QStringLiteral("未改"))
                         || visible.contains(QStringLiteral("没有被"))), qPrintable(visible));
        QVERIFY2(visible.contains(QStringLiteral("重新打开"))
                     || visible.contains(QStringLiteral("重试"))
                     || visible.contains(QStringLiteral("文件→打开")), qPrintable(visible));
        auto *issue = restarted->findChild<RecoveryIssuePage *>();
        QVERIFY(issue && issue->kind() == RecoveryIssuePage::Kind::OriginalFile);
        if (failureKind == 1) {
            auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
            QVERIFY(retry && retry->text() == QStringLiteral("重试打开"));
            QTest::mouseClick(retry, Qt::LeftButton);
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            QCOMPARE(providerCalls, 0);
            QCOMPARE(unexpectedDialogs, 0);
            QVERIFY(restarted->findChild<RecoveryIssuePage *>() == issue);
            QCOMPARE(sha256File(path), currentHash);
        } else if (failureKind == 0) {
            // Saving a clean pending-open item must not disguise it as dirty
            // merely to retain it across another real session restoration.
            QVERIFY(manager.saveSession({issue}, 0, false));
            const SessionData pending = manager.loadSession();
            QCOMPARE(pending.tabs.size(), 1);
            QVERIFY(!pending.tabs.first().modified);
            QVERIFY(pending.tabs.first().autosavePath.isEmpty());
            QCOMPARE(pending.tabs.first().filePath, path);
            restarted.reset(new MainWindow(nullptr, &composition));
            restarted->show();
            QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
            QSignalSpy again(restarted.get(), &MainWindow::sessionRestoreCompleted);
            restarted->deferredRestoreSession();
            QTRY_COMPARE_WITH_TIMEOUT(again.count(), 1, 10000);
            auto *retained = restarted->findChild<RecoveryIssuePage *>();
            QVERIFY(retained && retained->kind() == RecoveryIssuePage::Kind::OriginalFile);
            QVERIFY(!retained->captureSessionTab().modified);
            QVERIFY(QFileInfo::exists(manager.sessionFilePath()));
            QCOMPARE(unexpectedDialogs, 0);
            QCOMPARE(providerCalls, 0);
        }
    }

    void tc_2b1_modified_without_draft_does_not_load_original_data()
    {
        QTest::addColumn<bool>("explicitOpen");
        QTest::newRow("standalone") << false;
        QTest::newRow("coexists-with-explicit-open") << true;
    }

    void tc_2b1_modified_without_draft_does_not_load_original()
    {
        QFETCH(bool, explicitOpen);
        const QString root = temp_.filePath(QStringLiteral("lost-draft-%1").arg(explicitOpen));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("original.txt"));
        QVERIFY(writeAll(path, QByteArrayLiteral("disk original is not the lost edits\n")));
        const QByteArray sourceHash = sha256File(path);
        SessionManager manager(dataDir);
        const QJsonObject record{
            {QStringLiteral("filePath"), path},
            {QStringLiteral("modified"), true},
            {QStringLiteral("secureDocument"), false}};
        // A modified tab whose draft is gone. (A draft path outside the managed
        // autosave folder makes the whole manifest invalid, by design.)
        QVERIFY(writeAll(manager.sessionFilePath(), QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 2}, {QStringLiteral("activeTab"), 0},
            {QStringLiteral("tabs"), QJsonArray{record}}}).toJson()));
        const SessionData loaded = manager.loadSession();
        QCOMPARE(loaded.tabs.size(), 1);
        QVERIFY(loaded.tabs.first().modified);
        QVERIFY(loaded.tabs.first().autosavePath.isEmpty());
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        EditorTab *explicitTab = nullptr;
        QUuid explicitId;
        if (explicitOpen) {
            restarted->openFilesFromExternal({path});
            explicitTab = findEditor(restarted.get(), path);
            QVERIFY(explicitTab);
            explicitId = explicitTab->documentState().id;
        }
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(sha256File(path), sourceHash);
        if (explicitOpen) {
            QCOMPARE(findEditor(restarted.get(), path), explicitTab);
            QCOMPARE(explicitTab->documentState().id, explicitId);
            QVERIFY(!explicitTab->isModified());
            QCOMPARE(tabsOf(restarted.get())->currentWidget(), explicitTab);
            QCOMPARE(tabsOf(restarted.get())->count(), 2);
        } else {
            QVERIFY2(!findEditor(restarted.get(), path),
                     "A lost dirty recovery must not silently become a clean disk-file restore");
        }
        auto *issue = restarted->findChild<RecoveryIssuePage *>();
        QVERIFY(issue && issue->kind() == RecoveryIssuePage::Kind::UnsavedDraft);
        QVERIFY(issue->captureSessionTab().modified);
        const QString summary = issue->findChild<QLabel *>(QStringLiteral("recoveryIssueSummary"))->text();
        QVERIFY(!summary.contains(QStringLiteral("恢复稿仍保留")));
        QVERIFY(QFileInfo::exists(manager.sessionFilePath()));
    }

    void tc_2b1_unreadable_original_retry_restores_view()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows exclusive-handle file sharing fixture");
#else
        const QString root = temp_.filePath(QStringLiteral("unreadable-retry"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("selected-view.txt"));
        QVERIFY(writeAll(path, QByteArrayLiteral("const selected = 'view';\n")));
        const QByteArray sourceHash = sha256File(path);
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner first(new MainWindow, destroyWithoutClose);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first.get()));
        first->openFilesFromExternal({path});
        EditorTab *source = findEditor(first.get(), path);
        QVERIFY(source);
        auto *encoding = first->findChild<QAction *>(QStringLiteral("encoding.display.windows1252"));
        auto *language = first->findChild<QAction *>(QStringLiteral("language.javascript"));
        QVERIFY(encoding && encoding->isEnabled() && language && language->isEnabled());
        encoding->trigger();
        language->trigger();
        QCOMPARE(source->encoding(), TextEncoding::Windows1252);
        QCOMPARE(source->syntax(), Syntax::JavaScript);
        QVERIFY(!source->isModified());
        SessionManager manager(dataDir);
        QVERIFY(manager.saveSession({source}, 0, false));
        first.reset();
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        std::unique_ptr<void, decltype(&CloseHandle)> exclusive(handle, CloseHandle);
        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        auto *issue = restarted->findChild<RecoveryIssuePage *>();
        QVERIFY(issue && issue->kind() == RecoveryIssuePage::Kind::OriginalFile);
        QVERIFY(!findEditor(restarted.get(), path));
        exclusive.reset();
        auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        QVERIFY(retry && retry->text() == QStringLiteral("重试打开"));
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY(restarted->findChild<RecoveryIssuePage *>() == nullptr);
        EditorTab *restored = findEditor(restarted.get(), path);
        QVERIFY(restored);
        QCOMPARE(restored->encoding(), TextEncoding::Windows1252);
        QCOMPARE(restored->syntax(), Syntax::JavaScript);
        QVERIFY(!restored->isModified());
        QCOMPARE(restored->editor()->text(), QStringLiteral("const selected = 'view';\n"));
        QCOMPARE(sha256File(path), sourceHash);
#endif
    }

    void tc_2b1_dirty_draft_secure_path_change_is_rejected()
    {
        const QString root = temp_.filePath(QStringLiteral("dirty-source-became-secure"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString path = QDir(root).filePath(QStringLiteral("previously-plain.dat"));
        QVERIFY(writeAll(path, QByteArrayLiteral("ordinary source before replacement\n")));
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner first(new MainWindow, destroyWithoutClose);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first.get()));
        first->openFilesFromExternal({path});
        EditorTab *source = findEditor(first.get(), path);
        QVERIFY(source && !source->isL2Mode() && !source->isSecureDocument());
        source->editor()->append(QStringLiteral("unsaved plain draft must remain recoverable\n"));
        QVERIFY(source->isModified());
        SessionManager manager(dataDir);
        QVERIFY(manager.saveSession({source}, 0, true));
        const SessionData saved = manager.loadSession();
        QCOMPARE(saved.tabs.size(), 1);
        QVERIFY(saved.tabs.first().modified && !saved.tabs.first().secureDocument);
        const QString draftPath = saved.tabs.first().autosavePath;
        QVERIFY(DraftStore::isEncryptedDraft(draftPath));
        first.reset();
        const QByteArray draftHash = sha256File(draftPath);
        QVERIFY(writeAll(path, QByteArray::fromHex("03d9a29a67fb4bb501000400")
                               + QByteArray(256, '\0')));
        const QByteArray secureSourceHash = sha256File(path);
        AppComposition composition;
        const auto adapter = composition.persistenceAdapters().resolve(path);
        QVERIFY(adapter && adapter->persistence == DocumentPersistence::SecureFile);
        WindowOwner restarted(new MainWindow(nullptr, &composition), destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        int unexpectedDialogs = 0;
        QTimer dialogGuard;
        connect(&dialogGuard, &QTimer::timeout, this,
                [&]() { rejectUnexpectedDialogs(&unexpectedDialogs); });
        dialogGuard.start(20);
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(unexpectedDialogs, 0);
        QCOMPARE(sha256File(path), secureSourceHash);
        QCOMPARE(sha256File(draftPath), draftHash);
        if (auto *wronglyRestored = findEditor(restarted.get(), path)) {
            qInfo() << "2B1 dirty secure mismatch editor state: dirty="
                    << wronglyRestored->isModified() << "secure="
                    << wronglyRestored->isSecureDocument() << "persistence="
                    << int(wronglyRestored->documentState().persistence);
        }
        QVERIFY2(!findEditor(restarted.get(), path),
                 "A plain dirty recovery must not bind a saveable plain editor to a current secure file");
        auto *issue = restarted->findChild<RecoveryIssuePage *>();
        QVERIFY(issue && issue->kind() == RecoveryIssuePage::Kind::UnsavedDraft);
        QCOMPARE(issue->captureSessionTab().autosavePath, draftPath);
        QVERIFY(QFileInfo::exists(manager.sessionFilePath()));
        auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        QVERIFY(retry && retry->text() == QStringLiteral("重试恢复"));
        QTest::mouseClick(retry, Qt::LeftButton);
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        QCOMPARE(unexpectedDialogs, 0);
        QVERIFY(restarted->findChild<RecoveryIssuePage *>() == issue);
        QVERIFY(!findEditor(restarted.get(), path));
        QCOMPARE(sha256File(path), secureSourceHash);
        QCOMPARE(sha256File(draftPath), draftHash);
    }

    void tc_2b1_remove_original_issue_never_removes_source()
    {
        const QString root = temp_.filePath(QStringLiteral("remove-original-issue"));
        QVERIFY(QDir().mkpath(root));
        const RecoveryFixture fixture = createBinaryRecoveryFixture(root);
        QVERIFY(!fixture.journalPath.isEmpty());
        QByteArray corrupt = readAll(fixture.journalPath);
        QVERIFY(corrupt.size() > 80);
        corrupt.chop(1);
        QVERIFY(writeAll(fixture.journalPath, corrupt));
        const QByteArray draftHash = sha256File(fixture.journalPath);
        const QByteArray draftSourceHash = sha256File(fixture.sourcePath);
        const QString path = QDir(root).filePath(QStringLiteral("missing-then-returned.txt"));
        SessionManager manager(fixture.dataDir);
        const SessionTab draftRecord = manager.loadSession().tabs.first();
        QVERIFY(draftRecord.modified);
        QVERIFY(!draftRecord.autosavePath.isEmpty());
        const QJsonObject record{{QStringLiteral("filePath"), path},
                                 {QStringLiteral("modified"), false},
                                 {QStringLiteral("secureDocument"), false}};
        QJsonObject sessionJson = QJsonDocument::fromJson(readAll(manager.sessionFilePath())).object();
        QJsonArray records = sessionJson.value(QStringLiteral("tabs")).toArray();
        records.append(record);
        sessionJson.insert(QStringLiteral("tabs"), records);
        sessionJson.insert(QStringLiteral("activeTab"), 1);
        QVERIFY(writeAll(manager.sessionFilePath(), QJsonDocument(sessionJson).toJson()));
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        int unexpectedDialogs = 0;
        QTimer dialogGuard;
        connect(&dialogGuard, &QTimer::timeout, this,
                [&]() { rejectUnexpectedDialogs(&unexpectedDialogs); });
        dialogGuard.start(20);
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(unexpectedDialogs, 0);
        QWidget *issue = nullptr;
        QWidget *draftIssue = nullptr;
        for (QWidget *page : restarted->findChildren<QWidget *>(QStringLiteral("RecoveryIssuePage"))) {
            const auto *port = dynamic_cast<ISessionDocumentPort *>(page);
            QVERIFY(port);
            if (port->captureSessionTab().filePath == path)
                issue = page;
            if (port->captureSessionTab().filePath == fixture.sourcePath)
                draftIssue = page;
        }
        QVERIFY(draftIssue);
        QCOMPARE(sha256File(fixture.journalPath), draftHash);
        QVERIFY2(issue, "Clean reopen failures need an OriginalFile issue, not a silent skip");
        QVERIFY(writeAll(path, QByteArrayLiteral("source returned; remove only the pending tab\n")));
        const QByteArray sourceHash = sha256File(path);
        QPushButton *remove = nullptr;
        for (QPushButton *button : issue->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("移除标签"))
                remove = button;
        }
        QVERIFY(remove && remove->isVisible());
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_COMPARE(restarted->findChildren<QWidget *>(QStringLiteral("RecoveryIssuePage")).size(), 1);
        QVERIFY(restarted->findChild<QWidget *>(QStringLiteral("RecoveryIssuePage")) == draftIssue);
        QCOMPARE(sha256File(path), sourceHash);
        QCOMPARE(sha256File(fixture.journalPath), draftHash);
        QCOMPARE(sha256File(fixture.sourcePath), draftSourceHash);
        bool retainedDraftRecord = false;
        for (const SessionTab &remaining : manager.loadSession().tabs) {
            QVERIFY(remaining.filePath != path);
            if (remaining.filePath == fixture.sourcePath) {
                retainedDraftRecord = true;
                QCOMPARE(remaining.autosavePath, draftRecord.autosavePath);
                QCOMPARE(remaining.modified, draftRecord.modified);
                QCOMPARE(remaining.encoding, draftRecord.encoding);
                QCOMPARE(remaining.syntax, draftRecord.syntax);
                QCOMPARE(remaining.presentation, draftRecord.presentation);
            }
        }
        QVERIFY(retainedDraftRecord);
    }

    void tc_2b1_clean_duplicate_restores_active_item_identity()
    {
        const QString root = temp_.filePath(QStringLiteral("deduplicated-active-item"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString firstPath = QDir(root).filePath(QStringLiteral("first.txt"));
        const QString otherPath = QDir(root).filePath(QStringLiteral("other.txt"));
        QVERIFY(writeAll(firstPath, QByteArrayLiteral("first source\n")));
        QVERIFY(writeAll(otherPath, QByteArrayLiteral("other source\n")));
        const QByteArray firstHash = sha256File(firstPath);
        const QByteArray otherHash = sha256File(otherPath);
        const QJsonObject first{{QStringLiteral("filePath"), firstPath}};
        const QJsonObject other{{QStringLiteral("filePath"), otherPath}};
        SessionManager manager(dataDir);
        QVERIFY(writeAll(manager.sessionFilePath(), QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 2}, {QStringLiteral("activeTab"), 1},
            {QStringLiteral("tabs"), QJsonArray{first, first, other}}}).toJson()));
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner restarted(new MainWindow, destroyWithoutClose);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted.get()));
        QSignalSpy completed(restarted.get(), &MainWindow::sessionRestoreCompleted);
        restarted->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
        QCOMPARE(sha256File(firstPath), firstHash);
        QCOMPARE(sha256File(otherPath), otherHash);
        QCOMPARE(tabsOf(restarted.get())->count(), 2);
        QCOMPARE(tabsOf(restarted.get())->currentWidget(), findEditor(restarted.get(), firstPath));
        QVERIFY(!findEditor(restarted.get(), firstPath)->isModified());
        QVERIFY(!findEditor(restarted.get(), otherPath)->isModified());
    }

    void tc_r3_original_retry_reuse_close_cancel_keeps_one_responsibility()
    {
        const QString root = temp_.filePath(QStringLiteral("retry-reuse-close-cancel"));
        const QString dataDir = QDir(root).filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString originalPath = QDir(root).filePath(QStringLiteral("returned-original.txt"));
        const QString dirtyPath = QDir(root).filePath(QStringLiteral("cancel-close.txt"));
        const QByteArray originalBytes("returned ordinary source\n");
        const QByteArray dirtyDiskBytes("ordinary source before the user's edit\n");
        QVERIFY(writeAll(dirtyPath, dirtyDiskBytes));
        QVERIFY(!QFileInfo::exists(originalPath));
        SessionManager manager(dataDir);
        const QJsonObject record{{QStringLiteral("filePath"), originalPath},
                                 {QStringLiteral("modified"), false},
                                 {QStringLiteral("secureDocument"), false}};
        QVERIFY(writeAll(manager.sessionFilePath(), QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 2}, {QStringLiteral("activeTab"), 0},
            {QStringLiteral("tabs"), QJsonArray{record}}}).toJson()));

        const bool quitOnLastWindow = qApp->quitOnLastWindowClosed();
        qApp->setQuitOnLastWindowClosed(false);
        const auto restoreQuitPolicy = qScopeGuard([quitOnLastWindow] {
            qApp->setQuitOnLastWindowClosed(quitOnLastWindow);
        });
        using WindowOwner = std::unique_ptr<MainWindow, decltype(&destroyWithoutClose)>;
        WindowOwner window(new MainWindow, destroyWithoutClose);
        window->resize(1050, 700);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QSignalSpy completed(window.get(), &MainWindow::sessionRestoreCompleted);
        window->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        auto *tabs = tabsOf(window.get());
        QPointer<RecoveryIssuePage> issue = window->findChild<RecoveryIssuePage *>();
        QVERIFY(tabs && issue && issue->kind() == RecoveryIssuePage::Kind::OriginalFile);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), issue.data());

        // Restore the source externally, then open it through the actual public
        // route before Retry. Retry must reuse this exact live document.
        QVERIFY(writeAll(originalPath, originalBytes));
        window->openFilesFromExternal({originalPath, dirtyPath});
        QPointer<EditorTab> existing = findEditor(window.get(), originalPath);
        QPointer<EditorTab> dirty = findEditor(window.get(), dirtyPath);
        QVERIFY(existing && dirty && existing != dirty);
        QVERIFY(!existing->isSecureDocument() && !dirty->isSecureDocument());
        QVERIFY(!existing->isL2Mode() && !dirty->isL2Mode());
        const QUuid existingId = existing->documentState().id;
        const QUuid dirtyId = dirty->documentState().id;
        const QByteArray originalHash = sha256File(originalPath);
        const QByteArray dirtyHash = sha256File(dirtyPath);
        const QString unsavedSuffix = QStringLiteral("unsaved edit forces a real Cancel choice\n");
        // QScintilla append() empties the undo buffer. Use its public undoable
        // insertion API so the later real Undo command can restore the savepoint.
        dirty->editor()->insertAt(unsavedSuffix, dirty->editor()->lines() - 1, 0);
        const QString dirtyBody = QString::fromUtf8(dirtyDiskBytes) + unsavedSuffix;
        QCOMPARE(dirty->editor()->text(), dirtyBody);
        QVERIFY(dirty->isModified() && dirty->autosavePath().isEmpty());
        QVERIFY(dirty->editor()->isUndoAvailable() && dirty->canUndoCommand());
        const quint64 dirtyRevision = dirty->contentRevision();
        tabs->setCurrentWidget(issue);
        auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        QVERIFY(retry && retry->isVisible() && retry->isEnabled());
        QCOMPARE(tabs->count(), 3);

        QObject observationScope;
        QSignalSpy committed(window.get(), &MainWindow::shutdownCommitted);
        int cancelPrompts = 0;
        int cancelClicks = 0;
        QStringList unexpectedDialogs;
        QTimer dialogGuard;
        connect(&dialogGuard, &QTimer::timeout, &observationScope, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || !dialog->isModal()
                    || dialog->objectName() == QStringLiteral("sessionCommitProgress"))
                    continue;
                auto *box = qobject_cast<QMessageBox *>(dialog);
                if (box && box->standardButtons()
                               == (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel)
                    && box->text().contains(QFileInfo(dirtyPath).fileName())) {
                    ++cancelPrompts;
                    auto *cancel = box->button(QMessageBox::Cancel);
                    if (cancel && cancel->isVisible() && cancel->isEnabled()) {
                        ++cancelClicks;
                        QTest::mouseClick(cancel, Qt::LeftButton);
                    } else {
                        unexpectedDialogs.append(QStringLiteral("three-button prompt has no usable Cancel"));
                        box->reject();
                    }
                } else {
                    unexpectedDialogs.append(dialog->objectName() + QLatin1Char(':')
                                             + dialog->windowTitle());
                    dialog->reject();
                }
            }
        });
        dialogGuard.start(10);
        bool retryInProgress = false;
        bool interceptedRemoval = false;
        bool closeReturned = false;
        bool closeAccepted = true;
        connect(tabs, &QTabWidget::currentChanged, &observationScope, [&](int) {
            if (!retryInProgress || interceptedRemoval || !issue || tabs->indexOf(issue) >= 0)
                return;
            interceptedRemoval = true;
            // This is the real current-issue removal signal, not an invented
            // coordinator call. A safe implementation may defer public close.
            closeAccepted = window->close();
            closeReturned = true;
        }, Qt::DirectConnection);
        retryInProgress = true;
        QTest::mouseClick(retry, Qt::LeftButton);
        retryInProgress = false;
        QVERIFY2(interceptedRemoval && closeReturned,
                 "Retry did not reach removal of the selected original-file issue");
        QVERIFY(!closeAccepted);
        QTRY_COMPARE_WITH_TIMEOUT(cancelClicks, 1, 5000);
        QCOMPARE(cancelPrompts, 1);
        QCOMPARE(committed.count(), 0);
        QVERIFY2(unexpectedDialogs.isEmpty(), qPrintable(unexpectedDialogs.join(QLatin1Char('\n'))));
        QVERIFY(window->isVisible());
        QVERIFY(existing && dirty);
        QCOMPARE(findEditor(window.get(), originalPath), existing.data());
        QCOMPARE(existing->documentState().id, existingId);
        QCOMPARE(dirty->documentState().id, dirtyId);
        QCOMPARE(dirty->contentRevision(), dirtyRevision);
        QCOMPARE(existing->editor()->text(), QString::fromUtf8(originalBytes));
        QVERIFY(!existing->isModified());
        QCOMPARE(dirty->editor()->text(), dirtyBody);
        QVERIFY(dirty->isModified());
        QCOMPARE(sha256File(originalPath), originalHash);
        QCOMPARE(sha256File(dirtyPath), dirtyHash);
        qInfo() << "RETRY_REUSE_CANCEL_GUARDS_OK" << "cancelClicks" << cancelClicks
                << "visibleTabs" << tabs->count() << "sameDocument" << existingId;

        // Both safe outcomes are allowed: transfer to the existing editor, or
        // retain an accessible issue whose actual Retry can finish afterwards.
        if (issue && tabs->indexOf(issue) >= 0) {
            tabs->setCurrentWidget(issue);
            auto *retryAgain = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
            QVERIFY(retryAgain && retryAgain->isVisible() && retryAgain->isEnabled());
            QTest::mouseClick(retryAgain, Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
        QCOMPARE(findEditor(window.get(), originalPath), existing.data());
        QCOMPARE(existing->documentState().id, existingId);
        QCOMPARE(completed.count(), 1);

        // Undo the single real edit via the product command, then close normally
        // to exercise Coordinator capture/manifest publication without a private
        // save hook, a forced timer, a Discard choice, or changed source bytes.
        tabs->setCurrentWidget(dirty);
        auto *undo = window->findChild<QAction *>(QStringLiteral("edit.undo"));
        QVERIFY(undo && undo->isEnabled());
        undo->trigger();
        QCOMPARE(dirty->editor()->text(), QString::fromUtf8(dirtyDiskBytes));
        QVERIFY(!dirty->isModified());
        QVERIFY(window->close());
        QCOMPARE(committed.count(), 1);
        QCOMPARE(cancelClicks, 1);
        QVERIFY2(unexpectedDialogs.isEmpty(), qPrintable(unexpectedDialogs.join(QLatin1Char('\n'))));
        QCOMPARE(sha256File(originalPath), originalHash);
        QCOMPARE(sha256File(dirtyPath), dirtyHash);
        const SessionData saved = manager.loadSession();
        int originalRecords = 0;
        int dirtyRecords = 0;
        for (const SessionTab &savedRecord : saved.tabs) {
            originalRecords += savedRecord.filePath == originalPath;
            dirtyRecords += savedRecord.filePath == dirtyPath;
        }
        qInfo() << "RETRY_REUSE_CANCEL_MANIFEST" << "total" << saved.tabs.size()
                << "original" << originalRecords << "other" << dirtyRecords;
        QCOMPARE(originalRecords, 1);
        QCOMPARE(dirtyRecords, 1);
        QCOMPARE(saved.tabs.size(), 2);
    }

    void tc_hx_r01_stale_journal_retry_preserves_recovery()
    {
        qInfo("[HX-R01-STALE-RETRY]");
        const QString root = temp_.filePath(QStringLiteral("stale-retry"));
        QVERIFY(QDir().mkpath(root));
        RecoveryFixture fixture = createBinaryRecoveryFixture(root);
        QVERIFY(!fixture.journalPath.isEmpty());
        QVERIFY(DraftStore::isEncryptedDraft(fixture.journalPath));
        QVERIFY(!recoveryTreeContains(fixture.dataDir, QByteArrayLiteral("R1C!")));

        QByteArray changed = fixture.original;
        changed[900] = char(quint8(changed[900]) ^ 0x5a);
        QVERIFY(writeAll(fixture.sourcePath, changed));
        const QByteArray changedHash = sha256File(fixture.sourcePath);
        const QByteArray journalHash = sha256File(fixture.journalPath);
        const QString sessionPath = QDir(fixture.dataDir).filePath(
            QStringLiteral("session.json"));
        QVERIFY(QFileInfo::exists(sessionPath));

        auto *restarted = new MainWindow;
        restarted->resize(1050, 700);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted));
        restarted->deferredRestoreSession();
        QWidget *issue = waitForIssue(restarted);
        QVERIFY2(issue, "旧实现会删除失败 tab，并在队列收尾无条件删除 session.json");
        auto *code = issue->findChild<QLabel *>(QStringLiteral("recoveryIssueCode"));
        auto *retry = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        auto *discard = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueDiscard"));
        QVERIFY(code && code->text() == QStringLiteral("[recovery.source.changed]"));
        QVERIFY(retry && retry->text() == QStringLiteral("重试恢复"));
        QVERIFY(discard && discard->text() == QStringLiteral("放弃恢复"));
        QVERIFY(QFileInfo::exists(sessionPath));
        QCOMPARE(sha256File(fixture.sourcePath), changedHash);
        QCOMPARE(sha256File(fixture.journalPath), journalHash);

        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY(restarted->findChild<QWidget *>(
            QStringLiteral("RecoveryIssuePage")) == issue);
        QCOMPARE(sha256File(fixture.sourcePath), changedHash);
        QCOMPARE(sha256File(fixture.journalPath), journalHash);

        QVERIFY(writeAll(fixture.sourcePath, fixture.original));
        {
            QFile source(fixture.sourcePath);
            QVERIFY(source.open(QIODevice::ReadWrite));
            QVERIFY(source.setFileTime(fixture.sourceModified,
                                       QFileDevice::FileModificationTime));
        }
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(restarted->findChild<QWidget *>(
            QStringLiteral("RecoveryIssuePage")) == nullptr, 10000);
        EditorTab *restored = waitForHex(restarted, fixture.sourcePath);
        QVERIFY(restored);
        QCOMPARE(restored->l2View()->readByteRange(0, fixture.edited.size()),
                 fixture.edited);
        QVERIFY(restored->isModified());
        QCOMPARE(restored->l2View()->selectionStart(), qint64(64));
        QCOMPARE(restored->l2View()->selectionEnd(), qint64(68));
        QCOMPARE(sha256File(fixture.sourcePath),
                 QCryptographicHash::hash(fixture.original,
                                          QCryptographicHash::Sha256));
        QVERIFY(QFileInfo::exists(fixture.journalPath));
        QVERIFY(DraftStore::isEncryptedDraft(fixture.journalPath));
        destroyWithoutClose(restarted);
    }

    void tc_hx_r01_corrupt_journal_discard_is_explicit()
    {
        qInfo("[HX-R01-CORRUPT-DISCARD]");
        const QString root = temp_.filePath(QStringLiteral("corrupt-discard"));
        QVERIFY(QDir().mkpath(root));
        RecoveryFixture fixture = createBinaryRecoveryFixture(root);
        QVERIFY(!fixture.journalPath.isEmpty());
        const QByteArray sourceHash = sha256File(fixture.sourcePath);

        QByteArray corrupt = readAll(fixture.journalPath);
        QVERIFY(corrupt.size() > 80);
        // Keep the recognizable encrypted-draft header, but truncate the
        // terminal record so readChunks must reject the journal fail-closed.
        corrupt.chop(1);
        QVERIFY(writeAll(fixture.journalPath, corrupt));
        const QByteArray corruptHash = sha256File(fixture.journalPath);

        const QString otherPath = QDir(root).filePath(QStringLiteral("keep.txt"));
        const QByteArray otherBytes("keep this ordinary tab\r\n");
        QVERIFY(writeAll(otherPath, otherBytes));
        SessionManager manager(fixture.dataDir);
        SessionData originalSession = manager.loadSession();
        QCOMPARE(originalSession.tabs.size(), 1);
        SessionTab other;
        other.filePath = otherPath;
        other.encoding = QStringLiteral("UTF-8");
        other.presentation = QStringLiteral("text");
        originalSession.tabs.append(other);
        // Preserve the exact issue record while adding an unrelated clean tab.
        QFile sessionFile(manager.sessionFilePath());
        QVERIFY(sessionFile.open(QIODevice::ReadOnly));
        QJsonDocument sessionJson = QJsonDocument::fromJson(sessionFile.readAll());
        sessionFile.close();
        QJsonObject rootObject = sessionJson.object();
        QJsonArray tabs = rootObject.value(QStringLiteral("tabs")).toArray();
        QJsonObject otherObject;
        otherObject.insert(QStringLiteral("filePath"), otherPath);
        otherObject.insert(QStringLiteral("encoding"), QStringLiteral("UTF-8"));
        otherObject.insert(QStringLiteral("presentation"), QStringLiteral("text"));
        tabs.append(otherObject);
        rootObject.insert(QStringLiteral("tabs"), tabs);
        QVERIFY(sessionFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(sessionFile.write(QJsonDocument(rootObject).toJson()) > 0);
        sessionFile.close();
        const SessionData prepared = manager.loadSession();
        QCOMPARE(prepared.tabs.size(), 2);
        QCOMPARE(QFileInfo(prepared.tabs.first().autosavePath).absoluteFilePath(),
                 QFileInfo(fixture.journalPath).absoluteFilePath());

        auto *restarted = new MainWindow;
        restarted->resize(1050, 700);
        restarted->show();
        QVERIFY(QTest::qWaitForWindowExposed(restarted));
        restarted->deferredRestoreSession();
        QWidget *issue = waitForIssue(restarted);
        if (!issue) {
            if (QTabWidget *openTabs = tabsOf(restarted)) {
                qWarning("[HX-R01-CORRUPT-DISCARD] no issue page; tab count=%d",
                         openTabs->count());
                for (int index = 0; index < openTabs->count(); ++index) {
                    QWidget *page = openTabs->widget(index);
                    qWarning("[HX-R01-CORRUPT-DISCARD] tab %d class=%s object=%s",
                             index, page->metaObject()->className(),
                             qPrintable(page->objectName()));
                    if (auto *editor = qobject_cast<EditorTab *>(page))
                        qWarning("[HX-R01-CORRUPT-DISCARD] editor path=%s modified=%d",
                                 qPrintable(editor->filePath()), editor->isModified());
                }
            }
        }
        QVERIFY2(issue, "损坏 .ndraft 不得作为 legacy plaintext 打开或静默丢弃");
        auto *code = issue->findChild<QLabel *>(QStringLiteral("recoveryIssueCode"));
        auto *discard = issue->findChild<QPushButton *>(QStringLiteral("recoveryIssueDiscard"));
        QVERIFY(code && code->text().startsWith(QStringLiteral("[recovery.draft.")));
        QVERIFY(discard);
        QTRY_VERIFY(findEditor(restarted, otherPath));

        QTest::mouseClick(discard, Qt::LeftButton);
        auto *confirm = issue->findChild<QPushButton *>(
            QStringLiteral("recoveryIssueConfirmDiscard"));
        QVERIFY(confirm && confirm->isVisible());
        QVERIFY(QFileInfo::exists(fixture.journalPath));
        QCOMPARE(sha256File(fixture.journalPath), corruptHash);
        QCOMPARE(sha256File(fixture.sourcePath), sourceHash);

        QTest::mouseClick(confirm, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(restarted->findChild<QWidget *>(
            QStringLiteral("RecoveryIssuePage")) == nullptr, 5000);
        QVERIFY(!QFileInfo::exists(fixture.journalPath));
        QCOMPARE(sha256File(fixture.sourcePath), sourceHash);
        QCOMPARE(sha256File(otherPath),
                 QCryptographicHash::hash(otherBytes, QCryptographicHash::Sha256));
        QTRY_VERIFY(QFileInfo::exists(manager.sessionFilePath()));
        const SessionData retained = manager.loadSession();
        QCOMPARE(retained.tabs.size(), 1);
        QCOMPARE(QFileInfo(retained.tabs.first().filePath).absoluteFilePath(),
                 QFileInfo(otherPath).absoluteFilePath());
        destroyWithoutClose(restarted);
    }
};

QTEST_MAIN(RecoveryIssueSmoke)
#include "smoke_recovery_issue.moc"
