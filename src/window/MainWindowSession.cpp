#include "MainWindow.h"
#include "AppComposition.h"
#include "AsyncSerialQueue.h"
#include "EditorTab.h"
#include "EditorPreferences.h"
#include "SearchResultsDock.h"
#include "FileTierPolicy.h"
#include "SessionManager.h"
#include "VaultDock.h"
#include "WorkspaceRecoveryCoordinator.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScopeGuard>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

#include <memory>
#include <stdexcept>
#include <utility>

namespace {
std::unique_ptr<QSettings> createAppSettings()
{
    const QString dataDir = qEnvironmentVariable("NOTEPADSAFE_DATADIR").trimmed();
    const QFileInfo dataDirInfo(dataDir);
    if (!dataDir.isEmpty() && dataDirInfo.isAbsolute()) {
        QDir dir(dataDirInfo.absoluteFilePath());
        dir.mkpath(QStringLiteral("."));
        return std::make_unique<QSettings>(dir.filePath(QStringLiteral("settings.ini")),
                                           QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(QStringLiteral("NotepadSafe"),
                                       QStringLiteral("NotepadSafe"));
}
} // namespace

bool MainWindow::saveSessionNow(bool forceSnapshots, bool allowRecoveryChoice)
{
    if (!forceSnapshots && AsyncSerialQueue::session().busy())
        return true; // Coalesce periodic autosaves; the next timer captures the latest revision.

    bool preserveInvalidSession = false;
    if (!WorkspaceRecoveryCoordinator::forWindow(this).prepareSessionSave(
            forceSnapshots && allowRecoveryChoice, &preserveInvalidSession))
        return false;
    // Capture live ports only after the modal choice, which runs an event loop.
    int activeIndex = 0;
    const auto entries = WorkspaceRecoveryCoordinator::forWindow(this).captureEntries(&activeIndex);
    auto plan = m_session->captureAsyncSaveEntries(entries, activeIndex, forceSnapshots,
                                                  preserveInvalidSession);
    auto result = std::make_shared<std::shared_ptr<SessionManager::AsyncSaveResult>>();

    AsyncSerialQueue::Request request;
    request.type = AsyncTaskType::SessionSnapshot;
    request.priority = forceSnapshots ? AsyncTaskPriority::Highest
                                      : AsyncTaskPriority::Lowest;
    request.title = forceSnapshots ? QStringLiteral("正在提交恢复点")
                                   : QStringLiteral("正在保存恢复快照");
    request.categoryTag = QStringLiteral("Recovery");
    request.cancellable = false;
    request.worker = [manager = m_session, plan, result]
        (qint64, AsyncCancelPtr, const auto &, const auto &) {
            *result = manager->writeAsyncSave(plan);
            if (!manager->asyncSaveSucceeded(*result))
                throw std::runtime_error("session recovery snapshot commit failed");
        };

    QPointer<MainWindow> guarded(this);
    if (!forceSnapshots) {
        request.completed = [guarded, result](AsyncTaskState state) {
            if (!guarded || !guarded->m_session)
                return;
            QVector<ISessionDocumentPort *> currentTabs;
            for (int i = 0; i < guarded->m_tabs->count(); ++i) {
                if (auto *tab = dynamic_cast<ISessionDocumentPort *>(
                        guarded->m_tabs->widget(i)))
                    currentTabs.append(tab);
            }
            guarded->m_session->applyAsyncSave(*result, currentTabs);
            if (state == AsyncTaskState::Failed)
                guarded->statusBar()->showMessage(
                    QStringLiteral("自动恢复快照未能提交；将稍后重试。"), 5000);
        };
        AsyncSerialQueue::session().enqueue(std::move(request));
        return true;
    }

    QProgressDialog progress(QStringLiteral("正在建立最新恢复点…"), QString(),
                             0, 0, this);
    progress.setObjectName(QStringLiteral("sessionCommitProgress"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(150);
    progress.setCancelButton(nullptr);
    QEventLoop loop;
    bool committed = false;
    request.completed = [guarded, result, &loop, &progress,
                         &committed](AsyncTaskState state) {
        if (guarded && guarded->m_session) {
            QVector<ISessionDocumentPort *> currentTabs;
            for (int i = 0; i < guarded->m_tabs->count(); ++i) {
                if (auto *tab = dynamic_cast<ISessionDocumentPort *>(
                    guarded->m_tabs->widget(i)))
                    currentTabs.append(tab);
            }
            guarded->m_session->applyAsyncSave(*result, currentTabs);
            committed = state == AsyncTaskState::Finished
                        && guarded->m_session->asyncSaveSucceeded(*result);
            WorkspaceRecoveryCoordinator::forWindow(guarded).reportSessionSaveOutcome(
                !committed, guarded->m_session->asyncSaveError(*result),
                guarded->m_session->asyncSavePreservedPath(*result));
        }
        progress.close();
        loop.quit();
    };
    AsyncSerialQueue::session().enqueue(std::move(request));
    progress.show();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return committed;
}

void MainWindow::deferredRestoreSession()
{
    WorkspaceRecoveryCoordinator::forWindow(this).start();
}

void MainWindow::restoreNextSessionTab()
{
    WorkspaceRecoveryCoordinator::forWindow(this).restoreNext();
}

// ============ 设置 ============

void MainWindow::loadSettings()
{
    const auto settings = createAppSettings();
    const QString appearance = settings->value("appearanceMode").toString().trimmed();
    if (appearance == QLatin1String("dark")) {
        m_appearanceMode = AppearanceMode::Dark;
    } else if (appearance == QLatin1String("light")) {
        m_appearanceMode = AppearanceMode::Light;
    } else if (appearance == QLatin1String("system")) {
        m_appearanceMode = AppearanceMode::System;
    } else if (settings->contains("darkMode")) {
        // One-time migration preserves the old explicit visual choice. A new
        // profile with neither key follows Windows by default.
        m_appearanceMode = settings->value("darkMode").toBool()
                               ? AppearanceMode::Dark : AppearanceMode::Light;
    } else {
        m_appearanceMode = AppearanceMode::System;
    }
    const QStringList storedRecent = settings->value("recentFiles").toStringList();
    m_recentFiles.clear();
    for (const QString &storedPath : storedRecent) {
        if (storedPath.trimmed().isEmpty())
            continue;
        const QString normalized = QDir::cleanPath(
            QFileInfo(storedPath).absoluteFilePath());
        DiskFileVersion candidate;
        const bool candidateValid = captureDiskFileVersion(normalized, &candidate)
                                    == DiskFileState::Unchanged;
        bool duplicate = false;
        for (const QString &existingPath : std::as_const(m_recentFiles)) {
            duplicate = sameDiskFilePath(existingPath, normalized);
            if (!duplicate && candidateValid) {
                DiskFileVersion existing;
                duplicate = captureDiskFileVersion(existingPath, &existing)
                                == DiskFileState::Unchanged
                            && sameDiskFile(candidate, existing);
            }
            if (duplicate)
                break;
        }
        if (!duplicate)
            m_recentFiles.append(normalized);
        if (m_recentFiles.size() >= MAX_RECENT)
            break;
    }
    // createMenus() runs before settings are loaded, so rebuild now; otherwise
    // the visible submenu remains the initial "(none)" until another file is opened.
    rebuildRecentFilesMenu();

    m_edgeVisible = settings->value("edgeVisible", true).toBool();
    m_edgeColumn = normalizeRulerColumn(settings->value("edgeColumn", 80).toInt());
    m_wordWrap = settings->value("wordWrap", false).toBool();
    m_whitespaceVisible = settings->value("editor/showWhitespace", false).toBool();
    m_eolVisible = settings->value("editor/showEol", false).toBool();
    m_useTabs = settings->value("editor/useTabs", false).toBool();
    m_tabWidth = qBound(1, settings->value("editor/tabWidth", 4).toInt(), 16);
    m_backspaceUnindents = settings->value(
        "editor/backspaceUnindents", true).toBool();
    m_wordCompletion = settings->value("editor/wordCompletion", true).toBool();

    QSize winSize = settings->value("windowSize", QSize(1200, 800)).toSize();
    if (winSize.isValid())
        resize(winSize);

    QByteArray geo = settings->value("windowGeometry").toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);

    QByteArray state = settings->value("windowState").toByteArray();
    if (!state.isEmpty())
        restoreState(state);
    if (m_searchDock && dockWidgetArea(m_searchDock) == Qt::TopDockWidgetArea)
        addDockWidget(Qt::BottomDockWidgetArea, m_searchDock);

    const bool statusVisible = settings->value("statusBarVisible", true).toBool();
    if (m_statusVisibilityAction)
        m_statusVisibilityAction->setChecked(statusVisible);
    statusBar()->setVisible(statusVisible);
    if (m_vaultDock)
        m_vaultDock->show();
}

void MainWindow::saveSettings()
{
    const auto settings = createAppSettings();
    QString appearance = QStringLiteral("system");
    if (m_appearanceMode == AppearanceMode::Light)
        appearance = QStringLiteral("light");
    else if (m_appearanceMode == AppearanceMode::Dark)
        appearance = QStringLiteral("dark");
    settings->setValue("appearanceMode", appearance);
    settings->remove("darkMode");
    settings->setValue("recentFiles", m_recentFiles);
    settings->setValue("edgeVisible", m_edgeVisible);
    settings->setValue("edgeColumn", m_edgeColumn);
    settings->setValue("wordWrap", m_wordWrap);
    settings->setValue("editor/showWhitespace", m_whitespaceVisible);
    settings->setValue("editor/showEol", m_eolVisible);
    settings->setValue("editor/useTabs", m_useTabs);
    settings->setValue("editor/tabWidth", m_tabWidth);
    settings->setValue("editor/backspaceUnindents", m_backspaceUnindents);
    settings->setValue("editor/wordCompletion", m_wordCompletion);
    settings->setValue("windowSize", size());
    settings->setValue("windowGeometry", saveGeometry());
    settings->setValue("windowState", saveState());
    settings->remove("toolbarVisible");
    settings->setValue("statusBarVisible", statusBar()->isVisible());
    settings->remove("vaultDockVisible");
}

// ============ 关闭 ============

void MainWindow::closeEvent(QCloseEvent *event)
{
    // A nested window/tab close must not delete a target paused in teardown.
    if (m_windowCloseInProgress || !m_closingTabs.isEmpty()) {
        event->ignore();
        return;
    }
    if (!WorkspaceRecoveryCoordinator::forWindow(this).beginWindowClose()) {
        event->ignore(); // The synchronous restore stack must unwind before teardown.
        return;
    }
    m_windowCloseInProgress = true;
    const QPointer<MainWindow> owner(this);
    const auto releaseClose = qScopeGuard([owner] {
        if (owner)
            owner->m_windowCloseInProgress = false;
    });
    // shutdownL2BeforeClose() intentionally pumps the event loop while workers
    // settle. Prevent autosave/session timers from re-entering document I/O in
    // the middle of that close transaction. If closing is cancelled, restore
    // the timers with their normal intervals.
    const bool autosaveWasActive = m_autosaveTimer.isActive();
    const bool secureAutosaveWasActive = m_secureAutosaveTimer.isActive();
    const bool expiryWasActive = m_securityExpiryTimer.isActive();
    m_autosaveTimer.stop();
    m_secureAutosaveTimer.stop();
    m_securityExpiryTimer.stop();
    const auto resumeTimers = [this, autosaveWasActive,
                               secureAutosaveWasActive, expiryWasActive]() {
        WorkspaceRecoveryCoordinator::forWindow(this).cancelWindowClose();
        if (autosaveWasActive)
            m_autosaveTimer.start();
        if (secureAutosaveWasActive)
            m_secureAutosaveTimer.start();
        if (expiryWasActive)
            m_securityExpiryTimer.start();
    };

    // Step 1: 按 Notepad++ 规则检查关闭 (closingApp=true)
    // - 对"已存在文件且已修改"：弹窗提示用户 保存/丢弃/取消
    // - 对"未命名文件+有内容"：不提示用户，后续写入 autosave（下次启动恢复）
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!maybeSaveTab(i, /*closingApp=*/true)) {
            // 用户按了 Cancel，中止关闭
            resumeTimers();
            event->ignore();
            return;
        }
    }

    // Step 2: establish the latest durable recovery responsibility before any
    // irreversible close preparation. A failed commit must leave Accounts and
    // the document workspace usable for a real retry.
    if (!saveSessionNow(true, true)) {
        resumeTimers();
        event->ignore();
        return;
    }

    // 所有中央加密标签已经在 maybeSaveTab 中写回 KDBX 后，才允许锁定
    // Accounts；否则会先销毁密钥，再导致加密标签无法保存。这里刻意放在
    // 恢复点提交之后，避免提交失败时仍可见的窗口被 prepareToClose 静默锁库。
    if (m_vaultDock && !m_vaultDock->prepareToClose()) {
        resumeTimers();
        event->ignore();
        return;
    }
    emit shutdownCommitted();
    saveSettings();

    // Step 2.5（崩溃修复关键）：
    //    saveSessionNow() 后，所有需要 L2 mmap 的路径（exportToFile → applyAndSave）已完成。
    //    此时 MainWindow 仍完整（event->accept 还没执行），processEvents 合法。
    //    对每个 L2 tab 调 shutdownL2BeforeClose()：cancel 后台 LineIndex 构建 +
    //    processEvents 轮询直到 AsyncTask.state=终态 + unmap mmap。
    //    之后即使 EditorTab 析构调了 closeFile(false)，isOpen()=false → NOP；
    //    即使走到 closeFile(false)，内部也不会有 processEvents / emit / viewport update
    //    （m_inDestruction 守卫），不会触发半析构对象的 slot invoke → 0xC0000005。
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab) tab->shutdownL2BeforeClose();
    }

    // Step 3: 清理已正式保存的文件对应的 autosave（未命名/有修改的不清，下次需要恢复）
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab) continue;
        if (!tab->isSecureDocument() && !tab->isUntitled() && !tab->isModified()) {
            m_session->discardRecovery(tab);
        }
    }

    // Keep the sealed seven-day session for the next launch, but destroy all
    // process-resident document credentials and identity material now.
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(false);
    clearSensitiveDocumentClipboard();

    event->accept();
}
