#include "WorkspaceRecoveryCoordinator.h"

#include "AppComposition.h"
#include "DocumentOpenWorkflow.h"
#include "DocumentSessionView.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "RecoveryIssuePage.h"
#include "SessionManager.h"

#include <QFile>
#include <QFileInfo>
#include <QStatusBar>
#include <QSet>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

namespace {

void showEmptyWorkspaceHint(QStatusBar *bar, QTabWidget *tabs,
                            QStackedWidget *workspace, const QString &message)
{
    const auto clearOwnHint = [bar, message] {
        if (bar->currentMessage() == message)
            bar->clearMessage();
    };
    QObject::connect(tabs, &QTabWidget::currentChanged, bar, clearOwnHint,
                     Qt::SingleShotConnection);
    QObject::connect(workspace, &QStackedWidget::currentChanged, bar, clearOwnHint,
                     Qt::SingleShotConnection);
    bar->showMessage(message, 0);
}

} // namespace

WorkspaceRecoveryCoordinator::WorkspaceRecoveryCoordinator(MainWindow *window)
    : window_(window)
{
}

WorkspaceRecoveryCoordinator &WorkspaceRecoveryCoordinator::forWindow(MainWindow *window)
{
    if (!window->m_workspaceRecovery)
        window->m_workspaceRecovery = std::make_unique<WorkspaceRecoveryCoordinator>(window);
    return *window->m_workspaceRecovery;
}

void WorkspaceRecoveryCoordinator::ensureOriginalRecordsLoaded()
{
    if (recordsLoaded_)
        return;
    // Capture can precede scheduled restoration (including a first-paint close).
    // Only load the existing manifest; do not create surfaces or read draft bodies.
    const SessionData data = window_->m_session->loadSession();
    for (const SessionTab &record : data.tabs)
        items_.append(Item{record, Phase::Pending, {}});
    activeTab_ = data.activeTab;
    recordsLoaded_ = true;
}

QVector<SessionCaptureEntry> WorkspaceRecoveryCoordinator::captureEntries(int *activeIndex)
{
    ensureOriginalRecordsLoaded();
    QVector<SessionCaptureEntry> entries;
    QSet<QWidget *> represented;
    QHash<ISessionDocumentPort *, int> liveEntries;
    QWidget *current = window_->m_tabs->currentWidget();
    int selected = -1;
    int fallback = -1;
    const auto appendLive = [&](QWidget *page) {
        auto *port = dynamic_cast<ISessionDocumentPort *>(page);
        if (!port)
            return -1;
        if (!liveEntries.contains(port)) {
            liveEntries.insert(port, entries.size());
            entries.append(SessionCaptureEntry{port});
        }
        const int index = liveEntries.value(port);
        if (page == current)
            selected = index;
        return index;
    };
    const auto needsOriginal = [&](const Item &item) {
        return item.phase != Phase::Closed
               && (item.phase != Phase::Live || !item.widget
                   || window_->m_tabs->indexOf(item.widget) < 0);
    };
    QSet<int> retainedEntries;
    const auto appendOriginal = [&](int index) {
        const int entryIndex = entries.size();
        entries.append(SessionCaptureEntry{items_.at(index).original});
        retainedEntries.insert(index);
        if (current && (current == items_.at(index).widget
                        || (index == inFlightIndex_ && current == inFlightTarget_)))
            selected = entryIndex;
        if (index == activeTab_)
            fallback = entryIndex;
        return entryIndex;
    };
    bool needsRetainedEntries = false;
    for (const Item &item : items_)
        needsRetainedEntries = needsRetainedEntries || needsOriginal(item);
    if (finished_ || !needsRetainedEntries) {
        // After initial restoration, honor the user's actual tab order. A retry
        // substitutes its old record in the issue's UI slot, not the staging tab.
        for (int index = 0; index < window_->m_tabs->count(); ++index) {
            QWidget *page = window_->m_tabs->widget(index);
            int itemIndex = itemForWidget(page);
            if (inFlightIndex_ >= 0 && page == inFlightTarget_) {
                if (inFlightOriginalPage_ && window_->m_tabs->indexOf(inFlightOriginalPage_) >= 0)
                    continue;
                itemIndex = inFlightIndex_;
            }
            if (itemIndex >= 0 && items_.at(itemIndex).phase == Phase::Closed)
                continue;
            if (itemIndex >= 0 && needsOriginal(items_.at(itemIndex)))
                appendOriginal(itemIndex);
            else
                appendLive(page);
        }
        for (int index = 0; index < items_.size(); ++index)
            if (needsOriginal(items_.at(index)) && !retainedEntries.contains(index))
                appendOriginal(index);
        *activeIndex = selected >= 0 ? selected : qMax(0, fallback);
        return entries;
    }
    for (int index = 0; index < items_.size(); ++index) {
        const Item &item = items_.at(index);
        if (item.widget)
            represented.insert(item.widget);
        if (index == inFlightIndex_ && inFlightTarget_)
            represented.insert(inFlightTarget_);
        if (item.phase == Phase::Closed)
            continue;
        int entryIndex = -1;
        if (item.phase == Phase::Live && item.widget
            && window_->m_tabs->indexOf(item.widget) >= 0) {
            entryIndex = appendLive(item.widget);
        } else {
            entryIndex = appendOriginal(index);
        }
        if (index == activeTab_)
            fallback = entryIndex;
    }
    for (int index = 0; index < window_->m_tabs->count(); ++index) {
        QWidget *page = window_->m_tabs->widget(index);
        if (!represented.contains(page))
            appendLive(page);
    }
    *activeIndex = selected >= 0 ? selected : qMax(0, fallback);
    return entries;
}

void WorkspaceRecoveryCoordinator::queueNext()
{
    if (!started_ || finished_ || closing_ || inFlightIndex_ >= 0)
        return;
    const quint64 epoch = ++scheduleEpoch_;
    QTimer::singleShot(0, window_, [this, epoch] {
        if (epoch == scheduleEpoch_ && !closing_)
            restoreNext();
    });
}

void WorkspaceRecoveryCoordinator::beginFlight(int index)
{
    inFlightIndex_ = index;
    phaseBeforeFlight_ = items_.at(index).phase;
    inFlightOriginalPage_ = items_.at(index).widget;
    items_[index].phase = Phase::InFlight;
}

bool WorkspaceRecoveryCoordinator::mayCreateTarget(int itemIndex) const
{
    return itemIndex < 0 || (!closing_ && itemIndex == inFlightIndex_);
}

void WorkspaceRecoveryCoordinator::attachInFlightTarget(int itemIndex, EditorTab *tab)
{
    if (itemIndex >= 0 && itemIndex == inFlightIndex_)
        inFlightTarget_ = tab;
}

void WorkspaceRecoveryCoordinator::endFlight()
{
    if (inFlightIndex_ < 0)
        return;
    Item &item = items_[inFlightIndex_];
    if (item.phase == Phase::InFlight) {
        // Only reached after the synchronous reader/native calls returned.
        // Never discard the original recovery record when abandoning a staging tab.
        removeEditorWithoutRecoveryAction(inFlightTarget_);
        if (item.widget && item.widget != inFlightOriginalPage_) {
            window_->m_tabs->removeTab(window_->m_tabs->indexOf(item.widget));
            delete item.widget.data();
        }
        item.widget = inFlightOriginalPage_;
        item.phase = phaseBeforeFlight_;
    }
    inFlightTarget_.clear();
    inFlightOriginalPage_.clear();
    inFlightIndex_ = -1;
    if (deferredClose_) {
        const quint64 epoch = scheduleEpoch_;
        QTimer::singleShot(0, window_, [this, epoch] {
            if (closing_ && deferredClose_ && epoch == scheduleEpoch_)
                window_->close();
        });
    }
}

void WorkspaceRecoveryCoordinator::placeDeliveredTab(int itemIndex)
{
    auto *tabs = window_->m_tabs;
    QWidget *page = items_.at(itemIndex).widget;
    const int from = tabs->indexOf(page);
    if (from < 0 || items_.at(itemIndex).phase == Phase::Closed)
        return;
    // Insert only this newly delivered page beside an existing original
    // neighbor. Never sort already delivered pages or undo the user's moves.
    for (int next = itemIndex + 1; next < items_.size(); ++next) {
        const Item &neighbor = items_.at(next);
        const int to = tabs->indexOf(neighbor.widget);
        if (neighbor.phase != Phase::Closed && to >= 0 && to != from) {
            tabs->tabBar()->moveTab(from, from < to ? to - 1 : to);
            return; // moveTab can synchronously close/remove the page.
        }
    }
    for (int previous = itemIndex - 1; previous >= 0; --previous) {
        const Item &neighbor = items_.at(previous);
        const int to = tabs->indexOf(neighbor.widget);
        if (neighbor.phase != Phase::Closed && to >= 0 && to != from) {
            tabs->tabBar()->moveTab(from, from < to ? to : to + 1);
            return;
        }
    }
}

bool WorkspaceRecoveryCoordinator::beginWindowClose()
{
    closing_ = true;
    ++scheduleEpoch_;
    deferredClose_ = inFlightIndex_ >= 0;
    return !deferredClose_;
}

void WorkspaceRecoveryCoordinator::cancelWindowClose()
{
    closing_ = false;
    deferredClose_ = false;
    queueNext();
}

bool WorkspaceRecoveryCoordinator::isInFlight(const QWidget *widget) const
{
    return inFlightIndex_ >= 0
           && (widget == inFlightTarget_ || widget == inFlightOriginalPage_);
}

int WorkspaceRecoveryCoordinator::itemForWidget(const QWidget *widget) const
{
    for (int index = 0; index < items_.size(); ++index)
        if (items_.at(index).widget == widget)
            return index;
    return -1;
}

void WorkspaceRecoveryCoordinator::noteAcceptedRemoval(QWidget *widget)
{
    for (Item &item : items_)
        if (item.widget == widget)
            item.phase = Phase::Closed;
}

void WorkspaceRecoveryCoordinator::start()
{
    if (!window_ || started_ || closing_)
        return;
    ensureOriginalRecordsLoaded();
    started_ = true;
    if (items_.isEmpty()) {
        finished_ = true;
        const QPointer<MainWindow> guarded(window_);
        if (window_->m_tabs->count() == 0 && window_->m_workspaceStack
            && window_->m_workspaceStack->currentWidget() == window_->m_tabs
            && window_->statusBar()->currentMessage().isEmpty()) {
            showEmptyWorkspaceHint(window_->statusBar(), window_->m_tabs,
                window_->m_workspaceStack,
                QObject::tr("当前没有打开的文档。可通过文件→新建或文件→打开开始。"));
        }
        if (!guarded || closing_)
            return;
        emit window_->sessionRestoreCompleted();
        return;
    }

    hasUnsaved_ = false;
    // Scheduling indices do not change the original records or capture order.
    if (activeTab_ >= 0 && activeTab_ < items_.size())
        restoreOrder_.append(activeTab_);
    for (int index = 0; index < items_.size(); ++index)
        if (index != activeTab_)
            restoreOrder_.append(index);
    const QPointer<MainWindow> guarded(window_);
    window_->statusBar()->showMessage(
        QObject::tr("正在恢复会话… 0/%1 个标签页").arg(items_.size()), 0);
    if (guarded && !closing_)
        queueNext();
}

void WorkspaceRecoveryCoordinator::restoreNext()
{
    if (!window_ || items_.isEmpty() || finished_ || closing_ || inFlightIndex_ >= 0)
        return;
    while (processedCount_ < restoreOrder_.size()
           && items_.at(restoreOrder_.at(processedCount_)).phase == Phase::Closed)
        ++processedCount_;
    if (processedCount_ >= restoreOrder_.size()) {
        finish();
        return;
    }

    const int itemIndex = restoreOrder_.at(processedCount_);
    const SessionTab state = items_.at(itemIndex).original;
    const QPointer<MainWindow> guarded(window_);
    beginFlight(itemIndex);
    const auto finishFlight = qScopeGuard([this, guarded] { if (guarded) endFlight(); });
    QPointer<EditorTab> tab;
    QPointer<QWidget> resolved;
    bool loaded = false;
    bool reused = false;
    QString diagnosticCode;
    QString detail;
    const bool originalFile = !state.secureDocument && !state.modified
                              && state.autosavePath.isEmpty();

    if (state.secureDocument) {
        tab = window_->addNewTab(MainWindow::OpenPresentationPolicy::PreserveCurrentWorkspace, itemIndex);
        const QString securePath = !state.filePath.isEmpty()
            ? state.filePath : state.autosavePath;
        loaded = !closing_ && !securePath.isEmpty() && QFileInfo::exists(securePath)
                 && window_->openSecurePath(securePath, tab,
                     MainWindow::OpenPresentationPolicy::PreserveCurrentWorkspace);
    } else if (!originalFile) {
        tab = window_->addNewTab(MainWindow::OpenPresentationPolicy::PreserveCurrentWorkspace, itemIndex);
        loaded = !closing_ && restoreDraft(tab, state, &diagnosticCode, &detail);
        if (loaded)
            hasUnsaved_ = true;
    } else {
        tab = openOriginalFile(state, &reused, &diagnosticCode, &detail);
        loaded = tab != nullptr;
    }

    if (closing_)
        return;
    if (!loaded && !state.secureDocument) {
        resolved = replaceWithIssue(tab, state,
            originalFile ? RecoveryIssuePage::Kind::OriginalFile
                         : RecoveryIssuePage::Kind::UnsavedDraft,
            diagnosticCode.isEmpty()
                ? QStringLiteral("[recovery.restore.failed]") : diagnosticCode,
            detail);
    } else if (!loaded) {
        removeEditorWithoutRecoveryAction(tab);
    } else if (tab) {
        if (!reused)
            restoreDocumentSessionView(tab, state);
        if (closing_ || !tab)
            return;
        window_->m_tabs->setTabText(window_->m_tabs->indexOf(tab), tab->tabTitle());
        resolved = tab;
    }
    if (closing_)
        return;
    items_[itemIndex].widget = resolved;
    items_[itemIndex].phase = resolved ? Phase::Live : Phase::RetainedFailure;
    ++processedCount_;
    endFlight();
    if (!guarded || closing_)
        return;

    // A complete live responsibility and the next scheduling position must be
    // visible to close/Cancel before moveTab/currentChanged or status callbacks.
    if (resolved && !reused)
        placeDeliveredTab(itemIndex);
    if (!guarded || closing_)
        return;
    window_->statusBar()->showMessage(
        QObject::tr("正在恢复会话… %1/%2 个标签页")
            .arg(processedCount_).arg(items_.size()), 0);
    if (guarded && !closing_)
        queueNext();
}

void WorkspaceRecoveryCoordinator::finish()
{
    if (finished_ || closing_)
        return;
    finished_ = true;
    QSet<EditorTab *> restoredDocuments;
    int unresolvedCount = 0;
    for (const Item &item : items_) {
        if (item.phase == Phase::Closed)
            continue;
        auto *tab = qobject_cast<EditorTab *>(item.widget.data());
        if (tab && window_->m_tabs->indexOf(tab) >= 0)
            restoredDocuments.insert(tab);
        else
            ++unresolvedCount;
    }
    const QPointer<MainWindow> guarded(window_);
    if (unresolvedCount == 0) {
        // Completed UI restoration is not a durable save of dirty documents.
        // Keep the published manifest; SessionManager alone replaces/cleans it.
        QString summary = hasUnsaved_
            ? QObject::tr("已恢复 %1 个标签页（含未保存修改）").arg(restoredDocuments.size())
            : QObject::tr("已恢复 %1 个标签页").arg(restoredDocuments.size());
        const bool emptyDocumentWorkspace = window_->m_tabs->count() == 0
            && window_->m_workspaceStack
            && window_->m_workspaceStack->currentWidget() == window_->m_tabs;
        if (emptyDocumentWorkspace) {
            summary += QObject::tr("。可通过文件→新建或文件→打开开始。");
            showEmptyWorkspaceHint(window_->statusBar(), window_->m_tabs,
                window_->m_workspaceStack, summary);
        } else {
            window_->statusBar()->showMessage(summary, hasUnsaved_ ? 5000 : 3000);
        }
    } else {
        window_->statusBar()->showMessage(
            QObject::tr("已恢复 %1 个标签页，%2 项尚未完成。原文件未被本次恢复修改；请在待处理标签页重试，或用文件→打开。")
                .arg(restoredDocuments.size()).arg(unresolvedCount), 0);
    }

    if (!guarded || closing_)
        return;
    emit window_->sessionRestoreCompleted();
}

RecoveryIssuePage *WorkspaceRecoveryCoordinator::replaceWithIssue(
    EditorTab *failedTab, const SessionTab &state,
    RecoveryIssuePage::Kind kind,
    const QString &diagnosticCode, const QString &detail)
{
    const int index = failedTab ? window_->m_tabs->indexOf(failedTab) : -1;
    removeEditorWithoutRecoveryAction(failedTab);
    if (closing_)
        return nullptr;
    auto *issue = new RecoveryIssuePage(state, kind, diagnosticCode, detail, window_);
    if (inFlightIndex_ >= 0)
        items_[inFlightIndex_].widget = issue;
    const QString name = state.filePath.isEmpty()
        ? QObject::tr("未命名文档") : QFileInfo(state.filePath).fileName();
    const int inserted = window_->m_tabs->insertTab(
        index < 0 ? window_->m_tabs->count() : index, issue,
        (kind == RecoveryIssuePage::Kind::OriginalFile
             ? QObject::tr("打开待处理 · %1") : QObject::tr("恢复待处理 · %1")).arg(name));
    if (closing_)
        return issue;
    window_->m_tabs->tabBar()->setTabButton(inserted, QTabBar::LeftSide, nullptr);
    window_->m_tabs->tabBar()->setTabButton(inserted, QTabBar::RightSide, nullptr);
    QObject::connect(issue, &RecoveryIssuePage::retryRequested, window_,
                     [this, issue]() { retry(issue); });
    QObject::connect(issue, &RecoveryIssuePage::discardConfirmed, window_,
                     [this, issue]() { discard(issue); });
    QObject::connect(issue, &RecoveryIssuePage::originalRemovalRequested, window_,
                     [this, issue]() { removeOriginalIssue(issue); });
    return issue;
}

EditorTab *WorkspaceRecoveryCoordinator::openOriginalFile(
    const SessionTab &state, bool *reused, QString *diagnosticCode, QString *detail)
{
    *reused = false;
    if (state.secureDocument || state.modified || !state.autosavePath.isEmpty()) {
        *diagnosticCode = QStringLiteral("[recovery.original.invalid]");
        *detail = QObject::tr("此项包含未保存恢复信息，不能用磁盘原文件替代。请重试恢复。");
        return nullptr;
    }
    const QFileInfo info(state.filePath);
    if (state.filePath.isEmpty() || !info.isFile()) {
        *diagnosticCode = QStringLiteral("[recovery.original.missing]");
        *detail = QObject::tr("找不到原文件。请确认文件位置后重试打开，或用文件→打开选择文件。");
        return nullptr;
    }
    QFile readable(state.filePath);
    if (!readable.open(QIODevice::ReadOnly)) {
        *diagnosticCode = QStringLiteral("[recovery.original.unreadable]");
        *detail = QObject::tr("暂时无法读取原文件。请检查读取权限或文件占用情况后重试打开。");
        return nullptr;
    }
    const auto adapter = window_->m_composition->persistenceAdapters().resolve(state.filePath);
    if (adapter && adapter->persistence == DocumentPersistence::SecureFile) {
        *diagnosticCode = QStringLiteral("[recovery.original.secure]");
        *detail = QObject::tr("此路径当前是安全文件，不能按旧的普通文件记录恢复。请在窗口内用文件→打开；本次不会解锁。");
        return nullptr;
    }
    if (EditorTab *existing = window_->findOpenPlainDocument(state.filePath)) {
        *reused = true;
        return existing;
    }
    EditorTab *tab = window_->addNewTab(
        MainWindow::OpenPresentationPolicy::PreserveCurrentWorkspace, inFlightIndex_);
    if (closing_)
        return tab;
    if (!openSessionDocument(window_->m_composition->documentProviders(), tab, state)) {
        removeEditorWithoutRecoveryAction(tab);
        *diagnosticCode = QStringLiteral("[recovery.original.open.failed]");
        *detail = QObject::tr("原文件暂时无法打开。原文件未被修改，请检查文件后重试打开。");
        return nullptr;
    }
    return tab;
}

bool WorkspaceRecoveryCoordinator::restoreDraft(EditorTab *tab, const SessionTab &state,
                                                 QString *diagnosticCode, QString *detail)
{
    if (state.autosavePath.isEmpty()) {
        *diagnosticCode = QStringLiteral("[recovery.draft.missing]");
        *detail = QObject::tr("未找到此项未保存内容的恢复稿，不能用磁盘原文件替代。原文件未被修改。");
        return false;
    }
    if (!state.filePath.isEmpty()) {
        const auto adapter = window_->m_composition->persistenceAdapters().resolve(state.filePath);
        if (adapter && adapter->persistence == DocumentPersistence::SecureFile) {
            *diagnosticCode = QStringLiteral("[recovery.source.secure]");
            *detail = QObject::tr("原文件当前是安全文件，不能把未保存的普通内容绑定到它。原文件未被修改；请先用文件→打开确认原文件，再重试恢复。");
            return false;
        }
    }
    return window_->m_session->restoreAutosave(
        tab, state.autosavePath, state.filePath, detail, diagnosticCode);
}

void WorkspaceRecoveryCoordinator::retry(RecoveryIssuePage *issue)
{
    if (!issue || window_->m_tabs->indexOf(issue) < 0 || closing_ || inFlightIndex_ >= 0)
        return;
    const int itemIndex = itemForWidget(issue);
    if (itemIndex < 0 || items_.at(itemIndex).phase == Phase::Closed)
        return;
    const SessionTab state = issue->captureSessionTab();
    const int issueIndex = window_->m_tabs->indexOf(issue);
    const QPointer<RecoveryIssuePage> guardedIssue(issue);
    QPointer<QWidget> previousCurrent = window_->m_tabs->currentWidget();
    const bool issueWasCurrent = previousCurrent == issue;
    const bool originalFile = issue->kind() == RecoveryIssuePage::Kind::OriginalFile;
    bool reused = false;
    QPointer<EditorTab> tab;
    QString detail;
    QString code;
    bool loaded = false;
    beginFlight(itemIndex);
    const auto finishFlight = qScopeGuard([this] { endFlight(); });
    if (originalFile) {
        tab = openOriginalFile(state, &reused, &code, &detail);
        loaded = tab != nullptr;
    } else {
        tab = window_->addNewTab(MainWindow::OpenPresentationPolicy::FocusDocumentWorkspace, itemIndex);
        loaded = !closing_ && restoreDraft(tab, state, &code, &detail);
    }
    if (closing_ || !guardedIssue)
        return;
    if (!loaded) {
        removeEditorWithoutRecoveryAction(tab);
        if (closing_ || !guardedIssue)
            return;
        items_[itemIndex].phase = Phase::Live;
        endFlight();
        issue->updateFailure(
            code.isEmpty() ? QStringLiteral("[recovery.restore.failed]") : code,
            detail);
        if (window_->m_tabs->indexOf(previousCurrent) >= 0)
            window_->m_tabs->setCurrentWidget(previousCurrent);
        window_->statusBar()->showMessage(
            QObject::tr("此项仍未完成；原文件没有被本次重试修改。请查看原因后重试。"),
            5000);
        return;
    }

    if (!reused)
        restoreDocumentSessionView(tab, state);
    if (closing_ || !tab || !guardedIssue)
        return;
    window_->m_tabs->setTabText(window_->m_tabs->indexOf(tab), tab->tabTitle());
    if (!reused)
        window_->m_tabs->tabBar()->moveTab(window_->m_tabs->indexOf(tab), issueIndex);
    if (closing_ || !tab || !guardedIssue)
        return;
    // The destination is fully restored. Transfer responsibility before removing
    // the current issue can synchronously enter close/Cancel through currentChanged.
    items_[itemIndex].widget = tab;
    items_[itemIndex].phase = Phase::Live;
    endFlight();
    window_->m_tabs->removeTab(window_->m_tabs->indexOf(issue));
    if (guardedIssue)
        guardedIssue->deleteLater();
    if (closing_ || !tab)
        return;
    window_->m_tabs->setCurrentWidget(issueWasCurrent ? static_cast<QWidget *>(tab.data())
                                                      : previousCurrent.data());
    if (closing_)
        return;
    window_->statusBar()->showMessage(originalFile ? QObject::tr("原文件已重新打开。")
                                                  : QObject::tr("未保存修改已恢复。"), 4000);
    window_->saveSessionNow(true);
}

void WorkspaceRecoveryCoordinator::discard(RecoveryIssuePage *issue)
{
    if (!issue || issue->kind() != RecoveryIssuePage::Kind::UnsavedDraft
        || window_->m_tabs->indexOf(issue) < 0 || closing_ || isInFlight(issue))
        return;
    if (!window_->m_session->discardRecovery(issue)) {
        issue->updateFailure(
            QStringLiteral("[recovery.discard.failed]"),
            QObject::tr("恢复稿无法删除；本次放弃已取消，原文件和会话不变。"));
        window_->statusBar()->showMessage(
            QObject::tr("无法删除恢复稿；已保留待处理标签页。"), 5000);
        return;
    }
    removeIssuePage(issue);
    window_->statusBar()->showMessage(
        QObject::tr("已放弃此项未保存恢复；原文件未修改。"), 5000);
    window_->saveSessionNow(true);
}

void WorkspaceRecoveryCoordinator::removeOriginalIssue(RecoveryIssuePage *issue)
{
    if (!issue || issue->kind() != RecoveryIssuePage::Kind::OriginalFile
        || window_->m_tabs->indexOf(issue) < 0 || closing_ || isInFlight(issue))
        return;
    // This removes only the pending-open record. It must never call the
    // managed recovery-file deletion authority used by UnsavedDraft.
    removeIssuePage(issue);
    window_->statusBar()->showMessage(QObject::tr("已移除此待打开标签；原文件未修改。"), 5000);
    window_->saveSessionNow(true);
}

void WorkspaceRecoveryCoordinator::removeIssuePage(RecoveryIssuePage *issue)
{
    noteAcceptedRemoval(issue);
    QWidget *previousCurrent = window_->m_tabs->currentWidget();
    const bool issueWasCurrent = previousCurrent == issue;
    const int issueIndex = window_->m_tabs->indexOf(issue);
    window_->m_tabs->removeTab(issueIndex);
    issue->deleteLater();
    if (window_->m_tabs->count() == 0) {
        window_->addNewTab();
    } else if (!issueWasCurrent && window_->m_tabs->indexOf(previousCurrent) >= 0) {
        window_->m_tabs->setCurrentWidget(previousCurrent);
    } else {
        window_->m_tabs->setCurrentIndex(qMin(issueIndex,
                                              window_->m_tabs->count() - 1));
    }
}

void WorkspaceRecoveryCoordinator::removeEditorWithoutRecoveryAction(EditorTab *tab)
{
    if (!tab)
        return;
    const QPointer<EditorTab> guarded(tab);
    tab->shutdownL2BeforeClose();
    if (!guarded)
        return;
    const int index = window_->m_tabs->indexOf(tab);
    if (index >= 0)
        window_->m_tabs->removeTab(index);
    delete tab;
}
