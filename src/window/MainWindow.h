#pragma once

#include <QMainWindow>
#include <QVector>
#include <QStringList>
#include <QTimer>
#include "TextEncoding.h"

class QTabWidget;
class QLabel;
class QMenu;
class QAction;
class EditorTab;
class FindReplaceDialog;
class SearchResultsDock;
class SessionManager;
class VaultDock;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // 从命令行/拖放等外部传入打开文件（支持多个）
    Q_INVOKABLE void openFilesFromExternal(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // 文件
    void newFile();
    void openFile();
    bool saveFile();
    bool saveAsFile();
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
    void toggleDarkMode();
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void toggleEdgeLine();
    void setEdgeColumnFromAction();
    void toggleWordWrap();
    void toggleHexViewMode();

    // 编码
    void setEncodingFromAction();
    void convertEncodingFromAction();
    void displayEncodingFromAction();

    // 标签
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onTabModifiedChanged(bool modified);
    void onTabCursorPositionChanged(int line, int index);
    void onTabTitleChanged(const QString &title);
    void onTabEncodingChanged(TextEncoding encoding);

    // ===== 统一同步：不适配 L2 的菜单项 enabled（行排序 + 编码"转为..."组 5 项）=====
    // 用户要求：直接 UI 设 disable（setEnabled=false 变灰点不动 → 用户体验最清楚），不要函数内 early return 冗余
    // 在 onTabChanged 以及所有 tab 的 l2ModeStatusChanged 信号被触发时调用；tab=nullptr 表示无 tab（动作全 disable）
    void syncL2SensitiveActions(EditorTab *tab = nullptr);

    // 搜索结果
    void onJumpTo(int line, int index);
    void onJumpToByteOffset(qint64 byteOffset, int length);

private:
    QTabWidget *m_tabs = nullptr;
    FindReplaceDialog *m_findDialog = nullptr;
    SearchResultsDock *m_searchDock = nullptr;
    VaultDock *m_vaultDock = nullptr;
    SessionManager *m_session = nullptr;

    // 状态栏
    QLabel *m_statusPos = nullptr;
    QLabel *m_statusEncoding = nullptr;
    QLabel *m_statusSize = nullptr;
    QLabel *m_statusSyntax = nullptr;
    QLabel *m_statusModified = nullptr;

    // 最近文件菜单
    QMenu *m_recentMenu = nullptr;
    QStringList m_recentFiles;

    // 编码菜单（记录以便切换标签时同步勾选状态）
    QMenu *m_encodingMenu = nullptr;
    QMenu *m_displayEncodingMenu = nullptr;

    // L2 模式下禁用不适配菜单项（D001~D007 强制，L2 isL2Active=true 时 setEnabled(false) 变灰）
    QAction *m_actSortAsc = nullptr;          // 编辑 → 行升序排序（全 Scintilla 代码，L2 空+超大文件不可排序）
    QAction *m_actSortDesc = nullptr;         // 编辑 → 行降序排序
    QVector<QAction *> m_convertEncodingActions;  // 编码 → 转为 GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE（5 项，只改标志未做 re-encode，L2 禁用）

    // 视图菜单 action（用于 loadSettings 后同步勾选状态，避免数据与UI不一致）
    QAction *m_actDarkMode = nullptr;
    QAction *m_actWordWrap = nullptr;
    QAction *m_actEdgeVisible = nullptr;
    QAction *m_actHexViewMode = nullptr;
    QActionGroup *m_edgeColGroup = nullptr;   // 长行标尺列数组，遍历其 action 同步勾选

    // Dark Mode
    bool m_darkMode = false;

    // 长行标尺
    bool m_edgeVisible = true;
    int m_edgeColumn = 80;

    // 自动换行
    bool m_wordWrap = false;

    // 查找历史
    QString m_lastFindPattern;
    bool m_lastFindRegex = false;
    bool m_lastFindCase = false;
    bool m_lastFindWord = false;

    void createActions();
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void createDock();
    void createFindDialog();

    EditorTab *currentTab() const;
    EditorTab *addNewTab();
    // closingApp=true: 关闭整个应用，未命名文件走 autosave 不提示
    // closingApp=false (默认): 关闭单个标签，未命名文件且有内容也要提示是否保存
    bool maybeSaveTab(int index, bool closingApp = false);
    void updateStatusBar();
    void updateRecentFiles(const QString &path);
    void rebuildRecentFilesMenu();
    void applyDarkMode(bool dark);
    // 将当前 m_darkMode/m_wordWrap/m_edgeVisible/m_edgeColumn 同步到视图菜单的勾选状态
    // 必须在 createMenus 之后、loadSettings 之后调用，避免"设置恢复了但菜单勾选没变"
    void syncGlobalMenuCheckStates();
    void loadSettings();
    void saveSettings();
    void restoreSession();
    void saveSessionNow();

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
    // 完整结构体定义在 MainWindow.cpp（避免在此处 #include SessionManager.h 拖慢编译）
    struct PendingSessionRestore;
    PendingSessionRestore *m_pendingRestore = nullptr;  // 仅在恢复期间非空

    // 给一个 widget 及（如果是 EditorTab）内部 editor/viewport 关闭 acceptDrops
    // 并安装事件过滤器，转发 drag/drop 到 MainWindow，防止子控件（QsciScintilla）吞事件
    void registerDropProxy(QWidget *w);

    QTimer m_autosaveTimer;

    static constexpr int MAX_RECENT = 10;
};
