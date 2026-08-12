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

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
    QString localApp = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    // Windows 上 GenericDataLocation 是 %LOCALAPPDATA%
    m_dataDir = localApp + "/NotepadSafe";
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
            QSaveFile f(ap);
            if (f.open(QIODevice::WriteOnly)) {
                QByteArray data = encodeText(tab->editor()->text(), tab->encoding());
                f.write(data);
                if (f.commit()) {
                    toKeep << ap;
                    // 回写到 EditorTab 保持关联
                    tab->setAutosavePath(ap);
                } else {
                    st.autosavePath.clear();
                }
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
