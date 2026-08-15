#pragma once

#include <QWidget>
#include <QTimer>
#include <QByteArray>
#include <Qsci/qsciscintilla.h>
#include "TextEncoding.h"
#include "LargeFileSearch.h"   // 先入 LargeFileSearchMatch 定义

enum class ViewMode : int;  // 前置声明（定义在 VirtualFileView.h）

class QsciLexer;
class VirtualFileView;
class QStackedWidget;

// 语法类型
enum class Syntax {
    Plain,
    Cpp,
    Python,
    Json,
    Html,
    Xml,
    Sql,
    Yaml,
    Markdown,
};

// ============================================================================
//  L1（QsciScintilla 编辑器）的搜索匹配结果
//  注意：LargeFileSearch.h 已在全局命名空间定义 struct LargeFileSearchMatch（并别名
//  SearchMatch），此处故意用 ScintillaSearchMatch，避免 ODR 重定义；
//  ScintillaEditorMatch 向后兼容旧代码。
// ============================================================================
struct ScintillaSearchMatch {
    int line;       // 0-based 行号
    int index;      // 0-based 列号
    int length;     // 匹配长度
    QString text;   // 匹配文本
    QString context; // 所在行内容
};
using SearchMatch = ScintillaSearchMatch;

class EditorTab : public QWidget {
    Q_OBJECT
public:
    explicit EditorTab(QWidget *parent = nullptr);
    ~EditorTab() override;

    /** 关闭/退出前主动清理 L2 的异步构建任务 + mmap（MainWindow::closeEvent 开头调用）。
     *  此时 MainWindow 还完整，允许 processEvents，轮询到 AsyncTask 终态才返回，
     *  彻底避免「析构阶段还有 worker 访问 mmap」导致的 0xC0000005 访问违例。
     *  L1 模式：什么都不做，直接返回。 */
    void shutdownL2BeforeClose();

    // ---- L1 路径（QsciScintilla，现有接口完全不变，避免功能回退）----
    // 文件操作
    bool loadFile(const QString &path);
    bool loadAutosave(const QString &autosavePath);  // 仅加载内容，不设置 m_filePath（对用户透明）
    bool saveFile(QString *errMsg = nullptr);
    bool saveAsFile(const QString &path, QString *errMsg = nullptr);
    bool isModified() const;
    void setModified(bool m);
    bool isUntitled() const;
    QString filePath() const;
    void setFilePath(const QString &path);
    QString fileName() const;
    QString tabTitle() const;
    TextEncoding encoding() const;
    void setEncoding(TextEncoding enc);
    Syntax syntax() const;

    // 编辑器访问（L2 模式下仍返回有效 QsciScintilla 指针，但内容为空；
    // 优先使用下方统一分流的 selectAll/copy/paste/... 等接口，避免直接调 editor 方法）
    QsciScintilla *editor() const { return m_editor; }

    // 语法
    void setSyntax(Syntax syntax);
    static Syntax guessSyntaxFromExtension(const QString &fileName);

    // 行操作
    void duplicateLine();
    void deleteLine();
    void moveLineUp();
    void moveLineDown();
    void sortLinesAsc();
    void sortLinesDesc();

    // 书签
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    void clearBookmarks();

    // 搜索（供 FindReplaceDialog 调用；内部按 isL2Mode 自动分流 L1/L2）
    // findNext 返回是否找到，并跳转光标
    bool findNext(const QString &pattern, bool regex, bool caseSensitive,
                  bool wholeWord, bool forward, bool wrap);
    // replace 当前匹配（需先 findNext）
    void replaceCurrent(const QString &replaceText);
    // replaceAll 返回替换次数（L2：按 1000 条/UndoGroup 原子批）
    int replaceAll(const QString &find, const QString &replace, bool regex,
                   bool caseSensitive, bool wholeWord);
    // findAll 收集所有匹配；L1 走 QVector、L2 走 SearchResultsModel
    QVector<SearchMatch> findAll(const QString &pattern, bool regex,
                                 bool caseSensitive, bool wholeWord);
    // L2 专用 findAll（同步阻塞版本，内部对 LineIndex 构建做等待 + 搜索等待）
    // 仅用于 L1 层的 findNext/findPrev 内部（需要先同步拿到结果集）及旧调用方
    // 返回 SearchResultsModel 所有权（调用方 delete 或 setParent）
    class SearchResultsModel* findAllL2(const QString& pattern, bool regex,
                                        bool caseSensitive, bool wholeWord,
                                        QObject* parentForModel = nullptr);
    // L2 专用 findAll（纯异步，不阻塞 UI）：
    //   - LineIndex 未构建完：等 l2LineBuildFinished 信号后再启动搜索
    //   - 搜索走 AsyncTaskManager::submit（LargeFileSearch）
    //   - 完成后触发 findAllL2Finished(SearchResultsModel*, pattern, fileName)
    //   - 期间触发 findAllL2Progress(int percent, qint64 done, qint64 total)（-1 = 正在等 LineIndex）
    //  返回 true = 成功启动；false = L1 模式或参数空未启动
    bool findAllL2Async(const QString& pattern, bool regex,
                        bool caseSensitive, bool wholeWord,
                        QObject* parentForModel = nullptr);

    // 光标位置
    void getCursorPosition(int *line, int *index) const;
    void setCursorPosition(int line, int index);

    // 长行标尺
    void setEdgeColumn(int column);
    void setEdgeVisible(bool visible);
    bool isEdgeVisible() const;

    // 自动换行
    void setWordWrap(bool on);
    bool wordWrap() const;

    // 编码重新加载（保持内容不变，仅改变编码解释方式）
    void reloadWithEncoding(TextEncoding enc);

    // 以指定编码显示（Notepad++ 风格：不修改文件内容，仅改变显示解码方式）
    // L1 模式：重新以指定编码解码并加载到 QsciScintilla
    // L2 模式：设置 VirtualFileView 的 displayEncoding，重绘
    void setDisplayEncoding(TextEncoding enc);
    TextEncoding displayEncoding() const;
    void setDisplayEncodingL2(TextEncoding enc);
    TextEncoding displayEncodingL2() const;

    // L2 视图模式：文本 / 16 进制（仅 L2 生效）
    void setViewModeL2(ViewMode mode);
    ViewMode viewModeL2() const;
    // 当前 tab 是否正在显示 L2 VirtualFileView（大文件视图），非 L1/QScintilla
    bool isL2Active() const;

    // 内部 autosave 路径（用于会话恢复时对用户透明）
    QString autosavePath() const { return m_autosavePath; }
    void setAutosavePath(const QString &path) { m_autosavePath = path; }
    void clearAutosaveFile() const;

    // ---- 统一分流的剪贴板/编辑操作（L1/L2 都正确，推荐外部统一走这些，不要调 editor()->xxx()）----
    void selectAll();
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();

    // 焦点设置（跳转后对用户可见）
    void setFocusEditor();

    // L2 跳转（byteOffset = 虚拟字节偏移，selectLength>0 表示选中 [byteOffset, byteOffset+selectLength)）
    // L1 模式下自动换算成行/列再跳转。
    void gotoByteOffset(qint64 byteOffset, int selectLength = 0);

    // ---- L2 路径（VirtualFileView 大文件虚拟视图，独立接口，不破坏 L1）----
    // 当前是否处于 L2 模式
    bool isL2Mode() const { return m_l2Mode; }
    // 获取 L2 视图（未 openLargeFileL2 时返回 nullptr）
    VirtualFileView* l2View() const { return m_l2View; }
    // 显式打开大文件为 L2 模式（创建 VirtualFileView 实例 + openFile）。
    // 成功后 isL2Mode()=true；失败时回退到 L1 模式并返回 false，此时可走 loadFile 原路径。
    // 注意：loadFile() 不变（永远走 L1），只有显式调用本函数才走 L2。
    // errMsg：失败原因（可读），传 nullptr 忽略。
    bool openLargeFileL2(const QString &path, QString *errMsg = nullptr);
    // 保存 L2 视图到指定路径（用 applyAndSaveThenReset 流程）
    bool saveL2As(const QString &path, QString *errMsg = nullptr);

    // ==== 会话恢复/保存专用：零副作用 + L1/L2 自动分流 ====
    // · exportToFile：将当前内容（L1 文档 或 L2 mmap+patch）完整写入 destPath。
    //   不改变 modified 状态、不改变 m_filePath/m_autosavePath、不清空 L2 patches、
    //   不 remap。用于 SessionManager::saveSession 周期性 autosave。
    bool exportToFile(const QString &destPath) const;
    // · openAutosaveAutoMode：恢复 autosave 时按文件尺寸自动选 L1/L2：
    //   S1（< 32 MiB）→ 走原 loadAutosave (Scintilla sync，毫秒级)
    //   尺寸 ≥ 限制 → mmap autosave 文件到 L2 VirtualFileView（秒开），
    //                 然后对用户伪装为 realFilePath（标题栏/保存路径）并标记 modified。
    //   返回 true = 成功加载（L1 或 L2 均可）
    bool openAutosaveAutoMode(const QString &autosavePath,
                              const QString &realFilePath = QString());

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

signals:
    void modificationChanged(bool modified);
    void cursorPositionChanged(int line, int index);
    void encodingChanged(TextEncoding encoding);
    void syntaxChanged(Syntax syntax);
    void titleChanged(const QString &title);

    // 当前 EditorTab 内部 L2<->L1 模式切换（从 L1 切换到 L2 或反向）——MainWindow 用它同步"不适配 L2 的菜单项 enabled"
    // 触发点：openFile/L1 打开小文件(222行m_l2Mode=false)/openAutosaveAutoMode/... 等所有设置 m_l2Mode 的位置 emit
    // 极简设计：不带参数，由接收方调用 tab->isL2Active()/isL2Mode() 自行判断（减少参数不一致 bug）
    void l2ModeStatusChanged();

    // L2 专用信号：转发 VirtualFileView 的状态（MainWindow/状态栏同步）
    void l2CursorPositionChanged(int line, int col);
    void l2FileOpenedChanged(bool open);
    void l2SaveNeededChanged(bool needed);
    void l2UndoRedoChanged();
    // 转发 VirtualFileView 后台行索引构建进度（秒开 UI 状态栏）
    void l2LineBuildProgress(qint64 bytesDone, qint64 bytesTotal);
    void l2LineBuildFinished(bool ok);

    // L2 findAllL2Async 结果 / 进度信号（纯异步，不阻塞）
    void findAllL2Progress(int percent, qint64 done, qint64 total);
    void findAllL2Finished(class SearchResultsModel* model,
                           const QString& pattern,
                           const QString& fileName);

private:
    QsciScintilla *m_editor;
    QStackedWidget *m_stack = nullptr;  // 0: m_editor (L1), 1: m_l2View (L2)
    QString m_filePath;       // 真实文件路径（空=未命名）
    QString m_autosavePath;   // 内部 autosave 临时路径（对用户透明）
    TextEncoding m_encoding = TextEncoding::Utf8;
    Syntax m_syntax = Syntax::Plain;
    int m_bookmarkMarker = -1;
    bool m_edgeVisible = true;
    int m_edgeColumn = 80;

    void setupEditor();
    void applySyntax(Syntax syntax);
    QsciLexer *createLexer(Syntax syntax) const;
    void clearMarkers();
    void applyEdgeSettings();

    // 大文件分块加载：通过 SCI_APPENDTEXT 分块追加 UTF-8 字节，
    // 避免 setText() 一次性重建整个文档导致 UI 卡死
    void loadUtf8Chunked(const QByteArray &utf8);

    // ---- L2 私有（懒创建 VirtualFileView，避免影响 L1 默认路径）----
    bool               m_l2Mode = false;
    VirtualFileView   *m_l2View = nullptr;  // 懒创建，仅 openLargeFileL2 成功后非空
    bool               m_l2SaveNeeded = false;  // 对应 L2 saveNeededChanged 信号
    int                m_lastL2Line = 0;         // L2 光标缓存（getCursorPosition 用）
    int                m_lastL2Col  = 0;
    qint64             m_lastL2Byte = 0;         // L2 光标字节缓存（findNext 起始位置）

    // findAllL2Async 的内部上下文：避免同时启动多个搜索
    struct PendingL2Search {
        bool active = false;
        QString pattern;
        bool regex = false;
        bool caseSensitive = true;
        bool wholeWord = false;
        QPointer<QObject> parentForModel;
        QMetaObject::Connection waitLineBuildConn;
        QMetaObject::Connection searchProgressConn;
        QMetaObject::Connection searchFinishedConn;
        QMetaObject::Connection searchFailedConn;
    };
    PendingL2Search m_pendingL2Search;

    // 确保 VirtualFileView 存在（懒创建）；同时把 m_l2View 加入 m_stack。
    // 不依赖固定索引：Scintilla 在 L2 期间会被 reparent，Qt 会重排 stack 页面。
    VirtualFileView *ensureL2View();
    // findAllL2Async 的内部启动：LineIndex 已完整 → 实际调 LargeFileSearch::findAllViaTaskManager
    void startPendingL2Search();
    // 清理 m_pendingL2Search 所有连接并重置为 inactive
    void resetPendingL2Search();

    // ============================================================================
    //  L2ScintillaBridge —— VFV 不做任何绘制；Scintilla overlay 盖满 VFV viewport，
    //  是唯一 UI（唯一渲染/输入/IME/行号/高亮/自动换行/拖选）。
    //  这里只做：
    //    (1) 桥 1 下拉：activeWindowChanged(start,end) → 读窗口字节 → 灌 Scintilla；
    //    (2) 桥 2 上推 flush：切窗口/保存前把 Scintilla 当前文本 diff 回 VFV patches；
    //    (3) 桥 3 双向滚动同步 debounce：SCN_UPDATEUI → VFV scrollbars / syncActiveWindowToCursor。
    // ============================================================================
    struct L2ScintillaBridge {
        QByteArray baseline;        // 上次 setText 的窗口 bytes（用于桥 2 diff）
        qint64    windowStart = -1; // 对应窗口的全局起始字节（-1 无）
        qint64    windowLen   = 0;  // 窗口长度（= W_active）
        QTimer    syncVfvDebounce;  // 桥 3 Scintilla→VFV 防抖（250ms single-shot）
        bool      applying = false; // 桥 1/3 灌 Scintilla 时抑制 SCN_UPDATEUI 递归
    };
    L2ScintillaBridge m_bridge;

    // ---- L2ScintillaBridge 内部辅助 ----
    // 桥 1：读 VFV [s,e)（应用 patch）→ Scintilla setText；记录 baseline
    void bridgePopulateScintilla(qint64 s, qint64 e);
    // 桥 2：把 Scintilla 当前文本 vs baseline 做 diff → 写 VFV patches()。s/e 同 baseline 窗口。
    void bridgeFlushOverlayToPatches(QString* errMsg = nullptr);
    // 桥 3：syncVfvDebounce 超时回调 → 取 Scintilla 中心 → 换算全局字节 → syncActiveWindowToCursor
    void bridgeOnSyncDebounce();
    // 切回 L1 / closeEvent 时，把 overlay Scintilla 从 VFV viewport 上拆回 m_stack
    void bridgeReparentEditorToStack();
};
