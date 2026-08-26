#include "MainWindow.h"
#include "EditorTab.h"
#include "FileContentProbe.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SessionManager.h"
#include "TextEncoding.h"
#include "FileTierPolicy.h"
#include "VirtualFileView.h"
#include "StatusBarTaskWidget.h"
#include "AsyncSerialQueue.h"
#include "AsyncUiOperation.h"
#include "VaultDock.h"
#include "KdbxFormatProbe.h"
#include "SecureDocumentService.h"
#include "SecurityServiceFactory.h"
#include "SecuritySessionService.h"
#include "AppComposition.h"
#include "CommandRegistry.h"

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

void scrubByteArray(QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
#ifdef Q_OS_WIN
    ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
    volatile char *cursor = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        cursor[i] = 0;
#endif
    bytes.clear();
}

QString takeSensitiveLineEdit(QLineEdit *edit)
{
    if (!edit)
        return {};
    QString sensitive = edit->text();
    if (!sensitive.isEmpty()) {
        // Replace the widget's reference first. The returned QString then owns
        // the old buffer and its caller can overwrite it after use.
        edit->setText(QString(sensitive.size(), QChar(0)));
        edit->clear();
    }
    return sensitive;
}

void clearSensitiveLineEdit(QLineEdit *edit)
{
    QString sensitive = takeSensitiveLineEdit(edit);
    if (!sensitive.isEmpty())
        sensitive.fill(QChar(0));
    sensitive.clear();
}

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
class FileCredentialDialog final : public QDialog {
public:
    enum class Kind { Passphrase, RecoveryKey };

    FileCredentialDialog(const QString &title, const QString &message,
                         bool allowRecovery, QWidget *parent)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("fileCredentialDialog"));
        setWindowTitle(title);
        setMinimumWidth(500);
        auto *layout = new QVBoxLayout(this);
        auto *description = new QLabel(message, this);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto *form = new QFormLayout();
        kind_ = new QComboBox(this);
        kind_->setObjectName(QStringLiteral("fileCredentialKind"));
        kind_->addItem(QStringLiteral("文件密码"), static_cast<int>(Kind::Passphrase));
        if (allowRecovery)
            kind_->addItem(QStringLiteral("恢复密钥"), static_cast<int>(Kind::RecoveryKey));
        form->addRow(QStringLiteral("解锁方式"), kind_);

        fields_ = new QStackedWidget(this);
        passphrase_ = new QLineEdit(fields_);
        passphrase_->setObjectName(QStringLiteral("fileCredentialPassphrase"));
        passphrase_->setEchoMode(QLineEdit::Password);
        passphrase_->setClearButtonEnabled(true);
        fields_->addWidget(passphrase_);
        recoveryKey_ = new QLineEdit(fields_);
        recoveryKey_->setObjectName(QStringLiteral("fileCredentialRecoveryKey"));
        recoveryKey_->setEchoMode(QLineEdit::Password);
        recoveryKey_->setClearButtonEnabled(true);
        recoveryKey_->setPlaceholderText(QStringLiteral("NSR1-…"));
        fields_->addWidget(recoveryKey_);
        form->addRow(QStringLiteral("凭据"), fields_);
        layout->addLayout(form);

        auto *showInput = new QCheckBox(QStringLiteral("显示输入"), this);
        showInput->setObjectName(QStringLiteral("fileCredentialShowInput"));
        layout->addWidget(showInput);
        error_ = new QLabel(this);
        error_->setObjectName(QStringLiteral("fileCredentialError"));
        error_->setStyleSheet(QStringLiteral("color:#b00020"));
        error_->setWordWrap(true);
        layout->addWidget(error_);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel,
                                             Qt::Horizontal, this);
        buttons->setObjectName(QStringLiteral("fileCredentialButtons"));
        buttons->button(QDialogButtonBox::Open)->setText(QStringLiteral("打开"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(kind_, &QComboBox::currentIndexChanged, this, [this](int) {
            clearSecrets();
            error_->clear();
            fields_->setCurrentIndex(kind() == Kind::RecoveryKey ? 1 : 0);
            focusCredential();
        });
        connect(showInput, &QCheckBox::toggled, this, [this](bool shown) {
            const auto mode = shown ? QLineEdit::Normal : QLineEdit::Password;
            passphrase_->setEchoMode(mode);
            recoveryKey_->setEchoMode(mode);
        });
        passphrase_->setFocus();
    }

    ~FileCredentialDialog() override { clearSecrets(); }

    Kind kind() const
    {
        return static_cast<Kind>(kind_->currentData().toInt());
    }

    SecureBuffer takePassphrase()
    {
        QString text = takeSensitiveLineEdit(passphrase_);
        QByteArray utf8 = text.toUtf8();
        if (!text.isEmpty())
            text.fill(QChar(0));
        text.clear();
        SecureBuffer result(utf8.constData(), utf8.size());
        scrubByteArray(utf8);
        return result;
    }

    QString takeRecoveryKey()
    {
        QString raw = takeSensitiveLineEdit(recoveryKey_);
        QString result = raw.trimmed();
        if (!raw.isEmpty())
            raw.fill(QChar(0));
        raw.clear();
        return result;
    }

    void showError(const QString &message)
    {
        error_->setText(message);
        focusCredential();
    }

    void clearSecrets()
    {
        clearSensitiveLineEdit(passphrase_);
        clearSensitiveLineEdit(recoveryKey_);
    }

private:
    void focusCredential()
    {
        if (kind() == Kind::RecoveryKey)
            recoveryKey_->setFocus();
        else
            passphrase_->setFocus();
    }

    QComboBox *kind_ = nullptr;
    QStackedWidget *fields_ = nullptr;
    QLineEdit *passphrase_ = nullptr;
    QLineEdit *recoveryKey_ = nullptr;
    QLabel *error_ = nullptr;
};
#endif

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

// S1/S2/S3 自动分流打开文件（DESIGN §5.2 固定产品边界）：
//   S1 → EditorTab::loadFile (QsciScintilla 完整编辑)
//   S2/S3 → 只允许 mmap 路径。失败时绝不回退到主线程 readAll；否则一个
//           2 GiB+ 文件会把“只读分析”退化成界面卡死或进程内存耗尽。
// 返回是否成功（与 loadFile 返回一致）
static bool openTabAutoMode(EditorTab *tab, const QString &path,
                            std::optional<TextEncoding> forcedEncoding = std::nullopt)
{
    if (!tab) return false;
    const FileContentProbeResult probe = probeFileContent(path);
    // Binary routing belongs to the shared document opener so normal Open,
    // recent files, drag/drop and session restore cannot disagree.
    if (!forcedEncoding && probe.contentKind == DocumentContentKind::Binary) {
        QString error;
        if (!tab->openLargeFileL2(path, &error,
                                  L2Presentation::Hex,
                                  L2ContentKind::Binary)) {
            qWarning("[openTabAutoMode] binary mmap open failed: path='%s', error='%s'",
                     qUtf8Printable(path), qUtf8Printable(error));
            return false;
        }
        tab->setViewModeL2(ViewMode::Hex);
        return true;
    }
    const FileTierDecision decision = probe.tier;
    if (decision.usesMappedPreview()) {
        QString err;
        if (tab->openLargeFileL2(path, &err, L2Presentation::Text,
                                 L2ContentKind::Text, forcedEncoding))
            return true;
        qWarning("[openTabAutoMode] mmap 打开失败，拒绝回退到同步全量加载: path='%s', error='%s'",
                 qUtf8Printable(path), qUtf8Printable(err));
        return false;
    }
    return tab->loadFile(path, forcedEncoding);
}

static bool openSessionFile(EditorTab *tab, const SessionTab &state)
{
    if (!tab || state.filePath.isEmpty())
        return false;
    if (state.presentation == QLatin1String("hex")) {
        QString error;
        if (!tab->openLargeFileL2(state.filePath, &error,
                                  L2Presentation::Hex,
                                  L2ContentKind::Binary)) {
            qWarning("[session] explicit Hex restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    if (state.presentation == QLatin1String("text")
        && probeFileContent(state.filePath).contentKind
               == DocumentContentKind::Binary) {
        QString error;
        // The user explicitly overrode Binary/Unknown classification with a
        // text presentation. Preserve that choice across restart while keeping
        // a potentially huge/arbitrary byte stream on the read-only provider.
        if (!tab->openLargeFileL2(state.filePath, &error,
                                  L2Presentation::Text,
                                  L2ContentKind::Binary)) {
            qWarning("[session] explicit Text restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    return openTabAutoMode(tab, state.filePath);
}

static void restoreSessionPosition(EditorTab *tab, const SessionTab &state)
{
    if (!tab)
        return;
    if (state.cursorByteOffset >= 0) {
        const qint64 selectionStart = qMin(state.selectionStartByte,
                                           state.selectionEndByte);
        const qint64 selectionEnd = qMax(state.selectionStartByte,
                                         state.selectionEndByte);
        if (selectionStart >= 0 && selectionEnd > selectionStart) {
            tab->gotoByteOffset(selectionStart, int(qMin<qint64>(
                selectionEnd - selectionStart, INT_MAX)));
        } else {
            tab->gotoByteOffset(state.cursorByteOffset);
        }
        return;
    }
    tab->setCursorPosition(state.cursorLine, state.cursorIndex);
}

static void restoreSessionSplit(EditorTab *tab, const SessionTab &state)
{
    if (!tab || !state.splitView || !tab->canSplitView())
        return;
    tab->setSplitView(
        true, state.splitOrientation == QLatin1String("vertical")
                  ? Qt::Vertical : Qt::Horizontal);
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
    m_searchDock = new SearchResultsDock(this);
    m_searchDock->setObjectName(QStringLiteral("SearchResultsDock"));
    addDockWidget(Qt::BottomDockWidgetArea, m_searchDock);
    m_searchDock->hide();
    connect(m_searchDock, &SearchResultsDock::jumpToByteOffset,
            this, &MainWindow::onJumpToByteOffset);
    connect(m_searchDock, &SearchResultsDock::jumpToTabByteOffset,
            this, &MainWindow::onJumpToTabByteOffset);

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
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    QMessageBox::critical(this, QStringLiteral("账户功能不可用"),
                          QStringLiteral("当前构建未包含 KDBX 安全后端。"));
    return false;
#else
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
#endif
}

bool MainWindow::showSecurityCenterWorkspace()
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    QMessageBox::critical(this, QStringLiteral("安全功能不可用"),
                          QStringLiteral("当前构建未包含 KDBX 安全后端。"));
    return false;
#else
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
#endif
}

void MainWindow::refreshSecurityCenter()
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    const bool hasIdentity = m_securitySession && m_securitySession->hasIdentity();
    const bool active = hasIdentity && m_securitySession->hasActiveSession();
    if (m_securitySessionStatus) {
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
#else
    if (m_securitySessionStatus)
        m_securitySessionStatus->setText(QStringLiteral("当前构建未包含 KDBX 安全后端。"));
#endif
}

void MainWindow::changeMasterPassword()
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    return;
#else
    if (!m_securitySession || !m_securitySession->hasActiveSession())
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
#endif
}

void MainWindow::exportRecoveryKey()
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    return;
#else
    if (!m_securitySession || !m_securitySession->hasActiveSession())
        return;
    SecurityOperationLease lease;
    const VaultError leaseError = m_securitySession->acquireOperationLease(&lease);
    if (leaseError.isError()) {
        QMessageBox::critical(this, QStringLiteral("无法读取恢复密钥"),
                              leaseError.userMessage);
        return;
    }
    QString recoveryKey = SecurityIdentity::recoveryKeyText(lease.recoverySecret());
    lease.clear();

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
#endif
}

void MainWindow::verifyRecoveryKey()
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    return;
#else
    if (!m_securitySession || !m_securitySession->hasActiveSession())
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
        SecureBuffer candidate;
        VaultError result = SecurityIdentity::parseRecoveryKey(
            recoveryText, &candidate);
        if (!recoveryText.isEmpty())
            recoveryText.fill(QChar(0));
        recoveryText.clear();
        if (!result.isError()) {
            result = runSerializedSecurityOperation(
                this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
                QStringLiteral("正在验证恢复密钥"),
                [this, &candidate] {
                    return m_securitySession->verifyRecoverySecret(candidate);
                });
        }
        candidate.clear();
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
#endif
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

// ============ 文件操作 ============

void MainWindow::ensureSecurityServicesCreated()
{
    if (m_securitySession && m_secureDocuments)
        return;
    SecurityServiceBundle services = m_composition->createSecurityServices();
    m_securitySession = std::move(services.session);
    m_secureDocuments = std::move(services.documents);
}

void MainWindow::ensureFindDialogCreated()
{
    if (m_findDialog)
        return;
    m_composition->recordHeavyFactoryCall();
    m_findDialog = new FindReplaceDialog(this);
    m_findDialog->setSearchResultsDock(m_searchDock);
    m_findDialog->setTabWidget(m_tabs);
    m_findDialog->setActiveTab(currentTab());
    connect(m_findDialog, &FindReplaceDialog::findParametersChanged,
            this, [this](const QString &pattern, bool regex, bool cs, bool wo) {
        m_lastFindPattern = pattern;
        m_lastFindRegex = regex;
        m_lastFindCase = cs;
        m_lastFindWord = wo;
    });
}

bool MainWindow::ensureSecuritySessionInteractive()
{
    ensureSecurityServicesCreated();
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    QMessageBox::critical(this, QStringLiteral("安全功能不可用"),
                          QStringLiteral("当前构建未包含 KDBX 安全后端。"));
    return false;
#else
    const auto restoreMaskedTabs = [this]() {
        if (m_secureDocuments) {
            const int activeIndex = m_tabs ? m_tabs->currentIndex() : -1;
            for (int i = 0; m_tabs && i < m_tabs->count(); ++i) {
                auto *masked = qobject_cast<EditorTab *>(m_tabs->widget(i));
                if (!masked || !masked->isSecureDocument()
                    || !masked->isSecureDocumentMasked()) {
                    continue;
                }
                const QString backing = masked->filePath().isEmpty()
                                            ? masked->autosavePath() : masked->filePath();
                if (backing.isEmpty() || !QFileInfo::exists(backing))
                    continue;
                SecureDocumentSnapshot restored;
                const VaultError restoreError = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                    QStringLiteral("正在恢复安全文档"),
                    [this, backing, &restored] {
                        return m_secureDocuments->open(backing, &restored);
                    });
                if (!restoreError.isError()) {
                    adoptSecureSnapshot(restored, masked);
                    restored.text.fill(QChar(0));
                }
            }
            if (activeIndex >= 0 && activeIndex < m_tabs->count())
                m_tabs->setCurrentIndex(activeIndex);
        }
    };
    if (m_securitySession && m_securitySession->hasActiveSession()) {
        restoreMaskedTabs();
        return true;
    }
    if (!m_securitySession)
        return false;

    const bool creating = !m_securitySession->hasIdentity();
    QDialog dialog(this);
    dialog.setWindowTitle(creating ? QStringLiteral("建立本地安全身份")
                                   : QStringLiteral("登录 Notepad Safe"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(
        creating
            ? QStringLiteral("设置一个主密码。文件始终离线加密；本设备登录后 7 天内使用无提示。")
            : QStringLiteral("输入主密码以开始新的 7 天本机会话。"),
        &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *form = new QFormLayout();
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("securitySessionPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("主密码"), password);
    QLineEdit *confirmation = nullptr;
    if (creating) {
        confirmation = new QLineEdit(&dialog);
        confirmation->setObjectName(QStringLiteral("securitySessionConfirmation"));
        confirmation->setEchoMode(QLineEdit::Password);
        form->addRow(QStringLiteral("确认主密码"), confirmation);
    }
    layout->addLayout(form);
    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setStyleSheet(QStringLiteral("color:#b00020"));
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(creating ? QStringLiteral("建立身份")
                                                            : QStringLiteral("登录"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    password->setFocus();

    while (dialog.exec() == QDialog::Accepted) {
        QString passwordText = takeSensitiveLineEdit(password);
        QString confirmationText = creating && confirmation
            ? takeSensitiveLineEdit(confirmation) : QString();
        if (passwordText.size() < 12) {
            errorLabel->setText(QStringLiteral("主密码至少需要 12 个字符。"));
            passwordText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            password->setFocus();
            continue;
        }
        if (creating && confirmation && passwordText != confirmationText) {
            errorLabel->setText(QStringLiteral("两次输入的主密码不一致。"));
            passwordText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            password->setFocus();
            continue;
        }
        QByteArray passwordUtf8 = passwordText.toUtf8();
        passwordText.fill(QChar(0));
        confirmationText.fill(QChar(0));
        SecureBuffer passphrase(passwordUtf8.constData(), passwordUtf8.size());
        scrubByteArray(passwordUtf8);
        QString recoveryKey;
        VaultError result = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
            creating ? QStringLiteral("正在建立安全身份")
                     : QStringLiteral("正在解锁安全身份"),
            [this, creating, &passphrase, &recoveryKey] {
                return creating
                    ? m_securitySession->createIdentity(passphrase, &recoveryKey)
                    : m_securitySession->unlockIdentity(passphrase);
            });
        passphrase.clear();
        if (result.isError()) {
            errorLabel->setText(result.userMessage);
            password->setFocus();
            continue;
        }

        if (creating) {
            QDialog recoveryDialog(this);
            recoveryDialog.setWindowTitle(QStringLiteral("保存恢复密钥"));
            recoveryDialog.setMinimumWidth(620);
            auto *recoveryLayout = new QVBoxLayout(&recoveryDialog);
            auto *recoveryMessage = new QLabel(
                QStringLiteral("恢复密钥是主密码丢失后的唯一恢复方式。请保存到本设备之外；"
                               "Notepad Safe 和任何厂商都无法替你找回。"),
                &recoveryDialog);
            recoveryMessage->setWordWrap(true);
            recoveryLayout->addWidget(recoveryMessage);
            auto *keyView = new QPlainTextEdit(recoveryKey, &recoveryDialog);
            keyView->setReadOnly(true);
            keyView->setMaximumHeight(96);
            keyView->setObjectName(QStringLiteral("securityRecoveryKey"));
            recoveryLayout->addWidget(keyView);
            auto *verify = new QLineEdit(&recoveryDialog);
            verify->setPlaceholderText(QStringLiteral("输入恢复密钥最后 8 个字符以确认已保存"));
            recoveryLayout->addWidget(verify);
            auto *finish = new QDialogButtonBox(QDialogButtonBox::Ok,
                                                 Qt::Horizontal, &recoveryDialog);
            finish->button(QDialogButtonBox::Ok)->setText(QStringLiteral("完成"));
            finish->button(QDialogButtonBox::Ok)->setEnabled(false);
            recoveryLayout->addWidget(finish);
            const QString suffix = recoveryKey.right(8);
            connect(verify, &QLineEdit::textChanged, finish->button(QDialogButtonBox::Ok),
                    [button = finish->button(QDialogButtonBox::Ok), suffix](const QString &text) {
                        button->setEnabled(text.trimmed().compare(suffix,
                                                                  Qt::CaseInsensitive) == 0);
                    });
            connect(finish, &QDialogButtonBox::accepted,
                    &recoveryDialog, &QDialog::accept);
            recoveryDialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
            while (recoveryDialog.exec() != QDialog::Accepted) {
                // Identity creation is already durable at this point. Do not let a
                // dismissed dialog silently skip the only recovery-key handoff.
            }
            keyView->clear();
            clearSensitiveLineEdit(verify);
            recoveryKey.fill(QChar(0));
            recoveryKey.clear();
        }
        restoreMaskedTabs();
        return true;
    }
    clearSensitiveLineEdit(password);
    if (confirmation)
        clearSensitiveLineEdit(confirmation);
    return false;
#endif
}

void MainWindow::newFile()
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    if (!ensureSecuritySessionInteractive() || !m_secureDocuments)
        return;
    SecureDocumentSnapshot snapshot;
    const VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
        QStringLiteral("正在建立加密草稿"),
        [this, &snapshot] { return m_secureDocuments->newDraft(&snapshot); });
    if (error.isError()) {
        QMessageBox::critical(this, QStringLiteral("无法新建文档"), error.userMessage);
        return;
    }
    auto *tab = addNewTab();
    tab->initializeSecureDocument(snapshot.documentId, snapshot.title,
                                  snapshot.text, snapshot.filePath);
    tab->setAutosavePath(snapshot.backingPath);
    m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    statusBar()->showMessage(QStringLiteral("已新建文档"), 1800);
#else
    addNewTab();
#endif
}

EditorTab *MainWindow::addNewTab()
{
    showDocumentWorkspace();
    auto *tab = new EditorTab(this);
    int index = m_tabs->addTab(tab, QStringLiteral("未命名"));

    // 注册拖放代理：保证拖到这个 EditorTab/editor/viewport 任意位置都能触发 MainWindow 打开
    registerDropProxy(tab);
    connect(tab, &EditorTab::externalDropSurfaceCreated,
            this, &MainWindow::registerDropProxy);

    // 应用全局设置（标尺 + 换行）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);
    applyEditorPreferences(tab);
    // QScintilla keeps its own colors outside the QWidget palette. Apply the
    // resolved appearance to newly created/restored tabs as well as existing ones.
    applyDarkMode(m_darkMode);

    connect(tab, &EditorTab::modificationChanged, this, [this, tab](bool m) {
        if (m && tab->isSecureDocument() && !tab->isSecureDocumentTransitioning()) {
            tab->setSecureDocumentTitle(secureTitleFrom(tab));
            m_secureAutosaveTimer.start(5000);
        }
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0) {
            onTabModifiedChanged(m);
            m_tabs->setTabText(idx, tab->tabTitle());
        }
        if (currentTab() == tab)
            syncCommandStates(tab);
    });
    connect(tab, &EditorTab::commandCapabilitiesChanged,
            this, [this, tab]() {
        if (currentTab() == tab)
            syncCommandStates(tab);
    });
    connect(tab, &EditorTab::largeDocumentRuntimeStateChanged,
            this, [this, tab](EditorTab::LargeDocumentRuntimeState, const QString &) {
        if (currentTab() != tab)
            return;
        syncCommandStates(tab);
        updateStatusBar();
    });
    connect(tab, &EditorTab::contentChanged, this, [this, tab](quint64) {
        if (currentTab() == tab)
            m_characterStatusTimer.start();
    });
    connect(tab, &EditorTab::characterCountChanged, this, [this, tab]() {
        if (currentTab() == tab)
            updateStatusBar();
    });
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    connect(tab, &EditorTab::contentChanged, this, [this, tab](quint64) {
        if (!tab->isSecureDocument() || tab->isBinaryDocument()
            || tab->isSecureDocumentMasked()
            || tab->isSecureDocumentTransitioning()
            || tab->securePayloadLimitExclusive() <= 0
            || tab->editor()->length() < tab->securePayloadLimitExclusive()
            || tab->property("securePayloadLimitUndoPending").toBool()) {
            return;
        }
        tab->setProperty("securePayloadLimitUndoPending", true);
        QPointer<EditorTab> guarded(tab);
        QTimer::singleShot(0, tab, [this, guarded]() {
            if (!guarded)
                return;
            guarded->setProperty("securePayloadLimitUndoPending", false);
            if (!guarded->isSecureDocument() || guarded->isBinaryDocument()
                || guarded->editor()->length()
                       < guarded->securePayloadLimitExclusive()) {
                return;
            }
            guarded->undo();
            const bool restored = guarded->editor()->length()
                                  < guarded->securePayloadLimitExclusive();
            if (!restored)
                guarded->editor()->setReadOnly(true);
            statusBar()->showMessage(
                restored
                    ? QStringLiteral("安全文档必须小于 32 MiB；本次编辑已撤销。请另存为普通文件后继续。")
                    : QStringLiteral("安全文档已达到大小上限并已停止编辑；请另存为普通文件。"),
                8000);
            QApplication::beep();
        });
    });
#endif
    connect(tab, &EditorTab::cursorPositionChanged, this, &MainWindow::onTabCursorPositionChanged);
    connect(tab, &EditorTab::titleChanged, this, [this, tab](const QString &) {
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0)
            m_tabs->setTabText(idx, tab->tabTitle());
    });
    connect(tab, &EditorTab::encodingChanged, this, &MainWindow::onTabEncodingChanged);
    connect(tab, &EditorTab::syntaxChanged, this, [this](Syntax) {
        updateStatusBar();
    });
    // 秒开：L2 后台行索引构建进度 → 非模态状态栏显示（只更新当前激活 tab 的进度）
    connect(tab, &EditorTab::l2LineBuildProgress,
            this, [this, tab](qint64 done, qint64 total) {
        if (currentTab() != tab) return;
        if (tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
            statusBar()->clearMessage();
            return;
        }
        if (total <= 0) {
            statusBar()->clearMessage();
            return;
        }
        int pct = qBound(0, int(done * 100 / total), 100);
        QString msg = QObject::tr("正在建立行索引… %1%  (%2 / %3 MB)")
            .arg(pct)
            .arg(static_cast<double>(done) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(static_cast<double>(total) / (1024.0 * 1024.0), 0, 'f', 1);
        statusBar()->showMessage(msg, 0);
    });
    connect(tab, &EditorTab::l2LineBuildFinished, this, [this, tab](bool ok) {
        if (currentTab() != tab) return;
        if (tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
            statusBar()->clearMessage();
            return;
        }
        if (ok) statusBar()->showMessage(QObject::tr("行索引构建完成"), 3000);
        else    statusBar()->clearMessage();
    });
    connect(tab, &EditorTab::l2FileOpenedChanged, this, [this, tab](bool) {
        if (currentTab() == tab)
            updateStatusBar();
    });
    connect(tab, &EditorTab::l2SaveNeededChanged, this, [this, tab](bool) {
        if (currentTab() == tab)
            updateStatusBar();
    });

    // ============== L2 禁用不适配菜单：同一 tab 内部 L1↔L2 切换时立即同步（关键：addTab/currentChanged 不触发）==============
    // 最小侵入：直接用 sender() 转 EditorTab* 当 tab 参数（精准指向信号发射源，避免无参
    // syncCommandStates() → currentTab() 在 currentIndex 未切好时查到旧 tab → 新 tab 的
    // L2 状态被旧 tab 覆盖（enabled=true 不变灰的 D001/D003/D007 根因）。
    connect(tab, &EditorTab::l2ModeStatusChanged, this, [this]() {
        auto* src = qobject_cast<EditorTab*>(sender());
        syncCommandStates(src ? src : currentTab());
        if (!src || src == currentTab())
            syncGlobalMenuCheckStates();
    });

    m_tabs->setCurrentIndex(index);
    if (m_findDialog) {
        m_findDialog->setActiveTab(tab);
        m_findDialog->notifyTabOpened(tab);
    }
    // 新建后立刻同步一次 L2 敏感动作（覆盖新 tab 还没收到任何信号的早期空状态）
    syncCommandStates(tab);
    return tab;
}

void MainWindow::applyEditorPreferences(EditorTab *tab)
{
    if (!tab)
        return;
    tab->setWhitespaceVisible(m_whitespaceVisible);
    tab->setEolVisible(m_eolVisible);
    tab->setUseTabs(m_useTabs);
    tab->setTabWidth(m_tabWidth);
    tab->setBackspaceUnindents(m_backspaceUnindents);
    tab->setWordCompletionEnabled(m_wordCompletion);
}

QString MainWindow::secureTitleFrom(EditorTab *tab)
{
    if (!tab || !tab->editor())
        return QStringLiteral("未命名");
    if (tab->isBinaryDocument()) {
        if (tab->isSecureDocument() && !tab->secureDocumentTitle().trimmed().isEmpty()
            && tab->secureDocumentTitle() != QStringLiteral("未命名")) {
            return tab->secureDocumentTitle();
        }
        if (!tab->filePath().isEmpty())
            return QFileInfo(tab->filePath()).completeBaseName().left(80);
        return QStringLiteral("未命名");
    }
    const int lines = qMin(tab->editor()->lines(), 8);
    for (int i = 0; i < lines; ++i) {
        QString line = tab->editor()->text(i).trimmed();
        while (line.startsWith(QLatin1Char('#')))
            line.remove(0, 1);
        line = line.trimmed();
        if (!line.isEmpty())
            return line.left(80);
    }
    return QStringLiteral("未命名");
}

bool MainWindow::saveSecureTab(EditorTab *tab, bool allowInteractiveUnlock, bool showErrors)
{
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments || !m_securitySession)
        return false;
    if (tab->isSecureDocumentMasked()) {
        if (showErrors)
            statusBar()->showMessage(QStringLiteral("加密文档已锁定，请先解锁后再保存。"), 5000);
        return false;
    }
    if (!m_securitySession->hasActiveSession()) {
        if (!allowInteractiveUnlock || !ensureSecuritySessionInteractive()) {
            QString recoveryError;
            checkpointSecureTab(tab, &recoveryError);
            if (!showErrors && !recoveryError.isEmpty()) {
                statusBar()->showMessage(
                    QStringLiteral("安全文档未保存，且无法建立加密恢复点：%1")
                        .arg(recoveryError), 8000);
            }
            return false;
        }
    }

    const DiskFileState diskState = m_secureDocuments->externalFileState(
        tab->secureDocumentId());
    if (diskState == DiskFileState::Changed
        || diskState == DiskFileState::Missing
        || diskState == DiskFileState::Unreadable) {
        if (!allowInteractiveUnlock) {
            QString recoveryError;
            checkpointSecureTab(tab, &recoveryError);
            statusBar()->showMessage(
                recoveryError.isEmpty()
                    ? QStringLiteral("安全文档已在磁盘上变化；最新内容已写入本机加密恢复点。")
                    : QStringLiteral("安全文档已在磁盘上变化，且恢复点写入失败。"),
                8000);
            return false;
        }
        const ExternalSaveDisposition disposition = resolveSecureExternalSaveConflict(tab);
        return disposition == ExternalSaveDisposition::HandledSuccess;
    }

    const QString title = secureTitleFrom(tab);
    // An unnamed secure note already owns an encrypted .nsafe-draft backing
    // file. Ordinary Save commits that same persistence kind silently; only
    // Save As chooses a portable path or converts the document format.
    const QString targetPath;
    VaultError error;
    if (tab->isSecureBinaryDocument()) {
        QByteArray body;
        QString exportError;
        if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
            if (showErrors)
                QMessageBox::critical(this, QStringLiteral("加密保存失败"), exportError);
            return false;
        }
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave,
            showErrors ? AsyncTaskPriority::High : AsyncTaskPriority::Low,
            showErrors ? QStringLiteral("正在安全保存")
                       : QStringLiteral("正在保存加密恢复点"),
            [this, tab, &body, title, targetPath, showErrors] {
                return m_secureDocuments->saveBinary(
                    tab->secureDocumentId(), body, title, targetPath, showErrors);
            }, showErrors);
        scrubByteArray(body);
    } else {
        QString body = tab->editor()->text();
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave,
            showErrors ? AsyncTaskPriority::High : AsyncTaskPriority::Low,
            showErrors ? QStringLiteral("正在安全保存")
                       : QStringLiteral("正在保存加密恢复点"),
            [this, tab, &body, title, targetPath, showErrors] {
                return m_secureDocuments->save(
                    tab->secureDocumentId(), body, title, targetPath, showErrors);
            }, showErrors);
        if (!body.isEmpty())
            body.fill(QChar(0));
        body.clear();
    }
    if (error.isError()) {
        QString recoveryError;
        const bool recovered = checkpointSecureTab(tab, &recoveryError);
        if (error.diagnosticCode.startsWith(
                QStringLiteral("secure-document.external-change"))) {
            if (allowInteractiveUnlock) {
                const ExternalSaveDisposition disposition =
                    resolveSecureExternalSaveConflict(tab);
                return disposition == ExternalSaveDisposition::HandledSuccess;
            }
            statusBar()->showMessage(
                recovered
                    ? QStringLiteral("安全文档已在磁盘上变化；最新内容已写入本机加密恢复点。")
                    : QStringLiteral("安全文档已在磁盘上变化，且恢复点写入失败。"),
                8000);
            return false;
        }
        if (showErrors) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("加密保存失败"),
                            error.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(error.diagnosticCode);
            box.exec();
        }
        if (!showErrors) {
            statusBar()->showMessage(
                recovered
                    ? QStringLiteral("自动保存失败；最新内容已加密恢复，将在稍后重试。")
                    : QStringLiteral("自动保存及本机加密恢复点均失败，请立即手动保存。"),
                8000);
        }
        return false;
    }
    tab->setSecureDocumentTitle(title);
    tab->setAutosavePath(m_secureDocuments->backingPath(tab->secureDocumentId()));
    tab->setModified(false);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabText(index, tab->tabTitle());
    statusBar()->showMessage(showErrors ? QStringLiteral("已保存")
                                        : QStringLiteral("已自动保存"), 2000);
    return true;
}

bool MainWindow::checkpointSecureTab(EditorTab *tab, QString *errorMessage)
{
    if (errorMessage)
        errorMessage->clear();
    if (!tab || !tab->isSecureDocument() || tab->isSecureDocumentMasked()
        || !m_secureDocuments) {
        if (errorMessage)
            *errorMessage = QStringLiteral("文档当前不可恢复。");
        return false;
    }

    VaultError error;
    if (tab->isSecureBinaryDocument()) {
        QByteArray body;
        QString exportError;
        if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
            if (errorMessage)
                *errorMessage = exportError;
            return false;
        }
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
            QStringLiteral("正在建立本机加密恢复点"),
            [this, tab, &body] {
                return m_secureDocuments->checkpointBinary(
                    tab->secureDocumentId(), body);
            }, false);
        scrubByteArray(body);
    } else {
        QString body = tab->editor()->text();
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
            QStringLiteral("正在建立本机加密恢复点"),
            [this, tab, &body] {
                return m_secureDocuments->checkpointText(
                    tab->secureDocumentId(), body);
            }, false);
        if (!body.isEmpty())
            body.fill(QChar(0));
        body.clear();
    }
    if (error.isError() && errorMessage)
        *errorMessage = error.userMessage;
    return !error.isError();
}

void MainWindow::closeSecureDocumentOnSecurityLane(const QUuid &documentId)
{
    if (!m_secureDocuments || documentId.isNull())
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在关闭安全文档"),
        [this, documentId] {
            m_secureDocuments->close(documentId);
            return VaultError{};
        },
        false);
}

void MainWindow::closeAllSecureDocumentsOnSecurityLane()
{
    if (!m_secureDocuments)
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在清理安全文档"),
        [this] {
            m_secureDocuments->closeAll();
            return VaultError{};
        },
        false);
}

void MainWindow::endSecuritySessionOnSecurityLane(bool removeDeviceSession)
{
    if (!m_securitySession)
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
        QStringLiteral("正在结束安全会话"),
        [this, removeDeviceSession] {
            m_securitySession->endSession(removeDeviceSession);
            return VaultError{};
        },
        false);
}

void MainWindow::lockSecureDocuments()
{
    lockSecureDocumentsImpl(false);
}

void MainWindow::lockSecureDocumentsImpl(bool sessionExpired)
{
    m_secureAutosaveTimer.stop();
    bool accountChangesDiscarded = false;
    bool secureChangesDiscarded = false;
    QVector<EditorTab *> lockingTabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()) {
            lockingTabs.append(tab);
            tab->beginSecureDocumentLocking();
        }
    }
    purgeSensitiveSearchState();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()
            && tab->isModified() && !saveSecureTab(tab, false, false)) {
            // Confirm that this exact in-memory revision is recoverable. An
            // older recovery file must never authorize discarding newer text.
            QString recoveryError;
            if (!checkpointSecureTab(tab, &recoveryError)) {
                if (sessionExpired) {
                    secureChangesDiscarded = true;
                    continue;
                }
                for (EditorTab *lockingTab : lockingTabs)
                    lockingTab->cancelSecureDocumentLocking();
                statusBar()->showMessage(
                    QStringLiteral("保存及本机加密恢复均失败，已取消锁定。"),
                    8000);
                return;
            }
        }
    }
    if (m_vaultDock && !m_vaultDock->prepareToLock()) {
        if (sessionExpired) {
            // Expired credentials must not leave account fields visible. The
            // silent save has already failed, so conceal and clear the editor.
            m_vaultDock->discardAndLockForExternalReload();
            accountChangesDiscarded = true;
        } else {
            for (EditorTab *lockingTab : lockingTabs)
                lockingTab->cancelSecureDocumentLocking();
            statusBar()->showMessage(QStringLiteral("账户修改尚未处理，已取消锁定。"), 5000);
            return;
        }
    }
    // Lock is also a recovery boundary for ordinary documents. The operation is
    // explicit, so establish the latest encrypted S1/S2 recovery point first.
    saveSessionNow(true);
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(true);
    if (m_vaultDock)
        m_vaultDock->lockVault();
    maskSecureDocuments();
    if (secureChangesDiscarded || accountChangesDiscarded) {
        QString warning;
        if (secureChangesDiscarded && accountChangesDiscarded) {
            warning = QStringLiteral(
                "会话到期时安全文档与账户均无法保存；为清除明文，未保存内容已丢弃。");
        } else if (secureChangesDiscarded) {
            warning = QStringLiteral(
                "会话到期时安全文档保存及加密恢复均失败；为清除明文，未保存内容已丢弃。");
        } else {
            warning = QStringLiteral(
                "会话到期时账户保存失败；为清除明文，未保存字段已丢弃。");
        }
        statusBar()->showMessage(
            warning, 10000);
    }
}

void MainWindow::purgeSensitiveSearchState()
{
    clearSensitiveDocumentClipboard();
    m_lastFindPattern.fill(QChar(0));
    m_lastFindPattern.clear();
    m_lastFindRegex = false;
    m_lastFindCase = false;
    m_lastFindWord = false;
    if (m_findDialog)
        m_findDialog->clearSensitiveState();
    if (m_searchDock)
        m_searchDock->clearResults();
}

void MainWindow::rememberSensitiveDocumentClipboard(EditorTab *sourceTab)
{
    scrubByteArray(m_sensitiveDocumentClipboardFingerprint);
    if (!sourceTab || !sourceTab->isSecureDocument()
        || sourceTab->isSecureDocumentMasked()) {
        return;
    }

    QString clipboardText = QGuiApplication::clipboard()->text();
    QByteArray clipboardUtf8 = clipboardText.toUtf8();
    if (!clipboardUtf8.isEmpty()) {
        m_sensitiveDocumentClipboardFingerprint = QCryptographicHash::hash(
            clipboardUtf8, QCryptographicHash::Sha256);
    }
    scrubByteArray(clipboardUtf8);
    if (!clipboardText.isEmpty())
        clipboardText.fill(QChar(0));
}

void MainWindow::clearSensitiveDocumentClipboard()
{
    if (m_sensitiveDocumentClipboardFingerprint.isEmpty())
        return;

    QString clipboardText = QGuiApplication::clipboard()->text();
    QByteArray clipboardUtf8 = clipboardText.toUtf8();
    QByteArray currentFingerprint = QCryptographicHash::hash(
        clipboardUtf8, QCryptographicHash::Sha256);
    if (currentFingerprint == m_sensitiveDocumentClipboardFingerprint)
        QGuiApplication::clipboard()->clear();

    scrubByteArray(clipboardUtf8);
    if (!clipboardText.isEmpty())
        clipboardText.fill(QChar(0));
    scrubByteArray(currentFingerprint);
    scrubByteArray(m_sensitiveDocumentClipboardFingerprint);
}

void MainWindow::protectForSystemLock()
{
    if (m_systemProtectionActive)
        return;
    m_secureAutosaveTimer.stop();
    m_systemLockRecoveryFailed = false;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()
            && tab->isModified() && !saveSecureTab(tab, false, false)) {
            QString recoveryError;
            if (!checkpointSecureTab(tab, &recoveryError))
                m_systemLockRecoveryFailed = true;
        }
    }
    m_restoreAccountsAfterSystemUnlock = m_workspaceStack && m_accountsPage
                                         && m_workspaceStack->currentWidget() == m_accountsPage;
    if (m_vaultDock && !m_vaultDock->lockVaultSilently()) {
        // A system lock is a confidentiality boundary. If the account editor
        // cannot save, it is still cleared instead of keeping secrets visible.
        m_vaultDock->discardAndLockForExternalReload();
        m_systemLockRecoveryFailed = true;
    }
    saveSessionNow(true);
    purgeSensitiveSearchState();
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(false);
    maskSecureDocuments();
    m_systemProtectionActive = true;
}

void MainWindow::restoreAfterSystemUnlock()
{
    if (!m_systemProtectionActive)
        return;
    bool restored = false;
    if (m_securitySession && m_securitySession->restoreSealedSession()) {
        // This call is non-interactive because restoreSealedSession has already
        // rehydrated a valid DPAPI session; it also restores masked tabs.
        restored = ensureSecuritySessionInteractive();
    }
    m_systemProtectionActive = false;
    if (restored && m_restoreAccountsAfterSystemUnlock)
        showAccountsWorkspace();
    m_restoreAccountsAfterSystemUnlock = false;
    if (!restored)
        statusBar()->showMessage(QStringLiteral("安全会话需要重新登录。"), 5000);
    else if (m_systemLockRecoveryFailed)
        statusBar()->showMessage(
            QStringLiteral("锁屏前有安全文档未能保存或建立恢复点，请检查最近编辑。"), 10000);
    m_systemLockRecoveryFailed = false;
}

void MainWindow::maskSecureDocuments()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab || !tab->isSecureDocument())
            continue;
        tab->maskSecureDocument();
        m_tabs->setTabText(i, tab->tabTitle());
    }
    updateStatusBar();
}

EditorTab *MainWindow::adoptSecureSnapshot(SecureDocumentSnapshot &snapshot,
                                          EditorTab *reuseTab)
{
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    showDocumentWorkspace();
    EditorTab *tab = reuseTab ? reuseTab : addNewTab();
    if (reuseTab && reuseTab->isSecureDocument()
        && reuseTab->secureDocumentId() != snapshot.documentId
        && m_secureDocuments) {
        closeSecureDocumentOnSecurityLane(reuseTab->secureDocumentId());
    }
    if (snapshot.payloadKind == SecureDocumentPayloadKind::Binary) {
        QString error;
        if (!tab->initializeSecureBinaryDocument(
                snapshot.documentId, snapshot.title, std::move(snapshot.binary),
                snapshot.filePath, &error)) {
            closeSecureDocumentOnSecurityLane(snapshot.documentId);
            if (!reuseTab) {
                const int failedIndex = m_tabs->indexOf(tab);
                if (failedIndex >= 0)
                    m_tabs->removeTab(failedIndex);
                delete tab;
            }
            QMessageBox::critical(this, QStringLiteral("无法打开安全文件"), error);
            return nullptr;
        }
    } else {
        tab->initializeSecureDocument(snapshot.documentId, snapshot.title,
                                      snapshot.text, snapshot.filePath);
        if (!snapshot.text.isEmpty())
            snapshot.text.fill(QChar(0));
        snapshot.text.clear();
    }
    tab->setAutosavePath(snapshot.backingPath);
    tab->setModified(snapshot.recoveredUnsaved);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0) {
        m_tabs->setCurrentIndex(index);
        m_tabs->setTabText(index, tab->tabTitle());
    }
    tab->setFocusEditor();
    if (snapshot.recoveredUnsaved) {
        statusBar()->showMessage(
            QStringLiteral("已恢复上次未写入 KDBX 的本机加密内容，请确认后保存。"),
            10000);
        m_secureAutosaveTimer.start(5000);
    }
    return tab;
#else
    Q_UNUSED(snapshot)
    Q_UNUSED(reuseTab)
    return nullptr;
#endif
}

bool MainWindow::openSecurePath(const QString &path, EditorTab *reuseTab)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(path)
    Q_UNUSED(reuseTab)
    return false;
#else
    ensureSecurityServicesCreated();
    if (!m_secureDocuments)
        return false;
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!openTab || !openTab->isSecureDocument())
            continue;
        const QString backing = openTab->filePath().isEmpty()
                                    ? openTab->autosavePath() : openTab->filePath();
        if (QFileInfo(backing).absoluteFilePath().compare(absolutePath,
                                                          Qt::CaseInsensitive) == 0) {
            showDocumentWorkspace();
            m_tabs->setCurrentIndex(i);
            openTab->setFocusEditor();
            return true;
        }
    }
    if (!m_securitySession->hasActiveSession() && !ensureSecuritySessionInteractive())
        return false;

    SecureDocumentSnapshot snapshot;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在打开安全文档"),
        [this, path, &snapshot] { return m_secureDocuments->open(path, &snapshot); });
    if (!error.isError()) {
        if (!adoptSecureSnapshot(snapshot, reuseTab))
            return false;
        if (!snapshot.draft)
            updateRecentFiles(path);
        return true;
    }
    if (error.code != VaultErrorCode::InvalidCredentials) {
        QMessageBox box(QMessageBox::Critical, QStringLiteral("无法打开安全文件"),
                        error.userMessage, QMessageBox::Ok, this);
        box.setDetailedText(error.diagnosticCode);
        box.exec();
        return false;
    }

    FileCredentialDialog dialog(
        QStringLiteral("打开受其他密码保护的文件"),
        QStringLiteral("当前会话不能解锁此文件。选择该文件的主密码或恢复密钥；成功后本次会话内不再询问。"),
        true, this);
    while (dialog.exec() == QDialog::Accepted) {
        if (dialog.kind() == FileCredentialDialog::Kind::RecoveryKey) {
            QString recoveryText = dialog.takeRecoveryKey();
            if (recoveryText.isEmpty()) {
                dialog.showError(QStringLiteral("请输入恢复密钥。"));
                continue;
            }
            error = runSerializedSecurityOperation(
                this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                QStringLiteral("正在恢复安全文档"),
                [this, path, &recoveryText, &snapshot] {
                    return m_secureDocuments->openWithRecovery(path, recoveryText,
                                                               &snapshot);
                });
            recoveryText.fill(QChar(0));
            recoveryText.clear();
        } else {
            SecureBuffer buffer = dialog.takePassphrase();
            if (buffer.empty()) {
                dialog.showError(QStringLiteral("请输入文件密码。"));
                continue;
            }
            error = runSerializedSecurityOperation(
                this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                QStringLiteral("正在解锁安全文档"),
                [this, path, &buffer, &snapshot] {
                    return m_secureDocuments->openWithPassphrase(path, buffer, &snapshot);
                });
            buffer.clear();
        }
        if (!error.isError()) {
            if (!adoptSecureSnapshot(snapshot, reuseTab))
                return false;
            if (!snapshot.draft)
                updateRecentFiles(path);
            return true;
        }
        dialog.showError(error.userMessage);
    }
    return false;
#endif
}

bool MainWindow::openPathUnified(const QString &path, EditorTab *reuseTab,
                                 std::optional<TextEncoding> forcedEncoding)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    const KdbxHeaderInfo kdbx = KdbxFormatProbe::inspectFile(path);
    if (kdbx.recognized) {
        QString documentType;
        QUuid documentId;
        SecurityIdentityParameters fileParameters;
        const VaultError probe = SecureDocumentService::inspect(
            path, &documentType, &documentId, &fileParameters);
        if (!probe.isError() && documentType == QStringLiteral("SecureDocument")) {
            // A copied or hard-linked .nsafe still carries the same document id.
            // Focus its live editor instead of asking the service to open a
            // second competing handle to the same logical document.
            if (!reuseTab && !documentId.isNull()) {
                for (int i = 0; i < m_tabs->count(); ++i) {
                    auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
                    if (!openTab || openTab->secureDocumentId() != documentId)
                        continue;
                    showDocumentWorkspace();
                    m_tabs->setCurrentIndex(i);
                    openTab->setFocusEditor();
                    updateRecentFiles(path);
                    statusBar()->showMessage(QStringLiteral("文件已在当前窗口中打开"), 2000);
                    return true;
                }
            }
            return openSecurePath(path, reuseTab);
        }
        if (probe.isError()
            && probe.diagnosticCode != QStringLiteral("identity.envelope.missing")) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("安全文件已损坏"),
                            probe.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(probe.diagnosticCode);
            box.exec();
            return false;
        }

        const bool managedAccounts = !probe.isError()
                                     && documentType == QStringLiteral("Accounts");
        // Account KDBX routing needs its adapter before credentials are
        // applied. The workspace remains lazy for ordinary startup/documents,
        // but file-open is a legitimate first-use trigger.
        ensureAccountsWorkspaceCreated();
        if (!ensureSecuritySessionInteractive())
            return false;
        if (managedAccounts) {
            SecureBuffer rememberedMaster;
            VaultError sessionOpen = m_securitySession->acquireRememberedMasterKey(
                fileParameters, &rememberedMaster);
            if (!sessionOpen.isError()) {
                sessionOpen = m_vaultDock->unlockManagedStoreWithMasterKey(
                    path, rememberedMaster);
            }
            rememberedMaster.clear();
            if (sessionOpen.isError()) {
                SecureBuffer rememberedRecovery;
                VaultError recoveryOpen =
                    m_securitySession->acquireRememberedRecoverySecret(
                        fileParameters, &rememberedRecovery);
                if (!recoveryOpen.isError()) {
                    recoveryOpen = m_vaultDock->unlockManagedStoreWithRecoverySecret(
                        path, rememberedRecovery);
                }
                rememberedRecovery.clear();
                sessionOpen = recoveryOpen;
            }
            if (!sessionOpen.isError()) {
                updateRecentFiles(path);
                return showAccountsWorkspace();
            }
            if (sessionOpen.code != VaultErrorCode::InvalidCredentials) {
                QMessageBox box(QMessageBox::Critical,
                                QStringLiteral("无法打开账户集合"),
                                sessionOpen.userMessage, QMessageBox::Ok, this);
                box.setDetailedText(sessionOpen.diagnosticCode);
                box.exec();
                return false;
            }
        }

        FileCredentialDialog dialog(
            managedAccounts ? QStringLiteral("打开受其他密码保护的账户集合")
                            : QStringLiteral("打开兼容 KDBX"),
            managedAccounts
                ? QStringLiteral("当前会话不能解锁此账户文件。选择该文件的主密码或恢复密钥。")
                : QStringLiteral("输入此 KDBX 文件的主密码。"),
            managedAccounts, this);
        while (dialog.exec() == QDialog::Accepted) {
            VaultError error;
            if (dialog.kind() == FileCredentialDialog::Kind::RecoveryKey) {
                QString recoveryKey = dialog.takeRecoveryKey();
                if (recoveryKey.isEmpty()) {
                    dialog.showError(QStringLiteral("请输入恢复密钥。"));
                    continue;
                }
                SecureBuffer recoverySecret;
                error = SecurityIdentity::parseRecoveryKey(
                    recoveryKey, &recoverySecret);
                if (!error.isError()) {
                    error = m_vaultDock->unlockManagedStoreWithRecoverySecret(
                        path, recoverySecret);
                }
                if (!error.isError()) {
                    error = m_securitySession->rememberRecoverySecret(
                        fileParameters, recoverySecret);
                }
                recoverySecret.clear();
                recoveryKey.fill(QChar(0));
                recoveryKey.clear();
            } else {
                SecureBuffer buffer = dialog.takePassphrase();
                if (buffer.empty()) {
                    dialog.showError(QStringLiteral("请输入文件密码。"));
                    continue;
                }
                if (managedAccounts) {
                    SecureBuffer masterUnlockKey;
                    error = SecurityIdentity::deriveMasterUnlockKey(
                        buffer, fileParameters, &masterUnlockKey);
                    if (!error.isError()) {
                        error = m_vaultDock->unlockManagedStoreWithMasterKey(
                            path, masterUnlockKey);
                    }
                    if (!error.isError()) {
                        error = m_securitySession->rememberMasterKey(
                            fileParameters, masterUnlockKey);
                    }
                    masterUnlockKey.clear();
                } else {
                    error = m_vaultDock->unlockVault(path, buffer);
                }
                buffer.clear();
            }
            if (!error.isError()) {
                updateRecentFiles(path);
                return showAccountsWorkspace();
            }
            dialog.showError(error.userMessage);
        }
        return false;
    }
#endif

    // Normal Open is idempotent.  Probe at most 192 KiB regardless of file
    // size, then compare against the baselines already captured by each tab.
    // Reload deliberately supplies reuseTab and must re-read that tab instead.
    if (!reuseTab) {
        DiskFileVersion candidate;
        const DiskFileState candidateState = captureDiskFileVersion(path, &candidate);
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
            if (!openTab || openTab->isSecureDocument())
                continue;
            const bool samePath = sameDiskFilePath(openTab->filePath(), path);
            const bool sameIdentity = candidateState == DiskFileState::Unchanged
                                      && openTab->representsDiskFile(candidate);
            if (!samePath && !sameIdentity)
                continue;
            showDocumentWorkspace();
            m_tabs->setCurrentIndex(i);
            openTab->setFocusEditor();
            updateRecentFiles(path);
            statusBar()->showMessage(QStringLiteral("文件已在当前窗口中打开"), 2000);
            return true;
        }
    }

    showDocumentWorkspace();
    EditorTab *tab = reuseTab ? reuseTab : addNewTab();
    if (reuseTab && reuseTab->isSecureDocument()) {
        if (m_secureDocuments)
            closeSecureDocumentOnSecurityLane(reuseTab->secureDocumentId());
        // External Open may reuse the single empty default secure draft. The
        // reused tab must stop being a secure document before ordinary routing.
        reuseTab->convertToPlainDocument(QString());
    }
    const bool opened = openTabAutoMode(tab, path, forcedEncoding);
    if (!opened) {
        if (!reuseTab) {
            const int index = m_tabs->indexOf(tab);
            if (index >= 0)
                m_tabs->removeTab(index);
            delete tab;
        }
        return false;
    }
    updateRecentFiles(path);
    m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    if (tab == currentTab()) {
        syncGlobalMenuCheckStates();
        updateStatusBar();
    }
    return true;
}

void MainWindow::openFile()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("打开文件"), QString(),
        QStringLiteral("所有文件 (*);;"
                       "C/C++ 文件 (*.cpp *.cc *.cxx *.c *.h *.hpp *.hh *.hxx);;"
                       "Python 文件 (*.py *.pyw);;"
                       "JSON 文件 (*.json);;"
                       "HTML 文件 (*.html *.htm);;"
                       "XML 文件 (*.xml);;"
                       "SQL 文件 (*.sql);;"
                       "YAML 文件 (*.yaml *.yml);;"
                       "Markdown 文件 (*.md *.markdown);;"
                       "文本文件 (*.txt)"));
    for (const QString &file : files) {
        if (file.isEmpty())
            continue;
        if (!openPathUnified(file)) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                                 QStringLiteral("无法打开文件: %1").arg(file));
        }
    }
}

bool MainWindow::saveFile()
{
    if (m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage) {
        const bool saved = m_vaultDock && m_vaultDock->saveActiveEntry();
        if (saved)
            statusBar()->showMessage(QStringLiteral("账户已保存"), 2500);
        return saved;
    }
    if (!isDocumentWorkspaceActive())
        return false;
    auto *tab = currentTab();
    if (!tab)
        return false;
    if (tab->isSecureDocument())
        return saveSecureTab(tab, true);
    if (tab->isUntitled())
        return saveAsFile();
    // Binary documents are saved from their authoritative byte/patch model.
    // U+FFFD in a Hex character pane or Binary Text projection is display
    // state and does not imply that Save will re-encode untouched bytes.
    // Keep this destructive re-encoding confirmation only for ordinary text.
    if (tab->encodingHasDecodeErrors() && !tab->isBinaryDocument()) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("编码需要确认"),
                        QStringLiteral("源文件包含不符合当前 %1 解释的字节。"
                                       "继续保存会用当前显示文本覆盖这些原始字节。")
                            .arg(encodingName(tab->displayEncoding())),
                        QMessageBox::NoButton, this);
        auto *overwrite = box.addButton(
            QStringLiteral("仍以 %1 覆盖").arg(encodingName(tab->displayEncoding())),
                                        QMessageBox::AcceptRole);
        auto *cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != overwrite)
            return false;
        // The explicit decision turns the fallback into a user-selected
        // encoding so subsequent saves do not ask the same question again.
        tab->setEncoding(tab->displayEncoding());
    }
    switch (resolveExternalSaveConflict(tab)) {
    case ExternalSaveDisposition::HandledSuccess:
        return true;
    case ExternalSaveDisposition::Cancelled:
        return false;
    case ExternalSaveDisposition::Proceed:
        break;
    }
    QString err;
    if (!tab->saveFile(&err)) {
        QString detail = err.isEmpty() ? tab->filePath() : QStringLiteral("%1\n\n详细信息：%2").arg(tab->filePath(), err);
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法保存文件：\n%1").arg(detail));
        return false;
    }
    return true;
}

MainWindow::ExternalSaveDisposition MainWindow::resolveExternalSaveConflict(EditorTab *tab)
{
    if (!tab || tab->isSecureDocument())
        return ExternalSaveDisposition::Proceed;
    const EditorTab::ExternalFileState state = tab->externalFileState();
    if (state == EditorTab::ExternalFileState::NotTracked
        || state == EditorTab::ExternalFileState::Unchanged) {
        return ExternalSaveDisposition::Proceed;
    }

    QMessageBox box(QMessageBox::Warning, QStringLiteral("文件已在磁盘上更改"),
                    QStringLiteral("为避免覆盖其他程序的修改，本次保存已暂停。"),
                    QMessageBox::NoButton, this);
    if (state == EditorTab::ExternalFileState::Changed) {
        box.setInformativeText(
            QStringLiteral("“%1”在打开后发生了变化。\n"
                           "可以重新加载磁盘版本，或将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    } else if (state == EditorTab::ExternalFileState::Missing) {
        box.setInformativeText(
            QStringLiteral("“%1”已被移动或删除。\n"
                           "可以将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    } else {
        box.setInformativeText(
            QStringLiteral("当前无法安全读取“%1”以确认它是否变化。\n"
                           "可以将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    }

    QPushButton *reloadButton = nullptr;
    if (state == EditorTab::ExternalFileState::Changed) {
        reloadButton = box.addButton(QStringLiteral("重新加载磁盘版本"),
                                     QMessageBox::AcceptRole);
    }
    auto *saveAsButton = box.addButton(QStringLiteral("另存为…"), QMessageBox::ActionRole);
    auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();

    if (reloadButton && box.clickedButton() == reloadButton)
        return reloadTabFromDisk(tab) ? ExternalSaveDisposition::HandledSuccess
                                      : ExternalSaveDisposition::Cancelled;
    if (box.clickedButton() == saveAsButton)
        return saveAsFile() ? ExternalSaveDisposition::HandledSuccess
                            : ExternalSaveDisposition::Cancelled;
    return ExternalSaveDisposition::Cancelled;
}

MainWindow::ExternalSaveDisposition MainWindow::resolveSecureExternalSaveConflict(
    EditorTab *tab)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(tab)
    return ExternalSaveDisposition::Cancelled;
#else
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments)
        return ExternalSaveDisposition::Cancelled;
    const DiskFileState state = m_secureDocuments->externalFileState(
        tab->secureDocumentId());

    QMessageBox box(QMessageBox::Warning,
                    QStringLiteral("安全文档已在磁盘上更改"),
                    QStringLiteral("为避免覆盖其他程序写入的密文，本次保存已暂停。"),
                    QMessageBox::NoButton, this);
    if (state == DiskFileState::Changed) {
        box.setInformativeText(
            QStringLiteral("“%1”在打开后发生了变化。\n"
                           "可以重新解密并加载磁盘版本，或将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    } else if (state == DiskFileState::Missing) {
        box.setInformativeText(
            QStringLiteral("“%1”已被移动或删除。\n"
                           "可以将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    } else {
        box.setInformativeText(
            QStringLiteral("当前无法安全验证“%1”的磁盘版本。\n"
                           "可以将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    }

    QPushButton *reloadButton = nullptr;
    if (state == DiskFileState::Changed) {
        reloadButton = box.addButton(QStringLiteral("重新加载磁盘版本"),
                                     QMessageBox::AcceptRole);
    }
    auto *saveAsButton = box.addButton(QStringLiteral("另存为…"), QMessageBox::ActionRole);
    auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();

    if (reloadButton && box.clickedButton() == reloadButton)
        return reloadSecureTabFromDisk(tab) ? ExternalSaveDisposition::HandledSuccess
                                            : ExternalSaveDisposition::Cancelled;
    if (box.clickedButton() == saveAsButton)
        return saveAsFile() ? ExternalSaveDisposition::HandledSuccess
                            : ExternalSaveDisposition::Cancelled;
    return ExternalSaveDisposition::Cancelled;
#endif
}

bool MainWindow::reloadSecureTabFromDisk(EditorTab *tab)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(tab)
    return false;
#else
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments)
        return false;
    SecureDocumentSnapshot snapshot;
    const VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在重新加载安全文档"),
        [this, tab, &snapshot] {
            return m_secureDocuments->reload(tab->secureDocumentId(), &snapshot);
        });
    if (error.isError()) {
        QMessageBox box(QMessageBox::Critical, QStringLiteral("无法重新加载安全文档"),
                        error.userMessage, QMessageBox::Ok, this);
        box.setDetailedText(error.diagnosticCode);
        box.exec();
        return false;
    }
    if (!adoptSecureSnapshot(snapshot, tab))
        return false;
    if (m_searchDock)
        m_searchDock->clearResults();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已重新解密并加载磁盘版本"), 2500);
    return true;
#endif
}

bool MainWindow::reloadTabFromDisk(EditorTab *tab)
{
    if (!tab || tab->isSecureDocument() || tab->filePath().isEmpty())
        return false;
    const QString path = tab->filePath();
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法重新加载"),
                             QStringLiteral("无法读取文件：\n%1\n\n%2")
                                 .arg(path, probe.errorString()));
        return false;
    }
    probe.close();

    // Discarding edits is already explicit at the call site. Tear down mmap and
    // route through the complete Open path (KDBX header, binary and size tier).
    tab->shutdownL2BeforeClose();
    if (!openPathUnified(path, tab)) {
        QMessageBox::warning(this, QStringLiteral("无法重新加载"),
                             QStringLiteral("文件无法按当前格式重新打开：\n%1")
                                 .arg(path));
        return false;
    }
    if (m_searchDock)
        m_searchDock->clearResults();
    updateRecentFiles(path);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabText(index, tab->tabTitle());
    syncGlobalMenuCheckStates();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已从磁盘重新加载"), 2500);
    return true;
}

void MainWindow::reloadFile()
{
    EditorTab *tab = currentTab();
    if (!tab || (tab->isSecureDocument() ? tab->filePath().isEmpty()
                                         : tab->isUntitled()))
        return;
    if (tab->isModified()) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("重新加载文件"),
                        QStringLiteral("重新加载将丢弃“%1”的未保存修改。")
                            .arg(tab->fileName()),
                        QMessageBox::NoButton, this);
        auto *reloadButton = box.addButton(QStringLiteral("重新加载"),
                                           QMessageBox::DestructiveRole);
        auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(cancelButton);
        box.exec();
        if (box.clickedButton() != reloadButton)
            return;
    }
    if (tab->isSecureDocument())
        reloadSecureTabFromDisk(tab);
    else
        reloadTabFromDisk(tab);
}

bool MainWindow::saveAsFile()
{
    auto *tab = currentTab();
    if (!tab)
        return false;
    const QString secureFilter = QStringLiteral("Notepad Safe 安全文档 (*.nsafe)");
    const QString plainFilter = QStringLiteral("未加密文件 (*)");
    QString selectedFilter = tab->isSecureDocument() ? secureFilter
                                                      : plainFilter;
    QString suggestion;
    if (!tab->filePath().isEmpty())
        suggestion = tab->filePath();
    else
        suggestion = secureTitleFrom(tab) + (tab->isSecureDocument()
                                                  ? QStringLiteral(".nsafe")
                                                  : QStringLiteral(".txt"));
    QDialog saveAsDialog(this);
    saveAsDialog.setObjectName(QStringLiteral("saveAsDialog"));
    saveAsDialog.setWindowTitle(QStringLiteral("另存为"));
    saveAsDialog.resize(940, 650);
    auto *saveAsLayout = new QVBoxLayout(&saveAsDialog);
    saveAsLayout->setContentsMargins(10, 10, 10, 10);
    saveAsLayout->setSpacing(8);

    auto *fileDialog = new QFileDialog(
        &saveAsDialog, QString(), suggestion,
        secureFilter + QStringLiteral(";;") + plainFilter);
    fileDialog->setObjectName(QStringLiteral("saveAsFileDialog"));
    fileDialog->setWindowFlags(Qt::Widget);
    fileDialog->setAcceptMode(QFileDialog::AcceptSave);
    fileDialog->setFileMode(QFileDialog::AnyFile);
    // The product requires one dialog with an explicit, always-visible
    // encryption consequence. Native dialogs do not allow that content.
    fileDialog->setOption(QFileDialog::DontUseNativeDialog, true);
    fileDialog->setLabelText(QFileDialog::LookIn, QStringLiteral("位置："));
    fileDialog->setLabelText(QFileDialog::FileName, QStringLiteral("文件名："));
    fileDialog->setLabelText(QFileDialog::FileType, QStringLiteral("保存类型："));
    fileDialog->setLabelText(QFileDialog::Accept, QStringLiteral("保存"));
    fileDialog->setLabelText(QFileDialog::Reject, QStringLiteral("取消"));
    fileDialog->selectNameFilter(selectedFilter);
    saveAsLayout->addWidget(fileDialog, 1);

    auto *securityNotice = new QLabel(&saveAsDialog);
    securityNotice->setObjectName(QStringLiteral("saveAsSecurityNotice"));
    securityNotice->setWordWrap(true);
    securityNotice->setContentsMargins(8, 6, 8, 6);
    securityNotice->setMinimumHeight(42);
    saveAsLayout->addWidget(securityNotice);
    const QString plainSuffix = tab->isBinaryDocument()
                                    ? QStringLiteral("bin") : QStringLiteral("txt");
    const auto updateSaveAsNotice = [fileDialog, securityNotice, secureFilter,
                                     plainSuffix](const QString &filter) {
        if (filter == secureFilter) {
            fileDialog->setDefaultSuffix(QStringLiteral("nsafe"));
            securityNotice->setText(QStringLiteral(
                "将保存为离线加密的 Notepad Safe 文档。普通保存会继续保持此格式。"));
            securityNotice->setStyleSheet(QStringLiteral(
                "QLabel { color: #246b45; background: #eaf7ef; border: 1px solid #b9dfc8; "
                "border-radius: 4px; }"));
            const QStringList selected = fileDialog->selectedFiles();
            if (!selected.isEmpty()
                && !selected.constFirst().endsWith(QStringLiteral(".nsafe"),
                                                    Qt::CaseInsensitive)) {
                fileDialog->selectFile(selected.constFirst()
                                       + QStringLiteral(".nsafe"));
            }
        } else {
            fileDialog->setDefaultSuffix(plainSuffix);
            securityNotice->setText(QStringLiteral(
                "将创建未加密文件，其他程序可以直接读取其内容。普通保存会继续保持此格式。"));
            securityNotice->setStyleSheet(QStringLiteral(
                "QLabel { color: #7a4b00; background: #fff5dd; border: 1px solid #e8c77a; "
                "border-radius: 4px; }"));

            // .nsafe is reserved for KDBX-backed encrypted documents. When a
            // user changes the explicit type selector, update the inherited
            // suggestion immediately instead of silently writing plaintext
            // behind an encrypted-looking extension.
            const QStringList selected = fileDialog->selectedFiles();
            if (!selected.isEmpty()
                && selected.constFirst().endsWith(QStringLiteral(".nsafe"),
                                                   Qt::CaseInsensitive)) {
                QString plainName = selected.constFirst();
                plainName.chop(QStringLiteral(".nsafe").size());
                plainName += QLatin1Char('.') + plainSuffix;
                fileDialog->selectFile(plainName);
            }
        }
    };
    connect(fileDialog, &QFileDialog::filterSelected,
            &saveAsDialog, updateSaveAsNotice);
    connect(fileDialog, &QFileDialog::accepted,
            &saveAsDialog, &QDialog::accept);
    connect(fileDialog, &QFileDialog::rejected,
            &saveAsDialog, &QDialog::reject);
    updateSaveAsNotice(selectedFilter);

    if (saveAsDialog.exec() != QDialog::Accepted
        || fileDialog->selectedFiles().isEmpty()) {
        return false;
    }
    QString fileName = fileDialog->selectedFiles().constFirst();
    selectedFilter = fileDialog->selectedNameFilter();

    // The explicit type selector is authoritative. An extension is a display
    // hint only and must never silently change the security boundary.
    const bool saveSecure = selectedFilter == secureFilter;
    if (!saveSecure
        && fileName.endsWith(QStringLiteral(".nsafe"), Qt::CaseInsensitive)) {
        fileName.chop(QStringLiteral(".nsafe").size());
        fileName += QLatin1Char('.') + plainSuffix;
    }
    if (saveSecure) {
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
        const bool binaryPayload = tab->isBinaryDocument();
        if (tab->isLargeDocument() && !binaryPayload) {
            QMessageBox::information(
                this, QStringLiteral("不能转换为安全文档"),
                QStringLiteral("安全文档只接受小于 32 MiB 的内容。"
                               "当前文本处于大文件模式，未写入任何目标文件。"));
            return false;
        }
        if (!ensureSecuritySessionInteractive() || !m_secureDocuments)
            return false;
        if (!fileName.endsWith(QStringLiteral(".nsafe"), Qt::CaseInsensitive))
            fileName += QStringLiteral(".nsafe");
        const QString title = secureTitleFrom(tab);
        VaultError error;
        if (binaryPayload) {
            QByteArray body;
            QString exportError;
            if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
                QMessageBox::critical(this, QStringLiteral("另存为失败"), exportError);
                return false;
            }
            if (tab->isSecureDocument()) {
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在另存安全文档"),
                    [this, tab, &body, title, fileName] {
                        return m_secureDocuments->saveBinary(
                            tab->secureDocumentId(), body, title, fileName);
                    });
                if (!error.isError()) {
                    tab->setFilePath(QFileInfo(fileName).absoluteFilePath());
                    tab->setAutosavePath(
                        m_secureDocuments->backingPath(tab->secureDocumentId()));
                    tab->setSecureDocumentTitle(title);
                    tab->setModified(false);
                }
            } else {
                SecureDocumentSnapshot snapshot;
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在创建安全文档"),
                    [this, &body, title, fileName, &snapshot] {
                        return m_secureDocuments->saveAsSecureBinary(
                            body, title, fileName, &snapshot);
                    });
                if (!error.isError() && !adoptSecureSnapshot(snapshot, tab)) {
                    scrubByteArray(body);
                    return false;
                }
            }
            scrubByteArray(body);
        } else {
            QString body = tab->editor()->text();
            if (tab->isSecureDocument()) {
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在另存安全文档"),
                    [this, tab, &body, title, fileName] {
                        return m_secureDocuments->save(
                            tab->secureDocumentId(), body, title, fileName);
                    });
                if (!error.isError()) {
                    tab->setFilePath(QFileInfo(fileName).absoluteFilePath());
                    tab->setAutosavePath(
                        m_secureDocuments->backingPath(tab->secureDocumentId()));
                    tab->setSecureDocumentTitle(title);
                    tab->setModified(false);
                }
            } else {
                SecureDocumentSnapshot snapshot;
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在创建安全文档"),
                    [this, &body, title, fileName, &snapshot] {
                        return m_secureDocuments->saveAsSecure(
                            body, title, fileName, &snapshot);
                    });
                if (!error.isError() && !adoptSecureSnapshot(snapshot, tab)) {
                    body.fill(QChar(0));
                    return false;
                }
            }
            if (!body.isEmpty())
                body.fill(QChar(0));
            body.clear();
        }
        if (error.isError()) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("另存为失败"),
                            error.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(error.diagnosticCode);
            box.exec();
            return false;
        }
#else
        return false;
#endif
    } else {
        const QUuid previousSecureId = tab->isSecureDocument()
                                           ? tab->secureDocumentId() : QUuid();
        QString err;
        if (tab->isSecureDocument()) {
            if (tab->isSecureBinaryDocument()) {
                QByteArray raw;
                if (!tab->exportBinaryPayloadBytes(&raw, &err)) {
                    // export supplied the actionable validation message.
                } else {
                    QSaveFile output(fileName);
                    output.setDirectWriteFallback(false);
                    const bool written = output.open(QIODevice::WriteOnly)
                                         && output.write(raw) == raw.size()
                                         && output.commit();
                    scrubByteArray(raw);
                    if (!written)
                        err = output.errorString().isEmpty()
                                  ? QStringLiteral("无法原子写入文件。")
                                  : output.errorString();
                }
                if (err.isEmpty() && !openPathUnified(fileName, tab))
                    err = QStringLiteral("文件已写入，但无法重新打开目标文件。");
                // Re-enter through the unified opener so the newly created raw
                // file receives its mmap/Hex backing store and disk baseline.
            } else if (!tab->exportToFile(fileName)) {
                err = QStringLiteral("无法写入文件。");
            } else {
                if (m_secureDocuments)
                    closeSecureDocumentOnSecurityLane(previousSecureId);
                tab->convertToPlainDocument(QFileInfo(fileName).absoluteFilePath());
            }
        } else if (!tab->saveAsFile(fileName, &err)) {
            // err is populated by the text writer.
        }
        if (!err.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                                 QStringLiteral("无法保存文件：\n%1\n\n%2")
                                     .arg(fileName, err));
            return false;
        }
    }
    updateRecentFiles(fileName);
    int idx = m_tabs->indexOf(tab);
    if (idx >= 0)
        m_tabs->setTabText(idx, tab->tabTitle());
    syncCommandStates(tab);
    syncGlobalMenuCheckStates();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已另存为"), 2500);
    return true;
}

void MainWindow::closeCurrentTab()
{
    if (!m_tabs)
        return;
    const int index = m_tabs->currentIndex();
    if (index >= 0)
        onTabCloseRequested(index);
}

void MainWindow::closeAllTabs()
{
    if (!m_tabs || m_tabs->count() == 0)
        return;

    // Preflight every document before removing any tab. If the user cancels a
    // later prompt, the workspace remains intact instead of being half closed.
    QVector<int> order;
    order.reserve(m_tabs->count());
    const int current = m_tabs->currentIndex();
    if (current >= 0)
        order.push_back(current);
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (index != current)
            order.push_back(index);
    }
    for (const int index : order) {
        if (!maybeSaveTab(index, false))
            return;
    }

    for (int index = m_tabs->count() - 1; index >= 0; --index)
        closeTabUnchecked(index);
}

void MainWindow::openRecentFile()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act)
        return;
    QString path = act->data().toString();
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        for (int i = m_recentFiles.size() - 1; i >= 0; --i) {
            if (sameDiskFilePath(m_recentFiles.at(i), path))
                m_recentFiles.removeAt(i);
        }
        rebuildRecentFilesMenu();
        saveSettings();
        statusBar()->showMessage(
            QStringLiteral("文件已不存在，已从最近打开中移除：%1").arg(path), 5000);
        return;
    }
    if (!openPathUnified(path)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件: %1").arg(path));
    }
}

void MainWindow::clearRecentFiles()
{
    m_recentFiles.clear();
    rebuildRecentFilesMenu();
    saveSettings();
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
    if (m_undoAction)
        m_undoAction->setEnabled(hasDocument && tab->canUndoCommand());
    if (m_redoAction)
        m_redoAction->setEnabled(hasDocument && tab->canRedoCommand());
    if (m_cutAction)
        m_cutAction->setEnabled(canModify && hasSelection);
    if (m_copyAction)
        m_copyAction->setEnabled(hasSelection);
    if (m_pasteAction) {
        const QMimeData *clipboardData = QGuiApplication::clipboard()->mimeData();
        m_pasteAction->setEnabled(canModify && clipboardData
                                  && clipboardData->hasText()
                                  && !clipboardData->text().isEmpty());
    }
    if (m_selectAllAction)
        m_selectAllAction->setEnabled(hasDocument && !tab->isSecureDocumentMasked());
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
    const bool accountsWorkspace = m_workspaceStack && m_accountsPage
                                   && m_workspaceStack->currentWidget() == m_accountsPage;
    if (m_saveAction) {
        m_saveAction->setEnabled((hasDocument && (tab->isModified() || tab->isUntitled()))
                                 || (accountsWorkspace && m_vaultDock
                                     && m_vaultDock->canSaveActiveEntry()));
    }
    if (m_saveAsAction)
        m_saveAsAction->setEnabled(hasDocument);
    if (m_findAction) {
        m_findAction->setEnabled(hasDocument
                                 || (accountsWorkspace && m_vaultDock
                                     && m_vaultDock->canSearchAccounts()));
    }
    if (m_replaceAction)
        m_replaceAction->setEnabled(hasDocument);
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
    if (m_searchPanelAction)
        m_searchPanelAction->setEnabled(hasDocument);
    if (m_actReload)
        m_actReload->setEnabled(tab && (tab->isSecureDocument()
                                            ? !tab->filePath().isEmpty()
                                            : !tab->isUntitled()));
    if (m_actCloseTab)
        m_actCloseTab->setEnabled(tab != nullptr);
    if (m_actCloseAll)
        m_actCloseAll->setEnabled(documentWorkspace && m_tabs
                                  && m_tabs->count() > 0 && currentTab() != nullptr);
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

void MainWindow::onJumpToTabByteOffset(EditorTab *tab, qint64 byteOffset, int length)
{
    if (!tab)
        return;
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
            loaded = openSessionFile(tab, st);
        }

        if (!loaded) {
            tab->setSyntax(static_cast<Syntax>(st.syntax));
        } else {
            restoreSessionPosition(tab, st);
            restoreSessionSplit(tab, st);
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
//   · 走 openTabAutoMode ≥32MB 强制 L2 窗口化秒开，不再 L1 主线程阻塞读
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
        //   新逻辑：openTabAutoMode → ≥ 32MB 自动 openLargeFileL2（窗口化只读，无后台编辑加载）
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
        loaded = openSessionFile(tab, st);
        qDebug("[STARTUP][RESTORE tab %d/%d]   openTabAutoMode 返回 ok=%d, 耗时=%lld ms（关键：如果 > 500ms 就是阻塞启动的根因！）",
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
        restoreSessionPosition(tab, st);
        restoreSessionSplit(tab, st);
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
