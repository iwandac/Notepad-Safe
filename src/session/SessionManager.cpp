#include "SessionManager.h"
#include "DraftStore.h"
#include "EditorTab.h"
#include "VirtualFileView.h"

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
#include <QSet>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

void scrubSnapshot(QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
#ifdef Q_OS_WIN
    ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
    volatile char *cursor = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        cursor[i] = 0;
#endif
    bytes.clear();
}

} // namespace

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
    // S1 新快照固定为本机加密 .ndraft；旧 .tmp 只保留迁移读取。
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return m_autosaveDir + QString("/autosave_%1.ndraft").arg(uuid);
}

void SessionManager::removeAutosaveFile(const QString &path)
{
    if (!path.isEmpty() && QFile::exists(path))
        QFile::remove(path);
}

bool SessionManager::restoreAutosave(EditorTab *tab, const QString &autosavePath,
                                     const QString &realFilePath) const
{
    if (!tab || autosavePath.isEmpty() || !QFileInfo::exists(autosavePath))
        return false;
    bool loaded = false;
    if (DraftStore::isEncryptedDraft(autosavePath)) {
        QString error;
        DraftStore::SnapshotInfo info;
        if (!DraftStore::inspect(autosavePath, &info, &error)) {
            qWarning("[SessionManager] encrypted draft restore failed: %s",
                     qUtf8Printable(error));
            return false;
        }
        if (info.kind == DraftStore::PayloadKind::BinaryPatchJournal) {
            loaded = tab->restoreBinaryRecoveryJournal(
                autosavePath, realFilePath, &error);
        } else if (info.kind == DraftStore::PayloadKind::FullCheckpoint) {
            loaded = tab->restoreRecoveryCheckpoint(
                autosavePath, realFilePath, TextEncoding(info.encoding),
                info.plaintextBytes, &error);
        } else {
            QByteArray plaintext;
            if (!DraftStore::readSnapshot(autosavePath, &plaintext, &error)) {
                qWarning("[SessionManager] encrypted draft restore failed: %s",
                         qUtf8Printable(error));
                return false;
            }
            loaded = tab->restoreRecoveryPayload(plaintext, realFilePath, &error);
            scrubSnapshot(plaintext);
        }
        if (!loaded && !error.isEmpty())
            qWarning("[SessionManager] recovery payload rejected: %s", qUtf8Printable(error));
        if (loaded)
            tab->setAutosavePath(autosavePath);
    } else {
        // Backward-compatible reader for snapshots written before DraftStore v1.
        loaded = tab->openAutosaveAutoMode(autosavePath, realFilePath);
    }
    if (loaded && !realFilePath.isEmpty()) {
        tab->setFilePath(realFilePath);
        tab->setModified(true);
    }
    return loaded;
}

bool SessionManager::saveSession(const QVector<EditorTab *> &tabs, int activeIndex,
                                 bool forceSnapshots)
{
    ensureDirs();

    // 先收集本次需要保留的 autosave 路径，再删除其他的（避免先清空导致恢复时偶发失败）
    QStringList toKeep;
    for (EditorTab *tab : tabs) {
        if (tab->isSecureDocument()) {
            if (!tab->autosavePath().isEmpty())
                toKeep << tab->autosavePath();
            continue;
        }
        if (!tab->autosavePath().isEmpty())
            toKeep << tab->autosavePath();
    }

    QJsonArray tabsArray;
    QSet<QUuid> activeInstances;
    constexpr qint64 largeSnapshotIntervalMs = 5LL * 60 * 1000;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < tabs.size(); ++i) {
        EditorTab *tab = tabs[i];
        const QUuid instanceId = tab->sessionInstanceId();
        activeInstances.insert(instanceId);
        SessionTab st;
        st.filePath = tab->filePath();
        st.encoding = encodingName(tab->encoding());
        st.syntax = static_cast<int>(tab->syntax());
        tab->getCursorPosition(&st.cursorLine, &st.cursorIndex);
        if (tab->isL2Mode() && tab->l2View()) {
            st.cursorByteOffset = tab->currentByteOffset();
            st.selectionStartByte = tab->l2View()->selectionStart();
            st.selectionEndByte = tab->l2View()->selectionEnd();
            st.presentation = tab->viewModeL2() == ViewMode::Hex
                ? QStringLiteral("hex") : QStringLiteral("text");
        } else if (tab->editor()) {
            st.cursorByteOffset = tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_GETCURRENTPOS);
            st.selectionStartByte = tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONSTART);
            st.selectionEndByte = tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONEND);
            st.presentation = QStringLiteral("text");
        }
        st.modified = tab->isModified();
        st.splitView = tab->splitViewEnabled();
        if (st.splitView) {
            st.splitOrientation = tab->splitOrientation() == Qt::Vertical
                ? QStringLiteral("vertical") : QStringLiteral("horizontal");
        }
        st.secureDocument = tab->isSecureDocument();
        if (st.secureDocument) {
            st.documentId = tab->secureDocumentId().toString(QUuid::WithoutBraces);
            st.autosavePath = tab->autosavePath();
        }

        // 如果有未保存内容，写入 autosave 文件
        if (!st.secureDocument && (tab->isUntitled() || tab->isModified())) {
            const bool emptyUntitled = tab->isUntitled() && !tab->isModified()
                                       && !tab->isLargeDocument()
                                       && tab->editor()->length() == 0;
            if (emptyUntitled) {
                st.autosavePath.clear();
            } else {
            const bool largeDocument = tab->isLargeDocument();
            const QString previousPath = tab->autosavePath();
            const bool previousExists = !previousPath.isEmpty()
                                        && QFileInfo::exists(previousPath);
            QString ap = previousPath;
            if (!previousExists || !DraftStore::isEncryptedDraft(ap)) {
                // 新写入永远迁移到加密 .ndraft；旧 .tmp 仅在新快照提交失败时保留。
                ap = generateAutosavePath();
            }
            st.autosavePath = ap;

            const bool existingSnapshot = QFileInfo::exists(ap);
            const quint64 revision = tab->contentRevision();
            const bool sameRevision = existingSnapshot
                                      && m_snapshotRevisions.contains(instanceId)
                                      && m_snapshotRevisions.value(instanceId) == revision;
            const QDateTime previous = m_snapshotTimes.value(instanceId);
            const bool largeThrottled = tab->isLargeDocument() && existingSnapshot
                                        && previous.isValid()
                                        && previous.msecsTo(now) < largeSnapshotIntervalMs;
            const bool writeSnapshot = !existingSnapshot
                                       || (!sameRevision
                                           && (forceSnapshots || !largeThrottled));

            if (!writeSnapshot) {
                toKeep << ap;
                tab->setAutosavePath(ap);
            } else {
                bool snapshotWritten = false;
                QByteArray plaintext;
                QString draftError;
                if (tab->isBinaryDocument()) {
                    snapshotWritten = DraftStore::writeBinaryPatchJournal(
                        ap, [tab](QIODevice *output, QString *writerError) {
                            return tab->exportBinaryRecoveryJournal(output, writerError);
                        }, &draftError);
                } else {
                    const bool payloadReady = tab->exportRecoveryPayload(&plaintext,
                                                                          &draftError);
                    if (payloadReady) {
                        snapshotWritten = DraftStore::writeSnapshot(ap, plaintext,
                                                                     &draftError);
                    } else if (largeDocument && !tab->isL2Mode()) {
                    draftError.clear();
                    snapshotWritten = DraftStore::writeCheckpoint(
                        ap, int(tab->encoding()),
                        [tab](QIODevice *output, QString *writerError) {
                            return tab->exportRecoveryCheckpoint(output, writerError);
                        }, &draftError);
                    }
                }
                scrubSnapshot(plaintext);
                if (!snapshotWritten && !draftError.isEmpty())
                    qWarning("[SessionManager] encrypted draft write failed: %s",
                             qUtf8Printable(draftError));
                if (snapshotWritten) {
                toKeep << ap;
                tab->setAutosavePath(ap);
                m_snapshotRevisions.insert(instanceId, tab->contentRevision());
                m_snapshotTimes.insert(instanceId, now);
                // 原子提交成功后立即删除旧版明文快照，完成无损迁移。
                if (previousExists && previousPath != ap) {
                    toKeep.removeAll(previousPath);
                    QFile::remove(previousPath);
                }
                } else if (existingSnapshot) {
                // 新快照失败时保留最后一个可恢复版本，不能把唯一副本从会话中移除。
                toKeep << ap;
                tab->setAutosavePath(ap);
                } else if (previousExists) {
                    st.autosavePath = previousPath;
                    toKeep << previousPath;
                    tab->setAutosavePath(previousPath);
                } else {
                st.autosavePath.clear();
                }
            }
            }
        }

        QJsonObject obj;
        obj["filePath"] = st.filePath;
        obj["encoding"] = st.encoding;
        obj["syntax"] = st.syntax;
        obj["cursorLine"] = st.cursorLine;
        obj["cursorIndex"] = st.cursorIndex;
        obj["cursorByteOffset"] = QString::number(st.cursorByteOffset);
        obj["selectionStartByte"] = QString::number(st.selectionStartByte);
        obj["selectionEndByte"] = QString::number(st.selectionEndByte);
        obj["modified"] = st.modified;
        obj["autosavePath"] = st.autosavePath;
        obj["secureDocument"] = st.secureDocument;
        obj["documentId"] = st.documentId;
        obj["presentation"] = st.presentation;
        obj["splitView"] = st.splitView;
        obj["splitOrientation"] = st.splitOrientation;
        tabsArray.append(obj);
    }

    for (auto it = m_snapshotRevisions.begin(); it != m_snapshotRevisions.end();) {
        if (!activeInstances.contains(it.key()))
            it = m_snapshotRevisions.erase(it);
        else
            ++it;
    }
    for (auto it = m_snapshotTimes.begin(); it != m_snapshotTimes.end();) {
        if (!activeInstances.contains(it.key()))
            it = m_snapshotTimes.erase(it);
        else
            ++it;
    }

    // 清理不在保留列表里的旧 autosave
    QDir dir(m_autosaveDir);
    auto oldFiles = dir.entryInfoList({"autosave_*.ndraft", "autosave_*.tmp", "tab_*.tmp"},
                                      QDir::Files);
    for (const QFileInfo &fi : oldFiles) {
        if (!toKeep.contains(fi.absoluteFilePath()))
            QFile::remove(fi.absoluteFilePath());
    }

    QJsonObject root;
    root["version"] = 2;
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
        bool cursorOk = false;
        st.cursorByteOffset = obj["cursorByteOffset"].toString().toLongLong(&cursorOk);
        if (!cursorOk) st.cursorByteOffset = -1;
        bool selectionStartOk = false;
        st.selectionStartByte = obj["selectionStartByte"].toString()
                                    .toLongLong(&selectionStartOk);
        if (!selectionStartOk) st.selectionStartByte = -1;
        bool selectionEndOk = false;
        st.selectionEndByte = obj["selectionEndByte"].toString()
                                  .toLongLong(&selectionEndOk);
        if (!selectionEndOk) st.selectionEndByte = -1;
        st.modified = obj["modified"].toBool(false);
        st.autosavePath = obj["autosavePath"].toString();
        st.secureDocument = obj["secureDocument"].toBool(false);
        st.splitView = obj["splitView"].toBool(false);
        st.splitOrientation = obj["splitOrientation"].toString();
        st.documentId = obj["documentId"].toString();
        st.presentation = obj["presentation"].toString();
        data.tabs.append(st);
    }
    return data;
}

void SessionManager::clearSession()
{
    QFile::remove(sessionFilePath());
    QDir dir(m_autosaveDir);
    auto files = dir.entryList({"autosave_*.ndraft", "tab_*.tmp", "autosave_*.tmp"},
                               QDir::Files);
    for (const QString &f : files)
        QFile::remove(dir.filePath(f));
}

void SessionManager::cleanupOldAutosaves()
{
    QDir dir(m_autosaveDir);
    auto files = dir.entryInfoList({"autosave_*.ndraft", "tab_*.tmp", "autosave_*.tmp"},
                                   QDir::Files);
    QDateTime threshold = QDateTime::currentDateTime().addDays(-7);
    for (const QFileInfo &fi : files) {
        if (fi.lastModified() < threshold)
            QFile::remove(fi.absoluteFilePath());
    }
}
