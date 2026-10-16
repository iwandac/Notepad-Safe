#pragma once

#include <QObject>
#include <QVector>
#include <QString>
#include <QHash>
#include <QDateTime>
#include <QUuid>
#include <memory>
#include "SessionDocumentPort.h"

// 完整会话
struct SessionData {
    enum class LoadStatus { Missing, Loaded, Invalid };
    LoadStatus loadStatus = LoadStatus::Missing;
    int version = 2;
    QVector<SessionTab> tabs;
    int activeTab = 0;
};

// 会话管理器：自动缓存未保存文件 + 启动时恢复
class SessionManager : public QObject {
    Q_OBJECT
public:
    struct AsyncSavePlan;
    struct AsyncSaveResult;
    explicit SessionManager(QObject *parent = nullptr);
    // 测试用构造函数：指定自定义数据目录
    SessionManager(const QString &dataDir, QObject *parent = nullptr);

    // 获取 session.json 路径
    QString sessionFilePath() const;
    // 获取 autosave 目录
    QString autosaveDir() const;

    // 保存当前所有标签的状态
    // tabs: 所有标签列表，activeIndex: 当前活动标签索引
    bool saveSession(const QVector<ISessionDocumentPort *> &tabs, int activeIndex,
                     bool forceSnapshots = false);

    // UI-owned state is captured synchronously; encryption, filesystem I/O
    // and cleanup operate only on the detached plan in a worker.
    std::shared_ptr<AsyncSavePlan> captureAsyncSave(
        const QVector<ISessionDocumentPort *> &tabs, int activeIndex,
        bool forceSnapshots = false);
    std::shared_ptr<AsyncSavePlan> captureAsyncSaveEntries(
        const QVector<SessionCaptureEntry> &entries, int activeIndex,
        bool forceSnapshots = false, bool preserveInvalidSession = false);
    std::shared_ptr<AsyncSaveResult> writeAsyncSave(
        const std::shared_ptr<AsyncSavePlan> &plan) const;
    bool asyncSaveSucceeded(
        const std::shared_ptr<AsyncSaveResult> &result) const;
    QString asyncSavePreservedPath(const std::shared_ptr<AsyncSaveResult> &result) const;
    QString asyncSaveError(const std::shared_ptr<AsyncSaveResult> &result) const;
    QString preservedRecoveryDir() const;
    void applyAsyncSave(const std::shared_ptr<AsyncSaveResult> &result,
                        const QVector<ISessionDocumentPort *> &tabs);

    // 恢复内部快照。新版 .ndraft 在内存中解密后直接交给编辑器；旧版 .tmp
    // 仅作为迁移读取路径保留，不再产生新的 S1 明文快照。
    bool restoreAutosave(ISessionDocumentPort *tab, const QString &autosavePath,
                         const QString &realFilePath = QString(),
                         QString *error = nullptr,
                         QString *diagnosticCode = nullptr) const;

    // 加载上次会话
    SessionData loadSession() const;

    // 清理会话（成功恢复后调用）
    void clearSession();

    // 清理过期的 autosave 文件（超过 7 天）
    void cleanupOldAutosaves();

    // 生成新的 autosave 文件名（使用 UUID 保证唯一性/稳定性）
    QString generateAutosavePath() const;

    // 清理单个 autosave 文件（用户正式保存后调用）。仅允许删除本实例
    // autosave 目录内、由 SessionManager 命名的恢复文件。
    bool removeAutosaveFile(const QString &path) const;
    bool discardRecovery(ISessionDocumentPort *document) const;

private:
    QString m_dataDir;
    QString m_autosaveDir;
    QHash<QUuid, quint64> m_snapshotRevisions;
    QHash<QUuid, QDateTime> m_snapshotTimes;

    void ensureDirs();
    bool isManagedAutosavePath(const QString &path) const;
    bool preserveSessionFiles(QString *preservedPath, QString *error) const;
};
