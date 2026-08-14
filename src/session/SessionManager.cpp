#include "SessionManager.h"
#include "EditorTab.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDateTime>
#include <QSaveFile>
#include <QUuid>
#include <QProcessEnvironment>

static QString resolveDataDir()
{
    // 允许通过环境变量 NOTEPADSAFE_DATADIR 覆盖用户数据目录（e2e/自动化测试用）
    //   - 不设置：默认 %LOCALAPPDATA%\NotepadSafe（用户真实数据，原行为）
    //   - 设置：直接用该路径（沙箱/CI 指向 %TEMP%\NotepadSafe_E2E\data，不污染真实用户数据）
    QString env = QProcessEnvironment::systemEnvironment().value("NOTEPADSAFE_DATADIR").trimmed();
    if (!env.isEmpty()) {
        // 必须是绝对路径；否则回退默认（避免相对路径歧义）
        QFileInfo fi(env);
        if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
        qWarning("[SessionManager] NOTEPADSAFE_DATADIR='%s' 不是绝对路径，忽略回退默认",
                 qUtf8Printable(env));
    }
    QString localApp = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return localApp + "/NotepadSafe";
}

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
    // 支持 NOTEPADSAFE_DATADIR 覆盖：便于 e2e 沙箱/CI 在 %TEMP% 下独立运行
    m_dataDir     = resolveDataDir();
    m_autosaveDir = m_dataDir + "/autosave";
    ensureDirs();
}

SessionManager::SessionManager(const QString &dataDir, QObject *parent)
    : QObject(parent)
    , m_dataDir(dataDir)
    , m_autosaveDir(dataDir + "/autosave")
{
    ensureDirs();
}

void SessionManager::ensureDirs()
{
    QDir().mkpath(m_dataDir);
    QDir().mkpath(m_autosaveDir);
}

QString SessionManager::sessionFilePath() const
{
    return m_dataDir + "/session.json";
}

QString SessionManager::autosaveDir() const
{
    return m_autosaveDir;
}

QString SessionManager::generateAutosavePath() const
{
    // 用 UUID 生成唯一文件名，跨重启保持稳定（不会因 index 变化对不上）
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return m_autosaveDir + QString("/autosave_%1.tmp").arg(uuid);
}

void SessionManager::removeAutosaveFile(const QString &path)
{
    if (!path.isEmpty() && QFile::exists(path))
        QFile::remove(path);
}

bool SessionManager::saveSession(const QVector<EditorTab *> &tabs, int activeIndex)
{
    ensureDirs();

    // 先收集本次需要保留的 autosave 路径，再删除其他的（避免先清空导致恢复时偶发失败）
    QStringList toKeep;
    for (EditorTab *tab : tabs) {
        if (!tab->autosavePath().isEmpty())
            toKeep << tab->autosavePath();
    }

    QJsonArray tabsArray;
    for (int i = 0; i < tabs.size(); ++i) {
        EditorTab *tab = tabs[i];
        SessionTab st;
        st.filePath = tab->filePath();
        st.encoding = encodingName(tab->encoding());
        st.syntax = static_cast<int>(tab->syntax());
        tab->getCursorPosition(&st.cursorLine, &st.cursorIndex);
        st.modified = tab->isModified();

        // 如果有未保存内容，写入 autosave 文件
        if (tab->isUntitled() || tab->isModified()) {
            // 优先沿用已有的 autosave 路径（保持稳定），否则新建
            QString ap = tab->autosavePath();
            if (ap.isEmpty() || !QFileInfo::exists(ap)) {
                ap = generateAutosavePath();
            }
            st.autosavePath = ap;

            // ====== 关键修复（BUG A：L2 数据丢失 + BUG B：L1 大文件阻塞）======
            //   旧做法：encodeText(tab->editor()->text(), tab->encoding())
            //     · L2 模式：editor() 返回 L1 空/旧 QsciScintilla，内容丢失（用户 L2 patches 全没了）
            //     · L1 100MB：editor()->text() 转 100MB QString 主线程阻塞 1~2s
            //   新做法：tab->exportToFile(ap)
            //     · L1/L2 自动分流：L1 写文档文本，L2 流式 applyAndSave（4MB/块 + 原子写）
            //     · 零副作用：不改变 isModified、m_filePath、patches 状态
            if (tab->exportToFile(ap)) {
                toKeep << ap;
                // 回写到 EditorTab 保持关联
                tab->setAutosavePath(ap);
            } else {
                st.autosavePath.clear();
            }
        }

        QJsonObject obj;
        obj["filePath"] = st.filePath;
        obj["encoding"] = st.encoding;
        obj["syntax"] = st.syntax;
        obj["cursorLine"] = st.cursorLine;
        obj["cursorIndex"] = st.cursorIndex;
        obj["modified"] = st.modified;
        obj["autosavePath"] = st.autosavePath;
        tabsArray.append(obj);
    }

    // 清理不在保留列表里的旧 autosave
    QDir dir(m_autosaveDir);
    auto oldFiles = dir.entryInfoList({"autosave_*.tmp", "tab_*.tmp"}, QDir::Files);
    for (const QFileInfo &fi : oldFiles) {
        if (!toKeep.contains(fi.absoluteFilePath()))
            QFile::remove(fi.absoluteFilePath());
    }

    QJsonObject root;
    root["version"] = 1;
    root["tabs"] = tabsArray;
    root["activeTab"] = activeIndex;

    QJsonDocument doc(root);
    QSaveFile f(sessionFilePath());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(doc.toJson());
    return f.commit();
}

SessionData SessionManager::loadSession() const
{
    SessionData data;
    QFile f(sessionFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return data;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError)
        return data;

    QJsonObject root = doc.object();
    data.version = root["version"].toInt(1);
    data.activeTab = root["activeTab"].toInt(0);

    QJsonArray tabsArray = root["tabs"].toArray();
    for (const QJsonValue &v : tabsArray) {
        QJsonObject obj = v.toObject();
        SessionTab st;
        st.filePath = obj["filePath"].toString();
        st.encoding = obj["encoding"].toString();
        st.syntax = obj["syntax"].toInt(0);
        st.cursorLine = obj["cursorLine"].toInt(0);
        st.cursorIndex = obj["cursorIndex"].toInt(0);
        st.modified = obj["modified"].toBool(false);
        st.autosavePath = obj["autosavePath"].toString();
        data.tabs.append(st);
    }
    return data;
}

void SessionManager::clearSession()
{
    QFile::remove(sessionFilePath());
    QDir dir(m_autosaveDir);
    auto files = dir.entryList({"tab_*.tmp", "autosave_*.tmp"}, QDir::Files);
    for (const QString &f : files)
        QFile::remove(dir.filePath(f));
}

void SessionManager::cleanupOldAutosaves()
{
    QDir dir(m_autosaveDir);
    auto files = dir.entryInfoList({"tab_*.tmp", "autosave_*.tmp"}, QDir::Files);
    QDateTime threshold = QDateTime::currentDateTime().addDays(-7);
    for (const QFileInfo &fi : files) {
        if (fi.lastModified() < threshold)
            QFile::remove(fi.absoluteFilePath());
    }
}
