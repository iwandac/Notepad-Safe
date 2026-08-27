#include "PlainFilePersistence.h"

#include "DocumentContentPort.h"
#include "FileTierPolicy.h"

#include <QFileInfo>
#include <QSaveFile>

#include <utility>

PersistenceAdapterDescriptor PlainFilePersistence::descriptor() const
{
    return {QStringLiteral("plain.file"), QStringLiteral("Plain file"),
            0, DocumentPersistence::PlainFile};
}

bool PlainFilePersistence::recognizes(const QString &path) const
{
    return !path.endsWith(QLatin1String(".nsafe"), Qt::CaseInsensitive)
        && !path.endsWith(QLatin1String(".kdbx"), Qt::CaseInsensitive);
}

PersistenceResult PlainFilePersistence::save(
    IDocumentContentPort *content, const PersistenceSaveRequest &request)
{
    if (!content || request.targetPath.trimmed().isEmpty()) {
        return {PersistenceStatus::Error,
                QStringLiteral("保存目标或文档内容不可用。"), {}};
    }
    if (!content->persistenceState().capabilities.testFlag(
            DocumentCapability::Save)) {
        return {PersistenceStatus::Unsupported,
                QStringLiteral("当前文档为只读分析模式，不能保存或另存。"), {}};
    }
    const QString absolutePath = QFileInfo(request.targetPath).absoluteFilePath();
    if (request.requireExpectedVersion) {
        const DiskFileState state = compareDiskFileVersion(
            absolutePath, request.expectedVersion);
        if (state != DiskFileState::Unchanged) {
            return {PersistenceStatus::Conflict,
                    QStringLiteral("磁盘文件已变化，保存已停止。"), {}};
        }
    }

    DocumentSnapshot snapshot;
    QString snapshotError;
    if (!content->createPersistenceSnapshot(
            FileTierPolicy::StandardEditLimitBytes, &snapshot,
            &snapshotError)) {
        return {PersistenceStatus::Error, snapshotError, {}};
    }

    QByteArray payload;
    if (snapshot.contentKind == DocumentContentKind::Text) {
        bool representable = true;
        payload = encodeTextFragment(snapshot.text,
                                     content->persistenceEncoding(),
                                     &representable);
        if (!snapshot.text.isEmpty())
            snapshot.text.fill(QChar(0));
        snapshot.text.clear();
        if (!representable) {
            return {PersistenceStatus::Error,
                    QStringLiteral("文本包含 %1 无法表示的字符。")
                        .arg(encodingName(content->persistenceEncoding())), {}};
        }
        switch (content->persistenceEncoding()) {
        case TextEncoding::Utf8Bom: payload.prepend("\xef\xbb\xbf", 3); break;
        case TextEncoding::Utf16LE: payload.prepend("\xff\xfe", 2); break;
        case TextEncoding::Utf16BE: payload.prepend("\xfe\xff", 2); break;
        default: break;
        }
    } else {
        payload = std::move(snapshot.bytes);
    }

    const auto scrubPayload = [&payload]() {
        if (!payload.isEmpty())
            payload.fill(0);
        payload.clear();
    };
    QSaveFile output(absolutePath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(payload) != payload.size()) {
        const QString error = output.errorString();
        output.cancelWriting();
        scrubPayload();
        return {PersistenceStatus::Error,
                error.isEmpty()
                    ? QStringLiteral("无法原子写入目标文件。") : error,
                {}};
    }
    scrubPayload();

    const QString originalPath = content->persistenceState().location;
    QString backingError;
    if (!content->suspendPersistenceBacking(&backingError)) {
        output.cancelWriting();
        return {PersistenceStatus::Error,
                backingError.isEmpty()
                    ? QStringLiteral("无法释放文档的底层文件。") : backingError,
                {}};
    }
    if (!output.commit()) {
        const QString commitError = output.errorString();
        QString restoreError;
        content->resumePersistenceBacking(originalPath, &restoreError);
        return {PersistenceStatus::Error,
                restoreError.isEmpty()
                    ? QStringLiteral("无法提交目标文件：%1").arg(commitError)
                    : QStringLiteral("无法提交目标文件：%1；原文件重新打开失败：%2")
                          .arg(commitError, restoreError),
                {}};
    }

    DiskFileVersion committed;
    const bool verified = captureDiskFileVersion(absolutePath, &committed)
        == DiskFileState::Unchanged;
    content->acceptPersistenceCommit(absolutePath, committed);
    QString reopenError;
    if (!content->resumePersistenceBacking(absolutePath, &reopenError)) {
        return {PersistenceStatus::Error,
                QStringLiteral("文件已保存，但重新打开底层文件失败：%1")
                    .arg(reopenError), committed};
    }
    if (!verified) {
        return {PersistenceStatus::Error,
                QStringLiteral("文件已写入，但无法验证提交结果。"), {}};
    }
    return {PersistenceStatus::Success, {}, std::move(committed)};
}
