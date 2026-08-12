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

    // 编码
    void setEncodingFromAction();
    void convertEncodingFromAction();

    // 标签
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onTabModifiedChanged(bool modified);
    void onTabCursorPositionChanged(int line, int index);
    void onTabTitleChanged(const QString &title);
    void onTabEncodingChanged(TextEncoding encoding);

    // 搜索结果
    void onJumpTo(int line, int index);

private:
    QTabWidget *m_tabs = nullptr;
    FindReplaceDialog *m_findDialog = nullptr;
    SearchResultsDock *m_searchDock = nullptr;
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

    // 视图菜单 action（用于 loadSettings 后同步勾选状态，避免数据与UI不一致）
    QAction *m_actDarkMode = nullptr;
    QAction *m_actWordWrap = nullptr;
    QAction *m_actEdgeVisible = nullptr;
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

    // 给一个 widget 及（如果是 EditorTab）内部 editor/viewport 关闭 acceptDrops
    // 并安装事件过滤器，转发 drag/drop 到 MainWindow，防止子控件（QsciScintilla）吞事件
    void registerDropProxy(QWidget *w);

    QTimer m_autosaveTimer;

    static constexpr int MAX_RECENT = 10;
};
