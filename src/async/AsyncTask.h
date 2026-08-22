#ifndef ASYNCTASK_H
#define ASYNCTASK_H

#include <QString>
#include <QDateTime>
#include <QVector>
#include <QMetaType>
#include <atomic>
#include <memory>
#include <functional>

enum class AsyncTaskPriority : int {
    Highest = 0,  // 用户交互型：点击跳转后的紧急补扫等
    High    = 1,  // 用户主动发起的搜索等
    Normal  = 2,  // LineIndex 按需补扫（滚到哪扫到哪）
    Low     = 3,  // LineIndex 后台预扫、空闲打包
    Lowest  = 4   // 最低优先级：后台校验、统计、索引归档
};

enum class AsyncTaskState : int {
    Queued   = 0,
    Running  = 1,
    Finished = 2,
    Failed   = 3,
    Canceled = 4
};

enum class AsyncTaskType : int {
    Unknown = 0,
    LineIndexBuild = 1,   // L2 行索引构建（全量/增量/补扫）
    LargeFileSearch = 2,  // 大文件分块搜索
    CharacterCount = 3,
    EncodingConversion = 4,
    SessionSnapshot = 5,
    SecureOpen = 6,
    SecureSave = 7,
    SecureIdentity = 8,
    VaultOperation = 9,
    Other           = 99
};

struct AsyncErrorInfo {
    QString message;                 // 顶层错误描述（what / 业务语义）
    QString sourceFile;              // __FILE__ 或任务内部来源
    int     sourceLine = 0;          // __LINE__
    QVector<QString> stackFrames;    // Windows CaptureStackBackTrace 导出的符号列表（可空）

    bool isNull() const { return message.isEmpty(); }
    QString toString() const;
};

/**
 * @brief 跨线程取消标记：AsyncTaskManager 创建，随 worker 传递，
 *        worker 周期检查 cancel.load(std::memory_order_acquire)。
 */
using AsyncCancelPtr = QSharedPointer<std::atomic<bool>>;

struct AsyncTask {
    qint64  id = 0;                  // 全局唯一 id（Manager 自增）
    qint64  batchId = 0;             // 批次：同一菜单动作发出的一组任务共用，便于统计/批量取消
    AsyncTaskType type = AsyncTaskType::Unknown;
    AsyncTaskPriority priority = AsyncTaskPriority::Normal;
    AsyncTaskState state = AsyncTaskState::Queued;

    QString title;                   // 单行显示：如 "构建行索引：1.2GB.log"
    QString description;             // 详情：可换行，点击查看

    qint64  progressDone = 0;        // 进度：分子
    qint64  progressTotal = 0;       // 进度：分母；-1 表示未知（显示忙状态）
    int     percent() const {
        if (progressTotal <= 0 || progressDone < 0) return -1;
        if (progressDone >= progressTotal) return 100;
        return int((progressDone * 100LL) / progressTotal);
    }

    QDateTime createdAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    qint64 elapsedMs() const;        // 运行中/已完成 耗时
    // 基于已完成工作量的线性估计；数据不足或任务已结束时返回 -1。
    qint64 estimatedRemainingMs() const;

    AsyncCancelPtr cancel;           // 取消原子；Manager 分配 & 持有 cancel->store(true) 来取消
    bool cancellable = true;          // 原子提交/安全收尾阶段必须关闭取消
    AsyncErrorInfo error;            // 失败信息
    QString categoryTag;             // 可选：例如 "LineIndex" / "Crypto-Decrypt" / "Search"

    QString stateText() const;
    QString priorityText() const;
    QString typeText() const;
};

Q_DECLARE_METATYPE(AsyncTask)
Q_DECLARE_METATYPE(AsyncTaskState)
Q_DECLARE_METATYPE(AsyncTaskType)

#endif // ASYNCTASK_H
