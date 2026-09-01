#include "SessionManager.h"
#include "AppDataLocation.h"
#include "DraftStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSaveFile>
#include <QUuid>
#include <QSet>
#include <QBuffer>
#include <limits>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

constexpr qint64 MaximumSessionFileBytes = 4LL * 1024 * 1024;
constexpr qsizetype MaximumSessionTabs = 256;

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

bool isSameOrChildPath(const QString &rootPath, const QString &candidatePath)
{
    const QString root = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
    const QString candidate = QDir::cleanPath(QFileInfo(candidatePath).absoluteFilePath());
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return candidate.compare(root, sensitivity) == 0
           || candidate.startsWith(root + QLatin1Char('/'), sensitivity);
}

QString recoveryPathKey(const QString &path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

} // namespace

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
    // 支持 NOTEPADSAFE_DATADIR 覆盖：便于 e2e 沙箱/CI 在 %TEMP% 下独立运行
    m_dataDir     = resolveNotepadSafeDataRoot();
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

bool SessionManager::isManagedAutosavePath(const QString &path) const
{
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()
        || !QDir::match({QStringLiteral("autosave_*.ndraft"),
                         QStringLiteral("autosave_*.tmp"),
                         QStringLiteral("tab_*.tmp")},
                        QFileInfo(path).fileName())
        || !isSameOrChildPath(m_autosaveDir, path)) {
        return false;
    }

    // A directory junction inside autosave must not turn cleanup into deletion
    // outside the application data tree. Existing files can be checked by their
    // resolved target; non-existing targets are safe to create only by lexical
    // containment above.
    const QFileInfo candidate(path);
    if (candidate.exists()) {
        const QString resolvedRoot = QFileInfo(m_autosaveDir).canonicalFilePath();
        const QString resolvedCandidate = candidate.canonicalFilePath();
        if (resolvedRoot.isEmpty() || resolvedCandidate.isEmpty()
            || !isSameOrChildPath(resolvedRoot, resolvedCandidate)) {
            return false;
        }
    }
    return true;
}

bool SessionManager::removeAutosaveFile(const QString &path) const
{
    if (!isManagedAutosavePath(path))
        return false;
    return !QFile::exists(path) || QFile::remove(path);
}

bool SessionManager::discardRecovery(ISessionDocumentPort *document) const
{
    if (!document)
        return false;
    const QString path = document->recoveryPath();
    if (!path.isEmpty() && !removeAutosaveFile(path))
        return false;
    document->acceptRecoveryPath({});
    return true;
}

bool SessionManager::restoreAutosave(
    ISessionDocumentPort *tab, const QString &autosavePath,
    const QString &realFilePath, QString *error,
    QString *diagnosticCode) const
{
    if (error)
        error->clear();
    if (diagnosticCode)
        diagnosticCode->clear();
    const auto reject = [error, diagnosticCode](const QString &code,
                                                 const QString &message) {
        if (error)
            *error = message;
        if (diagnosticCode)
            *diagnosticCode = code;
        return false;
    };
    if (!tab || !isManagedAutosavePath(autosavePath)) {
        return reject(QStringLiteral("[recovery.draft.invalid]"),
                      tr("恢复稿路径不在应用的受管恢复目录中。"));
    }
    if (!QFileInfo::exists(autosavePath)) {
        return reject(QStringLiteral("[recovery.draft.missing]"),
                      tr("加密恢复稿已不存在。原文件未被修改。"));
    }
    const QFileInfo draftInfo(autosavePath);
    const bool encryptedDraft = DraftStore::isEncryptedDraft(autosavePath);
    const bool legacyPlaintext = draftInfo.fileName().endsWith(
        QStringLiteral(".tmp"), Qt::CaseInsensitive);
    if (!encryptedDraft && !legacyPlaintext) {
        return reject(QStringLiteral("[recovery.draft.invalid]"),
                      tr("加密恢复稿格式无效或已损坏。原文件未被修改。"));
    }

    SessionRestoreRequest request;
    request.encryptedPath = autosavePath;
    request.realFilePath = realFilePath;
    QString localError;
    if (encryptedDraft) {
        DraftStore::SnapshotInfo info;
        if (!DraftStore::inspect(autosavePath, &info, &localError)) {
            qWarning("[SessionManager] encrypted draft metadata rejected");
            return reject(QStringLiteral("[recovery.draft.invalid]"), localError);
        }
        request.encoding = info.encoding;
        request.plaintextBytes = info.plaintextBytes;
        if (info.kind == DraftStore::PayloadKind::BinaryPatchJournal) {
            request.kind = SessionRecoveryKind::BinaryPatchJournal;
        } else if (info.kind == DraftStore::PayloadKind::FullCheckpoint) {
            request.kind = SessionRecoveryKind::FullCheckpoint;
        } else {
            request.kind = SessionRecoveryKind::SmallOrJournal;
            if (!DraftStore::readSnapshot(autosavePath, &request.plaintext,
                                          &localError)) {
                qWarning("[SessionManager] encrypted draft could not be unlocked");
                return reject(QStringLiteral("[recovery.draft.unlock]"), localError);
            }
        }
    } else {
        request.legacyPlaintextFile = true;
    }

    const bool loaded = tab->restoreSessionRecovery(request, &localError);
    scrubSnapshot(request.plaintext);
    if (!loaded) {
        QString code = QStringLiteral("[recovery.restore.failed]");
        if (localError.contains(QStringLiteral("原始二进制文件已移动或变化")))
            code = QStringLiteral("[recovery.source.changed]");
        else if (localError.contains(QStringLiteral("当前 Windows 用户下解锁")))
            code = QStringLiteral("[recovery.draft.unlock]");
        else if (encryptedDraft
                 && (localError.contains(QStringLiteral("恢复检查点"))
                     || localError.contains(QStringLiteral("恢复日志元数据已损坏"))))
            code = QStringLiteral("[recovery.draft.invalid]");
        qWarning("[SessionManager] recovery payload rejected: %s",
                 qUtf8Printable(code));
        return reject(code, localError);
    }
    tab->acceptRecoveryPath(autosavePath);
    return true;
}

bool SessionManager::saveSession(
    const QVector<ISessionDocumentPort *> &tabs, int activeIndex,
    bool forceSnapshots)
{
    const auto plan = captureAsyncSave(tabs, activeIndex, forceSnapshots);
    const auto result = writeAsyncSave(plan);
    applyAsyncSave(result, tabs);
    return asyncSaveSucceeded(result);
}

struct SessionManager::AsyncSavePlan {
    struct Item {
        SessionTab state;
        bool retained = false;
        QUuid instanceId;
        quint64 revision = 0;
        QString previousPath;
        QString targetPath;
        bool previousExists = false;
        bool existingSnapshot = false;
        bool writeSnapshot = false;
        bool payloadReady = false;
        DraftStore::PayloadKind payloadKind = DraftStore::PayloadKind::SmallOrJournal;
        int payloadEncoding = 0;
        QByteArray payload;
        QString exportError;
    };

    QVector<Item> items;
    QSet<QUuid> activeInstances;
    int activeIndex = 0;
    QDateTime capturedAtUtc;
    QString autosaveDir;
    QString sessionPath;
    bool valid = true;
};

struct SessionManager::AsyncSaveResult {
    struct ItemResult {
        QUuid instanceId;
        quint64 revision = 0;
        QString autosavePath;
        bool snapshotWritten = false;
    };

    QVector<ItemResult> items;
    QSet<QUuid> activeInstances;
    QDateTime capturedAtUtc;
    bool sessionWritten = false;
};

std::shared_ptr<SessionManager::AsyncSavePlan> SessionManager::captureAsyncSave(
    const QVector<ISessionDocumentPort *> &tabs, int activeIndex,
    bool forceSnapshots)
{
    QVector<SessionCaptureEntry> entries;
    entries.reserve(tabs.size());
    for (auto *tab : tabs)
        entries.append(SessionCaptureEntry{tab});
    return captureAsyncSaveEntries(entries, activeIndex, forceSnapshots);
}

std::shared_ptr<SessionManager::AsyncSavePlan> SessionManager::captureAsyncSaveEntries(
    const QVector<SessionCaptureEntry> &entries, int activeIndex,
    bool forceSnapshots)
{
    ensureDirs();
    auto plan = std::make_shared<AsyncSavePlan>();
    plan->activeIndex = activeIndex;
    plan->capturedAtUtc = QDateTime::currentDateTimeUtc();
    plan->autosaveDir = m_autosaveDir;
    plan->sessionPath = sessionFilePath();
    constexpr qint64 largeSnapshotIntervalMs = 5LL * 60 * 1000;

    // Validate before exporting any live payload. Missing/corrupt managed drafts
    // remain discoverable failure records; an external ordinary path is not ours.
    for (const auto &entry : entries) {
        if (const auto *retained = std::get_if<SessionTab>(&entry);
            retained && !retained->secureDocument && !retained->autosavePath.isEmpty()
            && !isManagedAutosavePath(retained->autosavePath)) {
            plan->valid = false;
            return plan;
        }
    }
    for (const auto &entry : entries) {
        if (const auto *retained = std::get_if<SessionTab>(&entry)) {
            AsyncSavePlan::Item item;
            item.state = *retained;
            item.retained = true;
            plan->items.append(std::move(item));
            continue;
        }
        ISessionDocumentPort *tab = std::get<ISessionDocumentPort *>(entry);
        if (!tab)
            continue;
        AsyncSavePlan::Item item;
        item.instanceId = tab->sessionInstanceId();
        item.revision = tab->sessionRevision();
        plan->activeInstances.insert(item.instanceId);
        SessionTab &st = item.state;
        st = tab->captureSessionTab();
        if (st.secureDocument) {
            plan->items.append(std::move(item));
            continue;
        }

        if (!tab->needsRecoverySnapshot()) {
            plan->items.append(std::move(item));
            continue;
        }
        if (tab->isEmptyUntitledDocument()) {
            st.autosavePath.clear();
            plan->items.append(std::move(item));
            continue;
        }

        item.previousPath = isManagedAutosavePath(tab->recoveryPath())
            ? tab->recoveryPath() : QString();
        item.previousExists = !item.previousPath.isEmpty()
                              && QFileInfo::exists(item.previousPath);
        item.targetPath = item.previousPath;
        if (!item.previousExists || !DraftStore::isEncryptedDraft(item.targetPath))
            item.targetPath = generateAutosavePath();
        st.autosavePath = item.targetPath;
        item.existingSnapshot = QFileInfo::exists(item.targetPath);
        const bool sameRevision = item.existingSnapshot
                                  && m_snapshotRevisions.value(item.instanceId,
                                                               (std::numeric_limits<quint64>::max)())
                                         == item.revision;
        const QDateTime previousTime = m_snapshotTimes.value(item.instanceId);
        const bool largeThrottled = tab->isLargeRecoveryDocument()
                                    && item.existingSnapshot
                                    && previousTime.isValid()
                                    && previousTime.msecsTo(plan->capturedAtUtc)
                                           < largeSnapshotIntervalMs;
        item.writeSnapshot = !item.existingSnapshot
                             || (!sameRevision && (forceSnapshots || !largeThrottled));

        if (item.writeSnapshot) {
            SessionRecoveryPayload payload;
            item.payloadReady = tab->captureRecoveryPayload(
                &payload, &item.exportError);
            item.payload = std::move(payload.bytes);
            item.payloadEncoding = payload.encoding;
            if (payload.kind == SessionRecoveryKind::FullCheckpoint) {
                item.payloadKind = DraftStore::PayloadKind::FullCheckpoint;
            } else if (payload.kind == SessionRecoveryKind::BinaryPatchJournal) {
                item.payloadKind = DraftStore::PayloadKind::BinaryPatchJournal;
            } else {
                item.payloadKind = DraftStore::PayloadKind::SmallOrJournal;
            }
        }
        plan->items.append(std::move(item));
    }
    return plan;
}

std::shared_ptr<SessionManager::AsyncSaveResult> SessionManager::writeAsyncSave(
    const std::shared_ptr<AsyncSavePlan> &plan) const
{
    auto result = std::make_shared<AsyncSaveResult>();
    if (!plan || !plan->valid)
        return result;
    result->activeInstances = plan->activeInstances;
    result->capturedAtUtc = plan->capturedAtUtc;
    QSet<QString> toKeep;
    QJsonArray tabsArray;

    for (auto &item : plan->items) {
        AsyncSaveResult::ItemResult itemResult;
        itemResult.instanceId = item.instanceId;
        itemResult.revision = item.revision;
        QString finalPath = item.state.autosavePath;

        if (!item.state.secureDocument && item.writeSnapshot) {
            bool written = false;
            QString error = item.exportError;
            if (item.payloadReady) {
                if (item.payloadKind == DraftStore::PayloadKind::SmallOrJournal) {
                    written = DraftStore::writeSnapshot(item.targetPath, item.payload,
                                                        &error);
                } else {
                    const QByteArray *payload = &item.payload;
                    const auto writer = [payload](QIODevice *output, QString *writerError) {
                        if (output->write(*payload) == payload->size())
                            return true;
                        if (writerError)
                            *writerError = QObject::tr("恢复快照写入不完整。");
                        return false;
                    };
                    written = item.payloadKind == DraftStore::PayloadKind::FullCheckpoint
                        ? DraftStore::writeCheckpoint(item.targetPath,
                                                      item.payloadEncoding, writer, &error)
                        : DraftStore::writeBinaryPatchJournal(item.targetPath, writer,
                                                              &error);
                }
            }
            scrubSnapshot(item.payload);
            if (written) {
                finalPath = item.targetPath;
                itemResult.snapshotWritten = true;
                // A different previous path may still be referenced by the old
                // manifest or an undelivered item. Only post-publication GC owns it.
            } else if (item.existingSnapshot) {
                finalPath = item.targetPath;
            } else if (item.previousExists) {
                finalPath = item.previousPath;
            } else {
                finalPath.clear();
            }
            if (!written && !error.isEmpty())
                qWarning("[SessionManager] encrypted draft write failed: %s",
                         qUtf8Printable(error));
        }
        item.state.autosavePath = finalPath;
        itemResult.autosavePath = finalPath;
        if (!finalPath.isEmpty())
            toKeep.insert(recoveryPathKey(finalPath));
        if (!item.retained)
            result->items.append(itemResult);

        QJsonObject obj;
        obj["filePath"] = item.state.filePath;
        obj["encoding"] = item.state.encoding;
        obj["syntax"] = item.state.syntax;
        obj["cursorLine"] = item.state.cursorLine;
        obj["cursorIndex"] = item.state.cursorIndex;
        obj["cursorByteOffset"] = QString::number(item.state.cursorByteOffset);
        obj["selectionStartByte"] = QString::number(item.state.selectionStartByte);
        obj["selectionEndByte"] = QString::number(item.state.selectionEndByte);
        obj["modified"] = item.state.modified;
        obj["autosavePath"] = item.state.autosavePath;
        obj["secureDocument"] = item.state.secureDocument;
        obj["documentId"] = item.state.documentId;
        obj["presentation"] = item.state.presentation;
        obj["hexInsertMode"] = item.state.hexInsertMode;
        obj["splitView"] = item.state.splitView;
        obj["splitOrientation"] = item.state.splitOrientation;
        tabsArray.append(obj);
    }

    QJsonObject root;
    root["version"] = 2;
    root["tabs"] = tabsArray;
    root["activeTab"] = plan->activeIndex;
    const QByteArray json = QJsonDocument(root).toJson();
    QSaveFile file(plan->sessionPath);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(json) == json.size()) {
        result->sessionWritten = file.commit();
    }
    // The old session must remain fully recoverable until the replacement
    // session file has committed atomically.
    if (result->sessionWritten) {
        QDir dir(plan->autosaveDir);
        const auto oldFiles = dir.entryInfoList(
            {"autosave_*.ndraft", "autosave_*.tmp", "tab_*.tmp"}, QDir::Files);
        for (const QFileInfo &oldFile : oldFiles) {
            if (!toKeep.contains(recoveryPathKey(oldFile.absoluteFilePath())))
                QFile::remove(oldFile.absoluteFilePath());
        }
    }
    return result;
}

bool SessionManager::asyncSaveSucceeded(
    const std::shared_ptr<AsyncSaveResult> &result) const
{
    return result && result->sessionWritten;
}

void SessionManager::applyAsyncSave(
    const std::shared_ptr<AsyncSaveResult> &result,
    const QVector<ISessionDocumentPort *> &tabs)
{
    if (!result || !result->sessionWritten)
        return;
    QHash<QUuid, ISessionDocumentPort *> tabsByInstance;
    for (ISessionDocumentPort *tab : tabs) {
        if (tab)
            tabsByInstance.insert(tab->sessionInstanceId(), tab);
    }
    for (const auto &item : result->items) {
        if (ISessionDocumentPort *tab = tabsByInstance.value(item.instanceId, nullptr))
            tab->acceptRecoveryPath(item.autosavePath);
        if (item.snapshotWritten) {
            m_snapshotRevisions.insert(item.instanceId, item.revision);
            m_snapshotTimes.insert(item.instanceId, result->capturedAtUtc);
        }
    }
    for (auto it = m_snapshotRevisions.begin(); it != m_snapshotRevisions.end();) {
        if (!result->activeInstances.contains(it.key()))
            it = m_snapshotRevisions.erase(it);
        else
            ++it;
    }
    for (auto it = m_snapshotTimes.begin(); it != m_snapshotTimes.end();) {
        if (!result->activeInstances.contains(it.key()))
            it = m_snapshotTimes.erase(it);
        else
            ++it;
    }
}

SessionData SessionManager::loadSession() const
{
    SessionData data;
    QFile f(sessionFilePath());
    if (!f.open(QIODevice::ReadOnly) || f.size() <= 0
        || f.size() > MaximumSessionFileBytes)
        return data;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return data;

    QJsonObject root = doc.object();
    data.version = root["version"].toInt(1);
    if (data.version < 1 || data.version > 2)
        return SessionData{};

    QJsonArray tabsArray = root["tabs"].toArray();
    const qsizetype tabCount = qMin<qsizetype>(tabsArray.size(), MaximumSessionTabs);
    data.tabs.reserve(tabCount);
    for (qsizetype i = 0; i < tabCount; ++i) {
        const QJsonValue &v = tabsArray.at(i);
        if (!v.isObject())
            continue;
        QJsonObject obj = v.toObject();
        SessionTab st;
        st.filePath = obj["filePath"].toString();
        st.encoding = obj["encoding"].toString();
        st.syntax = qBound(0, obj["syntax"].toInt(0),
                           SessionTab::MaximumPersistedSyntaxValue);
        st.cursorLine = qMax(0, obj["cursorLine"].toInt(0));
        st.cursorIndex = qMax(0, obj["cursorIndex"].toInt(0));
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
        if (!st.secureDocument && !isManagedAutosavePath(st.autosavePath))
            st.autosavePath.clear();
        st.splitView = obj["splitView"].toBool(false);
        st.splitOrientation = obj["splitOrientation"].toString();
        st.documentId = obj["documentId"].toString();
        st.presentation = obj["presentation"].toString();
        st.hexInsertMode = obj["hexInsertMode"].toBool(false);
        data.tabs.append(st);
    }
    data.activeTab = data.tabs.isEmpty()
        ? 0 : qBound(0, root["activeTab"].toInt(0), data.tabs.size() - 1);
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
