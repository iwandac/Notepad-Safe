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
    const auto assignMenuId = [](QMenu *menu, const QString &id) {
        menu->setObjectName(id);
        menu->menuAction()->setObjectName(id);
    };
    // 文件菜单
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    assignMenuId(fileMenu, QStringLiteral("menu.file"));

    auto *newAct = fileMenu->addAction(QStringLiteral("新建(&N)"));
    newAct->setObjectName(QStringLiteral("file.new"));
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, &MainWindow::newFile);

    auto *openAct = fileMenu->addAction(QStringLiteral("打开(&O)..."));
    openAct->setObjectName(QStringLiteral("file.open"));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开(&R)"));
    assignMenuId(m_recentMenu, QStringLiteral("file.recent"));
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
    assignMenuId(editMenu, QStringLiteral("menu.edit"));
    auto *undoAct = editMenu->addAction(QStringLiteral("撤销(&U)"));
    m_undoAction = undoAct;
    undoAct->setObjectName(QStringLiteral("edit.undo"));
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->undo();
    });

    auto *redoAct = editMenu->addAction(QStringLiteral("重做(&R)"));
    m_redoAction = redoAct;
    redoAct->setObjectName(QStringLiteral("edit.redo"));
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->redo();
    });

    editMenu->addSeparator();

    auto *cutAct = editMenu->addAction(QStringLiteral("剪切(&T)"));
    m_cutAction = cutAct;
    cutAct->setObjectName(QStringLiteral("edit.cut"));
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
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->paste();
    });

    auto *selectAllAct = editMenu->addAction(QStringLiteral("全选(&A)"));
    m_selectAllAction = selectAllAct;
    selectAllAct->setObjectName(QStringLiteral("edit.selectAll"));
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
    connect(m_hexInsertModeAction, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->toggleHexOverwriteMode();
    });

    editMenu->addSeparator();

    auto *dupLineAct = editMenu->addAction(QStringLiteral("复制当前行(&D)"));
    dupLineAct->setObjectName(QStringLiteral("edit.line.duplicate"));
    dupLineAct->setShortcut(QKeySequence("Ctrl+Shift+D"));
    connect(dupLineAct, &QAction::triggered, this, &MainWindow::duplicateLine);

    auto *delLineAct = editMenu->addAction(QStringLiteral("删除当前行(&L)"));
    delLineAct->setObjectName(QStringLiteral("edit.line.delete"));
    delLineAct->setShortcut(QKeySequence("Ctrl+Shift+L"));
    connect(delLineAct, &QAction::triggered, this, &MainWindow::deleteLine);

    auto *moveUpAct = editMenu->addAction(QStringLiteral("上移当前行"));
    moveUpAct->setObjectName(QStringLiteral("edit.line.moveUp"));
    moveUpAct->setShortcut(QKeySequence("Alt+Up"));
    connect(moveUpAct, &QAction::triggered, this, &MainWindow::moveLineUp);

    auto *moveDownAct = editMenu->addAction(QStringLiteral("下移当前行"));
    moveDownAct->setObjectName(QStringLiteral("edit.line.moveDown"));
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
    sortAscAct->setObjectName(QStringLiteral("edit.lines.sortAscending"));
    connect(sortAscAct, &QAction::triggered, this, &MainWindow::sortLinesAsc);

    auto *sortDescAct = editMenu->addAction(QStringLiteral("行降序排序"));
    sortDescAct->setObjectName(QStringLiteral("edit.lines.sortDescending"));
    connect(sortDescAct, &QAction::triggered, this, &MainWindow::sortLinesDesc);

    editMenu->addSeparator();
    auto *indentMenu = editMenu->addMenu(QStringLiteral("缩进与补全"));
    assignMenuId(indentMenu, QStringLiteral("edit.indentation"));

    m_actUseTabs = indentMenu->addAction(QStringLiteral("使用 Tab 缩进"));
    m_actUseTabs->setObjectName(QStringLiteral("edit.indentation.useTabs"));
    m_actUseTabs->setCheckable(true);
    connect(m_actUseTabs, &QAction::toggled, this, [this](bool enabled) {
        m_useTabs = enabled;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setUseTabs(enabled);
        }
        saveSettings();
    });

    auto *tabWidthMenu = indentMenu->addMenu(QStringLiteral("Tab 宽度"));
    assignMenuId(tabWidthMenu, QStringLiteral("edit.indentation.tabWidth"));
    m_tabWidthGroup = new QActionGroup(this);
    m_tabWidthGroup->setExclusive(true);
    for (const int width : {2, 4, 8}) {
        auto *action = tabWidthMenu->addAction(QStringLiteral("%1 个字符").arg(width));
        action->setObjectName(QStringLiteral("edit.indentation.tabWidth.%1").arg(width));
        action->setCheckable(true);
        action->setData(width);
        m_tabWidthGroup->addAction(action);
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
    assignMenuId(searchMenu, QStringLiteral("menu.search"));
    m_findAction = searchMenu->addAction(QStringLiteral("查找(&F)..."));
    m_findAction->setObjectName(QStringLiteral("search.find"));
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, &MainWindow::showFindDialog);

    m_replaceAction = searchMenu->addAction(QStringLiteral("替换(&R)..."));
    m_replaceAction->setObjectName(QStringLiteral("search.replace"));
    // 用户强制：菜单显示 (&R) → 全局快捷键必须为 Ctrl+R 显示一致（Notepad++ 习惯），禁止 Ctrl+H
    // 见 QA SR02-COMFORT 3 条硬断言：text 含 &R + shortcut==QKeySequence("Ctrl+R") 精确 + Alt+S+R 访问键可打开
    m_replaceAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(m_replaceAction, &QAction::triggered, this, &MainWindow::showReplaceDialog);

    searchMenu->addSeparator();

    auto *findNextAct = searchMenu->addAction(QStringLiteral("查找下一个"));
    findNextAct->setObjectName(QStringLiteral("search.next"));
    findNextAct->setShortcut(QKeySequence("F3"));
    connect(findNextAct, &QAction::triggered, this, &MainWindow::findNext);

    auto *findPrevAct = searchMenu->addAction(QStringLiteral("查找上一个"));
    findPrevAct->setObjectName(QStringLiteral("search.previous"));
    findPrevAct->setShortcut(QKeySequence("Shift+F3"));
    connect(findPrevAct, &QAction::triggered, this, &MainWindow::findPrev);

    searchMenu->addSeparator();
    auto *matchingBraceAct = searchMenu->addAction(QStringLiteral("跳转到匹配括号"));
    matchingBraceAct->setObjectName(QStringLiteral("search.brace.match"));
    matchingBraceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+]")));
    connect(matchingBraceAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab()) tab->moveToMatchingBrace();
    });
    auto *selectBraceAct = searchMenu->addAction(QStringLiteral("选择到匹配括号"));
    selectBraceAct->setObjectName(QStringLiteral("search.brace.select"));
    selectBraceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+]")));
    connect(selectBraceAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab()) tab->selectToMatchingBrace();
    });

    // 语言菜单
    auto *syntaxMenu = menuBar()->addMenu(QStringLiteral("语言(&L)"));
    assignMenuId(syntaxMenu, QStringLiteral("menu.language"));
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
        if (i == 0) act->setChecked(true);
        connect(act, &QAction::triggered, this, &MainWindow::setSyntaxFromAction);
    }

    // 书签属于搜索导航，不再占用独立顶级菜单。
    auto *bookmarkMenu = searchMenu->addMenu(QStringLiteral("书签(&B)"));
    assignMenuId(bookmarkMenu, QStringLiteral("search.bookmarks"));

    auto *toggleBmAct = bookmarkMenu->addAction(QStringLiteral("切换书签(&M)"));
    toggleBmAct->setObjectName(QStringLiteral("bookmark.toggle"));
    toggleBmAct->setShortcut(QKeySequence("Ctrl+F2"));
    connect(toggleBmAct, &QAction::triggered, this, &MainWindow::toggleBookmark);

    auto *nextBmAct = bookmarkMenu->addAction(QStringLiteral("下一书签"));
    nextBmAct->setObjectName(QStringLiteral("bookmark.next"));
    nextBmAct->setShortcut(QKeySequence("F2"));
    connect(nextBmAct, &QAction::triggered, this, &MainWindow::nextBookmark);


    auto *prevBmAct = bookmarkMenu->addAction(QStringLiteral("上一书签"));
    prevBmAct->setObjectName(QStringLiteral("bookmark.previous"));
    prevBmAct->setShortcut(QKeySequence("Shift+F2"));
    connect(prevBmAct, &QAction::triggered, this, &MainWindow::prevBookmark);

    bookmarkMenu->addSeparator();

    auto *clearBmAct = bookmarkMenu->addAction(QStringLiteral("清除所有书签"));
    clearBmAct->setObjectName(QStringLiteral("bookmark.clear"));

    connect(clearBmAct, &QAction::triggered, this, &MainWindow::clearBookmarks);

    // 视图菜单
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    assignMenuId(viewMenu, QStringLiteral("menu.view"));

    auto *appearanceMenu = viewMenu->addMenu(QStringLiteral("外观"));
    assignMenuId(appearanceMenu, QStringLiteral("view.appearance"));
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
    m_actWordWrap->setCheckable(true);
    m_actWordWrap->setChecked(m_wordWrap);
    connect(m_actWordWrap, &QAction::triggered, this, &MainWindow::toggleWordWrap);

    auto *splitMenu = viewMenu->addMenu(QStringLiteral("分屏"));
    assignMenuId(splitMenu, QStringLiteral("view.split"));
    m_actSplitHorizontal = splitMenu->addAction(QStringLiteral("左右分屏"));
    m_actSplitHorizontal->setObjectName(QStringLiteral("view.split.horizontal"));
    m_actSplitHorizontal->setCheckable(true);
    m_actSplitVertical = splitMenu->addAction(QStringLiteral("上下分屏"));
    m_actSplitVertical->setObjectName(QStringLiteral("view.split.vertical"));
    m_actSplitVertical->setCheckable(true);
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
    assignMenuId(symbolsMenu, QStringLiteral("view.symbols"));
    m_actWhitespaceVisible = symbolsMenu->addAction(QStringLiteral("显示空格与制表符"));
    m_actWhitespaceVisible->setObjectName(QStringLiteral("view.symbols.whitespace"));
    m_actWhitespaceVisible->setCheckable(true);
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
    connect(m_actEolVisible, &QAction::toggled, this, [this](bool visible) {
        m_eolVisible = visible;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i)))
                tab->setEolVisible(visible);
        }
        saveSettings();
    });

    auto *foldMenu = viewMenu->addMenu(QStringLiteral("代码折叠"));
    assignMenuId(foldMenu, QStringLiteral("view.folding"));
    auto addFoldAction = [this, foldMenu](const QString &text,
                                          const QString &name,
                                          auto callback) {
        auto *action = foldMenu->addAction(text);
        action->setObjectName(name);
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
    assignMenuId(presentationMenu, QStringLiteral("view.presentation"));
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
    connect(togglePresentation, &QAction::triggered,
            this, &MainWindow::toggleHexViewMode);

    auto *edgeMenu = viewMenu->addMenu(QStringLiteral("长行标尺"));
    assignMenuId(edgeMenu, QStringLiteral("view.edge"));
    m_actEdgeVisible = edgeMenu->addAction(QStringLiteral("显示标尺"));
    m_actEdgeVisible->setObjectName(QStringLiteral("view.edge.visible"));
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
        if (col == m_edgeColumn) a->setChecked(true);
        connect(a, &QAction::triggered, this, &MainWindow::setEdgeColumnFromAction);
    }

    viewMenu->addSeparator();

    auto *zoomMenu = viewMenu->addMenu(QStringLiteral("缩放"));
    assignMenuId(zoomMenu, QStringLiteral("view.zoom"));
    auto *zoomInAct = zoomMenu->addAction(QStringLiteral("放大(&+)"));
    zoomInAct->setObjectName(QStringLiteral("view.zoom.in"));
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::zoomIn);

    auto *zoomOutAct = zoomMenu->addAction(QStringLiteral("缩小(&-)"));
    zoomOutAct->setObjectName(QStringLiteral("view.zoom.out"));
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::zoomOut);

    auto *resetZoomAct = zoomMenu->addAction(QStringLiteral("重置缩放"));
    resetZoomAct->setObjectName(QStringLiteral("view.zoom.reset"));
    resetZoomAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(resetZoomAct, &QAction::triggered, this, &MainWindow::resetZoom);

    viewMenu->addSeparator();
    auto *panelMenu = viewMenu->addMenu(QStringLiteral("面板"));
    assignMenuId(panelMenu, QStringLiteral("view.panels"));
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
    assignMenuId(m_encodingMenu, QStringLiteral("menu.encoding"));
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
    for (int i = 0; i < encItems.size(); ++i) {
        const auto &item = encItems[i];
        auto *a = m_encodingMenu->addAction(item.first);
        a->setObjectName(QStringLiteral("encoding.display.%1")
                             .arg(convertEncodingIds[i]));
        a->setCheckable(true);
        a->setData(static_cast<int>(item.second));
        encGroup->addAction(a);
        connect(a, &QAction::triggered, this, &MainWindow::displayEncodingFromAction);
    }

    m_encodingMenu->addSeparator();
    auto *eolMenu = m_encodingMenu->addMenu(QStringLiteral("换行符转换"));
    assignMenuId(eolMenu, QStringLiteral("encoding.eol"));
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
    assignMenuId(convertMenu, QStringLiteral("encoding.convert"));
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
    for (int i = 0; i < dispEncItems.size(); ++i) {
        const auto &item = dispEncItems[i];
        auto *a = convertMenu->addAction(item.first);
        a->setObjectName(QStringLiteral("encoding.convert.%1")
                             .arg(convertEncodingIds[i]));
        a->setData(static_cast<int>(item.second));
        connect(a, &QAction::triggered, this, &MainWindow::convertEncodingFromAction);
    }
    // 注意：菜单勾选在 onTabChanged / onTabEncodingChanged 中同步

    // 帮助菜单
    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    assignMenuId(toolsMenu, QStringLiteral("menu.tools"));
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
    assignMenuId(helpMenu, QStringLiteral("menu.help"));
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
