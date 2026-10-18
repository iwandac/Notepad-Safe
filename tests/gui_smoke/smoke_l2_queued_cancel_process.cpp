#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QTreeView>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "EditorTab.h"
#include "FindOverlay.h"
#include "LargeFileSearch.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "ReadOnlySearchSource.h"
#include "WindowedReadOnlyProvider.h"

// Deliberately separate from in-process GUI suites: a leaked search reservation
// can block both cancelAndWait() and MainWindow destruction. Only the parent
// QProcess owns the deadline. No product worker, result, or cancellation hook is
// replaced. Register the parent Qt cases only, after design review.
namespace {

constexpr qint64 kSourceSize = 33LL * 1024 * 1024;
constexpr int kChildBudgetMs = 20000;
constexpr int kStopBudgetMs = 2000;
constexpr int kKillBudgetMs = 3000;
const QByteArray kPhasePrefix("L2_CANCEL_PHASE|");
const QByteArray kWarmup("L2_PREPARE_SOURCE_743");
const QByteArray kQuery("L2_QUEUED_QUERY_918");
const QByteArray kRerunQuery("L2_TERMINAL_RERUN_652");
const QByteArray kNavigationQuery("NEXT_AFTER_CLOSE_463");
constexpr qint64 kWarmupOffset = 128;
constexpr qint64 kRerunOffset = 8192;

QByteArray sourceChangeReplacementBytes()
{
    QByteArray chunk(1024 * 1024, 'c');
    for (qsizetype offset = 127; offset < chunk.size(); offset += 128) chunk[offset] = '\n';
    chunk.replace(kWarmupOffset, kWarmup.size(), kWarmup);
    chunk.replace(kRerunOffset, kRerunQuery.size(), kRerunQuery);
    return chunk.repeated(33);
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        QThread::msleep(1);
    } while (elapsed.elapsed() < timeoutMs);
    return false;
}

bool publishPhase(const QByteArray &phase, QString detail = {})
{
    detail.replace(QLatin1Char('\n'), QLatin1Char(' '));
    detail.replace(QLatin1Char('\r'), QLatin1Char(' '));
    const QByteArray line = kPhasePrefix + phase + '|' + detail.toUtf8() + '\n';
#ifdef Q_OS_WIN
    // A Qt GUI child has no console requirement. QProcess supplies the actual
    // inherited stdout pipe; WriteFile avoids relying on a GUI CRT stdout
    // stream being attached to that handle. The parent must observe BOOT.
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    return ::WriteFile(output, line.constData(), DWORD(line.size()), &written, nullptr)
        && written == DWORD(line.size());
#else
    const bool written = std::fwrite(line.constData(), 1, size_t(line.size()), stdout)
        == size_t(line.size());
    return std::fflush(stdout) == 0 && written;
#endif
}

#define CHILD_CHECK(condition, explanation) \
    do { \
        if (!(condition)) { \
            publishPhase("FAIL", (explanation)); \
            return false; \
        } \
    } while (false)

// Local fixture only: real framework tasks occupy the ordinary pool. The
// canaries separately prove High and Highest do not escape that held capacity.
class HeldOrdinaryTasks final {
public:
    ~HeldOrdinaryTasks()
    {
        release();
        for (qint64 id : std::as_const(owned_))
            manager_.cancel(id);
        if (!waitUntil([this]() { return settled(); }, 3000))
            publishPhase("FAIL", QStringLiteral("Owned workload cleanup exceeded 3 seconds"));
    }

    bool occupy()
    {
        const int count = manager_.maximumThreadCount() - 1;
        if (count < 1) return false;
        for (int index = 0; index < count; ++index)
            if (!addHolder()) return false;
        return true;
    }

    bool addHolder(bool waitForEntry = false)
    {
        const auto state = std::make_shared<State>();
        const qint64 id = manager_.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Highest,
            QStringLiteral("排队取消验收负载 %1").arg(loads_.size()), QString(),
            [state](qint64, AsyncCancelPtr cancel, const auto &, const auto &) {
                state->entered.fetch_add(1, std::memory_order_release);
                const auto exited = qScopeGuard([state] {
                    state->exited.fetch_add(1, std::memory_order_release);
                });
                while (!state->released.load(std::memory_order_acquire)
                       && !(cancel && cancel->load(std::memory_order_acquire)))
                    QThread::msleep(2);
            }, 0, QStringLiteral("L2CancelHeldLoad"));
        if (id <= 0) return false;
        loads_.append(id);
        loadStates_.append(state);
        owned_.append(id);
        if (!waitForEntry) return true;
        QElapsedTimer deadline;
        deadline.start();
        // This taskAdded observer runs before the product wrapper is enqueued.
        // Wait only for our holder's atomic entry; do not dispatch GUI events.
        while (state->entered.load(std::memory_order_acquire) == 0
               && deadline.elapsed() < 3000) QThread::msleep(1);
        return state->entered.load(std::memory_order_acquire) == 1;
    }

    bool releaseOne()
    {
        for (const auto &state : std::as_const(loadStates_)) {
            if (!state->released.exchange(true, std::memory_order_acq_rel))
                return true;
        }
        return false;
    }

    bool saturated() const
    {
        int active = 0;
        for (qsizetype i = 0; i < loads_.size(); ++i) {
            const auto &state = loadStates_.at(i);
            if (state->released.load(std::memory_order_acquire)) continue;
            ++active;
            if (state->entered.load(std::memory_order_acquire) != 1
                || state->exited.load(std::memory_order_acquire) != 0)
                return false;
        }
        return active == manager_.maximumThreadCount() - 1 && active > 0;
    }

    bool hasHeldSlots() const
    {
        for (const auto &state : loadStates_)
            if (!state->released.load(std::memory_order_acquire)) return true;
        return false;
    }

    bool submitCanaries()
    {
        for (const auto priority : {AsyncTaskPriority::Normal,
                                    AsyncTaskPriority::High,
                                    AsyncTaskPriority::Highest}) {
            const auto entered = std::make_shared<std::atomic_int>(0);
            const qint64 id = manager_.submit(
                AsyncTaskType::Other, priority,
                QStringLiteral("排队优先级验收 %1").arg(int(priority)), QString(),
                [entered](qint64, AsyncCancelPtr, const auto &, const auto &) {
                    entered->fetch_add(1, std::memory_order_release);
                }, 0, QStringLiteral("L2CancelQueueCanary"));
            if (id <= 0)
                return false;
            canaries_.append({id, priority, entered});
            owned_.append(id);
        }
        return true;
    }

    bool canariesQueued() const
    {
        if (!saturated() || canaries_.size() != 3)
            return false;
        for (const Canary &canary : canaries_) {
            const AsyncTask task = manager_.taskById(canary.id);
            if (task.id != canary.id || task.priority != canary.priority
                || task.state != AsyncTaskState::Queued || task.startedAt.isValid()
                || canary.entered->load(std::memory_order_acquire) != 0)
                return false;
        }
        return true;
    }

    QString queueEvidence() const
    {
        int entered = 0;
        int exited = 0;
        for (const auto &state : loadStates_) {
            entered += state->entered.load(std::memory_order_acquire);
            exited += state->exited.load(std::memory_order_acquire);
        }
        QString evidence = QStringLiteral("capacity=%1 entered=%2 exited=%3 active=%4")
            .arg(manager_.maximumThreadCount() - 1).arg(entered).arg(exited).arg(entered - exited);
        for (const Canary &canary : canaries_) {
            const AsyncTask task = manager_.taskById(canary.id);
            evidence += QStringLiteral(" canary=%1 priority=%2 state=%3 entered=%4")
                .arg(canary.id).arg(int(task.priority)).arg(int(task.state))
                .arg(canary.entered->load(std::memory_order_acquire));
        }
        return evidence;
    }

    auto cancelCanaries() -> void
    {
        for (const Canary &canary : canaries_)
            manager_.cancel(canary.id);
    }

    void track(qint64 id) { owned_.append(id); }
    auto release() -> void
    {
        for (const auto &state : loadStates_)
            state->released.store(true, std::memory_order_release);
    }

    bool drainAfterTargetDestruction()
    {
        release();
        // All canceled canaries/search wrappers have higher queue priority.
        // A finished tail marker plus zero active threads proves the pool has
        // processed them after the borrowed source/engine was destroyed.
        const qint64 fence = manager_.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Lowest,
            QStringLiteral("取消后的队列排空验收"), QString(),
            [](qint64, AsyncCancelPtr, const auto &, const auto &) {},
            0, QStringLiteral("L2CancelDrainFence"));
        if (fence <= 0)
            return false;
        track(fence);
        return waitUntil([&]() {
            return settled() && manager_.taskById(fence).state == AsyncTaskState::Finished
                && manager_.activeThreadCount() == 0;
        }, 3000);
    }

private:
    struct State {
        std::atomic_bool released{false};
        std::atomic_int entered{0};
        std::atomic_int exited{0};
    };
    struct Canary {
        qint64 id;
        AsyncTaskPriority priority;
        std::shared_ptr<std::atomic_int> entered;
    };

    bool settled() const
    {
        for (const auto &state : loadStates_) {
            if (state->entered.load(std::memory_order_acquire)
                != state->exited.load(std::memory_order_acquire)) return false;
        }
        for (qint64 id : owned_) {
            const AsyncTask task = manager_.taskById(id);
            if (task.id != id || task.state == AsyncTaskState::Queued
                || task.state == AsyncTaskState::Running)
                return false;
        }
        for (const Canary &canary : canaries_) {
            if (canary.entered->load(std::memory_order_acquire) != 0)
                return false;
        }
        return true;
    }

    AsyncTaskManager &manager_ = AsyncTaskManager::instance();
    QVector<std::shared_ptr<State>> loadStates_;
    QVector<qint64> loads_;
    QVector<qint64> owned_;
    QVector<Canary> canaries_;
};

QModelIndex indexForTask(QListView *list, qint64 id)
{
    if (!list || !list->model())
        return {};
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        const QModelIndex candidate = list->model()->index(row, 0);
        if (candidate.data(AsyncTaskManager::TaskIdRole).toLongLong() == id)
            return candidate;
    }
    return {};
}

bool runChild(const QString &kind, const QString &stopAction,
              const QString &path, const QByteArray &expectedHash)
{
    const bool cancelAtAdded = stopAction == QStringLiteral("taskAddedCancel");
    const bool closeAtAdded = stopAction == QStringLiteral("taskAddedClose");
    const bool terminalRerun = stopAction == QStringLiteral("terminalRerun");
    const bool hideReopen = stopAction == QStringLiteral("hideReopen");
    const bool sourceChangeReentry = stopAction == QStringLiteral("sourceChangeTerminalReentry");
    const bool stopCancelClosesSource = stopAction == QStringLiteral("stopCancelClosesSource");
    const bool sourceCloseReenters = stopAction == QStringLiteral("sourceCloseReenters");
    const bool switchBackQuery = stopAction == QStringLiteral("querySwitchBack")
        || stopAction == QStringLiteral("querySwitchBackInvalidRegex");
    const bool invalidNextQuery = stopAction == QStringLiteral("querySwitchBackInvalidRegex");
    const bool mappingPendingClose = stopAction == QStringLiteral("mappingPendingClose");
    const bool mappingPendingCancel = stopAction == QStringLiteral("mappingPendingCancel");
    const bool mappingPendingFailure = stopAction == QStringLiteral("mappingPendingFailure");
    const bool mappingPreparation = mappingPendingClose || mappingPendingCancel || mappingPendingFailure;
    const bool lateNavigation = stopAction == QStringLiteral("canceledNavigationKeepsNewQueryBusy");
    const bool dualLanes = stopAction == QStringLiteral("dualNavFirst")
        || stopAction == QStringLiteral("dualAllFirst");
    CHILD_CHECK((kind == QStringLiteral("findAll") || kind == QStringLiteral("findOne"))
                    && (stopAction == QStringLiteral("cancelAndWait")
                        || stopAction == QStringLiteral("sourceClose")
                        || cancelAtAdded || closeAtAdded || stopCancelClosesSource || sourceCloseReenters
                        || ((terminalRerun || hideReopen || sourceChangeReentry || switchBackQuery || dualLanes) && kind == QStringLiteral("findAll"))
                        || ((mappingPreparation || lateNavigation) && kind == QStringLiteral("findOne"))),
                QStringLiteral("Invalid child scenario"));
    CHILD_CHECK(!expectedHash.isEmpty() && fileHash(path) == expectedHash,
                QStringLiteral("Initial source hash mismatch"));
    MainWindow window;
    window.resize(1200, 800);
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
    // On assertion failures release/cancel owned work before MainWindow tries
    // to destroy a possibly reserved engine. The parent also reacts to FAIL;
    // it never relies on this in-process destructor to break a leaked wait.
    HeldOrdinaryTasks loads;
    window.show();
    CHILD_CHECK(QTest::qWaitForWindowExposed(&window), QStringLiteral("MainWindow not exposed"));
    window.openFilesFromExternal({path});
    auto *tabs = window.findChild<QTabWidget *>();
    CHILD_CHECK(tabs && tabs->count() == 1, QStringLiteral("Expected one source tab"));
    QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
    CHILD_CHECK(target && target->windowedReadOnlyProvider(), QStringLiteral("Missing windowed text source"));
    QPointer<WindowedReadOnlyProvider> provider = target->windowedReadOnlyProvider();
    CHILD_CHECK(target->isReadOnlyByPolicy() && !target->isBinaryDocument()
                    && !target->isHexPresentation() && !target->isSecureDocument()
                    && target->contentSizeBytes() == kSourceSize,
                QStringLiteral("Fixture did not enter ordinary read-only text route"));

    auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
    CHILD_CHECK(findAction, QStringLiteral("Missing real Find action"));
    findAction->trigger();
    auto *dialog = window.findChild<FindOverlay *>();
    CHILD_CHECK(dialog && waitUntil([&]() { return dialog->isVisible(); }, 2000),
                QStringLiteral("Find dialog not visible"));
    auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
    auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
    CHILD_CHECK(pattern && scope && regex && hex && wholeWord && next && findAll,
                QStringLiteral("Missing real Find controls"));
    scope->setCurrentIndex(scope->findData(0));
    regex->setChecked(false);
    hex->setChecked(false);
    wholeWord->setChecked(false);
    pattern->setText(QString::fromLatin1(kWarmup));
    auto &manager = AsyncTaskManager::instance();
    if (mappingPreparation) {
        // This path intentionally branches BEFORE the normal navigation
        // warmup: the first real Next must still be waiting for its mapping.
        CHILD_CHECK(waitUntil([&]() {
            return provider && !provider->characterCountPending()
                && manager.queuedCount() == 0 && manager.runningCount() == 0
                && manager.activeThreadCount() == 0;
        }, 8000), QStringLiteral("Provider statistics did not finish before first navigation"));
        auto *coldSource = target->findChild<ReadOnlySearchSource *>();
        CHILD_CHECK(!coldSource || !coldSource->isReady(),
                    QStringLiteral("Mapping-pending fixture was already warmed up"));
        const QString secondPath = QFileInfo(path).dir().filePath(QStringLiteral("navigation-second.txt"));
        const QByteArray secondHash = fileHash(secondPath);
        const DocumentState initial = target->documentState();
        const QString body = target->editor()->text();
        const bool projectionModified = target->editor()->isModified();
        const auto initialEncoding = target->encoding();
        const qint64 selectionStart = provider->selectionStartOffset();
        const qint64 selectionEnd = provider->selectionEndOffset();
        quint64 guardedRevision = initial.revision;
        const auto guards = [&]() {
            return target && provider && target->windowedReadOnlyProvider() == provider.data()
                && target->documentState().id == initial.id && target->contentRevision() == guardedRevision
                && target->documentState().capabilities == initial.capabilities
                && target->documentState().location == initial.location
                && !target->isModified() && !initial.dirty && target->isReadOnlyByPolicy()
                && target->encoding() == initialEncoding && target->contentSizeBytes() == kSourceSize
                && target->editor()->text() == body && target->editor()->isModified() == projectionModified
                && target->editor()->isReadOnly() && provider->selectionStartOffset() == selectionStart
                && provider->selectionEndOffset() == selectionEnd && fileHash(path) == expectedHash
                && !secondHash.isEmpty() && fileHash(secondPath) == secondHash
                && !QApplication::activeModalWidget();
        };
        CHILD_CHECK(guards(), QStringLiteral("First-navigation source guards failed"));
        CHILD_CHECK(publishPhase("READY", QStringLiteral("provider ready; no navigation warmup")), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(loads.occupy() && waitUntil([&]() { return loads.saturated(); }, 3000)
                        && loads.submitCanaries(), QStringLiteral("Could not hold first-navigation ordinary pool"));
        QTest::qWait(100);
        CHILD_CHECK(loads.canariesQueued(), QStringLiteral("Mapping queue priority precondition failed"));
        CHILD_CHECK(publishPhase("POOL_QUEUED", loads.queueEvidence()), QStringLiteral("Phase pipe unavailable"));
        loads.cancelCanaries();
        pattern->setText(QString::fromLatin1(kQuery));
        QSignalSpy submitted(&manager, &AsyncTaskManager::taskAdded);
        QSignalSpy clicked(next, &QPushButton::clicked);
        QTest::mouseClick(next, Qt::LeftButton);
        qint64 mappingId = 0;
        int mappings = 0;
        int navigations = 0;
        for (const QList<QVariant> &arguments : submitted) {
            if (arguments.isEmpty()) continue;
            const qint64 id = arguments.constFirst().toLongLong();
            const AsyncTask task = manager.taskById(id);
            if (task.categoryTag == QStringLiteral("Mmap")
                && task.title.contains(QFileInfo(path).fileName())) {
                mappingId = id;
                ++mappings;
                loads.track(id);
            }
            if (task.categoryTag == QStringLiteral("SearchNavigation")) ++navigations;
        }
        QPointer<ReadOnlySearchSource> pendingSource = target->findChild<ReadOnlySearchSource *>();
        const AsyncTask mapping = manager.taskById(mappingId);
        CHILD_CHECK(clicked.size() == 1 && mappings == 1 && navigations == 0 && mappingId > 0
                        && mapping.id == mappingId && mapping.state == AsyncTaskState::Queued
                        && mapping.priority == AsyncTaskPriority::High && !mapping.startedAt.isValid()
                        && pendingSource && !pendingSource->isReady()
                        && pendingSource->preparationTaskId() == mappingId
                        && loads.saturated() && !next->isEnabled(),
                    QStringLiteral("First Next was not actually pending an unstarted High mapping"));
        CHILD_CHECK(publishPhase("QUEUED", QString::number(mappingId))
                        && publishPhase("MAPPING_PENDING", QStringLiteral("High Mmap queued; navigation tasks=0")),
                    QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(guards(), QStringLiteral("Pending mapping changed source data or selection"));
        CHILD_CHECK(publishPhase("GUARDS_OK", initial.id.toString()), QStringLiteral("Phase pipe unavailable"));
        if (!mappingPendingClose) {
            QSignalSpy preparationFailures(pendingSource, &ReadOnlySearchSource::failed);
            QSignalSpy providerChanges(provider.data(), &WindowedReadOnlyProvider::sourceFileChanged);
            QSignalSpy providerFailures(provider.data(), &WindowedReadOnlyProvider::sourceReloadFailed);
            const DiskFileVersion originalVersion = provider->sourceVersion();
            const DiskFileStamp originalStamp = provider->sourceStamp();
            int acceptedVersions = 0;
            QString acceptedPath;
            DiskFileVersion acceptedVersion;
            const auto observeAcceptedVersion = QObject::connect(
                provider.data(), &WindowedReadOnlyProvider::sourceVersionAccepted, &window,
                [&](QString sourcePath, DiskFileVersion version) {
                    ++acceptedVersions;
                    acceptedPath = std::move(sourcePath);
                    acceptedVersion = std::move(version);
                });
            const auto disconnectAcceptedVersion = qScopeGuard([&] {
                QObject::disconnect(observeAcceptedVersion);
            });
            if (mappingPendingFailure) {
#ifdef Q_OS_WIN
                HANDLE exclusive = ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                    GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                CHILD_CHECK(exclusive != INVALID_HANDLE_VALUE,
                            QStringLiteral("Could not acquire the real exclusive source handle"));
                const auto unlock = qScopeGuard([&] { ::CloseHandle(exclusive); });
                loads.release();
                CHILD_CHECK(waitUntil([&] {
                    return manager.taskById(mappingId).state == AsyncTaskState::Failed
                        && preparationFailures.size() == 1 && next->isEnabled();
                }, 3000), QStringLiteral("Actual mapping read failure did not settle once"));
#else
                CHILD_CHECK(false, QStringLiteral("Exclusive-handle fixture requires Windows"));
#endif
            } else {
                auto *taskButton = window.findChild<QToolButton *>(QStringLiteral("StatusBarTaskWidget_btn"));
                CHILD_CHECK(taskButton, QStringLiteral("Missing TaskPanel entry for queued preparation"));
                QTest::mouseClick(taskButton, Qt::LeftButton);
                auto *panel = window.findChild<AsyncTaskPanelDialog *>();
                CHILD_CHECK(panel, QStringLiteral("Task panel did not open"));
                auto *list = panel->findChild<QListView *>(QStringLiteral("backgroundTaskList"));
                auto *cancel = panel->findChild<QPushButton *>(QStringLiteral("cancelSelectedTaskButton"));
                const QModelIndex selected = indexForTask(list, mappingId);
                CHILD_CHECK(list && cancel && selected.isValid(), QStringLiteral("Exact preparation row unavailable"));
                list->setCurrentIndex(selected);
                QTest::mouseClick(cancel, Qt::LeftButton);
                CHILD_CHECK(waitUntil([&] {
                    return manager.taskById(mappingId).state == AsyncTaskState::Canceled
                        && preparationFailures.size() == 1 && next->isEnabled();
                }, 1500), QStringLiteral("Queued preparation cancellation did not settle once"));
                panel->hide();
                CHILD_CHECK(loads.saturated(), QStringLiteral("Canceled preparation consumed a held slot"));
                loads.release();
            }
            if (mappingPendingFailure) {
                const DocumentState current = target ? target->documentState() : DocumentState{};
                QString diagnostic = QStringLiteral("target=%1 provider=%2 id=%3 initialId=%4 revision=%5 initialRevision=%6 capsEqual=%7 locationEqual=%8")
                    .arg(bool(target)).arg(bool(provider)).arg(current.id.toString()).arg(initial.id.toString())
                    .arg(current.revision).arg(initial.revision).arg(current.capabilities == initial.capabilities)
                    .arg(current.location == initial.location);
                diagnostic += QStringLiteral(" bodyEqual=%1 projectionModifiedEqual=%2 dirty=%3 readonly=%4 encodingEqual=%5 size=%6 selection=%7,%8 initialSelection=%9,%10")
                    .arg(target && target->editor()->text() == body)
                    .arg(target && target->editor()->isModified() == projectionModified)
                    .arg(target && target->isModified()).arg(target && target->isReadOnlyByPolicy())
                    .arg(target && target->encoding() == initialEncoding).arg(target ? target->contentSizeBytes() : -1)
                    .arg(provider ? provider->selectionStartOffset() : -1).arg(provider ? provider->selectionEndOffset() : -1)
                    .arg(selectionStart).arg(selectionEnd);
                diagnostic += QStringLiteral(" hashEqual=%1 secondHashEqual=%2 sourceReady=%3 preparation=%4 mappingId=%5 mappingState=%6 changes=%7 failures=%8 modal=%9")
                    .arg(fileHash(path) == expectedHash).arg(fileHash(secondPath) == secondHash)
                    .arg(pendingSource && pendingSource->isReady()).arg(pendingSource ? pendingSource->preparationTaskId() : -1)
                    .arg(mappingId).arg(int(manager.taskById(mappingId).state))
                    .arg(providerChanges.size()).arg(providerFailures.size()).arg(bool(QApplication::activeModalWidget()));
                for (const auto &arguments : providerChanges)
                    diagnostic += QStringLiteral(" sourceChanged=%1->%2")
                        .arg(arguments.at(0).toLongLong()).arg(arguments.at(1).toLongLong());
                for (const auto &arguments : providerFailures)
                    diagnostic += QStringLiteral(" sourceFailure=%1").arg(arguments.constFirst().toString());
                CHILD_CHECK(publishPhase("MAPPING_FAILURE_GUARD_DIAGNOSTIC", diagnostic), QStringLiteral("Phase pipe unavailable"));
                CHILD_CHECK(target && target->contentRevision() == initial.revision + 1
                                && providerChanges.size() == 1 && providerFailures.size() == 1
                                && providerChanges.constFirst().at(0).toLongLong() == kSourceSize
                                && providerChanges.constFirst().at(1).toLongLong() == 0,
                            QStringLiteral("Exclusive unreadability did not invalidate the source exactly once"));
                guardedRevision = initial.revision + 1;
            }
            CHILD_CHECK(guards() && pendingSource && !pendingSource->isReady()
                            && pendingSource->preparationTaskId() == 0,
                        QStringLiteral("Preparation failure altered source or retained a mapping"));
            CHILD_CHECK(publishPhase(mappingPendingFailure ? "MAPPING_FAILED" : "MANAGER_CANCELED",
                                     QString::number(mappingId)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(waitUntil([&] { return manager.activeThreadCount() == 0; }, 3000),
                        QStringLiteral("Preparation wrappers did not leave the real pool"));
            if (mappingPendingFailure) {
                CHILD_CHECK(waitUntil([&] {
                    return providerChanges.size() == 2 && acceptedVersions == 1;
                }, 3000), QStringLiteral("Readable source did not produce exactly one recovery transition and accepted version"));
                CHILD_CHECK(target && provider && target->contentRevision() == initial.revision + 2
                                && providerChanges.size() == 2 && providerFailures.size() == 1
                                && providerChanges.at(1).at(0).toLongLong() == kSourceSize
                                && providerChanges.at(1).at(1).toLongLong() == kSourceSize
                                && acceptedVersions == 1 && acceptedVersion.valid && originalVersion.valid
                                && sameDiskFilePath(acceptedPath, originalVersion.absolutePath)
                                && sameDiskFilePath(acceptedVersion.absolutePath, originalVersion.absolutePath)
                                && acceptedVersion.size == originalVersion.size
                                && acceptedVersion.modifiedMs == originalVersion.modifiedMs
                                && acceptedVersion.fileIdentity == originalVersion.fileIdentity
                                && !originalVersion.sampleHash.isEmpty()
                                && acceptedVersion.sampleHash == originalVersion.sampleHash
                                && sameDiskFileStamp(provider->sourceStamp(), originalStamp),
                            QStringLiteral("Source recovery changed original version evidence or emitted an extra failure/transition"));
                guardedRevision = initial.revision + 2;
                QString admissionError;
                CHILD_CHECK(provider && provider->validateCurrent(&admissionError) && admissionError.isEmpty()
                                && guards() && providerChanges.size() == 2 && providerFailures.size() == 1
                                && acceptedVersions == 1,
                            QStringLiteral("Released exclusive source was not safely readable before retry: %1").arg(admissionError));
                CHILD_CHECK(publishPhase("MAPPING_SOURCE_READABLE", QStringLiteral("revision=%1 changes=2 failures=1 accepted=1 originalVersion=unchanged")
                                .arg(guardedRevision)), QStringLiteral("Phase pipe unavailable"));
            }
            QSignalSpy retryAdded(&manager, &AsyncTaskManager::taskAdded);
            findAction->trigger();
            QTest::mouseClick(next, Qt::LeftButton);
            CHILD_CHECK(waitUntil([&] {
                return next->isEnabled() && provider->selectionStartOffset() == 4096
                    && provider->selectionEndOffset() == 4096 + kQuery.size();
            }, 5000), QStringLiteral("Retry did not select the real original-source hit"));
            int retryMappings = 0;
            int retryNavigations = 0;
            for (const auto &args : retryAdded) {
                const AsyncTask task = manager.taskById(args.constFirst().toLongLong());
                if (task.categoryTag == QStringLiteral("Mmap")) {
                    CHILD_CHECK(task.id != mappingId, QStringLiteral("Retry reused the failed task identity"));
                    ++retryMappings;
                }
                if (task.categoryTag == QStringLiteral("SearchNavigation")) ++retryNavigations;
            }
            CHILD_CHECK(retryMappings == 1 && retryNavigations == 1
                            && preparationFailures.size() == 1 && fileHash(path) == expectedHash
                            && target->documentState().id == initial.id && !target->isModified(),
                        QStringLiteral("Retry lost once-only/data/task guards"));
            CHILD_CHECK(publishPhase("MAPPING_RETRY_FINISHED"), QStringLiteral("Phase pipe unavailable"));
        }
        auto *close = window.findChild<QAction *>(QStringLiteral("file.close"));
        CHILD_CHECK(close && close->isEnabled(), QStringLiteral("Close unavailable while mapping is pending"));
        CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
        QElapsedTimer closing;
        closing.start();
        close->trigger();
        CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(target.isNull() && provider.isNull() && pendingSource.isNull()
                        && manager.taskById(mappingId).state == (mappingPendingFailure ? AsyncTaskState::Failed : AsyncTaskState::Canceled)
                        && (!mappingPendingClose || loads.saturated()),
                    QStringLiteral("Pending mapping source did not close before loads were released"));
        CHILD_CHECK(publishPhase(mappingPendingFailure ? "MAPPING_FAILED" : "MANAGER_CANCELED", QString::number(mappingId))
                        && publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(loads.drainAfterTargetDestruction() && fileHash(path) == expectedHash
                        && fileHash(secondPath) == secondHash, QStringLiteral("Canceled mapping did not drain safely"));
        CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
        window.openFilesFromExternal({secondPath});
        QPointer<EditorTab> fresh = qobject_cast<EditorTab *>(tabs->currentWidget());
        CHILD_CHECK(fresh && !fresh->isL2Mode() && !fresh->isSecureDocument() && !fresh->isModified()
                        && fresh->filePath() == secondPath, QStringLiteral("New ordinary navigation source did not open"));
        const DocumentState freshState = fresh->documentState();
        const QString freshBody = fresh->editor()->text();
        const qint64 offset = freshBody.toUtf8().indexOf(kNavigationQuery);
        CHILD_CHECK(offset >= 0 && freshBody.toUtf8().size() < 256 * 1024
                        && fileHash(path) == expectedHash && fileHash(secondPath) == secondHash,
                    QStringLiteral("New document data guards failed before navigation UI expectation"));
        findAction->trigger();
        pattern->setText(QString::fromLatin1(kNavigationQuery));
        CHILD_CHECK(waitUntil([&]() { return next->isEnabled(); }, 1500),
                    QStringLiteral("Closing the pending mapping left the new document's Next disabled"));
        QSignalSpy freshAdded(&manager, &AsyncTaskManager::taskAdded);
        QTest::mouseClick(next, Qt::LeftButton);
        CHILD_CHECK(fresh && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == offset
                        && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == offset + kNavigationQuery.size(),
                    QStringLiteral("New small document did not synchronously select its exact match"));
        QTest::qWait(100);
        for (const QList<QVariant> &arguments : freshAdded) {
            CHILD_CHECK(arguments.isEmpty() || manager.taskById(arguments.constFirst().toLongLong()).categoryTag
                            != QStringLiteral("SearchNavigation"),
                        QStringLiteral("Small-document Next unexpectedly submitted asynchronous navigation"));
        }
        CHILD_CHECK(fresh && fresh->documentState().id == freshState.id
                        && fresh->contentRevision() == freshState.revision
                        && fresh->documentState().capabilities == freshState.capabilities
                        && fresh->editor()->text() == freshBody && !fresh->isModified()
                        && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == offset
                        && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == offset + kNavigationQuery.size()
                        && manager.taskById(mappingId).state == (mappingPendingFailure ? AsyncTaskState::Failed : AsyncTaskState::Canceled)
                        && fileHash(path) == expectedHash && fileHash(secondPath) == secondHash
                        && !QApplication::activeModalWidget(), QStringLiteral("Late mapping work altered new navigation or source data"));
        CHILD_CHECK(publishPhase("FRESH_NAVIGATION_DONE", QStringLiteral("inline [%1,%2)").arg(offset).arg(offset + kNavigationQuery.size())),
                    QStringLiteral("Phase pipe unavailable"));
        return true;
    }
    QSignalSpy warmupAdded(&manager, &AsyncTaskManager::taskAdded);
    QTest::mouseClick(next, Qt::LeftButton);
    CHILD_CHECK(waitUntil([&]() {
        return target && provider && target->findChild<ReadOnlySearchSource *>()
            && next->isEnabled() && !provider->characterCountPending()
            && provider->selectionStartOffset() == kWarmupOffset
            && provider->selectionEndOffset() == kWarmupOffset + kWarmup.size()
            && manager.queuedCount() == 0 && manager.runningCount() == 0
            && manager.activeThreadCount() == 0;
    }, 8000), QStringLiteral("Real FindNext warmup/source preparation did not finish"));
    QPointer<ReadOnlySearchSource> source = target->findChild<ReadOnlySearchSource *>();
    qint64 warmupId = 0;
    for (const QList<QVariant> &arguments : warmupAdded) {
        if (arguments.isEmpty())
            continue;
        const qint64 id = arguments.constFirst().toLongLong();
        if (manager.taskById(id).categoryTag == QStringLiteral("SearchNavigation"))
            warmupId = id;
    }
    CHILD_CHECK(warmupId > 0 && manager.taskById(warmupId).state == AsyncTaskState::Finished,
                QStringLiteral("Warmup navigation did not actually finish"));
    const DocumentState before = target->documentState();
    const QString projectionBefore = target->editor()->text();
    const bool projectionModifiedBefore = target->editor()->isModified();
    const auto encodingBefore = target->encoding();
    const qint64 selectionStartBefore = provider->selectionStartOffset();
    const qint64 selectionEndBefore = provider->selectionEndOffset();
    CHILD_CHECK(!before.id.isNull() && !before.dirty && !target->isModified()
                    && !before.capabilities.testFlag(DocumentCapability::Edit),
                QStringLiteral("Source must be clean and read-only"));
    CHILD_CHECK(publishPhase("READY", QStringLiteral("kind=%1 warmupTask=%2").arg(kind).arg(warmupId)),
                QStringLiteral("Phase pipe unavailable"));

    CHILD_CHECK(loads.occupy() && waitUntil([&]() { return loads.saturated(); }, 3000),
                QStringLiteral("Could not hold every ordinary worker"));
    CHILD_CHECK(loads.submitCanaries(), QStringLiteral("Could not submit priority canaries"));
    QTest::qWait(100);
    CHILD_CHECK(loads.canariesQueued(), QStringLiteral("Priority canary ran: queue precondition invalid"));
    CHILD_CHECK(publishPhase("POOL_QUEUED", loads.queueEvidence()), QStringLiteral("Phase pipe unavailable"));
    loads.cancelCanaries();

    // Completed read-only searches release their mapping. Pass exactly one
    // slot for fresh preparation, then refill it in Search taskAdded before
    // that product wrapper can enter the pool. The canceled canaries above
    // remain unchanged: they never execute or consume the preparation slot.
    bool preparationSlot = false;
    bool bridgeFailed = false;
    const auto preparationBridge = QObject::connect(
        &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
            const AsyncTask task = manager.taskById(id);
            if (!task.title.contains(QFileInfo(path).fileName())) return;
            if (task.categoryTag == QStringLiteral("Mmap") && !preparationSlot
                && loads.hasHeldSlots()) {
                preparationSlot = loads.releaseOne();
                bridgeFailed = !preparationSlot;
                if (sourceChangeReentry)
                    publishPhase("SOURCE_CHANGE_PREPARATION_SLOT",
                        QStringLiteral("mapping=%1 released=%2 %3").arg(id).arg(preparationSlot).arg(loads.queueEvidence()));
            } else if (preparationSlot
                       && (task.categoryTag == QStringLiteral("Search")
                           || task.categoryTag == QStringLiteral("SearchNavigation"))) {
                bridgeFailed = !loads.addHolder(true);
                preparationSlot = false;
                if (sourceChangeReentry)
                    publishPhase("SOURCE_CHANGE_SEARCH_SLOT",
                        QStringLiteral("search=%1 holderEntered=%2 saturated=%3 %4")
                            .arg(id).arg(!bridgeFailed).arg(loads.saturated()).arg(loads.queueEvidence()));
            }
            if (bridgeFailed) publishPhase("FAIL", QStringLiteral("Preparation slot bridge failed"));
        });
    const auto disconnectBridge = qScopeGuard([&] { QObject::disconnect(preparationBridge); });

    const auto clickPreparedQuery = [&](QPushButton *button) {
        bool accepted = false;
        const auto admitted = QObject::connect(
            &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (task.title.contains(QFileInfo(path).fileName())
                    && (task.categoryTag == QStringLiteral("Search")
                        || task.categoryTag == QStringLiteral("SearchNavigation"))) accepted = true;
            });
        const auto disconnectAdmitted = qScopeGuard([&] { QObject::disconnect(admitted); });
        QTest::mouseClick(button, Qt::LeftButton);
        // Next may first prepare a fresh source asynchronously. This still
        // observes the exact real Search submit, including its direct hooks.
        return waitUntil([&] { return accepted || bridgeFailed; }, 3000)
            && accepted && !bridgeFailed;
    };

    if (dualLanes) {
        const bool allFirst = stopAction == QStringLiteral("dualAllFirst");
        QPointer<LargeFileSearch> allEngine = source->findAllEngine();
        QPointer<LargeFileSearch> navigationEngine = source->navigationEngine();
        qint64 allId = 0;
        qint64 navigationId = 0;
        int allTerminals = 0;
        int navigationTerminals = 0;
        QStringList completionOrder;
        bool navigationSubmitted = false;
        bool orderSetupPassed = true;
        const auto terminals = QObject::connect(
            &manager, &AsyncTaskManager::taskFinished, &window,
            [&](qint64 id, AsyncTaskState state) {
                if (id != allId && id != navigationId) return;
                if (state != AsyncTaskState::Finished) orderSetupPassed = false;
                if (id == allId) { ++allTerminals; completionOrder.append(QStringLiteral("all")); }
                if (id == navigationId) { ++navigationTerminals; completionOrder.append(QStringLiteral("navigation")); }
            });
        const auto trackNavigation = QObject::connect(
            &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (task.categoryTag != QStringLiteral("SearchNavigation")
                    || !task.title.contains(QFileInfo(path).fileName())) return;
                navigationId = id;
                loads.track(id);
                if (allFirst) orderSetupPassed = loads.addHolder(true);
            });
        const auto disconnectDual = qScopeGuard([&] {
            QObject::disconnect(terminals);
            QObject::disconnect(trackNavigation);
        });
        pattern->setText(QString::fromLatin1(kQuery));
        CHILD_CHECK(clickPreparedQuery(findAll), QStringLiteral("Dual-lane Find All was not accepted"));
        allId = allEngine->lastTaskId();
        loads.track(allId);
        CHILD_CHECK(allId > 0 && manager.taskById(allId).state == AsyncTaskState::Queued
                        && allEngine->sourceInUse() && source->isReady() && loads.saturated(),
                    QStringLiteral("Dual-lane first reader was not really queued"));
        QMetaObject::Connection allEntered;
        if (allFirst) {
            // A real algorithm phase proves All's worker entered. Its UI
            // completion cannot be dispatched inside this direct phase slot.
            // Submit Nav now; its taskAdded installs a holder before Nav's
            // wrapper, so All exits first and Nav remains genuinely queued.
            allEntered = QObject::connect(
                &manager, &AsyncTaskManager::taskPhaseChanged, &window,
                [&](qint64 id, const QString &, int, int, qint64, qint64) {
                    if (id != allId || navigationSubmitted) return;
                    navigationSubmitted = true;
                    QTest::mouseClick(next, Qt::LeftButton);
                    orderSetupPassed = orderSetupPassed && navigationId > 0;
                });
            CHILD_CHECK(loads.releaseOne(), QStringLiteral("Could not release All's worker slot"));
        } else {
            CHILD_CHECK(clickPreparedQuery(next), QStringLiteral("Dual-lane navigation was not accepted"));
            navigationSubmitted = true;
            // Nav was accepted first at Highest. Queue a same-priority holder
            // after it, before freeing one slot; the observed exact-ID order
            // below proves Nav ran and the holder kept High-priority All queued.
            CHILD_CHECK(loads.addHolder() && loads.releaseOne(),
                        QStringLiteral("Could not stage navigation-first queue"));
        }
        const auto disconnectEntered = qScopeGuard([&] { QObject::disconnect(allEntered); });
        CHILD_CHECK(waitUntil([&] { return !completionOrder.isEmpty() || !orderSetupPassed; }, 5000)
                        && orderSetupPassed && navigationSubmitted && navigationId > 0,
                    QStringLiteral("The real worker/queue order precondition was not achieved"));
        CHILD_CHECK(completionOrder.size() == 1
                        && completionOrder.first() == (allFirst ? QStringLiteral("all") : QStringLiteral("navigation")),
                    QStringLiteral("Observed dual-lane completion order differs from the requested row"));
        CHILD_CHECK(waitUntil([&] { return loads.saturated(); }, 1500),
                    QStringLiteral("The replacement holder did not occupy the completed lane's slot"));
        const qint64 remainingId = allFirst ? navigationId : allId;
        LargeFileSearch *remaining = allFirst ? navigationEngine.data() : allEngine.data();
        const AsyncTask held = manager.taskById(remainingId);
        CHILD_CHECK(held.state == AsyncTaskState::Queued && !held.startedAt.isValid()
                        && remaining->sourceInUse() && source->isReady() && loads.saturated(),
                    QStringLiteral("The second real reader was not still queued and source-owning"));
        CHILD_CHECK(publishPhase("QUEUED", QString::number(remainingId))
                        && publishPhase("DUAL_SUBMITTED", QStringLiteral("all=%1 nav=%2").arg(allId).arg(navigationId)),
                    QStringLiteral("Phase pipe unavailable"));
        {
            QFile truncate(path);
            CHILD_CHECK(truncate.open(QIODevice::ReadWrite), QStringLiteral("Cannot test mapped-file resize"));
            const bool resized = truncate.resize(kSourceSize - 1);
            truncate.close();
            CHILD_CHECK(!resized && fileHash(path) == expectedHash,
                        QStringLiteral("A still-reserved reader lost its mapping before completion"));
        }
        CHILD_CHECK(target->documentState().id == before.id && !target->isModified()
                        && target->isReadOnlyByPolicy() && fileHash(path) == expectedHash,
                    QStringLiteral("Dual-reader source guards failed"));
        CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString())
                        && publishPhase("FIRST_FINISHED_OTHER_QUEUED"),
                    QStringLiteral("Phase pipe unavailable"));
        loads.release();
        CHILD_CHECK(waitUntil([&] {
            return allTerminals == 1 && navigationTerminals == 1
                && !source->isReady() && !allEngine->sourceInUse()
                && !navigationEngine->sourceInUse() && manager.activeThreadCount() == 0
                && next->isEnabled() && provider->selectionStartOffset() == 4096
                && provider->selectionEndOffset() == 4096 + kQuery.size();
        }, 5000), QStringLiteral("Both readers did not finish once and release the source"));
        CHILD_CHECK(orderSetupPassed && completionOrder.size() == 2
                        && provider->selectionStartOffset() == 4096
                        && provider->selectionEndOffset() == 4096 + kQuery.size()
                        && target->documentState().id == before.id
                        && target->contentRevision() == before.revision
                        && target->documentState().capabilities == before.capabilities
                        && fileHash(path) == expectedHash,
                    QStringLiteral("Dual-lane completion changed the byte/identity oracle"));
        CHILD_CHECK(publishPhase("BOTH_FINISHED_SOURCE_IDLE"), QStringLiteral("Phase pipe unavailable"));
        auto *close = window.findChild<QAction *>(QStringLiteral("file.close"));
        CHILD_CHECK(close && close->isEnabled(), QStringLiteral("Close unavailable after dual search"));
        CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
        QElapsedTimer closing;
        closing.start();
        close->trigger();
        CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(!target && !source && !allEngine && !navigationEngine,
                    QStringLiteral("Dual search source did not close"));
        CHILD_CHECK(publishPhase("TARGET_DESTROYED") && loads.drainAfterTargetDestruction()
                        && fileHash(path) == expectedHash && publishPhase("DRAINED"),
                    QStringLiteral("Dual search wrappers did not drain safely"));
        return true;
    }

    // These are implementation-time supplemental boundaries. The original
    // four after-submit cases below retain their real task-panel actions.
    if (cancelAtAdded || closeAtAdded || terminalRerun || hideReopen || sourceChangeReentry || stopCancelClosesSource || sourceCloseReenters || switchBackQuery || lateNavigation) {
        QPointer<LargeFileSearch> boundaryEngine = kind == QStringLiteral("findAll")
            ? source->findAllEngine() : source->navigationEngine();
        CHILD_CHECK(boundaryEngine, QStringLiteral("Prepared source has no search engine"));
        const auto expectedPriority = kind == QStringLiteral("findAll")
            ? AsyncTaskPriority::High : AsyncTaskPriority::Highest;
        const QString expectedCategory = kind == QStringLiteral("findAll")
            ? QStringLiteral("Search") : QStringLiteral("SearchNavigation");
        const QString expectedTitle = (kind == QStringLiteral("findAll")
            ? QStringLiteral("搜索大文件: %1") : QStringLiteral("查找下一个: %1"))
                .arg(QFileInfo(path).fileName());
        auto *closeAction = window.findChild<QAction *>(QStringLiteral("file.close"));
        CHILD_CHECK(closeAction && closeAction->isEnabled(), QStringLiteral("Real close action unavailable"));
        const auto sourceGuards = [&]() {
            if (!target || !provider || !source || !boundaryEngine
                || tabs->currentWidget() != target.data()
                || target->windowedReadOnlyProvider() != provider.data()
                || target->findChild<ReadOnlySearchSource *>() != source.data())
                return false;
            const DocumentState state = target->documentState();
            return state.id == before.id && state.revision == before.revision
                && state.location == before.location && state.origin == before.origin
                && state.persistence == before.persistence && state.contentKind == before.contentKind
                && state.securityClass == before.securityClass && state.capabilities == before.capabilities
                && state.dirty == before.dirty && !target->isModified()
                && target->encoding() == encodingBefore && target->contentSizeBytes() == kSourceSize
                && target->editor() == provider->editor() && target->editor()->isReadOnly()
                && target->editor()->text() == projectionBefore
                && target->editor()->isModified() == projectionModifiedBefore
                && provider->selectionStartOffset() == selectionStartBefore
                && provider->selectionEndOffset() == selectionEndBefore
                && fileHash(path) == expectedHash && !QApplication::activeModalWidget();
        };
        pattern->setText(QString::fromLatin1(kQuery));

        if (sourceChangeReentry) {
            auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
            auto *tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
            auto *summary = dock ? dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary")) : nullptr;
            auto *cancelSearch = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
            CHILD_CHECK(tree && summary && cancelSearch && sourceGuards(), QStringLiteral("Source-change fixture unavailable"));
            const QByteArray changedBytes = sourceChangeReplacementBytes();
            const QByteArray changedHash = QCryptographicHash::hash(changedBytes, QCryptographicHash::Sha256);
            const DiskFileVersion oldVersion = provider->sourceVersion();
            CHILD_CHECK(oldVersion.valid && !oldVersion.fileIdentity.isEmpty() && changedBytes.size() == kSourceSize,
                        QStringLiteral("Source-change initial version or replacement fixture is invalid"));
            DiskFileVersion changedVersion;
            quint64 acceptedRevision = 0;
            QString acceptedProjection;
            qint64 acceptedSelectionStart = -1;
            qint64 acceptedSelectionEnd = -1;
            qint64 taskA = 0;
            qint64 taskB = 0;
            int aSubmissions = 0;
            int bSubmissions = 0;
            int aDomainTerminals = 0;
            int bDomainTerminals = 0;
            int aManagerTerminals = 0;
            int bManagerTerminals = 0;
            int changedCallbacks = 0;
            bool replacementCommitted = false;
            bool insideContentCancel = false;
            bool bCommandAccepted = false;
            bool callbackFailed = false;
            QVector<qint64> bPreparations;
            const auto changedGuards = [&] {
                if (!target || !provider || !source || !boundaryEngine) return false;
                const DocumentState state = target->documentState();
                return tabs->currentWidget() == target.data() && target->windowedReadOnlyProvider() == provider.data()
                    && state.id == before.id && state.location == before.location && state.origin == before.origin
                    && state.persistence == before.persistence && state.contentKind == before.contentKind
                    && state.securityClass == before.securityClass && state.capabilities == before.capabilities
                    && state.dirty == before.dirty && !target->isModified() && target->isReadOnlyByPolicy()
                    && target->encoding() == encodingBefore && target->contentSizeBytes() == kSourceSize
                    && target->editor() == provider->editor() && target->editor()->isReadOnly()
                    && state.revision == acceptedRevision && acceptedRevision > before.revision
                    && provider->sourceVersion().fileIdentity == changedVersion.fileIdentity
                    && changedVersion.fileIdentity != oldVersion.fileIdentity
                    && target->editor()->text() == acceptedProjection
                    && provider->selectionStartOffset() == acceptedSelectionStart
                    && provider->selectionEndOffset() == acceptedSelectionEnd
                    && fileHash(path) == changedHash && !QApplication::activeModalWidget();
            };
            const auto trackSubmitted = QObject::connect(
                &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
                    const AsyncTask task = manager.taskById(id);
                    if (!task.title.contains(QFileInfo(path).fileName())) return;
                    if (insideContentCancel && task.categoryTag == QStringLiteral("Mmap")) {
                        bPreparations.append(id);
                        loads.track(id);
                    }
                    if (task.categoryTag != expectedCategory || task.title != expectedTitle) return;
                    loads.track(id);
                    if (pattern->text() == QString::fromLatin1(kQuery)) { taskA = id; ++aSubmissions; }
                    if (pattern->text() == QString::fromLatin1(kRerunQuery)) { taskB = id; ++bSubmissions; }
                }, Qt::DirectConnection);
            const auto trackManagerTerminal = QObject::connect(
                &manager, &AsyncTaskManager::taskFinished, &window, [&](qint64 id, AsyncTaskState) {
                    if (taskA > 0 && id == taskA) ++aManagerTerminals;
                    if (taskB > 0 && id == taskB) ++bManagerTerminals;
                }, Qt::DirectConnection);
            const auto disconnectTracking = qScopeGuard([&] {
                QObject::disconnect(trackSubmitted);
                QObject::disconnect(trackManagerTerminal);
            });
            CHILD_CHECK(clickPreparedQuery(findAll) && aSubmissions == 1 && taskA > 0
                            && manager.taskById(taskA).state == AsyncTaskState::Queued
                            && !manager.taskById(taskA).startedAt.isValid() && loads.saturated() && sourceGuards(),
                        QStringLiteral("Source-change A was not a guarded real Queued search"));
            CHILD_CHECK(publishPhase("QUEUED", QString::number(taskA))
                            && publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));

            // Registered after A's real submit: the existing request consumes
            // its terminal and settles Find before this observer submits B.
            const auto onDomainTerminal = QObject::connect(
                target.data(), &DocumentSearchSurface::findAllL2Finished, &window,
                [&](SearchResultsModel *model, const QString &finishedPattern, const QString &) {
                    if (finishedPattern == QString::fromLatin1(kRerunQuery)) { ++bDomainTerminals; return; }
                    if (finishedPattern != QString::fromLatin1(kQuery)) return;
                    ++aDomainTerminals;
                    if (aDomainTerminals != 1) { callbackFailed = true; return; }
                    bCommandAccepted = [&]() -> bool {
                        CHILD_CHECK(insideContentCancel && replacementCommitted && !source->isReady()
                                        && model && model->isIncomplete() && loads.saturated()
                                        && dialog->isVisible() && findAll->isVisible()
                                        && pattern->isEnabled() && findAll->isEnabled() && !cancelSearch->isVisible(),
                                    QStringLiteral("A did not settle synchronously inside real content-change cancellation"));
                        QString refreshError;
                        CHILD_CHECK(provider->refreshFileMapping(&refreshError), refreshError);
                        acceptedRevision = target->contentRevision();
                        acceptedProjection = target->editor()->text();
                        acceptedSelectionStart = provider->selectionStartOffset();
                        acceptedSelectionEnd = provider->selectionEndOffset();
                        CHILD_CHECK(changedGuards() && acceptedProjection != projectionBefore
                                        && acceptedProjection.contains(QString::fromLatin1(kRerunQuery))
                                        && !acceptedProjection.contains(QString::fromLatin1(kQuery))
                                        && provider->sourceBytes(0, 64) == changedBytes.first(64),
                                    QStringLiteral("Real refresh did not admit exactly the new ordinary version"));
                        CHILD_CHECK(publishPhase("NEW_VERSION_ACCEPTED", QString::number(acceptedRevision)),
                                    QStringLiteral("Phase pipe unavailable"));
                        pattern->setText(QString::fromLatin1(kRerunQuery));
                        QSignalSpy bClicks(findAll, &QPushButton::clicked);
                        // Real mouse input without nested event dispatch: B
                        // may first queue Mmap, which is sufficient admission
                        // here. Its Search task is checked after outer A exits.
                        const QPoint point = findAll->rect().center();
                        const QPointF global(findAll->mapToGlobal(point));
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), global,
                                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), global,
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(findAll, &press);
                        QApplication::sendEvent(findAll, &release);
                        CHILD_CHECK(bClicks.size() == 1 && bPreparations.size() + bSubmissions == 1
                                        && cancelSearch->isVisible() && !pattern->isEnabled() && !findAll->isEnabled(),
                                    QStringLiteral("B did not submit actual work inside A's synchronous terminal"));
                        CHILD_CHECK(publishPhase("SOURCE_CHANGE_B_COMMAND",
                                        QStringLiteral("preparation=%1 search=%2").arg(bPreparations.value(0)).arg(taskB)),
                                    QStringLiteral("Phase pipe unavailable"));
                        return true;
                    }();
                    callbackFailed = !bCommandAccepted;
                }, Qt::DirectConnection);
            const auto onContentChanged = QObject::connect(
                target.data(), &EditorTab::contentChanged, &window, [&](quint64 revision) {
                    if (!replacementCommitted || changedCallbacks != 0) return;
                    ++changedCallbacks;
                    const bool canceled = [&]() -> bool {
                        CHILD_CHECK(!source->isReady() && loads.saturated() && revision > before.revision
                                        && aDomainTerminals == 0 && target->documentState().id == before.id
                                        && target->editor()->text() == projectionBefore && fileHash(path) == changedHash,
                                    QStringLiteral("Actual validation did not invalidate A before refreshing its projection"));
                        insideContentCancel = true;
                        const auto leaveCancel = qScopeGuard([&] { insideContentCancel = false; });
                        target->cancelFindAllL2();
                        CHILD_CHECK(aDomainTerminals == 1 && bCommandAccepted && !callbackFailed,
                                    QStringLiteral("Content-change Cancel did not synchronously settle A and admit B"));
                        return true;
                    }();
                    callbackFailed = callbackFailed || !canceled;
                }, Qt::DirectConnection);
            // ReadOnlySearchSource connected its release slot in its ctor,
            // before this observer. No event pumping between commit and the
            // outer Editor completion's ordinary-source version validation.
            const auto onIdle = QObject::connect(
                boundaryEngine.data(), &LargeFileSearch::sourceBecameIdle, &window, [&] {
                    if (replacementCommitted || taskA <= 0 || callbackFailed) return;
                    const bool replaced = [&]() -> bool {
                        CHILD_CHECK(!source->isReady() && !boundaryEngine->sourceInUse()
                                        && boundaryEngine->totalMatches() == 33 && sourceGuards()
                                        && aDomainTerminals == 0 && loads.addHolder(true) && loads.saturated(),
                                    QStringLiteral("A did not finish its real scan and release the mapping before replacement"));
                        QSaveFile replacement(path);
                        replacement.setDirectWriteFallback(false);
                        CHILD_CHECK(replacement.open(QIODevice::WriteOnly)
                                        && replacement.write(changedBytes) == changedBytes.size() && replacement.commit(),
                                    replacement.errorString());
                        replacementCommitted = true;
                        CHILD_CHECK(captureDiskFileVersion(path, &changedVersion) == DiskFileState::Unchanged
                                        && changedVersion.valid && changedVersion.size == kSourceSize
                                        && changedVersion.fileIdentity != oldVersion.fileIdentity
                                        && fileHash(path) == changedHash && target->contentRevision() == before.revision
                                        && !source->isReady(), QStringLiteral("Atomic replacement did not establish the guarded new source"));
                        CHILD_CHECK(publishPhase("SOURCE_REPLACED_AT_IDLE", QString::fromLatin1(changedHash.toHex())),
                                    QStringLiteral("Phase pipe unavailable"));
                        return true;
                    }();
                    callbackFailed = callbackFailed || !replaced;
                }, Qt::DirectConnection);
            const auto disconnectReentry = qScopeGuard([&] {
                QObject::disconnect(onIdle);
                QObject::disconnect(onContentChanged);
                QObject::disconnect(onDomainTerminal);
            });
            CHILD_CHECK(loads.releaseOne(), QStringLiteral("Could not release one worker for A's real scan"));
            const bool bAdmitted = waitUntil([&] { return callbackFailed || bridgeFailed || (bSubmissions == 1 && loads.saturated()); }, 6000);
            QString admissionDiagnostic = QStringLiteral("accepted=%1 callbackFailed=%2 bridgeFailed=%3 slotReleased=%4 sourceReady=%5 preparation=%6 BSearch=%7 %8")
                .arg(bAdmitted).arg(callbackFailed).arg(bridgeFailed).arg(preparationSlot)
                .arg(source && source->isReady()).arg(source ? source->preparationTaskId() : -1)
                .arg(taskB).arg(loads.queueEvidence());
            for (const qint64 preparation : bPreparations) {
                const AsyncTask task = manager.taskById(preparation);
                admissionDiagnostic += QStringLiteral(" mapping=%1 state=%2 started=%3 error=%4")
                    .arg(preparation).arg(int(task.state)).arg(task.startedAt.isValid()).arg(task.error.message);
            }
            CHILD_CHECK(publishPhase("SOURCE_CHANGE_ADMISSION_DIAGNOSTIC", admissionDiagnostic), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(bAdmitted && !bridgeFailed
                            && !callbackFailed && replacementCommitted && changedCallbacks == 1 && bCommandAccepted,
                        QStringLiteral("Real source-change terminal reentry did not admit B"));
            bool outerTail = false;
            QObject fence;
            QMetaObject::invokeMethod(&fence, [&] {
                QMetaObject::invokeMethod(&fence, [&] { outerTail = true; }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
            CHILD_CHECK(waitUntil([&] { return outerTail; }, 1000) && !callbackFailed && changedGuards()
                            && taskB > 0 && taskB != taskA && bSubmissions == 1
                            && manager.taskById(taskB).state == AsyncTaskState::Queued
                            && manager.taskById(taskB).priority == AsyncTaskPriority::High
                            && !manager.taskById(taskB).startedAt.isValid() && loads.saturated()
                            && aDomainTerminals == 1 && bDomainTerminals == 0
                            && cancelSearch->isVisible() && !pattern->isEnabled() && !findAll->isEnabled(),
                        QStringLiteral("Outer A completion reset B or emitted a duplicate A terminal"));
            CHILD_CHECK(publishPhase("SOURCE_CHANGE_B_QUEUED", QString::number(taskB)), QStringLiteral("Phase pipe unavailable"));
            loads.release();
            CHILD_CHECK(waitUntil([&] {
                return bDomainTerminals == 1 && aManagerTerminals == 1 && bManagerTerminals == 1
                    && manager.taskById(taskB).state == AsyncTaskState::Finished
                    && findAll->isEnabled() && pattern->isEnabled() && !cancelSearch->isVisible()
                    && manager.queuedCount() == 0 && manager.runningCount() == 0 && manager.activeThreadCount() == 0;
            }, 5000), QStringLiteral("B did not finish after the outer A source-change callback returned"));
            const AsyncTaskState aState = manager.taskById(taskA).state;
            CHILD_CHECK(changedGuards() && !callbackFailed && aDomainTerminals == 1 && bDomainTerminals == 1
                            && aSubmissions == 1 && bSubmissions == 1
                            && (aState == AsyncTaskState::Finished || aState == AsyncTaskState::Canceled),
                        QStringLiteral("Source-change final identity/data or exactly-once terminal guards failed"));
            auto *result = qobject_cast<SearchResultsModel *>(tree->model());
            CHILD_CHECK(result && !result->isIncomplete() && result->totalMatches() == 33 && result->rowCount() == 33,
                        QStringLiteral("New-version B results are missing or incomplete"));
            for (int row = 0; row < 33; ++row) {
                const QModelIndex match = result->index(row, SearchResultsModel::COL_CONTEXT);
                CHILD_CHECK(result->matchByteOffset(match) == qint64(row) * 1024 * 1024 + kRerunOffset
                                && result->matchLength(match) == kRerunQuery.size(),
                            QStringLiteral("New-version B range does not match its independent byte oracle"));
            }
            CHILD_CHECK(pattern->text() == QString::fromLatin1(kRerunQuery)
                            && summary->text().contains(QString::fromLatin1(kRerunQuery))
                            && !summary->text().contains(QString::fromLatin1(kQuery))
                            && publishPhase("SOURCE_CHANGE_B_FINISHED",
                                QStringLiteral("A=%1 state=%2 manager=%3 domain=%4 B=%5 manager=%6 domain=%7")
                                    .arg(taskA).arg(int(aState)).arg(aManagerTerminals).arg(aDomainTerminals)
                                    .arg(taskB).arg(bManagerTerminals).arg(bDomainTerminals)),
                        QStringLiteral("Final source-change results belong to the wrong query"));
            CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
            QElapsedTimer closing;
            closing.start();
            closeAction->trigger();
            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(!target && !provider && !source && !boundaryEngine && publishPhase("TARGET_DESTROYED"),
                        QStringLiteral("Source-change query target did not close"));
            CHILD_CHECK(loads.drainAfterTargetDestruction() && fileHash(path) == changedHash
                            && aDomainTerminals == 1 && bDomainTerminals == 1
                            && aManagerTerminals == 1 && bManagerTerminals == 1 && publishPhase("DRAINED"),
                        QStringLiteral("Late source-change work altered the final source or terminal counts"));
            return true;
        }

        if (hideReopen) {
            auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
            auto *tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
            auto *summary = dock ? dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary")) : nullptr;
            auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
            auto *closeFind = dialog->findChild<QPushButton *>(QStringLiteral("closeSearchButton"));
            auto *cancelSearch = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
            CHILD_CHECK(tree && summary && status && closeFind && cancelSearch && sourceGuards(),
                        QStringLiteral("Hide/reopen fixture or real Find controls unavailable"));
            QVector<qint64> queryIds;
            int aTerminals = 0;
            int bTerminals = 0;
            const auto trackQueries = QObject::connect(
                &manager, &AsyncTaskManager::taskAdded, &window, [&](qint64 id) {
                    const AsyncTask task = manager.taskById(id);
                    if (task.categoryTag == expectedCategory && task.title == expectedTitle) {
                        queryIds.append(id);
                        loads.track(id);
                    }
                });
            const auto trackTerminals = QObject::connect(
                target.data(), &DocumentSearchSurface::findAllL2Finished, &window,
                [&](SearchResultsModel *, const QString &finishedPattern, const QString &) {
                    if (finishedPattern == QString::fromLatin1(kQuery)) ++aTerminals;
                    if (finishedPattern == QString::fromLatin1(kRerunQuery)) ++bTerminals;
                });
            const auto disconnectObservers = qScopeGuard([&] {
                QObject::disconnect(trackQueries);
                QObject::disconnect(trackTerminals);
            });
            CHILD_CHECK(clickPreparedQuery(findAll) && queryIds.size() == 1,
                        QStringLiteral("A was not submitted exactly once through real Find All"));
            const qint64 taskA = queryIds.constFirst();
            const auto queuedSearch = [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                return id > 0 && id != warmupId && task.id == id
                    && task.type == AsyncTaskType::LargeFileSearch
                    && task.categoryTag == expectedCategory && task.title == expectedTitle
                    && task.priority == AsyncTaskPriority::High
                    && task.state == AsyncTaskState::Queued && !task.startedAt.isValid()
                    && task.progressPhase.isEmpty() && task.progressDone == 0 && loads.saturated();
            };
            CHILD_CHECK(sourceGuards() && queuedSearch(taskA) && aTerminals == 0
                            && cancelSearch->isVisible() && cancelSearch->isEnabled()
                            && !findAll->isEnabled() && !pattern->isEnabled(),
                        QStringLiteral("A did not reach the guarded real Queued state"));
            CHILD_CHECK(publishPhase("QUEUED", QString::number(taskA))
                            && publishPhase("GUARDS_OK", before.id.toString()),
                        QStringLiteral("Phase pipe unavailable"));

            const QPointer<FindOverlay> originalDialog(dialog);
            const QPointer<QAbstractItemModel> preview(tree->model());
            CHILD_CHECK(preview && closeFind->isVisible() && closeFind->isEnabled(),
                        QStringLiteral("Active query has no preview or enabled close control"));
            const int previewRows = preview->rowCount();
            const QString activeSummary = summary->text();
            const QString activeStatus = status->text();
            const QVariant activeScope = scope->currentData();
            QSignalSpy hiddenTaskSubmissions(&manager, &AsyncTaskManager::taskAdded);
            QSignalSpy closeClicks(closeFind, &QPushButton::clicked);
            QTest::mouseClick(closeFind, Qt::LeftButton);
            CHILD_CHECK(originalDialog && !originalDialog->isVisible() && closeClicks.size() == 1,
                        QStringLiteral("Real close did not hide the existing Find window"));
            CHILD_CHECK(sourceGuards() && queuedSearch(taskA) && queryIds.size() == 1
                            && aTerminals == 0 && hiddenTaskSubmissions.isEmpty(),
                        QStringLiteral("Hiding Find canceled/replaced A or changed its source"));
            CHILD_CHECK(publishPhase("FIND_HIDDEN", QString::number(taskA)), QStringLiteral("Phase pipe unavailable"));
            findAction->trigger();
            CHILD_CHECK(originalDialog && QTest::qWaitForWindowExposed(originalDialog.data())
                            && window.findChild<FindOverlay *>() == originalDialog.data()
                            && window.findChildren<FindOverlay *>().size() == 1,
                        QStringLiteral("Real Find action did not reopen the same dialog"));
            CHILD_CHECK(sourceGuards() && queuedSearch(taskA) && queryIds.size() == 1
                            && hiddenTaskSubmissions.isEmpty() && aTerminals == 0 && bTerminals == 0,
                        QStringLiteral("Reopening Find submitted work, canceled A, or changed source state"));
            CHILD_CHECK(pattern->text() == QString::fromLatin1(kQuery) && scope->currentData() == activeScope
                            && !regex->isChecked() && !hex->isChecked() && !wholeWord->isChecked()
                            && !pattern->isEnabled() && !scope->isEnabled() && !findAll->isEnabled()
                            && cancelSearch->isVisible() && cancelSearch->isEnabled()
                            && preview && tree->model() == preview.data() && preview->rowCount() == previewRows
                            && summary->text() == activeSummary && status->text() == activeStatus,
                        QStringLiteral("Reopened Find lost the active query, busy state, or preview"));
            CHILD_CHECK(publishPhase("FIND_REOPENED_SAME_QUERY", QString::number(taskA)), QStringLiteral("Phase pipe unavailable"));

            QTest::mouseClick(cancelSearch, Qt::LeftButton);
            CHILD_CHECK(waitUntil([&] { return manager.taskById(taskA).state == AsyncTaskState::Canceled; }, 1000)
                            && sourceGuards(), QStringLiteral("Reopened Find cancellation changed data or failed to cancel A"));
            CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(taskA)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(waitUntil([&] {
                return aTerminals == 1 && pattern->isEnabled() && findAll->isEnabled() && !cancelSearch->isVisible();
            }, 1500) && sourceGuards() && loads.saturated() && queryIds.size() == 1,
                        QStringLiteral("Canceled A did not settle the reopened query UI exactly once"));
            pattern->setText(QString::fromLatin1(kRerunQuery));
            CHILD_CHECK(clickPreparedQuery(findAll) && queryIds.size() == 2,
                        QStringLiteral("B did not submit exactly one new real search after cancellation"));
            const qint64 taskB = queryIds.constLast();
            CHILD_CHECK(taskB != taskA && queuedSearch(taskB) && sourceGuards()
                            && aTerminals == 1 && bTerminals == 0
                            && cancelSearch->isVisible() && !pattern->isEnabled() && !findAll->isEnabled(),
                        QStringLiteral("B lacks an independent Queued task and guarded busy UI"));
            // Dispatch the old cancellation tail before B can run. This is an
            // event-order fence, not a sleep intended to win a worker race.
            bool deliveryTail = false;
            QObject fence;
            QMetaObject::invokeMethod(&fence, [&] {
                QMetaObject::invokeMethod(&fence, [&] { deliveryTail = true; }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
            CHILD_CHECK(waitUntil([&] { return deliveryTail; }, 1000) && queuedSearch(taskB)
                            && sourceGuards() && aTerminals == 1 && bTerminals == 0 && queryIds.size() == 2
                            && !pattern->isEnabled() && !findAll->isEnabled() && cancelSearch->isVisible()
                            && pattern->text() == QString::fromLatin1(kRerunQuery)
                            && summary->text().contains(QString::fromLatin1(kRerunQuery))
                            && !summary->text().contains(QString::fromLatin1(kQuery)),
                        QStringLiteral("Late A cancellation changed B's pending query"));
            CHILD_CHECK(publishPhase("REOPEN_B_QUEUED", QString::number(taskB)), QStringLiteral("Phase pipe unavailable"));
            loads.release();
            CHILD_CHECK(waitUntil([&] {
                return bTerminals == 1 && manager.taskById(taskB).state == AsyncTaskState::Finished
                    && findAll->isEnabled() && pattern->isEnabled() && !cancelSearch->isVisible()
                    && manager.queuedCount() == 0 && manager.runningCount() == 0 && manager.activeThreadCount() == 0;
            }, 5000), QStringLiteral("B did not complete after reopening Find and canceling A"));
            CHILD_CHECK(sourceGuards() && originalDialog && originalDialog->isVisible()
                            && queryIds.size() == 2 && aTerminals == 1 && bTerminals == 1
                            && manager.taskById(taskA).state == AsyncTaskState::Canceled,
                        QStringLiteral("Completed B changed source data or received duplicate/late A terminal"));
            auto *result = qobject_cast<SearchResultsModel *>(tree->model());
            CHILD_CHECK(result && !result->isIncomplete() && result->totalMatches() == 33 && result->rowCount() == 33,
                        QStringLiteral("B final result is missing, incomplete, or has an incorrect count"));
            for (int row = 0; row < 33; ++row) {
                const QModelIndex match = result->index(row, SearchResultsModel::COL_CONTEXT);
                CHILD_CHECK(result->matchByteOffset(match) == qint64(row) * 1024 * 1024 + kRerunOffset
                                && result->matchLength(match) == kRerunQuery.size(),
                            QStringLiteral("B final result contains an old or incorrect byte range"));
            }
            CHILD_CHECK(pattern->text() == QString::fromLatin1(kRerunQuery)
                            && summary->text().contains(QString::fromLatin1(kRerunQuery))
                            && !summary->text().contains(QString::fromLatin1(kQuery))
                            && publishPhase("REOPEN_B_FINISHED", QString::number(taskB)),
                        QStringLiteral("Final B feedback was polluted by A, or phase pipe unavailable"));
            CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
            QElapsedTimer closing;
            closing.start();
            closeAction->trigger();
            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(!target && !provider && !source && !boundaryEngine && publishPhase("TARGET_DESTROYED"),
                        QStringLiteral("Completed hide/reopen query source did not close"));
            CHILD_CHECK(loads.drainAfterTargetDestruction() && fileHash(path) == expectedHash
                            && aTerminals == 1 && bTerminals == 1 && queryIds.size() == 2
                            && publishPhase("DRAINED"), QStringLiteral("Hide/reopen late work changed source or terminal state"));
            return true;
        }

        if (lateNavigation) {
            const QString secondPath = QFileInfo(path).dir().filePath(QStringLiteral("navigation-second.txt"));
            const QByteArray secondHash = fileHash(secondPath);
            window.openFilesFromExternal({secondPath});
            QPointer<EditorTab> second = qobject_cast<EditorTab *>(tabs->currentWidget());
            CHILD_CHECK(second && second != target && !second->isL2Mode() && !second->isSecureDocument()
                            && !second->isModified() && second->filePath() == secondPath,
                        QStringLiteral("B did not open as an independent ordinary document"));
            const DocumentState secondState = second->documentState();
            const QString secondBody = second->editor()->text();
            const qint64 secondOffset = secondBody.toUtf8().indexOf(kNavigationQuery);
            CHILD_CHECK(secondBody.toUtf8().size() == 512 * 1024 && secondOffset == kRerunOffset,
                        QStringLiteral("B fixture must exceed the inline budget with an exact independent hit"));
            const qint64 secondSelectionStart = second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
            const qint64 secondSelectionEnd = second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
            const auto bothGuards = [&]() {
                return target && provider && source && boundaryEngine && second
                    && target->windowedReadOnlyProvider() == provider.data() && target->findChild<ReadOnlySearchSource *>() == source.data()
                    && target->documentState().id == before.id && target->contentRevision() == before.revision
                    && target->documentState().capabilities == before.capabilities
                    && target->editor()->text() == projectionBefore && !target->isModified()
                    && target->isReadOnlyByPolicy() && target->contentSizeBytes() == kSourceSize
                    && target->encoding() == encodingBefore
                    && provider->selectionStartOffset() == selectionStartBefore
                    && provider->selectionEndOffset() == selectionEndBefore
                    && second->documentState().id == secondState.id && second->contentRevision() == secondState.revision
                    && second->documentState().capabilities == secondState.capabilities
                    && second->editor()->text() == secondBody && !second->isModified()
                    && fileHash(path) == expectedHash && !secondHash.isEmpty()
                    && fileHash(secondPath) == secondHash && !QApplication::activeModalWidget();
            };
            tabs->setCurrentWidget(target.data());
            pattern->setText(QString::fromLatin1(kQuery));
            auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
            auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
            CHILD_CHECK(previous && status && next->isEnabled() && sourceGuards() && bothGuards(),
                        QStringLiteral("Navigation A/B initial data or UI guards failed"));
            qint64 taskA = 0;
            qint64 taskB = 0;
            int canceledCallbacks = 0;
            bool insideCancelClick = false;
            bool submittedB = false;
            bool deliveryTail = false;
            QString bStatus;
            QObject fenceLifetime;
            const QPointer<QObject> fenceGuard(&fenceLifetime);
            // Precede A's reservation terminal connection. The real TaskPanel
            // cancel enters here before A can enqueue its business callback.
            const auto beforeCanceled = QObject::connect(
                &manager, &AsyncTaskManager::taskFinished, &window,
                [&](qint64 id, AsyncTaskState state) {
                    if (id != taskA || state != AsyncTaskState::Canceled || canceledCallbacks != 0) return;
                    ++canceledCallbacks;
                    submittedB = [&]() -> bool {
                        CHILD_CHECK(insideCancelClick && manager.taskById(id).id == taskA
                                        && manager.taskById(id).state == AsyncTaskState::Canceled
                                        && loads.saturated() && bothGuards(),
                                    QStringLiteral("A did not cancel through the exact guarded TaskPanel callback"));
                        CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(id))
                                        && publishPhase("NAV_CANCELED_DIRECT"), QStringLiteral("Phase pipe unavailable"));
                        tabs->setCurrentWidget(second.data());
                        findAction->trigger();
                        pattern->setText(QString::fromLatin1(kNavigationQuery));
                        CHILD_CHECK(bothGuards(), QStringLiteral("Switching to B changed source data"));
                        CHILD_CHECK(next->isEnabled(),
                                    QStringLiteral("Canceled A left B's real Next disabled before submission"));
                        QSignalSpy bAdded(&manager, &AsyncTaskManager::taskAdded);
                        QSignalSpy bClicked(next, &QPushButton::clicked);
                        QTest::mouseClick(next, Qt::LeftButton);
                        int matches = 0;
                        for (const QList<QVariant> &arguments : bAdded) {
                            if (arguments.isEmpty()) continue;
                            const qint64 candidate = arguments.constFirst().toLongLong();
                            const AsyncTask task = manager.taskById(candidate);
                            if (task.categoryTag == QStringLiteral("SearchNavigation")
                                && task.title == QStringLiteral("查找下一个: %1").arg(QFileInfo(secondPath).fileName())) {
                                taskB = candidate;
                                ++matches;
                                loads.track(candidate);
                            }
                        }
                        const AsyncTask b = manager.taskById(taskB);
                        CHILD_CHECK(bClicked.size() == 1 && matches == 1 && taskB > 0 && taskB != taskA
                                        && b.id == taskB && b.state == AsyncTaskState::Queued
                                        && b.priority == AsyncTaskPriority::Highest && !b.startedAt.isValid()
                                        && loads.saturated() && !next->isEnabled() && !previous->isEnabled(),
                                    QStringLiteral("B was not a distinct real Highest-priority queued navigation"));
                        bStatus = status->text();
                        CHILD_CHECK(publishPhase("NAV_B_QUEUED_IN_CANCEL", QString::number(taskB)), QStringLiteral("Phase pipe unavailable"));
                        return QMetaObject::invokeMethod(&manager, [fenceGuard, &manager, &deliveryTail]() {
                            if (!fenceGuard) return;
                            QMetaObject::invokeMethod(&manager, [fenceGuard, &deliveryTail]() {
                                if (fenceGuard) deliveryTail = true;
                            }, Qt::QueuedConnection);
                        }, Qt::QueuedConnection);
                    }();
                }, Qt::DirectConnection);
            const auto disconnectCanceled = qScopeGuard([&]() { QObject::disconnect(beforeCanceled); });
            QSignalSpy aAdded(&manager, &AsyncTaskManager::taskAdded);
            CHILD_CHECK(clickPreparedQuery(next), QStringLiteral("Navigation A was not admitted"));
            taskA = boundaryEngine->lastTaskId();
            int aMatches = 0;
            for (const QList<QVariant> &arguments : aAdded) {
                if (!arguments.isEmpty() && arguments.constFirst().toLongLong() == taskA) ++aMatches;
            }
            loads.track(taskA);
            const AsyncTask a = manager.taskById(taskA);
            CHILD_CHECK(taskA > 0 && taskA != warmupId && aMatches == 1 && a.id == taskA
                            && a.categoryTag == expectedCategory && a.title == expectedTitle
                            && a.priority == AsyncTaskPriority::Highest && a.state == AsyncTaskState::Queued
                            && !a.startedAt.isValid() && loads.saturated() && !next->isEnabled(),
                        QStringLiteral("A was not a real unstarted navigation task"));
            CHILD_CHECK(publishPhase("QUEUED", QString::number(taskA)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(sourceGuards() && bothGuards(), QStringLiteral("Navigation source guards failed before A cancel"));
            CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));
            auto *taskButton = window.findChild<QToolButton *>(QStringLiteral("StatusBarTaskWidget_btn"));
            CHILD_CHECK(taskButton && taskButton->isVisible(), QStringLiteral("Task panel entry unavailable"));
            QTest::mouseClick(taskButton, Qt::LeftButton);
            auto *panel = window.findChild<AsyncTaskPanelDialog *>();
            CHILD_CHECK(panel && QTest::qWaitForWindowExposed(panel), QStringLiteral("Task panel not exposed"));
            auto *list = panel->findChild<QListView *>(QStringLiteral("backgroundTaskList"));
            auto *cancel = panel->findChild<QPushButton *>(QStringLiteral("cancelSelectedTaskButton"));
            const QModelIndex selected = indexForTask(list, taskA);
            CHILD_CHECK(list && cancel && selected.isValid(), QStringLiteral("Exact navigation A task row unavailable"));
            list->setCurrentIndex(selected);
            list->scrollTo(selected);
            CHILD_CHECK(cancel->isEnabled() && selected.data(AsyncTaskManager::TaskCancellableRole).toBool(),
                        QStringLiteral("Navigation A is not cancellable through the real panel"));
            insideCancelClick = true;
            QTest::mouseClick(cancel, Qt::LeftButton);
            insideCancelClick = false;
            CHILD_CHECK(canceledCallbacks == 1 && submittedB,
                        QStringLiteral("B was not submitted inside A's actual cancellation before old callback scheduling"));
            CHILD_CHECK(waitUntil([&]() { return deliveryTail; }, 1500), QStringLiteral("Old navigation delivery tail did not run"));
            CHILD_CHECK(bothGuards(), QStringLiteral("Late A navigation changed source data"));
            CHILD_CHECK(manager.taskById(taskA).state == AsyncTaskState::Canceled
                            && manager.taskById(taskB).state == AsyncTaskState::Queued
                            && !manager.taskById(taskB).startedAt.isValid() && loads.saturated()
                            && tabs->currentWidget() == second.data()
                            && pattern->text() == QString::fromLatin1(kNavigationQuery)
                            && !next->isEnabled() && !previous->isEnabled() && status->text() == bStatus
                            && second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == secondSelectionStart
                            && second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == secondSelectionEnd,
                        QStringLiteral("Late canceled A cleared B's queued busy state or changed its selection/status"));
            CHILD_CHECK(publishPhase("NAV_B_BUSY_AFTER_A"), QStringLiteral("Phase pipe unavailable"));
            loads.release();
            CHILD_CHECK(waitUntil([&]() {
                return second && manager.taskById(taskB).state == AsyncTaskState::Finished
                    && next->isEnabled()
                    && second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == secondOffset
                    && second->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == secondOffset + kNavigationQuery.size();
            }, 3000), QStringLiteral("B did not finish with its exact independent navigation hit"));
            CHILD_CHECK(bothGuards() && canceledCallbacks == 1 && manager.taskById(taskA).state == AsyncTaskState::Canceled,
                        QStringLiteral("Completed B or late A changed source data or task state"));
            CHILD_CHECK(publishPhase("NAV_B_FINISHED", QString::number(taskB)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
            QElapsedTimer closing;
            closing.start();
            tabs->setCurrentWidget(target.data());
            closeAction->trigger();
            CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull() && boundaryEngine.isNull(),
                        QStringLiteral("Navigation source A did not close"));
            tabs->setCurrentWidget(second.data());
            closeAction->trigger();
            CHILD_CHECK(second.isNull(), QStringLiteral("Navigation source B did not close"));
            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed()))
                            && publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(loads.drainAfterTargetDestruction() && fileHash(path) == expectedHash
                            && fileHash(secondPath) == secondHash && canceledCallbacks == 1,
                        QStringLiteral("Navigation source teardown or late wrapper drain changed source data"));
            CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
            return true;
        }

        if (switchBackQuery) {
            auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
            auto *tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
            auto *summary = dock ? dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary")) : nullptr;
            auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
            auto *cancelSearch = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
            CHILD_CHECK(tree && summary && status && cancelSearch, QStringLiteral("Missing real query feedback controls"));
            const QString otherPath = QFileInfo(path).dir().filePath(QStringLiteral("query-switch-other.txt"));
            const QByteArray otherBytes("ordinary companion document\n");
            {
                QFile otherFile(otherPath);
                CHILD_CHECK(otherFile.open(QIODevice::WriteOnly)
                                && otherFile.write(otherBytes) == otherBytes.size(),
                            QStringLiteral("Could not create ordinary switch target"));
            }
            const QByteArray otherHash = fileHash(otherPath);
            window.openFilesFromExternal({otherPath});
            QPointer<EditorTab> other = qobject_cast<EditorTab *>(tabs->currentWidget());
            CHILD_CHECK(other && other != target && !other->isL2Mode() && !other->isSecureDocument()
                            && !other->isModified() && other->filePath() == otherPath,
                        QStringLiteral("Companion did not open as a distinct ordinary document"));
            const DocumentState otherState = other->documentState();
            const auto companionGuards = [&]() {
                return other && other->documentState().id == otherState.id
                    && other->contentRevision() == otherState.revision
                    && other->documentState().capabilities == otherState.capabilities
                    && !other->isModified() && other->editor()->text() == QString::fromUtf8(otherBytes)
                    && !otherHash.isEmpty() && fileHash(otherPath) == otherHash;
            };
            tabs->setCurrentWidget(target.data());
            pattern->setText(QString::fromLatin1(kQuery));
            CHILD_CHECK(sourceGuards() && companionGuards() && loads.saturated() && findAll->isEnabled(),
                        QStringLiteral("Switch-query initial source/UI guards failed"));
            QSignalSpy aSubmitted(&manager, &AsyncTaskManager::taskAdded);
            CHILD_CHECK(clickPreparedQuery(findAll), QStringLiteral("Switch-query A was not admitted after preparation"));
            const qint64 taskA = boundaryEngine->lastTaskId();
            int aSubmissions = 0;
            for (const QList<QVariant> &arguments : aSubmitted) {
                if (!arguments.isEmpty() && arguments.constFirst().toLongLong() == taskA)
                    ++aSubmissions;
            }
            loads.track(taskA);
            const AsyncTask queuedA = manager.taskById(taskA);
            CHILD_CHECK(taskA > 0 && taskA != warmupId && aSubmissions == 1
                            && queuedA.id == taskA && queuedA.categoryTag == expectedCategory
                            && queuedA.title == expectedTitle && queuedA.priority == AsyncTaskPriority::High
                            && queuedA.state == AsyncTaskState::Queued && !queuedA.startedAt.isValid()
                            && loads.saturated() && cancelSearch->isVisible() && !findAll->isEnabled(),
                        QStringLiteral("A was not an exact real queued Find All"));
            CHILD_CHECK(publishPhase("QUEUED", QString::number(taskA)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(sourceGuards() && companionGuards(), QStringLiteral("Source guards failed before leaving A"));
            CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));

            tabs->setCurrentWidget(other.data());
            CHILD_CHECK(tabs->currentWidget() == other.data(), QStringLiteral("Did not actually switch away from A"));
            tabs->setCurrentWidget(target.data());
            CHILD_CHECK(sourceGuards() && companionGuards() && findAll->isEnabled() && pattern->isEnabled(),
                        QStringLiteral("Returning to the source did not permit a new query attempt"));
            const QString queryB = invalidNextQuery ? QStringLiteral("(") : QString::fromLatin1(kRerunQuery);
            regex->setChecked(invalidNextQuery);
            pattern->setText(queryB);
            QSignalSpy bSubmitted(&manager, &AsyncTaskManager::taskAdded);
            QSignalSpy bClicks(findAll, &QPushButton::clicked);
            QTest::mouseClick(findAll, Qt::LeftButton);
            CHILD_CHECK(bClicks.size() == 1, QStringLiteral("B was not attempted through the real Find All control"));
            qint64 taskB = 0;
            int bSubmissions = 0;
            for (const QList<QVariant> &arguments : bSubmitted) {
                if (arguments.isEmpty()) continue;
                const qint64 id = arguments.constFirst().toLongLong();
                const AsyncTask task = manager.taskById(id);
                if (task.categoryTag == expectedCategory && task.title == expectedTitle) {
                    taskB = id;
                    ++bSubmissions;
                    loads.track(id);
                }
            }
            CHILD_CHECK(sourceGuards() && companionGuards() && loads.saturated(),
                        QStringLiteral("B attempt changed source data or released the held workers"));
            const bool acceptedB = bSubmissions == 1;
            CHILD_CHECK(bSubmissions <= 1 && (!invalidNextQuery || bSubmissions == 0),
                        QStringLiteral("B submitted duplicate work or an invalid regex started a worker"));
            if (acceptedB) {
                const AsyncTask queuedB = manager.taskById(taskB);
                CHILD_CHECK(taskB > 0 && taskB != taskA && queuedB.id == taskB
                                && queuedB.state == AsyncTaskState::Queued && !queuedB.startedAt.isValid()
                                && queuedB.priority == AsyncTaskPriority::High
                                && cancelSearch->isVisible() && !findAll->isEnabled() && !pattern->isEnabled()
                                && summary->text().contains(queryB),
                            QStringLiteral("Accepted B lacks its own real queued task and visible query state"));
            } else if (invalidNextQuery) {
                CHILD_CHECK(!status->text().isEmpty() && findAll->isEnabled()
                                && pattern->isEnabled() && !cancelSearch->isVisible(),
                            QStringLiteral("Invalid B did not expose a terminal validation error"));
            } else {
                auto *rejected = qobject_cast<SearchResultsModel *>(tree->model());
                CHILD_CHECK(rejected && rejected->isIncomplete() && summary->text().contains(queryB)
                                && findAll->isEnabled() && pattern->isEnabled() && !cancelSearch->isVisible(),
                            QStringLiteral("Rejected B did not expose its own incomplete terminal result"));
            }
            const QString bSummary = summary->text();
            const QString bStatus = status->text();
            const QString bStatusTooltip = status->toolTip();
            const QPointer<QAbstractItemModel> bModel(tree->model());
            const int bRows = bModel ? bModel->rowCount() : 0;
            CHILD_CHECK(publishPhase("QUERY_B_OBSERVED", QStringLiteral("accepted=%1 task=%2 status=%3 summary=%4")
                                        .arg(acceptedB).arg(taskB).arg(bStatus, bSummary)),
                        QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(sourceGuards() && companionGuards(), QStringLiteral("Source guards failed before releasing A"));
            loads.release();
            CHILD_CHECK(waitUntil([&]() {
                const AsyncTask a = manager.taskById(taskA);
                const bool aTerminal = a.id == taskA && a.state != AsyncTaskState::Queued
                    && a.state != AsyncTaskState::Running;
                const AsyncTask b = acceptedB ? manager.taskById(taskB) : AsyncTask{};
                return aTerminal && (!acceptedB || (b.id == taskB && b.state != AsyncTaskState::Queued
                                                    && b.state != AsyncTaskState::Running))
                    && manager.queuedCount() == 0 && manager.runningCount() == 0
                    && manager.activeThreadCount() == 0;
            }, 5000), QStringLiteral("Real A/B work did not reach terminal states after release"));
            // The workers post domain delivery before their Manager terminal.
            // Two GUI-dispatcher hops observe all already-queued deliveries,
            // including pending cancellations, without manufacturing a result.
            bool deliveryTail = false;
            QObject fenceLifetime;
            const QPointer<QObject> fenceGuard(&fenceLifetime);
            CHILD_CHECK(QMetaObject::invokeMethod(&manager,
                [fenceGuard, &manager, &deliveryTail]() {
                    if (!fenceGuard) return;
                    QMetaObject::invokeMethod(&manager, [fenceGuard, &deliveryTail]() {
                        if (fenceGuard) deliveryTail = true;
                    }, Qt::QueuedConnection);
                }, Qt::QueuedConnection), QStringLiteral("Could not enqueue delivery tail"));
            CHILD_CHECK(waitUntil([&]() { return deliveryTail; }, 1500), QStringLiteral("Delivery tail did not run"));
            CHILD_CHECK(publishPhase("LATE_QUERY_DRAINED", QStringLiteral("A=%1 state=%2 B=%3")
                                        .arg(taskA).arg(int(manager.taskById(taskA).state)).arg(taskB)),
                        QStringLiteral("Phase pipe unavailable"));
            // Reach data guards before the expected stale-completion UI red.
            CHILD_CHECK(sourceGuards() && companionGuards(), QStringLiteral("Late A/B work changed source/identity/body"));
            CHILD_CHECK(pattern->text() == queryB && scope->currentData().toInt() == 0
                            && findAll->isEnabled() && pattern->isEnabled() && !cancelSearch->isVisible(),
                        QStringLiteral("Late A changed B query controls or left them busy"));
            if (acceptedB) {
                auto *model = qobject_cast<SearchResultsModel *>(tree->model());
                CHILD_CHECK(manager.taskById(taskB).state == AsyncTaskState::Finished
                                && model && !model->isIncomplete() && model->totalMatches() == 33
                                && summary->text().contains(queryB)
                                && !summary->text().contains(QString::fromLatin1(kQuery)),
                            QStringLiteral("Latest accepted B was replaced by an older query result"));
                for (int row = 0; row < 33; ++row) {
                    const QModelIndex hit = model->index(row, SearchResultsModel::COL_CONTEXT);
                    CHILD_CHECK(model->matchByteOffset(hit) == qint64(row) * 1024 * 1024 + kRerunOffset
                                    && model->matchLength(hit) == kRerunQuery.size(),
                                QStringLiteral("Latest B does not contain its exact source byte ranges"));
                }
            } else {
                CHILD_CHECK(status->text() == bStatus && status->toolTip() == bStatusTooltip
                                && summary->text() == bSummary && tree->model() == bModel.data()
                                && (!bModel || bModel->rowCount() == bRows),
                            QStringLiteral("Late A replaced B's visible rejection/validation error or result model"));
            }
            CHILD_CHECK(publishPhase("LATEST_QUERY_GUARDS_OK"), QStringLiteral("Phase pipe unavailable"));
            // Supplemental implementation-time ownership regression. Single-
            // file previews are bound to this same tree immediately. After
            // all query deliveries, an extra direct Dock-owned result has no
            // consumer; do not mistake it for an active aggregate child or an
            // externally owned model borrowed by the tree.
            const QPointer<QAbstractItemModel> displayedModel(tree->model());
            const bool hadDisplayedModel = !displayedModel.isNull();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            CHILD_CHECK((!hadDisplayedModel || displayedModel)
                            && tree->model() == displayedModel.data()
                            && sourceGuards() && companionGuards(),
                        QStringLiteral("Deferred result cleanup deleted the active model or changed source data"));
            const auto directModels = dock->findChildren<SearchResultsModel *>(QString(), Qt::FindDirectChildrenOnly);
            for (SearchResultsModel *ownedModel : directModels) {
                CHILD_CHECK(ownedModel == displayedModel.data(),
                            QStringLiteral("Dock retained an unconsumed old query result after DeferredDelete"));
            }
            // Inspect public QObject ownership by result type, never an arena
            // name/private field. This single-file journey has one consumer
            // and no outstanding query; a source-owned model is legal only
            // while it is the actual model borrowed by that consumer.
            const auto sourceModels = target->findChildren<SearchResultsModel *>();
            for (SearchResultsModel *sourceModel : sourceModels) {
                CHILD_CHECK(sourceModel == displayedModel.data(),
                            QStringLiteral("Source retained an unconsumed old query result after DeferredDelete"));
            }
            CHILD_CHECK(publishPhase("QUERY_MODELS_RECLAIMED", QStringLiteral("dock=%1 source=%2")
                                        .arg(directModels.size()).arg(sourceModels.size())),
                        QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
            QElapsedTimer closing;
            closing.start();
            closeAction->trigger();
            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull() && boundaryEngine.isNull()
                            && companionGuards() && fileHash(path) == expectedHash,
                        QStringLiteral("Latest-query source did not close safely"));
            CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(loads.drainAfterTargetDestruction() && companionGuards() && fileHash(path) == expectedHash,
                        QStringLiteral("Latest-query cleanup did not drain safely or changed a source"));
            CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
            return true;
        }

        if (stopCancelClosesSource || sourceCloseReenters) {
            QSignalSpy submitted(&manager, &AsyncTaskManager::taskAdded);
            CHILD_CHECK(clickPreparedQuery(kind == QStringLiteral("findAll") ? findAll : next),
                        QStringLiteral("Real query was not admitted after preparation"));
            const qint64 taskId = boundaryEngine->lastTaskId();
            int exactSubmissions = 0;
            for (const QList<QVariant> &arguments : submitted) {
                if (!arguments.isEmpty() && arguments.constFirst().toLongLong() == taskId)
                    ++exactSubmissions;
            }
            CHILD_CHECK(taskId > 0 && taskId != warmupId && exactSubmissions == 1,
                        QStringLiteral("Reentrant-stop target was not actually submitted once"));
            loads.track(taskId);
            QTest::qWait(50);
            const AsyncTask queued = manager.taskById(taskId);
            CHILD_CHECK(queued.id == taskId && queued.categoryTag == expectedCategory
                            && queued.title == expectedTitle && queued.priority == expectedPriority
                            && queued.state == AsyncTaskState::Queued && !queued.startedAt.isValid()
                            && queued.progressPhase.isEmpty() && queued.progressDone == 0
                            && loads.saturated(),
                        QStringLiteral("Reentrant-stop target was not demonstrably queued"));
            CHILD_CHECK(publishPhase("QUEUED", QString::number(taskId)), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(sourceGuards(), QStringLiteral("Source guards failed before arming reentrant close"));
            CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));
            bool outerStopOnStack = false;
            bool directClosePassed = false;
            int directCloseCount = 0;
            const auto onCanceled = QObject::connect(
                &manager, &AsyncTaskManager::taskFinished, &window,
                [&](qint64 id, AsyncTaskState state) {
                    if (id != taskId || state != AsyncTaskState::Canceled || directCloseCount != 0)
                        return;
                    ++directCloseCount;
                    directClosePassed = [&]() -> bool {
                        // Outer file.close may already have shut down part of
                        // the provider. Only stable target facts apply inside
                        // that close; the complete source guards ran above.
                        const bool callbackGuards = sourceCloseReenters
                            ? target && tabs->currentWidget() == target.data()
                                && target->documentState().id == before.id
                                && target->contentRevision() == before.revision
                                && fileHash(path) == expectedHash
                            : sourceGuards();
                        CHILD_CHECK(outerStopOnStack && loads.saturated() && callbackGuards
                                        && manager.taskById(id).id == taskId
                                        && manager.taskById(id).state == AsyncTaskState::Canceled,
                                    QStringLiteral("Close was not a guarded direct callback inside the outer stop"));
                        CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(id))
                                        && publishPhase("DIRECT_CLOSE_ENTER", QString::number(id)),
                                    QStringLiteral("Phase pipe unavailable"));
                        closeAction->trigger();
                        if (sourceCloseReenters) {
                            CHILD_CHECK(target && target->documentState().id == before.id
                                            && target->contentRevision() == before.revision
                                            && manager.taskById(id).state == AsyncTaskState::Canceled
                                            && loads.saturated() && fileHash(path) == expectedHash,
                                        QStringLiteral("Inner repeated file.close destroyed the outer close's target"));
                            CHILD_CHECK(publishPhase("DIRECT_CLOSE_RETURNED")
                                            && publishPhase("INNER_RETURNED_TARGET_ALIVE", before.id.toString()),
                                        QStringLiteral("Phase pipe unavailable"));
                        } else {
                            CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull()
                                            && boundaryEngine.isNull() && loads.saturated(),
                                        QStringLiteral("Direct canceled callback did not destroy the source"));
                            CHILD_CHECK(publishPhase("DIRECT_CLOSE_RETURNED") && publishPhase("TARGET_DESTROYED"),
                                        QStringLiteral("Phase pipe unavailable"));
                        }
                        return true;
                    }();
                }, Qt::DirectConnection);
            const auto disconnectCanceled = qScopeGuard([&]() { QObject::disconnect(onCanceled); });
            CHILD_CHECK(publishPhase(sourceCloseReenters ? "SOURCE_CLOSE_REENTRY_ARMED" : "REENTRANT_CLOSE_ARMED",
                                     QString::number(taskId))
                            && publishPhase("BEFORE_STOP", sourceCloseReenters
                                ? QStringLiteral("sourceClose") : QStringLiteral("cancelAndWait")),
                        QStringLiteral("Phase pipe unavailable"));
            // No preceding manager.cancel() or TaskPanel action: the tested
            // direct terminal is produced by stopWorkers' own cancellation.
            QElapsedTimer stopping;
            stopping.start();
            outerStopOnStack = true;
            if (sourceCloseReenters)
                closeAction->trigger();
            else
                boundaryEngine->cancelAndWait();
            outerStopOnStack = false;
            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(stopping.elapsed())), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(directCloseCount == 1 && directClosePassed && target.isNull()
                            && provider.isNull() && source.isNull() && boundaryEngine.isNull()
                            && loads.saturated() && fileHash(path) == expectedHash,
                        QStringLiteral("Outer stop did not return safely from source-destroying reentry"));
            if (sourceCloseReenters)
                CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
            CHILD_CHECK(loads.drainAfterTargetDestruction(), QStringLiteral("Reentrant-stop wrappers did not drain safely"));
            CHILD_CHECK(manager.taskById(taskId).state == AsyncTaskState::Canceled
                            && directCloseCount == 1 && fileHash(path) == expectedHash,
                        QStringLiteral("Late reentrant-stop callbacks or source mutation"));
            CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
            return true;
        }

        if (cancelAtAdded || closeAtAdded) {
            qint64 interceptedId = 0;
            int interceptedCount = 0;
            bool awaitingQueryAdmission = false;
            bool directPassed = false;
            const auto onAdded = QObject::connect(
                &manager, &AsyncTaskManager::taskAdded, &window,
                [&](qint64 id) {
                    const AsyncTask task = manager.taskById(id);
                    if (!awaitingQueryAdmission || interceptedCount != 0
                        || task.categoryTag != expectedCategory || task.title != expectedTitle
                        || pattern->text() != QString::fromLatin1(kQuery))
                        return;
                    ++interceptedCount; // Disarm before any nested signal.
                    interceptedId = id;
                    loads.track(id);
                    directPassed = [&]() -> bool {
                        CHILD_CHECK(task.id == id && id != warmupId && task.priority == expectedPriority
                                        && task.state == AsyncTaskState::Queued && !task.startedAt.isValid()
                                        && task.progressDone == 0 && task.progressPhase.isEmpty()
                                        && loads.saturated(),
                                    QStringLiteral("taskAdded boundary was not an unstarted exact query"));
                        CHILD_CHECK(publishPhase("QUEUED", QString::number(id))
                                        && publishPhase("TASK_ADDED_DIRECT", QString::number(id)),
                                    QStringLiteral("Phase pipe unavailable"));
                        CHILD_CHECK(sourceGuards(), QStringLiteral("Source guards failed inside taskAdded before action"));
                        CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));
                        if (cancelAtAdded) {
                            // The returned task ID has not reached the domain
                            // caller yet. Cancel the exact emitted Manager ID.
                            manager.cancel(id);
                            CHILD_CHECK(manager.taskById(id).id == id
                                            && manager.taskById(id).state == AsyncTaskState::Canceled,
                                        QStringLiteral("Immediate exact-ID cancellation did not settle Manager"));
                            CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(id)), QStringLiteral("Phase pipe unavailable"));
                        } else {
                            // Deliberately no preceding manager.cancel(): real
                            // close must handle a submit still on its stack.
                            CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("taskAddedClose")), QStringLiteral("Phase pipe unavailable"));
                            QElapsedTimer closing;
                            closing.start();
                            closeAction->trigger();
                            CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
                            CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull()
                                            && boundaryEngine.isNull() && loads.saturated(),
                                        QStringLiteral("Direct source close did not destroy the held target"));
                            CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
                        }
                        return true;
                    }();
                }, Qt::DirectConnection);
            const auto disconnectAdded = qScopeGuard([&]() { QObject::disconnect(onAdded); });
            awaitingQueryAdmission = true;
            CHILD_CHECK(clickPreparedQuery(kind == QStringLiteral("findAll") ? findAll : next),
                        QStringLiteral("Real query was not admitted after preparation"));
            awaitingQueryAdmission = false;
            CHILD_CHECK(interceptedCount == 1 && interceptedId > 0 && directPassed,
                        QStringLiteral("Exact taskAdded action was not completed once inside submit"));
            CHILD_CHECK(publishPhase("SUBMIT_RETURNED", QString::number(interceptedId)), QStringLiteral("Phase pipe unavailable"));
            if (cancelAtAdded) {
                auto *cancelSearch = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
                CHILD_CHECK(cancelSearch && waitUntil([&]() {
                    return pattern->isEnabled() && next->isEnabled() && findAll->isEnabled()
                        && !cancelSearch->isVisible();
                }, 1500), QStringLiteral("taskAdded cancellation did not settle real query controls"));
                CHILD_CHECK(loads.saturated() && sourceGuards(), QStringLiteral("Immediate cancel altered the held source"));
                CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("cancelAndWait")), QStringLiteral("Phase pipe unavailable"));
                QElapsedTimer stopping;
                stopping.start();
                boundaryEngine->cancelAndWait();
                CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(stopping.elapsed())), QStringLiteral("Phase pipe unavailable"));
                CHILD_CHECK(loads.saturated(), QStringLiteral("Cancel barrier released held workloads"));
                CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
                stopping.restart();
                closeAction->trigger();
                CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(stopping.elapsed())), QStringLiteral("Phase pipe unavailable"));
                CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull()
                                && boundaryEngine.isNull() && loads.saturated(),
                            QStringLiteral("Immediate-cancel target must die before held wrappers are released"));
                CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
            }
            CHILD_CHECK(loads.saturated() && fileHash(path) == expectedHash,
                        QStringLiteral("Submission return changed source bytes or released workloads"));
            CHILD_CHECK(loads.drainAfterTargetDestruction(), QStringLiteral("Immediate-action wrappers did not drain safely"));
            CHILD_CHECK(manager.taskById(interceptedId).id == interceptedId
                            && manager.taskById(interceptedId).state == AsyncTaskState::Canceled,
                        QStringLiteral("Late wrapper changed the immediate-action task state"));
            CHILD_CHECK(fileHash(path) == expectedHash, QStringLiteral("Source bytes changed after immediate-action drain"));
            CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
            if (closeAtAdded && kind == QStringLiteral("findOne")) {
                // Old navigation callbacks can be safely dropped on close,
                // but the dialog must not retain a permanently busy Next UI.
                const QString freshPath = QFileInfo(path).dir().filePath(QStringLiteral("after-close-navigation.txt"));
                const QByteArray freshBytes("prefix\nNEW_DOCUMENT_NEXT_463 is a real surviving match\n");
                const QByteArray freshQuery("NEW_DOCUMENT_NEXT_463");
                const qint64 expectedOffset = freshBytes.indexOf(freshQuery);
                {
                    QFile freshSource(freshPath);
                    CHILD_CHECK(freshSource.open(QIODevice::WriteOnly)
                                    && freshSource.write(freshBytes) == freshBytes.size(),
                                QStringLiteral("Cannot create ordinary post-close navigation source"));
                }
                const QByteArray freshHash = fileHash(freshPath);
                CHILD_CHECK(!freshHash.isEmpty(), QStringLiteral("Fresh source hash unavailable"));
                window.openFilesFromExternal({freshPath});
                QPointer<EditorTab> fresh = qobject_cast<EditorTab *>(tabs->currentWidget());
                CHILD_CHECK(fresh && !fresh->isL2Mode() && !fresh->isSecureDocument()
                                && !fresh->isModified() && fresh->filePath() == freshPath,
                            QStringLiteral("Post-close file did not open as an ordinary document"));
                const DocumentState freshState = fresh->documentState();
                const QString freshBody = fresh->editor()->text();
                CHILD_CHECK(freshBody == QString::fromUtf8(freshBytes)
                                && fileHash(freshPath) == freshHash && fileHash(path) == expectedHash,
                            QStringLiteral("Post-close source guards failed before UI expectation"));
                findAction->trigger();
                scope->setCurrentIndex(scope->findData(0));
                regex->setChecked(false);
                wholeWord->setChecked(false);
                hex->setChecked(false);
                pattern->setText(QString::fromLatin1(freshQuery));
                CHILD_CHECK(waitUntil([&]() { return next->isEnabled() && pattern->isEnabled(); }, 1500),
                            QStringLiteral("Closed target left navigation busy in the new document"));
                QSignalSpy navigationAdded(&manager, &AsyncTaskManager::taskAdded);
                QTest::mouseClick(next, Qt::LeftButton);
                // This tiny ordinary document uses the real inline L1 path;
                // a successful Next must not require an asynchronous task.
                CHILD_CHECK(fresh && next->isEnabled() && pattern->isEnabled()
                                && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == expectedOffset
                                && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND)
                                    == expectedOffset + freshQuery.size(),
                            QStringLiteral("Post-close inline Next did not select the exact new document byte range"));
                QTest::qWait(100);
                int navigationCount = 0;
                for (const QList<QVariant> &arguments : navigationAdded) {
                    if (arguments.isEmpty()) continue;
                    const qint64 id = arguments.constFirst().toLongLong();
                    const AsyncTask task = manager.taskById(id);
                    if (task.categoryTag == QStringLiteral("SearchNavigation")
                        && task.title == QStringLiteral("查找下一个: %1").arg(QFileInfo(freshPath).fileName())) {
                        ++navigationCount;
                        loads.track(id);
                    }
                }
                CHILD_CHECK(fresh && fresh->documentState().id == freshState.id
                                && fresh->contentRevision() == freshState.revision
                                && fresh->documentState().capabilities == freshState.capabilities
                                && fresh->editor()->text() == freshBody && !fresh->isModified()
                                && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART) == expectedOffset
                                && fresh->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND)
                                    == expectedOffset + freshQuery.size()
                                && fileHash(freshPath) == freshHash && fileHash(path) == expectedHash
                                && manager.taskById(interceptedId).state == AsyncTaskState::Canceled
                                && !QApplication::activeModalWidget(),
                            QStringLiteral("Post-close navigation changed source/identity or received stale results"));
                CHILD_CHECK(navigationCount == 0,
                            QStringLiteral("Short-document inline Next unexpectedly submitted an asynchronous navigation task"));
                CHILD_CHECK(publishPhase("FRESH_NAVIGATION_DONE", QStringLiteral("inline [%1,%2)")
                                    .arg(expectedOffset).arg(expectedOffset + freshQuery.size())),
                            QStringLiteral("Phase pipe unavailable"));
            }
            return true;
        }

        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        auto *tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
        auto *summary = dock ? dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary")) : nullptr;
        auto *cancelSearch = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        CHILD_CHECK(tree && summary && cancelSearch, QStringLiteral("Missing real rerun feedback controls"));
        int aTerminals = 0;
        int bTerminals = 0;
        bool insideATerminal = false;
        bool rerunCommandStarted = false;
        bool rerunCommandAccepted = false;
        QVector<qint64> aIds;
        QVector<qint64> bIds;
        QVector<qint64> bPreparationIds;
        const auto trackQueries = QObject::connect(
            &manager, &AsyncTaskManager::taskAdded, &window,
            [&](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if (insideATerminal && task.categoryTag == QStringLiteral("Mmap")
                    && task.title.contains(QFileInfo(path).fileName())) {
                    bPreparationIds.append(id);
                    return;
                }
                if (task.categoryTag != expectedCategory || task.title != expectedTitle)
                    return;
                if (rerunCommandStarted && pattern->text() == QString::fromLatin1(kRerunQuery))
                    bIds.append(id);
                else if (pattern->text() == QString::fromLatin1(kQuery))
                    aIds.append(id);
                else
                    return;
                loads.track(id);
            }, Qt::DirectConnection);
        const auto disconnectQueryTracking = qScopeGuard([&]() { QObject::disconnect(trackQueries); });
        const auto observeTerminal =
            [&](SearchResultsModel *, const QString &finishedPattern, const QString &) {
                if (finishedPattern == QString::fromLatin1(kRerunQuery)) {
                    ++bTerminals;
                    return;
                }
                if (finishedPattern != QString::fromLatin1(kQuery))
                    return;
                ++aTerminals;
                if (aTerminals != 1)
                    return;
                insideATerminal = true;
                const auto leaveTerminal = qScopeGuard([&]() { insideATerminal = false; });
                rerunCommandAccepted = [&]() -> bool {
                    CHILD_CHECK(loads.saturated() && sourceGuards()
                                    && pattern->isEnabled() && findAll->isEnabled()
                                    && !cancelSearch->isVisible(),
                                QStringLiteral("A terminal did not expose a safe, enabled real query UI"));
                    pattern->setText(QString::fromLatin1(kRerunQuery));
                    QSignalSpy bClicks(findAll, &QPushButton::clicked);
                    rerunCommandStarted = true;
                    QTest::mouseClick(findAll, Qt::LeftButton);
                    // A has released its mapping before this direct terminal.
                    // B's real command may first submit asynchronous Mmap; do
                    // not pump events inside A's terminal to force Search early.
                    CHILD_CHECK(bClicks.size() == 1 && aIds.size() == 1
                                    && bIds.size() + bPreparationIds.size() == 1
                                    && cancelSearch->isVisible() && !findAll->isEnabled() && !pattern->isEnabled(),
                                QStringLiteral("B command was not accepted with busy controls inside A's direct terminal slot"));
                    CHILD_CHECK(publishPhase("B_COMMAND_IN_TERMINAL",
                                             QStringLiteral("preparation=%1 search=%2")
                                                 .arg(bPreparationIds.value(0)).arg(bIds.value(0))),
                                QStringLiteral("Phase pipe unavailable"));
                    return true;
                }();
            };
        CHILD_CHECK(clickPreparedQuery(findAll), QStringLiteral("Initial A was not admitted after preparation"));
        CHILD_CHECK(aIds.size() == 1, QStringLiteral("Initial A query was not actually submitted once"));
        const qint64 aId = aIds.first();
        const AsyncTask queuedA = manager.taskById(aId);
        CHILD_CHECK(queuedA.id == aId && aId != warmupId && queuedA.state == AsyncTaskState::Queued
                        && !queuedA.startedAt.isValid() && loads.saturated(),
                    QStringLiteral("Initial A query is not queued behind the held workers"));
        CHILD_CHECK(publishPhase("QUEUED", QString::number(aId)), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(sourceGuards(), QStringLiteral("Source guards failed before A cancellation"));
        CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));
        // Observe after the real A submission has installed its receivers.
        // This does not require a permanent setActiveTab connection: the
        // production UI must settle A before this same direct terminal slot
        // clicks B, while the held pool still prevents either worker running.
        const auto onTerminal = QObject::connect(
            target.data(), &DocumentSearchSurface::findAllL2Finished, &window,
            observeTerminal, Qt::DirectConnection);
        const auto disconnectTerminal = qScopeGuard([&]() { QObject::disconnect(onTerminal); });
        CHILD_CHECK(cancelSearch->isVisible() && cancelSearch->isEnabled(), QStringLiteral("Real A Cancel control unavailable"));
        QTest::mouseClick(cancelSearch, Qt::LeftButton);
        CHILD_CHECK(manager.taskById(aId).state == AsyncTaskState::Canceled,
                    QStringLiteral("A cancellation did not settle Manager state"));
        CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(aId)), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(waitUntil([&]() { return aTerminals == 1; }, 1500) && rerunCommandAccepted
                        && sourceGuards() && bTerminals == 0
                        && !findAll->isEnabled() && !pattern->isEnabled() && cancelSearch->isVisible(),
                    QStringLiteral("A terminal did not accept B reentrantly or its tail cleared B busy"));
        CHILD_CHECK(waitUntil([&]() { return bIds.size() == 1 || bridgeFailed; }, 3000)
                        && !bridgeFailed && bIds.size() == 1 && bIds.first() != aId,
                    QStringLiteral("B was not actually admitted once after asynchronous preparation"));
        const qint64 bId = bIds.first();
        const AsyncTask queuedB = manager.taskById(bId);
        CHILD_CHECK(queuedB.id == bId && queuedB.categoryTag == expectedCategory
                        && queuedB.title == expectedTitle && queuedB.state == AsyncTaskState::Queued
                        && queuedB.priority == expectedPriority && !queuedB.startedAt.isValid()
                        && loads.saturated() && bPreparationIds.size() <= 1
                        && (bPreparationIds.isEmpty()
                            || manager.taskById(bPreparationIds.first()).state == AsyncTaskState::Finished),
                    QStringLiteral("Reentrant B lacks its exact queued Search after real preparation"));
        CHILD_CHECK(publishPhase("B_QUEUED_AFTER_PREPARATION", QString::number(bId)),
                    QStringLiteral("Phase pipe unavailable"));
        // Return all the way through the old A completion before releasing B.
        QTest::qWait(50);
        CHILD_CHECK(loads.saturated() && sourceGuards() && aTerminals == 1 && bTerminals == 0
                        && manager.taskById(bId).state == AsyncTaskState::Queued
                        && !findAll->isEnabled() && !pattern->isEnabled(),
                    QStringLiteral("Old A completion cleared or altered pending B"));
        loads.release();
        CHILD_CHECK(waitUntil([&]() {
            return bTerminals == 1 && manager.taskById(bId).state == AsyncTaskState::Finished
                && findAll->isEnabled() && pattern->isEnabled() && !cancelSearch->isVisible()
                && manager.queuedCount() == 0 && manager.runningCount() == 0
                && manager.activeThreadCount() == 0;
        }, 5000), QStringLiteral("Reentrant B did not finish through its real pending query"));
        QTest::qWait(100);
        auto *result = qobject_cast<SearchResultsModel *>(tree->model());
        CHILD_CHECK(sourceGuards() && aTerminals == 1 && bTerminals == 1
                        && aIds.size() == 1 && bIds.size() == 1
                        && manager.taskById(aId).state == AsyncTaskState::Canceled,
                    QStringLiteral("Rerun final document or exact-once terminal guards failed"));
        CHILD_CHECK(result && !result->isIncomplete() && result->totalMatches() == 33 && result->rowCount() == 33,
                    QStringLiteral("B final model is missing or incomplete"));
        for (int row = 0; row < 33; ++row) {
            const QModelIndex match = result->index(row, SearchResultsModel::COL_CONTEXT);
            CHILD_CHECK(result->matchByteOffset(match) == qint64(row) * 1024 * 1024 + kRerunOffset
                            && result->matchLength(match) == kRerunQuery.size(),
                        QStringLiteral("B result contains an old or incorrect byte range"));
        }
        CHILD_CHECK(pattern->text() == QString::fromLatin1(kRerunQuery)
                        && summary->text().contains(QString::fromLatin1(kRerunQuery))
                        && !summary->text().contains(QString::fromLatin1(kQuery)),
                    QStringLiteral("Old A query polluted the final B UI"));
        CHILD_CHECK(publishPhase("B_TERMINAL", QString::number(bId))
                        && publishPhase("RERUN_GUARDS_OK", before.id.toString()),
                    QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
        QElapsedTimer closing;
        closing.start();
        closeAction->trigger();
        CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closing.elapsed())), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull() && boundaryEngine.isNull(),
                    QStringLiteral("Completed reentrant query target did not close"));
        CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(loads.drainAfterTargetDestruction() && aTerminals == 1 && bTerminals == 1
                        && fileHash(path) == expectedHash,
                    QStringLiteral("Late rerun callbacks or source mutation after close"));
        CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
        return true;
    }

    pattern->setText(QString::fromLatin1(kQuery));
    QSignalSpy added(&manager, &AsyncTaskManager::taskAdded);
    CHILD_CHECK(clickPreparedQuery(kind == QStringLiteral("findAll") ? findAll : next),
                QStringLiteral("Real query was not admitted after preparation"));
    QPointer<LargeFileSearch> engine = kind == QStringLiteral("findAll")
        ? source->findAllEngine() : source->navigationEngine();
    CHILD_CHECK(engine, QStringLiteral("Real query did not create its engine"));
    const qint64 taskId = engine->lastTaskId();
    bool actualSubmission = false;
    for (const QList<QVariant> &arguments : added) {
        if (!arguments.isEmpty() && arguments.constFirst().toLongLong() == taskId)
            actualSubmission = true;
    }
    CHILD_CHECK(taskId > 0 && taskId != warmupId && actualSubmission,
                QStringLiteral("No new query task; cached/preparation result is not valid evidence"));
    loads.track(taskId);
    QTest::qWait(100);
    const AsyncTask queued = manager.taskById(taskId);
    const auto expectedPriority = kind == QStringLiteral("findAll")
        ? AsyncTaskPriority::High : AsyncTaskPriority::Highest;
    const QString expectedCategory = kind == QStringLiteral("findAll")
        ? QStringLiteral("Search") : QStringLiteral("SearchNavigation");
    CHILD_CHECK(loads.saturated() && queued.id == taskId
                    && queued.categoryTag == expectedCategory && queued.priority == expectedPriority
                    && queued.state == AsyncTaskState::Queued && !queued.startedAt.isValid()
                    && queued.progressPhase.isEmpty() && queued.progressDone == 0,
                QStringLiteral("Real query was not demonstrably queued behind held workers"));
    CHILD_CHECK(publishPhase("QUEUED", QStringLiteral("task=%1 category=%2 priority=%3")
                    .arg(taskId).arg(queued.categoryTag).arg(int(queued.priority))),
                QStringLiteral("Phase pipe unavailable"));

    auto *taskButton = window.findChild<QToolButton *>(QStringLiteral("StatusBarTaskWidget_btn"));
    CHILD_CHECK(taskButton && taskButton->isVisible(), QStringLiteral("Task-panel entry unavailable"));
    QTest::mouseClick(taskButton, Qt::LeftButton);
    auto *panel = window.findChild<AsyncTaskPanelDialog *>();
    CHILD_CHECK(panel && QTest::qWaitForWindowExposed(panel), QStringLiteral("Task panel not exposed"));
    auto *list = panel->findChild<QListView *>(QStringLiteral("backgroundTaskList"));
    auto *cancel = panel->findChild<QPushButton *>(QStringLiteral("cancelSelectedTaskButton"));
    const QModelIndex selected = indexForTask(list, taskId);
    CHILD_CHECK(list && cancel && selected.isValid(), QStringLiteral("Cannot select actual query task"));
    list->setCurrentIndex(selected);
    list->scrollTo(selected);
    CHILD_CHECK(cancel->isEnabled() && selected.data(AsyncTaskManager::TaskCancellableRole).toBool(),
                QStringLiteral("Queued query is not cancellable through its task panel"));
    QTest::mouseClick(cancel, Qt::LeftButton);
    CHILD_CHECK(waitUntil([&]() {
        return manager.taskById(taskId).state == AsyncTaskState::Canceled;
    }, 1000), QStringLiteral("Real task-panel cancellation did not settle Manager state"));
    CHILD_CHECK(publishPhase("MANAGER_CANCELED", QString::number(taskId)), QStringLiteral("Phase pipe unavailable"));

    CHILD_CHECK(target && provider && source && engine && loads.saturated()
                    && tabs->currentWidget() == target.data()
                    && target->windowedReadOnlyProvider() == provider.data()
                    && target->findChild<ReadOnlySearchSource *>() == source.data(),
                QStringLiteral("Cancellation changed the target/source identity"));
    const DocumentState after = target->documentState();
    CHILD_CHECK(after.id == before.id && after.revision == before.revision
                    && after.location == before.location && after.origin == before.origin
                    && after.persistence == before.persistence && after.contentKind == before.contentKind
                    && after.securityClass == before.securityClass && after.capabilities == before.capabilities
                    && after.dirty == before.dirty && !target->isModified()
                    && target->encoding() == encodingBefore && target->contentSizeBytes() == kSourceSize
                    && target->editor() == provider->editor() && target->editor()->isReadOnly()
                    && target->editor()->text() == projectionBefore
                    && target->editor()->isModified() == projectionModifiedBefore
                    && provider->selectionStartOffset() == selectionStartBefore
                    && provider->selectionEndOffset() == selectionEndBefore
                    && fileHash(path) == expectedHash,
                QStringLiteral("Source/hash/document-state guards failed before stop"));
    CHILD_CHECK(!QApplication::activeModalWidget(), QStringLiteral("Unexpected modal before stop"));
    CHILD_CHECK(publishPhase("GUARDS_OK", before.id.toString()), QStringLiteral("Phase pipe unavailable"));

    auto *closeAction = window.findChild<QAction *>(QStringLiteral("file.close"));
    CHILD_CHECK(closeAction && closeAction->isEnabled(), QStringLiteral("Real close action unavailable"));
    if (stopAction == QStringLiteral("cancelAndWait")) {
        CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("cancelAndWait")), QStringLiteral("Phase pipe unavailable"));
        QElapsedTimer stopTime;
        stopTime.start();
        engine->cancelAndWait(); // Deliberately may block; only parent owns the deadline.
        CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(stopTime.elapsed())), QStringLiteral("Phase pipe unavailable"));
        CHILD_CHECK(loads.saturated(), QStringLiteral("Barrier only returned after held work was released"));
    }
    CHILD_CHECK(publishPhase("BEFORE_STOP", QStringLiteral("sourceClose")), QStringLiteral("Phase pipe unavailable"));
    QElapsedTimer closeTime;
    closeTime.start();
    closeAction->trigger(); // Normal source-close path, not delete or a fake slot.
    CHILD_CHECK(publishPhase("STOP_RETURNED", QString::number(closeTime.elapsed())), QStringLiteral("Phase pipe unavailable"));
    CHILD_CHECK(target.isNull() && provider.isNull() && source.isNull() && engine.isNull()
                    && loads.saturated(),
                QStringLiteral("Source/engine must be destroyed while canceled wrappers remain queued"));
    CHILD_CHECK(publishPhase("TARGET_DESTROYED"), QStringLiteral("Phase pipe unavailable"));
    CHILD_CHECK(loads.drainAfterTargetDestruction(), QStringLiteral("Late canceled wrappers did not drain safely"));
    CHILD_CHECK(manager.taskById(taskId).state == AsyncTaskState::Canceled,
                QStringLiteral("Late wrapper changed the canceled task state"));
    CHILD_CHECK(fileHash(path) == expectedHash, QStringLiteral("Source bytes changed after close/drain"));
    CHILD_CHECK(publishPhase("DRAINED"), QStringLiteral("Phase pipe unavailable"));
    return true;
}

#undef CHILD_CHECK

struct ChildResult {
    bool stopped = false;
    bool sourceUnchanged = false;
    bool passed = false;
    QString failure;
    QByteArray transcript;
};

ChildResult superviseChild(const QString &kind, const QString &stopAction,
                           const QString &path, const QString &profile,
                           const QByteArray &expectedHash)
{
    ChildResult result;
    QProcess child;
    const auto cleanup = qScopeGuard([&]() {
        if (child.state() != QProcess::NotRunning) {
            child.kill();
            child.waitForFinished(kKillBudgetMs);
        }
    });
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NOTEPADSAFE_DATADIR"), profile);
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({QStringLiteral("--l2-queued-cancel-child"), kind, stopAction,
                        path, QString::fromLatin1(expectedHash.toHex())});
    child.setWorkingDirectory(QCoreApplication::applicationDirPath());
    child.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer total;
    total.start();
    child.start(QIODevice::ReadOnly);
    if (!child.waitForStarted(3000))
        result.failure = QStringLiteral("Child launch failed: %1").arg(child.errorString());

    QByteArray pending;
    QStringList phases;
    const bool addedClose = stopAction == QStringLiteral("taskAddedClose");
    const bool addedCancel = stopAction == QStringLiteral("taskAddedCancel");
    const bool terminalRerun = stopAction == QStringLiteral("terminalRerun");
    const bool hideReopen = stopAction == QStringLiteral("hideReopen");
    const bool sourceChangeReentry = stopAction == QStringLiteral("sourceChangeTerminalReentry");
    const bool reentrantClose = stopAction == QStringLiteral("stopCancelClosesSource");
    const bool sourceCloseReenters = stopAction == QStringLiteral("sourceCloseReenters");
    const bool switchBackQuery = stopAction == QStringLiteral("querySwitchBack")
        || stopAction == QStringLiteral("querySwitchBackInvalidRegex");
    const bool mappingPendingClose = stopAction == QStringLiteral("mappingPendingClose");
    const bool mappingPendingCancel = stopAction == QStringLiteral("mappingPendingCancel");
    const bool mappingPendingFailure = stopAction == QStringLiteral("mappingPendingFailure");
    const bool mappingPreparation = mappingPendingClose || mappingPendingCancel || mappingPendingFailure;
    const bool lateNavigation = stopAction == QStringLiteral("canceledNavigationKeepsNewQueryBusy");
    const bool dualLanes = stopAction == QStringLiteral("dualNavFirst")
        || stopAction == QStringLiteral("dualAllFirst");
    const QStringList expectedStops = reentrantClose
        ? QStringList{QStringLiteral("cancelAndWait")}
        : stopAction == QStringLiteral("cancelAndWait") || addedCancel
        ? QStringList{QStringLiteral("cancelAndWait"), QStringLiteral("sourceClose")}
        : QStringList{addedClose ? QStringLiteral("taskAddedClose") : QStringLiteral("sourceClose")};
    int stopsStarted = 0;
    int stopsReturned = 0;
    bool stopPending = false;
    QElapsedTimer stop;
    const auto readPhases = [&]() {
        const QByteArray output = child.readAll();
        result.transcript += output;
        if (result.transcript.size() > 256 * 1024)
            result.transcript.remove(0, result.transcript.size() - 256 * 1024);
        pending += output;
        qsizetype newline = -1;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);
            if (!line.startsWith(kPhasePrefix))
                continue;
            const QByteArray record = line.mid(kPhasePrefix.size());
            const qsizetype separator = record.indexOf('|');
            if (separator < 0) {
                result.failure = QStringLiteral("Malformed phase record");
                continue;
            }
            const QString phase = QString::fromUtf8(record.left(separator));
            const QString detail = QString::fromUtf8(record.mid(separator + 1));
            if (phase == QStringLiteral("FAIL")) {
                result.failure = QStringLiteral("Child assertion: %1").arg(detail);
            } else if (phase == QStringLiteral("BEFORE_STOP")) {
                if (!phases.contains(QStringLiteral("BOOT"))
                    || !phases.contains(QStringLiteral("READY"))
                    || !phases.contains(QStringLiteral("POOL_QUEUED"))
                    || !phases.contains(QStringLiteral("QUEUED"))
                    || !phases.contains(QStringLiteral("GUARDS_OK"))
                    // Direct close deliberately has no prior cancel. Its
                    // precise precondition is a guarded taskAdded stack.
                    || (!addedClose && !reentrantClose && !sourceCloseReenters && !switchBackQuery && !mappingPreparation && !dualLanes && !sourceChangeReentry
                        && !phases.contains(QStringLiteral("MANAGER_CANCELED")))
                    || (mappingPreparation && !phases.contains(QStringLiteral("MAPPING_PENDING")))
                    || (lateNavigation && (!phases.contains(QStringLiteral("NAV_B_QUEUED_IN_CANCEL"))
                        || !phases.contains(QStringLiteral("NAV_B_BUSY_AFTER_A"))
                        || !phases.contains(QStringLiteral("NAV_B_FINISHED"))))
                    || (reentrantClose && !phases.contains(QStringLiteral("REENTRANT_CLOSE_ARMED")))
                    || (sourceCloseReenters && !phases.contains(QStringLiteral("SOURCE_CLOSE_REENTRY_ARMED")))
                    || (switchBackQuery && (!phases.contains(QStringLiteral("QUERY_B_OBSERVED"))
                        || !phases.contains(QStringLiteral("LATE_QUERY_DRAINED"))
                        || !phases.contains(QStringLiteral("LATEST_QUERY_GUARDS_OK"))
                        || !phases.contains(QStringLiteral("QUERY_MODELS_RECLAIMED"))))
                    || ((addedClose || addedCancel) && !phases.contains(QStringLiteral("TASK_ADDED_DIRECT")))
                    || (addedCancel && !phases.contains(QStringLiteral("SUBMIT_RETURNED")))
                    || (terminalRerun && (!phases.contains(QStringLiteral("B_COMMAND_IN_TERMINAL"))
                        || !phases.contains(QStringLiteral("B_QUEUED_AFTER_PREPARATION"))
                        || !phases.contains(QStringLiteral("B_TERMINAL"))
                        || !phases.contains(QStringLiteral("RERUN_GUARDS_OK"))))
                    || (hideReopen && (!phases.contains(QStringLiteral("FIND_HIDDEN"))
                        || !phases.contains(QStringLiteral("FIND_REOPENED_SAME_QUERY"))
                        || !phases.contains(QStringLiteral("REOPEN_B_QUEUED"))
                        || !phases.contains(QStringLiteral("REOPEN_B_FINISHED"))))
                    || (sourceChangeReentry && (!phases.contains(QStringLiteral("SOURCE_REPLACED_AT_IDLE"))
                        || !phases.contains(QStringLiteral("NEW_VERSION_ACCEPTED"))
                        || !phases.contains(QStringLiteral("SOURCE_CHANGE_B_COMMAND"))
                        || !phases.contains(QStringLiteral("SOURCE_CHANGE_B_QUEUED"))
                        || !phases.contains(QStringLiteral("SOURCE_CHANGE_B_FINISHED"))))
                    || stopPending || stopsStarted >= expectedStops.size()
                    || detail != expectedStops.at(stopsStarted)) {
                    result.failure = QStringLiteral("Stop began without the required queue/cancel/guard evidence");
                } else {
                    ++stopsStarted;
                    stopPending = true;
                    stop.start();
                }
            } else if (phase == QStringLiteral("STOP_RETURNED")) {
                bool numeric = false;
                const qint64 elapsed = detail.toLongLong(&numeric);
                if (!stopPending || !numeric || elapsed < 0 || elapsed > kStopBudgetMs)
                    result.failure = QStringLiteral("Stop did not return within the 2-second contract: %1").arg(detail);
                stopPending = false;
                ++stopsReturned;
            }
            phases.append(phase);
        }
    };

    while (result.failure.isEmpty() && child.state() != QProcess::NotRunning) {
        child.waitForReadyRead(25);
        readPhases();
        if (stopPending && stop.elapsed() >= kStopBudgetMs) {
            result.failure = QStringLiteral("Product stop timeout after BEFORE_STOP (%1), not a successful close")
                .arg(expectedStops.value(stopsStarted - 1));
        } else if (total.elapsed() >= kChildBudgetMs) {
            result.failure = QStringLiteral("Child exceeded 20 seconds; last phase=%1")
                .arg(phases.isEmpty() ? QStringLiteral("none: stdout/launch not established") : phases.constLast());
        }
    }
    readPhases();
    if (child.state() != QProcess::NotRunning) {
        child.kill(); // This exact QProcess only; never enumerate or kill by executable name.
        child.waitForFinished(kKillBudgetMs);
        readPhases();
    }
    result.stopped = child.state() == QProcess::NotRunning;
    // Always inspect the original bytes, including after a timeout or crash.
    // The parent owns the fixture, so child termination cannot skip this guard.
    const QByteArray finalHash = fileHash(path);
    if (sourceChangeReentry) {
        const QByteArray replacementHash = QCryptographicHash::hash(
            sourceChangeReplacementBytes(), QCryptographicHash::Sha256);
        // Only this scenario intentionally replaces its fixture. After the
        // commit phase the exact new hash is mandatory, including after kill.
        // A pre-commit setup failure may leave the original bytes untouched;
        // missing required phases still prevents that from passing the test.
        result.sourceUnchanged = finalHash == replacementHash
            || (!phases.contains(QStringLiteral("SOURCE_REPLACED_AT_IDLE")) && finalHash == expectedHash);
    } else {
        result.sourceUnchanged = finalHash == expectedHash;
    }
    QStringList required = {QStringLiteral("BOOT"), QStringLiteral("READY"),
        QStringLiteral("POOL_QUEUED"), QStringLiteral("QUEUED"),
        QStringLiteral("GUARDS_OK"), QStringLiteral("TARGET_DESTROYED"),
        QStringLiteral("DRAINED"), QStringLiteral("DONE")};
    if (!addedClose && !switchBackQuery && !mappingPendingFailure && !dualLanes && !sourceChangeReentry)
        required << QStringLiteral("MANAGER_CANCELED");
    if (addedClose || addedCancel)
        required << QStringLiteral("TASK_ADDED_DIRECT") << QStringLiteral("SUBMIT_RETURNED");
    if (terminalRerun)
        required << QStringLiteral("B_COMMAND_IN_TERMINAL") << QStringLiteral("B_QUEUED_AFTER_PREPARATION")
                 << QStringLiteral("B_TERMINAL")
                 << QStringLiteral("RERUN_GUARDS_OK");
    if (hideReopen)
        required << QStringLiteral("FIND_HIDDEN") << QStringLiteral("FIND_REOPENED_SAME_QUERY")
                 << QStringLiteral("REOPEN_B_QUEUED") << QStringLiteral("REOPEN_B_FINISHED");
    if (sourceChangeReentry)
        required << QStringLiteral("SOURCE_REPLACED_AT_IDLE") << QStringLiteral("NEW_VERSION_ACCEPTED")
                 << QStringLiteral("SOURCE_CHANGE_B_COMMAND") << QStringLiteral("SOURCE_CHANGE_B_QUEUED")
                 << QStringLiteral("SOURCE_CHANGE_B_FINISHED");
    if (reentrantClose)
        required << QStringLiteral("REENTRANT_CLOSE_ARMED") << QStringLiteral("DIRECT_CLOSE_ENTER")
                 << QStringLiteral("DIRECT_CLOSE_RETURNED");
    if (sourceCloseReenters)
        required << QStringLiteral("SOURCE_CLOSE_REENTRY_ARMED") << QStringLiteral("DIRECT_CLOSE_ENTER")
                 << QStringLiteral("DIRECT_CLOSE_RETURNED") << QStringLiteral("INNER_RETURNED_TARGET_ALIVE");
    if (switchBackQuery)
        required << QStringLiteral("QUERY_B_OBSERVED") << QStringLiteral("LATE_QUERY_DRAINED")
                 << QStringLiteral("LATEST_QUERY_GUARDS_OK") << QStringLiteral("QUERY_MODELS_RECLAIMED");
    if (dualLanes) required << QStringLiteral("DUAL_SUBMITTED")
        << QStringLiteral("FIRST_FINISHED_OTHER_QUEUED") << QStringLiteral("BOTH_FINISHED_SOURCE_IDLE");
    if (mappingPendingFailure) required << QStringLiteral("MAPPING_FAILED");
    if (mappingPendingCancel || mappingPendingFailure) required << QStringLiteral("MAPPING_RETRY_FINISHED");
    if (mappingPreparation)
        required << QStringLiteral("MAPPING_PENDING") << QStringLiteral("FRESH_NAVIGATION_DONE");
    if (lateNavigation)
        required << QStringLiteral("NAV_CANCELED_DIRECT") << QStringLiteral("NAV_B_QUEUED_IN_CANCEL")
                 << QStringLiteral("NAV_B_BUSY_AFTER_A") << QStringLiteral("NAV_B_FINISHED");
    if (addedClose && kind == QStringLiteral("findOne"))
        required << QStringLiteral("FRESH_NAVIGATION_DONE");
    bool complete = true;
    for (const QString &phase : required)
        complete = complete && phases.contains(phase);
    result.passed = result.failure.isEmpty() && result.stopped && result.sourceUnchanged
        && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0
        && complete && !stopPending && stopsReturned == expectedStops.size();
    if (!result.passed && result.failure.isEmpty()) {
        result.failure = QStringLiteral("Child failed/crashed/incomplete: exit=%1 status=%2 phases=%3")
            .arg(child.exitCode()).arg(int(child.exitStatus())).arg(phases.join(QLatin1Char(',')));
    }
    return result;
}

} // namespace

class SmokeL2QueuedCancelProcess : public QObject {
    Q_OBJECT

private:
    void runScenario(const QString &kind, const QString &stopAction)
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("child-profile"));
        QVERIFY(QDir().mkpath(profile));
        const QString path = temporary.filePath(QStringLiteral("queued-source.txt"));
        QByteArray chunk(1024 * 1024, 'a');
        for (qsizetype offset = 127; offset < chunk.size(); offset += 128)
            chunk[offset] = '\n';
        chunk.replace(kWarmupOffset, kWarmup.size(), kWarmup);
        chunk.replace(4096, kQuery.size(), kQuery);
        if (stopAction == QStringLiteral("terminalRerun")
            || stopAction == QStringLiteral("hideReopen")
            || stopAction == QStringLiteral("querySwitchBack")
            || stopAction == QStringLiteral("querySwitchBackInvalidRegex"))
            chunk.replace(kRerunOffset, kRerunQuery.size(), kRerunQuery);
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            for (int part = 0; part < 33; ++part)
                QCOMPARE(source.write(chunk), qint64(chunk.size()));
            QCOMPARE(source.size(), kSourceSize);
        }
        const QByteArray expectedHash = fileHash(path);
        QVERIFY(!expectedHash.isEmpty());
        // These navigation sources belong to the parent, so a crash/kill of
        // the child cannot skip either source's byte-integrity assertion.
        QString secondPath;
        QByteArray secondHash;
        if (stopAction.startsWith(QStringLiteral("mappingPending"))
            || stopAction == QStringLiteral("canceledNavigationKeepsNewQueryBusy")) {
            secondPath = temporary.filePath(QStringLiteral("navigation-second.txt"));
            QByteArray secondBytes;
            if (stopAction.startsWith(QStringLiteral("mappingPending"))) {
                secondBytes = QByteArray("prefix\n") + kNavigationQuery + " is a small inline navigation hit\n";
            } else {
                // >256KiB uses the existing real L1 async navigation path;
                // no second L2 mapping or product threshold override is needed.
                secondBytes = QByteArray(512 * 1024, 'b');
                for (qsizetype offset = 127; offset < secondBytes.size(); offset += 128)
                    secondBytes[offset] = '\n';
                secondBytes.replace(kRerunOffset, kNavigationQuery.size(), kNavigationQuery);
            }
            {
                QFile secondSource(secondPath);
                QVERIFY(secondSource.open(QIODevice::WriteOnly));
                QCOMPARE(secondSource.write(secondBytes), qint64(secondBytes.size()));
            }
            secondHash = fileHash(secondPath);
            QVERIFY(!secondHash.isEmpty());
        }
        const ChildResult result = superviseChild(kind, stopAction, path, profile, expectedHash);
        const bool secondSourceUnchanged = secondPath.isEmpty() || fileHash(secondPath) == secondHash;
        qInfo().noquote() << QString::fromUtf8(result.transcript);
        qInfo().noquote() << "L2 queued cancellation parent:" << kind << stopAction
            << "childStopped=" << result.stopped
            << "sourceHashPreserved=" << result.sourceUnchanged
            << "secondSourceHashPreserved=" << secondSourceUnchanged;
        if (!result.stopped)
            temporary.setAutoRemove(false); // Preserve files if termination was not confirmed.
        // These assertions deliberately precede the expected product timeout.
        QVERIFY2(result.sourceUnchanged, "Parent source hash changed, including after child kill");
        QVERIFY2(secondSourceUnchanged, "Parent second-source hash changed, including after child kill");
        QVERIFY2(result.stopped, qPrintable(QStringLiteral("Child termination not confirmed; retained %1")
                                             .arg(temporary.path())));
        QVERIFY2(result.passed, qPrintable(result.failure));
    }

private slots:
    void dualLaneSourceOwnership_data()
    {
        QTest::addColumn<QString>("stopAction");
        QTest::newRow("navigationFirst") << QStringLiteral("dualNavFirst");
        QTest::newRow("findAllFirst") << QStringLiteral("dualAllFirst");
    }

    void dualLaneSourceOwnership()
    {
        QFETCH(QString, stopAction);
        runScenario(QStringLiteral("findAll"), stopAction);
    }

    void queuedFindAllCancellation_data()
    {
        QTest::addColumn<QString>("stopAction");
        QTest::newRow("cancelAndWait") << QStringLiteral("cancelAndWait");
        QTest::newRow("sourceClose") << QStringLiteral("sourceClose");
        QTest::newRow("taskAddedCancel") << QStringLiteral("taskAddedCancel");
        QTest::newRow("taskAddedClose") << QStringLiteral("taskAddedClose");
        QTest::newRow("terminalRerun") << QStringLiteral("terminalRerun");
        QTest::newRow("stopCancelClosesSource") << QStringLiteral("stopCancelClosesSource");
        QTest::newRow("sourceCloseReenters") << QStringLiteral("sourceCloseReenters");
        QTest::newRow("querySwitchBack") << QStringLiteral("querySwitchBack");
        QTest::newRow("querySwitchBackInvalidRegex") << QStringLiteral("querySwitchBackInvalidRegex");
        QTest::newRow("hideReopen") << QStringLiteral("hideReopen");
        QTest::newRow("sourceChangeTerminalReentry") << QStringLiteral("sourceChangeTerminalReentry");
    }

    void queuedFindAllCancellation()
    {
        QFETCH(QString, stopAction);
        runScenario(QStringLiteral("findAll"), stopAction);
    }

    void queuedFindOneCancellation_data()
    {
        QTest::addColumn<QString>("stopAction");
        QTest::newRow("cancelAndWait") << QStringLiteral("cancelAndWait");
        QTest::newRow("sourceClose") << QStringLiteral("sourceClose");
        QTest::newRow("taskAddedCancel") << QStringLiteral("taskAddedCancel");
        QTest::newRow("taskAddedClose") << QStringLiteral("taskAddedClose");
        QTest::newRow("stopCancelClosesSource") << QStringLiteral("stopCancelClosesSource");
        QTest::newRow("sourceCloseReenters") << QStringLiteral("sourceCloseReenters");
        QTest::newRow("mappingPendingClose") << QStringLiteral("mappingPendingClose");
        QTest::newRow("mappingPendingCancel") << QStringLiteral("mappingPendingCancel");
        QTest::newRow("mappingPendingFailure") << QStringLiteral("mappingPendingFailure");
        QTest::newRow("canceledNavigationKeepsNewQueryBusy") << QStringLiteral("canceledNavigationKeepsNewQueryBusy");
    }

    void queuedFindOneCancellation()
    {
        QFETCH(QString, stopAction);
        runScenario(QStringLiteral("findOne"), stopAction);
    }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1 && arguments.at(1) == QStringLiteral("--l2-queued-cancel-child")) {
        if (arguments.size() != 6 || !publishPhase("BOOT", QStringLiteral("Qt GUI stdout pipe ready")))
            return 72;
        const bool passed = runChild(arguments.at(2), arguments.at(3), arguments.at(4),
                                     QByteArray::fromHex(arguments.at(5).toLatin1()));
        if (!passed)
            return 1;
        return publishPhase("DONE") ? 0 : 73;
    }
    SmokeL2QueuedCancelProcess test;
    return QTest::qExec(&test, argc, argv);
}

#include "smoke_l2_queued_cancel_process.moc"
