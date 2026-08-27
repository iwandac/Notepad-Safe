#include "MainWindow.h"
#include "EditorTab.h"
#include "FileContentProbe.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SearchResultsController.h"
#include "SessionManager.h"
#include "TextEncoding.h"
#include "FileTierPolicy.h"
#include "VirtualFileView.h"
#include "StatusBarTaskWidget.h"
#include "AsyncSerialQueue.h"
#include "AsyncUiOperation.h"
#include "VaultDock.h"
#include "SecureDocumentService.h"
#include "SecurityServiceFactory.h"
#include "SecuritySessionService.h"
#include "AppComposition.h"
#include "FileCredentialDialog.h"
#include "SensitiveText.h"
#include "CommandRegistry.h"
#include "DocumentOpenWorkflow.h"

#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStyle>
#include <QStatusBar>
#include <QLabel>
#include <QToolButton>
#include <QToolBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QFont>
#include <QPalette>
#include <QStyleHints>
#include <QIcon>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QShortcut>
#include <QSignalBlocker>
#include <QKeySequence>
#include <QKeyEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QUuid>
#include <QPointer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QProcess>
#include <QLocale>
#include <QEventLoop>
#include <QProgressDialog>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#ifndef NOTEPADSAFE_VERSION
#define NOTEPADSAFE_VERSION "0.0.0-dev"
#endif

#ifdef Q_OS_WIN
#include <Windows.h>
#include <WtsApi32.h>
#endif

namespace {

bool systemHighContrastEnabled()
{
    // Deterministic GUI coverage without changing the developer machine's OS
    // accessibility settings. This is intentionally process-local.
    if (qEnvironmentVariableIntValue("NOTEPADSAFE_FORCE_HIGH_CONTRAST") == 1)
        return true;
#ifdef Q_OS_WIN
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(state), &state, 0)
           && (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
#else
    return false;
#endif
}


} // namespace

// 前向：同步编码菜单勾选状态
static void syncEncodingMenuTo(QMenu *menu, TextEncoding enc)
{
    if (!menu) return;
    const auto acts = menu->actions();
    for (QAction *a : acts) {
        bool ok = false;
        int val = a->data().toInt(&ok);
        if (ok && static_cast<TextEncoding>(val) == enc) {
            a->setChecked(true);
        } else if (ok) {
            a->setChecked(false);
        }
    }
}

MainWindow::MainWindow(QWidget *parent, AppComposition *composition)
    : QMainWindow(parent)
    , m_ownedComposition(composition ? nullptr : std::make_unique<AppComposition>())
    , m_composition(composition ? composition : m_ownedComposition.get())
{
    setWindowTitle(QStringLiteral("Notepad Safe"));
    setWindowIcon(QIcon(QStringLiteral(":/notepad_safe.png")));
    resize(1200, 800);
    setAcceptDrops(true);

    // 标签页
    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_workspaceStack = new QStackedWidget(this);
    m_workspaceStack->setObjectName(QStringLiteral("WorkspaceStack"));
    m_workspaceStack->addWidget(m_tabs);
    setCentralWidget(m_workspaceStack);
    // 把 m_tabs 也加入 drag 转发（防止 QTabBar 吞事件）
    registerDropProxy(m_tabs);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showTabContextMenu);

    // 搜索 Dock
    m_searchResultsController = std::make_unique<SearchResultsController>();
    m_searchDock = new SearchResultsDock(*m_searchResultsController, this);
    m_searchDock->setObjectName(QStringLiteral("SearchResultsDock"));
    addDockWidget(Qt::BottomDockWidgetArea, m_searchDock);
    m_searchDock->hide();
    connect(m_searchDock, &SearchResultsDock::jumpToByteOffset,
            this, &MainWindow::onJumpToByteOffset);
    connect(m_searchDock, &SearchResultsDock::jumpToDocumentByteOffset,
            this, &MainWindow::onJumpToDocumentByteOffset);
    connect(m_searchDock, &SearchResultsDock::clearSearchHighlightsRequested,
            this, [this](const QUuid &documentId) {
        for (int index = 0; m_tabs && index < m_tabs->count(); ++index) {
            auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
            if (tab && tab->documentState().id == documentId) {
                tab->clearSearchHighlights();
                return;
            }
        }
    });

    // Accounts and Security Center are created on first use. Their widgets and
    // backend discovery must not participate in the empty-Shell first paint.

    // 会话管理器
    m_session = new SessionManager(this);
    createActions();
    createMenus();         // 菜单项用默认值创建，action 指针存在 m_act* / m_edgeColGroup
    createToolBar();
    createStatusBar();
    m_commandRegistry = std::make_unique<CommandRegistry>();
    m_commandRegistry->bind(this);
    if (!m_commandRegistry->unboundCommandIds().isEmpty())
        qWarning() << "Unbound command ids:" << m_commandRegistry->unboundCommandIds();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, [this]() { syncCommandStates(currentTab()); });
    syncCommandStates(nullptr);

    loadSettings();        // 从 QSettings 覆盖外观/换行/标尺等偏好
    syncGlobalMenuCheckStates(); // 关键：把恢复后的设置同步回菜单勾选状态（解决UI不同步bug）
    applyAppearanceMode();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) {
                if (m_appearanceMode == AppearanceMode::System)
                    applyAppearanceMode();
            });

    // ============================================================
    //  启动阶段会话恢复触发权移交给 main.cpp !!
    //  原做法 v1：构造内同步 restoreSession() → 构造阻塞 → w.show() 永远执行不到
    //  原做法 v2：构造内 QTimer::singleShot(0, deferredRestoreSession)
    //             → main.cpp 的 processEvents 派发时，zero-timer 比 Paint 事件先运行
    //             → 会话恢复先跑完 916ms 才开始绘制窗口 → 用户依然感觉「双击没反应/静默」
    //  新做法 v3：StartupCoordinator 观察首次实际 Paint 完成，再以 queued
    //             invocation 触发 deferredRestoreSession。外部打开排在恢复之前，
    //             不依赖固定毫秒数猜测 DWM/事件队列时序。
    // ============================================================
    //   【此处不允许再注册任何启动会话恢复的 timer / 调用】

    // "没有标签就创建新标签"—— 同样延后到 deferredRestoreSession 结束后再判断
    // （否则空 tab + 异步恢复叠加导致多 1 个空白 tab）

    // 自动保存定时器（每 30 秒保存未保存内容）
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this]() {
        // 只在有未保存标签时才保存
        bool hasUnsaved = false;
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
            if (t && (t->isModified() || t->isUntitled())) {
                hasUnsaved = true;
                break;
            }
        }
        if (hasUnsaved)
            saveSessionNow();
    });
    m_autosaveTimer.start(30000);

    // 加密笔记直接保存回KDBX，绝不进入明文 session/autosave 目录。
    // 仅在安全身份已经解锁时后台保存；锁定状态下不突然弹出解锁框。
    m_secureAutosaveTimer.setSingleShot(true);
    m_secureAutosaveTimer.setInterval(5000);
    connect(&m_secureAutosaveTimer, &QTimer::timeout, this, [this]() {
        if (AsyncSerialQueue::security().busy()) {
            m_secureAutosaveTimer.start(1000);
            return;
        }
        if (!m_secureDocuments || !m_securitySession
            || !m_securitySession->hasActiveSession())
            return;
        bool retryNeeded = false;
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
            if (tab && tab->isSecureDocument() && tab->isModified())
                retryNeeded = !saveSecureTab(tab, false, false) || retryNeeded;
        }
        if (retryNeeded)
            m_secureAutosaveTimer.start(30000);
    });

    m_securityExpiryTimer.setInterval(60 * 1000);
    connect(&m_securityExpiryTimer, &QTimer::timeout, this, [this]() {
        if (AsyncSerialQueue::security().busy())
            return;
        if (!m_securitySession || !m_securitySession->expiresAtUtc().isValid()
            || QDateTime::currentDateTimeUtc() < m_securitySession->expiresAtUtc()) {
            return;
        }
        lockSecureDocumentsImpl(true);
    });
    m_securityExpiryTimer.start();
}

MainWindow::~MainWindow()
{
    m_destroying = true;
    m_securityExpiryTimer.stop();
    if (m_vaultDock)
        disconnect(m_vaultDock, nullptr, this, nullptr);
    // A test host or forced shutdown may destroy the window without sending a
    // close event. Release retained KeePassXC QObject graphs on their owning
    // security lane before the service objects themselves are destroyed.
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(false);
#ifdef Q_OS_WIN
    if (m_sessionNotificationsRegistered)
        ::WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(winId()));
#endif
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    if (!m_sessionNotificationsRegistered) {
        m_sessionNotificationsRegistered = ::WTSRegisterSessionNotification(
            reinterpret_cast<HWND>(winId()), NOTIFY_FOR_THIS_SESSION) != FALSE;
    }
#endif
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);
    if (m_firstPaintCompleted)
        return;
    m_firstPaintCompleted = true;
    emit firstPaintCompleted();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message,
                             qintptr *result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    auto *nativeMessage = static_cast<MSG *>(message);
    if (nativeMessage) {
        if (nativeMessage->message == WM_WTSSESSION_CHANGE) {
            if (nativeMessage->wParam == WTS_SESSION_LOCK)
                protectForSystemLock();
            else if (nativeMessage->wParam == WTS_SESSION_UNLOCK)
                restoreAfterSystemUnlock();
        } else if (nativeMessage->message == WM_POWERBROADCAST) {
            if (nativeMessage->wParam == PBT_APMSUSPEND)
                protectForSystemLock();
            else if (nativeMessage->wParam == PBT_APMRESUMEAUTOMATIC)
                QTimer::singleShot(0, this, &MainWindow::restoreAfterSystemUnlock);
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::showDocumentWorkspace()
{
    QWidget *leavingPage = m_workspaceStack ? m_workspaceStack->currentWidget() : nullptr;
    const bool leavingAccounts = leavingPage && leavingPage == m_accountsPage;
    const bool leavingAuxiliaryWorkspace = leavingPage && leavingPage != m_tabs;
    if (leavingAccounts && m_vaultDock && !m_vaultDock->releaseWorkspaceSecrets())
        return;
    if (m_workspaceStack && m_tabs)
        m_workspaceStack->setCurrentWidget(m_tabs);
    if (leavingAuxiliaryWorkspace && m_searchDockWasVisibleBeforeWorkspace && m_searchDock)
        m_searchDock->show();
    if (auto *tab = currentTab()) {
        if (m_findDialog)
            m_findDialog->setActiveTab(tab);
        tab->setFocusEditor();
    }
    syncCommandStates(currentTab());
    updateStatusBar();
}

bool MainWindow::isDocumentWorkspaceActive() const
{
    return !m_workspaceStack || !m_tabs
           || m_workspaceStack->currentWidget() == m_tabs;
}

void MainWindow::ensureAccountsWorkspaceCreated()
{
    if (m_accountsPage)
        return;
    m_composition->recordHeavyFactoryCall();
    m_accountsPage = new QWidget(m_workspaceStack);
    m_accountsPage->setObjectName(QStringLiteral("AccountsWorkspace"));
    auto *accountsLayout = new QVBoxLayout(m_accountsPage);
    accountsLayout->setContentsMargins(18, 14, 18, 14);
    auto *accountsHeader = new QHBoxLayout();
    auto *backToDocuments = new QPushButton(QStringLiteral("← 返回文档"), m_accountsPage);
    backToDocuments->setObjectName(QStringLiteral("accountsBackToDocuments"));
    auto *accountsTitle = new QLabel(QStringLiteral("账户"), m_accountsPage);
    QFont titleFont = accountsTitle->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    accountsTitle->setFont(titleFont);
    accountsHeader->addWidget(backToDocuments);
    accountsHeader->addSpacing(12);
    accountsHeader->addWidget(accountsTitle);
    accountsHeader->addStretch();
    accountsLayout->addLayout(accountsHeader);
    m_vaultDock = new VaultDock(m_accountsPage);
    m_vaultDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    auto *emptyDockTitle = new QWidget(m_vaultDock);
    emptyDockTitle->setFixedHeight(0);
    m_vaultDock->setTitleBarWidget(emptyDockTitle);
    accountsLayout->addWidget(m_vaultDock, 1);
    m_workspaceStack->addWidget(m_accountsPage);
    connect(backToDocuments, &QPushButton::clicked,
            this, &MainWindow::showDocumentWorkspace);
    connect(m_vaultDock, &VaultDock::lockRequested,
            this, &MainWindow::lockSecureDocuments);
    connect(m_vaultDock, &VaultDock::secureStoreLocked,
            this, &MainWindow::maskSecureDocuments);
    connect(m_vaultDock, &VaultDock::workspaceCommandStateChanged,
            this, [this]() { syncCommandStates(currentTab()); });
    connect(m_vaultDock, &VaultDock::userNotificationRequested,
            this, [this](const QString &message, int timeoutMs) {
                statusBar()->showMessage(message, timeoutMs);
            });
    connect(m_vaultDock, &VaultDock::externalStoreReloadRequested,
            this, [this](const QString &path) {
                if (!m_vaultDock || path.isEmpty())
                    return;
                m_vaultDock->discardAndLockForExternalReload();
                if (!openPathUnified(path)) {
                    QMessageBox::warning(
                        this, QStringLiteral("无法重新打开账户文件"),
                        QStringLiteral("磁盘版本无法使用当前会话或输入的凭据解锁。"));
                    return;
                }
                showAccountsWorkspace();
                statusBar()->showMessage(
                    QStringLiteral("已重新打开账户文件的磁盘版本"), 3000);
            });
}

void MainWindow::ensureSecurityCenterCreated()
{
    if (m_securityCenterPage)
        return;
    m_composition->recordHeavyFactoryCall();
    m_securityCenterPage = new QWidget(m_workspaceStack);
    m_securityCenterPage->setObjectName(QStringLiteral("SecurityCenterWorkspace"));
    auto *layout = new QVBoxLayout(m_securityCenterPage);
    layout->setContentsMargins(28, 20, 28, 24);
    layout->setSpacing(16);
    auto *header = new QHBoxLayout();
    auto *back = new QPushButton(QStringLiteral("← 返回文档"), m_securityCenterPage);
    back->setObjectName(QStringLiteral("securityCenterBackToDocuments"));
    auto *title = new QLabel(QStringLiteral("安全中心"), m_securityCenterPage);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    header->addWidget(back);
    header->addSpacing(12);
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);
    auto *intro = new QLabel(
        QStringLiteral("这里管理本机安全身份、恢复能力和七天设备会话。日常编辑无需在这里执行加解密操作。"),
        m_securityCenterPage);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *deviceGroup = new QGroupBox(QStringLiteral("本设备会话"), m_securityCenterPage);
    deviceGroup->setObjectName(QStringLiteral("securityCenterDeviceGroup"));
    auto *deviceLayout = new QHBoxLayout(deviceGroup);
    m_securitySessionStatus = new QLabel(deviceGroup);
    m_securitySessionStatus->setObjectName(QStringLiteral("securityCenterSessionStatus"));
    m_securitySessionStatus->setWordWrap(true);
    auto *lock = new QPushButton(QStringLiteral("立即锁定"), deviceGroup);
    lock->setObjectName(QStringLiteral("securityCenterLockNow"));
    deviceLayout->addWidget(m_securitySessionStatus, 1);
    deviceLayout->addWidget(lock, 0, Qt::AlignTop);
    layout->addWidget(deviceGroup);

    auto *identityGroup = new QGroupBox(QStringLiteral("主密码与本地身份"),
                                         m_securityCenterPage);
    identityGroup->setObjectName(QStringLiteral("securityCenterIdentityGroup"));
    auto *identityLayout = new QVBoxLayout(identityGroup);
    m_securityIdentityStatus = new QLabel(identityGroup);
    m_securityIdentityStatus->setObjectName(QStringLiteral("securityCenterIdentityStatus"));
    m_securityIdentityStatus->setWordWrap(true);
    identityLayout->addWidget(m_securityIdentityStatus);
    auto *explanation = new QLabel(
        QStringLiteral("主密码不存储在设备会话中；会话只保存由 Windows 当前用户保护的派生密钥。"),
        identityGroup);
    explanation->setWordWrap(true);
    identityLayout->addWidget(explanation);
    auto *identityActions = new QHBoxLayout();
    identityActions->addStretch();
    auto *changePassword = new QPushButton(QStringLiteral("修改主密码…"), identityGroup);
    changePassword->setObjectName(QStringLiteral("securityCenterChangePassword"));
    identityActions->addWidget(changePassword);
    identityLayout->addLayout(identityActions);
    layout->addWidget(identityGroup);

    auto *recoveryGroup = new QGroupBox(QStringLiteral("恢复"), m_securityCenterPage);
    recoveryGroup->setObjectName(QStringLiteral("securityCenterRecoveryGroup"));
    auto *recoveryLayout = new QHBoxLayout(recoveryGroup);
    m_securityRecoveryStatus = new QLabel(
        QStringLiteral("恢复密钥用于应用数据损坏、设备迁移或忘记主密码时解锁文件。请离线保存。"),
        recoveryGroup);
    m_securityRecoveryStatus->setObjectName(QStringLiteral("securityCenterRecoveryStatus"));
    m_securityRecoveryStatus->setWordWrap(true);
    auto *recovery = new QPushButton(QStringLiteral("查看或导出恢复密钥"), recoveryGroup);
    recovery->setObjectName(QStringLiteral("securityCenterRecoveryButton"));
    auto *verify = new QPushButton(QStringLiteral("验证恢复密钥…"), recoveryGroup);
    verify->setObjectName(QStringLiteral("securityCenterVerifyRecovery"));
    auto *recoveryActions = new QVBoxLayout();
    recoveryActions->addWidget(recovery);
    recoveryActions->addWidget(verify);
    recoveryActions->addStretch();
    recoveryLayout->addWidget(m_securityRecoveryStatus, 1);
    recoveryLayout->addLayout(recoveryActions);
    layout->addWidget(recoveryGroup);
    layout->addStretch();
    m_workspaceStack->addWidget(m_securityCenterPage);
    connect(back, &QPushButton::clicked, this, &MainWindow::showDocumentWorkspace);
    connect(recovery, &QPushButton::clicked, this, &MainWindow::exportRecoveryKey);
    connect(verify, &QPushButton::clicked, this, &MainWindow::verifyRecoveryKey);
    connect(changePassword, &QPushButton::clicked,
            this, &MainWindow::changeMasterPassword);
    connect(lock, &QPushButton::clicked, this, [this]() {
        lockSecureDocuments();
        refreshSecurityCenter();
        if (!m_securitySession || !m_securitySession->hasActiveSession())
            showDocumentWorkspace();
    });
}

bool MainWindow::showAccountsWorkspace()
{
    ensureSecurityServicesCreated();
    if (!m_securitySession || !m_securitySession->available()) {
        QMessageBox::critical(this, QStringLiteral("账户功能不可用"),
                              QStringLiteral("当前构建未包含 KDBX 安全后端。"));
        return false;
    }
    ensureAccountsWorkspaceCreated();
    if (!m_vaultDock || !ensureSecuritySessionInteractive())
        return false;
    if (!m_vaultDock->isSecureStoreOpen()) {
        const QString path = m_vaultDock->preferredManagedStorePath();
        SecurityOperationLease lease;
        VaultError error = m_securitySession->acquireOperationLease(&lease);
        if (error.isError()) {
            QMessageBox::critical(this, QStringLiteral("无法使用安全会话"),
                                  error.userMessage);
            return false;
        }
        if (QFileInfo::exists(path)) {
            error = m_vaultDock->unlockManagedStoreWithMasterKey(
                path, lease.masterUnlockKey());
            if (error.code == VaultErrorCode::InvalidCredentials) {
                lease.clear();
                // Historical/foreign account files get the same single fallback
                // password flow as files opened from the unified Open action.
                if (!openPathUnified(path) || !m_vaultDock->isSecureStoreOpen())
                    return false;
            } else if (error.isError()) {
                QMessageBox::critical(this, QStringLiteral("无法打开账户"),
                                      error.userMessage);
                return false;
            }
        } else {
            const QFileInfo target(path);
            if (!QDir().mkpath(target.absolutePath())) {
                QMessageBox::critical(this, QStringLiteral("无法建立账户文件"),
                                      QStringLiteral("无法创建目录：%1")
                                          .arg(target.absolutePath()));
                return false;
            }
            error = m_vaultDock->createManagedStoreForIdentity(
                path, m_securitySession->parameters(),
                lease.masterUnlockKey(), lease.recoverySecret());
            if (error.isError()) {
                QMessageBox::critical(this, QStringLiteral("无法建立账户文件"),
                                      error.userMessage);
                return false;
            }
        }
        lease.clear();
    }
    if (m_workspaceStack && m_accountsPage) {
        if (m_workspaceStack->currentWidget() != m_accountsPage) {
            if (m_workspaceStack->currentWidget() == m_tabs)
                m_searchDockWasVisibleBeforeWorkspace = m_searchDock && m_searchDock->isVisible();
            if (m_searchDock)
                m_searchDock->hide();
        }
        m_workspaceStack->setCurrentWidget(m_accountsPage);
    }
    syncCommandStates(nullptr);
    if (m_findDialog)
        m_findDialog->setActiveTab(nullptr);
    showWorkspaceStatus(QStringLiteral("账户"));
    return true;
}

bool MainWindow::showSecurityCenterWorkspace()
{
    ensureSecurityServicesCreated();
    if (!m_securitySession || !m_securitySession->available()) {
        QMessageBox::critical(this, QStringLiteral("安全功能不可用"),
                              QStringLiteral("当前构建未包含 KDBX 安全后端。"));
        return false;
    }
    ensureSecurityCenterCreated();
    if (!m_securityCenterPage || !ensureSecuritySessionInteractive())
        return false;
    if (m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage
        && m_vaultDock && !m_vaultDock->releaseWorkspaceSecrets()) {
        return false;
    }
    if (m_workspaceStack) {
        if (m_workspaceStack->currentWidget() == m_tabs)
            m_searchDockWasVisibleBeforeWorkspace = m_searchDock && m_searchDock->isVisible();
        if (m_searchDock)
            m_searchDock->hide();
        m_workspaceStack->setCurrentWidget(m_securityCenterPage);
    }
    refreshSecurityCenter();
    syncCommandStates(nullptr);
    if (m_findDialog)
        m_findDialog->setActiveTab(nullptr);
    showWorkspaceStatus(QStringLiteral("安全中心"));
    return true;
}

void MainWindow::refreshSecurityCenter()
{
    const bool backendAvailable = m_securitySession && m_securitySession->available();
    const bool hasIdentity = backendAvailable && m_securitySession->hasIdentity();
    const bool active = hasIdentity && m_securitySession->hasActiveSession();
    if (m_securitySessionStatus && !backendAvailable) {
        m_securitySessionStatus->setText(
            QStringLiteral("当前构建未包含 KDBX 安全后端。"));
    } else if (m_securitySessionStatus) {
        m_securitySessionStatus->setText(
            active
                ? QStringLiteral("已解锁\n本设备会话有效至：%1")
                      .arg(m_securitySession->expiresAtUtc().toLocalTime()
                               .toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                : QStringLiteral("已锁定"));
    }
    if (m_securityIdentityStatus) {
        if (!hasIdentity) {
            m_securityIdentityStatus->setText(QStringLiteral("尚未建立本地安全身份。"));
        } else {
            QString identity = m_securitySession->identityId();
            if (identity.size() > 16)
                identity = identity.left(16) + QChar(0x2026);
            QString identityText = QStringLiteral("本地身份：%1\n密钥版本：v%2")
                                       .arg(identity)
                                       .arg(m_securitySession->keyVersion());
            if (m_securitySession->issuedAtUtc().isValid()) {
                identityText += QStringLiteral("\n本次会话建立于：%1")
                                    .arg(m_securitySession->issuedAtUtc().toLocalTime()
                                             .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            }
            m_securityIdentityStatus->setText(identityText);
        }
    }
    if (m_securityRecoveryStatus) {
        QString recoveryText = QStringLiteral(
            "恢复密钥用于应用数据损坏、设备迁移或忘记主密码时解锁文件。请离线保存。");
        if (hasIdentity && m_securitySession->recoveryVerifiedAtUtc().isValid()) {
            recoveryText += QStringLiteral("\n上次验证：%1")
                                .arg(m_securitySession->recoveryVerifiedAtUtc()
                                         .toLocalTime()
                                         .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        } else if (hasIdentity) {
            recoveryText += QStringLiteral("\n尚未验证独立保存的恢复密钥。");
        }
        m_securityRecoveryStatus->setText(recoveryText);
    }
    if (m_securityCenterPage) {
        if (auto *button = m_securityCenterPage->findChild<QPushButton *>(
                QStringLiteral("securityCenterRecoveryButton"))) {
            button->setEnabled(active);
        }
        if (auto *button = m_securityCenterPage->findChild<QPushButton *>(
                QStringLiteral("securityCenterLockNow"))) {
            button->setEnabled(active);
        }
        if (auto *button = m_securityCenterPage->findChild<QPushButton *>(
                QStringLiteral("securityCenterChangePassword"))) {
            button->setEnabled(active);
        }
        if (auto *button = m_securityCenterPage->findChild<QPushButton *>(
                QStringLiteral("securityCenterVerifyRecovery"))) {
            button->setEnabled(active);
        }
    }
}

void MainWindow::changeMasterPassword()
{
    if (!m_securitySession || !m_securitySession->available()
        || !m_securitySession->hasActiveSession())
        return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("securityCenterChangePasswordDialog"));
    dialog.setWindowTitle(QStringLiteral("修改主密码"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        QStringLiteral("新建文件将使用新的密钥版本。已有文件不会被批量改写，"
                       "并仍可使用原密码或恢复密钥解锁。"),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    auto *currentPassword = new QLineEdit(&dialog);
    currentPassword->setObjectName(QStringLiteral("changePasswordCurrent"));
    currentPassword->setEchoMode(QLineEdit::Password);
    currentPassword->setClearButtonEnabled(true);
    auto *newPassword = new QLineEdit(&dialog);
    newPassword->setObjectName(QStringLiteral("changePasswordNew"));
    newPassword->setEchoMode(QLineEdit::Password);
    newPassword->setClearButtonEnabled(true);
    auto *confirmation = new QLineEdit(&dialog);
    confirmation->setObjectName(QStringLiteral("changePasswordConfirmation"));
    confirmation->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("当前主密码"), currentPassword);
    form->addRow(QStringLiteral("新主密码"), newPassword);
    form->addRow(QStringLiteral("确认新主密码"), confirmation);
    layout->addLayout(form);

    auto *showInput = new QCheckBox(QStringLiteral("显示输入"), &dialog);
    showInput->setObjectName(QStringLiteral("changePasswordShowInput"));
    layout->addWidget(showInput);
    connect(showInput, &QCheckBox::toggled, &dialog,
            [currentPassword, newPassword, confirmation](bool visible) {
        const auto mode = visible ? QLineEdit::Normal : QLineEdit::Password;
        currentPassword->setEchoMode(mode);
        newPassword->setEchoMode(mode);
        confirmation->setEchoMode(mode);
    });

    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setObjectName(QStringLiteral("changePasswordError"));
    errorLabel->setStyleSheet(QStringLiteral("color:#b00020"));
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                             | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("修改主密码"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    currentPassword->setFocus();

    while (dialog.exec() == QDialog::Accepted) {
        QString currentText = takeSensitiveLineEdit(currentPassword);
        QString replacementText = takeSensitiveLineEdit(newPassword);
        QString confirmationText = takeSensitiveLineEdit(confirmation);
        if (replacementText.size() < 12) {
            errorLabel->setText(QStringLiteral("新主密码至少需要 12 个字符。"));
            newPassword->setFocus();
            currentText.fill(QChar(0));
            replacementText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            continue;
        }
        if (replacementText != confirmationText) {
            errorLabel->setText(QStringLiteral("两次输入的新主密码不一致。"));
            newPassword->setFocus();
            currentText.fill(QChar(0));
            replacementText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            continue;
        }
        if (currentText == replacementText) {
            errorLabel->setText(QStringLiteral("新主密码必须与当前主密码不同。"));
            newPassword->setFocus();
            currentText.fill(QChar(0));
            replacementText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            continue;
        }

        QByteArray currentUtf8 = currentText.toUtf8();
        QByteArray replacementUtf8 = replacementText.toUtf8();
        currentText.fill(QChar(0));
        replacementText.fill(QChar(0));
        confirmationText.fill(QChar(0));
        SecureBuffer currentSecret(currentUtf8.constData(), currentUtf8.size());
        SecureBuffer replacementSecret(replacementUtf8.constData(),
                                       replacementUtf8.size());
        scrubByteArray(currentUtf8);
        scrubByteArray(replacementUtf8);
        const VaultError result = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
            QStringLiteral("正在更新安全身份"),
            [this, &currentSecret, &replacementSecret] {
                return m_securitySession->changeMasterPassword(
                    currentSecret, replacementSecret);
            });
        currentSecret.clear();
        replacementSecret.clear();
        if (result.isError()) {
            errorLabel->setText(result.userMessage);
            currentPassword->setFocus();
            continue;
        }

        refreshSecurityCenter();
        statusBar()->showMessage(
            QStringLiteral("主密码已更改；新建文件将使用密钥版本 v%1，已有文件保持原版本。")
                .arg(m_securitySession->keyVersion()),
            8000);
        return;
    }
    clearSensitiveLineEdit(currentPassword);
    clearSensitiveLineEdit(newPassword);
    clearSensitiveLineEdit(confirmation);
}

void MainWindow::exportRecoveryKey()
{
    if (!m_securitySession || !m_securitySession->available()
        || !m_securitySession->hasActiveSession())
        return;
    QString recoveryKey;
    const VaultError exportError = m_securitySession->exportRecoveryKey(&recoveryKey);
    if (exportError.isError()) {
        QMessageBox::critical(this, QStringLiteral("无法读取恢复密钥"),
                              exportError.userMessage);
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("securityCenterRecoveryDialog"));
    dialog.setWindowTitle(QStringLiteral("恢复密钥"));
    dialog.setMinimumWidth(620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *warning = new QLabel(
        QStringLiteral("拥有此密钥即可解锁你的加密文件。请仅保存到可信的离线位置，不要通过聊天或邮件发送。"),
        &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto *keyView = new QPlainTextEdit(recoveryKey, &dialog);
    keyView->setObjectName(QStringLiteral("securityCenterRecoveryKey"));
    keyView->setReadOnly(true);
    keyView->setMaximumHeight(96);
    layout->addWidget(keyView);
    auto *buttons = new QDialogButtonBox(&dialog);
    auto *copyButton = buttons->addButton(QStringLiteral("复制（60 秒后清除）"),
                                           QDialogButtonBox::ActionRole);
    copyButton->setObjectName(QStringLiteral("securityCenterRecoveryCopy"));
    auto *saveButton = buttons->addButton(QStringLiteral("保存到文件…"),
                                           QDialogButtonBox::ActionRole);
    saveButton->setObjectName(QStringLiteral("securityCenterRecoverySave"));
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);
    closeButton->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(copyButton, &QPushButton::clicked, &dialog, [this, &recoveryKey]() {
        QByteArray keyBytes = recoveryKey.toUtf8();
        const QByteArray fingerprint = QCryptographicHash::hash(
            keyBytes, QCryptographicHash::Sha256);
        QApplication::clipboard()->setText(recoveryKey);
        scrubByteArray(keyBytes);
        QTimer::singleShot(60 * 1000, this, [fingerprint]() {
            QClipboard *clipboard = QApplication::clipboard();
            QByteArray current = clipboard->text().toUtf8();
            const QByteArray currentFingerprint = QCryptographicHash::hash(
                current, QCryptographicHash::Sha256);
            scrubByteArray(current);
            if (currentFingerprint == fingerprint)
                clipboard->clear();
        });
        statusBar()->showMessage(QStringLiteral("恢复密钥已复制，将在 60 秒后从剪贴板清除"),
                                 5000);
    });
    connect(saveButton, &QPushButton::clicked, &dialog, [this, &dialog, &recoveryKey]() {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QStringLiteral("保存恢复密钥"),
            QStringLiteral("notepad-safe-recovery-key.txt"),
            QStringLiteral("文本文件 (*.txt);;所有文件 (*.*)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(&dialog, QStringLiteral("无法保存恢复密钥"),
                                  file.errorString());
            return;
        }
        QByteArray bytes = recoveryKey.toUtf8();
        const qint64 written = file.write(bytes);
        const qsizetype expected = bytes.size();
        scrubByteArray(bytes);
        if (written != expected || !file.commit()) {
            QMessageBox::critical(&dialog, QStringLiteral("无法保存恢复密钥"),
                                  file.errorString());
            return;
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        statusBar()->showMessage(QStringLiteral("恢复密钥已保存"), 3000);
    });
    dialog.exec();
    keyView->clear();
    if (!recoveryKey.isEmpty())
        recoveryKey.fill(QChar(0));
    recoveryKey.clear();
}

void MainWindow::verifyRecoveryKey()
{
    if (!m_securitySession || !m_securitySession->available()
        || !m_securitySession->hasActiveSession())
        return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("securityCenterVerifyRecoveryDialog"));
    dialog.setWindowTitle(QStringLiteral("验证恢复密钥"));
    dialog.setMinimumWidth(620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(
        QStringLiteral("请从独立保存的位置粘贴完整恢复密钥。验证只确认它属于当前本地身份，"
                       "不会改写任何加密文件。"),
        &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *input = new QLineEdit(&dialog);
    input->setObjectName(QStringLiteral("securityCenterRecoveryVerificationInput"));
    input->setEchoMode(QLineEdit::Password);
    input->setClearButtonEnabled(true);
    input->setPlaceholderText(QStringLiteral("NSR1-…"));
    layout->addWidget(input);
    auto *showInput = new QCheckBox(QStringLiteral("显示输入"), &dialog);
    showInput->setObjectName(QStringLiteral("securityCenterRecoveryVerificationShowInput"));
    layout->addWidget(showInput);
    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setObjectName(QStringLiteral("securityCenterRecoveryVerificationError"));
    errorLabel->setStyleSheet(QStringLiteral("color:#b00020"));
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                             | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("验证"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(showInput, &QCheckBox::toggled, input, [input](bool shown) {
        input->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    input->setFocus();

    while (dialog.exec() == QDialog::Accepted) {
        QString rawRecoveryText = takeSensitiveLineEdit(input);
        QString recoveryText = rawRecoveryText.trimmed();
        if (!rawRecoveryText.isEmpty())
            rawRecoveryText.fill(QChar(0));
        rawRecoveryText.clear();
        VaultError result = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
            QStringLiteral("正在验证恢复密钥"),
            [this, &recoveryText] {
                return m_securitySession->verifyRecoveryKey(recoveryText);
            });
        if (!recoveryText.isEmpty())
            recoveryText.fill(QChar(0));
        recoveryText.clear();
        if (result.isError()) {
            errorLabel->setText(result.userMessage);
            input->setFocus();
            continue;
        }

        refreshSecurityCenter();
        statusBar()->showMessage(QStringLiteral("恢复密钥验证成功"), 5000);
        return;
    }
    clearSensitiveLineEdit(input);
}

// ============ 创建 Actions / Menus / Toolbar / StatusBar ============

void MainWindow::createActions()
{
    // Actions 在 createMenus 中创建
}

void MainWindow::createMenus()
{
    // 文件菜单
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->menuAction()->setObjectName(QStringLiteral("menu.file"));

    auto *newAct = fileMenu->addAction(QStringLiteral("新建(&N)"));
    newAct->setObjectName(QStringLiteral("file.new"));
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, &MainWindow::newFile);

    auto *openAct = fileMenu->addAction(QStringLiteral("打开(&O)..."));
    openAct->setObjectName(QStringLiteral("file.open"));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开(&R)"));
    m_recentMenu->setObjectName(QStringLiteral("file.recent"));

    m_actReload = fileMenu->addAction(QStringLiteral("从磁盘重新加载(&L)"));
    m_actReload->setObjectName(QStringLiteral("file.reload"));
    m_actReload->setEnabled(false);
    connect(m_actReload, &QAction::triggered, this, &MainWindow::reloadFile);

    fileMenu->addSeparator();

    m_saveAction = fileMenu->addAction(QStringLiteral("保存(&S)"));
    m_saveAction->setObjectName(QStringLiteral("file.save"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveFile);

    m_saveAsAction = fileMenu->addAction(QStringLiteral("另存为(&A)..."));
    m_saveAsAction->setObjectName(QStringLiteral("file.saveAs"));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveAsFile);

    fileMenu->addSeparator();

    m_actCloseTab = fileMenu->addAction(QStringLiteral("关闭标签(&C)"));
    m_actCloseTab->setObjectName(QStringLiteral("file.close"));
    m_actCloseTab->setEnabled(false);
    m_actCloseTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(m_actCloseTab, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    m_actCloseAll = fileMenu->addAction(QStringLiteral("关闭所有标签"));
    m_actCloseAll->setObjectName(QStringLiteral("file.closeAll"));
    m_actCloseAll->setEnabled(false);
    connect(m_actCloseAll, &QAction::triggered, this, &MainWindow::closeAllTabs);

    fileMenu->addSeparator();

    auto *exitAct = fileMenu->addAction(QStringLiteral("退出(&X)"));
    exitAct->setObjectName(QStringLiteral("file.exit"));
    exitAct->setShortcut(QKeySequence::Quit);
    connect(exitAct, &QAction::triggered, this, &QMainWindow::close);

    rebuildRecentFilesMenu();

    // 编辑菜单
    auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    editMenu->menuAction()->setObjectName(QStringLiteral("menu.edit"));
    m_documentOnlyActions.append(editMenu->menuAction());

    auto *undoAct = editMenu->addAction(QStringLiteral("撤销(&U)"));
    m_undoAction = undoAct;
    undoAct->setObjectName(QStringLiteral("edit.undo"));
    m_documentOnlyActions.append(undoAct);
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->undo();
    });

    auto *redoAct = editMenu->addAction(QStringLiteral("重做(&R)"));
    m_redoAction = redoAct;
    redoAct->setObjectName(QStringLiteral("edit.redo"));
    m_documentOnlyActions.append(redoAct);
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->redo();
    });

    editMenu->addSeparator();

    auto *cutAct = editMenu->addAction(QStringLiteral("剪切(&T)"));
    m_cutAction = cutAct;
    cutAct->setObjectName(QStringLiteral("edit.cut"));
    m_documentOnlyActions.append(cutAct);
    cutAct->setShortcut(QKeySequence::Cut);
    connect(cutAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) {
            const bool hadSelection = t->hasSelectionCommand();
            t->cut();
            if (hadSelection)
                rememberSensitiveDocumentClipboard(t);
        }
    });

    auto *copyAct = editMenu->addAction(QStringLiteral("复制(&C)"));
    m_copyAction = copyAct;
    copyAct->setObjectName(QStringLiteral("edit.copy"));
    m_documentOnlyActions.append(copyAct);
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) {
            if (t->hasSelectionCommand()) {
                t->copy();
                rememberSensitiveDocumentClipboard(t);
            }
        }
    });

    auto *pasteAct = editMenu->addAction(QStringLiteral("粘贴(&P)"));
    m_pasteAction = pasteAct;
    pasteAct->setObjectName(QStringLiteral("edit.paste"));
    m_documentOnlyActions.append(pasteAct);
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->paste();
    });

    auto *selectAllAct = editMenu->addAction(QStringLiteral("全选(&A)"));
    m_selectAllAction = selectAllAct;
    selectAllAct->setObjectName(QStringLiteral("edit.selectAll"));
    m_documentOnlyActions.append(selectAllAct);
    selectAllAct->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->selectAll();
    });

    m_hexInsertModeAction = editMenu->addAction(
        QStringLiteral("十六进制插入模式(&I)"));
    m_hexInsertModeAction->setObjectName(QStringLiteral("edit.hexInsertMode"));
    m_hexInsertModeAction->setCheckable(true);
    m_hexInsertModeAction->setStatusTip(
        QStringLiteral("在十六进制视图中插入字节；也可按 Insert 或单击状态栏 INS/OVR 切换"));
    m_documentOnlyActions.append(m_hexInsertModeAction);
    connect(m_hexInsertModeAction, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->toggleHexOverwriteMode();
    });

    editMenu->addSeparator();

    auto *dupLineAct = editMenu->addAction(QStringLiteral("复制当前行(&D)"));
    dupLineAct->setObjectName(QStringLiteral("edit.line.duplicate"));
    m_documentOnlyActions.append(dupLineAct);
    m_textMutationActions.append(dupLineAct);
    dupLineAct->setShortcut(QKeySequence("Ctrl+Shift+D"));
    connect(dupLineAct, &QAction::triggered, this, &MainWindow::duplicateLine);

    auto *delLineAct = editMenu->addAction(QStringLiteral("删除当前行(&L)"));
    delLineAct->setObjectName(QStringLiteral("edit.line.delete"));
    m_documentOnlyActions.append(delLineAct);
    m_textMutationActions.append(delLineAct);
    delLineAct->setShortcut(QKeySequence("Ctrl+Shift+L"));
    connect(delLineAct, &QAction::triggered, this, &MainWindow::deleteLine);

    auto *moveUpAct = editMenu->addAction(QStringLiteral("上移当前行"));
    moveUpAct->setObjectName(QStringLiteral("edit.line.moveUp"));
    m_documentOnlyActions.append(moveUpAct);
    m_textMutationActions.append(moveUpAct);
    moveUpAct->setShortcut(QKeySequence("Alt+Up"));
    connect(moveUpAct, &QAction::triggered, this, &MainWindow::moveLineUp);

    auto *moveDownAct = editMenu->addAction(QStringLiteral("下移当前行"));
    moveDownAct->setObjectName(QStringLiteral("edit.line.moveDown"));
    m_documentOnlyActions.append(moveDownAct);
    m_textMutationActions.append(moveDownAct);
    moveDownAct->setShortcut(QKeySequence("Alt+Down"));
    connect(moveDownAct, &QAction::triggered, this, &MainWindow::moveLineDown);

    auto addNativeTextAction = [this, editMenu](const QString &text,
                                                 const QString &objectName,
                                                 const QKeySequence &shortcut,
                                                 auto callback) {
        auto *action = editMenu->addAction(text);
        action->setObjectName(objectName);
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        m_documentOnlyActions.append(action);
        m_textMutationActions.append(action);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };

    addNativeTextAction(QStringLiteral("转置当前行"),
                        QStringLiteral("edit.line.transpose"), {}, [this]() {
        if (auto *tab = currentTab()) tab->transposeLine();
    });

    editMenu->addSeparator();
    addNativeTextAction(QStringLiteral("选择下一个相同内容"),
                        QStringLiteral("edit.multiSelect.next"),
                        QKeySequence(QStringLiteral("Ctrl+D")), [this]() {
        if (auto *tab = currentTab()) tab->selectNextOccurrence();
    });
    addNativeTextAction(QStringLiteral("选择所有相同内容"),
                        QStringLiteral("edit.multiSelect.all"),
                        QKeySequence(QStringLiteral("Ctrl+Shift+Alt+D")), [this]() {
        if (auto *tab = currentTab()) tab->selectAllOccurrences();
    });
    addNativeTextAction(QStringLiteral("选区转换为大写"),
                        QStringLiteral("edit.selection.upper"), {}, [this]() {
        if (auto *tab = currentTab()) tab->selectionToUpper();
    });
    addNativeTextAction(QStringLiteral("选区转换为小写"),
                        QStringLiteral("edit.selection.lower"), {}, [this]() {
        if (auto *tab = currentTab()) tab->selectionToLower();
    });
    addNativeTextAction(QStringLiteral("合并选中行"),
                        QStringLiteral("edit.lines.join"), {}, [this]() {
        if (auto *tab = currentTab()) tab->joinSelectedLines();
    });
    addNativeTextAction(QStringLiteral("按窗口宽度拆分选中行"),
                        QStringLiteral("edit.lines.split"), {}, [this]() {
        if (auto *tab = currentTab()) tab->splitSelectedLines();
    });

    editMenu->addSeparator();

    auto *sortAscAct = editMenu->addAction(QStringLiteral("行升序排序"));
    m_actSortAsc = sortAscAct;
    sortAscAct->setObjectName(QStringLiteral("edit.lines.sortAscending"));
    m_documentOnlyActions.append(sortAscAct);
    connect(sortAscAct, &QAction::triggered, this, &MainWindow::sortLinesAsc);

    auto *sortDescAct = editMenu->addAction(QStringLiteral("行降序排序"));
    m_actSortDesc = sortDescAct;
    sortDescAct->setObjectName(QStringLiteral("edit.lines.sortDescending"));
    m_documentOnlyActions.append(sortDescAct);
    connect(sortDescAct, &QAction::triggered, this, &MainWindow::sortLinesDesc);

    editMenu->addSeparator();
    auto *indentMenu = editMenu->addMenu(QStringLiteral("缩进与补全"));
    indentMenu->setObjectName(QStringLiteral("edit.indentation"));
    m_documentOnlyActions.append(indentMenu->menuAction());

    m_actUseTabs = indentMenu->addAction(QStringLiteral("使用 Tab 缩进"));
    m_actUseTabs->setObjectName(QStringLiteral("edit.indentation.useTabs"));
    m_actUseTabs->setCheckable(true);
    m_documentOnlyActions.append(m_actUseTabs);
    connect(m_actUseTabs, &QAction::toggled, this, [this](bool enabled) {
        m_useTabs = enabled;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setUseTabs(enabled);
        }
        saveSettings();
    });

    auto *tabWidthMenu = indentMenu->addMenu(QStringLiteral("Tab 宽度"));
    tabWidthMenu->setObjectName(QStringLiteral("edit.indentation.tabWidth"));
    m_tabWidthGroup = new QActionGroup(this);
    m_tabWidthGroup->setExclusive(true);
    for (const int width : {2, 4, 8}) {
        auto *action = tabWidthMenu->addAction(QStringLiteral("%1 个字符").arg(width));
        action->setObjectName(QStringLiteral("edit.indentation.tabWidth.%1").arg(width));
        action->setCheckable(true);
        action->setData(width);
        m_tabWidthGroup->addAction(action);
        m_documentOnlyActions.append(action);
    }
    connect(m_tabWidthGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        m_tabWidth = action->data().toInt();
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setTabWidth(m_tabWidth);
        }
        saveSettings();
    });

    m_actBackspaceUnindents = indentMenu->addAction(QStringLiteral("Backspace 回退缩进"));
    m_actBackspaceUnindents->setObjectName(
        QStringLiteral("edit.indentation.backspaceUnindents"));
    m_actBackspaceUnindents->setCheckable(true);
    m_documentOnlyActions.append(m_actBackspaceUnindents);
    connect(m_actBackspaceUnindents, &QAction::toggled, this, [this](bool enabled) {
        m_backspaceUnindents = enabled;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setBackspaceUnindents(enabled);
        }
        saveSettings();
    });

    m_actWordCompletion = indentMenu->addAction(QStringLiteral("文档词语自动补全"));
    m_actWordCompletion->setObjectName(QStringLiteral("edit.completion.words"));
    m_actWordCompletion->setCheckable(true);
    m_documentOnlyActions.append(m_actWordCompletion);
    connect(m_actWordCompletion, &QAction::toggled, this, [this](bool enabled) {
        m_wordCompletion = enabled;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setWordCompletionEnabled(enabled);
        }
        saveSettings();
    });

    // 搜索菜单
    auto *searchMenu = menuBar()->addMenu(QStringLiteral("搜索(&S)"));
    searchMenu->menuAction()->setObjectName(QStringLiteral("menu.search"));

    m_findAction = searchMenu->addAction(QStringLiteral("查找(&F)..."));
    m_findAction->setObjectName(QStringLiteral("search.find"));
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, &MainWindow::showFindDialog);

    m_replaceAction = searchMenu->addAction(QStringLiteral("替换(&R)..."));
    m_replaceAction->setObjectName(QStringLiteral("search.replace"));
    m_documentOnlyActions.append(m_replaceAction);
    // 用户强制：菜单显示 (&R) → 全局快捷键必须为 Ctrl+R 显示一致（Notepad++ 习惯），禁止 Ctrl+H
    // 见 QA SR02-COMFORT 3 条硬断言：text 含 &R + shortcut==QKeySequence("Ctrl+R") 精确 + Alt+S+R 访问键可打开
    m_replaceAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(m_replaceAction, &QAction::triggered, this, &MainWindow::showReplaceDialog);

    searchMenu->addSeparator();

    auto *findNextAct = searchMenu->addAction(QStringLiteral("查找下一个"));
    findNextAct->setObjectName(QStringLiteral("search.next"));
    m_documentOnlyActions.append(findNextAct);
    findNextAct->setShortcut(QKeySequence("F3"));
    connect(findNextAct, &QAction::triggered, this, &MainWindow::findNext);

    auto *findPrevAct = searchMenu->addAction(QStringLiteral("查找上一个"));
    findPrevAct->setObjectName(QStringLiteral("search.previous"));
    m_documentOnlyActions.append(findPrevAct);
    findPrevAct->setShortcut(QKeySequence("Shift+F3"));
    connect(findPrevAct, &QAction::triggered, this, &MainWindow::findPrev);

    searchMenu->addSeparator();
    auto *matchingBraceAct = searchMenu->addAction(QStringLiteral("跳转到匹配括号"));
    matchingBraceAct->setObjectName(QStringLiteral("search.brace.match"));
    matchingBraceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+]")));
    m_documentOnlyActions.append(matchingBraceAct);
    m_nativeTextActions.append(matchingBraceAct);
    connect(matchingBraceAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab()) tab->moveToMatchingBrace();
    });
    auto *selectBraceAct = searchMenu->addAction(QStringLiteral("选择到匹配括号"));
    selectBraceAct->setObjectName(QStringLiteral("search.brace.select"));
    selectBraceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+]")));
    m_documentOnlyActions.append(selectBraceAct);
    m_nativeTextActions.append(selectBraceAct);
    connect(selectBraceAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab()) tab->selectToMatchingBrace();
    });

    // 语言菜单
    auto *syntaxMenu = menuBar()->addMenu(QStringLiteral("语言(&L)"));
    syntaxMenu->menuAction()->setObjectName(QStringLiteral("menu.language"));
    m_languageActions.append(syntaxMenu->menuAction());
    auto *syntaxGroup = new QActionGroup(this);
    const QStringList syntaxNames = {
        QStringLiteral("纯文本"), QStringLiteral("C/C++"),
        QStringLiteral("Python"), QStringLiteral("JSON"),
        QStringLiteral("HTML"), QStringLiteral("XML"),
        QStringLiteral("SQL"), QStringLiteral("YAML"),
        QStringLiteral("Markdown"), QStringLiteral("JavaScript"),
        QStringLiteral("Java"), QStringLiteral("C#"),
        QStringLiteral("CSS"), QStringLiteral("Shell"),
        QStringLiteral("Batch"), QStringLiteral("CMake"),
        QStringLiteral("Makefile"), QStringLiteral("Lua"),
        QStringLiteral("Ruby"), QStringLiteral("Perl"),
        QStringLiteral("Diff/Patch"), QStringLiteral("Properties/INI"),
        QStringLiteral("Assembly"),
    };
    const QStringList syntaxIds = {
        QStringLiteral("plain"), QStringLiteral("cpp"),
        QStringLiteral("python"), QStringLiteral("json"),
        QStringLiteral("html"), QStringLiteral("xml"),
        QStringLiteral("sql"), QStringLiteral("yaml"),
        QStringLiteral("markdown"), QStringLiteral("javascript"),
        QStringLiteral("java"), QStringLiteral("csharp"),
        QStringLiteral("css"), QStringLiteral("bash"),
        QStringLiteral("batch"), QStringLiteral("cmake"),
        QStringLiteral("makefile"), QStringLiteral("lua"),
        QStringLiteral("ruby"), QStringLiteral("perl"),
        QStringLiteral("diff"), QStringLiteral("properties"),
        QStringLiteral("asm"),
    };
    for (int i = 0; i < syntaxNames.size(); ++i) {
        auto *act = syntaxMenu->addAction(syntaxNames[i]);
        act->setObjectName(QStringLiteral("language.%1").arg(syntaxIds[i]));
        act->setCheckable(true);
        act->setData(i);
        syntaxGroup->addAction(act);
        m_languageActions.append(act);
        if (i == 0) act->setChecked(true);
        connect(act, &QAction::triggered, this, &MainWindow::setSyntaxFromAction);
    }

    // 书签属于搜索导航，不再占用独立顶级菜单。
    auto *bookmarkMenu = searchMenu->addMenu(QStringLiteral("书签(&B)"));
    bookmarkMenu->setObjectName(QStringLiteral("search.bookmarks"));
    m_documentOnlyActions.append(bookmarkMenu->menuAction());

    auto *toggleBmAct = bookmarkMenu->addAction(QStringLiteral("切换书签(&M)"));
    toggleBmAct->setObjectName(QStringLiteral("bookmark.toggle"));
    m_documentOnlyActions.append(toggleBmAct);
    toggleBmAct->setShortcut(QKeySequence("Ctrl+F2"));
    connect(toggleBmAct, &QAction::triggered, this, &MainWindow::toggleBookmark);

    auto *nextBmAct = bookmarkMenu->addAction(QStringLiteral("下一书签"));
    nextBmAct->setObjectName(QStringLiteral("bookmark.next"));
    m_documentOnlyActions.append(nextBmAct);
    nextBmAct->setShortcut(QKeySequence("F2"));
    connect(nextBmAct, &QAction::triggered, this, &MainWindow::nextBookmark);


    auto *prevBmAct = bookmarkMenu->addAction(QStringLiteral("上一书签"));
    prevBmAct->setObjectName(QStringLiteral("bookmark.previous"));
    m_documentOnlyActions.append(prevBmAct);
    prevBmAct->setShortcut(QKeySequence("Shift+F2"));
    connect(prevBmAct, &QAction::triggered, this, &MainWindow::prevBookmark);

    bookmarkMenu->addSeparator();

    auto *clearBmAct = bookmarkMenu->addAction(QStringLiteral("清除所有书签"));
    clearBmAct->setObjectName(QStringLiteral("bookmark.clear"));
    m_documentOnlyActions.append(clearBmAct);

    connect(clearBmAct, &QAction::triggered, this, &MainWindow::clearBookmarks);

    // 视图菜单
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->menuAction()->setObjectName(QStringLiteral("menu.view"));

    auto *appearanceMenu = viewMenu->addMenu(QStringLiteral("外观"));
    appearanceMenu->setObjectName(QStringLiteral("view.appearance"));
    m_appearanceGroup = new QActionGroup(this);
    m_appearanceGroup->setExclusive(true);
    m_actAppearanceSystem = appearanceMenu->addAction(QStringLiteral("跟随系统"));
    m_actAppearanceSystem->setObjectName(QStringLiteral("view.appearance.system"));
    m_actAppearanceSystem->setCheckable(true);
    m_actAppearanceSystem->setData(static_cast<int>(AppearanceMode::System));
    m_appearanceGroup->addAction(m_actAppearanceSystem);
    m_actAppearanceLight = appearanceMenu->addAction(QStringLiteral("浅色"));
    m_actAppearanceLight->setObjectName(QStringLiteral("view.appearance.light"));
    m_actAppearanceLight->setCheckable(true);
    m_actAppearanceLight->setData(static_cast<int>(AppearanceMode::Light));
    m_appearanceGroup->addAction(m_actAppearanceLight);
    m_actDarkMode = appearanceMenu->addAction(QStringLiteral("深色"));
    m_actDarkMode->setObjectName(QStringLiteral("view.appearance.dark"));
    m_actDarkMode->setCheckable(true);
    m_actDarkMode->setData(static_cast<int>(AppearanceMode::Dark));
    m_appearanceGroup->addAction(m_actDarkMode);
    connect(m_appearanceGroup, &QActionGroup::triggered,
            this, &MainWindow::setAppearanceModeFromAction);

    viewMenu->addSeparator();

    m_actWordWrap = viewMenu->addAction(QStringLiteral("自动换行"));
    m_actWordWrap->setObjectName(QStringLiteral("view.wordWrap"));
    m_documentOnlyActions.append(m_actWordWrap);
    m_actWordWrap->setCheckable(true);
    m_actWordWrap->setChecked(m_wordWrap);
    connect(m_actWordWrap, &QAction::triggered, this, &MainWindow::toggleWordWrap);

    auto *splitMenu = viewMenu->addMenu(QStringLiteral("分屏"));
    splitMenu->setObjectName(QStringLiteral("view.split"));
    m_documentOnlyActions.append(splitMenu->menuAction());
    m_actSplitHorizontal = splitMenu->addAction(QStringLiteral("左右分屏"));
    m_actSplitHorizontal->setObjectName(QStringLiteral("view.split.horizontal"));
    m_actSplitHorizontal->setCheckable(true);
    m_documentOnlyActions.append(m_actSplitHorizontal);
    m_actSplitVertical = splitMenu->addAction(QStringLiteral("上下分屏"));
    m_actSplitVertical->setObjectName(QStringLiteral("view.split.vertical"));
    m_actSplitVertical->setCheckable(true);
    m_documentOnlyActions.append(m_actSplitVertical);
    connect(m_actSplitHorizontal, &QAction::toggled, this, [this](bool enabled) {
        EditorTab *tab = currentTab();
        if (!tab)
            return;
        if (enabled) {
            QSignalBlocker blocker(m_actSplitVertical);
            m_actSplitVertical->setChecked(false);
            tab->setSplitView(true, Qt::Horizontal);
        } else if (tab->splitViewEnabled()
                   && tab->splitOrientation() == Qt::Horizontal) {
            tab->setSplitView(false);
        }
        syncCommandStates(tab);
    });
    connect(m_actSplitVertical, &QAction::toggled, this, [this](bool enabled) {
        EditorTab *tab = currentTab();
        if (!tab)
            return;
        if (enabled) {
            QSignalBlocker blocker(m_actSplitHorizontal);
            m_actSplitHorizontal->setChecked(false);
            tab->setSplitView(true, Qt::Vertical);
        } else if (tab->splitViewEnabled()
                   && tab->splitOrientation() == Qt::Vertical) {
            tab->setSplitView(false);
        }
        syncCommandStates(tab);
    });

    auto *symbolsMenu = viewMenu->addMenu(QStringLiteral("显示符号"));
    symbolsMenu->setObjectName(QStringLiteral("view.symbols"));
    m_documentOnlyActions.append(symbolsMenu->menuAction());
    m_actWhitespaceVisible = symbolsMenu->addAction(QStringLiteral("显示空格与制表符"));
    m_actWhitespaceVisible->setObjectName(QStringLiteral("view.symbols.whitespace"));
    m_actWhitespaceVisible->setCheckable(true);
    m_documentOnlyActions.append(m_actWhitespaceVisible);
    connect(m_actWhitespaceVisible, &QAction::toggled, this, [this](bool visible) {
        m_whitespaceVisible = visible;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setWhitespaceVisible(visible);
        }
        saveSettings();
    });
    m_actEolVisible = symbolsMenu->addAction(QStringLiteral("显示行尾符"));
    m_actEolVisible->setObjectName(QStringLiteral("view.symbols.eol"));
    m_actEolVisible->setCheckable(true);
    m_documentOnlyActions.append(m_actEolVisible);
    connect(m_actEolVisible, &QAction::toggled, this, [this](bool visible) {
        m_eolVisible = visible;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setEolVisible(visible);
        }
        saveSettings();
    });

    auto *foldMenu = viewMenu->addMenu(QStringLiteral("代码折叠"));
    foldMenu->setObjectName(QStringLiteral("view.folding"));
    m_documentOnlyActions.append(foldMenu->menuAction());
    m_lexerActions.append(foldMenu->menuAction());
    auto addFoldAction = [this, foldMenu](const QString &text,
                                          const QString &name,
                                          auto callback) {
        auto *action = foldMenu->addAction(text);
        action->setObjectName(name);
        m_documentOnlyActions.append(action);
        m_lexerActions.append(action);
        connect(action, &QAction::triggered, this, callback);
    };
    addFoldAction(QStringLiteral("折叠/展开当前块"),
                  QStringLiteral("view.folding.toggleCurrent"), [this]() {
        if (auto *tab = currentTab()) tab->foldCurrentLine();
    });
    addFoldAction(QStringLiteral("全部折叠"),
                  QStringLiteral("view.folding.collapseAll"), [this]() {
        if (auto *tab = currentTab()) tab->foldAll(false);
    });
    addFoldAction(QStringLiteral("全部展开"),
                  QStringLiteral("view.folding.expandAll"), [this]() {
        if (auto *tab = currentTab()) tab->foldAll(true);
    });

    viewMenu->addSeparator();

    auto *presentationMenu = viewMenu->addMenu(QStringLiteral("呈现方式"));
    presentationMenu->setObjectName(QStringLiteral("view.presentation"));
    m_documentOnlyActions.append(presentationMenu->menuAction());
    auto *presentationGroup = new QActionGroup(this);
    presentationGroup->setExclusive(true);
    m_actTextViewMode = presentationMenu->addAction(QStringLiteral("文本"));
    m_actTextViewMode->setObjectName(QStringLiteral("view.presentation.text"));
    m_actTextViewMode->setCheckable(true);
    presentationGroup->addAction(m_actTextViewMode);
    m_actHexViewMode = presentationMenu->addAction(QStringLiteral("十六进制"));
    m_actHexViewMode->setObjectName(QStringLiteral("view.presentation.hex"));
    m_actHexViewMode->setCheckable(true);
    presentationGroup->addAction(m_actHexViewMode);
    connect(m_actTextViewMode, &QAction::triggered, this, [this]() {
        auto *tab = currentTab();
        if (!tab || !tab->isL2Active())
            return;
        tab->setViewModeL2(ViewMode::Text);
        syncCommandStates(tab);
        syncGlobalMenuCheckStates();
        updateStatusBar();
    });
    connect(m_actHexViewMode, &QAction::triggered, this, [this]() {
        auto *tab = currentTab();
        if (!tab || !tab->isL2Active())
            return;
        tab->setViewModeL2(ViewMode::Hex);
        syncCommandStates(tab);
        syncGlobalMenuCheckStates();
        updateStatusBar();
    });
    // The menu entries select a concrete presentation; the established
    // shortcut remains a true two-way toggle.
    auto *togglePresentation = new QAction(this);
    togglePresentation->setObjectName(QStringLiteral("view.presentation.toggle"));
    togglePresentation->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    togglePresentation->setShortcutContext(Qt::WindowShortcut);
    addAction(togglePresentation);
    m_documentOnlyActions.append(togglePresentation);
    connect(togglePresentation, &QAction::triggered,
            this, &MainWindow::toggleHexViewMode);

    auto *edgeMenu = viewMenu->addMenu(QStringLiteral("长行标尺"));
    edgeMenu->setObjectName(QStringLiteral("view.edge"));
    m_documentOnlyActions.append(edgeMenu->menuAction());
    m_actEdgeVisible = edgeMenu->addAction(QStringLiteral("显示标尺"));
    m_actEdgeVisible->setObjectName(QStringLiteral("view.edge.visible"));
    m_documentOnlyActions.append(m_actEdgeVisible);
    m_actEdgeVisible->setCheckable(true);
    m_actEdgeVisible->setChecked(m_edgeVisible);
    connect(m_actEdgeVisible, &QAction::triggered, this, &MainWindow::toggleEdgeLine);

    edgeMenu->addSeparator();
    m_edgeColGroup = new QActionGroup(this);
    QList<int> edgeCols = {72, 80, 100, 120, 150, 200};
    for (int col : edgeCols) {
        auto *a = edgeMenu->addAction(QStringLiteral("%1 列").arg(col));
        a->setObjectName(QStringLiteral("view.edge.column.%1").arg(col));
        a->setCheckable(true);
        a->setData(col);
        m_edgeColGroup->addAction(a);
        m_documentOnlyActions.append(a);
        if (col == m_edgeColumn) a->setChecked(true);
        connect(a, &QAction::triggered, this, &MainWindow::setEdgeColumnFromAction);
    }

    viewMenu->addSeparator();

    auto *zoomMenu = viewMenu->addMenu(QStringLiteral("缩放"));
    zoomMenu->setObjectName(QStringLiteral("view.zoom"));
    m_documentOnlyActions.append(zoomMenu->menuAction());
    auto *zoomInAct = zoomMenu->addAction(QStringLiteral("放大(&+)"));
    zoomInAct->setObjectName(QStringLiteral("view.zoom.in"));
    m_documentOnlyActions.append(zoomInAct);
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::zoomIn);

    auto *zoomOutAct = zoomMenu->addAction(QStringLiteral("缩小(&-)"));
    zoomOutAct->setObjectName(QStringLiteral("view.zoom.out"));
    m_documentOnlyActions.append(zoomOutAct);
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::zoomOut);

    auto *resetZoomAct = zoomMenu->addAction(QStringLiteral("重置缩放"));
    resetZoomAct->setObjectName(QStringLiteral("view.zoom.reset"));
    m_documentOnlyActions.append(resetZoomAct);
    resetZoomAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(resetZoomAct, &QAction::triggered, this, &MainWindow::resetZoom);

    viewMenu->addSeparator();
    auto *panelMenu = viewMenu->addMenu(QStringLiteral("面板"));
    panelMenu->setObjectName(QStringLiteral("view.panels"));
    m_searchPanelAction = panelMenu->addAction(QStringLiteral("搜索结果"));
    m_searchPanelAction->setObjectName(QStringLiteral("view.panel.searchResults"));
    m_searchPanelAction->setCheckable(true);
    connect(m_searchPanelAction, &QAction::toggled, this, [this](bool visible) {
        if (m_searchDock)
            m_searchDock->setVisible(visible);
    });
    connect(m_searchDock, &QDockWidget::visibilityChanged,
            m_searchPanelAction, &QAction::setChecked);
    m_tasksAction = panelMenu->addAction(QStringLiteral("后台任务(&T)"));
    m_tasksAction->setObjectName(QStringLiteral("tools.tasks"));
    connect(m_tasksAction, &QAction::triggered, this, [this]() {
        if (m_taskWidget)
            m_taskWidget->showPanel();
    });
    viewMenu->addSeparator();
    m_toolbarVisibilityAction = viewMenu->addAction(QStringLiteral("主工具栏"));
    m_toolbarVisibilityAction->setObjectName(QStringLiteral("view.toolbar"));
    m_toolbarVisibilityAction->setCheckable(true);
    m_toolbarVisibilityAction->setChecked(true);
    connect(m_toolbarVisibilityAction, &QAction::toggled, this, [this](bool visible) {
        if (auto *toolbar = findChild<QToolBar *>(QStringLiteral("MainToolBar")))
            toolbar->setVisible(visible);
    });
    m_statusVisibilityAction = viewMenu->addAction(QStringLiteral("状态栏"));
    m_statusVisibilityAction->setObjectName(QStringLiteral("view.statusBar"));
    m_statusVisibilityAction->setCheckable(true);
    m_statusVisibilityAction->setChecked(true);
    connect(m_statusVisibilityAction, &QAction::toggled,
            statusBar(), &QStatusBar::setVisible);

    // 编码菜单（Notepad++ 同款）
    m_encodingMenu = menuBar()->addMenu(QStringLiteral("编码(&N)"));
    m_encodingMenu->menuAction()->setObjectName(QStringLiteral("menu.encoding"));
    m_documentOnlyActions.append(m_encodingMenu->menuAction());
    auto *encGroup = new QActionGroup(this);
    m_displayEncodingMenu = m_encodingMenu;
    QVector<QPair<QString, TextEncoding>> encItems = {
        {QStringLiteral("以 GBK 编码显示"),                 TextEncoding::GBK},
        {QStringLiteral("以 GB18030 编码显示"),             TextEncoding::GB18030},
        {QStringLiteral("以 Big5 编码显示"),                TextEncoding::Big5},
        {QStringLiteral("以 Shift-JIS 编码显示"),           TextEncoding::ShiftJis},
        {QStringLiteral("以 Windows-1252 编码显示"),        TextEncoding::Windows1252},
        {QStringLiteral("以 UTF-8 编码显示"),               TextEncoding::Utf8},
        {QStringLiteral("以 UTF-8 BOM 编码显示"),           TextEncoding::Utf8Bom},
        {QStringLiteral("以 UTF-16 Little Endian 编码显示"), TextEncoding::Utf16LE},
        {QStringLiteral("以 UTF-16 Big Endian 编码显示"),    TextEncoding::Utf16BE},
    };
    const QStringList convertEncodingIds = {
        QStringLiteral("gbk"), QStringLiteral("gb18030"),
        QStringLiteral("big5"), QStringLiteral("shiftjis"),
        QStringLiteral("windows1252"), QStringLiteral("utf8"),
        QStringLiteral("utf8bom"), QStringLiteral("utf16le"),
        QStringLiteral("utf16be"),
    };
    m_displayEncodingActions.clear();
    for (int i = 0; i < encItems.size(); ++i) {
        const auto &item = encItems[i];
        auto *a = m_encodingMenu->addAction(item.first);
        a->setObjectName(QStringLiteral("encoding.display.%1")
                             .arg(convertEncodingIds[i]));
        a->setCheckable(true);
        a->setData(static_cast<int>(item.second));
        encGroup->addAction(a);
        m_displayEncodingActions.append(a);
        m_documentOnlyActions.append(a);
        connect(a, &QAction::triggered, this, &MainWindow::displayEncodingFromAction);
    }

    m_encodingMenu->addSeparator();
    auto *eolMenu = m_encodingMenu->addMenu(QStringLiteral("换行符转换"));
    eolMenu->setObjectName(QStringLiteral("encoding.eol"));
    m_documentOnlyActions.append(eolMenu->menuAction());
    m_textMutationActions.append(eolMenu->menuAction());
    const QList<QPair<QString, QsciScintilla::EolMode>> eolModes = {
        {QStringLiteral("Windows (CRLF)"), QsciScintilla::EolWindows},
        {QStringLiteral("Unix (LF)"), QsciScintilla::EolUnix},
        {QStringLiteral("Mac (CR)"), QsciScintilla::EolMac},
    };
    const QStringList eolIds = {
        QStringLiteral("crlf"), QStringLiteral("lf"), QStringLiteral("cr")
    };
    for (int i = 0; i < eolModes.size(); ++i) {
        auto *action = eolMenu->addAction(eolModes.at(i).first);
        action->setObjectName(QStringLiteral("encoding.eol.%1").arg(eolIds.at(i)));
        m_documentOnlyActions.append(action);
        m_textMutationActions.append(action);
        const auto mode = eolModes.at(i).second;
        connect(action, &QAction::triggered, this, [this, mode]() {
            if (auto *tab = currentTab()) {
                if (tab->convertEols(mode))
                    updateStatusBar();
            }
        });
    }

    // Conversion is intentionally folded away from the direct display
    // interpretation actions. It always saves a new file in the target
    // encoding and opens that result; the source file is never overwritten.
    m_encodingMenu->addSeparator();
    auto *convertMenu = m_encodingMenu->addMenu(QStringLiteral("转换为"));
    convertMenu->setObjectName(QStringLiteral("encoding.convert"));
    QVector<QPair<QString, TextEncoding>> dispEncItems = {
        {QStringLiteral("转为 GBK"),               TextEncoding::GBK},
        {QStringLiteral("转为 GB18030"),           TextEncoding::GB18030},
        {QStringLiteral("转为 Big5"),              TextEncoding::Big5},
        {QStringLiteral("转为 Shift-JIS"),         TextEncoding::ShiftJis},
        {QStringLiteral("转为 Windows-1252"),      TextEncoding::Windows1252},
        {QStringLiteral("转为 UTF-8"),             TextEncoding::Utf8},
        {QStringLiteral("转为 UTF-8 BOM"),         TextEncoding::Utf8Bom},
        {QStringLiteral("转为 UTF-16 Little Endian"), TextEncoding::Utf16LE},
        {QStringLiteral("转为 UTF-16 Big Endian"),    TextEncoding::Utf16BE},
    };
    m_convertEncodingActions.clear();
    for (int i = 0; i < dispEncItems.size(); ++i) {
        const auto &item = dispEncItems[i];
        auto *a = convertMenu->addAction(item.first);
        a->setObjectName(QStringLiteral("encoding.convert.%1")
                             .arg(convertEncodingIds[i]));
        a->setData(static_cast<int>(item.second));
        m_convertEncodingActions.append(a);
        m_documentOnlyActions.append(a);
        connect(a, &QAction::triggered, this, &MainWindow::convertEncodingFromAction);
    }
    // 注意：菜单勾选在 onTabChanged / onTabEncodingChanged 中同步

    // 帮助菜单
    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    toolsMenu->menuAction()->setObjectName(QStringLiteral("menu.tools"));
    m_accountsAction = toolsMenu->addAction(QStringLiteral("账户(&A)"));
    m_accountsAction->setObjectName(QStringLiteral("tools.accounts"));
    m_accountsAction->setIconText(QStringLiteral("账户"));
    connect(m_accountsAction, &QAction::triggered,
            this, &MainWindow::showAccountsWorkspace);
    m_securityCenterAction = toolsMenu->addAction(QStringLiteral("安全中心(&S)"));
    m_securityCenterAction->setObjectName(QStringLiteral("tools.securityCenter"));
    connect(m_securityCenterAction, &QAction::triggered,
            this, &MainWindow::showSecurityCenterWorkspace);
    toolsMenu->addSeparator();
    auto *lockVaultAct = toolsMenu->addAction(QStringLiteral("立即锁定(&L)"));
    lockVaultAct->setObjectName(QStringLiteral("tools.lockNow"));
    connect(lockVaultAct, &QAction::triggered, this, &MainWindow::lockSecureDocuments);
    if (m_tasksAction)
        toolsMenu->addAction(m_tasksAction);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->menuAction()->setObjectName(QStringLiteral("menu.help"));
    auto *aboutAct = helpMenu->addAction(QStringLiteral("关于(&A)"));
    aboutAct->setObjectName(QStringLiteral("help.about"));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, QStringLiteral("关于 Notepad Safe"),
            QStringLiteral("<h3>Notepad Safe</h3>"
                           "<p>离线安全文本、Hex 与大文件分析编辑器。</p>"
                           "<p><b>应用版本：</b>%1<br>"
                           "<b>Qt：</b>%2<br>"
                           "<b>QScintilla：</b>2.14.1<br>"
                           "<b>KDBX：</b>KeePassXC Core 2.7.12<br>"
                           "<b>密码学：</b>Botan 3.12.0 + libargon2 20190702<br>"
                           "<b>正则：</b>PCRE2 10.47</p>"
                           "<p>许可证：GNU GPL v3。核心供应商均按锁文件和 SHA-256/commit 固定，"
                           "更新必须通过格式、加密、搜索与 GUI 回归门禁。</p>"
                           "<p><a href='https://www.qt.io/'>Qt</a> · "
                           "<a href='https://www.riverbankcomputing.com/software/qscintilla/'>QScintilla</a> · "
                           "<a href='https://keepassxc.org/'>KeePassXC</a> · "
                           "<a href='https://botan.randombit.net/'>Botan</a> · "
                           "<a href='https://www.pcre.org/'>PCRE2</a></p>")
                .arg(QStringLiteral(NOTEPADSAFE_VERSION),
                     QString::fromLatin1(qVersion())));
    });

    // Top-level information architecture: File, Edit, Search, View, Language,
    // Encoding, Tools, Help. View was populated after Language above, so move
    // its menu action once instead of duplicating menu construction paths.
    menuBar()->removeAction(viewMenu->menuAction());
    menuBar()->insertAction(syntaxMenu->menuAction(), viewMenu->menuAction());
}

void MainWindow::createToolBar()
{
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setObjectName(QStringLiteral("MainToolBar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(16, 16));

    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *newAction = findChild<QAction *>(QStringLiteral("file.new"));
    auto *openAction = findChild<QAction *>(QStringLiteral("file.open"));
    if (newAction) {
        newAction->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        newAction->setIconText(QStringLiteral("新建"));
        newAction->setToolTip(QStringLiteral("新建（默认安全保存）"));
        toolbar->addAction(newAction);
    }
    if (openAction) {
        openAction->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        openAction->setIconText(QStringLiteral("打开"));
        openAction->setToolTip(QStringLiteral("打开文件"));
        toolbar->addAction(openAction);
    }
    if (m_saveAction) {
        m_saveAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        m_saveAction->setIconText(QStringLiteral("保存"));
        m_saveAction->setToolTip(QStringLiteral("保存"));
        toolbar->addAction(m_saveAction);
    }
    toolbar->addSeparator();
    if (m_findAction) {
        m_findAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
        m_findAction->setIconText(QStringLiteral("查找"));
        m_findAction->setToolTip(QStringLiteral("查找 / 查找全部"));
        toolbar->addAction(m_findAction);
    }
    if (m_accountsAction) {
        m_accountsAction->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
        toolbar->addAction(m_accountsAction);
    }
}

void MainWindow::createStatusBar()
{
    m_statusPos = new QLabel(QStringLiteral("行: 1, 列: 1"));
    m_statusPos->setObjectName(QStringLiteral("status.position"));
    m_statusPos->setAccessibleName(QStringLiteral("光标位置"));
    m_statusPos->setMinimumWidth(120);
    statusBar()->addPermanentWidget(m_statusPos);

    m_statusEncoding = new QLabel(QStringLiteral("UTF-8"));
    m_statusEncoding->setObjectName(QStringLiteral("status.encoding"));
    m_statusEncoding->setAccessibleName(QStringLiteral("编码与换行符"));
    m_statusEncoding->setMinimumWidth(80);
    statusBar()->addPermanentWidget(m_statusEncoding);

    m_statusSyntax = new QLabel(QStringLiteral("纯文本"));
    m_statusSyntax->setObjectName(QStringLiteral("status.context"));
    m_statusSyntax->setAccessibleName(QStringLiteral("文档类型或当前工作区"));
    m_statusSyntax->setMinimumWidth(80);
    statusBar()->addPermanentWidget(m_statusSyntax);

    m_statusSize = new QLabel(QStringLiteral("0 B"));
    m_statusSize->setObjectName(QStringLiteral("status.size"));
    m_statusSize->setAccessibleName(QStringLiteral("文档大小"));
    m_statusSize->setMinimumWidth(80);
    statusBar()->addPermanentWidget(m_statusSize);

    m_statusCharacters = new QLabel(QStringLiteral("字符数: 0"));
    m_statusCharacters->setObjectName(QStringLiteral("status.characters"));
    m_statusCharacters->setAccessibleName(QStringLiteral("文档总字符数"));
    m_statusCharacters->setMinimumWidth(100);
    statusBar()->addPermanentWidget(m_statusCharacters);

    m_characterStatusTimer.setSingleShot(true);
    m_characterStatusTimer.setInterval(120);
    connect(&m_characterStatusTimer, &QTimer::timeout,
            this, &MainWindow::updateStatusBar);

    m_hexEditModeButton = new QToolButton;
    m_hexEditModeButton->setObjectName(QStringLiteral("status.hexEditMode"));
    m_hexEditModeButton->setAccessibleName(QStringLiteral("十六进制编辑模式"));
    m_hexEditModeButton->setAutoRaise(true);
    m_hexEditModeButton->setMinimumWidth(44);
    m_hexEditModeButton->hide();
    statusBar()->addPermanentWidget(m_hexEditModeButton);
    connect(m_hexEditModeButton, &QToolButton::clicked, this, [this]() {
        if (auto *tab = currentTab())
            tab->toggleHexOverwriteMode();
    });

    m_statusModified = new QLabel(QStringLiteral(""));
    m_statusModified->setObjectName(QStringLiteral("status.modified"));
    m_statusModified->setAccessibleName(QStringLiteral("保存状态"));
    m_statusModified->setMinimumWidth(40);
    statusBar()->addPermanentWidget(m_statusModified);

    // 活跃任务或失败存在时才显示一个状态 chip；线程占用只放在 tooltip/任务中心，
    // 不让实现细节永久挤占编辑状态栏。
    m_taskWidget = new StatusBarTaskWidget(this);
    statusBar()->addPermanentWidget(m_taskWidget);
}

// ============ 编辑操作 ============

void MainWindow::duplicateLine()
{
    if (auto *t = currentTab()) t->duplicateLine();
}

void MainWindow::deleteLine()
{
    if (auto *t = currentTab()) t->deleteLine();
}

void MainWindow::moveLineUp()
{
    if (auto *t = currentTab()) t->moveLineUp();
}

void MainWindow::moveLineDown()
{
    if (auto *t = currentTab()) t->moveLineDown();
}

void MainWindow::sortLinesAsc()
{
    if (auto *t = currentTab()) t->sortLinesAsc();
}

void MainWindow::sortLinesDesc()
{
    if (auto *t = currentTab()) t->sortLinesDesc();
}

// ============ 搜索 ============

void MainWindow::showFindDialog()
{
    ensureFindDialogCreated();
    if (m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage) {
        if (m_vaultDock)
            m_vaultDock->focusAccountSearch();
        return;
    }
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showFindMode();
}

void MainWindow::showReplaceDialog()
{
    ensureFindDialogCreated();
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showReplaceMode();
}

void MainWindow::findNext()
{
    ensureFindDialogCreated();
    if (!currentTab() || m_lastFindPattern.isEmpty())
        return;
    m_findDialog->findNextFromShortcut(true);
}

void MainWindow::findPrev()
{
    ensureFindDialogCreated();
    if (!currentTab() || m_lastFindPattern.isEmpty())
        return;
    m_findDialog->findNextFromShortcut(false);
}

// ============ 语法 ============

void MainWindow::setSyntaxFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act)
        return;
    int syntax = act->data().toInt();
    auto *tab = currentTab();
    if (!tab)
        return;
    tab->setSyntax(static_cast<Syntax>(syntax));
    syncCommandStates(tab);
    updateStatusBar();
}

// ============ 书签 ============

void MainWindow::toggleBookmark()
{
    if (auto *t = currentTab()) t->toggleBookmark();
}

void MainWindow::nextBookmark()
{
    if (auto *t = currentTab()) t->nextBookmark();
}

void MainWindow::prevBookmark()
{
    if (auto *t = currentTab()) t->prevBookmark();
}

void MainWindow::clearBookmarks()
{
    if (auto *t = currentTab()) t->clearBookmarks();
}

// ============ 视图 ============

void MainWindow::setAppearanceModeFromAction(QAction *action)
{
    if (!action)
        return;
    const int value = action->data().toInt();
    if (value < static_cast<int>(AppearanceMode::System)
        || value > static_cast<int>(AppearanceMode::Dark)) {
        return;
    }
    m_appearanceMode = static_cast<AppearanceMode>(value);
    applyAppearanceMode();
    syncGlobalMenuCheckStates();
    saveSettings();
}

void MainWindow::zoomIn()
{
    if (auto *t = currentTab()) t->editor()->zoomIn();
}

void MainWindow::zoomOut()
{
    if (auto *t = currentTab()) t->editor()->zoomOut();
}

void MainWindow::resetZoom()
{
    if (auto *t = currentTab()) t->editor()->zoomTo(0);
}

void MainWindow::toggleEdgeLine()
{
    m_edgeVisible = !m_edgeVisible;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setEdgeVisible(m_edgeVisible);
    }
    // 同步勾选（toggle 是用户直接点菜单，QAction 自己会切；这里兜底防手动调用）
    if (m_actEdgeVisible) m_actEdgeVisible->setChecked(m_edgeVisible);
    saveSettings();
}

void MainWindow::setEdgeColumnFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    int col = act->data().toInt();
    if (col <= 0) return;
    m_edgeColumn = col;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setEdgeColumn(m_edgeColumn);
    }
    // 同步 action group 勾选
    if (m_edgeColGroup) {
        for (QAction *a : m_edgeColGroup->actions()) {
            a->setChecked(a->data().toInt() == m_edgeColumn);
        }
    }
    saveSettings();
}

void MainWindow::toggleWordWrap()
{
    EditorTab *active = currentTab();
    if (active && active->isReadOnlyByPolicy()) {
        syncCommandStates(active);
        syncGlobalMenuCheckStates();
        return;
    }
    m_wordWrap = !m_wordWrap;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setWordWrap(m_wordWrap);
    }
    if (m_actWordWrap) m_actWordWrap->setChecked(m_wordWrap);
    saveSettings();
}

// ============================================================================
//  16 进制视图模式切换（仅对当前 L2 VirtualFileView tab 生效；L1/QScintilla tab 忽略）
// ============================================================================
void MainWindow::toggleHexViewMode()
{
    auto *tab = currentTab();
    if (!tab || !tab->isL2Active()) {
        syncGlobalMenuCheckStates();
        return;
    }
    ViewMode now = tab->viewModeL2();
    ViewMode next = (now == ViewMode::Hex) ? ViewMode::Text : ViewMode::Hex;
    tab->setViewModeL2(next);
    syncCommandStates(tab);
    syncGlobalMenuCheckStates();
    updateStatusBar();
}

// 统一把外观/换行/标尺/呈现方式同步到视图菜单。
// 这是解决"恢复设置后菜单勾选未同步显示"的核心函数
void MainWindow::syncGlobalMenuCheckStates()
{
    if (m_actAppearanceSystem)
        m_actAppearanceSystem->setChecked(m_appearanceMode == AppearanceMode::System);
    if (m_actAppearanceLight)
        m_actAppearanceLight->setChecked(m_appearanceMode == AppearanceMode::Light);
    if (m_actDarkMode)
        m_actDarkMode->setChecked(m_appearanceMode == AppearanceMode::Dark);
    if (m_actWordWrap) {
        EditorTab *tab = currentTab();
        m_actWordWrap->setChecked(tab && tab->isReadOnlyByPolicy()
                                      ? false : m_wordWrap);
    }
    if (m_actWhitespaceVisible)
        m_actWhitespaceVisible->setChecked(m_whitespaceVisible);
    if (m_actEolVisible)
        m_actEolVisible->setChecked(m_eolVisible);
    if (m_actUseTabs)
        m_actUseTabs->setChecked(m_useTabs);
    if (m_actBackspaceUnindents)
        m_actBackspaceUnindents->setChecked(m_backspaceUnindents);
    if (m_actWordCompletion)
        m_actWordCompletion->setChecked(m_wordCompletion);
    if (m_actEdgeVisible) m_actEdgeVisible->setChecked(m_edgeVisible);
    if (m_actTextViewMode || m_actHexViewMode) {
        auto *tab = currentTab();
        const bool dualPresentation = tab && tab->isL2Active();
        const bool hex = dualPresentation && tab->viewModeL2() == ViewMode::Hex;
        if (m_actTextViewMode) {
            m_actTextViewMode->setEnabled(dualPresentation);
            m_actTextViewMode->setChecked(dualPresentation && !hex);
        }
        if (m_actHexViewMode) {
            m_actHexViewMode->setEnabled(dualPresentation);
            m_actHexViewMode->setChecked(hex);
        }
    }
    if (m_edgeColGroup) {
        for (QAction *a : m_edgeColGroup->actions()) {
            a->setChecked(a->data().toInt() == m_edgeColumn);
        }
    }
    if (m_tabWidthGroup) {
        for (QAction *action : m_tabWidthGroup->actions())
            action->setChecked(action->data().toInt() == m_tabWidth);
    }
}

// 给 w 及其（当它是 EditorTab 时）内部的 editor/editor->viewport() 关闭 acceptDrops
// 并安装事件过滤器，把 DragEnter/DragMove/Drop 都转发到 MainWindow 处理
void MainWindow::registerDropProxy(QWidget *w)
{
    if (!w) return;
    w->setAcceptDrops(false);
    w->installEventFilter(this);

    // 特殊：EditorTab 内部的 QsciScintilla 才是用户最常拖到的控件
    auto *tab = qobject_cast<EditorTab *>(w);
    if (tab && tab->editor()) {
        QsciScintilla *ed = tab->editor();
        ed->setAcceptDrops(false);
        ed->installEventFilter(this);
        if (ed->viewport()) {
            ed->viewport()->setAcceptDrops(false);
            ed->viewport()->installEventFilter(this);
        }
    }
}

// ============ 外部文件打开（命令行/拖放通用） ============

void MainWindow::openFilesFromExternal(const QStringList &paths)
{
    if (paths.isEmpty()) return;
    int anyEmptyUntitledIdx = -1;
    // 只有当只有一个"未命名 且 空 且 未修改"的标签时，第一个文件才覆盖该标签
    if (m_tabs->count() == 1) {
        auto *t0 = qobject_cast<EditorTab *>(m_tabs->widget(0));
        if (t0 && t0->isUntitled() && !t0->isModified()
            && t0->editor()->text().isEmpty()) {
            anyEmptyUntitledIdx = 0;
        }
    }
    int idx = 0;
    for (const QString &p : paths) {
        if (p.isEmpty()) continue;
        QFileInfo fi(p);
        if (!fi.isFile()) continue;
        EditorTab *reuseTab = nullptr;
        if (idx == 0 && anyEmptyUntitledIdx >= 0)
            reuseTab = qobject_cast<EditorTab *>(m_tabs->widget(anyEmptyUntitledIdx));
        // openPathUnified owns tab creation for every non-placeholder file.
        // This keeps drag/drop and command-line opens on the same duplicate-
        // detection path as File > Open and Recent Files.
        if (!openPathUnified(p, reuseTab)) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                QStringLiteral("无法打开文件: %1").arg(p));
        }
        ++idx;
    }
}

// ============ 编码 ============

void MainWindow::setEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    tab->setEncoding(enc);
    tab->setModified(true);
}

void MainWindow::convertEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    if (tab->isSecureDocument() || tab->isUntitled() || tab->isBinaryDocument())
        return;
    if (tab->encodingHasDecodeErrors()) {
        QMessageBox::information(
            this, QStringLiteral("请先确认源编码"),
            QStringLiteral("源文件包含不符合当前 %1 解释的字节。"
                           "请先用“以 … 编码显示”选择正确源编码，再执行转换。")
                .arg(encodingName(tab->displayEncoding())));
        return;
    }
    if (tab->isL2Active()
        && tab->externalFileState() != EditorTab::ExternalFileState::Unchanged) {
        QMessageBox::warning(
            this, QStringLiteral("源文件已经变化"),
            QStringLiteral("磁盘文件在打开后已被修改、替换或删除。"
                           "请重新打开源文件后再执行编码转换。"));
        return;
    }

    const QFileInfo sourceInfo(tab->filePath());
    QString suffix = sourceInfo.completeSuffix();
    const QString stem = sourceInfo.completeBaseName().isEmpty()
        ? sourceInfo.fileName() : sourceInfo.completeBaseName();
    QString targetSuggestion = sourceInfo.dir().filePath(
        stem + QStringLiteral("-")
        + encodingName(enc).toLower().replace(QLatin1Char(' '), QLatin1Char('-'))
        + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix));
    QFileDialog conversionDialog(
        this, QStringLiteral("转换为 %1 并另存为").arg(encodingName(enc)),
        targetSuggestion, QStringLiteral("所有文件 (*)"));
    conversionDialog.setObjectName(QStringLiteral("encodingConversionSaveAsDialog"));
    conversionDialog.setAcceptMode(QFileDialog::AcceptSave);
    conversionDialog.setFileMode(QFileDialog::AnyFile);
    conversionDialog.setOption(QFileDialog::DontUseNativeDialog, true);
    conversionDialog.selectFile(targetSuggestion);
    if (conversionDialog.exec() != QDialog::Accepted
        || conversionDialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString target = conversionDialog.selectedFiles().constFirst();
    if (sameDiskFilePath(tab->filePath(), target)) {
        QMessageBox::warning(this, QStringLiteral("请选择新文件"),
                             QStringLiteral("编码转换不会覆盖源文件，请选择其他路径。"));
        return;
    }
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (openTab && openTab != tab && !openTab->filePath().isEmpty()
            && sameDiskFilePath(openTab->filePath(), target)) {
            QMessageBox::warning(
                this, QStringLiteral("目标文件已打开"),
                QStringLiteral("为避免写入后仍显示旧内容，请关闭目标文件标签，或选择其他路径。"));
            return;
        }
    }

    struct ConversionResult { bool converted = false; QString error; };
    auto result = QSharedPointer<ConversionResult>::create();
    auto smallText = tab->isL2Active()
        ? QSharedPointer<QString>()
        : QSharedPointer<QString>::create(tab->editor()->text());
    const QString sourcePath = tab->filePath();
    const TextEncoding sourceEncoding = tab->displayEncoding();
    QProgressDialog progress(QStringLiteral("正在转换编码…"),
                             QStringLiteral("取消"), 0, 1000, this);
    progress.setObjectName(QStringLiteral("encodingConversionProgress"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setValue(0);

    QEventLoop loop;
    qint64 conversionTaskId = -1;
    const auto finishedConnection = connect(
        &AsyncTaskManager::instance(), &AsyncTaskManager::taskFinished, &loop,
        [&loop, &conversionTaskId](qint64 id, AsyncTaskState) {
            if (id == conversionTaskId)
                loop.quit();
        });
    const auto progressConnection = connect(
        &AsyncTaskManager::instance(), &AsyncTaskManager::taskProgressChanged,
        &progress, [&progress, &conversionTaskId](qint64 id, qint64 done, qint64 total) {
            if (id == conversionTaskId && total > 0)
                progress.setValue(int(qMin<qint64>(1000, done * 1000 / total)));
        });
    conversionTaskId = AsyncTaskManager::instance().submit(
AsyncTaskType::EncodingConversion, AsyncTaskPriority::Highest,
        QStringLiteral("正在转换文件编码"), QString(),
        [result, smallText, sourcePath, sourceEncoding, target, enc]
        (qint64, AsyncCancelPtr cancel, const auto &reportProgress,
         const auto &reportError) {
            if (smallText) {
                result->converted = writeTextAsEncoding(
                    target, *smallText, enc, &result->error);
                smallText->fill(QChar(0));
                smallText->clear();
            } else {
                result->converted = transcodeFileEncoding(
                    sourcePath, sourceEncoding, target, enc, &result->error,
                    cancel.data(), reportProgress);
            }
            if (!result->converted && result->error != QStringLiteral("转换已取消。")) {
                AsyncErrorInfo asyncError;
                asyncError.message = QStringLiteral("编码转换未完成。");
                reportError(asyncError);
            }
        }, 0, QStringLiteral("Encoding"), 1000, tab->isL2Active());
    connect(&progress, &QProgressDialog::canceled, this,
            [conversionTaskId] {
                AsyncTaskManager::instance().cancel(conversionTaskId);
            });
    progress.show();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    disconnect(finishedConnection);
    disconnect(progressConnection);
    progress.close();

    const bool converted = result->converted;
    const QString error = result->error;
    if (!converted) {
        if (error != QStringLiteral("转换已取消。")) {
            QMessageBox::warning(this, QStringLiteral("编码转换失败"), error);
        }
        return;
    }
    if (!openPathUnified(QFileInfo(target).absoluteFilePath(), nullptr, enc)) {
        QMessageBox::warning(this, QStringLiteral("转换完成但打开失败"),
                             QStringLiteral("新文件已保存：\n%1").arg(target));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已转换为 %1，并打开新文件").arg(encodingName(enc)), 4000);
}

void MainWindow::displayEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    if (tab->isSecureDocument() || tab->isUntitled()) {
        syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
        return;
    }
    if (tab->displayEncoding() == enc)
        return;

    // L1 rereads the saved bytes. A modified small document uses the ordinary
    // Save/Cancel decision; after Save it follows the exact unmodified path.
    if (!tab->isL2Active() && tab->isModified()) {
        const auto decision = QMessageBox::warning(
            this, QStringLiteral("保存修改"),
            QStringLiteral("“%1”有未保存的修改。要先保存再以 %2 编码显示吗？")
                .arg(tab->fileName(), encodingName(enc)),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (decision != QMessageBox::Save) {
            syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
            return;
        }
        if (!saveFile()) {
            syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
            return;
        }
    }

    tab->setDisplayEncoding(enc);
    syncEncodingMenuTo(m_encodingMenu, tab->encoding());
    syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
    updateStatusBar();
}


void MainWindow::applyDarkMode(bool dark)
{
    QPalette pal;
    if (m_highContrastMode) {
        // Let the platform own every color in accessibility mode. In
        // particular, do not put our dark menu stylesheet on top of a user's
        // custom Windows contrast scheme.
        pal = QApplication::style()->standardPalette();

    } else if (dark) {
        pal.setColor(QPalette::Window, QColor(45, 45, 48));
        pal.setColor(QPalette::WindowText, QColor(208, 208, 208));
        pal.setColor(QPalette::Base, QColor(30, 30, 30));
        pal.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
        pal.setColor(QPalette::Text, QColor(208, 208, 208));
        pal.setColor(QPalette::Button, QColor(60, 60, 64));
        pal.setColor(QPalette::ButtonText, QColor(208, 208, 208));
        pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
        pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        pal.setColor(QPalette::ToolTipBase, QColor(60, 60, 64));
        pal.setColor(QPalette::ToolTipText, QColor(208, 208, 208));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(125, 125, 125));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor(125, 125, 125));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(125, 125, 125));
    } else {
        // The platform standard palette may itself be dark when Windows is in
        // dark mode.  "Light" is an explicit user choice, so restoring that
        // palette must not depend on the current OS theme (or on the custom
        // palette previously installed by this application).
        pal = QApplication::style()->standardPalette();
        pal.setColor(QPalette::Window, QColor(240, 240, 240));
        pal.setColor(QPalette::WindowText, QColor(0, 0, 0));
        pal.setColor(QPalette::Base, QColor(255, 255, 255));
        pal.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
        pal.setColor(QPalette::Text, QColor(0, 0, 0));
        pal.setColor(QPalette::Button, QColor(240, 240, 240));
        pal.setColor(QPalette::ButtonText, QColor(0, 0, 0));
        pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
        pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        pal.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
        pal.setColor(QPalette::ToolTipText, QColor(0, 0, 0));
    }
    qApp->setPalette(pal);
    // The native Windows style does not consistently honor QPalette text
    // roles for owner-drawn menus. Without these narrow overrides it produces
    // black menu text on our dark background. Keep the stylesheet limited to
    // menus/tooltips so individual editor/account control styles still work.
    qApp->setStyleSheet(!m_highContrastMode && dark ? QStringLiteral(
        "QMenuBar { background-color:#2d2d30; color:#d0d0d0; }"
        "QMenuBar::item:selected { background-color:#3c3c40; color:#ffffff; }"
        "QMenuBar::item:pressed { background-color:#3c3c40; color:#ffffff; }"
        "QMenu { background-color:#2d2d30; color:#d0d0d0; border:1px solid #55555a; }"
        "QMenu::item:selected { background-color:#0078d7; color:#ffffff; }"
        "QMenu::item:disabled { color:#7d7d7d; }"
        "QMenu::separator { height:1px; background:#505055; margin:4px 8px; }"
        "QToolTip { color:#d0d0d0; background-color:#3c3c40; border:1px solid #66666a; }")
        : QString());

    // 更新所有 EditorTab 的编辑器配色
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab)
            continue;
        auto *e = tab->editor();
        if (m_highContrastMode) {
            e->setPaper(pal.color(QPalette::Base));
            e->setColor(pal.color(QPalette::Text));
            e->setMarginsBackgroundColor(pal.color(QPalette::Window));
            e->setMarginsForegroundColor(pal.color(QPalette::WindowText));
            e->setCaretLineBackgroundColor(pal.color(QPalette::AlternateBase));
            e->setSelectionBackgroundColor(pal.color(QPalette::Highlight));
            e->setSelectionForegroundColor(pal.color(QPalette::HighlightedText));
        } else if (dark) {
            e->setPaper(QColor(30, 30, 30));
            e->setColor(QColor(208, 208, 208));
            e->setMarginsBackgroundColor(QColor(45, 45, 48));
            e->setMarginsForegroundColor(QColor(150, 150, 150));
            e->setCaretLineBackgroundColor(QColor(60, 60, 64));
            e->setSelectionBackgroundColor(QColor(0, 120, 215));
            e->setSelectionForegroundColor(QColor(255, 255, 255));
        } else {
            e->setPaper(QColor(255, 255, 255));
            e->setColor(QColor(0, 0, 0));
            e->setMarginsBackgroundColor(QColor(240, 240, 240));
            e->setMarginsForegroundColor(QColor(120, 120, 120));
            e->setCaretLineBackgroundColor(QColor(245, 245, 220));
            e->setSelectionBackgroundColor(QColor(51, 153, 255));
            e->setSelectionForegroundColor(QColor(255, 255, 255));
        }
    }
}

void MainWindow::applyAppearanceMode()
{
    m_highContrastMode = systemHighContrastEnabled();
    bool dark = false;
    switch (m_appearanceMode) {
    case AppearanceMode::Dark:
        dark = true;
        break;
    case AppearanceMode::Light:
        dark = false;
        break;
    case AppearanceMode::System: {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        dark = scheme == Qt::ColorScheme::Dark;
        if (scheme == Qt::ColorScheme::Unknown) {
            dark = QApplication::style()->standardPalette()
                       .color(QPalette::Window).lightness() < 128;
        }
        break;
    }
    }
    m_darkMode = dark;
    applyDarkMode(dark);
}

// ============ 标签事件 ============

EditorTab *MainWindow::currentTab() const
{
    if (m_destroying || !m_tabs)
        return nullptr;
    if (m_workspaceStack && m_workspaceStack->currentWidget() != m_tabs)
        return nullptr;
    return qobject_cast<EditorTab *>(m_tabs->currentWidget());
}

void MainWindow::syncCommandStates(EditorTab *tab)
{
    if (m_destroying)
        return;
    const bool documentWorkspace = isDocumentWorkspaceActive();
    if (!tab && documentWorkspace)
        tab = currentTab();
    if (!documentWorkspace)
        tab = nullptr;
    const bool hasDocument = tab != nullptr;
    for (QAction *action : m_documentOnlyActions) {
        if (action)
            action->setEnabled(hasDocument);
    }
    const bool canModify = hasDocument && tab->canModifyCommand();
    const bool hasSelection = hasDocument && tab->hasSelectionCommand();
    const bool accountsWorkspace = m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage;
    const QMimeData *clipboardData = QGuiApplication::clipboard()->mimeData();
    if (m_commandRegistry) {
        const DocumentState documentState = hasDocument
            ? tab->documentState() : DocumentState{};
        CommandContext context;
        context.document = hasDocument ? &documentState : nullptr;
        context.hasDocument = hasDocument;
        context.canModify = canModify;
        context.hasSelection = hasSelection;
        context.clipboardHasText = clipboardData && clipboardData->hasText()
            && !clipboardData->text().isEmpty();
        context.canUndo = hasDocument && tab->canUndoCommand();
        context.canRedo = hasDocument && tab->canRedoCommand();
        context.masked = hasDocument && tab->isSecureDocumentMasked();
        context.modifiedOrUntitled = hasDocument
            && (tab->isModified() || tab->isUntitled());
        context.accountsWorkspace = accountsWorkspace;
        context.accountCanSave = accountsWorkspace && m_vaultDock
            && m_vaultDock->canSaveActiveEntry();
        context.accountCanSearch = accountsWorkspace && m_vaultDock
            && m_vaultDock->canSearchAccounts();
        context.reloadable = hasDocument && (tab->isSecureDocument()
            ? !tab->filePath().isEmpty() : !tab->isUntitled());
        context.canCloseAll = documentWorkspace && m_tabs
            && m_tabs->count() > 0 && currentTab() != nullptr;
        m_commandRegistry->applyContext(context);
    }
    if (m_actWordWrap)
        m_actWordWrap->setEnabled(hasDocument && !tab->isReadOnlyByPolicy());
    const bool editableHex = hasDocument && canModify && tab->isL2Active()
        && tab->viewModeL2() == ViewMode::Hex;
    if (m_hexInsertModeAction) {
        m_hexInsertModeAction->setEnabled(editableHex);
        m_hexInsertModeAction->setChecked(
            editableHex && !tab->hexOverwriteMode());
    }
    for (QAction *action : m_textMutationActions) {
        if (action)
            action->setEnabled(hasDocument && tab->supportsTextMutationCommands());
    }
    const bool nativeTextSurface = hasDocument && !tab->isL2Active()
                                   && !tab->isBinaryDocument()
                                   && !tab->isSecureDocumentMasked();
    for (QAction *action : m_nativeTextActions) {
        if (action)
            action->setEnabled(nativeTextSurface);
    }
    for (QAction *action : m_lexerActions) {
        if (action)
            action->setEnabled(hasDocument && tab->supportsLexerCommands());
    }
    for (QAction *action : m_languageActions) {
        if (action)
            action->setEnabled(nativeTextSurface);
    }
    const bool splitAvailable = hasDocument && tab->canSplitView();
    if (m_actSplitHorizontal) {
        QSignalBlocker blocker(m_actSplitHorizontal);
        m_actSplitHorizontal->setEnabled(splitAvailable);
        m_actSplitHorizontal->setChecked(
            hasDocument && tab->splitViewEnabled()
            && tab->splitOrientation() == Qt::Horizontal);
    }
    if (m_actSplitVertical) {
        QSignalBlocker blocker(m_actSplitVertical);
        m_actSplitVertical->setEnabled(splitAvailable);
        m_actSplitVertical->setChecked(
            hasDocument && tab->splitViewEnabled()
            && tab->splitOrientation() == Qt::Vertical);
    }
    if (m_statusModified && accountsWorkspace) {
        m_statusModified->setText(m_vaultDock && m_vaultDock->canSaveActiveEntry()
                                      ? QStringLiteral("未保存")
                                      : QString());
    }
    // 无 tab 时：所有这些动作一律禁用，避免误点
    const bool textPresentation = hasDocument
        && (!tab->isL2Active() || tab->viewModeL2() == ViewMode::Text);
    const bool displayEncodingAvailable = hasDocument
        && !tab->isSecureDocument() && !tab->isUntitled();
    const bool conversionAvailable = displayEncodingAvailable
        && !tab->isBinaryDocument();
    if (m_encodingMenu)
        m_encodingMenu->menuAction()->setEnabled(displayEncodingAvailable
                                                 || conversionAvailable);
    for (QAction *action : m_displayEncodingActions) {
        if (action)
            action->setEnabled(displayEncodingAvailable);
    }
    const bool enabled = canModify && !tab->isL2Active();
    if (m_actSortAsc) m_actSortAsc->setEnabled(enabled);
    if (m_actSortDesc) m_actSortDesc->setEnabled(enabled);
    for (auto *a : m_convertEncodingActions) {
        if (a) a->setEnabled(conversionAvailable);
    }
    const bool hasDualPresentation = hasDocument && tab->isL2Active();
    if (m_actTextViewMode)
        m_actTextViewMode->setEnabled(hasDualPresentation);
    if (m_actHexViewMode)
        m_actHexViewMode->setEnabled(hasDualPresentation);
    // 注："以编码显示..."组不在 m_convertEncodingActions 里，文档工作区内
    // 始终可用（只改显示解码不改字节流，L2 允许）。
}

void MainWindow::onTabChanged(int index)
{
    EditorTab *tab = nullptr;
    if (index >= 0) tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (m_findDialog) m_findDialog->setActiveTab(tab);
    // 统一同步不适配 L2 的菜单项（含 index<0 无 tab → 全部变灰）
    syncCommandStates(tab);
    syncGlobalMenuCheckStates();
    if (!tab) {
        updateStatusBar();
        return;
    }
    // 同步编码菜单勾选到当前标签
    syncEncodingMenuTo(m_encodingMenu, tab->encoding());
    syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
    // 应用全局设置（保持所有标签一致）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);
    applyEditorPreferences(tab);
    updateStatusBar();
}

void MainWindow::showTabContextMenu(const QPoint &position)
{
    if (!m_tabs || !m_tabs->tabBar() || !isDocumentWorkspaceActive())
        return;

    QTabBar *bar = m_tabs->tabBar();
    int index = bar->tabAt(position);
    QPoint popupPosition = bar->mapToGlobal(position);
    // Qt sends (-1, -1) for the keyboard context-menu key. In that case use
    // the active tab and anchor the menu below its label.
    if (index < 0 && position.x() < 0 && m_tabs->currentIndex() >= 0) {
        index = m_tabs->currentIndex();
        const QRect rect = bar->tabRect(index);
        popupPosition = bar->mapToGlobal(QPoint(rect.left(), rect.bottom()));
    }
    if (index < 0)
        return;

    m_tabs->setCurrentIndex(index);
    auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (!tab)
        return;

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("tabContextMenu"));
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    QAction *save = menu->addAction(QStringLiteral("保存"));
    save->setObjectName(QStringLiteral("tabContext.save"));
    save->setShortcutVisibleInContextMenu(true);
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this]() { saveFile(); });

    QAction *saveAs = menu->addAction(QStringLiteral("另存为…"));
    saveAs->setObjectName(QStringLiteral("tabContext.saveAs"));
    saveAs->setShortcutVisibleInContextMenu(true);
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs, &QAction::triggered, this, [this]() { saveAsFile(); });

    menu->addSeparator();
    QAction *close = menu->addAction(QStringLiteral("关闭"));
    close->setObjectName(QStringLiteral("tabContext.close"));
    close->setShortcutVisibleInContextMenu(true);
    close->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(close, &QAction::triggered, this, [this, index]() {
        onTabCloseRequested(index);
    });

    QAction *closeOthers = menu->addAction(QStringLiteral("关闭其他标签"));
    closeOthers->setObjectName(QStringLiteral("tabContext.closeOthers"));
    closeOthers->setEnabled(m_tabs->count() > 1);
    connect(closeOthers, &QAction::triggered, this, [this, index]() {
        closeOtherTabs(index);
    });

    QAction *closeAll = menu->addAction(QStringLiteral("关闭全部标签"));
    closeAll->setObjectName(QStringLiteral("tabContext.closeAll"));
    connect(closeAll, &QAction::triggered, this, &MainWindow::closeAllTabs);

    // DraftStore paths are private implementation details. Only user-named
    // files expose path actions, including named encrypted .nsafe files.
    const QString path = tab->filePath();
    if (!path.isEmpty()) {
        menu->addSeparator();
        QAction *copyPath = menu->addAction(QStringLiteral("复制完整路径"));
        copyPath->setObjectName(QStringLiteral("tabContext.copyPath"));
        connect(copyPath, &QAction::triggered, this, [this, path]() {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
            statusBar()->showMessage(QStringLiteral("已复制文件路径"), 2500);
        });

        QAction *reveal = menu->addAction(QStringLiteral("在资源管理器中显示"));
        reveal->setObjectName(QStringLiteral("tabContext.reveal"));
        reveal->setEnabled(QFileInfo::exists(path));
        connect(reveal, &QAction::triggered, this, [this, path]() {
            bool opened = false;
#ifdef Q_OS_WIN
            opened = QProcess::startDetached(
                QStringLiteral("explorer.exe"),
                {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
            opened = QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
            if (!opened)
                statusBar()->showMessage(QStringLiteral("无法打开文件所在位置"), 5000);
        });
    }

    menu->addSeparator();
    QAction *asText = menu->addAction(QStringLiteral("以文本重新打开"));
    asText->setObjectName(QStringLiteral("tabContext.presentText"));
    QAction *asHex = menu->addAction(QStringLiteral("以十六进制重新打开"));
    asHex->setObjectName(QStringLiteral("tabContext.presentHex"));
    const bool presentationSwitchAvailable = tab->isL2Active();
    asText->setEnabled(presentationSwitchAvailable
                       && tab->viewModeL2() != ViewMode::Text);
    asHex->setEnabled(presentationSwitchAvailable
                      && tab->viewModeL2() != ViewMode::Hex);
    if (!presentationSwitchAvailable) {
        const QString reason = QStringLiteral(
            "当前直接编辑视图尚不需要重新装载；二进制和大文件视图可在文本与十六进制间切换。");
        asText->setStatusTip(reason);
        asHex->setStatusTip(reason);
    }
    connect(asText, &QAction::triggered, this, [this, tab]() {
        if (!tab || !tab->isL2Active())
            return;
        tab->setViewModeL2(ViewMode::Text);
        syncCommandStates(tab);
        syncGlobalMenuCheckStates();
        updateStatusBar();
    });
    connect(asHex, &QAction::triggered, this, [this, tab]() {
        if (!tab || !tab->isL2Active())
            return;
        tab->setViewModeL2(ViewMode::Hex);
        syncCommandStates(tab);
        syncGlobalMenuCheckStates();
        updateStatusBar();
    });

    menu->popup(popupPosition);
}

void MainWindow::closeTabUnchecked(int index)
{
    if (!m_tabs || index < 0 || index >= m_tabs->count())
        return;
    QWidget *widget = m_tabs->widget(index);
    auto *tab = qobject_cast<EditorTab *>(widget);
    if (tab) {
        if (m_searchDock)
            m_searchDock->closeResultsFor(tab->documentState().id);
        if (tab->isSecureDocument() && m_secureDocuments)
            closeSecureDocumentOnSecurityLane(tab->secureDocumentId());
        else
            tab->clearAutosaveFile();
        tab->shutdownL2BeforeClose();
    }
    m_tabs->removeTab(index);
    delete widget;
}

void MainWindow::closeOtherTabs(int keepIndex)
{
    if (!m_tabs || keepIndex < 0 || keepIndex >= m_tabs->count())
        return;
    QPointer<EditorTab> keep = qobject_cast<EditorTab *>(m_tabs->widget(keepIndex));
    if (!keep)
        return;

    // Preflight all affected documents before deleting any of them, matching
    // Close All's all-or-nothing cancellation behavior.
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->widget(index) == keep)
            continue;
        if (!maybeSaveTab(index, false)) {
            const int currentKeepIndex = m_tabs->indexOf(keep);
            if (currentKeepIndex >= 0)
                m_tabs->setCurrentIndex(currentKeepIndex);
            return;
        }
    }
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        if (m_tabs->widget(index) != keep)
            closeTabUnchecked(index);
    }
    const int currentKeepIndex = m_tabs->indexOf(keep);
    if (currentKeepIndex >= 0)
        m_tabs->setCurrentIndex(currentKeepIndex);
}

void MainWindow::onTabCloseRequested(int index)
{
    // closingApp=false: 单标签关闭模式
    if (!maybeSaveTab(index, /*closingApp=*/false))
        return;
    closeTabUnchecked(index);
}

bool MainWindow::maybeSaveTab(int index, bool closingApp)
{
    auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (!tab)
        return true;

    // 加密笔记采用 Note 风格自动保存；绝不进入明文 autosave，也不弹出
    // “另存为”对话框。保存失败时阻止关闭，避免静默丢失。
    if (tab->isSecureDocument()) {
        if (!tab->isModified())
            return true;
        return saveSecureTab(tab, true, true);
    }

    // 无论何种情况，内容未修改都直接放行
    if (!tab->isModified()) {
        // 空内容的未命名文件：直接清 autosave（没内容就没必要缓存）
        if (tab->isUntitled() && tab->editor()->text().trimmed().isEmpty())
            tab->clearAutosaveFile();
        return true;
    }

    // ===== 关闭整个应用的 Notepad++ 逻辑 =====
    if (closingApp) {
        // ① "未命名+有内容"：不提示用户，由后续 saveSession 写入 autosave（下次恢复）
        if (tab->isUntitled())
            return true;
        // ② 从 autosave 会话恢复的 tab + autosave 文件还存在：
        //    数据已在 autosave 里，closeEvent 紧接着的 saveSessionNow() 会再落盘一次，
        //    下次启动还能完整恢复（等价于 ① 的"未命名 autosave"语义，只是对用户显示原名）。
        //    若每次恢复后关闭都弹「是否保存」，用户会被"我没改过东西也弹窗"困惑。
        const QString &asp = tab->autosavePath();
        if (!asp.isEmpty() && QFileInfo::exists(asp))
            return true;
        // ③ 已存在文件+用户修改过+没有 autosave 兜底：必须提示保存/丢弃/取消
    }

    // ===== 单标签关闭：未命名且有内容也要提示 =====
    // （下面的代码对 closingApp=true 的"已存在文件修改"以及 closingApp=false 的全部情况都提示）

    auto ret = QMessageBox::warning(
        this, QStringLiteral("Notepad Safe"),
        QStringLiteral("标签 \"%1\" 已修改。\n是否保存更改？").arg(tab->fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (ret == QMessageBox::Save) {
        m_tabs->setCurrentIndex(index);
        if (tab->isUntitled()) {
            // 单标签关闭时的另存为
            bool ok = saveAsFile();
            // 如果用户取消另存为对话框，则视为取消操作
            return ok;
        }
        return saveFile();
    }
    if (ret == QMessageBox::Cancel)
        return false;
    // Discard: 丢弃改动
    if (!tab->isSecureDocument())
        tab->clearAutosaveFile();
    return true;
}

void MainWindow::onTabModifiedChanged(bool modified)
{
    Q_UNUSED(modified)
    // Size is the live document payload, not the backing file on disk. This is
    // especially important for secure documents, whose KDBX container size is
    // unrelated to the text the user is editing.
    updateStatusBar();
}

void MainWindow::onTabCursorPositionChanged(int line, int index)
{
    EditorTab *tab = currentTab();
    if (tab && tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
        // Hex uses cursorPositionChanged for both byte movement and OVR/INS
        // mode changes.  Refresh the complete permanent status contract so
        // the size/tier/mode segment cannot retain a stale INS/OVR value.
        updateStatusBar();
        return;
    }
    m_statusPos->setText(QStringLiteral("行: %1, 列: %2").arg(line + 1).arg(index + 1));
}

void MainWindow::onTabTitleChanged(const QString &title)
{
    Q_UNUSED(title);
    // tab 文本在 lambda 中更新
}

void MainWindow::onTabEncodingChanged(TextEncoding encoding)
{
    const EditorTab *tab = currentTab();
    const QString origin = tab && tab->encodingHasDecodeErrors()
        ? QStringLiteral(" · 包含非法字节")
        : tab && tab->encodingOrigin() == EncodingOrigin::Fallback
            ? QStringLiteral(" · 默认假设") : QString();
    m_statusEncoding->setText(tab
        ? QStringLiteral("%1%2 · %3")
              .arg(encodingName(encoding), origin, tab->lineEndingName())
        : encodingName(encoding));
    syncEncodingMenuTo(m_encodingMenu, encoding);
}

void MainWindow::onJumpToByteOffset(qint64 byteOffset, int length)
{
    auto *tab = currentTab();
    if (!tab) return;
    tab->gotoByteOffset(byteOffset, length);
    tab->setFocusEditor();
}

void MainWindow::onJumpToDocumentByteOffset(const QUuid &documentId,
                                             quint64 expectedRevision,
                                             qint64 byteOffset, int length)
{
    EditorTab *tab = nullptr;
    for (int index = 0; m_tabs && index < m_tabs->count(); ++index) {
        auto *candidate = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (candidate && candidate->documentState().id == documentId) {
            tab = candidate;
            break;
        }
    }
    if (!tab)
        return;
    if (tab->contentRevision() != expectedRevision) {
        if (m_searchDock)
            m_searchDock->invalidateResultsFor(documentId);
        return;
    }
    const int tabIndex = m_tabs->indexOf(tab);
    if (tabIndex < 0)
        return;
    showDocumentWorkspace();
    m_tabs->setCurrentIndex(tabIndex);
    QPointer<EditorTab> target(tab);
    QTimer::singleShot(0, tab, [target, byteOffset, length]() {
        if (!target)
            return;
        target->gotoByteOffset(byteOffset, length);
        target->setFocusEditor();
    });
}

void MainWindow::updateStatusBar()
{
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab) {
        // An empty document workspace must not retain metadata from the tab
        // that was just closed. The task chip remains independently visible
        // when background work exists.
        if (m_statusPos) m_statusPos->hide();
        if (m_statusEncoding) m_statusEncoding->hide();
        if (m_statusSyntax) m_statusSyntax->hide();
        if (m_statusSize) m_statusSize->hide();
        if (m_statusCharacters) m_statusCharacters->hide();
        if (m_hexEditModeButton) m_hexEditModeButton->hide();
        if (m_statusModified) m_statusModified->hide();
        return;
    }
    if (m_statusPos) m_statusPos->show();
    if (m_statusEncoding) m_statusEncoding->show();
    if (m_statusSyntax) m_statusSyntax->show();
    if (m_statusSize) m_statusSize->show();
    if (m_statusCharacters) m_statusCharacters->show();
    if (m_statusModified) m_statusModified->show();

    int line, index;
    tab->getCursorPosition(&line, &index);
    const bool hexMode = tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex;
    if (m_hexEditModeButton) {
        m_hexEditModeButton->setVisible(hexMode);
        if (hexMode) {
            const bool overwrite = tab->hexOverwriteMode();
            m_hexEditModeButton->setText(overwrite ? QStringLiteral("OVR")
                                                    : QStringLiteral("INS"));
            m_hexEditModeButton->setEnabled(tab->canModifyCommand());
            m_hexEditModeButton->setToolTip(overwrite
                ? QStringLiteral("覆盖模式：输入替换现有字节。单击或按 Insert 切换为插入模式")
                : QStringLiteral("插入模式：输入新增字节。单击或按 Insert 切换为覆盖模式"));
        }
    }
    if (m_hexInsertModeAction) {
        m_hexInsertModeAction->setEnabled(hexMode && tab->canModifyCommand());
        m_hexInsertModeAction->setChecked(hexMode && !tab->hexOverwriteMode());
    }
    if (hexMode) {
        const qint64 offset = qMax<qint64>(0, tab->currentByteOffset());
        m_statusPos->setText(QStringLiteral("偏移: 0x%1")
            .arg(QString::number(offset, 16).rightJustified(8, QLatin1Char('0')).toUpper()));
    } else {
        m_statusPos->setText(QStringLiteral("行: %1, 列: %2").arg(line + 1).arg(index + 1));
    }
    const QString origin = tab->encodingHasDecodeErrors()
        ? QStringLiteral(" · 包含非法字节")
        : tab->encodingOrigin() == EncodingOrigin::Fallback
            ? QStringLiteral(" · 默认假设") : QString();
    m_statusEncoding->setText(hexMode
                                     ? QStringLiteral("原始字节 · 字符区 %1")
                                           .arg(encodingName(tab->displayEncoding()))
                                     : QStringLiteral("%1%2 · %3")
                                           .arg(encodingName(tab->encoding()), origin,
                                                tab->lineEndingName()));

    const QStringList syntaxNames = {
        QStringLiteral("纯文本"), QStringLiteral("C/C++"),
        QStringLiteral("Python"), QStringLiteral("JSON"),
        QStringLiteral("HTML"), QStringLiteral("XML"),
        QStringLiteral("SQL"), QStringLiteral("YAML"),
        QStringLiteral("Markdown"), QStringLiteral("JavaScript"),
        QStringLiteral("Java"), QStringLiteral("C#"),
        QStringLiteral("CSS"), QStringLiteral("Shell"),
        QStringLiteral("Batch"), QStringLiteral("CMake"),
        QStringLiteral("Makefile"), QStringLiteral("Lua"),
        QStringLiteral("Ruby"), QStringLiteral("Perl"),
        QStringLiteral("Diff/Patch"), QStringLiteral("Properties/INI"),
        QStringLiteral("Assembly"),
    };
    int s = static_cast<int>(tab->syntax());
    if (hexMode)
        m_statusSyntax->setText(QStringLiteral("十六进制"));
    else if (s >= 0 && s < syntaxNames.size())
        m_statusSyntax->setText(syntaxNames[s]);

    // Display the live payload size. EditorTab routes this to the active data
    // provider, so an mmap S2/S3 preview never reports the unopened legacy
    // VirtualFileView as 0 B and a secure note never reports its KDBX size.
    const qint64 size = tab->contentSizeBytes();
    QString sizeStr;
    if (size < 1024)
        sizeStr = QStringLiteral("%1 B").arg(size);
    else if (size < 1024 * 1024)
        sizeStr = QStringLiteral("%1 KB").arg(size / 1024.0, 0, 'f', 1);
    else if (size < 1024LL * 1024 * 1024)
        sizeStr = QStringLiteral("%1 MB").arg(size / (1024.0 * 1024), 0, 'f', 2);
    else
        sizeStr = QStringLiteral("%1 GB").arg(size / (1024.0 * 1024 * 1024), 0, 'f', 2);

    const QString documentType = tab->isBinaryDocument()
        ? QStringLiteral("Binary") : QStringLiteral("Text");
    const QString presentation = hexMode
        ? QStringLiteral("Hex") : QStringLiteral("Text");
    const bool readOnlyAnalysis = tab->isReadOnlyByPolicy()
        || tab->largeDocumentRuntimeState()
               == EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis;
    const QString capability = readOnlyAnalysis
        ? QStringLiteral("只读分析") : QStringLiteral("可编辑");
    m_statusSize->setText(QStringLiteral("%1 · %2 · %3 · %4")
                              .arg(sizeStr, documentType, presentation, capability));
    m_statusSize->setToolTip(
        QStringLiteral("源文件实际字节大小 · 文档类型 · 呈现方式 · 当前能力"));

    const qint64 characters = tab->contentCharacterCount();
    if (hexMode) {
        m_statusCharacters->setText(
            QStringLiteral("字节数: %1").arg(QLocale().toString(characters)));
        m_statusCharacters->setToolTip(QStringLiteral("十六进制文档的原始字节总数"));
    } else if (tab->isBinaryDocument()) {
        m_statusCharacters->setText(QStringLiteral("字符数: 不做全文统计"));
        m_statusCharacters->setToolTip(
            QStringLiteral("Binary Text 仅按当前窗口解码，避免低价值全文扫描"));
    } else if (characters >= 0) {
        m_statusCharacters->setText(
            QStringLiteral("字符数: %1").arg(QLocale().toString(characters)));
        m_statusCharacters->setToolTip(QStringLiteral("当前文本的 Unicode 字符总数"));
    } else {
        m_statusCharacters->setText(QStringLiteral("字符数: 统计中…"));
        m_statusCharacters->setToolTip(
            QStringLiteral("正在后台扫描窗口化只读文档，不阻塞浏览"));
    }

    m_statusModified->setText(tab->isModified() ? QStringLiteral("● 已修改") : QStringLiteral(""));
}

void MainWindow::showWorkspaceStatus(const QString &workspaceName)
{
    if (m_statusPos) m_statusPos->hide();
    if (m_statusEncoding) m_statusEncoding->hide();
    if (m_statusSize) m_statusSize->hide();
    if (m_statusCharacters) m_statusCharacters->hide();
    if (m_hexEditModeButton) m_hexEditModeButton->hide();
    if (m_statusModified) m_statusModified->hide();
    if (m_statusSyntax) {
        m_statusSyntax->setText(workspaceName);
        m_statusSyntax->show();
    }
}

// ============ 最近文件 ============

void MainWindow::updateRecentFiles(const QString &path)
{
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    DiskFileVersion candidate;
    const bool candidateValid = captureDiskFileVersion(normalized, &candidate)
                                == DiskFileState::Unchanged;
    for (int i = m_recentFiles.size() - 1; i >= 0; --i) {
        const QString existingPath = m_recentFiles.at(i);
        bool duplicate = sameDiskFilePath(existingPath, normalized);
        if (!duplicate && candidateValid) {
            DiskFileVersion existing;
            duplicate = captureDiskFileVersion(existingPath, &existing)
                            == DiskFileState::Unchanged
                        && sameDiskFile(candidate, existing);
        }
        if (duplicate)
            m_recentFiles.removeAt(i);
    }
    m_recentFiles.prepend(normalized);
    while (m_recentFiles.size() > MAX_RECENT)
        m_recentFiles.removeLast();
    rebuildRecentFilesMenu();
    saveSettings();
}

void MainWindow::rebuildRecentFilesMenu()
{
    m_recentMenu->clear();
    if (m_recentFiles.isEmpty()) {
        auto *act = m_recentMenu->addAction(QStringLiteral("(无)"));
        act->setEnabled(false);
        return;
    }
    for (const QString &file : m_recentFiles) {
        QString name = QFileInfo(file).fileName();
        auto *act = m_recentMenu->addAction(name + "  [" + file + "]");
        act->setData(file);
        connect(act, &QAction::triggered, this, &MainWindow::openRecentFile);
    }
    m_recentMenu->addSeparator();
    auto *clearAct = m_recentMenu->addAction(QStringLiteral("清除列表"));
    connect(clearAct, &QAction::triggered, this, &MainWindow::clearRecentFiles);
}

// ============ 拖放 ============

// 判断 MIME 是否含至少一个可打开的本地文件
static bool mimeHasLocalFiles(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) return false;
    for (const QUrl &u : mime->urls()) {
        QString p = u.toLocalFile();
        if (!p.isEmpty() && QFileInfo(p).isFile())
            return true;
    }
    return false;
}

static QStringList filePathsFromMime(const QMimeData *mime)
{
    QStringList res;
    if (!mime || !mime->hasUrls()) return res;
    for (const QUrl &u : mime->urls()) {
        QString p = u.toLocalFile();
        if (!p.isEmpty() && QFileInfo(p).isFile())
            res << p;
    }
    return res;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // QScintilla consumes editing shortcuts and its native context menu before
    // the window-level QActions can run. Route every editor copy/cut entry path
    // through the same commands so secure-document clipboard fingerprints are
    // always registered and can be conditionally cleared at lock time.
    EditorTab *sourceTab = nullptr;
    for (int index = 0; m_tabs && index < m_tabs->count(); ++index) {
        auto *candidate = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (candidate && candidate->editor()
            && (watched == candidate->editor()
                || watched == candidate->editor()->viewport())) {
            sourceTab = candidate;
            break;
        }
    }
    if (sourceTab && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->matches(QKeySequence::Copy)) {
            if (sourceTab->hasSelectionCommand()) {
                sourceTab->copy();
                rememberSensitiveDocumentClipboard(sourceTab);
            }
            keyEvent->accept();
            return true;
        }
        if (keyEvent->matches(QKeySequence::Cut)) {
            if (sourceTab->hasSelectionCommand()) {
                sourceTab->cut();
                rememberSensitiveDocumentClipboard(sourceTab);
            }
            keyEvent->accept();
            return true;
        }
    }
    if (sourceTab && event->type() == QEvent::ContextMenu) {
        auto *contextEvent = static_cast<QContextMenuEvent *>(event);
        QMenu menu(this);
        menu.addAction(m_undoAction);
        menu.addAction(m_redoAction);
        menu.addSeparator();
        menu.addAction(m_cutAction);
        menu.addAction(m_copyAction);
        menu.addAction(m_pasteAction);
        menu.addSeparator();
        menu.addAction(m_selectAllAction);
        menu.exec(contextEvent->globalPos());
        contextEvent->accept();
        return true;
    }

    // 统一转发 DragEnter/DragMove/Drop：
    // 经过 registerDropProxy 注册过的控件（m_tabs/每个EditorTab/每个QsciScintilla/其viewport）
    // 都会安装本事件过滤器并关闭 acceptDrops。这里把它们的 drag 事件都转给 MainWindow 处理。
    // 这就是修复"拖放到编辑区有时没反应"的关键。
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *e = static_cast<QDragEnterEvent *>(event);
        if (mimeHasLocalFiles(e->mimeData())) {
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *e = static_cast<QDragMoveEvent *>(event);
        if (mimeHasLocalFiles(e->mimeData())) {
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::Drop: {
        auto *e = static_cast<QDropEvent *>(event);
        QStringList paths = filePathsFromMime(e->mimeData());
        if (!paths.isEmpty()) {
            openFilesFromExternal(paths);
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    default: break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (mimeHasLocalFiles(event->mimeData()))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths = filePathsFromMime(event->mimeData());
    if (paths.isEmpty())
        return;
    openFilesFromExternal(paths);
    event->acceptProposedAction();
}
