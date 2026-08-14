#include "MainWindow.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SessionManager.h"
#include "TextEncoding.h"
#include "DynamicThresholds.h"
#include "VirtualFileView.h"
#include "StatusBarTaskWidget.h"

#include <QTabWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStyle>
#include <QStatusBar>
#include <QLabel>
#include <QToolBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QSettings>
#include <QFont>
#include <QPalette>
#include <QIcon>
#include <QApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QDateTime>
#include <QElapsedTimer>

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

// L1/L2 自动分流打开文件（DESIGN §5.2 动态阈值）：
//   size < L1_EDIT_LIMIT → EditorTab::loadFile (QsciScintilla 完整编辑)
//   size ≥ → 先 try openLargeFileL2，失败回退到 loadFile（避免用户无法打开）
// 返回是否成功（与 loadFile 返回一致）
static bool openTabAutoMode(EditorTab *tab, const QString &path)
{
    if (!tab) return false;
    QFileInfo fi(path);
    qint64 size = fi.exists() ? fi.size() : 0;
    qint64 limit = DynamicThresholds::instance().L1_EDIT_LIMIT;
    if (size >= limit) {
        QString err;
        if (tab->openLargeFileL2(path, &err))
            return true;
        // L2 失败（mmap 不支持/编码不是 UTF-8 兼容字节？）：回退到 L1 分块加载
        // 不弹 warning：交给 caller 统一处理失败提示
    }
    return tab->loadFile(path);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
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
    setCentralWidget(m_tabs);
    // 把 m_tabs 也加入 drag 转发（防止 QTabBar 吞事件）
    registerDropProxy(m_tabs);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // 搜索 Dock
    m_searchDock = new SearchResultsDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_searchDock);
    m_searchDock->hide();
    connect(m_searchDock, &SearchResultsDock::jumpTo, this, &MainWindow::onJumpTo);
    connect(m_searchDock, &SearchResultsDock::jumpToByteOffset,
            this, &MainWindow::onJumpToByteOffset);

    // 查找/替换对话框
    m_findDialog = new FindReplaceDialog(this);
    m_findDialog->setSearchResultsDock(m_searchDock);
    connect(m_findDialog, &FindReplaceDialog::findParametersChanged,
            this, [this](const QString &pattern, bool regex, bool cs, bool wo) {
        m_lastFindPattern = pattern;
        m_lastFindRegex = regex;
        m_lastFindCase = cs;
        m_lastFindWord = wo;
    });

    // 会话管理器
    m_session = new SessionManager(this);

    createActions();
    createMenus();         // 菜单项用默认值创建，action 指针存在 m_act* / m_edgeColGroup
    createToolBar();
    createStatusBar();

    loadSettings();        // 从 QSettings 覆盖 m_darkMode/m_wordWrap/m_edge*
    syncGlobalMenuCheckStates(); // 关键：把恢复后的设置同步回菜单勾选状态（解决UI不同步bug）
    applyDarkMode(m_darkMode);

    // ============================================================
    //  启动阶段会话恢复触发权移交给 main.cpp !!
    //  原做法 v1：构造内同步 restoreSession() → 构造阻塞 → w.show() 永远执行不到
    //  原做法 v2：构造内 QTimer::singleShot(0, deferredRestoreSession)
    //             → main.cpp 的 processEvents 派发时，zero-timer 比 Paint 事件先运行
    //             → 会话恢复先跑完 916ms 才开始绘制窗口 → 用户依然感觉「双击没反应/静默」
    //  新做法 v3：main.cpp 在 w.show() + sync() + 双 processEvents + 打日志确认可见后，
    //             再用 QTimer::singleShot(30ms) 触发 deferredRestoreSession。
    //             30ms 延时 + DWM 已合成首帧 → 窗口标题栏/空客户区必然先对用户可见，
    //             之后才开始状态栏显示「正在恢复会话 0/N」异步加载 tab。
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

    auto *newAct = fileMenu->addAction(QStringLiteral("新建(&N)"));
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, &MainWindow::newFile);

    auto *openAct = fileMenu->addAction(QStringLiteral("打开(&O)..."));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开(&R)"));

    fileMenu->addSeparator();

    auto *saveAct = fileMenu->addAction(QStringLiteral("保存(&S)"));
    saveAct->setShortcut(QKeySequence::Save);
    connect(saveAct, &QAction::triggered, this, &MainWindow::saveFile);

    auto *saveAsAct = fileMenu->addAction(QStringLiteral("另存为(&A)..."));
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::saveAsFile);

    fileMenu->addSeparator();

    auto *exitAct = fileMenu->addAction(QStringLiteral("退出(&X)"));
    exitAct->setShortcut(QKeySequence::Quit);
    connect(exitAct, &QAction::triggered, this, &QMainWindow::close);

    rebuildRecentFilesMenu();

    // 编辑菜单
    auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));

    auto *undoAct = editMenu->addAction(QStringLiteral("撤销(&U)"));
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->editor()->undo();
    });

    auto *redoAct = editMenu->addAction(QStringLiteral("重做(&R)"));
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->editor()->redo();
    });

    editMenu->addSeparator();

    auto *cutAct = editMenu->addAction(QStringLiteral("剪切(&T)"));
    cutAct->setShortcut(QKeySequence::Cut);
    connect(cutAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->cut();
    });

    auto *copyAct = editMenu->addAction(QStringLiteral("复制(&C)"));
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->copy();
    });

    auto *pasteAct = editMenu->addAction(QStringLiteral("粘贴(&P)"));
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->paste();
    });

    auto *selectAllAct = editMenu->addAction(QStringLiteral("全选(&A)"));
    selectAllAct->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTab()) t->selectAll();
    });

    editMenu->addSeparator();

    auto *dupLineAct = editMenu->addAction(QStringLiteral("复制当前行(&D)"));
    dupLineAct->setShortcut(QKeySequence("Ctrl+D"));
    connect(dupLineAct, &QAction::triggered, this, &MainWindow::duplicateLine);

    auto *delLineAct = editMenu->addAction(QStringLiteral("删除当前行(&L)"));
    delLineAct->setShortcut(QKeySequence("Ctrl+Shift+L"));
    connect(delLineAct, &QAction::triggered, this, &MainWindow::deleteLine);

    auto *moveUpAct = editMenu->addAction(QStringLiteral("上移当前行"));
    moveUpAct->setShortcut(QKeySequence("Alt+Up"));
    connect(moveUpAct, &QAction::triggered, this, &MainWindow::moveLineUp);

    auto *moveDownAct = editMenu->addAction(QStringLiteral("下移当前行"));
    moveDownAct->setShortcut(QKeySequence("Alt+Down"));
    connect(moveDownAct, &QAction::triggered, this, &MainWindow::moveLineDown);

    editMenu->addSeparator();

    auto *sortAscAct = editMenu->addAction(QStringLiteral("行升序排序"));
    m_actSortAsc = sortAscAct;
    connect(sortAscAct, &QAction::triggered, this, &MainWindow::sortLinesAsc);

    auto *sortDescAct = editMenu->addAction(QStringLiteral("行降序排序"));
    m_actSortDesc = sortDescAct;
    connect(sortDescAct, &QAction::triggered, this, &MainWindow::sortLinesDesc);

    // 搜索菜单
    auto *searchMenu = menuBar()->addMenu(QStringLiteral("搜索(&S)"));

    auto *findAct = searchMenu->addAction(QStringLiteral("查找(&F)..."));
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this, &MainWindow::showFindDialog);

    auto *replaceAct = searchMenu->addAction(QStringLiteral("替换(&R)..."));
    // 用户强制：菜单显示 (&R) → 全局快捷键必须为 Ctrl+R 显示一致（Notepad++ 习惯），禁止 Ctrl+H
    // 见 QA SR02-COMFORT 3 条硬断言：text 含 &R + shortcut==QKeySequence("Ctrl+R") 精确 + Alt+S+R 访问键可打开
    replaceAct->setShortcut(QKeySequence("Ctrl+R"));
    connect(replaceAct, &QAction::triggered, this, &MainWindow::showReplaceDialog);

    searchMenu->addSeparator();

    auto *findNextAct = searchMenu->addAction(QStringLiteral("查找下一个"));
    findNextAct->setShortcut(QKeySequence("F3"));
    connect(findNextAct, &QAction::triggered, this, &MainWindow::findNext);

    auto *findPrevAct = searchMenu->addAction(QStringLiteral("查找上一个"));
    findPrevAct->setShortcut(QKeySequence("Shift+F3"));
    connect(findPrevAct, &QAction::triggered, this, &MainWindow::findPrev);

    // 语法菜单
    auto *syntaxMenu = menuBar()->addMenu(QStringLiteral("语法(&Y)"));
    auto *syntaxGroup = new QActionGroup(this);
    QStringList syntaxNames = {
        QStringLiteral("纯文本"), QStringLiteral("C/C++"),
        QStringLiteral("Python"), QStringLiteral("JSON"),
        QStringLiteral("HTML"), QStringLiteral("XML"),
        QStringLiteral("SQL"), QStringLiteral("YAML"),
        QStringLiteral("Markdown"),
    };
    for (int i = 0; i < syntaxNames.size(); ++i) {
        auto *act = syntaxMenu->addAction(syntaxNames[i]);
        act->setCheckable(true);
        act->setData(i);
        syntaxGroup->addAction(act);
        if (i == 0) act->setChecked(true);
        connect(act, &QAction::triggered, this, &MainWindow::setSyntaxFromAction);
    }

    // 书签菜单
    auto *bookmarkMenu = menuBar()->addMenu(QStringLiteral("书签(&B)"));

    auto *toggleBmAct = bookmarkMenu->addAction(QStringLiteral("切换书签(&M)"));
    toggleBmAct->setShortcut(QKeySequence("Ctrl+F2"));
    connect(toggleBmAct, &QAction::triggered, this, &MainWindow::toggleBookmark);

    auto *nextBmAct = bookmarkMenu->addAction(QStringLiteral("下一书签"));
    nextBmAct->setShortcut(QKeySequence("F2"));
    connect(nextBmAct, &QAction::triggered, this, &MainWindow::nextBookmark);

    auto *prevBmAct = bookmarkMenu->addAction(QStringLiteral("上一书签"));
    prevBmAct->setShortcut(QKeySequence("Shift+F2"));
    connect(prevBmAct, &QAction::triggered, this, &MainWindow::prevBookmark);

    bookmarkMenu->addSeparator();

    auto *clearBmAct = bookmarkMenu->addAction(QStringLiteral("清除所有书签"));
    connect(clearBmAct, &QAction::triggered, this, &MainWindow::clearBookmarks);

    // 视图菜单
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));

    m_actDarkMode = viewMenu->addAction(QStringLiteral("深色模式"));
    m_actDarkMode->setCheckable(true);
    m_actDarkMode->setChecked(m_darkMode);
    connect(m_actDarkMode, &QAction::triggered, this, &MainWindow::toggleDarkMode);

    viewMenu->addSeparator();

    m_actWordWrap = viewMenu->addAction(QStringLiteral("自动换行"));
    m_actWordWrap->setCheckable(true);
    m_actWordWrap->setChecked(m_wordWrap);
    m_actWordWrap->setShortcut(QKeySequence("Ctrl+W"));
    connect(m_actWordWrap, &QAction::triggered, this, &MainWindow::toggleWordWrap);

    viewMenu->addSeparator();

    m_actEdgeVisible = viewMenu->addAction(QStringLiteral("长行标尺(竖线)"));
    m_actEdgeVisible->setCheckable(true);
    m_actEdgeVisible->setChecked(m_edgeVisible);
    connect(m_actEdgeVisible, &QAction::triggered, this, &MainWindow::toggleEdgeLine);

    viewMenu->addSeparator();

    m_actHexViewMode = viewMenu->addAction(QStringLiteral("16 进制编辑模式"));
    m_actHexViewMode->setCheckable(true);
    m_actHexViewMode->setChecked(false);
    m_actHexViewMode->setShortcut(QKeySequence("Ctrl+Shift+H"));
    connect(m_actHexViewMode, &QAction::triggered, this, &MainWindow::toggleHexViewMode);

    viewMenu->addSeparator();

    auto *edgeColMenu = viewMenu->addMenu(QStringLiteral("长行标尺列数"));
    m_edgeColGroup = new QActionGroup(this);
    QList<int> edgeCols = {72, 80, 100, 120, 150, 200};
    for (int col : edgeCols) {
        auto *a = edgeColMenu->addAction(QStringLiteral("%1 列").arg(col));
        a->setCheckable(true);
        a->setData(col);
        m_edgeColGroup->addAction(a);
        if (col == m_edgeColumn) a->setChecked(true);
        connect(a, &QAction::triggered, this, &MainWindow::setEdgeColumnFromAction);
    }

    viewMenu->addSeparator();

    auto *zoomInAct = viewMenu->addAction(QStringLiteral("放大(&+)"));
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::zoomIn);

    auto *zoomOutAct = viewMenu->addAction(QStringLiteral("缩小(&-)"));
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::zoomOut);

    auto *resetZoomAct = viewMenu->addAction(QStringLiteral("重置缩放"));
    resetZoomAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(resetZoomAct, &QAction::triggered, this, &MainWindow::resetZoom);

    // 编码菜单（Notepad++ 同款）
    m_encodingMenu = menuBar()->addMenu(QStringLiteral("编码(&N)"));
    auto *encGroup = new QActionGroup(this);
    // 按 Notepad++ 顺序：ANSI -> UTF-8 -> UTF-8 BOM -> UCS-2 LE -> UCS-2 BE
    QVector<QPair<QString, TextEncoding>> encItems = {
        {QStringLiteral("转为 ANSI (GBK)"),          TextEncoding::GBK},
        {QStringLiteral("转为 UTF-8 (无 BOM)"),       TextEncoding::Utf8},
        {QStringLiteral("转为 UTF-8 (含 BOM)"),       TextEncoding::Utf8Bom},
        {QStringLiteral("转为 UCS-2 Little Endian"),  TextEncoding::Utf16LE},
        {QStringLiteral("转为 UCS-2 Big Endian"),     TextEncoding::Utf16BE},
    };
    m_convertEncodingActions.clear();
    for (const auto &item : encItems) {
        auto *a = m_encodingMenu->addAction(item.first);
        a->setCheckable(true);
        a->setData(static_cast<int>(item.second));
        encGroup->addAction(a);
        m_convertEncodingActions.append(a);
        connect(a, &QAction::triggered, this, &MainWindow::convertEncodingFromAction);
    }

    // "以编码显示"子菜单（Notepad++ 风格：不修改文件内容，仅改变显示解码方式）
    m_encodingMenu->addSeparator();
    m_displayEncodingMenu = m_encodingMenu->addMenu(QStringLiteral("以编码显示"));
    auto *dispEncGroup = new QActionGroup(this);
    QVector<QPair<QString, TextEncoding>> dispEncItems = {
        {QStringLiteral("以 ANSI (GBK) 显示"),        TextEncoding::GBK},
        {QStringLiteral("以 UTF-8 显示"),             TextEncoding::Utf8},
        {QStringLiteral("以 UTF-8 (含 BOM) 显示"),    TextEncoding::Utf8Bom},
        {QStringLiteral("以 UCS-2 Little Endian 显示"), TextEncoding::Utf16LE},
        {QStringLiteral("以 UCS-2 Big Endian 显示"),    TextEncoding::Utf16BE},
    };
    for (const auto &item : dispEncItems) {
        auto *a = m_displayEncodingMenu->addAction(item.first);
        a->setCheckable(true);
        a->setData(static_cast<int>(item.second));
        dispEncGroup->addAction(a);
        connect(a, &QAction::triggered, this, &MainWindow::displayEncodingFromAction);
    }
    // 注意：菜单勾选在 onTabChanged / onTabEncodingChanged 中同步

    // 帮助菜单
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto *aboutAct = helpMenu->addAction(QStringLiteral("关于(&A)"));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, QStringLiteral("关于 Notepad Safe"),
            QStringLiteral("<h3>Notepad Safe</h3>"
                           "<p>基于 Qt 6.11 + QScintilla 2.14.1 的加密笔记本编辑器。</p>"
                           "<p>版本: 0.3.1</p>"
                           "<p>许可证: GPL v3</p>"));
    });
}

void MainWindow::createToolBar()
{
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(16, 16));

    toolbar->addAction(QStringLiteral("新建"), this, &MainWindow::newFile);
    toolbar->addAction(QStringLiteral("打开"), this, &MainWindow::openFile);
    toolbar->addAction(QStringLiteral("保存"), this, &MainWindow::saveFile);
    toolbar->addSeparator();
    toolbar->addAction(QStringLiteral("查找"), this, &MainWindow::showFindDialog);
    toolbar->addAction(QStringLiteral("替换"), this, &MainWindow::showReplaceDialog);
}

void MainWindow::createStatusBar()
{
    m_statusPos = new QLabel(QStringLiteral("行: 1, 列: 1"));
    m_statusPos->setMinimumWidth(120);
    statusBar()->addWidget(m_statusPos);

    m_statusEncoding = new QLabel(QStringLiteral("UTF-8"));
    m_statusEncoding->setMinimumWidth(80);
    statusBar()->addWidget(m_statusEncoding);

    m_statusSyntax = new QLabel(QStringLiteral("纯文本"));
    m_statusSyntax->setMinimumWidth(80);
    statusBar()->addWidget(m_statusSyntax);

    m_statusSize = new QLabel(QStringLiteral("0 B"));
    m_statusSize->setMinimumWidth(80);
    statusBar()->addWidget(m_statusSize);

    m_statusModified = new QLabel(QStringLiteral(""));
    m_statusModified->setMinimumWidth(40);
    statusBar()->addPermanentWidget(m_statusModified);

    // ============ 通用异步任务：状态栏右下角永久 widget ============
    //  显示：线程占用（活跃/并发上限）+ 后台任务数量按钮
    //  点击按钮：弹出 AsyncTaskPanelDialog（非模态，查看全部任务/进度/取消/异常堆栈）
    auto *taskWidget = new StatusBarTaskWidget(this);
    statusBar()->addPermanentWidget(taskWidget);
}

void MainWindow::createDock()
{
    // Dock 在构造函数中已创建
}

void MainWindow::createFindDialog()
{
    // 查找对话框在构造函数中已创建
}

// ============ 文件操作 ============

void MainWindow::newFile()
{
    addNewTab();
}

EditorTab *MainWindow::addNewTab()
{
    auto *tab = new EditorTab(this);
    int index = m_tabs->addTab(tab, QStringLiteral("未命名"));

    // 注册拖放代理：保证拖到这个 EditorTab/editor/viewport 任意位置都能触发 MainWindow 打开
    registerDropProxy(tab);

    // 应用全局设置（标尺 + 换行）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);

    connect(tab, &EditorTab::modificationChanged, this, [this, tab](bool m) {
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0) {
            onTabModifiedChanged(m);
            m_tabs->setTabText(idx, tab->tabTitle());
        }
    });
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
        if (ok) statusBar()->showMessage(QObject::tr("行索引构建完成"), 3000);
        else    statusBar()->clearMessage();
    });

    // ============== L2 禁用不适配菜单：同一 tab 内部 L1↔L2 切换时立即同步（关键：addTab/currentChanged 不触发）==============
    // 最小侵入：直接用 sender() 转 EditorTab* 当 tab 参数（精准指向信号发射源，避免无参
    // syncL2SensitiveActions() → currentTab() 在 currentIndex 未切好时查到旧 tab → 新 tab 的
    // L2 状态被旧 tab 覆盖（enabled=true 不变灰的 D001/D003/D007 根因）。
    connect(tab, &EditorTab::l2ModeStatusChanged, this, [this]() {
        auto* src = qobject_cast<EditorTab*>(sender());
        syncL2SensitiveActions(src ? src : currentTab());
    });

    m_tabs->setCurrentIndex(index);
    m_findDialog->setActiveTab(tab);
    // 新建后立刻同步一次 L2 敏感动作（覆盖新 tab 还没收到任何信号的早期空状态）
    syncL2SensitiveActions(tab);
    return tab;
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
        auto *tab = addNewTab();
        if (!openTabAutoMode(tab, file)) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                                 QStringLiteral("无法打开文件: %1").arg(file));
            // 关闭空标签
            int idx = m_tabs->indexOf(tab);
            if (idx >= 0)
                m_tabs->removeTab(idx);
        } else {
            updateRecentFiles(file);
            m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
        }
    }
}

bool MainWindow::saveFile()
{
    auto *tab = currentTab();
    if (!tab)
        return false;
    if (tab->isUntitled())
        return saveAsFile();
    QString err;
    if (!tab->saveFile(&err)) {
        QString detail = err.isEmpty() ? tab->filePath() : QStringLiteral("%1\n\n详细信息：%2").arg(tab->filePath(), err);
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法保存文件：\n%1").arg(detail));
        return false;
    }
    return true;
}

bool MainWindow::saveAsFile()
{
    auto *tab = currentTab();
    if (!tab)
        return false;
    QString fileName = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存为"), QString(),
        QStringLiteral("所有文件 (*);;文本文件 (*.txt);;C/C++ (*.cpp);;Python (*.py);;JSON (*.json)"));
    if (fileName.isEmpty())
        return false;
    QString err;
    if (!tab->saveAsFile(fileName, &err)) {
        QString detail = err.isEmpty() ? fileName : QStringLiteral("%1\n\n详细信息：%2").arg(fileName, err);
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法保存文件：\n%1").arg(detail));
        return false;
    }
    updateRecentFiles(fileName);
    int idx = m_tabs->indexOf(tab);
    if (idx >= 0)
        m_tabs->setTabText(idx, tab->tabTitle());
    return true;
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
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("文件不存在: %1").arg(path));
        m_recentFiles.removeAll(path);
        rebuildRecentFilesMenu();
        return;
    }
    auto *tab = addNewTab();
    if (!openTabAutoMode(tab, path)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件: %1").arg(path));
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0)
            m_tabs->removeTab(idx);
    } else {
        m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
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
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showFindMode();
}

void MainWindow::showReplaceDialog()
{
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showReplaceMode();
}

void MainWindow::findNext()
{
    auto *tab = currentTab();
    if (!tab || m_lastFindPattern.isEmpty())
        return;
    tab->findNext(m_lastFindPattern, m_lastFindRegex, m_lastFindCase,
                  m_lastFindWord, true, true);
}

void MainWindow::findPrev()
{
    auto *tab = currentTab();
    if (!tab || m_lastFindPattern.isEmpty())
        return;
    tab->findNext(m_lastFindPattern, m_lastFindRegex, m_lastFindCase,
                  m_lastFindWord, false, true);
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

void MainWindow::toggleDarkMode()
{
    m_darkMode = !m_darkMode;
    applyDarkMode(m_darkMode);
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
        // L1 tab：提示用户 16 进制模式仅支持大文件 L2 视图
        if (m_actHexViewMode) m_actHexViewMode->setChecked(false);
        return;
    }
    ViewMode now = tab->viewModeL2();
    ViewMode next = (now == ViewMode::Hex) ? ViewMode::Text : ViewMode::Hex;
    tab->setViewModeL2(next);
    if (m_actHexViewMode) m_actHexViewMode->setChecked(next == ViewMode::Hex);
}

// 统一把 m_darkMode/m_wordWrap/m_edgeVisible/m_edgeColumn 同步到视图菜单的勾选
// 这是解决"恢复设置后菜单勾选未同步显示"的核心函数
void MainWindow::syncGlobalMenuCheckStates()
{
    if (m_actDarkMode)  m_actDarkMode->setChecked(m_darkMode);
    if (m_actWordWrap)  m_actWordWrap->setChecked(m_wordWrap);
    if (m_actEdgeVisible) m_actEdgeVisible->setChecked(m_edgeVisible);
    if (m_actHexViewMode) {
        auto *tab = currentTab();
        bool checked = tab && tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex;
        m_actHexViewMode->setChecked(checked);
    }
    if (m_edgeColGroup) {
        for (QAction *a : m_edgeColGroup->actions()) {
            a->setChecked(a->data().toInt() == m_edgeColumn);
        }
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
        EditorTab *tab = nullptr;
        if (idx == 0 && anyEmptyUntitledIdx >= 0) {
            tab = qobject_cast<EditorTab *>(m_tabs->widget(anyEmptyUntitledIdx));
        } else {
            tab = addNewTab();
        }
        if (!tab) continue;
        if (openTabAutoMode(tab, p)) {
            updateRecentFiles(p);
            m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
        } else {
            QMessageBox::warning(this, QStringLiteral("错误"),
                QStringLiteral("无法打开文件: %1").arg(p));
            // 如果是新创建的空标签则关闭
            if (anyEmptyUntitledIdx < 0 || idx != 0) {
                int i = m_tabs->indexOf(tab);
                if (i >= 0) m_tabs->removeTab(i);
            }
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
    if (tab->encoding() == enc) return;

    // Notepad++ 语义：改变下次保存时的编码标记，并在状态栏显示
    if (!tab->isUntitled() && tab->isModified()) {
        auto ret = QMessageBox::question(
            this, QStringLiteral("编码切换"),
            QStringLiteral("当前文件有未保存的修改。\n"
                           "切换编码后，下次保存将使用新编码写入。\n"
                           "是否继续？"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            // 恢复勾选状态
            syncEncodingMenuTo(m_encodingMenu, tab->encoding());
            return;
        }
    }
    tab->setEncoding(enc);
    // 切换编码视为需要保存的修改
    if (!tab->isModified())
        tab->setModified(true);
}

void MainWindow::displayEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    // Notepad++ "以编码显示"：不修改文件内容，仅改变显示解码方式
    tab->setDisplayEncoding(enc);
}

void MainWindow::applyDarkMode(bool dark)
{
    QPalette pal;
    if (dark) {
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
    } else {
        pal = QApplication::style()->standardPalette();
    }
    qApp->setPalette(pal);

    // 更新所有 EditorTab 的编辑器配色
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab)
            continue;
        auto *e = tab->editor();
        if (dark) {
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

// ============ 标签事件 ============

EditorTab *MainWindow::currentTab() const
{
    return qobject_cast<EditorTab *>(m_tabs->currentWidget());
}

void MainWindow::syncL2SensitiveActions(EditorTab *tab)
{
    if (!tab) tab = currentTab();
    // 无 tab 时：所有这些动作一律禁用，避免误点
    const bool enabled = (tab != nullptr) && !tab->isL2Active();
    if (m_actSortAsc) m_actSortAsc->setEnabled(enabled);
    if (m_actSortDesc) m_actSortDesc->setEnabled(enabled);
    for (auto *a : m_convertEncodingActions) {
        if (a) a->setEnabled(enabled);
    }
    // 注："以编码显示..."组不在 m_convertEncodingActions 里，它始终可点（只改显示解码不改字节流，L2 允许）
}

void MainWindow::onTabChanged(int index)
{
    EditorTab *tab = nullptr;
    if (index >= 0) tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (m_findDialog) m_findDialog->setActiveTab(tab);
    // 统一同步不适配 L2 的菜单项（含 index<0 无 tab → 全部变灰）
    syncL2SensitiveActions(tab);
    if (!tab) return;
    // 同步编码菜单勾选到当前标签
    syncEncodingMenuTo(m_encodingMenu, tab->encoding());
    syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
    // 应用全局设置（保持所有标签一致）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);
    updateStatusBar();
}

void MainWindow::onTabCloseRequested(int index)
{
    // closingApp=false: 单标签关闭模式
    if (!maybeSaveTab(index, /*closingApp=*/false))
        return;
    auto *w = m_tabs->widget(index);
    auto *tab = qobject_cast<EditorTab *>(w);
    // 只要标签被移除（无论何种情况），未命名的 autosave 必须删除（不在打开标签里了）
    if (tab) {
        tab->clearAutosaveFile();
    }
    m_tabs->removeTab(index);
    delete w;
}

bool MainWindow::maybeSaveTab(int index, bool closingApp)
{
    auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (!tab)
        return true;

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
    tab->clearAutosaveFile();
    return true;
}

void MainWindow::onTabModifiedChanged(bool modified)
{
    m_statusModified->setText(modified ? QStringLiteral("● 已修改") : QStringLiteral(""));
}

void MainWindow::onTabCursorPositionChanged(int line, int index)
{
    m_statusPos->setText(QStringLiteral("行: %1, 列: %2").arg(line + 1).arg(index + 1));
}

void MainWindow::onTabTitleChanged(const QString &title)
{
    Q_UNUSED(title);
    // tab 文本在 lambda 中更新
}

void MainWindow::onTabEncodingChanged(TextEncoding encoding)
{
    m_statusEncoding->setText(encodingName(encoding));
    syncEncodingMenuTo(m_encodingMenu, encoding);
}

void MainWindow::onJumpTo(int line, int index)
{
    auto *tab = currentTab();
    if (!tab)
        return;
    tab->setCursorPosition(line, index);
    tab->setFocusEditor();
}

void MainWindow::onJumpToByteOffset(qint64 byteOffset, int length)
{
    auto *tab = currentTab();
    if (!tab) return;
    tab->gotoByteOffset(byteOffset, length);
    tab->setFocusEditor();
}

void MainWindow::updateStatusBar()
{
    auto *tab = currentTab();
    if (!tab)
        return;

    int line, index;
    tab->getCursorPosition(&line, &index);
    m_statusPos->setText(QStringLiteral("行: %1, 列: %2").arg(line + 1).arg(index + 1));
    m_statusEncoding->setText(encodingName(tab->encoding()));

    QStringList syntaxNames = {
        QStringLiteral("纯文本"), QStringLiteral("C/C++"),
        QStringLiteral("Python"), QStringLiteral("JSON"),
        QStringLiteral("HTML"), QStringLiteral("XML"),
        QStringLiteral("SQL"), QStringLiteral("YAML"),
        QStringLiteral("Markdown"),
    };
    int s = static_cast<int>(tab->syntax());
    if (s >= 0 && s < syntaxNames.size())
        m_statusSyntax->setText(syntaxNames[s]);

    // 文件大小
    qint64 size = 0;
    if (tab->isL2Mode() && tab->l2View()) {
        size = tab->l2View()->fileSize();
    } else if (tab->filePath().isEmpty()) {
        size = tab->editor()->length();
    } else {
        size = QFileInfo(tab->filePath()).size();
    }
    QString sizeStr;
    if (size < 1024)
        sizeStr = QStringLiteral("%1 B").arg(size);
    else if (size < 1024 * 1024)
        sizeStr = QStringLiteral("%1 KB").arg(size / 1024.0, 0, 'f', 1);
    else
        sizeStr = QStringLiteral("%1 MB").arg(size / (1024.0 * 1024), 0, 'f', 2);
    m_statusSize->setText(sizeStr);

    m_statusModified->setText(tab->isModified() ? QStringLiteral("● 已修改") : QStringLiteral(""));
}

// ============ 最近文件 ============

void MainWindow::updateRecentFiles(const QString &path)
{
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
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
            if (st.filePath.isEmpty()) {
                // 未命名文件：使用 loadAutosave，对用户保持"未命名"
                loaded = tab->loadAutosave(st.autosavePath);
                if (loaded) {
                    hasUnsaved = true;
                }
            } else {
                // 已存在文件有修改：从 autosave 恢复内容（但 loadFile 会设置 m_filePath，
                // 我们需要先 load autosave 再设置真实 file path）
                loaded = tab->loadAutosave(st.autosavePath);
                if (loaded) {
                    tab->setFilePath(st.filePath);  // 恢复真实路径
                    tab->setModified(true);
                    hasUnsaved = true;
                }
            }
            // 恢复语法（autosave 本身不猜语法）
            tab->setSyntax(static_cast<Syntax>(st.syntax));
        } else if (!st.filePath.isEmpty() && QFileInfo::exists(st.filePath)) {
            loaded = tab->loadFile(st.filePath);
        }

        if (!loaded) {
            tab->setSyntax(static_cast<Syntax>(st.syntax));
        } else {
            tab->setCursorPosition(st.cursorLine, st.cursorIndex);
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

void MainWindow::saveSessionNow()
{
    QVector<EditorTab *> tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab)
            tabs.append(tab);
    }
    m_session->saveSession(tabs, m_tabs->currentIndex());
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
};

// ============================================================================
//  异步会话恢复：deferredRestoreSession + restoreNextSessionTab
//  设计目的：
//   · 构造函数返回前不读任何大文件（避免 main.cpp 的 w.show() 被延后）
//   · 每个 tab 恢复之间 QApplication::processEvents 让窗口重绘
//   · 走 openTabAutoMode ≥32MB 强制 L2 mmap 秒开，不再 L1 主线程阻塞读
// ============================================================================
void MainWindow::deferredRestoreSession()
{
    // 此时 main.cpp 的 w.show() 已调用；但首次 paint 需要事件循环派发。
    // 主动 AllEvents 派发一次，确保窗口立刻可见（用户不会"双击后以为没启动"）。
    QApplication::processEvents(QEventLoop::AllEvents);

    SessionData data = m_session->loadSession();
    if (data.tabs.isEmpty()) {
        // 没有会话记录：补一个空 tab（对应原构造函数中的空 tab 兜底）
        if (m_tabs->count() == 0)
            addNewTab();
        return;
    }

    // 构造排队上下文（使用 new，因为恢复过程跨多个事件循环 tick）
    m_pendingRestore = new PendingSessionRestore;
    m_pendingRestore->tabs                 = data.tabs;   // 拷贝 session 列表
    m_pendingRestore->activeTab            = data.activeTab;
    m_pendingRestore->currentIndex         = 0;
    m_pendingRestore->hasUnsaved           = false;
    m_pendingRestore->sessionJsonPathToRemove = m_session->sessionFilePath();

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
        if (m_pendingRestore->activeTab >= 0 && m_pendingRestore->activeTab < m_tabs->count())
            m_tabs->setCurrentIndex(m_pendingRestore->activeTab);

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
            addNewTab();
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

    if (!st.autosavePath.isEmpty() && QFileInfo::exists(st.autosavePath)) {
        // ====== 关键修复（BUG C：大文件 autosave 阻塞启动）======
        //   旧做法：直接 tab->loadAutosave(st.autosavePath)
        //     · 100MB autosave：file.readAll() 主线程同步读 1~3s，窗口不显示、无提示
        //   新做法：tab->openAutosaveAutoMode(autosave, real)
        //     · 尺寸 < L1_EDIT_LIMIT：走原 loadAutosave（毫秒级）
        //     · 尺寸 ≥ 限制：L2 mmap autosave 文件 → incomplete LineIndex → 秒开返回
        //       后台 AsyncTaskManager 增量构建行索引，用户可见窗口 + 状态栏进度
        //   参数：autosavePath = 实际数据源，filePath = 对用户显示的原名/未来保存路径
        const qint64 asSize = QFileInfo(st.autosavePath).size();
        const qint64 limit  = DynamicThresholds::instance().L1_EDIT_LIMIT;
        qDebug("[STARTUP][RESTORE tab %d/%d] → 走 autosave 分支: size=%lld KB, limit=%lld KB (L1_EDIT_LIMIT=%lld MB)",
               m_pendingRestore->currentIndex + 1, total,
               asSize / 1024, limit / 1024, limit / (1024 * 1024));
        if (asSize >= limit) {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L2 mmap 秒开 (asSize >= L1_EDIT_LIMIT)，"
                   "应 < 50ms 返回；若之后 >= 500ms 才返回，说明 openAutosaveAutoMode 内部阻塞了！",
                   m_pendingRestore->currentIndex + 1, total);
        } else {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L1 同步 loadAutosave (asSize < 限制)，"
                   "一般毫秒级。", m_pendingRestore->currentIndex + 1, total);
        }

        if (st.filePath.isEmpty()) {
            loaded = tab->openAutosaveAutoMode(st.autosavePath);
            if (loaded) m_pendingRestore->hasUnsaved = true;
        } else {
            loaded = tab->openAutosaveAutoMode(st.autosavePath, st.filePath);
            if (loaded) m_pendingRestore->hasUnsaved = true;
        }
        qDebug("[STARTUP][RESTORE tab %d/%d]   openAutosaveAutoMode 返回 ok=%d, 耗时=%lld ms（关键：如果这里 > 500ms 就是阻塞启动的根因！）",
               m_pendingRestore->currentIndex + 1, total, loaded ? 1 : 0, tabTimer.elapsed());
        tab->setSyntax(static_cast<Syntax>(st.syntax));
    } else if (!st.filePath.isEmpty() && QFileInfo::exists(st.filePath)) {
        // ===== 关键修复：不再直接 tab->loadFile(st.filePath) =====
        //   原 bug：即便 100MB 也走 L1 全量 readAll + 主线程分块；
        //   新逻辑：openTabAutoMode → ≥ 32MB 自动 openLargeFileL2 (mmap + 后台 AsyncTaskManager)
        const qint64 realSize = QFileInfo(st.filePath).size();
        const qint64 limit    = DynamicThresholds::instance().L1_EDIT_LIMIT;
        qDebug("[STARTUP][RESTORE tab %d/%d] → 走真实文件路径分支: size=%lld KB, limit=%lld KB (L1_EDIT_LIMIT=%lld MB)",
               m_pendingRestore->currentIndex + 1, total,
               realSize / 1024, limit / 1024, limit / (1024 * 1024));
        if (realSize >= limit) {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L2 mmap 秒开 (size >= L1_EDIT_LIMIT)，应 < 50ms 返回。",
                   m_pendingRestore->currentIndex + 1, total);
        } else {
            qDebug("[STARTUP][RESTORE tab %d/%d]   自动选 L1 同步 loadFile (< 限制)。",
                   m_pendingRestore->currentIndex + 1, total);
        }
        loaded = openTabAutoMode(tab, st.filePath);
        qDebug("[STARTUP][RESTORE tab %d/%d]   openTabAutoMode 返回 ok=%d, 耗时=%lld ms（关键：如果 > 500ms 就是阻塞启动的根因！）",
               m_pendingRestore->currentIndex + 1, total, loaded ? 1 : 0, tabTimer.elapsed());
    } else {
        qDebug("[STARTUP][RESTORE tab %d/%d] → 文件不存在或路径为空，保持空标签页",
               m_pendingRestore->currentIndex + 1, total);
    }
    tabTimer.restart();

    if (!loaded) {
        tab->setSyntax(static_cast<Syntax>(st.syntax));
    } else {
        tab->setCursorPosition(st.cursorLine, st.cursorIndex);
    }
    m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());

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
    QSettings settings("NotepadSafe", "NotepadSafe");
    m_darkMode = settings.value("darkMode", false).toBool();
    m_recentFiles = settings.value("recentFiles").toStringList();
    while (m_recentFiles.size() > MAX_RECENT)
        m_recentFiles.removeLast();

    m_edgeVisible = settings.value("edgeVisible", true).toBool();
    m_edgeColumn = settings.value("edgeColumn", 80).toInt();
    if (m_edgeColumn < 1) m_edgeColumn = 80;
    m_wordWrap = settings.value("wordWrap", false).toBool();

    QSize winSize = settings.value("windowSize", QSize(1200, 800)).toSize();
    if (winSize.isValid())
        resize(winSize);

    QByteArray geo = settings.value("windowGeometry").toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);

    QByteArray state = settings.value("windowState").toByteArray();
    if (!state.isEmpty())
        restoreState(state);
}

void MainWindow::saveSettings()
{
    QSettings settings("NotepadSafe", "NotepadSafe");
    settings.setValue("darkMode", m_darkMode);
    settings.setValue("recentFiles", m_recentFiles);
    settings.setValue("edgeVisible", m_edgeVisible);
    settings.setValue("edgeColumn", m_edgeColumn);
    settings.setValue("wordWrap", m_wordWrap);
    settings.setValue("windowSize", size());
    settings.setValue("windowGeometry", saveGeometry());
    settings.setValue("windowState", saveState());
}

// ============ 关闭 ============

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();

    // Step 1: 按 Notepad++ 规则检查关闭 (closingApp=true)
    // - 对"已存在文件且已修改"：弹窗提示用户 保存/丢弃/取消
    // - 对"未命名文件+有内容"：不提示用户，后续写入 autosave（下次启动恢复）
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!maybeSaveTab(i, /*closingApp=*/true)) {
            // 用户按了 Cancel，中止关闭
            event->ignore();
            return;
        }
    }

    // Step 2: 保存会话
    saveSessionNow();

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
        if (!tab->isUntitled() && !tab->isModified()) {
            tab->clearAutosaveFile();
        }
    }

    event->accept();
}