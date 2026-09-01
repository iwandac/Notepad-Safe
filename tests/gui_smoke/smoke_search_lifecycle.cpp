#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>

#include <atomic>
#include <memory>
#include <utility>

#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"

namespace {

// Test-owned workload, not a pool configuration override or search result
// double. Every load uses the production scheduler and checks cancellation.
class OwnedSearchTasks final {
public:
    explicit OwnedSearchTasks(QObject *context, const QString &fileName)
    {
        added_ = QObject::connect(
            &manager_, &AsyncTaskManager::taskAdded, context,
            [this, fileName](qint64 id) {
                const AsyncTask task = manager_.taskById(id);
                if (task.categoryTag == QStringLiteral("SearchFindAll")
                    && task.title.contains(fileName)) {
                    searches_.append(id);
                    owned_.append(id);
                }
            });
    }

    ~OwnedSearchTasks()
    {
        QObject::disconnect(added_);
        releaseLoads();
        for (qint64 id : std::as_const(owned_))
            manager_.cancel(id);
        if (!waitUntilSettled(3000))
            qWarning("search lifecycle cleanup exceeded its owned-task deadline");
    }

    bool occupyGeneralPool()
    {
        // AsyncThreadPool reserves one of maximumThreadCount() for security.
        // Do not change either pool's capacity or cancel unrelated tasks.
        const int count = manager_.maximumThreadCount() - 1;
        if (count < 1)
            return false;
        for (int index = 0; index < count; ++index) {
            const auto state = loads_;
            const qint64 id = manager_.submit(
                AsyncTaskType::Other, AsyncTaskPriority::High,
                QStringLiteral("搜索生命周期可释放负载 %1").arg(index),
                QStringLiteral("占用普通工作线程，验证真实排队与取消"),
                [state](qint64, AsyncCancelPtr cancel,
                        const std::function<void(qint64, qint64)> &,
                        const std::function<void(const AsyncErrorInfo &)> &) {
                    state->entered.fetch_add(1, std::memory_order_release);
                    const auto exited = qScopeGuard([state]() {
                        state->exited.fetch_add(1, std::memory_order_release);
                    });
                    while (!state->released.load(std::memory_order_acquire)
                           && !(cancel && cancel->load(std::memory_order_acquire))) {
                        QThread::msleep(2);
                    }
                }, 0, QStringLiteral("SearchLifecycleTestLoad"));
            if (id <= 0)
                return false;
            owned_.append(id);
            loadIds_.append(id);
        }
        return true;
    }

    bool allLoadsRunning() const
    {
        if (loadIds_.isEmpty()
            || loads_->entered.load(std::memory_order_acquire) != loadIds_.size())
            return false;
        for (qint64 id : loadIds_) {
            if (manager_.taskById(id).state != AsyncTaskState::Running)
                return false;
        }
        return true;
    }

    auto releaseLoads() -> void
    {
        loads_->released.store(true, std::memory_order_release);
    }

    bool waitUntilSettled(int timeoutMs) const
    {
        QElapsedTimer deadline;
        deadline.start();
        do {
            // Domain completions are queued to live UI objects. Drain them
            // before MainWindow/EditorTab teardown, including on QVERIFY exits.
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            bool settled = loads_->entered.load(std::memory_order_acquire)
                == loads_->exited.load(std::memory_order_acquire);
            for (qint64 id : owned_) {
                const AsyncTask task = manager_.taskById(id);
                settled = settled && task.id == id
                    && task.state != AsyncTaskState::Queued
                    && task.state != AsyncTaskState::Running;
            }
            if (settled)
                return true;
            QThread::msleep(1);
        } while (deadline.elapsed() < timeoutMs);
        return false;
    }

    const QVector<qint64> &searches() const { return searches_; }
    void track(qint64 id) { owned_.append(id); }

private:
    struct LoadState {
        std::atomic_bool released{false};
        std::atomic_int entered{0};
        std::atomic_int exited{0};
    };
    AsyncTaskManager &manager_ = AsyncTaskManager::instance();
    std::shared_ptr<LoadState> loads_ = std::make_shared<LoadState>();
    QVector<qint64> owned_;
    QVector<qint64> loadIds_;
    QVector<qint64> searches_;
    QMetaObject::Connection added_;
};

} // namespace

class SmokeSearchLifecycle : public QObject {
    Q_OBJECT

private slots:
    void l1QueuedNavigationCancelSettlesAndAllowsImmediateRerun()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreDataDir = qScopeGuard([&]() {
            if (hadDataDir) qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        const QString path = temporary.filePath(QStringLiteral("queued-navigation-l1.txt"));
        const QByteArray queryA("L1_QUEUED_NAV_A");
        const QByteArray queryB("L1_QUEUED_NAV_B");
        constexpr qint64 offsetA = 4096;
        constexpr qint64 offsetB = 8192;
        QByteArray original(512 * 1024, 'x');
        for (qsizetype offset = 127; offset < original.size(); offset += 128)
            original[offset] = '\n';
        original.replace(offsetA, queryA.size(), queryA);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(original), qint64(original.size()));
        }
        const auto sourceBytes = [&]() {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const auto sourceHash = [&]() {
            return QCryptographicHash::hash(sourceBytes(), QCryptographicHash::Sha256);
        };
        QCOMPARE(sourceBytes(), original);
        const QByteArray hashBefore = sourceHash();

        MainWindow window;
        const auto cleanupWindow = qScopeGuard([&]() {
            if (auto *tabs = window.findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page))
                        tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
        });
        // On every early return, release/cancel/drain this case's tasks before
        // document destruction. Existing Find All helper behavior is unchanged.
        OwnedSearchTasks tasks(&window, QFileInfo(path).fileName());
        window.resize(1200, 800);
        window.showNormal();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 3000);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && !target->isL2Mode());
        QCOMPARE(target->editor()->text().toUtf8(), original);
        // B exists only in the unsaved snapshot, at a manually fixed byte
        // offset. No Find All or navigation warmup can populate a cache.
        target->editor()->setCursorPosition(64, 0);
        target->editor()->setFocus();
        QTest::keyClicks(target->editor(), QString::fromLatin1(queryB));
        const QByteArray dirtyBytes = original.left(offsetB) + queryB + original.mid(offsetB);
        QCOMPARE(target->editor()->text().toUtf8(), dirtyBytes);
        QVERIFY(target->isModified());
        const QString bodyBefore = target->editor()->text();
        const DocumentState stateBefore = target->documentState();
        const auto encodingBefore = target->encoding();
        target->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL, 128UL, 132L);
        const auto selectionStart = [&]() -> qint64 {
            return target->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
        };
        const auto selectionEnd = [&]() -> qint64 {
            return target->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
        };
        QCOMPARE(selectionStart(), qint64(128));
        QCOMPARE(selectionEnd(), qint64(132));
        const auto dataGuards = [&]() {
            return target && tabs->currentWidget() == target.data()
                && !target->isL2Mode() && target->documentState().id == stateBefore.id
                && target->contentRevision() == stateBefore.revision
                && target->documentState().capabilities == stateBefore.capabilities
                && target->documentState().location == stateBefore.location
                && target->encoding() == encodingBefore
                && target->isModified() && target->documentState().dirty
                && target->editor()->text() == bodyBefore
                && sourceBytes() == original && sourceHash() == hashBefore;
        };

        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        QVERIFY(findAction);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        QVERIFY(pattern && scope && regex && wholeWord && caseSensitive
                && wrap && next && previous && findAll && status);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        wholeWord->setChecked(false);
        caseSensitive->setChecked(true);
        wrap->setChecked(false);
        pattern->setText(QString::fromLatin1(queryA));

        auto &manager = AsyncTaskManager::instance();
        QVector<qint64> navigationIds;
        const QString navigationTitle = QStringLiteral("查找下一个: %1").arg(QFileInfo(path).fileName());
        const auto navigationAdded = connect(
            &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (task.categoryTag == QStringLiteral("SearchNavigation")
                    && task.title == navigationTitle) {
                    navigationIds.append(id);
                    tasks.track(id);
                }
            }, Qt::DirectConnection);
        const auto disconnectNavigationAdded = qScopeGuard([&]() {
            QObject::disconnect(navigationAdded);
        });
        QSignalSpy finished(&manager, &AsyncTaskManager::taskFinished);
        QVERIFY(tasks.occupyGeneralPool());
        QTRY_VERIFY_WITH_TIMEOUT(tasks.allLoadsRunning(), 3000);
        const auto canaryEntered = std::make_shared<std::atomic_int>(0);
        const qint64 canaryId = manager.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Highest,
            QStringLiteral("L1 定向导航排队验收"), QString(),
            [canaryEntered](qint64, AsyncCancelPtr, const auto &, const auto &) {
                canaryEntered->fetch_add(1, std::memory_order_release);
            }, 0, QStringLiteral("L1NavigationQueueCanary"));
        QVERIFY(canaryId > 0);
        tasks.track(canaryId);
        const auto poolHeld = [&]() {
            const AsyncTask canary = manager.taskById(canaryId);
            return tasks.allLoadsRunning() && canary.id == canaryId
                && canary.priority == AsyncTaskPriority::Highest
                && canary.state == AsyncTaskState::Queued && !canary.startedAt.isValid()
                && canaryEntered->load(std::memory_order_acquire) == 0;
        };
        const auto exactQueuedNavigation = [&](qint64 id) {
            const AsyncTask task = manager.taskById(id);
            return task.id == id && task.type == AsyncTaskType::LargeFileSearch
                && task.categoryTag == QStringLiteral("SearchNavigation")
                && task.priority == AsyncTaskPriority::Highest
                && task.state == AsyncTaskState::Queued && !task.startedAt.isValid()
                && task.progressPhase.isEmpty() && task.progressDone == 0;
        };
        QVERIFY(poolHeld() && dataGuards());
        QVERIFY(next->isEnabled());
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(navigationIds.size(), qsizetype(1));
        const qint64 taskA = navigationIds.at(0);
        QVERIFY(poolHeld() && exactQueuedNavigation(taskA));
        // Navigation blocks another directional command, but deliberately
        // leaves the query editable and Find All independent (existing UI contract).
        QVERIFY(!next->isEnabled() && !previous->isEnabled() && pattern->isEnabled());
        QVERIFY(dataGuards());
        QCOMPARE(selectionStart(), qint64(128));
        QCOMPARE(selectionEnd(), qint64(132));

        auto *taskButton = window.findChild<QToolButton *>(QStringLiteral("StatusBarTaskWidget_btn"));
        QVERIFY(taskButton && taskButton->isVisible());
        QTest::mouseClick(taskButton, Qt::LeftButton);
        auto *panel = window.findChild<AsyncTaskPanelDialog *>();
        QVERIFY(panel && QTest::qWaitForWindowExposed(panel));
        auto *list = panel->findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        auto *cancel = panel->findChild<QPushButton *>(QStringLiteral("cancelSelectedTaskButton"));
        QVERIFY(list && list->model() && cancel);
        QModelIndex taskRow;
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            const QModelIndex candidate = list->model()->index(row, 0);
            if (candidate.data(AsyncTaskManager::TaskIdRole).toLongLong() == taskA) {
                taskRow = candidate;
                break;
            }
        }
        QVERIFY(taskRow.isValid());
        list->setCurrentIndex(taskRow);
        list->scrollTo(taskRow);
        QVERIFY(cancel->isEnabled());
        QVERIFY(taskRow.data(AsyncTaskManager::TaskCancellableRole).toBool());
        QVERIFY(poolHeld() && exactQueuedNavigation(taskA));
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskA).state, AsyncTaskState::Canceled, 1000);
        // Safety must be reached even on the original missing-UI-completion
        // implementation. Keep every load held until B is truly submitted.
        QVERIFY(poolHeld() && dataGuards());
        QVERIFY(!manager.taskById(taskA).startedAt.isValid());
        QCOMPARE(selectionStart(), qint64(128));
        QCOMPARE(selectionEnd(), qint64(132));
        qInfo("L1_NAV_CANCEL_GUARDS_OK task=%lld", static_cast<long long>(taskA));
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && previous->isEnabled()
                                     && findAll->isEnabled() && pattern->isEnabled(), 1000);
        QVERIFY(status->text().contains(QStringLiteral("取消"))
                || status->text().contains(QStringLiteral("停止")));
        panel->hide();
        dialog->raise();
        pattern->setText(QString::fromLatin1(queryB));
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(navigationIds.size(), qsizetype(2));
        const qint64 taskB = navigationIds.at(1);
        QVERIFY(taskB != taskA);
        QVERIFY(poolHeld() && exactQueuedNavigation(taskB));
        QVERIFY(!next->isEnabled() && !previous->isEnabled() && pattern->isEnabled());
        QVERIFY(dataGuards());
        QCOMPARE(selectionStart(), qint64(128));
        QCOMPARE(selectionEnd(), qint64(132));
        const auto pendingTailDrained = std::make_shared<bool>(false);
        QTimer::singleShot(0, &window, [&, pendingTailDrained]() {
            QTimer::singleShot(0, &window, [pendingTailDrained]() { *pendingTailDrained = true; });
        });
        QTRY_VERIFY_WITH_TIMEOUT(*pendingTailDrained, 1000);
        QVERIFY(poolHeld() && exactQueuedNavigation(taskB));
        QVERIFY(!next->isEnabled() && !previous->isEnabled() && pattern->isEnabled());
        QCOMPARE(selectionStart(), qint64(128));
        QCOMPARE(selectionEnd(), qint64(132));
        manager.cancel(canaryId);
        tasks.releaseLoads();

        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskB).state, AsyncTaskState::Finished, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && pattern->isEnabled(), 3000);
        QVERIFY(dataGuards());
        QTRY_COMPARE_WITH_TIMEOUT(selectionStart(), offsetB, 3000);
        QCOMPARE(selectionEnd(), offsetB + queryB.size());
        QCOMPARE(target->editor()->selectedText(), QString::fromLatin1(queryB));
        QVERIFY(target->lastSearchError().isEmpty() && status->text().isEmpty());
        QVERIFY(tasks.waitUntilSettled(3000));
        const auto completedTailDrained = std::make_shared<bool>(false);
        QTimer::singleShot(0, &window, [&, completedTailDrained]() {
            QTimer::singleShot(0, &window, [completedTailDrained]() { *completedTailDrained = true; });
        });
        QTRY_VERIFY_WITH_TIMEOUT(*completedTailDrained, 1000);
        int aTerminals = 0;
        int bTerminals = 0;
        for (const QList<QVariant> &notification : finished) {
            const qint64 id = notification.at(0).toLongLong();
            if (id == taskA) {
                ++aTerminals;
                QCOMPARE(qvariant_cast<AsyncTaskState>(notification.at(1)), AsyncTaskState::Canceled);
            } else if (id == taskB) {
                ++bTerminals;
                QCOMPARE(qvariant_cast<AsyncTaskState>(notification.at(1)), AsyncTaskState::Finished);
            }
        }
        QCOMPARE(aTerminals, 1);
        QCOMPARE(bTerminals, 1);
        QCOMPARE(navigationIds.size(), qsizetype(2));
        QCOMPARE(manager.taskById(taskA).state, AsyncTaskState::Canceled);
        QCOMPARE(canaryEntered->load(std::memory_order_acquire), 0);
        QVERIFY(dataGuards());
        QCOMPARE(selectionStart(), offsetB);
        QCOMPARE(selectionEnd(), offsetB + queryB.size());
        QCOMPARE(target->editor()->selectedText(), QString::fromLatin1(queryB));
        QCOMPARE(pattern->text(), QString::fromLatin1(queryB));
        QVERIFY(next->isEnabled() && pattern->isEnabled());
    }

    void l1QueuedCancelSettlesAndAllowsImmediateRerun_data()
    {
        QTest::addColumn<QString>("cancelTiming");
        QTest::newRow("afterSubmit") << QStringLiteral("afterSubmit");
        // Supplemental implementation-time boundary, not an original red run.
        QTest::newRow("taskAdded") << QStringLiteral("taskAdded");
    }

    void l1QueuedCancelSettlesAndAllowsImmediateRerun()
    {
        QFETCH(QString, cancelTiming);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreDataDir = qScopeGuard([&]() {
            if (hadDataDir)
                qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
            else
                qunsetenv("NOTEPADSAFE_DATADIR");
        });

        const QString path = temporary.filePath(QStringLiteral("queued-cancel-l1.txt"));
        const QByteArray original("FIRST_QUERY on disk\nFIRST_QUERY again\n");
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write(original), qint64(original.size()));
        }
        const auto sourceHash = [&]() {
            QFile source(path);
            if (!source.open(QIODevice::ReadOnly))
                return QByteArray{};
            return QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
        };
        const QByteArray hashBefore = sourceHash();
        QVERIFY(!hashBefore.isEmpty());

        MainWindow window;
        const auto cleanupWindow = qScopeGuard([&]() {
            if (auto *tabs = window.findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page))
                        tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
        });
        // Destruction order is intentional: release/cancel/drain owned work
        // first, then destroy document widgets, then restore the data directory.
        OwnedSearchTasks tasks(&window, QFileInfo(path).fileName());
        window.showNormal();
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 3000);
        auto *target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && !target->isL2Mode());
        target->editor()->append(QStringLiteral("RETRY_QUERY exists only in the unsaved document\n"));
        QVERIFY(target->isModified());
        const QString bodyBefore = target->editor()->text();
        const DocumentState stateBefore = target->documentState();

        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(pattern && scope && regex && findAll && cancel && status && tree && summary);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        pattern->setText(QStringLiteral("FIRST_QUERY"));

        QVERIFY(tasks.occupyGeneralPool());
        QTRY_VERIFY_WITH_TIMEOUT(tasks.allLoadsRunning(), 3000);
        auto &manager = AsyncTaskManager::instance();
        qint64 immediateCancelId = 0;
        int immediateCancelCount = 0;
        bool queryClickOnStack = false;
        bool immediatePreconditions = false;
        const auto cancelOnAdded = QObject::connect(
            &manager, &AsyncTaskManager::taskAdded, &window,
            [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (cancelTiming != QStringLiteral("taskAdded")
                    || !queryClickOnStack || immediateCancelCount != 0
                    || task.categoryTag != QStringLiteral("SearchFindAll")
                    || task.title != QStringLiteral("查找全部: %1").arg(QFileInfo(path).fileName())
                    || pattern->text() != QStringLiteral("FIRST_QUERY"))
                    return;
                ++immediateCancelCount;
                immediateCancelId = id;
                immediatePreconditions = task.id == id && task.state == AsyncTaskState::Queued
                    && !task.startedAt.isValid() && tasks.allLoadsRunning()
                    && cancel->isVisible() && cancel->isEnabled()
                    && !findAll->isEnabled() && !pattern->isEnabled();
                // submit() has emitted taskAdded but has not returned its ID
                // to EditorTab. Cancel this exact production task directly.
                manager.cancel(id);
            }, Qt::DirectConnection);
        const auto disconnectCancelOnAdded = qScopeGuard([&]() {
            QObject::disconnect(cancelOnAdded);
        });
        queryClickOnStack = true;
        QTest::mouseClick(findAll, Qt::LeftButton);
        queryClickOnStack = false;
        QTRY_COMPARE_WITH_TIMEOUT(tasks.searches().size(), qsizetype(1), 1000);
        const qint64 canceledId = tasks.searches().at(0);
        if (cancelTiming == QStringLiteral("afterSubmit")) {
            // Preserve the original red path through the real Cancel button.
            QCOMPARE(immediateCancelCount, 0);
            QCOMPARE(manager.taskById(canceledId).state, AsyncTaskState::Queued);
            QVERIFY(cancel->isVisible() && cancel->isEnabled());
            QVERIFY(!findAll->isEnabled() && !pattern->isEnabled());
            QTest::mouseClick(cancel, Qt::LeftButton);
        } else {
            QCOMPARE(immediateCancelCount, 1);
            QCOMPARE(immediateCancelId, canceledId);
            QVERIFY(immediatePreconditions);
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(canceledId).state,
                                  AsyncTaskState::Canceled, 1000);
        // Check document safety before the terminal-UI assertion so the queued
        // cancellation regression still reaches these guards on the old product.
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(target));
        QCOMPARE(target->documentState().id, stateBefore.id);
        QCOMPARE(target->contentRevision(), stateBefore.revision);
        QCOMPARE(target->documentState().capabilities, stateBefore.capabilities);
        QCOMPARE(target->editor()->text(), bodyBefore);
        QVERIFY(target->isModified());
        QCOMPARE(sourceHash(), hashBefore);
        // Keep all load workers held: cancellation must settle even though the
        // canceled search's worker function has never had a chance to execute.
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled()
                                     && pattern->isEnabled(), 1000);
        const QString canceledStatus = status->text() + summary->text();
        QVERIFY(canceledStatus.contains(QStringLiteral("取消"))
                || canceledStatus.contains(QStringLiteral("停止")));
        QVERIFY(!tree->model() || tree->model()->rowCount() == 0);

        pattern->setText(QStringLiteral("RETRY_QUERY"));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(tasks.searches().size(), qsizetype(2), 1000);
        const qint64 retryId = tasks.searches().at(1);
        QVERIFY(retryId != canceledId);
        QCOMPARE(manager.taskById(retryId).state, AsyncTaskState::Queued);
        QVERIFY(cancel->isVisible() && !findAll->isEnabled());
        tasks.releaseLoads();

        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(retryId).state,
                                  AsyncTaskState::Finished, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model() && tree->model()->rowCount() == 1, 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        // Drain late queued notifications without starting another query.
        QTest::qWait(100);
        auto *model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model && !model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(1));
        const QModelIndex result = model->index(0, SearchResultsModel::COL_CONTEXT);
        const QByteArray retryBytes("RETRY_QUERY");
        const qint64 expectedOffset = bodyBefore.toUtf8().indexOf(retryBytes);
        QVERIFY(expectedOffset >= 0);
        QCOMPARE(model->matchByteOffset(result), expectedOffset);
        QCOMPARE(model->matchLength(result), int(retryBytes.size()));
        QVERIFY(result.data(SearchResultsModel::ContextRole).toString().contains(QStringLiteral("RETRY_QUERY")));
        QVERIFY(!result.data(SearchResultsModel::ContextRole).toString().contains(QStringLiteral("FIRST_QUERY")));
        QVERIFY(summary->text().contains(QStringLiteral("RETRY_QUERY")));
        QVERIFY(!summary->text().contains(QStringLiteral("FIRST_QUERY")));
        QCOMPARE(pattern->text(), QStringLiteral("RETRY_QUERY"));
        QCOMPARE(manager.taskById(canceledId).state, AsyncTaskState::Canceled);
        QCOMPARE(tasks.searches().size(), qsizetype(2));
        QCOMPARE(immediateCancelCount, cancelTiming == QStringLiteral("taskAdded") ? 1 : 0);
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(target));
        QCOMPARE(target->documentState().id, stateBefore.id);
        QCOMPARE(target->contentRevision(), stateBefore.revision);
        QCOMPARE(target->documentState().capabilities, stateBefore.capabilities);
        QCOMPARE(target->editor()->text(), bodyBefore);
        QVERIFY(target->isModified());
        QCOMPARE(sourceHash(), hashBefore);
    }

    void l1LateCompletionPreservesNewSingleQuery()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreDataDir = qScopeGuard([&]() {
            if (hadDataDir) qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        const QString pathA = temporary.filePath(QStringLiteral("late-single-a.txt"));
        const QString pathB = temporary.filePath(QStringLiteral("late-single-b.txt"));
        const QByteArray bytesA("prefix\nLATE_QUERY_A_527 first\nLATE_QUERY_A_527 second\n");
        const QByteArray bytesB("header B remains on disk\n");
        const QByteArray queryA("LATE_QUERY_A_527");
        const QByteArray queryB("LIVE_QUERY_B_684");
        for (const auto &source : {qMakePair(pathA, bytesA), qMakePair(pathB, bytesB)}) {
            QFile file(source.first);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(source.second), qint64(source.second.size()));
        }
        const auto hash = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
            return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        };
        const QByteArray hashA = hash(pathA);
        const QByteArray hashB = hash(pathB);
        QVERIFY(!hashA.isEmpty() && !hashB.isEmpty());

        MainWindow window;
        const auto cleanupWindow = qScopeGuard([&]() {
            if (auto *tabs = window.findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page)) tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
        });
        OwnedSearchTasks tasks(&window, QStringLiteral("late-single-"));
        window.showNormal();
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({pathA, pathB});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        QPointer<EditorTab> targetA;
        QPointer<EditorTab> targetB;
        for (int index = 0; index < tabs->count(); ++index) {
            auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
            QVERIFY(tab && !tab->isL2Mode() && !tab->isSecureDocument());
            if (tab->filePath() == pathA) targetA = tab;
            if (tab->filePath() == pathB) targetB = tab;
        }
        QVERIFY(targetA && targetB);
        targetA->editor()->append(QStringLiteral("unsaved A tail\n"));
        targetB->editor()->append(QStringLiteral("LIVE_QUERY_B_684 first unsaved hit\nLIVE_QUERY_B_684 second unsaved hit\n"));
        QVERIFY(targetA->isModified() && targetB->isModified());
        const QString bodyA = targetA->editor()->text();
        const QString bodyB = targetB->editor()->text();
        const DocumentState stateA = targetA->documentState();
        const DocumentState stateB = targetB->documentState();
        const auto sourceGuards = [&]() {
            return targetA && targetB && tabs->count() == 2
                && targetA->documentState().id == stateA.id
                && targetB->documentState().id == stateB.id
                && targetA->contentRevision() == stateA.revision
                && targetB->contentRevision() == stateB.revision
                && targetA->documentState().capabilities == stateA.capabilities
                && targetB->documentState().capabilities == stateB.capabilities
                && targetA->editor()->text() == bodyA && targetB->editor()->text() == bodyB
                && targetA->isModified() && targetB->isModified()
                && hash(pathA) == hashA && hash(pathB) == hashB
                && !QApplication::activeModalWidget();
        };
        QVERIFY(sourceGuards());
        tabs->setCurrentWidget(targetA.data());
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(pattern && scope && regex && wholeWord && hex && findAll && cancel && tree && summary);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        wholeWord->setChecked(false);
        hex->setChecked(false);
        pattern->setText(QString::fromLatin1(queryA));
        auto &manager = AsyncTaskManager::instance();
        qint64 taskA = 0;
        qint64 taskB = 0;
        int aTerminalCount = 0;
        bool boundaryPassed = false;
        bool deliveryTailReached = false;
        QString boundaryFailure;
        // Destroy this before the referenced locals/owned-task drain. The two
        // dispatcher fences may still be queued if an assertion exits early.
        QObject fenceLifetime;
        const QPointer<QObject> fenceGuard(&fenceLifetime);
        // Connect BEFORE A's helper exists. Qt invokes this direct observer
        // before the helper consumes A's real successful Manager terminal.
        const auto beforeATerminal = QObject::connect(
            &manager, &AsyncTaskManager::taskFinished, &window,
            [&](qint64 id, AsyncTaskState terminal) {
                const AsyncTask task = manager.taskById(id);
                if (task.categoryTag != QStringLiteral("SearchFindAll")
                    || task.title != QStringLiteral("查找全部: %1").arg(QFileInfo(pathA).fileName()))
                    return;
                ++aTerminalCount;
                if (aTerminalCount != 1) return;
                taskA = id;
                const auto fail = [&](const QString &message) {
                    boundaryFailure = message;
                    return false;
                };
                boundaryPassed = [&]() -> bool {
                    if (terminal != AsyncTaskState::Finished || task.id != id
                        || !task.error.isNull() || tasks.searches().size() != 1
                        || tasks.searches().constFirst() != id || !sourceGuards())
                        return fail(QStringLiteral("A did not reach its exact real successful terminal with intact sources"));
                    if (!cancel->isVisible() || findAll->isEnabled())
                        return fail(QStringLiteral("A business completion was delivered before the ordered observer"));
                    if (!tasks.occupyGeneralPool())
                        return fail(QStringLiteral("Could not submit owned ordinary-pool loads"));
                    QElapsedTimer loading;
                    loading.start();
                    while (!tasks.allLoadsRunning() && loading.elapsed() < 3000) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                        QThread::msleep(1);
                    }
                    if (!tasks.allLoadsRunning())
                        return fail(QStringLiteral("Owned load workers did not actually enter"));
                    // A's helper has not seen this terminal yet, even while
                    // the nested event loop observes the real load workers.
                    tabs->setCurrentWidget(targetB.data());
                    if (!pattern->isEnabled() || !findAll->isEnabled())
                        return fail(QStringLiteral("Switching target did not permit a new current-document query"));
                    pattern->setText(QString::fromLatin1(queryB));
                    QTest::mouseClick(findAll, Qt::LeftButton);
                    if (tasks.searches().size() != 2)
                        return fail(QStringLiteral("B was not actually submitted by the real Find All control"));
                    taskB = tasks.searches().at(1);
                    const AsyncTask queuedB = manager.taskById(taskB);
                    if (taskB <= 0 || taskB == taskA || queuedB.id != taskB
                        || queuedB.categoryTag != QStringLiteral("SearchFindAll")
                        || queuedB.title != QStringLiteral("查找全部: %1").arg(QFileInfo(pathB).fileName())
                        || queuedB.priority != AsyncTaskPriority::High
                        || queuedB.state != AsyncTaskState::Queued || queuedB.startedAt.isValid()
                        || !tasks.allLoadsRunning() || !cancel->isVisible()
                        || findAll->isEnabled() || pattern->isEnabled() || !sourceGuards())
                        return fail(QStringLiteral("B lacks exact queued-task/busy/source evidence before A delivery"));
                    // First fence is posted before the helper's completion;
                    // its second hop is appended after that completion. No
                    // fabricated search result or guessed timing is needed.
                    return QMetaObject::invokeMethod(&manager,
                        [fenceGuard, &manager, &deliveryTailReached]() {
                            if (!fenceGuard) return;
                            QMetaObject::invokeMethod(&manager,
                                [fenceGuard, &deliveryTailReached]() {
                                    if (fenceGuard) deliveryTailReached = true;
                                }, Qt::QueuedConnection);
                        }, Qt::QueuedConnection);
                }();
            }, Qt::DirectConnection);
        const auto disconnectBeforeATerminal = qScopeGuard([&]() {
            QObject::disconnect(beforeATerminal);
        });
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(aTerminalCount > 0, 5000);
        QVERIFY2(boundaryPassed, qPrintable(boundaryFailure));
        QTRY_VERIFY_WITH_TIMEOUT(deliveryTailReached, 1500);
        // Safety guards deliberately precede the expected old-query UI red.
        QVERIFY(sourceGuards());
        QCOMPARE(manager.taskById(taskA).state, AsyncTaskState::Finished);
        QCOMPARE(manager.taskById(taskB).state, AsyncTaskState::Queued);
        QVERIFY(!manager.taskById(taskB).startedAt.isValid() && tasks.allLoadsRunning());
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(targetB.data()));
        QCOMPARE(pattern->text(), QString::fromLatin1(queryB));
        QCOMPARE(scope->currentData().toInt(), 0);
        QVERIFY2(cancel->isVisible() && cancel->isEnabled()
                     && !findAll->isEnabled() && !pattern->isEnabled() && !scope->isEnabled(),
                 "A's late success must not clear the real queued B query's busy state");
        QVERIFY2(!summary->text().contains(QString::fromLatin1(queryA)),
                 "A's late success must not replace the latest query's Dock summary");
        QVERIFY(!tree->model() || tree->model()->rowCount() == 0);

        tasks.releaseLoads();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskB).state, AsyncTaskState::Finished, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled() && pattern->isEnabled(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model() && tree->model()->rowCount() == 2, 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        QTest::qWait(100);
        QVERIFY(sourceGuards());
        auto *model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model && !model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(2));
        const QByteArray expectedBody = bodyB.toUtf8();
        qsizetype offset = expectedBody.indexOf(queryB);
        for (int row = 0; row < 2; ++row) {
            QVERIFY(offset >= 0);
            const QModelIndex result = model->index(row, SearchResultsModel::COL_CONTEXT);
            QCOMPARE(model->matchByteOffset(result), qint64(offset));
            QCOMPARE(model->matchLength(result), int(queryB.size()));
            offset = expectedBody.indexOf(queryB, offset + queryB.size());
        }
        QCOMPARE(offset, qsizetype(-1));
        QVERIFY(summary->text().contains(QString::fromLatin1(queryB))
                && !summary->text().contains(QString::fromLatin1(queryA)));
        QCOMPARE(pattern->text(), QString::fromLatin1(queryB));
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(targetB.data()));
        QCOMPARE(tasks.searches().size(), qsizetype(2));
        QCOMPARE(aTerminalCount, 1);
        QCOMPARE(manager.taskById(taskA).state, AsyncTaskState::Finished);
        QCOMPARE(manager.taskById(taskB).state, AsyncTaskState::Finished);
    }

    void allOpenQueuedTargetCloseSettlesAndAllowsNewQuery()
    {
        // Supplemental close/settlement boundary: use the real All Open UI,
        // not a manually invoked domain callback or an empty success result.
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreDataDir = qScopeGuard([&]() {
            if (hadDataDir) qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        const QString closingPath = temporary.filePath(QStringLiteral("all-open-queued-close.txt"));
        const QString survivorPath = temporary.filePath(QStringLiteral("all-open-queued-keep.txt"));
        const QByteArray closingBytes("SHARED_QUERY in the closing document\n");
        const QByteArray survivorBytes("prefix\nSHARED_QUERY in the surviving document\nRETRY_SURVIVOR only here\n");
        for (const auto &source : {qMakePair(closingPath, closingBytes), qMakePair(survivorPath, survivorBytes)}) {
            QFile file(source.first);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(source.second), qint64(source.second.size()));
        }
        const auto hash = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
            return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        };
        const QByteArray closingHash = hash(closingPath);
        const QByteArray survivorHash = hash(survivorPath);
        QVERIFY(!closingHash.isEmpty() && !survivorHash.isEmpty());

        MainWindow window;
        const auto cleanupWindow = qScopeGuard([&]() {
            if (auto *tabs = window.findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page)) tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
        });
        OwnedSearchTasks tasks(&window, QStringLiteral("all-open-queued-"));
        bool sawModal = false;
        QTimer dismissUnexpectedModal;
        QObject::connect(&dismissUnexpectedModal, &QTimer::timeout, &window, [&]() {
            if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                sawModal = true;
                modal->reject(); // Do not let an unexpected Save dialog hang the suite.
            }
        });
        dismissUnexpectedModal.start(10);
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({closingPath, survivorPath});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        QPointer<EditorTab> closing;
        QPointer<EditorTab> survivor;
        for (int index = 0; index < tabs->count(); ++index) {
            auto *tab = qobject_cast<EditorTab *>(tabs->widget(index));
            QVERIFY(tab && !tab->isL2Mode() && !tab->isModified());
            if (tab->filePath() == closingPath) closing = tab;
            if (tab->filePath() == survivorPath) survivor = tab;
        }
        QVERIFY(closing && survivor);
        const DocumentState closingState = closing->documentState();
        const DocumentState survivorState = survivor->documentState();
        tabs->setCurrentWidget(closing);
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        auto *closeAction = window.findChild<QAction *>(QStringLiteral("file.close"));
        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && closeAction && dock);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(pattern && scope && regex && wholeWord && hex && findAll && cancel && tree && summary);
        QVERIFY(scope->findData(1) >= 0);
        scope->setCurrentIndex(scope->findData(1));
        regex->setChecked(false);
        wholeWord->setChecked(false);
        hex->setChecked(false);
        pattern->setText(QStringLiteral("SHARED_QUERY"));
        QVERIFY(tasks.occupyGeneralPool());
        QTRY_VERIFY_WITH_TIMEOUT(tasks.allLoadsRunning(), 3000);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(tasks.searches().size(), qsizetype(2), 1500);
        auto &manager = AsyncTaskManager::instance();
        qint64 closingTaskId = 0;
        qint64 survivorTaskId = 0;
        for (qint64 id : tasks.searches()) {
            const AsyncTask task = manager.taskById(id);
            QCOMPARE(task.state, AsyncTaskState::Queued);
            QVERIFY(!task.startedAt.isValid());
            if (task.title == QStringLiteral("查找全部: %1").arg(QFileInfo(closingPath).fileName())) closingTaskId = id;
            if (task.title == QStringLiteral("查找全部: %1").arg(QFileInfo(survivorPath).fileName())) survivorTaskId = id;
        }
        QVERIFY(closingTaskId > 0 && survivorTaskId > 0 && closingTaskId != survivorTaskId);
        QVERIFY(cancel->isVisible() && !findAll->isEnabled() && tasks.allLoadsRunning());
        QCOMPARE(closing->documentState().id, closingState.id);
        QCOMPARE(closing->contentRevision(), closingState.revision);
        QCOMPARE(closing->editor()->text(), QString::fromUtf8(closingBytes));
        QVERIFY(!closing->isModified() && closeAction->isEnabled());
        closeAction->trigger();
        QVERIFY(closing.isNull());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(survivor.data()));
        QVERIFY(!sawModal && !QApplication::activeModalWidget());
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(closingTaskId).state, AsyncTaskState::Canceled, 1000);
        QVERIFY(tasks.allLoadsRunning());
        QCOMPARE(hash(closingPath), closingHash);
        QCOMPARE(hash(survivorPath), survivorHash);
        tasks.releaseLoads();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(survivorTaskId).state, AsyncTaskState::Finished, 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        // Safety is checked before the pending-target/UI assertion: a callback
        // dropped only because its document died must still reach these guards.
        QVERIFY(survivor);
        QCOMPARE(survivor->documentState().id, survivorState.id);
        QCOMPARE(survivor->contentRevision(), survivorState.revision);
        QCOMPARE(survivor->documentState().capabilities, survivorState.capabilities);
        QCOMPARE(survivor->editor()->text(), QString::fromUtf8(survivorBytes));
        QVERIFY(!survivor->isModified() && !sawModal);
        QCOMPARE(hash(closingPath), closingHash);
        QCOMPARE(hash(survivorPath), survivorHash);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled()
                                    && pattern->isEnabled() && scope->isEnabled(), 1500);
        QCOMPARE(scope->currentData().toInt(), 1);
        auto *model = tree->model();
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 1);
        QModelIndex group = model->index(0, 0);
        QCOMPARE(group.data().toString(), QFileInfo(survivorPath).fileName());
        QCOMPARE(model->rowCount(group), 1);
        QModelIndex hit = model->index(0, SearchResultsModel::COL_CONTEXT, group);
        QCOMPARE(hit.data(SearchResultsModel::ByteOffsetRole).toLongLong(), qint64(survivorBytes.indexOf("SHARED_QUERY")));
        QCOMPARE(hit.data(SearchResultsModel::LengthRole).toInt(), int(QByteArray("SHARED_QUERY").size()));

        pattern->setText(QStringLiteral("RETRY_SURVIVOR"));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(tasks.searches().size(), qsizetype(3), 1500);
        const qint64 retryId = tasks.searches().at(2);
        QVERIFY(retryId != closingTaskId && retryId != survivorTaskId);
        QCOMPARE(manager.taskById(retryId).title, QStringLiteral("查找全部: %1").arg(QFileInfo(survivorPath).fileName()));
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(retryId).state, AsyncTaskState::Finished, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled(), 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        QTest::qWait(100);
        model = tree->model();
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 1);
        group = model->index(0, 0);
        QCOMPARE(group.data().toString(), QFileInfo(survivorPath).fileName());
        QCOMPARE(model->rowCount(group), 1);
        hit = model->index(0, SearchResultsModel::COL_CONTEXT, group);
        QCOMPARE(hit.data(SearchResultsModel::ByteOffsetRole).toLongLong(), qint64(survivorBytes.indexOf("RETRY_SURVIVOR")));
        QCOMPARE(hit.data(SearchResultsModel::LengthRole).toInt(), int(QByteArray("RETRY_SURVIVOR").size()));
        QVERIFY(summary->text().contains(QStringLiteral("RETRY_SURVIVOR"))
                && !summary->text().contains(QStringLiteral("SHARED_QUERY")));
        QVERIFY(closing.isNull() && survivor && !sawModal);
        QCOMPARE(manager.taskById(closingTaskId).state, AsyncTaskState::Canceled);
        QCOMPARE(survivor->documentState().id, survivorState.id);
        QCOMPARE(survivor->contentRevision(), survivorState.revision);
        QCOMPARE(survivor->documentState().capabilities, survivorState.capabilities);
        QCOMPARE(survivor->editor()->text(), QString::fromUtf8(survivorBytes));
        QVERIFY(!survivor->isModified());
        QCOMPARE(hash(closingPath), closingHash);
        QCOMPARE(hash(survivorPath), survivorHash);
    }
};

QTEST_MAIN(SmokeSearchLifecycle)
#include "smoke_search_lifecycle.moc"
