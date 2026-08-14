#pragma once

#include <QObject>
#include <QVector>
#include <QByteArray>
#include <QString>
#include <QRegularExpression>
#include <QPoint>
#include <functional>
#include <atomic>

#include "DynamicThresholds.h"
#include "AsyncTask.h"

class QFile;
class LineIndex;
class PatchList;
class SearchResultsModel;

// ============================================================================
//  搜索匹配项（L2/LargeFileSearch 专用）。
//  注意：EditorTab.h（L1 Scintilla）使用同名的 ScintillaSearchMatch，
//  本头文件使用 LargeFileSearchMatch，避免全局命名空间 struct 重定义冲突。
// ============================================================================
struct LargeFileSearchMatch {
    qint64   byteOffset;
    qint32   length;
    qint32   line;
    qint32   col;
    QByteArray lineContext;
};

// ============================================================================
//  MOC 友好信号：跨 DLL/静态库使用 QList<T> 需要 T 被 moc 认识（见 Qt6 新 moc）。
//  但 QList<LargeFileSearchMatch> 不是 Qt 内建类型，也不需要 Q_DECLARE_METATYPE，
//  我们统一用 QVector（已经被内部 API 接受），避免 moc 的模板参数推导失败。
//  对外 partialBatch 的 QVector 也可以通过 Qt::QueuedConnection 传递（QVector 自动注册）。
// ============================================================================

// ============================================================================
//  LargeFileSearch — 大文件全量搜索系统
//
//  分块：默认 16MB，4KB overlap（避免跨块匹配漏掉）
//  线程池：QThreadPool + QtConcurrent（按 DynamicThresholds::SEARCH_THREADS）
//  批次：每 1000 条匹配 → 发一个 partialBatch 信号（UI 可增量显示）
//  溢出：当内存结果 > MAX_MATCHES_MEM → 追加写入 matchesOverrun.bin
//  取消：通过 *cancel 原子标志
//  findNext/findPrev：基于当前光标位置找下一个/上一个单条
//  replaceAll：DESIGN.md §5.6 1 UndoGroup = 1000 替换批 + 中间保存压力提示
// ============================================================================

class LargeFileSearch : public QObject {
    Q_OBJECT
public:
    explicit LargeFileSearch(QObject* parent = nullptr);
    ~LargeFileSearch() override;

    struct Options {
        QRegularExpression regex;   // 非空：使用正则模式；空：字面值大小写敏感精确匹配
        bool caseSensitive = true;
        bool wholeWord     = false; // 字面值模式下生效
        bool forward       = true;  // 不影响 findAll（总是从头到尾），影响 findNext/findPrev
        bool useOverrunFile = true; // 超 MAX_MATCHES_MEM 是否溢出到磁盘
    };

    // 设置数据源：mmap + LineIndex + PatchList（全部由 VirtualFileView 提供）
    void setSource(const uchar* mmapBase, qint64 mmapSize,
                   const LineIndex* lineIndex,
                   const PatchList* patchList);

    // findAll：异步启动，通过信号 finished/failed/progress/partialBatch 与调用者交互
    void findAll(const QString& pattern, const Options& opts);

    // findAllViaTaskManager：走 AsyncTaskManager 统一派发，支持 AsyncTaskPanel 取消/进度
    //  - 返回 AsyncTaskManager 分配的 taskId（>0 表示已入队/运行；<=0 表示启动失败走 findAll 回退）
    //  - 任务结束仍然触发 finished/failed/progress 信号（兼容旧接入）
    qint64 findAllViaTaskManager(const QString& pattern, const Options& opts,
                                 const QString& fileDisplayName = QString());

    // 当前 taskId（findAllViaTaskManager 启动后有效；findAll 直接启动则为 0）
    qint64 lastTaskId() const { return lastTaskId_; }

    // 结果总数（内存 + 溢出文件）——带 acquire 屏障，确保 worker 写入主线程可见
    qint64 totalMatches() const {
        std::atomic_thread_fence(std::memory_order_acquire);
        return qint64(memMatches_.size()) + overrunCount_;
    }

    // 访问内存结果（仅前 MAX_MATCHES_MEM 条）
    const QVector<LargeFileSearchMatch>& memoryMatches() const { return memMatches_; }

    // 访问溢出文件路径（为空 = 无溢出）；文件格式见实现顶部注释
    const QString& overrunFilePath() const { return overrunFile_; }
    qint64 overrunCount() const { return overrunCount_; }

    // findNext/findPrev：从 fromByte 开始，在当前匹配集合里找下/上一条；未找到返回 -1
    // 注意：未执行 findAll 时返回 -1
    qint64 findNext(qint64 fromByte, LargeFileSearchMatch* outMatch) const;
    qint64 findPrev(qint64 fromByte, LargeFileSearchMatch* outMatch) const;

    // cancel 下一次（或当前） findAll
    void requestCancel();

    // replaceAll 单线程：按 DESIGN.md §5.6 步骤 3 分批执行；
    // 每批调用 progressBatch / 需要中间保存时调用 onPressureSoftWarn。
    // 返回 true 全部完成；false 中途取消/失败/中间保存中断。
    struct ReplaceCallbacks {
        std::function<void(qint64 done, qint64 total)> progressBatch;
        std::function<bool(const QString& hintMsg)> onPressureSoftWarn;  // 返回 true=立即保存并清空patch后继续；false=直接执行
        std::function<QString()> needSaveAsPath;  // 返回用户选择的中间保存路径
    };
    bool replaceAll(const QString& pattern, const QString& replacement,
                    const Options& opts, PatchList& patches,
                    ReplaceCallbacks callbacks, QString* errMsg,
                    qint64* outReplacedCount = nullptr);

signals:
    void progress(qint64 bytesProcessed, qint64 totalBytes);
    void partialBatch(const QVector<LargeFileSearchMatch>& batch);
    void finished(qint64 totalMatches);
    void failed(const QString& message);

private:
    const uchar*   mmapBase_ = nullptr;
    qint64         mmapSize_ = 0;
    const LineIndex*  lineIndex_ = nullptr;
    const PatchList*  patchList_ = nullptr;

    QVector<LargeFileSearchMatch> memMatches_;
    qint64               maxMemMatches_ = 0;  // 在构造函数中从 DynamicThresholds 读取（避免类静态初始化顺序触发 instance()）
    QString              overrunFile_;
    qint64               overrunCount_ = 0;
    QFile*               overrunFp_    = nullptr;

    std::atomic<bool>    cancel_{false};
    qint64               lastTaskId_ = 0;  // AsyncTaskManager 派发后的 taskId（0 = 非 Manager 管理）

    // ---------- 内部 helper ----------
    bool prepareOverrunFile(QString* errMsg);
    void appendMatches(const QVector<LargeFileSearchMatch>& batch);  // 内存 + 溢出文件分流
    void closeOverrunFile();

    // 在 [rangeStart, rangeEnd) 字节区间做一次扫描；pattern/literal 依 Options 决定
    // 返回本块内的匹配集合
    QVector<LargeFileSearchMatch> scanChunk(qint64 chunkStart, qint64 chunkEnd,
                                   const Options& opts,
                                   const QRegularExpression* reOrNull,
                                   const QByteArray& literalBytesOrEmpty,
                                   qint64 overlapBack /*前一块 overlap 的额外尾部字节数，用于去重*/) const;

    // 工具：虚拟字节偏移 [start, end) 还原成文本（考虑 patch）
    QByteArray readVirtualBytes(qint64 start, qint64 end) const;

    // 工具：给定虚拟偏移 → 填充 line/col + lineContext（512 字节以内行）
    void fillLineContext(LargeFileSearchMatch& m) const;

    // 字面值模式匹配（KMP 一次扫描，O(n+m) 最坏）
    static QVector<QPair<qint64, int>> kmpMatches(const uchar* text, qint64 n,
                                                  const QByteArray& needle,
                                                  bool caseSensitive);
    // 构造 KMP failure 函数
    static QVector<int> kmpBuildFail(const QByteArray& needle, bool caseSensitive);
};

// ============================================================================
//  溢出文件格式（matchesOverrun.bin）
//   MAGIC  (4)   = 'MSOV'
//   VERSION(4)   = 1
//   COUNT  (8)   = overrun total count
//   for each SearchMatch:
//     byteOffset (8)
//     length     (4)
//     line       (4)
//     col        (4)
//     ctxLen     (4)   → lineContext bytes
//     lineContext bytes
// ============================================================================
