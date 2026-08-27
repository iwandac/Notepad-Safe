#include "MainWindow.h"
#include "AppComposition.h"
#include "AsyncSerialQueue.h"
#include "DocumentOpenWorkflow.h"
#include "EditorTab.h"
#include "FileTierPolicy.h"
#include "SessionManager.h"
#include "VaultDock.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>

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

// ============ 会话 ============

void MainWindow::restoreSession()
{
    SessionData data = m_session->loadSession();
    if (data.tabs.isEmpty())
        return;

    bool hasUnsaved = false;
    for (const SessionTab &st : data.tabs) {
        auto *tab = addNewTab();

        bool loaded = false;
        // 优先从 autosave 恢复未保存内容
        if (!st.autosavePath.isEmpty() && QFileInfo::exists(st.autosavePath)) {
            loaded = m_session->restoreAutosave(tab, st.autosavePath, st.filePath);
            if (loaded)
                hasUnsaved = true;
            // 恢复语法（autosave 本身不猜语法）
            tab->setSyntax(static_cast<Syntax>(st.syntax));
        } else if (!st.filePath.isEmpty() && QFileInfo::exists(st.filePath)) {
            loaded = openSessionDocument(m_composition->documentProviders(), tab, st);
        }

        if (!loaded) {
            tab->setSyntax(static_cast<Syntax>(st.syntax));
        } else {
            restoreDocumentSessionView(tab, st);
        }
        m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    }

    if (data.activeTab >= 0 && data.activeTab < m_tabs->count())
        m_tabs->setCurrentIndex(data.activeTab);

    if (hasUnsaved) {
        statusBar()->showMessage(QStringLiteral("已恢复上次未保存的会话"), 5000);
    }

    // 恢复完成后清理 session.json（不清 autosave，因为会话内存中已关联，
    // 下次 saveSession 会用 keep-list 正确保留/清理）
    QFile::remove(m_session->sessionFilePath());
}
void MainWindow::saveSessionNow(bool forceSnapshots)
{
    if (!forceSnapshots && AsyncSerialQueue::session().busy())
        return; // Coalesce periodic autosaves; the next timer captures the latest revision.

    QVector<EditorTab *> tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        // SessionManager only records secure document ids and encrypted backing
        // paths; it never serializes decrypted secure text.
        if (tab)
            tabs.append(tab);
    }
    auto plan = m_session->captureAsyncSave(
        tabs, m_tabs->currentIndex(), forceSnapshots);
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
            QVector<EditorTab *> currentTabs;
            for (int i = 0; i < guarded->m_tabs->count(); ++i) {
                if (auto *tab = qobject_cast<EditorTab *>(guarded->m_tabs->widget(i)))
                    currentTabs.append(tab);
            }
            guarded->m_session->applyAsyncSave(*result, currentTabs);
            if (state == AsyncTaskState::Failed)
                guarded->statusBar()->showMessage(
                    QStringLiteral("自动恢复快照未能提交；将稍后重试。"), 5000);
        };
        AsyncSerialQueue::session().enqueue(std::move(request));
        return;
    }

    QProgressDialog progress(QStringLiteral("正在建立最新恢复点…"), QString(),
                             0, 0, this);
    progress.setObjectName(QStringLiteral("sessionCommitProgress"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(150);
    progress.setCancelButton(nullptr);
    QEventLoop loop;
    request.completed = [guarded, result, &loop, &progress](AsyncTaskState state) {
        if (guarded && guarded->m_session) {
            QVector<EditorTab *> currentTabs;
            for (int i = 0; i < guarded->m_tabs->count(); ++i) {
                if (auto *tab = qobject_cast<EditorTab *>(guarded->m_tabs->widget(i)))
                    currentTabs.append(tab);
            }
            guarded->m_session->applyAsyncSave(*result, currentTabs);
            if (state == AsyncTaskState::Failed) {
                guarded->statusBar()->showMessage(
                    QStringLiteral("恢复点提交失败；已有恢复文件仍被保留。"), 7000);
            }
        }
        progress.close();
        loop.quit();
    };
    AsyncSerialQueue::session().enqueue(std::move(request));
    progress.show();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

// ============================================================================
//  PendingSessionRestore：MainWindow 私有结构体（会话恢复队列状态）
//  头文件用前置声明避免拖慢编译；完整定义只在此 cpp 中。
// ============================================================================
struct MainWindow::PendingSessionRestore {
    QVector<SessionTab> tabs;
    int activeTab = 0;
    int currentIndex = 0;   // 已处理到第几个 tab
    bool hasUnsaved = false;
    QString sessionJsonPathToRemove;
    QPointer<EditorTab> preferredActiveTab;
};

// ============================================================================
//  异步会话恢复：deferredRestoreSession + restoreNextSessionTab
//  设计目的：
//   · 构造函数返回前不读任何大文件（避免 main.cpp 的 w.show() 被延后）
//   · 每个 tab 恢复之间 QApplication::processEvents 让窗口重绘
//   · 走 openDocumentByPolicy ≥32MB 强制 L2 窗口化秒开，不再 L1 主线程阻塞读
// ============================================================================
void MainWindow::deferredRestoreSession()
{
    // 此时 main.cpp 的 w.show() 已调用；但首次 paint 需要事件循环派发。
    // 主动 AllEvents 派发一次，确保窗口立刻可见（用户不会"双击后以为没启动"）。
    QApplication::processEvents(QEventLoop::AllEvents);

    QPointer<EditorTab> preferredActiveTab = currentTab();
    SessionData data = m_session->loadSession();
    if (data.tabs.isEmpty()) {
        // 没有会话记录：补一个空 tab（对应原构造函数中的空 tab 兜底）
        if (m_tabs->count() == 0)
            newFile();
        return;
    }

    // 构造排队上下文（使用 new，因为恢复过程跨多个事件循环 tick）
    m_pendingRestore = new PendingSessionRestore;
    m_pendingRestore->tabs                 = data.tabs;   // 拷贝 session 列表
    m_pendingRestore->activeTab            = data.activeTab;
    m_pendingRestore->currentIndex         = 0;
    m_pendingRestore->hasUnsaved           = false;
    m_pendingRestore->sessionJsonPathToRemove = m_session->sessionFilePath();
    m_pendingRestore->preferredActiveTab      = preferredActiveTab;

    // 立刻给用户看到 "正在恢复会话… 0/M"（StatusBar 永久可见，同时窗口已绘制）
    statusBar()->showMessage(
        QStringLiteral("正在恢复会话… 0/%1 个标签页").arg(m_pendingRestore->tabs.size()),
        0);

    // 第 1 个 tab：singleShot(0) 入队，确保 deferredRestoreSession 返回后事件循环再跑它
    QTimer::singleShot(0, this, &MainWindow::restoreNextSessionTab);
}

void MainWindow::restoreNextSessionTab()
{
    if (!m_pendingRestore) return;

    const int total = m_pendingRestore->tabs.size();

    // --- 收尾：全部 tab 已处理完 ---
    if (m_pendingRestore->currentIndex >= total) {
        if (m_pendingRestore->preferredActiveTab
            && m_tabs->indexOf(m_pendingRestore->preferredActiveTab) >= 0) {
            m_tabs->setCurrentWidget(m_pendingRestore->preferredActiveTab);
        } else if (m_pendingRestore->activeTab >= 0
                   && m_pendingRestore->activeTab < m_tabs->count()) {
            m_tabs->setCurrentIndex(m_pendingRestore->activeTab);
        }

        if (!m_pendingRestore->sessionJsonPathToRemove.isEmpty())
            QFile::remove(m_pendingRestore->sessionJsonPathToRemove);

        if (m_pendingRestore->hasUnsaved) {
            statusBar()->showMessage(
                QStringLiteral("已恢复 %1 个标签页（含未保存修改）").arg(total), 5000);
        } else {
            statusBar()->showMessage(
                QStringLiteral("已恢复 %1 个标签页").arg(total), 3000);
        }

        delete m_pendingRestore;
        m_pendingRestore = nullptr;
        // 安全兜底：一个 tab 都没恢复成功（比如文件全部被删除）→ 补一个空 tab
        if (m_tabs->count() == 0)
            newFile();
        return;
    }

    QElapsedTimer tabTimer; tabTimer.start();
    const SessionTab &st = m_pendingRestore->tabs[m_pendingRestore->currentIndex];
    EditorTab *tab = addNewTab();
    qDebug("[STARTUP][RESTORE tab %d/%d] addNewTab 完成, 耗时=%lld ms | filePath='%s' autosave='%s'",
           m_pendingRestore->currentIndex + 1, total, tabTimer.elapsed(),
           qUtf8Printable(st.filePath), qUtf8Printable(st.autosavePath));
    tabTimer.restart();

    bool loaded = false;

    if (st.secureDocument) {
        const QString securePath = !st.filePath.isEmpty() ? st.filePath : st.autosavePath;
        loaded = !securePath.isEmpty() && QFileInfo::exists(securePath)
                 && openSecurePath(securePath, tab);
    } else if (!st.autosavePath.isEmpty() && QFileInfo::exists(st.autosavePath)) {
        // ====== 关键修复（BUG C：大文件 autosave 阻塞启动）======
        //   旧做法：直接 tab->loadAutosave(st.autosavePath)
        //     · 100MB autosave：file.readAll() 主线程同步读 1~3s，窗口不显示、无提示
        //   新做法：tab->openAutosaveAutoMode(autosave, real)
        //     · S1：走原 loadAutosave（毫秒级）
        //     · 尺寸 ≥ 限制：L2 mmap autosave 文件 → incomplete LineIndex → 秒开返回
        //       后台 AsyncTaskManager 增量构建行索引，用户可见窗口 + 状态栏进度
        //   参数：autosavePath = 实际数据源，filePath = 对用户显示的原名/未来保存路径
        const qint64 asSize = QFileInfo(st.autosavePath).size();
        const qint64 limit = FileTierPolicy::StandardEditLimitBytes;
        const FileTierDecision tier = FileTierPolicy::classify(asSize);
        qDebug("[STARTUP][RESTORE tab %d/%d] → 走 autosave 分支: size=%lld KB, S1-limit=%lld KB",
               m_pendingRestore->currentIndex + 1, total,
               asSize / 1024, limit / 1024);
        if (tier.usesMappedPreview()) {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选窗口化秒开 (S2/S3)，"
                   "应 < 50ms 返回；若之后 >= 500ms 才返回，说明 openAutosaveAutoMode 内部阻塞了！",
                   m_pendingRestore->currentIndex + 1, total);
        } else {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L1 同步 loadAutosave (asSize < 限制)，"
                   "一般毫秒级。", m_pendingRestore->currentIndex + 1, total);
        }

        loaded = m_session->restoreAutosave(tab, st.autosavePath, st.filePath);
        if (loaded)
            m_pendingRestore->hasUnsaved = true;
        qDebug("[STARTUP][RESTORE tab %d/%d]   restoreAutosave 返回 ok=%d, 耗时=%lld ms",
               m_pendingRestore->currentIndex + 1, total, loaded ? 1 : 0, tabTimer.elapsed());
        tab->setSyntax(static_cast<Syntax>(st.syntax));
    } else if (!st.filePath.isEmpty() && QFileInfo::exists(st.filePath)) {
        // ===== 关键修复：不再直接 tab->loadFile(st.filePath) =====
        //   原 bug：即便 100MB 也走 L1 全量 readAll + 主线程分块；
        //   新逻辑：openDocumentByPolicy → ≥ 32MB 自动 openLargeFileL2（窗口化只读，无后台编辑加载）
        const qint64 realSize = QFileInfo(st.filePath).size();
        const qint64 limit = FileTierPolicy::StandardEditLimitBytes;
        const FileTierDecision tier = FileTierPolicy::classify(realSize);
        qDebug("[STARTUP][RESTORE tab %d/%d] → 走真实文件路径分支: size=%lld KB, S1-limit=%lld KB",
               m_pendingRestore->currentIndex + 1, total,
               realSize / 1024, limit / 1024);
        if (tier.usesMappedPreview()) {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选窗口化秒开 (S2/S3)，应 < 50ms 返回。",
                   m_pendingRestore->currentIndex + 1, total);
        } else {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L1 同步 loadFile (< 限制)。",
                   m_pendingRestore->currentIndex + 1, total);
        }
        loaded = openSessionDocument(m_composition->documentProviders(), tab, st);
        qDebug("[STARTUP][RESTORE tab %d/%d]   openDocumentByPolicy 返回 ok=%d, 耗时=%lld ms（关键：如果 > 500ms 就是阻塞启动的根因！）",
               m_pendingRestore->currentIndex + 1, total, loaded ? 1 : 0, tabTimer.elapsed());
    } else {
        qDebug("[STARTUP][RESTORE tab %d/%d] → 文件不存在或路径为空，保持空标签页",
               m_pendingRestore->currentIndex + 1, total);
    }
    tabTimer.restart();

    if (!loaded) {
        // A failed/missing session item must not survive as a legacy plaintext
        // blank tab. After the restore queue finishes, the normal default-new
        // path creates an encrypted draft if no document could be recovered.
        const int failedIndex = m_tabs->indexOf(tab);
        if (failedIndex >= 0)
            m_tabs->removeTab(failedIndex);
        delete tab;
        tab = nullptr;
    } else {
        restoreDocumentSessionView(tab, st);
        m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    }

    // Session items are background restoration when the process was launched
    // for an explicit file. Do not let each restored tab steal the visible
    // document while the user is already reading or editing it.
    if (m_pendingRestore->preferredActiveTab
        && m_tabs->indexOf(m_pendingRestore->preferredActiveTab) >= 0) {
        m_tabs->setCurrentWidget(m_pendingRestore->preferredActiveTab);
    }

    m_pendingRestore->currentIndex++;
    const int done = m_pendingRestore->currentIndex;
    statusBar()->showMessage(
        QStringLiteral("正在恢复会话… %1/%2 个标签页").arg(done).arg(total), 0);

    qDebug("[STARTUP][RESTORE tab %d/%d] 收尾(cursor/syntax/title/statusBar)耗时=%lld ms。调用 processEvents 让 UI 重绘…",
           done, total, tabTimer.elapsed());
    QElapsedTimer peTimer; peTimer.start();
    // 仅重绘、不接受新用户输入（避免编辑态冲突），窗口可见性 + 状态栏进度会更新
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    qDebug("[STARTUP][RESTORE tab %d/%d] processEvents(ExcludeUserInput) 耗时=%lld ms。"
           "下一 tab 通过 singleShot(0) 入队，UI 线程先释放给 DWM 合成。",
           done, total, peTimer.elapsed());

    // 下一个 tab：入队（如果 L2 大文件，AsyncTaskManager 会在后台继续跑不阻塞）
    QTimer::singleShot(0, this, &MainWindow::restoreNextSessionTab);
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
    m_edgeColumn = settings->value("edgeColumn", 80).toInt();
    if (m_edgeColumn < 1) m_edgeColumn = 80;
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

    const bool toolbarVisible = settings->value("toolbarVisible", true).toBool();
    const bool statusVisible = settings->value("statusBarVisible", true).toBool();
    if (m_toolbarVisibilityAction)
        m_toolbarVisibilityAction->setChecked(toolbarVisible);
    if (auto *toolbar = findChild<QToolBar *>(QStringLiteral("MainToolBar")))
        toolbar->setVisible(toolbarVisible);
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
    if (auto *toolbar = findChild<QToolBar *>(QStringLiteral("MainToolBar")))
        settings->setValue("toolbarVisible", toolbar->isVisible());
    settings->setValue("statusBarVisible", statusBar()->isVisible());
    settings->remove("vaultDockVisible");
}

// ============ 关闭 ============

void MainWindow::closeEvent(QCloseEvent *event)
{
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

    // 所有中央加密标签已经在 maybeSaveTab 中写回KDBX后，才允许锁定
    // 安全身份；否则会先销毁密钥，再导致加密标签无法保存。
    if (m_vaultDock && !m_vaultDock->prepareToClose()) {
        resumeTimers();
        event->ignore();
        return;
    }
    saveSettings();


    // Step 2: 保存会话
    saveSessionNow(true);

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
            tab->clearAutosaveFile();
        }
    }

    // Keep the sealed seven-day session for the next launch, but destroy all
    // process-resident document credentials and identity material now.
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(false);
    clearSensitiveDocumentClipboard();

    event->accept();
}
