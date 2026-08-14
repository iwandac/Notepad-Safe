#pragma once

#include <QObject>
#include <QVector>
#include <QString>

class EditorTab;

// 单个标签的会话数据
struct SessionTab {
    QString filePath;        // 原始文件路径（空表示未命名）
    QString encoding;        // 编码名称
    int syntax = 0;          // Syntax 枚举值
    int cursorLine = 0;
    int cursorIndex = 0;
    bool modified = false;
    QString autosavePath;    // 未保存内容的临时文件路径
};

// 完整会话
struct SessionData {
    int version = 1;
    QVector<SessionTab> tabs;
    int activeTab = 0;
};

// 会话管理器：自动缓存未保存文件 + 启动时恢复
class SessionManager : public QObject {
    Q_OBJECT
public:
    explicit SessionManager(QObject *parent = nullptr);
    // 测试用构造函数：指定自定义数据目录
    SessionManager(const QString &dataDir, QObject *parent = nullptr);

    // 获取 session.json 路径
    QString sessionFilePath() const;
    // 获取 autosave 目录
    QString autosaveDir() const;

    // 保存当前所有标签的状态
    // tabs: 所有标签列表，activeIndex: 当前活动标签索引
    bool saveSession(const QVector<EditorTab *> &tabs, int activeIndex);

    // 加载上次会话
    SessionData loadSession() const;

    // 清理会话（成功恢复后调用）
    void clearSession();

    // 清理过期的 autosave 文件（超过 7 天）
    void cleanupOldAutosaves();

    // 生成新的 autosave 文件名（使用 UUID 保证唯一性/稳定性）
    QString generateAutosavePath() const;

    // 清理单个 autosave 文件（用户正式保存后调用）
    static void removeAutosaveFile(const QString &path);

private:
    QString m_dataDir;
    QString m_autosaveDir;

    void ensureDirs();
};
