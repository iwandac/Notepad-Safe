#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeView>

#include <memory>
#include <utility>

#include <Qsci/qsciscintilla.h>
#include "AppComposition.h"
#include "AsyncTaskManager.h"
#include "DiskFileVersion.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include "LargeFileSearch.h"
#include "ReadOnlySearchSource.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

namespace {
constexpr qint64 MiB = 1024 * 1024;
constexpr qint64 OriginalOffset = 2 * MiB;
const QByteArray Marker("EXTERNAL_QUERY_739");

bool terminal(const AsyncTask &task)
{
    return task.id > 0 && task.state != AsyncTaskState::Queued
        && task.state != AsyncTaskState::Running;
}

QByteArray hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QVector<qint64> byteOracle(const QByteArray &bytes)
{
    QVector<qint64> offsets;
    for (qsizetype offset = bytes.indexOf(Marker); offset >= 0;
         offset = bytes.indexOf(Marker, offset + Marker.size())) offsets.append(offset);
    return offsets;
}

// Catch even a transient modal shown during a synchronous read/copy. Rejection
// is queued so an unexpected exec() can unwind instead of hanging the suite.
class SecureReplacementModalGuard final : public QObject {
public:
    SecureReplacementModalGuard() { qApp->installEventFilter(this); }
    ~SecureReplacementModalGuard() override { qApp->removeEventFilter(this); }
    QStringList shown;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            if (auto *dialog = qobject_cast<QDialog *>(watched);
                dialog && dialog->isModal()) {
                shown.append(QString::fromLatin1(dialog->metaObject()->className())
                             + QLatin1Char(':') + dialog->windowTitle());
                const QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, this, [guard] { if (guard) guard->reject(); });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

// Own only a temporary profile/source and this document's real search tasks.
// No synthetic source-change signals, private cache writes or pool overrides.
class ExternalSearchDocument final {
public:
    ~ExternalSearchDocument()
    {
        QObject::disconnect(added_);
        auto &manager = AsyncTaskManager::instance();
        for (qint64 id : std::as_const(tasks)) manager.cancel(id);
        if (!QTest::qWaitFor([&]() {
                for (qint64 id : std::as_const(tasks))
                    if (!terminal(manager.taskById(id))) return false;
                return true;
            }, 5000)) qWarning("external-change fixture did not drain its owned searches");
        if (window) {
            if (auto *tabs = window->findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *editor = qobject_cast<EditorTab *>(page))
                        editor->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
            window.reset();
        }
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", previousProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool open(bool spooled = false, AppComposition *composition = nullptr)
    {
        const auto fail = [this](const QString &stage, const QString &detail) {
            openError = stage + QStringLiteral(": ") + detail;
            return false;
        };
        openError.clear();
        if (!temporary_.isValid())
            return fail(QStringLiteral("create temporary directory"), temporary_.errorString());
        const QString profile = temporary_.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile))
            return fail(QStringLiteral("create isolated profile"), profile);
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        previousProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        if (!qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8()))
            return fail(QStringLiteral("select isolated profile"), profile);
        profileSet_ = true;
        path = temporary_.filePath(QStringLiteral("external-search.txt"));
        original = (QByteArray(1023, 'p') + '\n').repeated(33 * 1024);
        original.replace(OriginalOffset, Marker.size(), Marker);
        if (spooled) {
            const QByteArray matches = (Marker + '\n').repeated(60050);
            original.replace(OriginalOffset, matches.size(), matches);
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("open fixture source for writing"), file.errorString());
        const qint64 written = file.write(original);
        if (written != original.size() || !file.flush())
            return fail(QStringLiteral("write fixture source"),
                        QStringLiteral("written=%1 expected=%2 error=%3")
                            .arg(written).arg(original.size()).arg(file.errorString()));
        file.close();
        window = std::make_unique<MainWindow>(nullptr, composition);
        auto &manager = AsyncTaskManager::instance();
        added_ = QObject::connect(&manager, &AsyncTaskManager::taskAdded, window.get(),
            [this, &manager](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (task.type == AsyncTaskType::LargeFileSearch
                    && task.title.contains(QFileInfo(path).fileName())) tasks.append(id);
            });
        window->resize(1200, 800);
        window->show();
        window->raise();
        window->activateWindow();
        if (!QTest::qWaitForWindowExposed(window.get()))
            return fail(QStringLiteral("expose MainWindow"),
                        QStringLiteral("visible=%1 active=%2 minimized=%3 geometry=%4,%5 %6x%7")
                            .arg(window->isVisible()).arg(window->isActiveWindow())
                            .arg(window->isMinimized()).arg(window->x()).arg(window->y())
                            .arg(window->width()).arg(window->height()));
        window->openFilesFromExternal({path});
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs)
            return fail(QStringLiteral("locate document tabs"), window->statusBar()->currentMessage());
        if (!QTest::qWaitFor([&]() { return tabs->count() == 1; }, 3000))
            return fail(QStringLiteral("open one external document"),
                        QStringLiteral("tabs=%1 activeIndex=%2 status=%3")
                            .arg(tabs->count()).arg(tabs->currentIndex())
                            .arg(window->statusBar()->currentMessage()));
        tab = qobject_cast<EditorTab *>(tabs->currentWidget());
        if (!tab)
            return fail(QStringLiteral("locate active EditorTab"),
                        tabs->currentWidget()
                            ? QString::fromLatin1(tabs->currentWidget()->metaObject()->className())
                            : QStringLiteral("no active page"));
        state = tab->documentState();
        view = tab->viewModeL2();
        if (!sameDiskFilePath(state.location, path))
            return fail(QStringLiteral("bind actual external source"),
                        QStringLiteral("actual=%1 expected=%2 status=%3")
                            .arg(state.location, path, window->statusBar()->currentMessage()));
        const QString safety = safetyError(original);
        if (!safety.isEmpty())
            return fail(QStringLiteral("opened document safety guards"), safety);
        return true;
    }

    QString safetyError(const QByteArray &expected, bool allowTightenedCapabilities = false) const
    {
        if (!tab)
            return QStringLiteral("lost document object");
        if (!tab->isL2Mode() || !tab->windowedReadOnlyProvider())
            return QStringLiteral("lost production S2 document/provider: l2=%1 provider=%2 path=%3 bytes=%4")
                .arg(tab->isL2Mode()).arg(tab->windowedReadOnlyProvider() != nullptr)
                .arg(tab->filePath()).arg(tab->contentSizeBytes());
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs || tabs->currentWidget() != tab.data())
            return QStringLiteral("active document changed");
        const DocumentState current = tab->documentState();
        const bool capabilitiesMatch = allowTightenedCapabilities
            ? (current.capabilities & ~state.capabilities) == DocumentCapabilities{}
            : current.capabilities == state.capabilities;
        if (current.id != state.id || current.location != state.location
            || !capabilitiesMatch || tab->viewModeL2() != view)
            return QStringLiteral("document metadata changed: id=%1 expectedId=%2 path=%3 expectedPath=%4 "
                                  "capabilities=%5 expectedCapabilities=%6 view=%7 expectedView=%8")
                .arg(current.id.toString(), state.id.toString(), current.location, state.location)
                .arg(int(current.capabilities)).arg(int(state.capabilities))
                .arg(int(tab->viewModeL2())).arg(int(view));
        // Existing S2 open binds the path without promoting persistence=None
        // to PlainFile. Preserve that observed metadata without mutating the
        // document from the fixture; secure routing remains forbidden here.
        if (current.securityClass != DocumentSecurityClass::OrdinaryDocument
            || tab->isSecureDocument()
            || current.persistence == DocumentPersistence::SecureFile
            || current.persistence != state.persistence
            || !tab->isReadOnlyByPolicy() || !tab->windowedReadOnlyProvider()->editor()->isReadOnly()
            || tab->isModified() || current.dirty)
            return QStringLiteral("ordinary clean read-only policy mismatch: securityClass=%1 "
                                  "persistence=%2 baselinePersistence=%3 policyReadonly=%4 "
                                  "providerReadonly=%5 modified=%6 stateDirty=%7 providerBytes=%8 secure=%9")
                .arg(int(current.securityClass)).arg(int(current.persistence))
                .arg(int(state.persistence)).arg(tab->isReadOnlyByPolicy())
                .arg(tab->windowedReadOnlyProvider()->editor()->isReadOnly())
                .arg(tab->isModified()).arg(current.dirty)
                .arg(tab->windowedReadOnlyProvider()->fileSize()).arg(tab->isSecureDocument());
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QStringLiteral("read source safety guard: ") + file.errorString();
        const QByteArray actual = file.readAll();
        if (actual != expected || hash(actual) != hash(expected))
            return QStringLiteral("source bytes/hash differ: bytes=%1 expectedBytes=%2 sha256=%3 expectedSha256=%4")
                .arg(actual.size()).arg(expected.size())
                .arg(QString::fromLatin1(hash(actual).toHex()), QString::fromLatin1(hash(expected).toHex()));
        return {};
    }

    QString profileFile(const QString &name) const
    {
        return temporary_.filePath(QStringLiteral("profile/") + name);
    }

    std::unique_ptr<MainWindow> window;
    QPointer<EditorTab> tab;
    QString path;
    QString openError;
    QByteArray original;
    DocumentState state;
    ViewMode view = ViewMode::Text;
    QVector<qint64> tasks;
private:
    QTemporaryDir temporary_;
    bool profileSet_ = false, hadProfile_ = false;
    QByteArray previousProfile_;
    QMetaObject::Connection added_;
};
} // namespace

class SmokeSearchExternalChanges : public QObject {
    Q_OBJECT
private slots:
    void completedSearchInvalidatesAndReruns_data()
    {
        QTest::addColumn<QString>("mutation");
        QTest::addColumn<bool>("spooled");
        QTest::newRow("append") << QStringLiteral("append") << false;
        QTest::newRow("sameIdentityTruncate") << QStringLiteral("sameIdentityTruncate") << false;
        QTest::newRow("sameSizeAtomicReplace") << QStringLiteral("sameSizeAtomicReplace") << false;
        QTest::newRow("spooledSameIdentityTruncate") << QStringLiteral("sameIdentityTruncate") << true;
        QTest::newRow("spooledSameSizeAtomicReplace") << QStringLiteral("sameSizeAtomicReplace") << true;
    }

    void completedSearchInvalidatesAndReruns()
    {
        QFETCH(QString, mutation);
        QFETCH(bool, spooled);
        ExternalSearchDocument document;
        QVERIFY2(document.open(spooled), qPrintable(document.openError));
        QVector<qint64> originalOffsets;
        for (int row = 0; row < (spooled ? 60050 : 1); ++row)
            originalOffsets.append(OriginalOffset + row * (Marker.size() + 1));
        QCOMPARE(byteOracle(document.original), originalOffsets);
        DiskFileVersion before;
        QCOMPARE(captureDiskFileVersion(document.path, &before), DiskFileState::Unchanged);
        QVERIFY(before.valid && !before.fileIdentity.isEmpty());
        QCOMPARE(before.size, 33 * MiB);
        const quint64 originalRevision = document.tab->contentRevision();
        auto *provider = document.tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        QCOMPARE(provider->fileSize(), before.size);

        auto *findAction = document.window->findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = document.window->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = document.window->findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        auto *rerun = dock->findChild<QPushButton *>(QStringLiteral("searchResultsRerunButton"));
        QVERIFY(pattern && scope && regex && caseSensitive && wholeWord && hex
                && findAll && cancel && tree && summary && rerun);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        caseSensitive->setChecked(true);
        wholeWord->setChecked(false);
        hex->setChecked(false);
        pattern->setText(QString::fromLatin1(Marker));
        QVERIFY2(document.safetyError(document.original).isEmpty(),
                 qPrintable(document.safetyError(document.original)));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(document.tasks.size(), qsizetype(1), 2000);
        auto &manager = AsyncTaskManager::instance();
        const qint64 firstTask = document.tasks.first();
        QTRY_VERIFY_WITH_TIMEOUT(terminal(manager.taskById(firstTask)), 15000);
        QCOMPARE(manager.taskById(firstTask).state, AsyncTaskState::Finished);
        QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled() && !cancel->isVisible(), 15000);
        QPointer<SearchResultsModel> model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model && !model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(originalOffsets.size()));
        QCOMPARE(model->rowCount(), int(originalOffsets.size()));
        const QModelIndex result = model->index(0, SearchResultsModel::COL_LINE);
        QCOMPARE(model->matchByteOffset(result), OriginalOffset);
        QCOMPARE(model->matchLength(result), int(Marker.size()));
        const QModelIndex lastResult = model->index(model->rowCount() - 1, SearchResultsModel::COL_LINE);
        QCOMPARE(model->matchByteOffset(lastResult), originalOffsets.last());
        QCOMPARE(model->matchLength(lastResult), int(Marker.size()));
        if (spooled) {
            auto *source = document.tab->findChild<ReadOnlySearchSource *>();
            QVERIFY(source && source->findAllEngine());
            QVERIFY(source->findAllEngine()->overrunCount() > 0);
            QVERIFY(QFileInfo::exists(source->findAllEngine()->overrunFilePath()));
        }
        QCOMPARE(document.tab->contentRevision(), originalRevision);
        QVERIFY2(document.safetyError(document.original).isEmpty(),
                 qPrintable(document.safetyError(document.original)));
        QCOMPARE(compareDiskFileVersion(document.path, before), DiskFileState::Unchanged);

        dialog->hide();
        dock->show();
        dock->raise();
        document.window->raise();
        document.window->activateWindow();
        tree->scrollTo(result);
        QApplication::processEvents();
        const QPoint oldResultPoint = tree->visualRect(result).center();
        QVERIFY(tree->visualRect(result).isValid());
        QVERIFY(tree->viewport()->rect().contains(oldResultPoint));
        const bool legacyMapped = document.tab->l2View() && document.tab->l2View()->isOpen();
        const qint64 legacyBytes = document.tab->l2View() ? document.tab->l2View()->mmapSize() : 0;
        qInfo() << "after completed Find All:" << mutation << "legacyOpen=" << legacyMapped
                << "legacyMmapBytes=" << legacyBytes << "providerBytes=" << provider->fileSize();

        // No event pumping between replacement and timestamp restoration:
        // same-size replacement must not accidentally pass a metadata-only poll.
        QByteArray changed = document.original;
        QVector<qint64> expectedOffsets;
        if (mutation == QStringLiteral("append")) {
            const QByteArray suffix = Marker + '\n';
            QFile file(document.path);
            QVERIFY2(file.open(QIODevice::Append), qPrintable(file.errorString()));
            QCOMPARE(file.write(suffix), qint64(suffix.size()));
            file.close();
            changed += suffix;
            expectedOffsets = originalOffsets;
            expectedOffsets.append(before.size);
        } else if (mutation == QStringLiteral("sameIdentityTruncate")) {
            QFile file(document.path);
            QVERIFY2(file.open(QIODevice::ReadWrite), qPrintable(file.errorString()));
#ifdef Q_OS_WIN
            ::SetLastError(ERROR_SUCCESS);
#endif
            const bool resized = file.resize(MiB);
#ifdef Q_OS_WIN
            const unsigned long nativeError = ::GetLastError();
#else
            const unsigned long nativeError = 0;
#endif
            const QString resizeEvidence = QStringLiteral(
                "same-identity QFile::resize(1048576): ok=%1 QtError=%2 OSLastError=%3 error=%4; "
                "after completed FindAll legacyOpen=%5 legacyMmapBytes=%6 providerBytes=%7")
                .arg(resized).arg(int(file.error())).arg(nativeError).arg(file.errorString())
                .arg(legacyMapped).arg(legacyBytes).arg(provider->fileSize());
            qInfo().noquote() << resizeEvidence;
            if (!resized) {
                file.close();
                QVERIFY2(document.safetyError(document.original).isEmpty(),
                         qPrintable(document.safetyError(document.original)));
            }
            QVERIFY2(resized, qPrintable(resizeEvidence)); // Never replace this with atomic rewrite.
            constexpr qint64 newOffset = 4096;
            QVERIFY(file.seek(newOffset));
            QCOMPARE(file.write(Marker), qint64(Marker.size()));
            file.close();
            changed.truncate(MiB);
            changed.replace(newOffset, Marker.size(), Marker);
            expectedOffsets = {newOffset};
        } else {
            QCOMPARE(mutation, QStringLiteral("sameSizeAtomicReplace"));
            constexpr qint64 newOffset = 4 * MiB;
            for (qint64 oldOffset : originalOffsets)
                changed.replace(oldOffset, Marker.size(), QByteArray(Marker.size(), 'p'));
            changed.replace(newOffset, Marker.size(), Marker);
            QSaveFile replacement(document.path);
            replacement.setDirectWriteFallback(false);
            QVERIFY2(replacement.open(QIODevice::WriteOnly), qPrintable(replacement.errorString()));
            QCOMPARE(replacement.write(changed), qint64(changed.size()));
            const bool committed = replacement.commit();
            const QString commitError = replacement.errorString();
            if (!committed) {
                QVERIFY2(document.safetyError(document.original).isEmpty(),
                         qPrintable(document.safetyError(document.original)));
            }
            QVERIFY2(committed, qPrintable(commitError));
            QFile timestamp(document.path);
            QVERIFY2(timestamp.open(QIODevice::ReadWrite), qPrintable(timestamp.errorString()));
            QVERIFY2(timestamp.setFileTime(QDateTime::fromMSecsSinceEpoch(before.modifiedMs),
                                            QFileDevice::FileModificationTime),
                     qPrintable(timestamp.errorString()));
            timestamp.close();
            expectedOffsets = {newOffset};
        }

        // Prove the exact external operation before evaluating product behavior.
        DiskFileVersion after;
        QCOMPARE(captureDiskFileVersion(document.path, &after), DiskFileState::Unchanged);
        QVERIFY(after.valid && !after.fileIdentity.isEmpty());
        QCOMPARE(after.size, qint64(changed.size()));
        QVERIFY(hash(changed) != hash(document.original));
        QCOMPARE(byteOracle(changed), expectedOffsets);
        QVERIFY2(document.safetyError(changed).isEmpty(), qPrintable(document.safetyError(changed)));
        if (mutation == QStringLiteral("sameSizeAtomicReplace")) {
            QCOMPARE(after.size, before.size);
            QCOMPARE(after.modifiedMs, before.modifiedMs);
            QVERIFY(after.fileIdentity != before.fileIdentity);
        } else {
            QCOMPARE(after.fileIdentity, before.fileIdentity);
            QVERIFY(after.size != before.size);
        }
        qInfo() << "external mutation proved:" << mutation << "identityBefore=" << before.fileIdentity
                << "identityAfter=" << after.fileIdentity << "size=" << after.size
                << "mtimeBefore=" << before.modifiedMs << "mtimeAfter=" << after.modifiedMs
                << "sha256=" << hash(changed).toHex();

        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > originalRevision, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(summary->isVisible()
                                    && summary->text().contains(QStringLiteral("失效")), 5000);
        QVERIFY(rerun->isVisible() && rerun->isEnabled());
        // Invalidation deliberately precedes debounced admission. Wait for
        // actual accepted evidence, not a delay or size-only coincidence (an
        // atomic replacement may retain both size and timestamp).
        QTRY_VERIFY_WITH_TIMEOUT(provider->sourceVersion().fileIdentity == after.fileIdentity
                                     && provider->sourceVersion().size == after.size
                                     && provider->sourceVersion().modifiedMs == after.modifiedMs
                                     && provider->sourceVersion().sampleHash == after.sampleHash, 5000);
        QCOMPARE(provider->fileSize(), after.size);
        QVERIFY2(document.safetyError(changed).isEmpty(), qPrintable(document.safetyError(changed)));
        // Old rows may be removed or retained as disabled stale results. Click
        // the real prior viewport coordinate in either presentation.
        document.tab->gotoByteOffset(64, 5);
        const qint64 caret = document.tab->currentByteOffset();
        const qint64 anchor = provider->selectionAnchorOffset();
        const qint64 selectionCaret = provider->selectionCaretOffset();
        const quint64 refreshedRevision = document.tab->contentRevision();
        QSignalSpy jump(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        QSignalSpy legacyJump(dock, &SearchResultsDock::jumpToByteOffset);
        QVERIFY(jump.isValid() && legacyJump.isValid());
        QVERIFY(tree->viewport()->rect().contains(oldResultPoint));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, oldResultPoint);
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, oldResultPoint);
        bool queuedTurnDrained = false;
        QObject jumpFence;
        QTimer::singleShot(0, &jumpFence, [&]() { queuedTurnDrained = true; });
        QTRY_VERIFY_WITH_TIMEOUT(queuedTurnDrained, 2000);
        QCOMPARE(jump.count(), 0);
        QCOMPARE(legacyJump.count(), 0);
        QCOMPARE(document.tab->currentByteOffset(), caret);
        QCOMPARE(provider->selectionAnchorOffset(), anchor);
        QCOMPARE(provider->selectionCaretOffset(), selectionCaret);
        QCOMPARE(document.tab->contentRevision(), refreshedRevision);
        QVERIFY2(document.safetyError(changed).isEmpty(), qPrintable(document.safetyError(changed)));

        QSignalSpy rerunRequest(dock, &SearchResultsDock::rerunRequested);
        QVERIFY(rerunRequest.isValid());
        QCOMPARE(pattern->text(), QString::fromLatin1(Marker));
        QTest::mouseClick(rerun, Qt::LeftButton); // Same retained query, real Dock action.
        QCOMPARE(rerunRequest.count(), 1);
        QCOMPARE(rerunRequest.first().at(0).toUuid(), document.state.id);
        QTRY_COMPARE_WITH_TIMEOUT(document.tasks.size(), qsizetype(2), 2000);
        const qint64 secondTask = document.tasks.last();
        QVERIFY(secondTask != firstTask);
        QTRY_VERIFY_WITH_TIMEOUT(terminal(manager.taskById(secondTask)), 15000);
        QCOMPARE(manager.taskById(secondTask).state, AsyncTaskState::Finished);
        QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled() && !cancel->isVisible(), 15000);
        model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model && !model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(expectedOffsets.size()));
        QCOMPARE(model->rowCount(), int(expectedOffsets.size()));
        for (int row = 0; row < expectedOffsets.size(); ++row) {
            const QModelIndex index = model->index(row, SearchResultsModel::COL_LINE);
            QCOMPARE(model->matchByteOffset(index), expectedOffsets.at(row));
            QCOMPARE(model->matchLength(index), int(Marker.size()));
            QCOMPARE(changed.mid(model->matchByteOffset(index), model->matchLength(index)), Marker);
        }
        QCOMPARE(pattern->text(), QString::fromLatin1(Marker));
        QCOMPARE(scope->currentData().toInt(), 0);
        QVERIFY(!regex->isChecked() && caseSensitive->isChecked()
                && !wholeWord->isChecked() && !hex->isChecked());
        QVERIFY(!summary->text().contains(QStringLiteral("失效")));
        QCOMPARE(document.tab->contentRevision(), refreshedRevision);
        QVERIFY2(document.safetyError(changed).isEmpty(), qPrintable(document.safetyError(changed)));
        QCOMPARE(compareDiskFileVersion(document.path, after), DiskFileState::Unchanged);
    }

    void ordinaryReadOnlyRejectsSecureReplacement_data()
    {
        QTest::addColumn<bool>("prePollReads");
        QTest::newRow("prePollReads") << true;
        QTest::newRow("automaticRefresh") << false;
    }

    void ordinaryReadOnlyRejectsSecureReplacement()
    {
        QFETCH(bool, prePollReads);
        const QString fixturePath = QFINDTESTDATA("../fixtures/kdbx/Format400.kdbx");
        QFile fixture(fixturePath);
        QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));
        const QByteArray secureBytes = fixture.readAll();
        fixture.close();
        QCOMPARE(secureBytes.size(), qsizetype(1801));
        QCOMPARE(secureBytes.first(12).toHex(), QByteArray("03d9a29a67fb4bb500000400"));
        const QByteArray secureHash = QByteArray::fromHex(
            "4f23b3f5f6c71209e135dab92d0f06034315d3f70e3e2ef3ec3a77ff37b49435");
        QCOMPARE(hash(secureBytes), secureHash);

        AppComposition composition;
        SecureReplacementModalGuard modalGuard;
        ExternalSearchDocument document;
        QVERIFY2(document.open(false, &composition), qPrintable(document.openError));
        auto *tabs = document.window->findChild<QTabWidget *>();
        auto *workspace = document.window->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        QPointer<WindowedReadOnlyProvider> provider = document.tab->windowedReadOnlyProvider();
        QVERIFY(tabs && workspace && provider);
        QCOMPARE(workspace->currentWidget(), tabs);
        QCOMPARE(provider->fileSize(), 33 * MiB);
        const auto ordinaryRoute = composition.persistenceAdapters().resolve(document.path);
        QVERIFY(ordinaryRoute.has_value());
        QCOMPARE(ordinaryRoute->persistence, DocumentPersistence::PlainFile);
        const int factoriesBeforeFind = composition.heavyFactoryCalls();

        auto *findAction = document.window->findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = document.window->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = document.window->findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        auto *rerun = dock->findChild<QPushButton *>(QStringLiteral("searchResultsRerunButton"));
        QVERIFY(pattern && scope && regex && caseSensitive && wholeWord && hex
                && findAll && cancel && tree && summary && rerun);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        caseSensitive->setChecked(true);
        wholeWord->setChecked(false);
        hex->setChecked(false);
        pattern->setText(QString::fromLatin1(Marker));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(document.tasks.size(), qsizetype(1), 2000);
        auto &manager = AsyncTaskManager::instance();
        const qint64 searchTask = document.tasks.first();
        QTRY_VERIFY_WITH_TIMEOUT(terminal(manager.taskById(searchTask)), 15000);
        QCOMPARE(manager.taskById(searchTask).state, AsyncTaskState::Finished);
        QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled() && !cancel->isVisible(), 15000);
        auto *model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model && !model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(1));
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex oldResult = model->index(0, SearchResultsModel::COL_LINE);
        QCOMPARE(model->matchByteOffset(oldResult), OriginalOffset);
        QCOMPARE(model->matchLength(oldResult), int(Marker.size()));
        auto *searchSource = document.tab->findChild<ReadOnlySearchSource *>();
        QVERIFY(searchSource);
        QTRY_VERIFY_WITH_TIMEOUT(!searchSource->isReady(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!provider->characterCountPending(), 15000);

        dialog->hide();
        dock->show();
        dock->raise();
        tree->scrollTo(oldResult);
        QApplication::processEvents();
        const QPoint oldResultPoint = tree->visualRect(oldResult).center();
        QVERIFY(tree->visualRect(oldResult).isValid());
        QVERIFY(tree->viewport()->rect().contains(oldResultPoint));
        document.tab->gotoByteOffset(0, 32);
        QCOMPARE(provider->selectionStartOffset(), qint64(0));
        QCOMPARE(provider->selectionEndOffset(), qint64(32));
        provider->editor()->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(provider->editor()->hasFocus(), 2000);
        const QByteArray oldSelection = document.original.first(32);
        QCOMPARE(oldSelection, QByteArray(32, 'p'));
        const auto sendActualCopyKeys = [&]() {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
            QApplication::sendEvent(provider->editor(), &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_C, Qt::ControlModifier);
            QApplication::sendEvent(provider->editor(), &release);
        };
        auto *clipboard = QApplication::clipboard();
        auto *savedClipboard = new QMimeData;
        if (const QMimeData *mime = clipboard->mimeData())
            for (const QString &format : mime->formats()) savedClipboard->setData(format, mime->data(format));
        const auto restoreClipboard = qScopeGuard([clipboard, savedClipboard] {
            clipboard->setMimeData(savedClipboard);
        });
        sendActualCopyKeys(); // Prove the real input route and selected source before mutation.
        QCOMPARE(clipboard->text(), QString::fromLatin1(oldSelection));
        const QString clipboardSentinel = QStringLiteral("KEEP_EXISTING_CLIPBOARD_2C");
        clipboard->setText(clipboardSentinel);
        const QByteArray oldProjectionHash = hash(provider->editor()->text().toUtf8());
        QVERIFY(oldProjectionHash != hash(QByteArray{}));
        const quint64 oldRevision = document.tab->contentRevision();
        DiskFileVersion before;
        QCOMPARE(captureDiskFileVersion(document.path, &before), DiskFileState::Unchanged);
        QVERIFY(before.valid && !before.fileIdentity.isEmpty());
        QVERIFY2(document.safetyError(document.original).isEmpty(),
                 qPrintable(document.safetyError(document.original)));

        QVector<QByteArray> projectionHashes;
        QStringList statusMessages;
        QVector<qint64> securityTasks;
        QObject observation;
        connect(provider->editor(), &QsciScintilla::textChanged, &observation, [&] {
            if (provider) projectionHashes.append(hash(provider->editor()->text().toUtf8()));
        }, Qt::DirectConnection);
        connect(document.window->statusBar(), &QStatusBar::messageChanged, &observation,
                [&](const QString &message) { statusMessages.append(message); }, Qt::DirectConnection);
        connect(&manager, &AsyncTaskManager::taskAdded, &observation, [&](qint64 id) {
            const auto type = manager.taskById(id).type;
            if (type == AsyncTaskType::SecureOpen || type == AsyncTaskType::SecureSave
                || type == AsyncTaskType::SecureIdentity || type == AsyncTaskType::VaultOperation)
                securityTasks.append(id);
        }, Qt::DirectConnection);
        const auto drainUnexpectedSecurityTasks = qScopeGuard([&] {
            for (qint64 id : std::as_const(securityTasks)) manager.cancel(id);
            if (!QTest::qWaitFor([&] {
                    for (qint64 id : std::as_const(securityTasks))
                        if (!terminal(manager.taskById(id))) return false;
                    return true;
                }, 5000)) qWarning("secure-replacement fixture did not drain unexpected owned security work");
        });
        QSignalSpy reloadFailed(provider, &WindowedReadOnlyProvider::sourceReloadFailed);
        QVERIFY(reloadFailed.isValid());
        // The ordinary Find dialog itself is a counted lazy factory. Establish
        // the mutation baseline only after all real search/copy setup finishes.
        // The default secure.kdbx entry is route-only: adapter() observes an
        // already-created instance here and has no factory to instantiate.
        const int persistenceInstancesBeforeProbe = composition.persistenceAdapters().instantiatedCount();
        const bool secureAdapterAlreadyCreated =
            composition.persistenceAdapters().adapter(QStringLiteral("secure.kdbx")) != nullptr;
        const int factoriesBefore = composition.heavyFactoryCalls();
        qInfo() << "SECURE_REPLACEMENT_FACTORY_BASELINE" << QTest::currentDataTag()
                << "beforeFind=" << factoriesBeforeFind << "beforeMutation=" << factoriesBefore
                << "persistenceInstances=" << composition.persistenceAdapters().instantiatedCount()
                << "secureAdapterAlreadyCreated=" << secureAdapterAlreadyCreated;
        QCOMPARE(composition.persistenceAdapters().instantiatedCount(), persistenceInstancesBeforeProbe);
        QVERIFY2(!secureAdapterAlreadyCreated, "fixture entered security services before replacement");
        QVERIFY(securityTasks.isEmpty());
        QVERIFY(modalGuard.shown.isEmpty());
        QVERIFY(!QFileInfo::exists(document.profileFile(QStringLiteral("security-session-v1.bin"))));
        const auto eventTurnRan = std::make_shared<bool>(false);
        QTimer::singleShot(0, &observation, [eventTurnRan] { *eventTurnRan = true; });

        QSaveFile replacement(document.path);
        replacement.setDirectWriteFallback(false);
        QVERIFY2(replacement.open(QIODevice::WriteOnly), qPrintable(replacement.errorString()));
        QCOMPARE(replacement.write(secureBytes), qint64(secureBytes.size()));
        QVERIFY2(replacement.commit(), qPrintable(replacement.errorString()));
        QByteArray beforePollBytes;
        QString beforePollClipboard;
        if (prePollReads) {
            // No QTest key helper or event pumping here. Both real entry points
            // run before any assertion of their behavior can abort the case.
            beforePollBytes = provider->sourceBytes(0, 32);
            sendActualCopyKeys();
            beforePollClipboard = clipboard->text();
        }
        const bool noEventTurnBetweenCommitAndReads = !*eventTurnRan;
        DiskFileVersion after;
        QCOMPARE(captureDiskFileVersion(document.path, &after), DiskFileState::Unchanged);
        QVERIFY(after.valid && !after.fileIdentity.isEmpty());
        QVERIFY(after.fileIdentity != before.fileIdentity);
        QCOMPARE(after.size, qint64(secureBytes.size()));
        const int factoriesBeforeSecureResolve = composition.heavyFactoryCalls();
        const auto secureRoute = composition.persistenceAdapters().resolve(document.path);
        qInfo() << "SECURE_REPLACEMENT_ROUTE_PROBE" << QTest::currentDataTag()
                << "heavyBefore=" << factoriesBeforeSecureResolve
                << "heavyAfter=" << composition.heavyFactoryCalls();
        QVERIFY(secureRoute.has_value());
        QCOMPARE(secureRoute->id, QStringLiteral("secure.kdbx"));
        QCOMPARE(secureRoute->persistence, DocumentPersistence::SecureFile);

        const auto securityGuardError = [&]() -> QString {
            const QString safety = document.safetyError(secureBytes, true);
            if (!safety.isEmpty()) return safety;
            if (composition.heavyFactoryCalls() != factoriesBefore)
                return QStringLiteral("heavy factory count changed after replacement: before=%1 after=%2")
                    .arg(factoriesBefore).arg(composition.heavyFactoryCalls());
            if (!securityTasks.isEmpty())
                return QStringLiteral("ordinary refresh submitted %1 security tasks").arg(securityTasks.size());
            if (!modalGuard.shown.isEmpty() || QApplication::activeModalWidget())
                return QStringLiteral("unexpected modal: ") + modalGuard.shown.join(QLatin1Char(';'));
            if (workspace->currentWidget() != tabs || tabs->count() != 1
                || tabs->currentWidget() != document.tab.data())
                return QStringLiteral("workspace or document changed");
            if (QFileInfo::exists(document.profileFile(QStringLiteral("security-session-v1.bin"))))
                return QStringLiteral("created security identity/session state");
            const auto capabilities = document.tab->documentState().capabilities;
            if (capabilities.testFlag(DocumentCapability::Edit)
                || capabilities.testFlag(DocumentCapability::Save))
                return QStringLiteral("small secure replacement promoted read-only document to editable");
            QFile originalFixture(fixturePath);
            if (!originalFixture.open(QIODevice::ReadOnly) || hash(originalFixture.readAll()) != secureHash)
                return QStringLiteral("original secure fixture was changed");
            return {};
        };
        const auto logSecurityEvidence = [&](const char *stage) {
            qInfo() << "SECURE_REPLACEMENT_SECURITY_EVIDENCE" << QTest::currentDataTag() << stage
                    << "heavyBeforeMutation=" << factoriesBefore
                    << "heavyNow=" << composition.heavyFactoryCalls()
                    << "securityTaskCount=" << securityTasks.size();
            for (qint64 id : std::as_const(securityTasks)) {
                const AsyncTask task = manager.taskById(id);
                qInfo() << "SECURE_REPLACEMENT_SECURITY_TASK" << "id=" << id
                        << "type=" << int(task.type) << "typeText=" << task.typeText()
                        << "category=" << task.categoryTag << "title=" << task.title;
            }
        };
        logSecurityEvidence("before-behavior-assertions");
        QVERIFY2(securityGuardError().isEmpty(), qPrintable(securityGuardError()));
        qInfo() << "SECURE_REPLACEMENT_GUARDS_OK" << QTest::currentDataTag()
                << "identityBefore=" << before.fileIdentity << "identityAfter=" << after.fileIdentity
                << "fixtureSha256=" << secureHash.toHex() << "prePollBytes=" << beforePollBytes.size()
                << "prePollClipboardSha256=" << hash(beforePollClipboard.toUtf8()).toHex();
        if (prePollReads) {
            QVERIFY2(noEventTurnBetweenCommitAndReads, "an event loop ran before both pre-poll inputs completed");
            QVERIFY2(beforePollBytes.isEmpty() || beforePollBytes == oldSelection,
                     "sourceBytes read the new secure source under the old ordinary-file authorization");
            QVERIFY2(beforePollClipboard == clipboardSentinel
                         || beforePollClipboard == QString::fromLatin1(oldSelection),
                     "actual Ctrl+C must refuse or copy only the already-authorized ordinary snapshot");
        }

        // Observe the production poll/debounce, never call refreshFileMapping.
        QTRY_VERIFY_WITH_TIMEOUT(!projectionHashes.isEmpty() || !reloadFailed.isEmpty()
                                     || document.tab->contentRevision() > oldRevision, 5000);
        logSecurityEvidence("after-automatic-observation");
        QVERIFY2(securityGuardError().isEmpty(), qPrintable(securityGuardError()));
        for (const QByteArray &projectionHash : std::as_const(projectionHashes)) {
            QVERIFY2(projectionHash == oldProjectionHash || projectionHash == hash(QByteArray{}),
                     "a transient projection displayed bytes from the replacement secure source");
        }
        QVERIFY(hash(provider->editor()->text().toUtf8()) == oldProjectionHash
                || provider->editor()->text().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document.tab->contentRevision() > oldRevision, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(summary->isVisible()
                                     && summary->text().contains(QStringLiteral("失效")), 5000);
        QVERIFY(rerun->isVisible());
        // Revision/Dock invalidation is immediate; the automatic read refusal
        // is delivered by the later debounced attempt. Await that real signal,
        // never equate an advanced revision with a completed refresh.
        QTRY_VERIFY_WITH_TIMEOUT(!reloadFailed.isEmpty(), 5000);
        QVERIFY2(securityGuardError().isEmpty(), qPrintable(securityGuardError()));
        bool actionableSecureRefusal = false;
        for (const QString &message : std::as_const(statusMessages)) {
            const bool namesSecurity = message.contains(QStringLiteral("安全"))
                || message.contains(QStringLiteral("加密")) || message.contains(QStringLiteral("受保护"));
            actionableSecureRefusal |= namesSecurity && message.contains(QStringLiteral("打开"));
            QVERIFY2(!message.contains(QStringLiteral("已刷新只读视图")),
                     "blocked secure replacement was reported as a successful ordinary refresh");
        }
        QVERIFY2(actionableSecureRefusal, qPrintable(statusMessages.join(QLatin1Char('\n'))));

        const qint64 caret = document.tab->currentByteOffset();
        const qint64 anchor = provider->selectionAnchorOffset();
        const qint64 selectionCaret = provider->selectionCaretOffset();
        const quint64 invalidatedRevision = document.tab->contentRevision();
        QSignalSpy jump(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        QSignalSpy legacyJump(dock, &SearchResultsDock::jumpToByteOffset);
        QVERIFY(jump.isValid() && legacyJump.isValid());
        QVERIFY(tree->viewport()->rect().contains(oldResultPoint));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, oldResultPoint);
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, oldResultPoint);
        bool jumpTurnDrained = false;
        QObject jumpFence;
        QTimer::singleShot(0, &jumpFence, [&] { jumpTurnDrained = true; });
        QTRY_VERIFY_WITH_TIMEOUT(jumpTurnDrained, 2000);
        QCOMPARE(jump.count(), 0);
        QCOMPARE(legacyJump.count(), 0);
        QCOMPARE(document.tab->currentByteOffset(), caret);
        QCOMPARE(provider->selectionAnchorOffset(), anchor);
        QCOMPARE(provider->selectionCaretOffset(), selectionCaret);
        QCOMPARE(document.tab->contentRevision(), invalidatedRevision);
        logSecurityEvidence("after-stale-result-click");
        QVERIFY2(securityGuardError().isEmpty(), qPrintable(securityGuardError()));
        QCOMPARE(compareDiskFileVersion(document.path, after), DiskFileState::Unchanged);
        // Check the entire observation history again after the real stale click.
        for (const QByteArray &projectionHash : std::as_const(projectionHashes))
            QVERIFY(projectionHash == oldProjectionHash || projectionHash == hash(QByteArray{}));
    }
};

QTEST_MAIN(SmokeSearchExternalChanges)
#include "smoke_search_external_changes.moc"
