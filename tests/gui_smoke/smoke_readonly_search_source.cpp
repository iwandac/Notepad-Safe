#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>
#include <utility>

#include "AsyncTaskManager.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "LargeFileSearch.h"
#include "MainWindow.h"
#include "ReadOnlySearchSource.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "WindowedReadOnlyProvider.h"

namespace {
QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}
}

class SmokeReadOnlySearchSource final : public QObject {
    Q_OBJECT
private slots:
    void completedSpoolSurvivesCachedNavigationAndDifferentQuery()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const bool hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray oldProfile = qgetenv("NOTEPADSAFE_DATADIR");
        const QString profile = fixture.filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(profile));
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreProfile = qScopeGuard([&] {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", oldProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        constexpr qint64 SourceSize = 33LL * 1024 * 1024;
        constexpr int HitCount = 60050;
        constexpr qint64 OtherOffset = 2LL * 1024 * 1024 + 8;
        const QByteArray other("Z_NAV_SECOND_629");
        QByteArray bytes = (QByteArray(127, 'p') + '\n').repeated(int(SourceSize / 128));
        bytes.replace(0, HitCount * 2, QByteArray("X\n").repeated(HitCount));
        bytes.replace(OtherOffset, other.size(), other);
        const QString path = fixture.filePath(QStringLiteral("ordinary-spool-cache.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), SourceSize);
        }
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        window.raise();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->count() == 1);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && target->windowedReadOnlyProvider());
        QPointer<WindowedReadOnlyProvider> provider = target->windowedReadOnlyProvider();
        const DocumentState initial = target->documentState();
        QVERIFY(target->isReadOnlyByPolicy() && !target->isBinaryDocument());
        QVERIFY(!target->isSecureDocument() && !target->isHexPresentation());
        QVERIFY(!initial.id.isNull() && !initial.dirty && !target->isModified());
        QVERIFY(!initial.capabilities.testFlag(DocumentCapability::Edit));
        QCOMPARE(target->contentSizeBytes(), SourceSize);
        auto &manager = AsyncTaskManager::instance();
        QSignalSpy terminals(&manager, &AsyncTaskManager::taskFinished);
        QVector<qint64> ownedTasks;
        const auto added = connect(&manager, &AsyncTaskManager::taskAdded, &window,
            [&](qint64 id) {
                if (manager.taskById(id).title.contains(QFileInfo(path).fileName()))
                    ownedTasks.append(id);
            });
        const auto cleanup = qScopeGuard([&] {
            disconnect(added);
            for (qint64 id : std::as_const(ownedTasks)) manager.cancel(id);
            if (target) target->shutdownL2BeforeClose();
        });
        const auto guards = [&] {
            return target && provider && target->documentState().id == initial.id
                && target->contentRevision() == initial.revision
                && target->documentState().capabilities == initial.capabilities
                && !target->isModified() && target->isReadOnlyByPolicy()
                && provider == target->windowedReadOnlyProvider()
                && target->contentSizeBytes() == SourceSize
                && fileHash(path) == expectedHash && !QApplication::activeModalWidget();
        };
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        QVERIFY(findAction);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *all = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *dock = window.findChild<SearchResultsDock *>();
        QVERIFY(pattern && caseSensitive && wrap && next && previous && all && cancel && status && dock);
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(tree);
        caseSensitive->setChecked(true); // This case isolates ownership, not dense case folding.
        pattern->setText(QStringLiteral("X"));
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && tree->model(), 15000);
        QPointer<SearchResultsModel> model = qobject_cast<SearchResultsModel *>(tree->model());
        QPointer<ReadOnlySearchSource> source = target->findChild<ReadOnlySearchSource *>();
        QVERIFY(model && source && !model->isIncomplete());
        auto *engine = source->findAllEngine();
        QCOMPARE(model->totalMatches(), qint64(HitCount));
        qint64 findAllTaskId = 0;
        for (qint64 id : std::as_const(ownedTasks)) {
            if (manager.taskById(id).categoryTag == QStringLiteral("Search")) {
                QCOMPARE(findAllTaskId, qint64(0));
                findAllTaskId = id;
            }
        }
        QVERIFY(findAllTaskId > 0);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(findAllTaskId).state,
                                  AsyncTaskState::Finished, 2000);
        QVERIFY(model->rowCount() > 50000); // The inspected tail is a virtual spool row.
        QVERIFY(engine->memoryMatches().size() <= 50000 && engine->overrunCount() > 0);
        const QString spoolPath = engine->overrunFilePath();
        const QByteArray spoolHash = fileHash(spoolPath);
        QVERIFY(!spoolPath.isEmpty() && !spoolHash.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!source->isReady() && !engine->sourceInUse()
            && !source->navigationEngine()->sourceInUse(), 2000);
        QVERIFY(guards());
        const int tailRow = model->rowCount() - 1;
        constexpr qint64 TailOffset = 2LL * (HitCount - 1);
        const auto tail = model->groupedLineAt(tailRow);
        QCOMPARE(tail.firstByteOffset, TailOffset);
        QCOMPARE(tail.firstLength, 1);
        provider->goToOffset(OtherOffset, other.size());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), OtherOffset, 2000);
        QVERIFY(provider->selectionStartOffset() != TailOffset);
        const QModelIndex tailIndex = model->index(tailRow, SearchResultsModel::COL_CONTEXT);
        tree->scrollTo(tailIndex, QAbstractItemView::PositionAtCenter);
        QTRY_VERIFY_WITH_TIMEOUT(tree->visualRect(tailIndex).isValid(), 2000);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                         tree->visualRect(tailIndex).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          tree->visualRect(tailIndex).center());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), TailOffset, 2000);
        QCOMPARE(provider->selectionEndOffset(), TailOffset + 1);
        const qsizetype beforeCached = ownedTasks.size();
        wrap->setChecked(false);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset);
        QVERIFY(status->text().contains(QStringLiteral("未找到")));
        wrap->setChecked(true);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), qint64(0));
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset);
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset - 2);
        QCOMPARE(ownedTasks.size(), beforeCached);
        QVERIFY(!source->isReady());
        QVERIFY(guards());
        pattern->setText(QString::fromLatin1(other));
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled()
            && provider->selectionStartOffset() == OtherOffset, 8000);
        QCOMPARE(provider->selectionEndOffset(), OtherOffset + other.size());
        int preparations = 0;
        int navigations = 0;
        for (qsizetype i = beforeCached; i < ownedTasks.size(); ++i) {
            const AsyncTask task = manager.taskById(ownedTasks.at(i));
            if (task.categoryTag == QStringLiteral("Mmap")) ++preparations;
            if (task.categoryTag == QStringLiteral("SearchNavigation")) ++navigations;
            QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Finished, 2000);
        }
        QCOMPARE(preparations, 1);
        QCOMPARE(navigations, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!source->isReady(), 2000);
        QVERIFY(guards());
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel *>(model.data()));
        QCOMPARE(model->totalMatches(), qint64(HitCount));
        QCOMPARE(model->groupedLineAt(tailRow).firstByteOffset, TailOffset);
        QCOMPARE(model->groupedLineAt(tailRow).matchByteRanges, tail.matchByteRanges);
        QCOMPARE(fileHash(spoolPath), spoolHash);
        QCOMPARE(provider->sourceBytes(OtherOffset, other.size()), other);
        for (qint64 id : std::as_const(ownedTasks)) {
            const QString category = manager.taskById(id).categoryTag;
            if (category != QStringLiteral("Search") && category != QStringLiteral("SearchNavigation")
                && category != QStringLiteral("Mmap")) continue;
            int count = 0;
            for (const auto &args : terminals)
                if (args.constFirst().toLongLong() == id) ++count;
            QCOMPARE(count, 1);
        }
    }
    void partialBatchCancellationSurvivesDockReopenAndFreshQuery()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const bool hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray oldProfile = qgetenv("NOTEPADSAFE_DATADIR");
        const QString profile = fixture.filePath(QStringLiteral("partial-profile"));
        QVERIFY(QDir().mkpath(profile));
        QVERIFY(qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8()));
        const auto restoreProfile = qScopeGuard([&] {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", oldProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        constexpr qint64 SourceSize = 33LL * 1024 * 1024;
        constexpr int BlockSize = 1024;
        constexpr int FullCount = int(SourceSize / BlockSize);
        constexpr qint64 MarkerOffset = 64;
        const QByteArray marker("PARTIAL_A_283");
        const QByteArray fresh("FRESH_B_641");
        const QVector<qint64> freshOffsets{SourceSize / 2 + 512, SourceSize - BlockSize + 512};
        QByteArray block(BlockSize, 'p');
        // Uniform nonzero controls exceed the real binary-probe threshold;
        // this is B2 Binary content shown as read-only Text, not S2 plain text.
        block.replace(0, 48, QByteArray(48, char(1)));
        for (int offset = 127; offset < BlockSize; offset += 128) block[offset] = '\n';
        block.replace(MarkerOffset, marker.size(), marker);
        QByteArray bytes = block.repeated(FullCount);
        for (qint64 offset : freshOffsets) bytes.replace(offset, fresh.size(), fresh);
        QCOMPARE(bytes.size(), SourceSize);
        QCOMPARE(bytes.count(marker), qsizetype(FullCount));
        QCOMPARE(bytes.count(fresh), qsizetype(2));
        for (int index = 0; index < FullCount; ++index)
            QCOMPARE(bytes.mid(qint64(index) * BlockSize + MarkerOffset, marker.size()), marker);
        const QString path = fixture.filePath(QStringLiteral("partial-binary-source.dat"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), SourceSize);
        }
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->count() == 1);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && target->isBinaryDocument() && target->isHexPresentation()
                && target->isReadOnlyByPolicy() && !target->isSecureDocument());
        auto *textAction = window.findChild<QAction *>(QStringLiteral("view.presentation.text"));
        QVERIFY(textAction && textAction->isEnabled());
        textAction->trigger();
        QVERIFY(target->isBinaryDocument() && !target->isHexPresentation() && target->isReadOnlyByPolicy());
        QPointer<WindowedReadOnlyProvider> provider = target->windowedReadOnlyProvider();
        QVERIFY(provider && provider->hasSource() && provider->presentation() == WindowedReadOnlyProvider::Presentation::Text);
        QCOMPARE(target->displayEncoding(), TextEncoding::Utf8);
        QCOMPARE(target->contentSizeBytes(), SourceSize);
        const DocumentState initial = target->documentState();
        const QString initialProjection = target->editor()->text();
        const bool initialProjectionModified = target->editor()->isModified();
        const qint64 initialSelectionStart = provider->selectionStartOffset();
        const qint64 initialSelectionEnd = provider->selectionEndOffset();
        QVERIFY(!initial.id.isNull() && !initial.dirty && !target->isModified());
        QVERIFY(!initial.capabilities.testFlag(DocumentCapability::Edit)
                && !initial.capabilities.testFlag(DocumentCapability::Save));
        const auto guards = [&] {
            if (!target || !provider) return false;
            const DocumentState state = target->documentState();
            return tabs->count() == 1 && tabs->currentWidget() == target.data()
                && target->windowedReadOnlyProvider() == provider.data()
                && state.id == initial.id && state.revision == initial.revision
                && state.location == initial.location && state.origin == initial.origin
                && state.persistence == initial.persistence && state.contentKind == initial.contentKind
                && state.securityClass == initial.securityClass && state.capabilities == initial.capabilities
                && state.dirty == initial.dirty && !target->isModified() && target->isReadOnlyByPolicy()
                && target->isBinaryDocument() && !target->isHexPresentation() && !target->isSecureDocument()
                && target->displayEncoding() == TextEncoding::Utf8 && target->contentSizeBytes() == SourceSize
                && target->editor() == provider->editor() && target->editor()->isReadOnly()
                && target->editor()->text() == initialProjection
                && target->editor()->isModified() == initialProjectionModified
                && provider->selectionStartOffset() == initialSelectionStart
                && provider->selectionEndOffset() == initialSelectionEnd
                && fileHash(path) == expectedHash && !QApplication::activeModalWidget();
        };
        QVERIFY(guards());
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        auto *dockAction = window.findChild<QAction *>(QStringLiteral("view.panel.searchResults"));
        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dockAction && dock);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog && dialog->isVisible());
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *all = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *closeFind = dialog->findChild<QPushButton *>(QStringLiteral("closeSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(pattern && scope && regex && hex && caseSensitive && wholeWord && all && cancel && closeFind && tree && summary);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        hex->setChecked(false);
        caseSensitive->setChecked(true);
        wholeWord->setChecked(false);
        pattern->setText(QString::fromLatin1(marker));

        auto &manager = AsyncTaskManager::instance();
        QSignalSpy managerTerminals(&manager, &AsyncTaskManager::taskFinished);
        QSignalSpy cancelClicks(cancel, &QPushButton::clicked);
        QVector<qint64> ownedTasks;
        QVector<qint64> searchTasks;
        int aDomainTerminals = 0;
        int bDomainTerminals = 0;
        int realPartialBatches = 0;
        int rowsAtCancel = 0;
        qint64 matchesAtCancel = 0;
        bool attemptedCancel = false;
        QString observationFailure;
        QObject observation;
        const auto terminalCount = [&](qint64 id) {
            int count = 0;
            for (const auto &arguments : managerTerminals)
                if (arguments.constFirst().toLongLong() == id) ++count;
            return count;
        };
        const auto ownedSettled = [&] {
            for (qint64 id : std::as_const(ownedTasks)) {
                const auto task = manager.taskById(id);
                if (task.id != id || task.state == AsyncTaskState::Queued || task.state == AsyncTaskState::Running) return false;
            }
            return true;
        };
        const auto cleanup = qScopeGuard([&] {
            for (qint64 id : std::as_const(ownedTasks)) manager.cancel(id);
            if (!QTest::qWaitFor(ownedSettled, 3000)) qWarning("Partial-result fixture owned-task drain exceeded 3 seconds");
            if (target) target->shutdownL2BeforeClose();
        });
        connect(target.data(), &DocumentSearchSurface::findAllL2Partial, &observation,
            [&](const QVector<LargeFileSearchMatch> &batch, const QString &query, const QString &, TextEncoding, bool) {
                if (query == QString::fromLatin1(marker) && !batch.isEmpty()) ++realPartialBatches;
            }, Qt::DirectConnection);
        connect(target.data(), &DocumentSearchSurface::findAllL2Finished, &observation,
            [&](SearchResultsModel *, const QString &query, const QString &) {
                if (query == QString::fromLatin1(marker)) ++aDomainTerminals;
                if (query == QString::fromLatin1(fresh)) ++bDomainTerminals;
            }, Qt::DirectConnection);
        connect(&manager, &AsyncTaskManager::taskAdded, &observation, [&](qint64 id) {
            const AsyncTask task = manager.taskById(id);
            if (!task.title.contains(QFileInfo(path).fileName())) return;
            ownedTasks.append(id);
            if (task.categoryTag != QStringLiteral("Search")) return;
            searchTasks.append(id);
            if (searchTasks.size() != 1) return;
            // A starts from a real cold source. Its asynchronous Mmap has
            // returned, the Dock preview exists, and Search is not enqueued
            // until taskAdded returns. No scheduler setting or hook is changed.
            auto *preview = qobject_cast<SearchResultsModel *>(tree->model());
            if (!preview || preview->totalMatches() != 0) {
                observationFailure = QStringLiteral("Initial live preview was not available before A entered its worker");
                return;
            }
            connect(preview, &QAbstractItemModel::modelReset, &observation, [&, preview] {
                if (attemptedCancel || preview->totalMatches() == 0) return;
                attemptedCancel = true;
                rowsAtCancel = preview->rowCount();
                matchesAtCancel = preview->totalMatches();
                if (realPartialBatches == 0 || tree->model() != preview || rowsAtCancel <= 0
                    || !dock->isVisible() || !cancel->isVisible() || !cancel->isEnabled()
                    || !target || target->documentState().id != initial.id
                    || target->contentRevision() != initial.revision) {
                    observationFailure = QStringLiteral("First real nonempty preview did not expose the active Cancel control");
                    return;
                }
                // Only real button input while endResetModel emits its normal
                // signal: no model mutation, event pumping, worker pause, or
                // full-file hash calculation in this timing-sensitive callback.
                const QPoint point = cancel->rect().center();
                const QPointF global(cancel->mapToGlobal(point));
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), global,
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), global,
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(cancel, &press);
                QApplication::sendEvent(cancel, &release);
            }, Qt::DirectConnection);
        }, Qt::DirectConnection);
        auto *coldSource = target->findChild<ReadOnlySearchSource *>();
        QVERIFY(!coldSource || !coldSource->isReady());
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!observationFailure.isEmpty() || attemptedCancel || aDomainTerminals > 0, 15000);
        QVERIFY2(observationFailure.isEmpty(), qPrintable(observationFailure));
        QVERIFY2(attemptedCancel, "CANCEL_WINDOW_NOT_ESTABLISHED: A finished before a cancellable nonempty preview was observed");
        QCOMPARE(cancelClicks.size(), qsizetype(1));
        QCOMPARE(searchTasks.size(), qsizetype(1));
        const qint64 taskA = searchTasks.constFirst();
        QTRY_VERIFY_WITH_TIMEOUT(aDomainTerminals == 1 && terminalCount(taskA) == 1
                                && all->isEnabled() && pattern->isEnabled() && !cancel->isVisible(), 15000);
        QVERIFY(guards()); // Data guards precede the cancellation-window claim.
        QPointer<SearchResultsModel> partial = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(partial);
        const qint64 retainedCount = partial->totalMatches();
        qInfo() << "PARTIAL_CANCEL_WINDOW" << "BinaryContent/ReadOnlyText" << "task=" << taskA
                << "firstRows=" << rowsAtCancel << "firstMatches=" << matchesAtCancel
                << "retained=" << retainedCount << "oracle=" << FullCount
                << "incomplete=" << partial->isIncomplete() << "managerState=" << int(manager.taskById(taskA).state)
                << "domainTerminals=" << aDomainTerminals << "managerTerminals=" << terminalCount(taskA);
        if (retainedCount >= FullCount)
            QFAIL("CANCEL_WINDOW_NOT_ESTABLISHED: the real worker produced the full oracle before cancellation; this is not a partial-cancel pass");
        QVERIFY(matchesAtCancel > 0 && rowsAtCancel > 0 && retainedCount > 0);
        QVERIFY(partial->isIncomplete());
        QCOMPARE(manager.taskById(taskA).state, AsyncTaskState::Canceled);
        const QString incompleteReason = partial->incompleteReason();
        QVERIFY(incompleteReason.contains(QStringLiteral("取消")) || incompleteReason.contains(QStringLiteral("停止")));
        QVERIFY(partial->incompleteUserReason().contains(QStringLiteral("不完整")));
        const QString retainedSummary = summary->text();
        QVERIFY(retainedSummary.contains(QStringLiteral("至少 %1 处").arg(retainedCount))
                && retainedSummary.contains(QStringLiteral("不完整")));
        QCOMPARE(partial->rowCount(), int(retainedCount));
        const auto partialRangesCorrect = [&] {
            if (!partial || partial->totalMatches() != retainedCount || partial->rowCount() != retainedCount) return false;
            for (int row = 0; row < partial->rowCount(); ++row) {
                const QModelIndex match = partial->index(row, SearchResultsModel::COL_CONTEXT);
                if (partial->matchByteOffset(match) != qint64(row) * BlockSize + MarkerOffset
                    || partial->matchLength(match) != marker.size()) return false;
            }
            return true;
        };
        QVERIFY(partialRangesCorrect());
        QPointer<ReadOnlySearchSource> source = target->findChild<ReadOnlySearchSource *>();
        QVERIFY(source);
        QTRY_VERIFY_WITH_TIMEOUT(!source->isReady() && ownedSettled(), 2000);
        QVERIFY(closeFind->isVisible() && closeFind->isEnabled());
        QTest::mouseClick(closeFind, Qt::LeftButton);
        QVERIFY(!dialog->isVisible());
        QVERIFY(dock->isVisible() && dockAction->isChecked());
        dockAction->trigger();
        QVERIFY(!dock->isVisible() && !dockAction->isChecked());
        QVERIFY(guards() && partialRangesCorrect() && partial->isIncomplete());
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel *>(partial.data()));
        dockAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible() && dockAction->isChecked(), 2000);
        QCOMPARE(window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock")), dock);
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel *>(partial.data()));
        QVERIFY(guards() && partialRangesCorrect() && partial->isIncomplete());
        QCOMPARE(partial->incompleteReason(), incompleteReason);
        QCOMPARE(summary->text(), retainedSummary);
        QCOMPARE(searchTasks.size(), qsizetype(1));
        QCOMPARE(aDomainTerminals, 1);
        QCOMPARE(terminalCount(taskA), 1);

        findAction->trigger();
        QVERIFY(dialog->isVisible() && window.findChild<FindReplaceDialog *>() == dialog);
        QVERIFY(pattern->isEnabled() && all->isEnabled());
        pattern->setText(QString::fromLatin1(fresh));
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(searchTasks.size(), qsizetype(2), 3000);
        const qint64 taskB = searchTasks.constLast();
        QVERIFY(taskB > 0 && taskB != taskA);
        QTRY_VERIFY_WITH_TIMEOUT(bDomainTerminals == 1 && terminalCount(taskB) == 1 && ownedSettled()
                                && all->isEnabled() && pattern->isEnabled() && !cancel->isVisible(), 15000);
        bool deliveryTail = false;
        QObject fence;
        QMetaObject::invokeMethod(&fence, [&] {
            QMetaObject::invokeMethod(&fence, [&] { deliveryTail = true; }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        QVERIFY(QTest::qWaitFor([&] { return deliveryTail; }, 1000));
        QVERIFY(guards());
        QCOMPARE(searchTasks.size(), qsizetype(2));
        QCOMPARE(aDomainTerminals, 1);
        QCOMPARE(bDomainTerminals, 1);
        QCOMPARE(terminalCount(taskA), 1);
        QCOMPARE(terminalCount(taskB), 1);
        QCOMPARE(manager.taskById(taskA).state, AsyncTaskState::Canceled);
        QCOMPARE(manager.taskById(taskB).state, AsyncTaskState::Finished);
        auto *freshModel = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(freshModel && !freshModel->isIncomplete());
        QCOMPARE(freshModel->totalMatches(), qint64(freshOffsets.size()));
        QCOMPARE(freshModel->rowCount(), int(freshOffsets.size()));
        for (int row = 0; row < freshOffsets.size(); ++row) {
            const QModelIndex match = freshModel->index(row, SearchResultsModel::COL_CONTEXT);
            QCOMPARE(freshModel->matchByteOffset(match), freshOffsets.at(row));
            QCOMPARE(freshModel->matchLength(match), int(fresh.size()));
            QCOMPARE(provider->sourceBytes(freshOffsets.at(row), fresh.size()), fresh);
        }
        QCOMPARE(pattern->text(), QString::fromLatin1(fresh));
        QVERIFY(summary->text().contains(QString::fromLatin1(fresh))
                && !summary->text().contains(QString::fromLatin1(marker)));
        QVERIFY(guards());
        qInfo() << "PARTIAL_DOCK_REOPEN_FRESH_QUERY" << "A=" << taskA << "retained=" << retainedCount
                << "B=" << taskB << "matches=" << freshModel->totalMatches() << "sourceHash=" << expectedHash.toHex();
    }
};

QTEST_MAIN(SmokeReadOnlySearchSource)
#include "smoke_readonly_search_source.moc"
