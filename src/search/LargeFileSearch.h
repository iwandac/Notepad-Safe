#pragma once

#include <QObject>
#include <QVector>
#include <QByteArray>
#include <QString>
#include <QPoint>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>

#include "DynamicThresholds.h"
#include "AsyncTask.h"
#include "TextEncoding.h"

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
    qint64   contextByteOffset = -1; // lineContext[0] 的全局字节偏移；-1=旧记录按 col 回退
    QByteArray lineContext;
};
Q_DECLARE_METATYPE(LargeFileSearchMatch)

// ============================================================================
//  MOC 友好信号：跨 DLL/静态库使用 QList<T> 需要 T 被 moc 认识（见 Qt6 新 moc）。
//  对外统一用 QVector，并显式注册元素类型，保证 worker → UI 的 queued
//  partialBatch 在静态 Qt 与不同编译单元之间都可可靠传递。
// ============================================================================

// ============================================================================
//  LargeFileSearch — 大文件全量搜索系统
//
//  字面值：16 MiB 顺序 KMP，状态跨块延续，不使用 overlap
//  正则：PCRE2 10.47 8-bit 在完整 mmap subject 上以 offset window 推进；
//        patch 内容先流式形成临时 mmap 快照，因此跨块/跨行不会漏报
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
        bool regexMode     = false;
        bool hexBytesMode  = false; // "DE AD BE EF" -> exact raw-byte KMP
        bool byteContextMode = false; // 16-byte ASCII preview context for Hex targets
        // Text presentation may intentionally decode malformed source bytes
        // as U+FFFD (for example Binary -> Text). Use the raw decoding iterator
        // instead of rejecting the complete subject as invalid UTF-8.
        bool tolerateInvalidEncoding = false;
        bool caseSensitive = true;
        bool wholeWord     = false; // Unicode letter/number/underscore boundary
        bool forward       = true;  // 不影响 findAll（总是从头到尾），影响 findNext/findPrev
        bool useOverrunFile = true; // 超 MAX_MATCHES_MEM 是否溢出到磁盘
        // Physical encoding of a text target. Regex over GBK is executed by
        // the raw code-page backend and keeps mmap byte offsets authoritative.
        TextEncoding textEncoding = TextEncoding::Utf8;
    };

    static bool parseHexBytePattern(const QString &pattern, QByteArray *bytes,
                                    QString *error = nullptr);

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

    // F3/Shift+F3 use a directional scan and return the first authoritative
    // match without waiting for a complete Find All index.
    using OneMatchCallback = std::function<void(
        bool found, LargeFileSearchMatch match, QString error)>;
    qint64 findOneViaTaskManager(const QString &pattern, const Options &opts,
                                 qint64 fromByte, bool forward, bool wrap,
                                 const QString &fileDisplayName,
                                 QObject *callbackContext,
                                 OneMatchCallback callback);

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
    const QVector<QPair<qint64, qint64>>& overrunCheckpoints() const {
        return overrunCheckpoints_;
    }
    qint64 overrunGroupedCount() const { return overrunGroupedCount_; }

    // findNext/findPrev：从 fromByte 开始，在当前匹配集合里找下/上一条；未找到返回 -1
    // 注意：未执行 findAll 时返回 -1
    qint64 findNext(qint64 fromByte, LargeFileSearchMatch* outMatch) const;
    qint64 findPrev(qint64 fromByte, LargeFileSearchMatch* outMatch) const;

    // cancel 下一次（或当前） findAll
    void requestCancel();

    // Cancel queued/running search work and wait until no worker can access
    // this object or its borrowed mmap/LineIndex/PatchList source. Callers
    // must cross this barrier before unmapping or replacing the source.
    void cancelAndWait();

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
    // Frozen when the source snapshot is attached. If false, the worker never
    // reads LineIndex while its background builder may swap data on the UI thread.
    bool          lineIndexCompleteAtSnapshot_ = false;
    const PatchList*  patchList_ = nullptr;

    QVector<LargeFileSearchMatch> memMatches_;
    qint64               maxMemMatches_ = 0;  // 在构造函数中从 DynamicThresholds 读取（避免类静态初始化顺序触发 instance()）
    QString              overrunFile_;
    qint64               overrunCount_ = 0;
    QFile*               overrunFp_    = nullptr;
    bool                 storageFailed_ = false;
    QString              scanError_;
    QVector<QPair<qint64, qint64>> overrunCheckpoints_; // grouped row -> file offset
    QVector<qint64>        overrunCheckpointBytes_;
    qint64               overrunGroupedCount_ = 0;
    qint32               overrunLastLine_ = std::numeric_limits<qint32>::min();
    qint64               overrunLastContext_ = std::numeric_limits<qint64>::min();

    std::atomic<bool>    cancel_{false};
    qint64               lastTaskId_ = 0;  // AsyncTaskManager 派发后的 taskId（0 = 非 Manager 管理）

    // A reservation is acquired before a lambda containing `this` is queued,
    // not when it eventually starts. Destruction/source replacement can then
    // wait for both queued and running workers without a check/use race.
    std::mutex              workerMutex_;
    std::condition_variable workerIdle_;
    int                     workerReservations_ = 0;
    bool                    shuttingDown_ = false;

    // ---------- 内部 helper ----------
    bool prepareOverrunFile(QString* errMsg);
    void appendMatches(const QVector<LargeFileSearchMatch>& batch);  // 内存 + 溢出文件分流
    void closeOverrunFile();
    bool reserveWorker();
    void releaseWorker();
    void stopWorkers(bool permanent);

    bool scanAllBounded(qint64 virtualSize, const QString &pattern,
                        const Options &opts,
                        const std::function<bool()> &isCancelled,
                        const std::function<void(qint64)> &reportProgress,
                        const std::function<bool(const QVector<LargeFileSearchMatch>&)> &reportBatch);
    bool scanOneBounded(qint64 virtualSize, const QString &pattern,
                        const Options &opts, qint64 fromByte,
                        bool forward, bool wrap,
                        const std::function<bool()> &isCancelled,
                        const std::function<void(qint64)> &reportProgress,
                        LargeFileSearchMatch *match, QString *error) const;

    // 工具：虚拟字节偏移 [start, end) 还原成文本（考虑 patch）
    QByteArray readVirtualBytes(qint64 start, qint64 end) const;

    // 工具：给定虚拟偏移 → 填充 line/col + lineContext（512 字节以内行）
    void fillLineContext(LargeFileSearchMatch& m) const;
    void fillByteContext(LargeFileSearchMatch& m) const;

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
