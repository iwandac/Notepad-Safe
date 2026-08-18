#pragma once

#include <QtGlobal>
#include <QAbstractScrollArea>
#include <QTimer>
#include <QPainter>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QFuture>
#include <QFutureWatcher>
#include <QPointer>
#include <QElapsedTimer>
#include <QLocale>

#include <atomic>
#include <memory>

#include "LineIndex.h"
#include "PatchList.h"
#include "AsyncTask.h"
#include "TextEncoding.h"
#include "SharedReadOnlyFile.h"

class QEventLoop;
class QProgressDialog;

// ============================================================================
//  VirtualFileView — L2 虚拟视图（P1 基础视图）
//
//  职责（P1）：
//   · QFile::map() 打开大文件 mmap（Windows 下 FileMapping + MapViewOfFile）
//   · 后台线程构建 LineIndex（QProgressDialog + 取消原子）
//   · paintEvent 自绘「可见行 50~100 行」≥ 55fps（不画全文件）
//   · 垂直/水平滚动条同步 LineIndex 的总行数/最长行
//   · 光标 byte 位置（P1 简化为整行浏览级光标：鼠标单击某行 → 定位到行首）
//   · 书签集合（QSet<行号>），点击行号区切换，上/下一个书签跳转
//   · 对外提供「按 byte 范围读文本」→ 委托 PatchList::renderRange
//   · 对外提供 gotoLine / gotoByteOffset / saveAs（用 renderRange 分块写）
//   · 对外暴露 lineIndex() 只读引用 → LargeFileSearch 与 Dock 跳转用
//   · 文件关闭 / mmap 释放 / 保存后 remapFile
//
//  16 进制视图模式：
//   · 每行 16 字节：[ 地址列(8 位 16 进制) ][ 16×(2 位 16 进制 + 空格) ][ ASCII 列（16 字）]
//   · 大文件下不使用 LineIndex（按固定 16B/行分页），避免二进制文件 LineIndex 行数爆炸。
//   · 字节导航用 gotoByteOffset（仍然有效）；书签切换为 16 进制行的偏移。
//
//  不做的（P2/P3/P4 分别落地）：
//   - 光标逐字节导航 + 文本选择 + 键盘输入编辑（P3）
//   - 语法高亮词法器（P1 仅黑白纯文本，等宽字体）
//   - 大文件搜索 / 替换（P2/P4）
//   - 粘贴、删除、Ctrl+Z/Y（P3）
//
//  行高 & 字号：使用系统等宽字体（Courier New/Consolas），字号跟随全局 DPI
// ============================================================================

enum class ViewMode {
    Text,  // 文本模式（按 LineIndex 显示文本）
    Hex,   // 16 进制模式（按 16B/行显示原始字节）
};

class VirtualFileView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit VirtualFileView(QWidget* parent = nullptr);
    ~VirtualFileView() override;

    // ---- 打开 / 关闭 ----
    // filePath：本地文件路径（绝对）。parentWidget 仅用于 QProgressDialog 的父窗口，
    // 可以传 nullptr（用 this 当父）。
    // 返回 false 时 errMsg 有可读原因。
    bool openFile(const QString& filePath, QString* errMsg = nullptr,
                  QWidget* parentWidgetForProgress = nullptr);
    // Decrypted secure binary payloads must never be written to a temporary
    // file. They use the same byte/patch/Hex provider directly from a bounded
    // in-memory backing store.
    bool openMemory(QByteArray bytes, ViewMode mode = ViewMode::Hex,
                    QString* errMsg = nullptr);

    // 重新 map 同一个文件（保存成功后调用：旧 unmap → map → 重建 LineIndex → patch reset）
    bool remapFile(QString* errMsg = nullptr);

    // 同步打开（诊断/测试专用）：主线程同步完成 mmap + quickBuild + 全量 LineIndex 构建，
    // 返回时 isOpen() == true, lineIndex 完整。不依赖 AsyncTaskManager/QEventLoop。
    bool openFileSync(const QString& filePath, QString* errMsg = nullptr);

    // 关闭并释放 mmap
    //  allowProcessEvents=false：析构上下文必须传 false！此时对象半析构，
    //    processEvents 可能派发指向本对象的 QueuedConnection（如 onAsyncLineBuildSwapped）
    //    导致 use-after-free / 虚表访问崩溃 0xC0000005。改用固定时间保守等待（最坏 500ms
    //    对 LineIndex 100MB 级任务绰绰有余——每 16MB 就检查 cancel，最坏仅 16MB 扫描时间 10~30ms）。
    void closeFile(bool allowProcessEvents = true);

    bool isOpen() const { return m_memoryBacked || mmapBase_ != nullptr; }
    // Windowed S2/S3 presentation owns its own bounded projection. Its hidden
    // compatibility mmap exists only for search and must not compete by
    // building a second full-file line index.
    void setFullLineIndexBuildEnabled(bool enabled) {
        m_fullLineIndexBuildEnabled = enabled;
    }
    bool fullLineIndexBuildEnabled() const {
        return m_fullLineIndexBuildEnabled;
    }
    bool isMemoryBacked() const { return m_memoryBacked; }
    QString filePath() const { return file_.fileName(); }
    qint64 fileSize() const { return mmapSize_; }
    const uchar* mmapBase() const { return mmapBase_; }
    qint64 mmapSize() const { return mmapSize_; }

    const LineIndex& lineIndex() const { return lineIndex_; }

    // ---- 导航 ----
    // 0-based 行号跳转
    void gotoLine(int line, bool selectTo = false);
    // 字节偏移跳转
    void gotoByteOffset(qint64 byteOffset, bool selectTo = false);

    // ---- 书签 ----
    void toggleBookmarkAtCursor();
    int  prevBookmark(int fromLine = -1) const; // -1 表示无
    int  nextBookmark(int fromLine = -1) const;
    void clearBookmarks();

    // ---- 读 ----
    // 读 [startByte, endByte) 字节范围（已应用 patch），返回 UTF-8 字节数组
    QByteArray readByteRange(qint64 startByte, qint64 endByte) const;

    // ---- 保存 ----
    // 流式 applyAndSave：4MB/块 + progress + cancel + 原子提交（PatchList 新接口）
    bool saveAs(const QString& outPath,
                std::function<void(qint64,qint64)> progress = {},
                std::atomic<bool>* cancel = nullptr,
                QString* errMsg = nullptr);

    // ---- 对外搜索 API ----
    // 返回内部 LargeFileSearch 实例；VirtualFileView 负责 setSource 更新。
    class LargeFileSearch* searchEngine();

    // ---- 搜索关键词高亮 ----
    // 设置当前搜索匹配的高亮位置（字节偏移 + 长度），用于 paintEvent 染色。
    // 传空向量清除高亮。VirtualFileView 不持有所有权，只拷贝。
    void setSearchHighlights(const QVector<QPair<qint64, qint32>>& positions);
    void clearSearchHighlights();

    // ---- 对外 PatchList 只读/可写访问（LargeFileSearch 替换用）----
    const PatchList& patches() const { return patches_; }
    PatchList&       patches()       { return patches_; }

    // ---- 编辑（P3 补齐）----
    // 在光标位置插入字节（UTF-8）；按字节偏移操作不做 UTF-8 分割保护。
    bool insertAtCursor(const QByteArray& bytes, QString* errMsg = nullptr);
    // 删除光标左侧 nBytes 个字节（退格）
    bool backspaceBytes(int nBytes, QString* errMsg = nullptr);
    // 删除光标右侧 nBytes 个字节（Delete）
    bool deleteBytesAhead(int nBytes, QString* errMsg = nullptr);
    // 粘贴：字节串插入；超过 PASTE_LIMIT 时触发压力提示（这里只调用，UI 由父窗口处理信号）
    bool pasteBytes(const QByteArray& bytes, QString* errMsg = nullptr);
    // 替换 [byteOffset, byteOffset+oldLength) 为 newData（查找/替换、回退操作核心）
    bool replaceBytes(qint64 byteOffset, qint32 oldLength, const QByteArray& newData,
                      QString* errMsg = nullptr);
    // LargeFileSearch can mutate the shared PatchList in one batch. Route the
    // resulting dirty/undo/layout transition back through the same observable
    // contract as interactive edits.
    void notifyExternalPatchMutation();
    // 删除当前选区；若无选区则什么都不做。返回 true 表示执行了删除。
    bool removeSelection(QString* errMsg = nullptr);
    // 撤销 / 重做（委托 PatchList）
    bool undo(QString* errMsg = nullptr);
    bool redo(QString* errMsg = nullptr);
    bool canUndo() const { return patches_.canUndo(); }
    bool canRedo() const { return patches_.canRedo(); }

    // ---- 显示编码（Notepad++ "以编码显示"：不修改文件内容，仅改变解码方式）----
    void setDisplayEncoding(TextEncoding enc);
    TextEncoding displayEncoding() const { return m_displayEncoding; }

    // ---- 视图模式：文本 / 16 进制 ----
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_viewMode; }
    void setHexBytesPerRow(int bytesPerRow);
    int hexBytesPerRow() const { return m_hexBytesPerRow; }

    // ---- 自动换行（L2 大文件）----
    void setWordWrap(bool on);
    bool wordWrap() const { return m_wordWrap; }

    // ---- 选择 & 剪贴板 ----
    bool hasSelection() const { return selStartByte_ != selEndByte_; }
    qint64 selectionStart() const { return std::min(selStartByte_, selEndByte_); }
    qint64 selectionEnd()   const { return std::max(selStartByte_, selEndByte_); }
    // 设置字节选区 [start,end)（start 可以大于 end）
    void selectBytes(qint64 start, qint64 end) {
        selStartByte_ = start; selEndByte_ = end; selAnchorByte_ = start; cursorByte_ = end;
        viewport()->update();
    }
    QByteArray selectedBytes() const;
    void selectAll();
    void clearSelection() { selStartByte_ = selEndByte_ = cursorByte_; selAnchorByte_ = cursorByte_; viewport()->update(); }

    // ---- 便捷只读状态访问器 ----
    qint64 virtualFileSize() const { return isOpen() ? patches_.virtualSize(mmapSize_) : 0; }
    // Zero disables the cap. Secure binary documents set the product's
    // exclusive payload limit so an edit can never create an unsavable state.
    void setVirtualSizeLimitExclusive(qint64 limit) {
        m_virtualSizeLimitExclusive = qMax<qint64>(0, limit);
    }
    qint64 totalLines()      const { return m_viewMode == ViewMode::Hex ? hexTotalLines() : lineIndex_.totalLines(); }
    bool   isModified()      const { return patches_.isModified(); }
    // 光标位置访问器（GUI smoke 调试 & 断言用）
    qint64 cursorByte() const { return cursorByte_; }
    int    cursorLine() const;
    qint64 cursorColByte() const;
    // 复制选中到剪贴板（UTF-8 文本）；无选中则 no-op
    void copy() const;
    // 复制选中 + 删除选中字节；返回 false 仅在删除失败
    bool cut(QString* errMsg = nullptr);
    // 从剪贴板读文本（UTF-8）→ 插入光标位置；委托 pasteBytes
    bool paste(QString* errMsg = nullptr);

    // ========================================================================
    //  小 Chunk + 预读 prev/next + Scintilla 小窗口编辑（按方案落地）
    // ========================================================================
    // 计算「Scintilla 小窗口字节容量」W_active = min(W_display, W_throughput)
    //   - W_display  = 11 screens × bytes/screen（参考 ZST 3-page sliding window）
    //   - W_throughput = 采样吞吐 × 10ms 重灌预算，clamp [256KB, 8MB]（VSCode LFV 5MB 参考）
    qint64 computeActiveWindowBytes() const;

    // 对外：当前 Scintilla 需要显示的字节窗口 [byteStart, byteEnd)。
    // EditorTab 在构造 overlay Scintilla 时，需要用这个范围喂 setData。
    qint64 activeWindowByteStart() const { return m_activeWindowStart; }
    qint64 activeWindowByteEnd()   const { return m_activeWindowStart + m_activeWindowLen; }

    // 硬件吞吐采样：openFile 完成 onMmapReady 后跑一次 64MB 预读得到 throughput
    // 注：通过 AsyncTaskManager 的结果回调会用 QMetaObject::invokeMethod 从 worker 线程
    //     调进来，所以必须加 Q_INVOKABLE（否则 invokeMethod 报 No such method，fallback
    //     到 100MB/s 吞吐 → Chunk 估算保守但不影响显示）。
    Q_INVOKABLE void setSampledThroughputBytesPerSec(qint64 bps) { m_sampledThroughputBps = qMax<qint64>(0, bps); }
    qint64 sampledThroughputBytesPerSec() const { return m_sampledThroughputBps; }

    // 把「当前可视窗口中心字节」同步到 W_active 对齐的新 activeWindow（±1 chunk 预读）
    // EditorTab 在 overlay Scintilla 滚动后需要同步两边位置时可以显式调用。
    void syncActiveWindowToCursor(qint64 centerByte);

signals:
    // Active window 发生了切换（由于 debounce/落点跳转/预读完成导致 start/len 变化）
    // 父窗口 EditorTab 应该连接到这里，以重新把 applyPatches(readByteRange(start, end)) 灌到 overlay Scintilla
    void activeWindowChanged(qint64 byteStart, qint64 byteEnd);

    void cursorPositionChanged(int line, int col);   // 0-based，行首列 0
    void fileOpenedChanged(bool open);
    void saveNeededChanged(bool needed);             // patch 列表非空即需要保存
    void undoRedoChanged();                          // undo/redo 栈变化
    void patchPressureWarn();                        // 达到 soft warn：状态栏非阻塞提示中间保存
    void viewModeChanged(ViewMode mode);             // 文本/16 进制模式切换
    // 后台构建 LineIndex 进度（bytesDone, bytesTotal）：QueuedConnection 连接到 UI 线程即可
    void lineBuildProgress(qint64 bytesDone, qint64 bytesTotal);
    // 后台 LineIndex 构建完成（已 swap 进 lineIndex_）
    void lineBuildFinished(bool ok);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private slots:
    // ---- 小 chunk 滚动 3 档节流槽（按 QAbstractSlider 官方三档：预览/防抖/落点）----
    // L1：sliderMoved 只做轻量预览（半透明 jump-to-byte 水印，不读 chunk/不灌 Scintilla）
    void onScrollSliderMovedPreviewL1(int value);
    // L2：120ms debounce 后 → cancel 上一轮未完成的 prefetch → 提交新的 ±1 chunk 预取
    void onScrollDebouncedL2();
    // L3：sliderReleased / valueChanged(tracking=false) → 精确跳转到目标中心
    void onScrollSliderReleasedL3();

    // Prefetch worker 完成（±1 chunk 读取进 LRU）→ 如果 activeWindow 需要切换则改并 emit activeWindowChanged
    void onPrefetchFinished();

private:
    // ---- 内部 ----
    void initScrollBars();
    void updateScrollBarsFromContent();
    // 把滚动条信号连接到 3 档节流槽（在 openFile/构造初始化里调用）
    void connectScrollBarThrottling();

    // 小 chunk LRU 缓冲槽（CSV ClearView 参考：50-page 硬顶，这里 12 chunk 够 ±1 预取 + 历史缓冲）
    static constexpr int kChunkSlotsHardCap = 12;
    struct ChunkSlot {
        qint64   chunkId    = -1;        // floor(byteOffset / chunkBytes)
        QByteArray bytes;                // 原始 mmapped bytes（未应用 patch）
        qint64   lastUseTick = 0;        // 最近使用 tick，LRU 淘汰用
    };

    // 当前 activeWindow 长度变化时，按新 C_chunk = activeWindowBytes 把 LRU 全清（chunk size 变了旧的没意义）
    qint64 currentChunkBytes() const { return m_lastComputedW > 0 ? m_lastComputedW : Q_INT64_C(2) * 1024 * 1024; }

    // 从 LRU 拿（命中则更新 lastUseTick，未命中返回 nullptr）
    ChunkSlot* lruFindChunk(qint64 chunkId);
    // 写 LRU（超过硬顶则淘汰 lastUseTick 最小的）
    void       lruPutChunk(qint64 chunkId, QByteArray bytes);
    // 清空 LRU（chunk size 变化 / 关闭文件）
    void       lruClear();

    // 提交 AsyncTask：预取 chunkIds（一般 3 个：prev/cur/next）；结果写进 LRU，完成后 onPrefetchFinished
    void submitPrefetchForChunks(const QVector<qint64>& chunkIds,
                                 bool cancelOngoing = true,
                                 bool refreshProjection = true);

    // 把 centerByte 对齐到 W_active 窗口边界（尽量让 centerByte 落在窗口中心 ± 25% 范围内，prev/next 覆盖）
    // 返回 true 如果 m_activeWindowStart/m_activeWindowLen 变了（需要触发 activeWindowChanged）
    bool alignActiveWindowTo(qint64 centerByte, qint64 wBytes);

    // In incomplete Text-index mode the global thumb is a byte-progress
    // control, so its target is the first byte to show rather than a viewport
    // centre. Keeping that distinction prevents a long logical line from
    // pulling the projection backwards after every Scintilla layout update.
    bool alignActiveWindowToTop(qint64 topByte, qint64 wBytes);

    // 把当前 vertical/horizontal scrollbar 的值 → 当前可视中心字节（Text/Hex 模式都能用）
    qint64 computeViewportCenterByteFromScrollbars() const;

    // 计算 W_display：≈ 11 screens × 每屏字节数（用当前真实 viewport 尺寸 + 当前字体度量，不猜）
    qint64 computeDisplayWindowBytes() const;

    // 计算视图的度量
    void updateMetrics();
    int  lineHeight() const;                // 像素
    int  charWidth() const;                 // 平均字符宽度（等宽即等于 '0' 宽度）
    int  gutterWidth() const;               // 行号区像素宽度（文本模式）；16 进制模式下地址列宽度
    int  firstVisibleLine() const;          // 由垂直滚动条 value 计算
    int  visibleLineCount() const;          // 由视口高度 / lineHeight 向上取整
    int  horizontalOffsetPx() const;        // 由水平滚动条 value 计算（像素）
    // hscroll 估算 → 字节偏移近似（中心字节计算用；无符号纯文本假设每字节 ≈ 1 列）
    qint64 hscrollBytesApprox() const {
        const int offPx = horizontalOffsetPx();
        const int cw = qMax(1, charWidth());
        return qMax<qint64>(0, qint64(offPx) / qint64(cw));
    }

    // Hex view helpers use the same adaptive bytes-per-row as the visible
    // Scintilla projection so scrollbar, caret and absolute offsets agree.
    qint64 hexTotalLines() const {
        const qint64 size = virtualFileSize();
        if (size <= 0) return 0;
        const qint64 row = qMax(1, m_hexBytesPerRow);
        return (size + row - 1) / row;
    }
    // 16 进制视图：第 line 行起始字节偏移
    qint64 hexLineToByteOffset(int line) const {
        return qBound(qint64(0), qint64(line) * qint64(m_hexBytesPerRow), virtualFileSize());
    }
    // 16 进制视图：字节偏移 → 0-based 行号
    int hexByteToLine(qint64 off) const {
        if (off <= 0) return 0;
        return int(qMin<qint64>(off / qint64(m_hexBytesPerRow), hexTotalLines() - 1));
    }

    // 绘制：行号区 + 内容区
    void drawGutterLine(QPainter& p, int viewY, int lineNo, bool isCurrent, bool hasBookmark);
    // 绘制一行内容；返回该行占用的显示行数（自动换行时可能>1）
    int  drawContentLine(QPainter& p, int viewX, int viewY, int lineNo);
    // 16 进制视图：绘制一行（地址 + 16 hex + 16 ascii）
    void drawHexLine(QPainter& p, int viewX, int viewY, int lineNo);

    // 将字节区间按 m_displayEncoding 解码为 QChar 数组（用于渲染）
    // 返回解码后的字符数；每个字符的源字节宽度写入 charWidths（可选）
    QVector<uint> decodeBytesForDisplay(const uchar* data, int len) const;

    // 光标行/列计算（编辑后用虚拟坐标，未编辑用原始坐标）
    // col 为 qint64：二进制长行单行列号可超 INT_MAX（P0 截断根因）
    void computeCursorLineCol(int* line, qint64* col) const;

    // LineIndex 后台构建（runBlockingBuildInBackground：用 QtConcurrent::run + 信号等待）
    // 失败填 errMsg、返回 false；cancel 设 true 也返回 false
    bool buildLineIndexWithProgress(QWidget* progressParent, QString* errMsg);

    // 给 QProgressDialog 的父窗口定位：优先 progressParent，其次 this
    QWidget* pickProgressParent(QWidget* progressParent);

    // 从 (viewX, viewY) 像素点 → (line, virtCol)：
    //   line: 0-based 行号，virtCol: 虚拟坐标下的行内字节偏移（qint64，长行不溢出）
    // 全程虚拟坐标，编辑后点击定位正确（P2 行末换行修复）
    void viewportPointToCursorPos(const QPoint& pt, int* line, qint64* virtCol) const;

    // ---- 后台 LineIndex 构建（秒开机制）----
    void startAsyncLineBuild();
    // worker 线程函数：从 data[0..size] 构建 LineIndex，每 16MB 通过 reportProgress 向 UI 发进度；
    // 完成后通过 QueuedConnection 发 onAsyncLineBuildSwapped(result, ok)。
    static LineIndex buildLineIndexOnWorker(const uchar* data, qint64 size,
                                            AsyncCancelPtr cancel,
                                            std::function<void(qint64,qint64)> reportProgress);
    // 后台构建成功：swap 进 lineIndex_ 并刷新 UI
    Q_INVOKABLE void onAsyncLineBuildSwapped(LineIndex result, bool buildOk);
    // 旧信号转发：仍然对外 emit lineBuildFinished(ok)（保持接口不变）
    void onAsyncLineBuildFinished(); // 保留，仅作为转发函数（在 onAsyncLineBuildSwapped 末尾调用）

    // ---- 异步 mmap（首次打开不阻塞 UI 线程）----
    qint64 m_mmapTaskId = -1;  // AsyncTaskManager 的 mmap 任务 ID
    bool   m_loading = false;  // 正在异步 mmap 加载中
    Q_INVOKABLE void onMmapReady(quintptr ptr, qint64 size, LineIndex quickIndex);

    // ---- 成员 ----
    SharedReadOnlyFile file_;
    QByteArray m_memoryBacking;
    bool       m_memoryBacked = false;
    uchar*     mmapBase_ = nullptr;
    qint64     mmapSize_ = 0;
    bool       m_fullLineIndexBuildEnabled = true;

    LineIndex  lineIndex_;
    PatchList  patches_;

    // 后台全量构建状态
    qint64                                   m_lineBuildTaskId = -1;  // AsyncTaskManager::submit 返回的 id
    AsyncCancelPtr                           lineBuildCancel_;        // 跨线程取消原子
    // 已废弃：QFuture / QFutureWatcher 不再使用（统一走 AsyncTaskManager 调度）
    // 保留字段避免修改过多代码：置空即可
    QFuture<LineIndex>                       lineBuildFuture_;
    QFutureWatcher<LineIndex>*               lineBuildWatcher_ = nullptr;

    // 字体 & 度量
    QFont          font_;
    QFontMetrics   fm_;
    int            lineHeight_   = 0;
    int            charWidth_    = 0;
    int            gutterWidth_  = 0;
    int            maxLineChars_ = 0;   // LineIndex 估算的最长行（字节数）
    int            m_hexBytesPerRow = 16;
    int            maxLineDigits_ = 1;  // 行号需要的数字位数（总行数的 log10）

    // 光标 & 选择
    qint64         cursorByte_   = 0;   // 字节偏移
    qint64         selAnchorByte_= 0;   // 选择锚（P1 暂未用；后续实现）

    // 书签
    QSet<int>      bookmarks_;

    // 选择：[selStartByte_, selEndByte_] 左闭右开区间；若相等表示无选择。
    // 鼠标拖动会更新 selEndByte_，同时保持 selStartByte_。
    qint64         selStartByte_ = 0;
    qint64         selEndByte_   = 0;
    qint64         m_virtualSizeLimitExclusive = 0;

    // 搜索引擎（懒创建，首次调用 searchEngine 时 new，closeFile 不删除）
    mutable class LargeFileSearch* search_ = nullptr;

    // 搜索关键词高亮位置（字节偏移, 长度），paintEvent 时染色
    QVector<QPair<qint64, qint32>> searchHighlights_;

    // 显示编码（Notepad++ "以编码显示"：不修改文件，仅改变解码方式）
    TextEncoding m_displayEncoding = TextEncoding::Utf8;
    // 视图模式：文本 / 16 进制
    ViewMode     m_viewMode = ViewMode::Text;
    // 自动换行
    bool m_wordWrap = false;

    // 后台 LineIndex 构建进度（字节），用于 paintEvent 显示"索引构建中"提示
    qint64 m_buildProgressBytes = 0;

    // 最近一次保存时的 l2Count（用于判断是否需要发 patchPressureWarn）
    int            lastL2Count_ = 0;

    // 颜色（P1 固定浅色；P2 根据全局深色模式切换）
    struct Palette {
        QColor bg           = Qt::white;
        QColor gutterBg     = QColor(0xF0, 0xF0, 0xF0);
        QColor gutterText   = QColor(0x70, 0x70, 0x70);
        QColor text         = Qt::black;
        QColor currentLine  = QColor(0xFF, 0xF2, 0xCC);
        QColor bookmarkMark = QColor(0xD9, 0x53, 0x4F);
        QColor selBg        = QColor(0x33, 0x99, 0xFF, 0x50);
        QColor searchHighlightBg = QColor(0xFF, 0xFF, 0x00, 0x80);  // 搜索匹配背景色（半透明黄）
        QColor searchHighlightFg = QColor(0xFF, 0x00, 0x00);        // 搜索匹配前景色（红）
    } palette_;

    // ---- 小 chunk 滚动 / 预读 / Scintilla 小窗口 overlay 成员 ----
    QTimer*                     m_scrollDebounceTimer = nullptr;  // 120ms single-shot，L2 防抖
    qint64                      m_lastScrollValuePending = 0;    // debounce 缓存最新 slider 值
    qint64                      m_sampledThroughputBps = 0;      // 硬件吞吐：0 = 未采样，fallback 100MB/s
    qint64                      m_lastComputedW = 0;              // 上一次 computeActiveWindowBytes 的结果（用于 C_chunk 调整时清 LRU）

    qint64                      m_activeWindowStart = 0;          // 当前 Scintilla overlay 对应的字节起点
    qint64                      m_activeWindowLen   = 0;          // 当前窗口字节长度（= W_active）

    QVector<ChunkSlot>          m_chunkLru;                       // 小 chunk LRU 缓冲池（硬顶 kChunkSlotsHardCap）
    std::atomic<qint64>         m_lruTickCounter{0};             // LRU tick 原子，避免跨线程 data race

    // 预取任务：用 AsyncTaskManager + QFutureWatcher 实现 cancel（甩滚时取消无效的 chunk 读取）
    qint64                      m_prefetchTaskId = -1;
    QSharedPointer<std::atomic<bool>> m_prefetchCancel;                 // 跨线程取消原子（AsyncCancelPtr = QSharedPointer<atomic>）
    QVector<std::shared_ptr<std::atomic_bool>> m_prefetchWorkers;
    QFutureWatcher<QVector<ChunkSlot>>* m_prefetchWatcher = nullptr;  // 完成 / canceled 信号

    // L1 预览水印：sliderMoved 过程中画"跳转到 xxx bytes"的半透明提示
    qint64                      m_previewJumpByteHint = -1;       // -1 = 不画

    // 崩溃修复（0xC0000005）：closeFile 末尾的 UI 刷新 / 信号 emit 如果在
    //  ~VirtualFileView 中执行，会触发已经半析构的 EditorTab/父对象上的 slot
    //  调用（此时 EditorTab 子类析构已完成，vtable 已切换），导致 0xC0000005。
    //  ~VirtualFileView 开始时置 true，closeFile 末尾的所有「对外可见副作用」
    //  （UI repaint / signal emit / scrollbar 更新）都 NOP。内部数据清理
    //  （unmap / lineIndex clear / file close / patch reset）仍然执行。
    bool           m_inDestruction = false;
    bool           m_closingFile = false;
    quint64        m_mappingGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_lineBuildWorkerDone;
};
