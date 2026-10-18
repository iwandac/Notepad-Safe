#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPixmap>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeView>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <Qsci/qsciscintilla.h>
#include "AppComposition.h"
#include "AsyncTaskManager.h"
#include "DiskFileVersion.h"
#include "EditorTab.h"
#include "VirtualFileView.h"
#include "FindOverlay.h"
#include "MainWindow.h"
#include "OrdinaryReadSource.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "WindowedReadOnlyProvider.h"

namespace {
constexpr qint64 MiB = 1024 * 1024;
constexpr int ObserveTimeout = 5000;

QByteArray digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}

bool taskEnded(const AsyncTask &task)
{
    return task.id > 0 && task.state != AsyncTaskState::Queued
        && task.state != AsyncTaskState::Running;
}

// Unexpected modal dialogs are evidence of failure, but cannot hang this test.
class ModalGuard final : public QObject {
public:
    ModalGuard() { qApp->installEventFilter(this); }
    ~ModalGuard() override { qApp->removeEventFilter(this); }
    QStringList shown;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(watched);
            if (dialog && dialog->isModal()) {
                shown.append(dialog->windowTitle());
                const QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, this, [guard] { if (guard) guard->reject(); });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

class ExclusiveSourceLock final {
public:
    ~ExclusiveSourceLock() { release(); }
    bool acquire(const QString &path)
    {
#ifdef Q_OS_WIN
        handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                              0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
#else
        Q_UNUSED(path);
        return false;
#endif
    }
    void release()
    {
#ifdef Q_OS_WIN
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#endif
    }
private:
#ifdef Q_OS_WIN
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

// Reads actual projection state; no Tail controller, private cache or test hook.
struct ReadingPosition {
    QString text;
    long firstLine = 0;
    long xOffset = 0;
    long selectionStart = 0;
    long selectionEnd = 0;
    qint64 sourceStart = 0;
    qint64 sourceEnd = 0;
    qint64 topByte = 0;

    static ReadingPosition capture(WindowedReadOnlyProvider *provider)
    {
        auto *editor = provider->editor();
        return {editor->text(),
                editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE),
                editor->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET),
                editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART),
                editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND),
                provider->selectionStartOffset(), provider->selectionEndOffset(),
                provider->visibleTopOffset()};
    }
    bool equals(WindowedReadOnlyProvider *provider) const
    {
        const auto current = capture(provider);
        return text == current.text && firstLine == current.firstLine
            && xOffset == current.xOffset && selectionStart == current.selectionStart
            && selectionEnd == current.selectionEnd && sourceStart == current.sourceStart
            && sourceEnd == current.sourceEnd && topByte == current.topByte;
    }
    // A source change can refresh the view in two steps (remap, then the
    // end-of-file scroll clamp); read the position once it stops moving.
    static ReadingPosition settled(WindowedReadOnlyProvider *provider)
    {
        ReadingPosition last = capture(provider);
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 3000) {
            QTest::qWait(250);
            if (last.equals(provider))
                return last;
            last = capture(provider);
        }
        return last;
    }
    // Names every field that moved, for failure messages.
    QString differenceFrom(WindowedReadOnlyProvider *provider) const
    {
        const auto now = capture(provider);
        QStringList moved;
        if (text != now.text) moved << QStringLiteral("text(%1->%2 chars)").arg(text.size()).arg(now.text.size());
        if (firstLine != now.firstLine) moved << QStringLiteral("firstLine %1->%2").arg(firstLine).arg(now.firstLine);
        if (xOffset != now.xOffset) moved << QStringLiteral("xOffset %1->%2").arg(xOffset).arg(now.xOffset);
        if (selectionStart != now.selectionStart || selectionEnd != now.selectionEnd)
            moved << QStringLiteral("selection %1-%2->%3-%4").arg(selectionStart).arg(selectionEnd)
                         .arg(now.selectionStart).arg(now.selectionEnd);
        if (sourceStart != now.sourceStart || sourceEnd != now.sourceEnd)
            moved << QStringLiteral("source %1-%2->%3-%4").arg(sourceStart).arg(sourceEnd)
                         .arg(now.sourceStart).arg(now.sourceEnd);
        if (topByte != now.topByte) moved << QStringLiteral("topByte %1->%2").arg(topByte).arg(now.topByte);
        return moved.join(QStringLiteral("; "));
    }
};

class TailDocument final {
public:
    ~TailDocument()
    {
        QObject::disconnect(added_);
        auto &manager = AsyncTaskManager::instance();
        for (qint64 id : std::as_const(tasks)) manager.cancel(id);
        if (!QTest::qWaitFor([&] {
                for (qint64 id : std::as_const(tasks))
                    if (!taskEnded(manager.taskById(id))) return false;
                return true;
            }, 5000)) qWarning("Tail fixture did not drain its owned document tasks");
        if (window) {
            if (auto *tabs = window->findChild<QTabWidget *>()) {
                while (tabs->count()) {
                    QWidget *page = tabs->widget(0);
                    if (auto *editor = qobject_cast<EditorTab *>(page))
                        editor->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
            window.reset();
        }
        composition.reset();
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", oldProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool start(bool openSource = true)
    {
        if (!temporary.isValid()) return fail(temporary.errorString());
        const QString profile = temporary.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile)) return fail(QStringLiteral("cannot create isolated profile"));
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        profileSet_ = qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        if (!profileSet_) return fail(QStringLiteral("cannot isolate profile"));
        QByteArray line = QByteArrayLiteral("TAIL_BASE_") + QStringLiteral("日志界线_").toUtf8();
        line += QByteArray(1022 - line.size(), 'x');
        line += "\r\n";
        expected = line.repeated(33 * 1024);
        path = temporary.filePath(QStringLiteral("tail-source.log"));
        if (!writeFile(path, expected)) return false;
        if (captureDiskFileStamp(path, &initialStamp) != DiskFileState::Unchanged
            || initialStamp.fileIdentity.isEmpty()) return fail(QStringLiteral("source identity unavailable"));
        composition = std::make_unique<AppComposition>();
        window = std::make_unique<MainWindow>(nullptr, composition.get());
        added_ = QObject::connect(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded,
            window.get(), [this](qint64 id) { tasks.append(id); });
        window->resize(1200, 800);
        window->show();
        window->raise();
        window->activateWindow();
        if (!QTest::qWaitForWindowExposed(window.get())) return fail(QStringLiteral("native window not exposed"));
        const QString theme = qEnvironmentVariable("NOTEPADSAFE_TAIL_THEME");
        if (!theme.isEmpty()) {
            if ((theme != QStringLiteral("light") && theme != QStringLiteral("dark"))
                || QGuiApplication::platformName() != QStringLiteral("windows")
                || QApplication::style()->objectName().contains(QStringLiteral("fusion"), Qt::CaseInsensitive))
                return fail(QStringLiteral("Tail screenshots require native Windows light/dark theme"));
            auto *appearance = window->findChild<QAction *>(QStringLiteral("view.appearance.") + theme);
            if (!clickMenu(appearance) || !appearance->isChecked())
                return fail(QStringLiteral("real appearance menu did not select requested theme"));
        }
        if (!openSource) return true;
        window->openFilesFromExternal({path});
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs || !QTest::qWaitFor([&] { return tabs->count() == 1; }, 3000))
            return fail(QStringLiteral("ordinary S2 did not open"));
        tab = qobject_cast<EditorTab *>(tabs->currentWidget());
        if (!tab || !tab->windowedReadOnlyProvider()) return fail(QStringLiteral("ordinary S2 Provider missing"));
        provider = tab->windowedReadOnlyProvider();
        initialState = tab->documentState();
        if (!QTest::qWaitFor([&] { return !provider->characterCountPending(); }, 15000))
            return fail(QStringLiteral("initial statistics did not settle"));
        return guards().isEmpty() || fail(guards());
    }

    bool findTailUi()
    {
        toggle = window->findChild<QAction *>(QStringLiteral("view.tail.toggle"));
        resume = window->findChild<QAction *>(QStringLiteral("view.tail.resume"));
        status = provider ? provider->findChild<QLabel *>(QStringLiteral("tailStatusLabel")) : nullptr;
        resumeButton = provider ? provider->findChild<QAbstractButton *>(QStringLiteral("tailResumeButton")) : nullptr;
        if (!toggle || !resume) return fail(QStringLiteral("TAIL_NOT_IMPLEMENTED: view.tail.toggle/resume actions absent"));
        if (provider && (!status || !resumeButton))
            return fail(QStringLiteral("TAIL_NOT_IMPLEMENTED: tailStatusLabel/tailResumeButton absent"));
        return true;
    }

    bool clickMenu(QAction *action)
    {
        if (!action || !action->isEnabled()) return fail(QStringLiteral("requested menu action absent/disabled"));
        for (auto *menu : window->findChildren<QMenu *>()) {
            if (!menu->actions().contains(action)) continue;
            menu->popup(window->mapToGlobal(QPoint(120, 100)));
            if (!QTest::qWaitFor([&] { return menu->isVisible(); }, 1000))
                return fail(QStringLiteral("real action menu did not open"));
            const QRect area = menu->actionGeometry(action);
            if (area.isEmpty() || !menu->rect().contains(area.center())) {
                menu->close();
                return fail(QStringLiteral("menu action has no clickable geometry"));
            }
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, area.center());
            return true;
        }
        return fail(QStringLiteral("action is not present in a production menu"));
    }

    QString guards(bool readSource = true) const
    {
        if (readSource && fileDigest(path) != digest(expected)) return QStringLiteral("source SHA256 changed unexpectedly");
        if (!tab || !provider || tab->windowedReadOnlyProvider() != provider)
            return QStringLiteral("lost document/provider identity");
        const auto state = tab->documentState();
        if (state.id != initialState.id || state.location != initialState.location
            || state.origin != initialState.origin || state.persistence != initialState.persistence
            || state.securityClass != DocumentSecurityClass::OrdinaryDocument
            || state.contentKind != DocumentContentKind::Text || state.dirty || tab->isModified()
            || !tab->isReadOnlyByPolicy() || !provider->editor()->isReadOnly()
            || (state.capabilities & ~initialState.capabilities) != DocumentCapabilities{}
            || state.capabilities.testFlag(DocumentCapability::Edit)
            || state.capabilities.testFlag(DocumentCapability::Save)
            || state.revision < initialState.revision || tab->isSecureDocument())
            return QStringLiteral("document id/revision/ordinary/readonly capabilities changed unsafely");
        if (!modals.shown.isEmpty() || QApplication::activeModalWidget())
            return QStringLiteral("unexpected modal: ") + modals.shown.join(QLatin1Char(';'));
        return {};
    }

    bool append(const QByteArray &bytes)
    {
        DiskFileStamp before;
        if (captureDiskFileStamp(path, &before) != DiskFileState::Unchanged) return fail(QStringLiteral("append baseline unavailable"));
        QFile file(path);
        if (!file.open(QIODevice::Append)) return fail(file.errorString());
        if (file.write(bytes) != bytes.size() || !file.flush()) return fail(file.errorString());
        file.close();
        expected += bytes;
        DiskFileStamp after;
        if (captureDiskFileStamp(path, &after) != DiskFileState::Unchanged
            || after.fileIdentity != before.fileIdentity || after.size != expected.size())
            return fail(QStringLiteral("fixture failed to prove same-identity append"));
        return fileDigest(path) == digest(expected) || fail(QStringLiteral("append byte oracle mismatch"));
    }

    bool replaceAtomically(const QByteArray &bytes, bool preserveMtime = false)
    {
        DiskFileStamp before;
        if (captureDiskFileStamp(path, &before) != DiskFileState::Unchanged) return fail(QStringLiteral("replace baseline unavailable"));
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return fail(file.errorString());
        if (preserveMtime) {
            QFile timestamp(path);
            if (!timestamp.open(QIODevice::ReadWrite)
                || !timestamp.setFileTime(QDateTime::fromMSecsSinceEpoch(before.modifiedMs), QFileDevice::FileModificationTime))
                return fail(timestamp.errorString());
        }
        expected = bytes;
        DiskFileVersion actual;
        if (captureDiskFileVersion(path, &actual) != DiskFileState::Unchanged
            || actual.fileIdentity.isEmpty() || actual.fileIdentity == before.fileIdentity
            || !sameDiskFilePath(actual.absolutePath, path) || actual.size != bytes.size()
            || (preserveMtime && actual.modifiedMs != before.modifiedMs)
            || fileDigest(path) != digest(bytes)) return fail(QStringLiteral("atomic replacement did not replace exact target identity/bytes"));
        qInfo() << "TAIL_ATOMIC_REPLACEMENT" << actual.absolutePath << before.fileIdentity << actual.fileIdentity;
        return true;
    }

    bool writeFile(const QString &target, const QByteArray &bytes)
    {
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(bytes) != bytes.size() || !file.flush()) return fail(file.errorString());
        return true;
    }

    bool atEof() const
    {
        if (provider->windowEnd() != expected.size()) return false;
        if (provider->presentation() == WindowedReadOnlyProvider::Presentation::Text)
            return provider->visibleBottomOffset() >= expected.size();
        // Hex's source probe names the leftmost byte of its bottom row. Use
        // public Scintilla viewport geometry to prove the last row is visible.
        auto *editor = provider->editor();
        const long lines = std::max(1L, editor->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT));
        const long lastVisible = editor->SendScintilla(QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
                                                       static_cast<unsigned long>(lines - 1));
        const long lastWrap = std::max(1L, editor->SendScintilla(QsciScintillaBase::SCI_WRAPCOUNT,
                                                               static_cast<unsigned long>(lines - 1)));
        const long displayLines = std::max(lines, lastVisible + lastWrap);
        return editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE)
            + editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN) >= displayLines;
    }
    bool following() const { return status && status->isVisible() && status->text().contains(QStringLiteral("正在跟随")); }
    bool paused() const { return status && status->isVisible() && status->text().contains(QStringLiteral("暂停")); }
    bool stopped() const { return status && status->isVisible() && status->text().contains(QStringLiteral("已停止")); }
    int statisticsTasksSince(qsizetype first) const
    {
        int count = 0;
        for (qsizetype i = first; i < tasks.size(); ++i) {
            const auto task = AsyncTaskManager::instance().taskById(tasks.at(i));
            if (task.categoryTag == QStringLiteral("LineNumberIndex")
                || task.categoryTag == QStringLiteral("CharacterCount")) ++count;
        }
        return count;
    }
    bool fail(const QString &message) { error = message; return false; }

    bool capture(const QString &phase)
    {
        const QString directory = qEnvironmentVariable("NOTEPADSAFE_TAIL_EVIDENCE_DIR");
        if (directory.isEmpty()) return true;
        if (!QFileInfo(directory).isAbsolute() || !QDir().mkpath(directory))
            return fail(QStringLiteral("Tail evidence directory must be an available absolute output path"));
        const QString theme = qEnvironmentVariable("NOTEPADSAFE_TAIL_THEME", QStringLiteral("native"));
        const QString row = QString::fromLatin1(QTest::currentDataTag() ? QTest::currentDataTag() : "single");
        const QString name = QStringLiteral("tail-%1-%2-%3-%4.png")
            .arg(theme, QString::fromLatin1(QTest::currentTestFunction()), row, phase);
        return window->grab().save(QDir(directory).filePath(name))
            || fail(QStringLiteral("could not save native Tail evidence image"));
    }

    QTemporaryDir temporary;
    ModalGuard modals;
    std::unique_ptr<AppComposition> composition;
    std::unique_ptr<MainWindow> window;
    QPointer<EditorTab> tab;
    QPointer<WindowedReadOnlyProvider> provider;
    QAction *toggle = nullptr, *resume = nullptr;
    QPointer<QLabel> status;
    QPointer<QAbstractButton> resumeButton;
    QString path, error;
    QByteArray expected;
    DocumentState initialState;
    DiskFileStamp initialStamp;
    QVector<qint64> tasks;
private:
    bool profileSet_ = false, hadProfile_ = false;
    QByteArray oldProfile_;
    QMetaObject::Connection added_;
};
} // namespace

class SmokeTailFollow : public QObject {
    Q_OBJECT
private slots:
    void defaultOffAndUnsupportedTargets()
    {
        TailDocument document;
        QVERIFY2(document.start(false), qPrintable(document.error));
        QCOMPARE(fileDigest(document.path), digest(document.expected));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        QVERIFY(!document.toggle->isEnabled());
        QVERIFY(!document.toggle->isChecked());
        QVERIFY(!document.resume->isEnabled());

        const QString smallPath = document.temporary.filePath(QStringLiteral("editable.txt"));
        const QByteArray smallBytes("ordinary editable text\r\n");
        QVERIFY2(document.writeFile(smallPath, smallBytes), qPrintable(document.error));
        document.window->openFilesFromExternal({smallPath});
        auto *tabs = document.window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 3000);
        auto *editable = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(editable && !editable->isReadOnlyByPolicy());
        const auto state = editable->documentState();
        const QString text = editable->editor()->text();
        QVERIFY(!document.toggle->isEnabled() && !document.resume->isEnabled());
        // QAction::trigger is intentionally also attempted while disabled: the
        // command entry must not promote this unsupported document into Tail.
        document.toggle->trigger();
        document.resume->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(editable->documentState().id, state.id);
        QCOMPARE(editable->contentRevision(), state.revision);
        QCOMPARE(editable->editor()->text(), text);
        QCOMPARE(editable->documentState().capabilities, state.capabilities);
        QCOMPARE(fileDigest(smallPath), digest(smallBytes));
        QVERIFY(!document.toggle->isChecked());
        QVERIFY(document.modals.shown.isEmpty());
    }

    void enableAboveEofPausesWithoutMoving()
    {
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        QVERIFY(document.toggle->isEnabled() && document.toggle->isCheckable());
        QVERIFY(!document.toggle->isChecked());
        QVERIFY(document.status->text().contains(QStringLiteral("关闭")));
        QVERIFY(!document.status->accessibleName().isEmpty());
        QVERIFY2(document.capture(QStringLiteral("off")), qPrintable(document.error));
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        const auto position = ReadingPosition::capture(document.provider);
        const quint64 revision = document.tab->contentRevision();
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QCOMPARE(document.tab->contentRevision(), revision);
        QTRY_VERIFY_WITH_TIMEOUT(document.paused(), 2000);
        QVERIFY(position.equals(document.provider));
        QVERIFY(document.resumeButton->isVisible() && document.resumeButton->isEnabled());
        QVERIFY(document.resumeButton->text().contains(QStringLiteral("回到末尾")));
        QVERIFY(!document.resumeButton->accessibleName().isEmpty());
        QVERIFY2(document.capture(QStringLiteral("paused-above")), qPrintable(document.error));
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QVERIFY(!document.toggle->isChecked());
        const QByteArray suffix("OFF_STILL_OBSERVES\r\n");
        QVERIFY2(document.append(suffix), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > revision, ObserveTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        QVERIFY(!document.toggle->isChecked());
    }

    void menuFollowPreservesUtf8CrlfBoundaries_data()
    {
        QTest::addColumn<bool>("hex");
        QTest::newRow("text") << false;
        QTest::newRow("hex") << true;
    }

    void menuFollowPreservesUtf8CrlfBoundaries()
    {
        QFETCH(bool, hex);
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        if (hex) {
            QString switchError;
            QVERIFY2(document.tab->reopenWindowedPresentation(ViewMode::Hex, &switchError),
                     qPrintable(switchError));
            QVERIFY(document.tab->isHexPresentation());
        }
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        const quint64 revision = document.tab->contentRevision();
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        QCOMPARE(document.tab->contentRevision(), revision);
        const qsizetype taskBaseline = document.tasks.size();
        for (int batch = 0; batch < 3; ++batch) {
            // First batch crosses the real 2 MiB projection span with one long
            // line. UTF-8 and CRLF are literal source bytes, never an oracle
            // derived from the product decoder. Later batches end in unique rows.
            QByteArray suffix;
            if (batch == 0) suffix = QByteArray(2 * MiB + 7, 'L') + "\r\n";
            const QString marker = QStringLiteral("TAIL_BATCH_%1_中文终行").arg(batch);
            suffix += marker.toUtf8() + "\r\n";
            const qint64 oldSize = document.expected.size();
            const quint64 beforeRevision = document.tab->contentRevision();
            QVERIFY2(document.append(suffix), qPrintable(document.error));
            QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
            QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), ObserveTimeout);
            QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
            QVERIFY(document.tab->contentRevision() > beforeRevision);
            QCOMPARE(document.provider->sourceStamp().fileIdentity, document.initialStamp.fileIdentity);
            QCOMPARE(document.provider->sourceBytes(oldSize, suffix.size()), suffix);
            const auto rawTail = document.provider->sourceBytes(document.expected.size() - 64, 64);
            QCOMPARE(rawTail, document.expected.last(64));
            if (!hex) {
                QCOMPARE(document.provider->editor()->text().count(marker), 1);
                QVERIFY(!document.provider->editor()->text().contains(QChar::ReplacementCharacter));
            }
            QVERIFY(document.following());
        }
        QCOMPARE(document.statisticsTasksSince(taskBaseline), 0);
        if (hex) {
            auto *count = document.window->findChild<QLabel *>(QStringLiteral("status.characters"));
            QVERIFY(count);
            const QString exactBytes = QStringLiteral("字节数: %1")
                .arg(QLocale().toString(qint64(document.expected.size())));
            QTRY_COMPARE_WITH_TIMEOUT(count->text(), exactBytes, 2000);
            QVERIFY(!count->text().contains(QStringLiteral("字符")));
            QVERIFY(!count->text().contains(QStringLiteral("统计")));
            QVERIFY(!document.status->text().contains(QStringLiteral("字符统计")));
        } else {
            QVERIFY(document.status->text().contains(QStringLiteral("统计")));
            QVERIFY(document.status->text().contains(QStringLiteral("未更新")));
        }
        QVERIFY2(document.capture(QStringLiteral("following-statistics-stale")), qPrintable(document.error));
    }

    void pausedAppendPreservesNativeReadingAndResume()
    {
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        auto *editor = document.provider->editor();
        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_PageUp);
        QTRY_VERIFY_WITH_TIMEOUT(document.paused(), 2000);
        // Actual native keyboard selection and actual horizontal scrollbar;
        // never restore their values after append or directly mutate the model.
        QTest::keyClick(editor, Qt::Key_Home);
        for (int i = 0; i < 8; ++i) QTest::keyClick(editor, Qt::Key_Right, Qt::ShiftModifier);
        auto *horizontal = editor->horizontalScrollBar();
        QVERIFY(horizontal);
        const auto horizontalEvidence = [&] {
            return QStringLiteral("TAIL_HORIZONTAL_READY visible=%1 max=%2 policy=%3 viewport=%4 "
                                  "nativeEnabled=%5 nativeScrollWidth=%6")
                .arg(horizontal->isVisible()).arg(horizontal->maximum())
                .arg(int(editor->horizontalScrollBarPolicy())).arg(editor->viewport()->width())
                .arg(editor->SendScintilla(QsciScintillaBase::SCI_GETHSCROLLBAR))
                .arg(editor->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH));
        };
        qInfo().noquote() << horizontalEvidence();
        // Scintilla resets its tracked scroll width to 1 when installing a
        // projection. Its real paint/layout discovers the long line width.
        QVERIFY2(QTest::qWaitFor([&] {
            return horizontal->isVisible() && horizontal->maximum() > 0;
        }, 2000), qPrintable(horizontalEvidence()));
        qInfo().noquote() << horizontalEvidence();
        horizontal->setFocus();
        QTest::keyClick(horizontal, Qt::Key_End);
        QTRY_VERIFY_WITH_TIMEOUT(editor->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET) > 0, 2000);
        const auto reading = ReadingPosition::capture(document.provider);
        QVERIFY(reading.selectionEnd > reading.selectionStart);
        QVERIFY(reading.xOffset > 0);
        QVERIFY(document.resumeButton->isVisible() && document.resumeButton->isEnabled());
        QSignalSpy textWrites(editor, &QsciScintilla::textChanged);
        const qsizetype taskBaseline = document.tasks.size();
        for (int batch = 0; batch < 3; ++batch) {
            const quint64 revision = document.tab->contentRevision();
            const QByteArray suffix = QByteArray("PAUSED_BATCH_") + QByteArray::number(batch)
                + QStringLiteral("_保持选区\r\n").toUtf8();
            QVERIFY2(document.append(suffix), qPrintable(document.error));
            QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
            QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > revision, ObserveTimeout);
            QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
            QCOMPARE(document.provider->sourceStamp().fileIdentity, document.initialStamp.fileIdentity);
            QVERIFY2(reading.equals(document.provider), "TAIL_PAUSED_APPEND_MOVED_NATIVE_READING");
            QCOMPARE(textWrites.count(), 0);
            QVERIFY(document.paused());
        }
        QCOMPARE(document.statisticsTasksSince(taskBaseline), 0);
        QVERIFY2(document.capture(QStringLiteral("paused-native-selection")), qPrintable(document.error));
        QTest::mouseClick(document.resumeButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof() && document.following(), ObserveTimeout);
        QCOMPARE(document.provider->selectionStartOffset(), document.provider->selectionEndOffset());
        QCOMPARE(editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART),
                 editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(editor->text().contains(QStringLiteral("PAUSED_BATCH_2_保持选区")));
        QVERIFY2(document.append(QByteArrayLiteral("RESUMED_LATEST\r\n")), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), ObserveTimeout);
        QVERIFY(document.following());
        QVERIFY(editor->text().contains(QStringLiteral("RESUMED_LATEST")));
        QVERIFY2(document.capture(QStringLiteral("resumed")), qPrintable(document.error));
    }

    void navigationPausesTail_data()
    {
        QTest::addColumn<QString>("navigation");
        QTest::newRow("findNext") << QStringLiteral("findNext");
        QTest::newRow("dockJump") << QStringLiteral("dockJump");
        QTest::newRow("publicGotoApi") << QStringLiteral("publicGotoApi");
    }

    void navigationPausesTail()
    {
        QFETCH(QString, navigation);
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        const QByteArray marker("TAIL_NAVIGATION_UNIQUE_739");
        const qint64 markerOffset = document.expected.size();
        const QByteArray padding = (QByteArray(1022, 'n') + "\r\n").repeated(128);
        QVERIFY2(document.append(marker + "\r\n" + padding), qPrintable(document.error));
        QCOMPARE(document.expected.indexOf(marker), markerOffset);
        QCOMPARE(document.expected.lastIndexOf(marker), markerOffset);
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(!document.provider->characterCountPending(), 15000);

        FindOverlay *dialog = nullptr;
        QPushButton *next = nullptr;
        SearchResultsDock *dock = nullptr;
        QTreeView *tree = nullptr;
        QModelIndex result;
        if (navigation != QStringLiteral("publicGotoApi")) {
            QVERIFY2(document.clickMenu(document.window->findChild<QAction *>(QStringLiteral("search.find"))),
                     qPrintable(document.error));
            dialog = document.window->findChild<FindOverlay *>();
            QVERIFY(dialog);
            auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
            auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
            auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
            auto *caseCheck = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
            auto *whole = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
            auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
            auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
            auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
            auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
            next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
            QVERIFY(pattern && scope && regex && caseCheck && whole && hex && wrap && findAll && cancel && next);
            QVERIFY(scope->findData(0) >= 0);
            scope->setCurrentIndex(scope->findData(0));
            regex->setChecked(false);
            caseCheck->setChecked(true);
            whole->setChecked(false);
            hex->setChecked(false);
            wrap->setChecked(true);
            pattern->setText(QString::fromLatin1(marker));
            if (navigation == QStringLiteral("dockJump")) {
                const qsizetype taskBaseline = document.tasks.size();
                QTest::mouseClick(findAll, Qt::LeftButton);
                QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled() && !cancel->isVisible(), 15000);
                dock = document.window->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
                tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
                QVERIFY(dock && tree);
                auto *model = qobject_cast<SearchResultsModel *>(tree->model());
                QVERIFY(model && !model->isIncomplete());
                QCOMPARE(model->totalMatches(), qint64(1));
                result = model->index(0, SearchResultsModel::COL_CONTEXT);
                QCOMPARE(model->matchByteOffset(result), markerOffset);
                QCOMPARE(model->matchLength(result), int(marker.size()));
                bool realSearchFinished = false;
                for (qsizetype i = taskBaseline; i < document.tasks.size(); ++i) {
                    const auto task = AsyncTaskManager::instance().taskById(document.tasks.at(i));
                    if (task.type == AsyncTaskType::LargeFileSearch) {
                        QTRY_COMPARE_WITH_TIMEOUT(AsyncTaskManager::instance().taskById(task.id).state,
                                                  AsyncTaskState::Finished, 3000);
                        realSearchFinished = true;
                    }
                }
                QVERIFY(realSearchFinished);
            }
            dialog->close();
        }
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        const quint64 revision = document.tab->contentRevision();
        const qsizetype beforeNavigation = document.tasks.size();
        if (navigation == QStringLiteral("findNext")) {
            QVERIFY2(document.clickMenu(document.window->findChild<QAction *>(QStringLiteral("search.find"))),
                     qPrintable(document.error));
            QVERIFY(dialog->isVisible() && next->isEnabled());
            QTest::mouseClick(next, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(document.provider->selectionStartOffset(), markerOffset, 15000);
            QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled(), 15000);
            bool realNavigationFinished = false;
            for (qsizetype i = beforeNavigation; i < document.tasks.size(); ++i) {
                const auto task = AsyncTaskManager::instance().taskById(document.tasks.at(i));
                if (task.categoryTag == QStringLiteral("SearchNavigation")) {
                    QTRY_COMPARE_WITH_TIMEOUT(AsyncTaskManager::instance().taskById(task.id).state,
                                              AsyncTaskState::Finished, 3000);
                    realNavigationFinished = true;
                }
            }
            QVERIFY(realNavigationFinished);
            dialog->close();
        } else if (navigation == QStringLiteral("dockJump")) {
            QSignalSpy jumps(dock, &SearchResultsDock::jumpToDocumentByteOffset);
            dock->show();
            dock->raise();
            tree->scrollTo(result, QAbstractItemView::PositionAtCenter);
            QCoreApplication::processEvents();
            const QRect rect = tree->visualRect(result);
            QVERIFY(rect.isValid() && tree->viewport()->rect().contains(rect.center()));
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            QTRY_COMPARE_WITH_TIMEOUT(jumps.count(), 1, 2000);
            QCOMPARE(jumps.first().at(0).toUuid(), document.initialState.id);
            QCOMPARE(jumps.first().at(1).toULongLong(), revision);
            QCOMPARE(jumps.first().at(2).toLongLong(), markerOffset);
            QCOMPARE(jumps.first().at(3).toInt(), int(marker.size()));
        } else {
            // There is no Go To dialog/command in the current product. This
            // row explicitly tests its existing public byte-navigation boundary.
            qInfo("TAIL_PUBLIC_GOTO_API: not a Go To dialog GUI acceptance");
            document.tab->gotoByteOffset(markerOffset, int(marker.size()));
        }
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->selectionStartOffset(), markerOffset, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->selectionEndOffset(), markerOffset + marker.size(), 3000);
        QCOMPARE(document.provider->sourceBytes(markerOffset, marker.size()), marker);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QCOMPARE(document.tab->contentRevision(), revision);
        QTRY_VERIFY_WITH_TIMEOUT(document.paused(), 2000);
        const auto reading = ReadingPosition::capture(document.provider);
        QSignalSpy textWrites(document.provider->editor(), &QsciScintilla::textChanged);
        QVERIFY2(document.append(QByteArrayLiteral("AFTER_NAVIGATION_PAUSE\r\n")), qPrintable(document.error));
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > revision, ObserveTimeout);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(reading.equals(document.provider));
        QCOMPARE(textWrites.count(), 0);
        QVERIFY(document.paused());
        QVERIFY2(document.capture(QStringLiteral("navigation-paused")), qPrintable(document.error));
    }

    void disablingBeforeLatePaintDoesNotReclaimReadingPosition()
    {
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        QMenu *tailMenu = nullptr;
        for (auto *menu : document.window->findChildren<QMenu *>())
            if (menu->actions().contains(document.toggle)) { tailMenu = menu; break; }
        QVERIFY(tailMenu);
        tailMenu->popup(document.window->mapToGlobal(QPoint(120, 100)));
        QTRY_VERIFY_WITH_TIMEOUT(tailMenu->isVisible(), 1000);
        const QRect toggleRect = tailMenu->actionGeometry(document.toggle);
        QVERIFY(toggleRect.isValid() && tailMenu->rect().contains(toggleRect.center()));
        auto *editor = document.provider->editor();
        QSignalSpy paints(editor, &QsciScintillaBase::SCN_PAINTED);
        QVERIFY(paints.isValid());
        QObject observation;
        bool intercepted = false;
        bool capturedReading = false;
        QString callbackError;
        ReadingPosition reading;
        qsizetype paintsBeforeReturn = 0;
        const qint64 previousSize = document.expected.size();
        const quint64 revision = document.tab->contentRevision();
        connect(document.provider, &WindowedReadOnlyProvider::windowReady, &observation,
            [&](qint64, qint64 end, qint64) {
                if (intercepted || end <= previousSize) return;
                intercepted = true;
                if (!tailMenu->isVisible() || !document.toggle->isChecked()) {
                    callbackError = QStringLiteral("fixture lost real open Tail menu before source window became ready");
                    return;
                }
                // windowReady is emitted after updating the EOF projection,
                // before its scheduled viewport paint. Synchronous actual menu
                // input avoids QTest helpers pumping that paint before off.
                const QPoint point = toggleRect.center();
                const QPoint global = tailMenu->mapToGlobal(point);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tailMenu, &press);
                QApplication::sendEvent(tailMenu, &release);
                if (document.toggle->isChecked()) {
                    callbackError = QStringLiteral("actual Tail menu click did not turn following off");
                    return;
                }
                editor->setFocus(Qt::OtherFocusReason);
                QKeyEvent upPress(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
                QKeyEvent upRelease(QEvent::KeyRelease, Qt::Key_PageUp, Qt::NoModifier);
                QApplication::sendEvent(editor, &upPress);
                QApplication::sendEvent(editor, &upRelease);
                reading = ReadingPosition::capture(document.provider);
                paintsBeforeReturn = paints.count();
                capturedReading = true;
            }, Qt::DirectConnection);
        QVERIFY2(document.append((QByteArray(1022, 'p') + "\r\n").repeated(64)), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(intercepted, ObserveTimeout);
        QVERIFY2(callbackError.isEmpty(), qPrintable(callbackError));
        QVERIFY(capturedReading);
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(paints.count() > paintsBeforeReturn, 2000);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(reading.equals(document.provider), "TAIL_OFF_LATE_PAINT_RECLAIMED_READING_POSITION");
        QVERIFY(!document.toggle->isChecked());
        QVERIFY(document.status->text().contains(QStringLiteral("关闭")));
        QVERIFY(document.tab->contentRevision() > revision);
        const quint64 offRevision = document.tab->contentRevision();
        const qint64 offTop = document.provider->visibleTopOffset();
        QVERIFY2(document.append(QByteArrayLiteral("OFF_OBSERVATION_REMAINS_ACTIVE\r\n")), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > offRevision, ObserveTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QVERIFY(!document.toggle->isChecked());
        QCOMPARE(document.provider->visibleTopOffset(), offTop);
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.capture(QStringLiteral("off-after-late-paint")), qPrintable(document.error));
    }

    void disablingDuringTextChangedDoesNotRearmEofPaint()
    {
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        QMenu *tailMenu = nullptr;
        for (auto *menu : document.window->findChildren<QMenu *>())
            if (menu->actions().contains(document.toggle)) { tailMenu = menu; break; }
        QVERIFY(tailMenu);
        tailMenu->popup(document.window->mapToGlobal(QPoint(120, 100)));
        QTRY_VERIFY_WITH_TIMEOUT(tailMenu->isVisible(), 1000);
        const QRect actionRect = tailMenu->actionGeometry(document.toggle);
        QVERIFY(actionRect.isValid() && tailMenu->rect().contains(actionRect.center()));
        auto *editor = document.provider->editor();
        QSignalSpy writes(editor, &QsciScintilla::textChanged);
        QSignalSpy paints(editor, &QsciScintillaBase::SCN_PAINTED);
        QVERIFY(writes.isValid() && paints.isValid());
        QObject observation;
        const qint64 oldSize = document.expected.size();
        const quint64 oldRevision = document.tab->contentRevision();
        bool textIntercepted = false;
        bool offInsideTextChanged = false;
        bool paintIntercepted = false;
        bool readingCaptured = false;
        int eventSequence = 0;
        int offSequence = 0;
        int inputSequence = 0;
        qsizetype paintsBeforeReturn = 0;
        QString callbackError;
        ReadingPosition reading;
        // Observe the real viewport Paint before Scintilla processes it, without
        // consuming it. An expired reload need not publish windowReady at all.
        class BeforeViewportPaint final : public QObject {
        public:
            std::function<void()> beforePaint;
        protected:
            bool eventFilter(QObject *, QEvent *event) override
            {
                if (event->type() == QEvent::Paint && beforePaint) beforePaint();
                return false;
            }
        } paintBoundary;
        paintBoundary.beforePaint = [&] {
            if (!offInsideTextChanged || paintIntercepted) return;
            paintIntercepted = true;
            if (document.toggle->isChecked()) {
                callbackError = QStringLiteral("Tail was re-enabled before the first post-off viewport paint");
                return;
            }
            inputSequence = ++eventSequence;
            editor->setFocus(Qt::OtherFocusReason);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_PageUp, Qt::NoModifier);
            QApplication::sendEvent(editor, &press);
            QApplication::sendEvent(editor, &release);
            reading = ReadingPosition::capture(document.provider);
            paintsBeforeReturn = paints.count();
            readingCaptured = true;
        };
        editor->viewport()->installEventFilter(&paintBoundary);
        connect(editor, &QsciScintilla::textChanged, &observation, [&] {
            if (textIntercepted || document.provider->windowEnd() <= oldSize) return;
            textIntercepted = true;
            if (!tailMenu->isVisible() || !document.toggle->isChecked()) {
                callbackError = QStringLiteral("fixture lost actual Tail menu during projection textChanged");
                return;
            }
            // This is a genuine new projection's synchronous textChanged, not
            // a fabricated signal. Off may cause the outer reload to abort.
            const QPoint point = actionRect.center();
            const QPoint global = tailMenu->mapToGlobal(point);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(tailMenu, &press);
            QApplication::sendEvent(tailMenu, &release);
            offInsideTextChanged = !document.toggle->isChecked();
            if (offInsideTextChanged)
                offSequence = ++eventSequence;
            else
                callbackError = QStringLiteral("real off command did not finish inside textChanged");
        }, Qt::DirectConnection);
        QVERIFY2(document.append((QByteArray(1022, 'r') + "\r\n").repeated(64)), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(paintIntercepted, ObserveTimeout);
        QVERIFY2(callbackError.isEmpty(), qPrintable(callbackError));
        QVERIFY(textIntercepted && offInsideTextChanged && readingCaptured && writes.count() > 0);
        QVERIFY(offSequence > 0 && inputSequence > offSequence);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.tab->contentRevision() > oldRevision);
        QCOMPARE(document.provider->sourceStamp().fileIdentity, document.initialStamp.fileIdentity);
        QVERIFY(!document.toggle->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(paints.count() > paintsBeforeReturn, 2000);
        QVERIFY2(reading.equals(document.provider), "TAIL_TEXT_CHANGED_OFF_REARMED_LATE_EOF_PAINT");
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        QVERIFY(document.status->text().contains(QStringLiteral("关闭")));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.capture(QStringLiteral("off-during-text-changed")), qPrintable(document.error));
    }

    void pageUpDuringWindowReadyPausesBeforeLatePaint()
    {
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        auto *editor = document.provider->editor();
        editor->setFocus(Qt::OtherFocusReason);
        QSignalSpy paints(editor, &QsciScintillaBase::SCN_PAINTED);
        QVERIFY(paints.isValid());
        QObject observation;
        const qint64 oldSize = document.expected.size();
        const quint64 oldRevision = document.tab->contentRevision();
        bool intercepted = false;
        bool inputDelivered = false;
        bool pausedOnReturn = false;
        bool wasFollowing = false;
        qsizetype paintsBeforeReturn = 0;
        ReadingPosition reading;
        connect(document.provider, &WindowedReadOnlyProvider::windowReady, &observation,
            [&](qint64, qint64 end, qint64) {
                if (intercepted || end <= oldSize) return;
                intercepted = true;
                wasFollowing = document.following() && document.toggle->isChecked();
                // A real user input event must not be classified as an internal
                // scroll merely because it arrives within a projection signal.
                QKeyEvent press(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_PageUp, Qt::NoModifier);
                QApplication::sendEvent(editor, &press);
                QApplication::sendEvent(editor, &release);
                inputDelivered = true;
                pausedOnReturn = document.paused();
                reading = ReadingPosition::capture(document.provider);
                paintsBeforeReturn = paints.count();
            }, Qt::DirectConnection);
        QVERIFY2(document.append((QByteArray(1022, 'w') + "\r\n").repeated(64)), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(intercepted, ObserveTimeout);
        QVERIFY(wasFollowing && inputDelivered);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.tab->contentRevision() > oldRevision);
        QCOMPARE(document.provider->sourceStamp().fileIdentity, document.initialStamp.fileIdentity);
        QVERIFY2(pausedOnReturn, "TAIL_REAL_PAGEUP_IGNORED_INSIDE_WINDOW_READY");
        QTRY_VERIFY_WITH_TIMEOUT(paints.count() > paintsBeforeReturn, 2000);
        QVERIFY(reading.equals(document.provider));
        QVERIFY(document.provider->visibleBottomOffset() < document.expected.size());
        QVERIFY(document.paused());
        QVERIFY(document.resumeButton->isVisible() && document.resumeButton->isEnabled());
        QSignalSpy textWrites(editor, &QsciScintilla::textChanged);
        QVERIFY2(document.append(QByteArrayLiteral("AFTER_REENTRANT_PAGEUP\r\n")), qPrintable(document.error));
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(reading.equals(document.provider));
        QCOMPARE(textWrites.count(), 0);
        QVERIFY(document.paused());
        QVERIFY2(document.capture(QStringLiteral("pageup-during-window-ready")), qPrintable(document.error));
    }

    void splitAppendCompletesProjection_data()
    {
        QTest::addColumn<bool>("utf8Split");
        QTest::newRow("utf8") << true;
        QTest::newRow("crlf") << false;
    }

    void splitAppendCompletesProjection()
    {
        QFETCH(bool, utf8Split);
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        const qint64 originalSize = document.expected.size();
        const qsizetype taskBaseline = document.tasks.size();
        const QByteArray prefix = utf8Split ? QByteArrayLiteral("SPLIT_UTF8_") : QByteArrayLiteral("SPLIT_CRLF_");
        // Lead UTF-8 byte / CR sits at 1 MiB - 1. The remainder is
        // committed by a distinct real append after phase one's visible update.
        QByteArray first(MiB - 1 - prefix.size(), 'z');
        first += prefix;
        first += utf8Split ? QByteArray::fromHex("e4b8") : QByteArrayLiteral("\r");
        const QByteArray second = utf8Split
            ? QByteArray::fromHex("ad") + QByteArrayLiteral("\r\nUTF8_COMPLETED\r\n")
            : QByteArrayLiteral("\nCRLF_COMPLETED\r\n");
        const QString completeText = utf8Split
            ? QStringLiteral("SPLIT_UTF8_中\r\nUTF8_COMPLETED\r\n")
            : QStringLiteral("SPLIT_CRLF_\r\nCRLF_COMPLETED\r\n");
        QVERIFY2(document.append(first), qPrintable(document.error));
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), ObserveTimeout);
        QCOMPARE(document.provider->sourceBytes(originalSize, first.size()), first);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.following());
        // Loss-aware U+FFFD is permitted while the source itself has an
        // incomplete UTF-8 character. This phase cannot be judged as valid UTF-8.
        qInfo() << "TAIL_SPLIT_PHASE_ONE" << (utf8Split ? "incomplete-utf8" : "trailing-cr")
                << "sourceTail=" << first.last(4).toHex();
        const quint64 phaseOneRevision = document.tab->contentRevision();
        QVERIFY2(document.append(second), qPrintable(document.error));
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), ObserveTimeout);
        QCOMPARE(document.provider->sourceBytes(originalSize, first.size() + second.size()), first + second);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.tab->contentRevision() > phaseOneRevision);
        const QString projection = document.provider->editor()->text();
        QCOMPARE(projection.count(completeText), 1);
        QVERIFY(!projection.contains(QChar::ReplacementCharacter));
        QVERIFY(!projection.contains(QStringLiteral("\r\r\n")));
        QVERIFY(document.following());
        QCOMPARE(document.statisticsTasksSince(taskBaseline), 0);
        QVERIFY2(document.capture(QStringLiteral("split-completed")), qPrintable(document.error));
    }

    void sourceFaultStopsAndNeverAutoResumes_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("truncate") << QStringLiteral("truncate");
        QTest::newRow("sameSizeIdentityReplace") << QStringLiteral("replace");
        QTest::newRow("deleteRecreate") << QStringLiteral("delete");
        QTest::newRow("unreadable") << QStringLiteral("unreadable");
        QTest::newRow("rewrittenEofThenAppend") << QStringLiteral("rewrite");
    }

    void sourceFaultStopsAndNeverAutoResumes()
    {
        QFETCH(QString, fault);
#ifndef Q_OS_WIN
        if (fault == QStringLiteral("unreadable")) QSKIP("share=0 denial fixture targets Windows");
#endif
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        const quint64 revision = document.tab->contentRevision();
        const QByteArray original = document.expected;
        ExclusiveSourceLock lock;
        QStringList reasons;
        if (fault == QStringLiteral("truncate")) {
            QFile source(document.path);
            QVERIFY(source.open(QIODevice::ReadWrite));
            QVERIFY2(source.resize(32 * MiB + 1024), qPrintable(source.errorString()));
            source.close();
            document.expected.truncate(32 * MiB + 1024);
            reasons = {QStringLiteral("截断"), QStringLiteral("缩短")};
        } else if (fault == QStringLiteral("replace")) {
            QByteArray changed = original;
            changed.replace(changed.size() - 40, 12, QByteArrayLiteral("REPLACED_END"));
            QCOMPARE(changed.size(), original.size());
            QVERIFY2(document.replaceAtomically(changed, true), qPrintable(document.error));
            reasons = {QStringLiteral("替换"), QStringLiteral("身份")};
        } else if (fault == QStringLiteral("delete")) {
            QVERIFY2(QFile::remove(document.path), "fixture could not remove the exact temporary source");
            DiskFileStamp absent;
            QCOMPARE(captureDiskFileStamp(document.path, &absent), DiskFileState::Missing);
            reasons = {QStringLiteral("删除"), QStringLiteral("不存在"), QStringLiteral("找不到")};
        } else if (fault == QStringLiteral("unreadable")) {
            QVERIFY2(lock.acquire(document.path), "fixture could not obtain exclusive share=0 handle");
            SharedReadOnlyFile blocked;
            blocked.setFileName(document.path);
            QVERIFY(!blocked.open());
            reasons = {QStringLiteral("读取"), QStringLiteral("访问"), QStringLiteral("占用")};
        } else {
            QFile source(document.path);
            QVERIFY(source.open(QIODevice::ReadWrite));
            const qint64 offset = original.size() - 48;
            const QByteArray overwrite("REWRITTEN_EOF");
            QVERIFY(source.seek(offset));
            QCOMPARE(source.write(overwrite), qint64(overwrite.size()));
            QVERIFY(source.seek(original.size()));
            const QByteArray suffix("AFTER_REWRITE\r\n");
            QCOMPARE(source.write(suffix), qint64(suffix.size()));
            source.close();
            document.expected.replace(offset, overwrite.size(), overwrite);
            document.expected += suffix;
            reasons = {QStringLiteral("重写"), QStringLiteral("内容变化"), QStringLiteral("内容已变")};
        }
        const bool unavailable = fault == QStringLiteral("delete") || fault == QStringLiteral("unreadable");
        QVERIFY2(document.guards(!unavailable).isEmpty(), qPrintable(document.guards(!unavailable)));
        if (!unavailable) {
            DiskFileStamp actual;
            QCOMPARE(captureDiskFileStamp(document.path, &actual), DiskFileState::Unchanged);
            if (fault != QStringLiteral("replace")) QCOMPARE(actual.fileIdentity, document.initialStamp.fileIdentity);
            QCOMPARE(actual.size, qint64(document.expected.size()));
        }
        QTRY_VERIFY_WITH_TIMEOUT(document.stopped(), ObserveTimeout);
        const QString stoppedText = document.status->text();
        bool specificReason = false;
        for (const auto &reason : reasons) specificReason |= stoppedText.contains(reason);
        QVERIFY2(specificReason, qPrintable(QStringLiteral("TAIL_STOP_REASON: ") + stoppedText));
        QVERIFY(!document.resume->isEnabled());
        QVERIFY(!document.resumeButton->isEnabled() || !document.resumeButton->isVisible());
        QVERIFY2(document.capture(QStringLiteral("stopped")), qPrintable(document.error));
        // Recovery is a real source operation, not a controller reset. The
        // already-stopped Tail must not resume when normal observation recovers.
        lock.release();
        if (fault == QStringLiteral("delete")) {
            QVERIFY2(document.writeFile(document.path, original), qPrintable(document.error));
            document.expected = original;
        }
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.append(QByteArrayLiteral("RECOVERED_BUT_NOT_RESUMED\r\n")), qPrintable(document.error));
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->fileSize(), qint64(document.expected.size()), ObserveTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > revision, ObserveTimeout);
        QVERIFY(document.stopped());
        QVERIFY(!document.following());
        const auto recoveredPosition = ReadingPosition::settled(document.provider);
        document.resume->trigger();
        QCoreApplication::processEvents();
        QVERIFY(document.stopped());
        QVERIFY2(recoveredPosition.equals(document.provider),
                 qPrintable(recoveredPosition.differenceFrom(document.provider)));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));

        // Only explicit off/on may establish a fresh, admitted ordinary source
        // baseline. Opening from above must still preserve the reading position.
        QVERIFY(document.toggle->isChecked());
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QVERIFY(!document.toggle->isChecked());
        document.tab->gotoByteOffset(1024, 8);
        QTRY_COMPARE_WITH_TIMEOUT(document.provider->selectionStartOffset(), qint64(1024), 2000);
        const auto beforeReenable = ReadingPosition::capture(document.provider);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.paused(), 2000);
        QVERIFY(beforeReenable.equals(document.provider));
        DiskFileStamp recoveredStamp;
        QCOMPARE(captureDiskFileStamp(document.path, &recoveredStamp), DiskFileState::Unchanged);
        QVERIFY(sameDiskFileStamp(document.provider->sourceStamp(), recoveredStamp));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY(document.resumeButton->isVisible() && document.resumeButton->isEnabled());
        QTest::mouseClick(document.resumeButton, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof() && document.following(), ObserveTimeout);
        QVERIFY2(document.append(QByteArrayLiteral("EXPLICITLY_RESUMED_AFTER_FAILURE\r\n")), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), ObserveTimeout);
        QVERIFY(document.following());
        QVERIFY(document.provider->editor()->text().contains(QStringLiteral("EXPLICITLY_RESUMED_AFTER_FAILURE")));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.capture(QStringLiteral("explicit-recovery")), qPrintable(document.error));
    }

    void secureReplacementStopsWithoutUnlock()
    {
        const QString fixturePath = QFINDTESTDATA("../fixtures/kdbx/Format400.kdbx");
        QFile secure(fixturePath);
        QVERIFY(secure.open(QIODevice::ReadOnly));
        const QByteArray encrypted = secure.readAll();
        secure.close();
        QCOMPARE(encrypted.size(), qsizetype(1801));
        QCOMPARE(encrypted.first(12).toHex(), QByteArrayLiteral("03d9a29a67fb4bb500000400"));
        const QByteArray secureHash = QByteArray::fromHex("4f23b3f5f6c71209e135dab92d0f06034315d3f70e3e2ef3ec3a77ff37b49435");
        QCOMPARE(digest(encrypted), secureHash);
        TailDocument document;
        QVERIFY2(document.start(), qPrintable(document.error));
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QVERIFY2(document.findTailUi(), qPrintable(document.error));
        document.provider->goToOffset(document.expected.size());
        QTRY_VERIFY_WITH_TIMEOUT(document.atEof(), 3000);
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QTRY_VERIFY_WITH_TIMEOUT(document.following(), 2000);
        auto *workspace = document.window->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        QVERIFY(workspace);
        QWidget *const currentPage = workspace->currentWidget();
        const QString ordinaryProjection = document.provider->editor()->text();
        QStringList projected;
        QObject observer;
        QSignalSpy reloadFailures(document.provider, &WindowedReadOnlyProvider::sourceReloadFailed);
        QVERIFY(reloadFailures.isValid());
        connect(document.provider->editor(), &QsciScintilla::textChanged, &observer,
                [&] { projected.append(document.provider->editor()->text()); });
        // Take the lazy-factory baseline after actual open, menu and source prep.
        QVERIFY(document.composition->persistenceAdapters().adapter(QStringLiteral("secure.kdbx")) == nullptr);
        const int factories = document.composition->heavyFactoryCalls();
        const qsizetype firstTask = document.tasks.size();
        QVERIFY2(document.replaceAtomically(encrypted), qPrintable(document.error));
        const auto route = document.composition->persistenceAdapters().resolve(document.path);
        QVERIFY(route && route->persistence == DocumentPersistence::SecureFile);
        QTRY_VERIFY_WITH_TIMEOUT(document.stopped(), ObserveTimeout);
        QCOMPARE(fileDigest(document.path), secureHash);
        QCOMPARE(fileDigest(fixturePath), secureHash);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QCOMPARE(workspace->currentWidget(), currentPage);
        QCOMPARE(document.composition->heavyFactoryCalls(), factories);
        for (qsizetype i = firstTask; i < document.tasks.size(); ++i) {
            const auto task = AsyncTaskManager::instance().taskById(document.tasks.at(i));
            QVERIFY(task.type != AsyncTaskType::SecureOpen && task.type != AsyncTaskType::SecureSave
                    && task.type != AsyncTaskType::SecureIdentity && task.type != AsyncTaskType::VaultOperation);
        }
        for (const auto &text : projected)
            QVERIFY2(text.isEmpty() || text == ordinaryProjection, "TAIL_SECURE_REPLACEMENT_PROJECTED_NEW_SOURCE");
        qInfo() << "TAIL_SECURE_STOP_BEFORE_ADMISSION" << document.status->text()
                << "reloadFailures=" << reloadFailures.count();
        // The identity poll stops immediately; only the subsequent real
        // debounced admission can identify the replacement as a secure format.
        QVERIFY2(QTest::qWaitFor([&] {
            return reloadFailures.count() > 0
                && (document.status->text().contains(QStringLiteral("安全"))
                    || document.status->text().contains(QStringLiteral("加密")));
        }, ObserveTimeout), qPrintable(QStringLiteral("TAIL_SECURE_ADMISSION state=%1 failures=%2 last=%3")
            .arg(document.status->text()).arg(reloadFailures.count())
            .arg(reloadFailures.isEmpty() ? QString{} : reloadFailures.last().first().toString())));
        QVERIFY(document.provider->sourceBytes(0, 32).isEmpty());
        QVERIFY(document.status->text().contains(QStringLiteral("安全"))
                || document.status->text().contains(QStringLiteral("加密")));
        QVERIFY(!document.resume->isEnabled());
        QVERIFY(document.modals.shown.isEmpty());
        QVERIFY(!QFileInfo::exists(document.temporary.filePath(QStringLiteral("profile/security-session-v1.bin"))));
        QVERIFY2(document.capture(QStringLiteral("secure-rejected")), qPrintable(document.error));
        QVERIFY(document.toggle->isChecked());
        QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        QVERIFY(!document.toggle->isChecked());
        // Refusing to enable the menu is a valid fail-closed projection. If the
        // action is available, its real command must revalidate and still reject.
        if (document.toggle->isEnabled())
            QVERIFY2(document.clickMenu(document.toggle), qPrintable(document.error));
        else
            document.toggle->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!document.following());
        QVERIFY(!document.resume->isEnabled());
        QVERIFY(document.provider->sourceBytes(0, 32).isEmpty());
        QCOMPARE(fileDigest(document.path), secureHash);
        QCOMPARE(fileDigest(fixturePath), secureHash);
        QVERIFY2(document.guards().isEmpty(), qPrintable(document.guards()));
        QCOMPARE(workspace->currentWidget(), currentPage);
        QCOMPARE(document.composition->heavyFactoryCalls(), factories);
        for (qsizetype i = firstTask; i < document.tasks.size(); ++i) {
            const auto task = AsyncTaskManager::instance().taskById(document.tasks.at(i));
            QVERIFY(task.type != AsyncTaskType::SecureOpen && task.type != AsyncTaskType::SecureSave
                    && task.type != AsyncTaskType::SecureIdentity && task.type != AsyncTaskType::VaultOperation);
        }
        for (const auto &text : projected)
            QVERIFY2(text.isEmpty() || text == ordinaryProjection, "TAIL_SECURE_OFF_ON_PROJECTED_NEW_SOURCE");
        QVERIFY(document.modals.shown.isEmpty());
        QVERIFY2(document.capture(QStringLiteral("secure-reenable-refused")), qPrintable(document.error));
    }
};

QTEST_MAIN(SmokeTailFollow)
#include "smoke_tail_follow.moc"
