#include <QLabel>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QAction>
#include <QtGui/QTextCursor>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>

#include "DraftStore.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "SessionManager.h"
#include "VirtualFileView.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Aclapi.h>
#include <windows.h>
#endif

namespace {

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

EditorTab *currentEditorTab(MainWindow *window)
{
    auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
    return tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
}

int tabCount(MainWindow *window)
{
    auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
    return tabs ? tabs->count() : 0;
}

EditorTab *waitForHexTab(MainWindow *window, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        EditorTab *tab = currentEditorTab(window);
        if (tabCount(window) == 1 && tab && tab->isBinaryDocument()
            && tab->l2View() && tab->l2View()->isOpen()
            && tab->viewModeL2() == ViewMode::Hex) {
            return tab;
        }
        QTest::qWait(10);
    }
    return nullptr;
}

void destroyWindowWithoutClose(MainWindow *window)
{
    if (!window)
        return;
    if (auto *tabs = window->findChild<QTabWidget *>()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(tabs->widget(i)))
                tab->shutdownL2BeforeClose();
        }
    }
    delete window;
    QApplication::processEvents(QEventLoop::AllEvents, 100);
}

bool directoryContainsPlaintext(const QString &root, const QByteArray &needle,
                                QString *matchedPath)
{
    if (needle.size() < 8)
        return false;
    QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        if (readAll(path).contains(needle)) {
            if (matchedPath)
                *matchedPath = path;
            return true;
        }
    }
    return false;
}

QStringList siblingFilesExcept(const QString &directory,
                               const QString &allowedFileName)
{
    QStringList unexpected;
    const QFileInfoList files = QDir(directory).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) {
        if (file.fileName() != allowedFileName)
            unexpected.append(file.fileName());
    }
    return unexpected;
}

struct SaveErrorCapture {
    bool shown = false;
    QString title;
    QString text;
    QString informativeText;
    QString detailedText;
};

void armSaveErrorCapture(QWidget *owner, SaveErrorCapture *capture)
{
    QTimer::singleShot(100, owner, [owner, capture]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        // A plain save failure is a non-blocking notice above the document.
        auto *notice = owner->findChild<QLabel *>(QStringLiteral("noticeText"));
        if (!box && capture && notice && notice->isVisible()) {
            capture->shown = true;
            capture->text = notice->text();
            capture->detailedText = notice->toolTip();
            return;
        }
        if (!box || !capture)
            return;
        capture->shown = true;
        capture->title = box->windowTitle();
        capture->text = box->text();
        capture->informativeText = box->informativeText();
        capture->detailedText = box->detailedText();
        if (auto *ok = box->button(QMessageBox::Ok))
            QTest::mouseClick(ok, Qt::LeftButton);
        else
            box->accept();
    });
}

#ifdef Q_OS_WIN
class DirectoryCandidateCreateDeny final {
public:
    explicit DirectoryCandidateCreateDeny(const QString &directory)
        : nativePath_(QDir::toNativeSeparators(
              QFileInfo(directory).absoluteFilePath()).toStdWString())
    {
    }

    ~DirectoryCandidateCreateDeny() { restore(); }

    bool apply(QString *error)
    {
        if (applied_)
            return true;
        DWORD status = ::GetNamedSecurityInfoW(
            nativePath_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &originalDacl_, nullptr, &securityDescriptor_);
        if (status != ERROR_SUCCESS)
            return fail(error, QStringLiteral("GetNamedSecurityInfoW"), status);

        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return fail(error, QStringLiteral("OpenProcessToken"), ::GetLastError());
        DWORD tokenBytes = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
        QByteArray tokenStorage(int(tokenBytes), Qt::Uninitialized);
        if (!::GetTokenInformation(token, TokenUser, tokenStorage.data(),
                                   tokenBytes, &tokenBytes)) {
            const DWORD lastError = ::GetLastError();
            ::CloseHandle(token);
            return fail(error, QStringLiteral("GetTokenInformation"), lastError);
        }
        ::CloseHandle(token);

        const auto *tokenUser = reinterpret_cast<const TOKEN_USER *>(
            tokenStorage.constData());
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        ::BuildTrusteeWithSidW(&deny.Trustee, tokenUser->User.Sid);
        status = ::SetEntriesInAclW(1, &deny, originalDacl_, &deniedDacl_);
        if (status != ERROR_SUCCESS)
            return fail(error, QStringLiteral("SetEntriesInAclW"), status);
        status = ::SetNamedSecurityInfoW(
            nativePath_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, deniedDacl_, nullptr);
        if (status != ERROR_SUCCESS)
            return fail(error, QStringLiteral("SetNamedSecurityInfoW"), status);
        applied_ = true;
        return true;
    }

    bool restore(QString *error = nullptr)
    {
        if (!securityDescriptor_)
            return true;
        bool restored = true;
        if (applied_) {
            const DWORD status = ::SetNamedSecurityInfoW(
                nativePath_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                nullptr, nullptr, originalDacl_, nullptr);
            restored = status == ERROR_SUCCESS;
            if (!restored)
                fail(error, QStringLiteral("restore SetNamedSecurityInfoW"), status);
        }
        if (deniedDacl_)
            ::LocalFree(deniedDacl_);
        ::LocalFree(securityDescriptor_);
        deniedDacl_ = nullptr;
        securityDescriptor_ = nullptr;
        originalDacl_ = nullptr;
        applied_ = false;
        return restored;
    }

private:
    static bool fail(QString *error, const QString &operation, DWORD code)
    {
        if (error)
            *error = QStringLiteral("%1 failed: %2").arg(operation).arg(code);
        return false;
    }

    std::wstring nativePath_;
    PSECURITY_DESCRIPTOR securityDescriptor_ = nullptr;
    PACL originalDacl_ = nullptr;
    PACL deniedDacl_ = nullptr;
    bool applied_ = false;
};

class NoDeleteSharingFileLock final {
public:
    ~NoDeleteSharingFileLock()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle_);
    }

    bool open(const QString &path, QString *error)
    {
        const std::wstring nativePath = QDir::toNativeSeparators(
            QFileInfo(path).absoluteFilePath()).toStdWString();
        handle_ = ::CreateFileW(nativePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE)
            return true;
        if (error)
            *error = QStringLiteral("CreateFileW failed: %1").arg(::GetLastError());
        return false;
    }

    void close()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

enum class RecoveryPrimitive {
    Nibble,
    Ascii,
    Insert,
    Delete,
    Cut,
    Paste,
    SelectionReplace,
};

struct RecoveryCase {
    const char *id;
    RecoveryPrimitive primitive;
    bool insertMode;
    bool asciiSurface;
    bool reverseSelection;
};

} // namespace

class HexRecoverySmoke final : public QObject {
    Q_OBJECT

    QTemporaryDir temp_;
    QByteArray originalDataDir_;
    bool originalDataDirWasSet_ = false;

    void runRecoveryCase(const RecoveryCase &contract)
    {
        qInfo().noquote() << "[HX-R01]" << contract.id;
        const QString caseRoot = temp_.filePath(QString::fromLatin1(contract.id));
        const QString dataDir = QDir(caseRoot).filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        QByteArray original(128, '\0');
        for (int i = 0; i < original.size(); ++i)
            original[i] = char((i * 37 + 11) & 0xff);
        original[0] = '\0';
        const QString sourcePath = QDir(caseRoot).filePath(QStringLiteral("source.bin"));
        QVERIFY(QDir().mkpath(caseRoot));
        QVERIFY(writeAll(sourcePath, original));
        const QByteArray sourceHash = sha256(original);

        auto *first = new MainWindow;
        first->resize(1100, 720);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first));
        first->openFilesFromExternal({sourcePath});
        EditorTab *edited = waitForHexTab(first);
        QVERIFY2(edited, contract.id);
        auto *ascii = edited->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(edited));

        QWidget *surface = contract.asciiSurface
            ? static_cast<QWidget *>(ascii)
            : static_cast<QWidget *>(edited->editor());
        surface->setFocus(Qt::OtherFocusReason);
        if (contract.insertMode && edited->hexOverwriteMode())
            QTest::keyClick(surface, Qt::Key_Insert);
        QCOMPARE(!edited->hexOverwriteMode(), contract.insertMode);

        QByteArray expected = original;
        QByteArray plaintextCanary;
        switch (contract.primitive) {
        case RecoveryPrimitive::Nibble:
            edited->gotoByteOffset(10);
            edited->editor()->setFocus(Qt::OtherFocusReason);
            QTest::keyClicks(edited->editor(), QStringLiteral("A5"));
            expected[10] = char(0xa5);
            break;
        case RecoveryPrimitive::Ascii: {
            QTextCursor caret(ascii->document());
            caret.setPosition(12);
            ascii->setTextCursor(caret);
            ascii->setFocus(Qt::OtherFocusReason);
            QTest::keyClicks(ascii, QStringLiteral("Z"));
            expected[12] = 'Z';
            break;
        }
        case RecoveryPrimitive::Insert:
            QVERIFY(!edited->hexOverwriteMode());
            edited->gotoByteOffset(8);
            edited->editor()->setFocus(Qt::OtherFocusReason);
            QTest::keyClicks(edited->editor(), QStringLiteral("C3"));
            expected.insert(8, char(0xc3));
            break;
        case RecoveryPrimitive::Delete: {
            QTextCursor caret(ascii->document());
            caret.setPosition(14);
            ascii->setTextCursor(caret);
            ascii->setFocus(Qt::OtherFocusReason);
            QTest::keyClick(ascii, Qt::Key_Delete);
            expected.remove(14, 1);
            break;
        }
        case RecoveryPrimitive::Cut:
        {
            QTextCursor selection(ascii->document());
            selection.setPosition(16);
            selection.setPosition(20, QTextCursor::KeepAnchor);
            ascii->setTextCursor(selection);
            ascii->setFocus(Qt::OtherFocusReason);
            QTest::keyClick(ascii, Qt::Key_X, Qt::ControlModifier);
            expected.remove(16, 4);
            break;
        }
        case RecoveryPrimitive::Paste: {
            const qint64 start = 18;
            const qint64 end = 21;
            if (contract.reverseSelection) {
                const int first = int(start * 3);
                const int last = int(end * 3 - 1);
                edited->editor()->setSelection(0, last, 0, first);
            } else {
                edited->gotoByteOffset(start, int(end - start));
            }
            edited->editor()->setFocus(Qt::OtherFocusReason);
            plaintextCanary = QByteArray::fromHex("DEADBEEF9AC71042");
            QGuiApplication::clipboard()->setText(
                QStringLiteral("DE AD BE EF 9A C7 10 42"));
            QTest::keyClick(edited->editor(), Qt::Key_V, Qt::ControlModifier);
            expected.replace(start, end - start, plaintextCanary);
            break;
        }
        case RecoveryPrimitive::SelectionReplace:
            edited->gotoByteOffset(22, 4);
            edited->editor()->setFocus(Qt::OtherFocusReason);
            QTest::keyClicks(edited->editor(), QStringLiteral("5A"));
            expected.replace(22, 4, QByteArray(1, char(0x5a)));
            break;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(edited->l2View()->readByteRange(
                     0, edited->l2View()->virtualFileSize()), expected);
        QVERIFY(edited->isModified());

        // The checkpoint view is deliberately independent of each primitive's
        // resulting caret. Recovery must preserve this exact byte identity.
        const qint64 checkpointStart = 40;
        const qint64 checkpointEnd = 45;
        edited->gotoByteOffset(checkpointStart,
                               int(checkpointEnd - checkpointStart));
        QCOMPARE(edited->currentByteOffset(), checkpointEnd);

        SessionManager session(dataDir);
        QVERIFY2(session.saveSession({edited}, 0, true), contract.id);
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.tabs.size(), 1);
        QCOMPARE(stored.tabs.first().presentation, QStringLiteral("hex"));
        QCOMPARE(stored.tabs.first().selectionStartByte, checkpointStart);
        QCOMPARE(stored.tabs.first().selectionEndByte, checkpointEnd);
        QCOMPARE(stored.tabs.first().hexInsertMode, contract.insertMode);

        const QString journalPath = stored.tabs.first().autosavePath;
        DraftStore::SnapshotInfo info;
        QString inspectError;
        QVERIFY2(DraftStore::inspect(journalPath, &info, &inspectError),
                 qPrintable(inspectError));
        QCOMPARE(info.kind, DraftStore::PayloadKind::BinaryPatchJournal);
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));
        const QByteArray ciphertext = readAll(journalPath);
        QVERIFY(!ciphertext.contains("NotepadSafe.BinaryPatchJournal.v1"));
        if (!plaintextCanary.isEmpty())
            QVERIFY(!ciphertext.contains(plaintextCanary));
        QString plaintextPath;
        QVERIFY2(!directoryContainsPlaintext(
                     dataDir, QByteArrayLiteral("NotepadSafe.BinaryPatchJournal.v1"),
                     &plaintextPath), qPrintable(plaintextPath));
        if (!plaintextCanary.isEmpty()) {
            QVERIFY2(!directoryContainsPlaintext(dataDir, plaintextCanary,
                                                 &plaintextPath),
                     qPrintable(plaintextPath));
        }
        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);

        destroyWindowWithoutClose(first);

        auto *restoredWindow = new MainWindow;
        restoredWindow->resize(1100, 720);
        restoredWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restoredWindow));
        restoredWindow->deferredRestoreSession();
        EditorTab *restored = waitForHexTab(restoredWindow);
        QVERIFY2(restored, contract.id);
        QCOMPARE(restored->l2View()->readByteRange(
                     0, restored->l2View()->virtualFileSize()), expected);
        QVERIFY(restored->isModified());
        QCOMPARE(restored->l2View()->selectionStart(), checkpointStart);
        QCOMPARE(restored->l2View()->selectionEnd(), checkpointEnd);
        QCOMPARE(restored->currentByteOffset(), checkpointEnd);
        QCOMPARE(!restored->hexOverwriteMode(), contract.insertMode);
        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        destroyWindowWithoutClose(restoredWindow);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temp_.isValid());
        originalDataDirWasSet_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        originalDataDir_ = qgetenv("NOTEPADSAFE_DATADIR");
    }

    void cleanupTestCase()
    {
        if (originalDataDirWasSet_)
            qputenv("NOTEPADSAFE_DATADIR", originalDataDir_);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void tc_hx_r01_nibble_ovr_hex_none()
    {
        runRecoveryCase({"HX-R01-NIBBLE-OVR-HEX-NONE",
                         RecoveryPrimitive::Nibble, false, false, false});
    }

    void tc_hx_r01_ascii_ovr_ascii_none()
    {
        runRecoveryCase({"HX-R01-ASCII-OVR-ASCII-NONE",
                         RecoveryPrimitive::Ascii, false, true, false});
    }

    void tc_hx_r01_insert_ins_hex_none()
    {
        runRecoveryCase({"HX-R01-INSERT-INS-HEX-NONE",
                         RecoveryPrimitive::Insert, true, false, false});
    }

    void tc_hx_r01_delete_ovr_ascii_none()
    {
        runRecoveryCase({"HX-R01-DELETE-OVR-ASCII-NONE",
                         RecoveryPrimitive::Delete, false, true, false});
    }

    void tc_hx_r01_cut_ins_ascii_forward()
    {
        runRecoveryCase({"HX-R01-CUT-INS-ASCII-FORWARD",
                         RecoveryPrimitive::Cut, true, true, false});
    }

    void tc_hx_r01_paste_ovr_hex_reverse()
    {
        runRecoveryCase({"HX-R01-PASTE-OVR-HEX-REVERSE",
                         RecoveryPrimitive::Paste, false, false, true});
    }

    void tc_hx_r01_selection_replace_ins_hex_forward()
    {
        runRecoveryCase({"HX-R01-SELECTION-REPLACE-INS-HEX-FORWARD",
                         RecoveryPrimitive::SelectionReplace, true, false, false});
    }

    void tc_hx_navigation_ctrl_home_end_relinquishes_source_selection()
    {
        const QString caseRoot = temp_.filePath(
            QStringLiteral("HX-R01-NAV-COLLAPSE"));
        const QString dataDir = QDir(caseRoot).filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        QByteArray original(64, '\0');
        for (int i = 0; i < original.size(); ++i)
            original[i] = char((i * 31 + 7) & 0xff);
        const QString sourcePath = QDir(caseRoot).filePath(
            QStringLiteral("ctrl-home-end.bin"));
        QVERIFY(writeAll(sourcePath, original));
        const QByteArray sourceHash = sha256(original);

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({sourcePath});
        EditorTab *tab = waitForHexTab(window);
        QVERIFY(tab);

        // Ctrl+Home from a source-authoritative selection must collapse it at
        // BOF. OVR input then changes exactly byte zero, not the stale range.
        tab->gotoByteOffset(8, 4);
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(tab->editor(), Qt::Key_Home, Qt::ControlModifier);
        QTest::keyClicks(tab->editor(), QStringLiteral("FF"));
        QByteArray expected = original;
        expected[0] = char(0xff);
        QCOMPARE(tab->l2View()->readByteRange(
                     0, tab->l2View()->virtualFileSize()), expected);

        // Ctrl+End has the same collapse contract. In INS it appends one byte
        // at EOF instead of replacing the previous source selection.
        tab->gotoByteOffset(16, 4);
        QTest::keyClick(tab->editor(), Qt::Key_Insert);
        QVERIFY(!tab->hexOverwriteMode());
        QTest::keyClick(tab->editor(), Qt::Key_End, Qt::ControlModifier);
        QTest::keyClicks(tab->editor(), QStringLiteral("EE"));
        expected.append(char(0xee));
        QCOMPARE(tab->l2View()->readByteRange(
                     0, tab->l2View()->virtualFileSize()), expected);
        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        destroyWindowWithoutClose(window);
    }

    void tc_hx_p03_external_change_save_conflict_preserves_binary_dirty_recovery()
    {
        const QString caseRoot = temp_.filePath(QStringLiteral("HX-P03-EXTERNAL-CONFLICT"));
        const QString dataDir = QDir(caseRoot).filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        QByteArray original(192, '\0');
        for (int i = 0; i < original.size(); ++i)
            original[i] = char((i * 19 + 7) & 0xff);
        original[0] = '\0';
        const QString sourcePath = QDir(caseRoot).filePath(QStringLiteral("conflict.bin"));
        QVERIFY(writeAll(sourcePath, original));

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({sourcePath});
        EditorTab *tab = waitForHexTab(window);
        QVERIFY(tab);

        const QByteArray localCanary = QByteArray::fromHex(
            "4C4F43414C2D44495254592D4259544553");
        tab->gotoByteOffset(24, 4);
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QGuiApplication::clipboard()->setText(
            QStringLiteral("4C 4F 43 41 4C 2D 44 49 52 54 59 2D 42 59 54 45 53"));
        QTest::keyClick(tab->editor(), Qt::Key_V, Qt::ControlModifier);
        QByteArray localExpected = original;
        localExpected.replace(24, 4, localCanary);
        QCOMPARE(tab->l2View()->readByteRange(
                     0, tab->l2View()->virtualFileSize()), localExpected);
        QVERIFY(tab->isModified());

        // Establish the encrypted recovery point before another writer changes
        // the authoritative disk revision. A stale journal must not be silently
        // replayed against that different revision.
        SessionManager session(dataDir);
        QVERIFY(session.saveSession({tab}, 0, true));
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.tabs.size(), 1);
        const QString journalPath = stored.tabs.first().autosavePath;
        QVERIFY(QFileInfo::exists(journalPath));
        DraftStore::SnapshotInfo info;
        QString inspectError;
        QVERIFY2(DraftStore::inspect(journalPath, &info, &inspectError),
                 qPrintable(inspectError));
        QCOMPARE(info.kind, DraftStore::PayloadKind::BinaryPatchJournal);

        QByteArray external(original.size(), '\0');
        for (int i = 0; i < external.size(); ++i)
            external[i] = char((i * 29 + 3) & 0xff);
        external.replace(8, 20, QByteArrayLiteral("EXTERNAL-WRITER-WINS"));
        // B1 owns a live read-only mapping, so Windows correctly refuses an
        // atomic replace while that handle is open. A second program can still
        // modify the existing file in place; exercise that real conflict path.
        QTest::qWait(50);
        QFile externalWriter(sourcePath);
        QVERIFY(externalWriter.open(QIODevice::ReadWrite));
        QVERIFY(externalWriter.seek(0));
        QCOMPARE(externalWriter.write(external), qint64(external.size()));
        QVERIFY(externalWriter.flush());
        externalWriter.close();
        QCOMPARE(readAll(sourcePath), external);
        QCOMPARE(tab->externalFileState(), EditorTab::ExternalFileState::Changed);

        QAction *saveAction = window->findChild<QAction *>(QStringLiteral("file.save"));
        QVERIFY(saveAction && saveAction->isEnabled());
        bool conflictShown = false;
        QTimer::singleShot(100, window, [&conflictShown]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (box->windowTitle() != QStringLiteral("文件已在磁盘上更改"))
                return;
            for (QPushButton *button : box->findChildren<QPushButton *>()) {
                if (button->text().contains(QStringLiteral("取消"))) {
                    conflictShown = true;
                    QTest::mouseClick(button, Qt::LeftButton);
                    return;
                }
            }
        });
        saveAction->trigger();
        QVERIFY2(conflictShown, "HX-P03 did not expose the binary save-conflict journey");

        QCOMPARE(readAll(sourcePath), external);
        QCOMPARE(tab->l2View()->readByteRange(24, 24 + localCanary.size()),
                 localCanary);
        QVERIFY(tab->isModified());
        QVERIFY(QFileInfo::exists(journalPath));
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));
        QVERIFY(!readAll(journalPath).contains(localCanary));
        QCOMPARE(tab->externalFileState(), EditorTab::ExternalFileState::Changed);
        destroyWindowWithoutClose(window);
    }

    void tc_hx_p03_candidate_create_failure_keeps_source_dirty_and_recoverable()
    {
#ifndef Q_OS_WIN
        QSKIP("HX-P03 candidate-create fault uses a real Windows directory DACL");
#else
        const QString caseId = QStringLiteral("HX-P03-CANDIDATE-CREATE-FAIL");
        const QString caseRoot = temp_.filePath(caseId);
        const QString dataDir = QDir(caseRoot).filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        QByteArray original(4096, '\0');
        for (int i = 0; i < original.size(); ++i)
            original[i] = char((i * 41 + 13) & 0xff);
        original[0] = '\0';
        const QString sourcePath = QDir(caseRoot).filePath(
            QStringLiteral("candidate-create.bin"));
        QVERIFY(writeAll(sourcePath, original));
        const QByteArray sourceHash = sha256(original);

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({sourcePath});
        EditorTab *tab = waitForHexTab(window);
        QVERIFY(tab);

        const QByteArray localCanary = QByteArray::fromHex(
            "C001D00D4C4F43414C2D4845582D45444954");
        tab->gotoByteOffset(64, 5);
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QGuiApplication::clipboard()->setText(
            QStringLiteral("C0 01 D0 0D 4C 4F 43 41 4C 2D 48 45 58 2D 45 44 49 54"));
        QTest::keyClick(tab->editor(), Qt::Key_V, Qt::ControlModifier);
        QByteArray expected = original;
        expected.replace(64, 5, localCanary);
        QCOMPARE(tab->l2View()->readByteRange(0, tab->l2View()->virtualFileSize()),
                 expected);
        QVERIFY(tab->isModified());
        tab->gotoByteOffset(80, 6);

        SessionManager session(dataDir);
        QVERIFY(session.saveSession({tab}, 0, true));
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.tabs.size(), 1);
        const QString journalPath = stored.tabs.first().autosavePath;
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));

        DirectoryCandidateCreateDeny denyCreate(caseRoot);
        QString aclError;
        QVERIFY2(denyCreate.apply(&aclError), qPrintable(aclError));
        QCOMPARE(tab->externalFileState(), EditorTab::ExternalFileState::Unchanged);
        QFile deniedProbe(QDir(caseRoot).filePath(QStringLiteral("denied-probe.tmp")));
        QVERIFY2(!deniedProbe.open(QIODevice::WriteOnly),
                 "directory fault did not deny a real sibling candidate create");

        QAction *saveAction = window->findChild<QAction *>(QStringLiteral("file.save"));
        QVERIFY(saveAction && saveAction->isEnabled());
        SaveErrorCapture capture;
        armSaveErrorCapture(window, &capture);
        saveAction->trigger();
        QTRY_VERIFY2_WITH_TIMEOUT(capture.shown, "candidate-create failure was not user-visible", 2000);
        const QString visibleDiagnostic = capture.text + capture.informativeText
            + capture.detailedText;
        QVERIFY2(visibleDiagnostic.contains(
                     QStringLiteral("[plain.save.candidate.open]")),
                 qPrintable(visibleDiagnostic));

        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        QCOMPARE(tab->l2View()->readByteRange(0, tab->l2View()->virtualFileSize()),
                 expected);
        QVERIFY(tab->isModified());
        QVERIFY(QFileInfo::exists(journalPath));
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));
        QVERIFY(!readAll(journalPath).contains(localCanary));
        QString plaintextPath;
        QVERIFY2(!directoryContainsPlaintext(dataDir, localCanary, &plaintextPath),
                 qPrintable(plaintextPath));
        QCOMPARE(siblingFilesExcept(caseRoot, QFileInfo(sourcePath).fileName()),
                 QStringList());
        qInfo().noquote() << "[HX-P03]" << caseId << "source-sha256"
                          << sourceHash.toHex();

        QVERIFY2(denyCreate.restore(&aclError), qPrintable(aclError));
        destroyWindowWithoutClose(window);

        auto *restoredWindow = new MainWindow;
        restoredWindow->resize(1100, 720);
        restoredWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restoredWindow));
        restoredWindow->deferredRestoreSession();
        EditorTab *restored = waitForHexTab(restoredWindow);
        QVERIFY(restored);
        QCOMPARE(restored->l2View()->readByteRange(
                     0, restored->l2View()->virtualFileSize()), expected);
        QVERIFY(restored->isModified());
        QCOMPARE(restored->l2View()->selectionStart(), qint64(80));
        QCOMPARE(restored->l2View()->selectionEnd(), qint64(86));
        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        destroyWindowWithoutClose(restoredWindow);
#endif
    }

    void tc_hx_p03_commit_replace_failure_keeps_source_dirty_and_recoverable()
    {
#ifndef Q_OS_WIN
        QSKIP("HX-P03 commit fault uses a real Windows no-delete-sharing handle");
#else
        const QString caseId = QStringLiteral("HX-P03-COMMIT-REPLACE-FAIL");
        const QString caseRoot = temp_.filePath(caseId);
        const QString dataDir = QDir(caseRoot).filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(dataDir));
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        QByteArray original(4096, '\0');
        for (int i = 0; i < original.size(); ++i)
            original[i] = char((i * 47 + 17) & 0xff);
        original[0] = '\0';
        const QString sourcePath = QDir(caseRoot).filePath(
            QStringLiteral("commit-replace.bin"));
        QVERIFY(writeAll(sourcePath, original));
        const QByteArray sourceHash = sha256(original);

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({sourcePath});
        EditorTab *tab = waitForHexTab(window);
        QVERIFY(tab);

        const QByteArray localCanary = QByteArray::fromHex(
            "DEC0ADDE434F4D4D49542D4845582D45444954");
        tab->gotoByteOffset(96, 7);
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QGuiApplication::clipboard()->setText(
            QStringLiteral("DE C0 AD DE 43 4F 4D 4D 49 54 2D 48 45 58 2D 45 44 49 54"));
        QTest::keyClick(tab->editor(), Qt::Key_V, Qt::ControlModifier);
        QByteArray expected = original;
        expected.replace(96, 7, localCanary);
        QCOMPARE(tab->l2View()->readByteRange(0, tab->l2View()->virtualFileSize()),
                 expected);
        QVERIFY(tab->isModified());
        tab->gotoByteOffset(120, 5);

        SessionManager session(dataDir);
        QVERIFY(session.saveSession({tab}, 0, true));
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.tabs.size(), 1);
        const QString journalPath = stored.tabs.first().autosavePath;
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));

        NoDeleteSharingFileLock replaceLock;
        QString lockError;
        QVERIFY2(replaceLock.open(sourcePath, &lockError), qPrintable(lockError));
        QCOMPARE(tab->externalFileState(), EditorTab::ExternalFileState::Unchanged);
        const QString createProofPath = QDir(caseRoot).filePath(
            QStringLiteral("candidate-write-proof.tmp"));
        QVERIFY(writeAll(createProofPath, expected));
        QVERIFY(QFile::remove(createProofPath));

        QAction *saveAction = window->findChild<QAction *>(QStringLiteral("file.save"));
        QVERIFY(saveAction && saveAction->isEnabled());
        SaveErrorCapture capture;
        armSaveErrorCapture(window, &capture);
        saveAction->trigger();
        QTRY_VERIFY2_WITH_TIMEOUT(capture.shown, "commit/replace failure was not user-visible", 2000);
        const QString visibleDiagnostic = capture.text + capture.informativeText
            + capture.detailedText;
        QVERIFY2(visibleDiagnostic.contains(QStringLiteral("[plain.save.commit]")),
                 qPrintable(visibleDiagnostic));

        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        QCOMPARE(tab->l2View()->readByteRange(0, tab->l2View()->virtualFileSize()),
                 expected);
        QVERIFY(tab->isModified());
        QVERIFY(QFileInfo::exists(journalPath));
        QVERIFY(DraftStore::isEncryptedDraft(journalPath));
        QVERIFY(!readAll(journalPath).contains(localCanary));
        QString plaintextPath;
        QVERIFY2(!directoryContainsPlaintext(dataDir, localCanary, &plaintextPath),
                 qPrintable(plaintextPath));
        QCOMPARE(siblingFilesExcept(caseRoot, QFileInfo(sourcePath).fileName()),
                 QStringList());
        qInfo().noquote() << "[HX-P03]" << caseId << "source-sha256"
                          << sourceHash.toHex();

        replaceLock.close();
        destroyWindowWithoutClose(window);

        auto *restoredWindow = new MainWindow;
        restoredWindow->resize(1100, 720);
        restoredWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restoredWindow));
        restoredWindow->deferredRestoreSession();
        EditorTab *restored = waitForHexTab(restoredWindow);
        QVERIFY(restored);
        QCOMPARE(restored->l2View()->readByteRange(
                     0, restored->l2View()->virtualFileSize()), expected);
        QVERIFY(restored->isModified());
        QCOMPARE(restored->l2View()->selectionStart(), qint64(120));
        QCOMPARE(restored->l2View()->selectionEnd(), qint64(125));
        QCOMPARE(sha256(readAll(sourcePath)), sourceHash);
        destroyWindowWithoutClose(restoredWindow);
#endif
    }
};

int main(int argc, char **argv)
{
    qputenv("QTEST_FUNCTION_TIMEOUT", QByteArrayLiteral("120000"));
    QApplication app(argc, argv);
    HexRecoverySmoke suite;
    return QTest::qExec(&suite, argc, argv);
}

#include "smoke_hex_recovery.moc"
