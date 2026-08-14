#ifndef CRYPTOTASK_H
#define CRYPTOTASK_H

#include <QObject>
#include <QString>
#include <memory>
#include "AsyncTask.h"

/**
 * @brief 加密 / 解密流水线（占位抽象类）—— 为后续大文件加密功能落地预留 QTaskTree recipe 接口。
 *
 * 设计目标：
 *   - 解密（打开加密文件）优先级：AsyncTaskPriority::Highest，保证用户点击打开后立即抢占执行，不会被后台加密任务占满（抗饥饿）。
 *   - 加密（另存为加密）优先级：AsyncTaskPriority::High，用户主动点保存，比 LineIndex 预扫更重要。
 *   - 流水线（QtTaskTree::Group Recipe 描述，真实实现时 include <QtTaskTree/qtasktree.h>）：
 *       解密：[并行解密分块块1‖块2‖…] -> Barrier -> [写入临时明文 mmap 文件] -> [构建 LineIndex（增量）] -> [SHA256 校验]
 *       加密：[流式读取 + PatchList 合并] -> [分块压缩/AES-GCM] -> [写加密头+块] -> [SHA256 校验]
 *
 * 本类仅声明接口，加密算法细节（算法、密钥派生、块格式）在后续实现时填。现在把 Async 调度层和 QTaskTree 的桥接接口先定义好。
 *
 * Recipe 类型选型：
 *   为了不把 QtTaskTree 头文件强制引入到所有编译单元（降低编译依赖），这里用 `std::shared_ptr<void>` 做类型擦除占位。
 *   真实实现时：
 *       #include <QtTaskTree/qtasktree.h>
 *       return std::make_shared<Group>(std::move(your_group_recipe));
 *   消费方（AsyncTaskManager）：
 *       auto groupPtr = std::static_pointer_cast<Group>(recipeHolder);
 *       QTaskTree tree(*groupPtr, nullptr); tree.start();
 */
using CryptoRecipeHolder = std::shared_ptr<void>;

class CryptoTaskBase
{
public:
    virtual ~CryptoTaskBase() = default;

    /** 任务类型（枚举复用 AsyncTaskType::FileEncrypt / FileDecrypt） */
    virtual AsyncTaskType taskType() const noexcept = 0;

    /** 提交给 AsyncTaskManager 时使用的显示标题 */
    virtual QString title() const = 0;

    /** 提交给 AsyncTaskManager 时使用的详细描述（含文件路径/大小/算法） */
    virtual QString description() const = 0;

    /** 优先级：解密 Highest；加密 High */
    virtual AsyncTaskPriority priority() const noexcept = 0;

    /**
     * @brief 构造一个声明式的 QTaskTree Group recipe。
     *        recipe 内部步骤使用 QThreadFunctionTask 在 AsyncThreadPool 的线程里并发执行。
     * @param outTaskId  [output] 若非空，返回 AsyncTaskManager::submit 登记的 taskId（用于 UI 取消/查看）
     * @param cancelPtr  [input]  跨线程取消原子指针（AsyncTaskManager 提供）
     * @param reportProgress [input] 回调：reportProgress(done, total)，将被包装为 QueuedConnection 回主线程更新 Model
     * @param reportError    [input] 回调：失败时调用，写 error.stackFrames/message
     * @return CryptoRecipeHolder (shared_ptr<void>)：包装 QtTaskTree::Group。
     *         占位实现：返回 nullptr（未实现算法，走 AsyncTaskManager::submit 普通 worker 即可）
     */
    virtual CryptoRecipeHolder buildRecipe(
        qint64 *outTaskId,
        AsyncCancelPtr cancelPtr,
        std::function<void(qint64 done, qint64 total)> reportProgress,
        std::function<void(const AsyncErrorInfo& err)> reportError
    ) = 0;
};

class FileEncryptTask : public CryptoTaskBase
{
public:
    FileEncryptTask(QString srcPlainPath, QString dstEncryptedPath, QString passwordHint)
        : src_(std::move(srcPlainPath)), dst_(std::move(dstEncryptedPath)), hint_(std::move(passwordHint)) {}

    AsyncTaskType taskType() const noexcept override { return AsyncTaskType::FileEncrypt; }
    QString title() const override { return QObject::tr("加密保存: %1").arg(dst_); }
    QString description() const override {
        return QObject::tr("源: %1\n目标: %2\n密码提示: %3\n\n（该功能为架构占位，尚未实现加密写入算法）").arg(src_, dst_, hint_);
    }
    AsyncTaskPriority priority() const noexcept override { return AsyncTaskPriority::High; }

    CryptoRecipeHolder buildRecipe(qint64 *outTaskId, AsyncCancelPtr cancelPtr,
                                   std::function<void(qint64,qint64)> reportProgress,
                                   std::function<void(const AsyncErrorInfo&)> reportError) override
    {
        Q_UNUSED(cancelPtr); Q_UNUSED(reportProgress); Q_UNUSED(reportError);
        if (outTaskId) *outTaskId = -1;
        // ===== TODO（真实加密接入时实现） =====
        //   #include <QtTaskTree/qtasktree.h>
        //   Group group;
        //   group.add(step1).add(step2) ...;
        //   return std::make_shared<Group>(std::move(group));
        return nullptr;
    }
private:
    QString src_, dst_, hint_;
};

class FileDecryptTask : public CryptoTaskBase
{
public:
    FileDecryptTask(QString srcEncryptedPath, QString passwordHint)
        : src_(std::move(srcEncryptedPath)), hint_(std::move(passwordHint)) {}

    AsyncTaskType taskType() const noexcept override { return AsyncTaskType::FileDecrypt; }
    QString title() const override { return QObject::tr("解密打开: %1").arg(src_); }
    QString description() const override {
        return QObject::tr("加密文件: %1\n密码提示: %2\n\n（该功能为架构占位，尚未实现解密读取算法）").arg(src_, hint_);
    }
    AsyncTaskPriority priority() const noexcept override { return AsyncTaskPriority::Highest; }

    CryptoRecipeHolder buildRecipe(qint64 *outTaskId, AsyncCancelPtr cancelPtr,
                                   std::function<void(qint64,qint64)> reportProgress,
                                   std::function<void(const AsyncErrorInfo&)> reportError) override
    {
        Q_UNUSED(cancelPtr); Q_UNUSED(reportProgress); Q_UNUSED(reportError);
        if (outTaskId) *outTaskId = -1;
        return nullptr;
    }
private:
    QString src_, hint_;
};

#endif // CRYPTOTASK_H
