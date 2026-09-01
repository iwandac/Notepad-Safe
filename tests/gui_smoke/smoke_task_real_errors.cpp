#include <QtCore/QCoreApplication>
#include "RegisteredReadFixture.h"
#include <QtTest/QSignalSpy>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QAccessible>
#include <QtGui/QAccessibleInterface>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>

#include <Qsci/qsciscintilla.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "WindowedReadOnlyProvider.h"

// Supplementary 2B3 production integration acceptance. The first case was
// prepared after the original task-language red tests, not as pre-fix evidence.
// No injected errors, worker replacement, pool configuration, or product hooks.
namespace {

class OwnedReadErrorTasks final {
public:
    explicit OwnedReadErrorTasks(QObject *context)
    {
        added_ = QObject::connect(
            &manager_, &AsyncTaskManager::taskAdded, context, [this](qint64 id) {
                const AsyncTask task = manager_.taskById(id);
                // taskAdded is synchronous with the one provider.openFile()
                // call below. Do not claim other providers' background work.
                if (collectProviderTasks_
                    && task.categoryTag == QStringLiteral("LineNumberIndex")) {
                    providerIds_.append(id);
                    ownedIds_.append(id);
                }
            });
    }

    ~OwnedReadErrorTasks()
    {
        QObject::disconnect(added_);
        releaseLoads();
        for (qint64 id : std::as_const(ownedIds_))
            manager_.cancel(id);
        if (!waitUntilSettled(3000))
            qWarning("real task error cleanup exceeded its owned-task deadline");
    }

    bool occupyGeneralPool()
    {
        // The production pool reserves one slot for security. Fill only the
        // ordinary slots with cancellable, releasable production AsyncTasks.
        const int count = manager_.maximumThreadCount() - 1;
        if (count < 1)
            return false;
        for (int index = 0; index < count; ++index) {
            const auto state = loads_;
            const qint64 id = manager_.submit(
                AsyncTaskType::Other, AsyncTaskPriority::High,
                QStringLiteral("真实读取错误验收负载 %1").arg(index), QString(),
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
                }, 0, QStringLiteral("TaskRealErrorTestLoad"));
            if (id <= 0)
                return false;
            loadIds_.append(id);
            ownedIds_.append(id);
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

    void collectProviderTasks(bool collect) { collectProviderTasks_ = collect; }
    const QVector<qint64> &providerIds() const { return providerIds_; }

    bool waitUntilSettled(int timeoutMs) const
    {
        QElapsedTimer deadline;
        deadline.start();
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            bool settled = loads_->entered.load(std::memory_order_acquire)
                == loads_->exited.load(std::memory_order_acquire);
            for (qint64 id : ownedIds_) {
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

private:
    struct LoadState {
        std::atomic_bool released{false};
        std::atomic_int entered{0};
        std::atomic_int exited{0};
    };
    AsyncTaskManager &manager_ = AsyncTaskManager::instance();
    std::shared_ptr<LoadState> loads_ = std::make_shared<LoadState>();
    QVector<qint64> loadIds_;
    QVector<qint64> ownedIds_;
    QVector<qint64> providerIds_;
    QMetaObject::Connection added_;
    bool collectProviderTasks_ = false;
};

#ifdef Q_OS_WIN
class ExclusiveReadLock final {
public:
    ~ExclusiveReadLock() { close(); }

    bool open(const QString &path, QString *error)
    {
        const QString native = QDir::toNativeSeparators(path);
        handle_ = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE)
            return true;
        if (error)
            *error = QStringLiteral("Exclusive CreateFileW failed: %1")
                .arg(::GetLastError());
        return false;
    }

    bool close()
    {
        if (handle_ == INVALID_HANDLE_VALUE)
            return true;
        if (!::CloseHandle(handle_))
            return false;
        handle_ = INVALID_HANDLE_VALUE;
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

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

QString accessibleText(QAccessibleInterface *accessible)
{
    if (!accessible || !accessible->isValid())
        return {};
    return accessible->text(QAccessible::Name) + QLatin1Char('\n')
        + accessible->text(QAccessible::Description) + QLatin1Char('\n')
        + accessible->text(QAccessible::Value);
}

QString visibleAccessibleText(QAccessibleInterface *accessible, int depth = 0)
{
    if (!accessible || !accessible->isValid() || depth > 12
        || accessible->state().invisible || accessible->state().offscreen)
        return {};
    QString out = accessibleText(accessible);
    for (int child = 0; child < qMin(accessible->childCount(), 256); ++child)
        out += QLatin1Char('\n') + visibleAccessibleText(accessible->child(child), depth + 1);
    return out;
}

} // namespace

class SmokeTaskRealErrors : public QObject {
    Q_OBJECT

private slots:
    void tc_2b3_real_line_statistics_open_error_reaches_task_panel()
    {
#ifndef Q_OS_WIN
        QSKIP("The real no-sharing source lock is a Windows fixture.");
#else
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
        const bool previousAccessible = QAccessible::isActive();
        QAccessible::setActive(true);
        const auto restoreAccessibility = qScopeGuard([&]() {
            QAccessible::setActive(previousAccessible);
        });

        const QString path = temporary.filePath(QStringLiteral("real-statistics-read-error.txt"));
        const QByteArray original("Visible source survives a statistics failure.\nSecond physical line.\n");
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

        WindowedReadOnlyProvider provider;
        OwnedReadErrorTasks tasks(&provider);
        // Any early QVERIFY return closes the exclusive handle first, then
        // invalidates provider callbacks, releases/cancels/drains owned tasks
        // while the provider is alive, and only then destroys the fixture.
        const auto closeProvider = qScopeGuard([&]() { provider.closeFile(); });
        ExclusiveReadLock exclusive;
        QVERIFY(tasks.occupyGeneralPool());
        QTRY_VERIFY_WITH_TIMEOUT(tasks.allLoadsRunning(), 3000);

        QString openError;
        tasks.collectProviderTasks(true);
        const bool opened = provider.openFile(
            path, WindowedReadOnlyProvider::Presentation::Text,
            registeredReadFixturePolicy(), &openError);
        tasks.collectProviderTasks(false);
        QVERIFY2(opened, qPrintable(openError));
        QCOMPARE(tasks.providerIds().size(), qsizetype(1));
        const qint64 taskId = tasks.providerIds().constFirst();
        auto &manager = AsyncTaskManager::instance();
        const AsyncTask queued = manager.taskById(taskId);
        QCOMPARE(queued.state, AsyncTaskState::Queued);
        QCOMPARE(queued.type, AsyncTaskType::LineIndexBuild);
        QCOMPARE(queued.categoryTag, QStringLiteral("LineNumberIndex"));
        QVERIFY(provider.editor());
        QVERIFY(provider.editor()->isReadOnly());
        QCOMPARE(provider.fileSize(), qint64(original.size()));
        QCOMPARE(provider.presentation(), WindowedReadOnlyProvider::Presentation::Text);
        QVERIFY(provider.characterCountPending());
        const QString projectionBefore = provider.editor()->text();
        QVERIFY(projectionBefore.contains(QStringLiteral("Visible source survives")));
        const bool modifiedBefore = provider.editor()->isModified();
        QCOMPARE(provider.sourceBytes(0, original.size()), original);
        QSignalSpy invalidations(&provider, &WindowedReadOnlyProvider::sourceFileChanged);
        QSignalSpy sourceFailures(&provider, &WindowedReadOnlyProvider::sourceReloadFailed);

        // Initial projection reads have closed their source handles. Holding
        // GENERIC_READ/share=0 now makes the actual statistics worker's next
        // openSharedReadOnlyFile fail, without writing or replacing the file.
        QString lockError;
        QVERIFY2(exclusive.open(path, &lockError), qPrintable(lockError));
        QCOMPARE(manager.taskById(taskId).state, AsyncTaskState::Queued);
        tasks.releaseLoads();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskId).state,
                                  AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!provider.characterCountPending(), 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        const AsyncTask failed = manager.taskById(taskId);
        QCOMPARE(failed.id, taskId);
        QVERIFY(!failed.error.message.isEmpty());
        // This is the actual SharedReadOnlyFile OS diagnostic, not an injected
        // reportError call or an English-substring classifier in the product.
        const QString sharingFailure = QStringLiteral("CreateFile failed with Windows error %1")
            .arg(qulonglong(ERROR_SHARING_VIOLATION));
        QVERIFY2(failed.error.message.contains(sharingFailure), qPrintable(failed.error.message));

        QVERIFY(exclusive.close());
        QCOMPARE(invalidations.size(), 1);
        QCOMPARE(sourceFailures.size(), 1);
        QCOMPARE(sourceHash(), hashBefore);
        QCOMPARE(provider.sourceBytes(0, original.size()), original);
        QVERIFY(provider.editor()->isReadOnly());
        QCOMPARE(provider.fileSize(), qint64(original.size()));
        QCOMPARE(provider.presentation(), WindowedReadOnlyProvider::Presentation::Text);
        QCOMPARE(provider.editor()->text(), projectionBefore);
        QCOMPARE(provider.editor()->isModified(), modifiedBefore);
        QCOMPARE(provider.characterCount(), qint64(-1));
        QVERIFY(!provider.characterCountPending());
        QCOMPARE(tasks.providerIds().size(), qsizetype(1));

        const QString userMessage = failed.error.userMessage.trimmed();
        QVERIFY2(!userMessage.isEmpty(), qPrintable(failed.error.message));
        QVERIFY(userMessage != QStringLiteral("此任务未完成，请查看技术详情后重试。"));
        QVERIFY2(userMessage.contains(QStringLiteral("读取"))
                     && userMessage.contains(QStringLiteral("旧结果已失效")), qPrintable(userMessage));
        QVERIFY2(userMessage.contains(QStringLiteral("检查"))
                     && userMessage.contains(QStringLiteral("重新打开")), qPrintable(userMessage));
        QVERIFY(!userMessage.contains(QStringLiteral("CreateFile")));
        QVERIFY(!userMessage.contains(QStringLiteral("Windows error")));

        AsyncTaskPanelDialog panel;
        panel.show();
        panel.raise();
        panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        auto *summary = panel.findChild<QLabel *>(QStringLiteral("backgroundTaskSummary"));
        auto *toggle = panel.findChild<QAbstractButton *>(QStringLiteral("backgroundTaskDetailsToggle"));
        auto *technical = panel.findChild<QWidget *>(QStringLiteral("backgroundTaskTechnicalDetails"));
        auto *details = panel.findChild<QPlainTextEdit *>(QStringLiteral("backgroundTaskDetails"));
        QVERIFY(list && summary && toggle && technical && details);
        const QModelIndex failedIndex = indexForTask(list, taskId);
        QVERIFY(failedIndex.isValid());
        list->setCurrentIndex(failedIndex);
        list->scrollTo(failedIndex);
        QTRY_VERIFY_WITH_TIMEOUT(summary->isVisible() && summary->text().contains(userMessage), 1000);
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QVERIFY(!technical->isVisible() && !details->isVisible());
        QVERIFY(!summary->text().contains(failed.error.message));
        const QString publicRole = failedIndex.data(Qt::AccessibleTextRole).toString();
        QVERIFY(publicRole.contains(userMessage));
        QVERIFY(!publicRole.contains(sharingFailure));
        QCOMPARE(failedIndex.data(AsyncTaskManager::TaskErrorRole).toString(),
                 failed.error.toString());
        const QString spoken = visibleAccessibleText(QAccessible::queryAccessibleInterface(&panel));
        QVERIFY2(spoken.contains(userMessage), qPrintable(spoken));
        QVERIFY2(!spoken.contains(sharingFailure), qPrintable(spoken));

        list->setFocus();
        for (int step = 0; step < 32 && QApplication::focusWidget() != toggle; ++step)
            QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &panel,
                            Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), toggle);
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(technical->isVisible() && details->isVisible(), 1000);
        QVERIFY(details->isReadOnly());
        QVERIFY(details->toPlainText().contains(failed.error.toString()));
        QCOMPARE(manager.taskById(taskId).error.toString(), failed.error.toString());
        QCOMPARE(manager.taskById(taskId).error.userMessage, failed.error.userMessage);
        QCOMPARE(list->currentIndex().data(AsyncTaskManager::TaskIdRole).toLongLong(), taskId);
        QCOMPARE(sourceHash(), hashBefore);
        QVERIFY(provider.editor()->isReadOnly());
        QCOMPARE(provider.fileSize(), qint64(original.size()));
        QCOMPARE(provider.editor()->text(), projectionBefore);
        QCOMPARE(provider.editor()->isModified(), modifiedBefore);
#endif
    }

    void tc_2b3_real_statistics_failure_stops_character_status()
    {
#ifndef Q_OS_WIN
        QSKIP("The real no-sharing source lock is a Windows fixture.");
#else
        // A new regression case for the MainWindow character status, separate
        // from the already-prepared task-panel production integration above.
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
        const bool previousAccessible = QAccessible::isActive();
        QAccessible::setActive(true);
        const auto restoreAccessibility = qScopeGuard([&]() {
            QAccessible::setActive(previousAccessible);
        });

        const QString path = temporary.filePath(QStringLiteral("ordinary-statistics-source.txt"));
        constexpr qint64 sourceSize = 33LL * 1024 * 1024;
        QByteArray chunk(1024 * 1024, 'a');
        for (qsizetype offset = 127; offset < chunk.size(); offset += 128)
            chunk[offset] = '\n';
        const QByteArray marker("Ordinary text remains available after statistics stop.");
        chunk.replace(0, marker.size(), marker);
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            // Write actual text. A sparse/zero-filled 33 MiB fixture would take
            // the binary route and could not exercise this character status.
            for (int part = 0; part < 33; ++part)
                QCOMPARE(source.write(chunk), qint64(chunk.size()));
            QCOMPARE(source.size(), sourceSize);
        }
        const auto sourceHash = [&]() {
            QFile source(path);
            if (!source.open(QIODevice::ReadOnly))
                return QByteArray{};
            QCryptographicHash hash(QCryptographicHash::Sha256);
            return hash.addData(&source) ? hash.result() : QByteArray{};
        };
        const QByteArray hashBefore = sourceHash();
        QVERIFY(!hashBefore.isEmpty());

        MainWindow window;
        window.resize(1400, 900);
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
        QPointer<WindowedReadOnlyProvider> provider;
        OwnedReadErrorTasks tasks(&window);
        const auto closeProvider = qScopeGuard([&]() {
            if (provider)
                provider->closeFile();
        });
        // Reverse destruction: close exclusive handle, invalidate provider
        // callbacks, release/cancel/drain owned tasks, then remove tabs/window.
        ExclusiveReadLock exclusive;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(tasks.occupyGeneralPool());
        QTRY_VERIFY_WITH_TIMEOUT(tasks.allLoadsRunning(), 3000);
        tasks.collectProviderTasks(true);
        window.openFilesFromExternal({path});
        tasks.collectProviderTasks(false);

        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target);
        provider = target->windowedReadOnlyProvider();
        QVERIFY(provider && provider->editor());
        QCOMPARE(target->editor(), provider->editor());
        QCOMPARE(target->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        QVERIFY(target->isReadOnlyByPolicy() && !target->isBinaryDocument());
        QVERIFY(!target->isHexPresentation() && !target->isSecureDocument());
        QCOMPARE(tasks.providerIds().size(), qsizetype(1));
        const qint64 taskId = tasks.providerIds().constFirst();
        auto &manager = AsyncTaskManager::instance();
        QCOMPARE(manager.taskById(taskId).state, AsyncTaskState::Queued);
        QVERIFY(provider->characterCountPending());
        QCOMPARE(target->contentSizeBytes(), sourceSize);
        QCOMPARE(target->contentCharacterCount(), qint64(-1));
        QVERIFY(provider->editor()->isReadOnly());
        const DocumentState stateBefore = target->documentState();
        QVERIFY(!stateBefore.id.isNull());
        QVERIFY(!stateBefore.dirty && !target->isModified());
        QVERIFY(!stateBefore.capabilities.testFlag(DocumentCapability::Edit));
        const QString projectionBefore = provider->editor()->text();
        QVERIFY(projectionBefore.contains(QString::fromUtf8(marker)));
        const bool projectionModifiedBefore = provider->editor()->isModified();
        const auto encodingBefore = target->encoding();
        const QString pathBefore = target->filePath();
        auto *count = window.findChild<QLabel *>(QStringLiteral("status.characters"));
        QVERIFY(count);
        QTRY_VERIFY_WITH_TIMEOUT(count->isVisible()
                                     && count->text().contains(QStringLiteral("统计中")), 1000);
        QSignalSpy invalidations(provider, &WindowedReadOnlyProvider::sourceFileChanged);
        QSignalSpy sourceFailures(provider, &WindowedReadOnlyProvider::sourceReloadFailed);

        QString lockError;
        QVERIFY2(exclusive.open(path, &lockError), qPrintable(lockError));
        QCOMPARE(manager.taskById(taskId).state, AsyncTaskState::Queued);
        tasks.releaseLoads();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskId).state,
                                  AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(provider && !provider->characterCountPending(), 3000);
        QVERIFY(tasks.waitUntilSettled(3000));
        const AsyncTask failed = manager.taskById(taskId);
        const QString sharingFailure = QStringLiteral("CreateFile failed with Windows error %1")
            .arg(qulonglong(ERROR_SHARING_VIOLATION));
        QVERIFY2(failed.error.message.contains(sharingFailure), qPrintable(failed.error.message));
        QVERIFY(exclusive.close());

        // A failed ordinary-source admission invalidates old result identities
        // exactly once, without changing the actual document or its bytes.
        QCOMPARE(sourceHash(), hashBefore);
        QVERIFY(target && provider);
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(target.data()));
        QCOMPARE(target->windowedReadOnlyProvider(), provider.data());
        QCOMPARE(target->documentState().id, stateBefore.id);
        QCOMPARE(target->contentRevision(), stateBefore.revision + 1);
        QCOMPARE(invalidations.size(), 1);
        QCOMPARE(sourceFailures.size(), 1);
        QCOMPARE(target->documentState().location, stateBefore.location);
        QCOMPARE(target->documentState().origin, stateBefore.origin);
        QCOMPARE(target->documentState().persistence, stateBefore.persistence);
        QCOMPARE(target->documentState().securityClass, stateBefore.securityClass);
        QCOMPARE(target->documentState().contentKind, stateBefore.contentKind);
        QCOMPARE(target->documentState().capabilities, stateBefore.capabilities);
        QCOMPARE(target->documentState().dirty, stateBefore.dirty);
        QVERIFY(!target->isModified());
        QCOMPARE(target->filePath(), pathBefore);
        QCOMPARE(target->encoding(), encodingBefore);
        QCOMPARE(provider->fileSize(), sourceSize);
        QCOMPARE(target->contentSizeBytes(), sourceSize);
        QCOMPARE(provider->sourceBytes(0, chunk.size()), chunk);
        QCOMPARE(provider->sourceBytes(sourceSize - chunk.size(), chunk.size()), chunk);
        QCOMPARE(target->editor(), provider->editor());
        QCOMPARE(provider->editor()->text(), projectionBefore);
        QCOMPARE(provider->editor()->isModified(), projectionModifiedBefore);
        QVERIFY(provider->editor()->isReadOnly() && target->isReadOnlyByPolicy());
        QCOMPARE(target->contentCharacterCount(), qint64(-1));
        QVERIFY(!provider->characterCountPending());

        // Observe the real status label updated by characterCountChanged;
        // do not call the presenter or MainWindow's refresh slot from the test.
        QTRY_VERIFY_WITH_TIMEOUT(count->isVisible()
                                     && !count->text().contains(QStringLiteral("统计中"))
                                     && count->text().contains(QStringLiteral("暂不可用")), 1500);
        const QString details = count->toolTip() + QLatin1Char('\n')
            + count->accessibleDescription();
        QVERIFY2(!details.contains(QStringLiteral("正在后台统计")), qPrintable(details));
        QVERIFY2(details.contains(QStringLiteral("重试"))
                     || details.contains(QStringLiteral("重新打开"))
                     || (details.contains(QStringLiteral("任务"))
                         && details.contains(QStringLiteral("查看"))), qPrintable(details));
        const QString spoken = accessibleText(QAccessible::queryAccessibleInterface(count));
        QVERIFY2(!spoken.contains(QStringLiteral("统计中")), qPrintable(spoken));
        QVERIFY2(spoken.contains(QStringLiteral("暂不可用")), qPrintable(spoken));
        QCOMPARE(manager.taskById(taskId).state, AsyncTaskState::Failed);
        QCOMPARE(tasks.providerIds().size(), qsizetype(1));
        QCOMPARE(sourceHash(), hashBefore);
#endif
    }
};

QTEST_MAIN(SmokeTaskRealErrors)
#include "smoke_task_real_errors.moc"
