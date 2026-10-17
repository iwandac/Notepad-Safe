#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QVector>
#include <QStringList>
#include <QTimer>
#include <QSet>
#include "TextEncoding.h"

#include <memory>
#include <optional>

class QTabWidget;
class QShowEvent;
class QStackedWidget;
class QLabel;
class QToolButton;
class QMenu;
class QAction;
class QUuid;
class QPoint;
class EditorTab;
struct ThemeTokens;
class FindReplaceDialog;
class SearchResultsDock;
class SearchResultsController;
class SessionManager;
class VaultDock;
class StatusBarTaskWidget;
class WorkspaceRecoveryCoordinator;
class ISecuritySessionService;
class ISecureDocumentPersistence;
class AppComposition;
class CommandRegistry;
class DocumentTailActions;
class QPaintEvent;
struct SecureDocumentSnapshot;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr,
                        AppComposition *composition = nullptr);
    ~MainWindow() override;

    // 从命令行/拖放等外部传入打开文件（支持多个）
    Q_INVOKABLE void openFilesFromExternal(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;

private slots:
    // 文件
    void newFile();
    void openFile();
    void reloadFile();
    bool saveFile();
    bool saveAsFile();
    void closeCurrentTab();
    void closeAllTabs();
    void openRecentFile();
    void clearRecentFiles();

    // 编辑
    void duplicateLine();
    void deleteLine();
    void moveLineUp();
    void moveLineDown();
    void sortLinesAsc();
    void sortLinesDesc();

    // 搜索
    void showFindDialog();
    void showReplaceDialog();
    void findNext();
    void findPrev();

    // 语法
    void setSyntaxFromAction();

    // 书签
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    void clearBookmarks();

    // 视图
    void setAppearanceModeFromAction(QAction *action);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void toggleEdgeLine();
    void setEdgeColumnFromAction();
    void toggleWordWrap();

    // 编码
    void setEncodingFromAction();
    void convertEncodingFromAction();
    void displayEncodingFromAction();

    // 标签
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void showTabContextMenu(const QPoint &position);
    void onTabModifiedChanged(bool modified);
    void onTabCursorPositionChanged(int line, int index);
    void onTabTitleChanged(const QString &title);
    void onTabEncodingChanged(TextEncoding encoding);

    // 统一计算文档、账户、安全中心三类工作区的命令可用性，并叠加 L2 能力限制。
    void syncCommandStates(EditorTab *tab = nullptr);

    // 搜索结果
    void onJumpToByteOffset(qint64 byteOffset, int length);
    void onJumpToDocumentByteOffset(const QUuid &documentId,
                                    quint64 expectedRevision,
                                    qint64 byteOffset, int length);
    void lockSecureDocuments();
    void maskSecureDocuments();

signals:
    void firstPaintCompleted();
    void sessionRestoreCompleted();
    // Emitted only after every user-cancellable close check and the forced
    // recovery-point commit have succeeded, immediately before teardown.
    void shutdownCommitted();

private:
    friend class WorkspaceRecoveryCoordinator;
    std::unique_ptr<AppComposition> m_ownedComposition;
    AppComposition *m_composition = nullptr;
    std::unique_ptr<CommandRegistry> m_commandRegistry;
    DocumentTailActions *m_tailActions = nullptr;
    bool m_firstPaintCompleted = false;
    QTabWidget *m_tabs = nullptr;
    QSet<QWidget *> m_closingTabs;
    bool m_windowCloseInProgress = false;
    QStackedWidget *m_workspaceStack = nullptr;
    QWidget *m_accountsPage = nullptr;
    QWidget *m_securityCenterPage = nullptr;
    FindReplaceDialog *m_findDialog = nullptr;
    SearchResultsDock *m_searchDock = nullptr;
    std::unique_ptr<SearchResultsController> m_searchResultsController;
    VaultDock *m_vaultDock = nullptr;
    SessionManager *m_session = nullptr;
    std::unique_ptr<ISecuritySessionService> m_securitySession;
    ISecureDocumentPersistence *m_securePersistence = nullptr;

    // 状态栏
    QLabel *m_statusPos = nullptr;
    QLabel *m_statusEncoding = nullptr;
    QLabel *m_statusSize = nullptr;
    QLabel *m_statusCharacters = nullptr;
    QLabel *m_statusSyntax = nullptr;
    QLabel *m_statusModified = nullptr;
    QToolButton *m_hexEditModeButton = nullptr;
    QToolButton *m_externalOpenFailuresButton = nullptr;
    QStringList m_externalOpenFailures;
    StatusBarTaskWidget *m_taskWidget = nullptr;

    // 最近文件菜单
    QMenu *m_recentMenu = nullptr;
    QStringList m_recentFiles;

    // 编码菜单（记录以便切换标签时同步勾选状态）
    QMenu *m_encodingMenu = nullptr;
    QMenu *m_displayEncodingMenu = nullptr;

    // L2 模式下禁用不适配菜单项（D001~D007 强制，L2 isL2Active=true 时 setEnabled(false) 变灰）

    // 视图菜单 action（用于 loadSettings 后同步勾选状态，避免数据与UI不一致）
    QAction *m_actDarkMode = nullptr;
    QAction *m_actAppearanceSystem = nullptr;
    QAction *m_actAppearanceLight = nullptr;
    QAction *m_actWordWrap = nullptr;
    QAction *m_actWhitespaceVisible = nullptr;
    QAction *m_actEolVisible = nullptr;
    QAction *m_actUseTabs = nullptr;
    QAction *m_actBackspaceUnindents = nullptr;
    QAction *m_actWordCompletion = nullptr;
    QAction *m_actEdgeVisible = nullptr;
    QAction *m_searchPanelAction = nullptr;
    QAction *m_statusVisibilityAction = nullptr;
    QAction *m_actReload = nullptr;
    QAction *m_actCloseTab = nullptr;
    QAction *m_actCloseAll = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_hexInsertModeAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_replaceAction = nullptr;
    QAction *m_accountsAction = nullptr;
    QAction *m_securityCenterAction = nullptr;
    QAction *m_tasksAction = nullptr;
    QLabel *m_securityIdentityStatus = nullptr;
    QLabel *m_securitySessionStatus = nullptr;
    QLabel *m_securityRecoveryStatus = nullptr;
    QActionGroup *m_edgeColGroup = nullptr;   // 长行标尺列数组，遍历其 action 同步勾选
    QActionGroup *m_tabWidthGroup = nullptr;

    enum class AppearanceMode { System = 0, Light = 1, Dark = 2 };
    AppearanceMode m_appearanceMode = AppearanceMode::System;
    QActionGroup *m_appearanceGroup = nullptr;
    // Resolved palette state. System mode updates this when Windows changes.
    bool m_darkMode = false;
    bool m_highContrastMode = false;

    // 长行标尺
    bool m_edgeVisible = true;
    int m_edgeColumn = 80;

    // 自动换行
    bool m_wordWrap = false;
    bool m_whitespaceVisible = false;
    bool m_eolVisible = false;
    bool m_useTabs = false;
    int m_tabWidth = 4;
    bool m_backspaceUnindents = true;
    bool m_wordCompletion = true;
    bool m_searchDockWasVisibleBeforeWorkspace = false;

    // 查找历史
    QString m_lastFindPattern;
    bool m_lastFindRegex = false;
    bool m_lastFindCase = false;
    bool m_lastFindWord = false;

    void createActions();
    void createMenus();
    void createStatusBar();

    enum class OpenPresentationPolicy {
        FocusDocumentWorkspace,
        PreserveCurrentWorkspace
    };
    enum class OpenAuthorizationPolicy {
        AllowInteractive,
        DenyInteractiveUnlock
    };
    EditorTab *currentTab() const;
    EditorTab *addNewTab(
        OpenPresentationPolicy presentation =
            OpenPresentationPolicy::FocusDocumentWorkspace,
        int recoveryItemIndex = -1);
    void showDocumentWorkspace();
    bool showAccountsWorkspace();
    bool showSecurityCenterWorkspace();
    void refreshSecurityCenter();
    void changeMasterPassword();
    void exportRecoveryKey();
    void verifyRecoveryKey();
    bool ensureSecuritySessionInteractive(
        OpenPresentationPolicy presentation = OpenPresentationPolicy::FocusDocumentWorkspace);
    bool openPathUnified(const QString &path, EditorTab *reuseTab = nullptr,
                         std::optional<TextEncoding> forcedEncoding = std::nullopt,
                         OpenPresentationPolicy presentation =
                             OpenPresentationPolicy::FocusDocumentWorkspace,
                         OpenAuthorizationPolicy authorization =
                             OpenAuthorizationPolicy::AllowInteractive,
                         QString *failureReason = nullptr,
                         bool *securityRefused = nullptr);
    EditorTab *findOpenPlainDocument(const QString &path) const;
    bool openSecurePath(const QString &path, EditorTab *reuseTab = nullptr,
                        OpenPresentationPolicy presentation = OpenPresentationPolicy::FocusDocumentWorkspace);
    EditorTab *adoptSecureSnapshot(SecureDocumentSnapshot &snapshot,
                                   EditorTab *reuseTab = nullptr,
                                   OpenPresentationPolicy presentation = OpenPresentationPolicy::FocusDocumentWorkspace);
    enum class ExternalSaveDisposition { Proceed, HandledSuccess, Cancelled };
    ExternalSaveDisposition resolveExternalSaveConflict(EditorTab *tab);
    ExternalSaveDisposition resolveSecureExternalSaveConflict(EditorTab *tab);
    bool reloadTabFromDisk(EditorTab *tab);
    bool reloadSecureTabFromDisk(EditorTab *tab);
    void ensureSecurityServicesCreated();
    void ensureFindDialogCreated();
    void ensureAccountsWorkspaceCreated();
    void ensureSecurityCenterCreated();
    // closingApp=true: 关闭整个应用，未命名文件走 autosave 不提示
    // closingApp=false (默认): 关闭单个标签，未命名文件且有内容也要提示是否保存
    bool maybeSaveTab(int index, bool closingApp = false);
    void closeTabUnchecked(int index, bool checkSave = false);
    void closeOtherTabs(int keepIndex);
    void updateStatusBar();
    void showWorkspaceStatus(const QString &workspaceName);
    void updateRecentFiles(const QString &path);
    void applyEditorPreferences(EditorTab *tab);
    void rebuildRecentFilesMenu();
    void applyDarkMode(bool dark);
    void applyTheme(const ThemeTokens &t);
    void applyAppearanceMode();
    // 将当前 m_darkMode/m_wordWrap/m_edgeVisible/m_edgeColumn 同步到视图菜单的勾选状态
    // 必须在 createMenus 之后、loadSettings 之后调用，避免"设置恢复了但菜单勾选没变"
    void syncGlobalMenuCheckStates();
    bool isDocumentWorkspaceActive() const;
    void loadSettings();
    void saveSettings();
    bool saveSessionNow(bool forceSnapshots = false, bool allowRecoveryChoice = false);
    bool saveSecureTab(EditorTab *tab, bool allowInteractiveUnlock, bool showErrors = true);
    bool checkpointSecureTab(EditorTab *tab, QString *errorMessage = nullptr);
    static QString secureTitleFrom(EditorTab *tab);
    void purgeSensitiveSearchState();
    void rememberSensitiveDocumentClipboard(EditorTab *sourceTab);
    void clearSensitiveDocumentClipboard();
    void closeSecureDocumentOnSecurityLane(const QUuid &documentId);
    void closeAllSecureDocumentsOnSecurityLane();
    void endSecuritySessionOnSecurityLane(bool removeDeviceSession);
    void lockSecureDocumentsImpl(bool sessionExpired);
    void protectForSystemLock();
    void restoreAfterSystemUnlock();

    // ===== 启动阶段异步会话恢复（避免构造阻塞导致窗口不显示）=====
public slots:
    // ⚠️  只能由 main.cpp 在 w.show()+首帧绘制完成后调用（QTimer::singleShot 延时触发）
    //     禁止在 MainWindow 构造内直接/间接（zero-timer）调用，否则会抢在 Paint 前
    //     同步执行会话恢复，导致「全程静默不弹窗直到加载完成才显示」的用户体验 bug。
    void deferredRestoreSession();
private slots:
    // 恢复队列：每 singleShot(0) 处理 1 个 tab，处理间允许 UI 重绘
    void restoreNextSessionTab();
private:
    std::unique_ptr<WorkspaceRecoveryCoordinator> m_workspaceRecovery;

    // 给一个 widget 及（如果是 EditorTab）内部 editor/viewport 关闭 acceptDrops
    // 并安装事件过滤器，转发 drag/drop 到 MainWindow，防止子控件（QsciScintilla）吞事件
    void registerDropProxy(QWidget *w);

    QTimer m_autosaveTimer;
    QTimer m_secureAutosaveTimer;
    QTimer m_securityExpiryTimer;
    QTimer m_characterStatusTimer;
    bool m_systemProtectionActive = false;
    bool m_sessionNotificationsRegistered = false;
    bool m_restoreAccountsAfterSystemUnlock = false;
    bool m_systemLockRecoveryFailed = false;
    bool m_destroying = false;
    QByteArray m_sensitiveDocumentClipboardFingerprint;

    static constexpr int MAX_RECENT = 10;
};
