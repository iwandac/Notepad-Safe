#include "SecureDocumentService.h"

#include "SecurityIdentity.h"
#include "VaultBackendFactory.h"
#include "VaultService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSaveFile>

#include <algorithm>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

VaultError documentError(VaultErrorCode code, const QString &message,
                         const QString &diagnostic)
{
    return {code, message, diagnostic};
}

void scrubByteArray(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
#ifdef Q_OS_WIN
        ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
        volatile char *cursor = bytes.data();
        for (qsizetype i = 0; i < bytes.size(); ++i)
            cursor[i] = 0;
#endif
    }
    bytes.clear();
}

QString defaultDataDirectory()
{
    const QString override = QProcessEnvironment::systemEnvironment()
                                 .value(QStringLiteral("NOTEPADSAFE_DATADIR")).trimmed();
    if (!override.isEmpty() && QFileInfo(override).isAbsolute())
        return QDir::cleanPath(override);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("NotepadSafe"));
}

QString textFrom(const SecureBuffer &buffer)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

QByteArray bytesFrom(const SecureBuffer &buffer)
{
    return QByteArray(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

bool atomicCopy(const QString &sourcePath, const QString &destinationPath, QString *detail)
{
    QFile source(sourcePath);
    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        if (detail)
            *detail = source.isOpen() ? destination.errorString() : source.errorString();
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0 || destination.write(buffer.constData(), count) != count) {
            if (detail)
                *detail = count < 0 ? source.errorString() : destination.errorString();
            destination.cancelWriting();
            return false;
        }
    }
    if (!destination.commit()) {
        if (detail)
            *detail = destination.errorString();
        return false;
    }
    return true;
}

} // namespace

SecureDocumentService::SecureDocumentService(SecuritySessionService *securitySession,
                                             const QString &dataDirectory)
    : securitySession_(securitySession)
    , dataDirectory_(dataDirectory.trimmed().isEmpty() ? defaultDataDirectory()
                                                       : QDir::cleanPath(dataDirectory))
{
    QDir().mkpath(QDir(dataDirectory_).filePath(QStringLiteral("drafts")));
}

SecureDocumentService::~SecureDocumentService()
{
    closeAll();
}

VaultError SecureDocumentService::validatePayload(const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() >= PayloadMaximumExclusive) {
        scrubByteArray(utf8);
        return documentError(VaultErrorCode::UnsupportedFormat,
                             QStringLiteral("安全文档必须小于 32 MiB。请另存为普通文本或二进制文件。"),
                             QStringLiteral("secure-document.payload.limit"));
    }
    scrubByteArray(utf8);
    return VaultError::success();
}

VaultError SecureDocumentService::validatePayload(const QByteArray &binary)
{
    if (binary.size() >= PayloadMaximumExclusive) {
        return documentError(VaultErrorCode::UnsupportedFormat,
                             QStringLiteral("安全文档必须小于 32 MiB。请另存为普通文件。"),
                             QStringLiteral("secure-document.binary-payload.limit"));
    }
    return VaultError::success();
}

QString SecureDocumentService::draftPath(const QUuid &documentId) const
{
    return QDir(dataDirectory_).filePath(
        QStringLiteral("drafts/%1.nsafe-draft")
            .arg(documentId.toString(QUuid::WithoutBraces)));
}

SecureDocumentService::Handle *SecureDocumentService::find(const QUuid &documentId) const
{
    for (const auto &handle : handles_) {
        if (handle->documentId == documentId)
            return handle.get();
    }
    return nullptr;
}

VaultError SecureDocumentService::createHandle(const QString &path, bool draft,
                                               Handle **output)
{
    if (!output || !securitySession_ || !securitySession_->hasActiveSession()) {
        return documentError(VaultErrorCode::Locked,
                             QStringLiteral("请先登录本地安全身份。"),
                             QStringLiteral("secure-document.session.required"));
    }
    auto handle = std::make_unique<Handle>();
    SecurityOperationLease lease;
    VaultError error = securitySession_->acquireOperationLease(&lease);
    if (error.isError())
        return error;
    SecureBuffer databaseCredential;
    error = SecurityIdentity::createForIdentity(
        securitySession_->parameters(), lease.masterUnlockKey(),
        lease.recoverySecret(), &databaseCredential,
        &handle->publicCustomData, QStringLiteral("SecureDocument"));
    if (error.isError())
        return error;
    error = securitySession_->sealDocumentCredential(
        databaseCredential, &handle->sealedDatabaseCredential);
    databaseCredential.clear();
    lease.clear();
    if (error.isError())
        return error;
    SecurityIdentityParameters parameters;
    error = SecurityIdentity::describe(handle->publicCustomData, &parameters, nullptr,
                                       &handle->documentId);
    if (error.isError())
        return error;
    handle->entryId = QUuid::createUuid();
    handle->filePath = path;
    handle->draft = draft;
    Handle *raw = handle.get();
    handles_.push_back(std::move(handle));
    *output = raw;
    return VaultError::success();
}

VaultError SecureDocumentService::newDraft(SecureDocumentSnapshot *snapshot)
{
    if (!snapshot)
        return documentError(VaultErrorCode::InternalError, QStringLiteral("新建文档参数无效。"),
                             QStringLiteral("secure-document.new.arguments"));
    Handle *handle = nullptr;
    VaultError error = createHandle({}, true, &handle);
    if (error.isError())
        return error;
    handle->filePath = draftPath(handle->documentId);
    snapshot->documentId = handle->documentId;
    snapshot->filePath.clear();
    snapshot->backingPath = handle->filePath;
    snapshot->title = QStringLiteral("未命名");
    snapshot->text.clear();
    snapshot->binary.clear();
    snapshot->payloadKind = SecureDocumentPayloadKind::Text;
    snapshot->draft = true;
    return VaultError::success();
}

VaultError SecureDocumentService::inspect(const QString &filePath,
                                          QString *documentType,
                                          QUuid *documentId,
                                          SecurityIdentityParameters *parametersOutput)
{
    if (!documentType)
        return documentError(VaultErrorCode::InternalError, QStringLiteral("文件探测参数无效。"),
                             QStringLiteral("secure-document.inspect.arguments"));
    VaultService service(createProductionVaultBackend());
    QVariantMap publicData;
    VaultError error = service.readPublicCustomData(filePath, &publicData);
    if (error.isError())
        return error;
    SecurityIdentityParameters parameters;
    error = SecurityIdentity::describe(publicData, &parameters, documentType, documentId);
    if (!error.isError() && parametersOutput)
        *parametersOutput = parameters;
    return error;
}

VaultError SecureDocumentService::open(const QString &filePath,
                                       SecureDocumentSnapshot *snapshot)
{
    if (!snapshot || !securitySession_ || !securitySession_->hasActiveSession()) {
        return documentError(VaultErrorCode::Locked,
                             QStringLiteral("请先登录本地安全身份。"),
                             QStringLiteral("secure-document.session.required"));
    }
    VaultService service(createProductionVaultBackend());
    QVariantMap publicData;
    VaultError error = service.readPublicCustomData(filePath, &publicData);
    if (error.isError())
        return error;
    SecurityIdentityParameters parameters;
    QString documentType;
    error = SecurityIdentity::describe(publicData, &parameters, &documentType);
    if (error.isError())
        return error;
    if (documentType != QStringLiteral("SecureDocument")) {
        return documentError(VaultErrorCode::UnsupportedFormat,
                             QStringLiteral("此 KDBX 文件不是安全文档。"),
                             QStringLiteral("secure-document.type.%1").arg(documentType));
    }
    SecureBuffer credential;
    if (securitySession_->matches(parameters)) {
        SecurityOperationLease lease;
        error = securitySession_->acquireOperationLease(&lease);
        if (!error.isError()) {
            error = SecurityIdentity::unlockWithMasterKey(
                publicData, lease.masterUnlockKey(), &credential);
        }
        lease.clear();
    } else {
        SecureBuffer rememberedMaster;
        error = securitySession_->acquireRememberedMasterKey(
            parameters, &rememberedMaster);
        if (!error.isError()) {
            error = SecurityIdentity::unlockWithMasterKey(
                publicData, rememberedMaster, &credential);
        }
        rememberedMaster.clear();
        if (error.isError()) {
            SecureBuffer rememberedRecovery;
            VaultError recoveryError = securitySession_->acquireRememberedRecoverySecret(
                parameters, &rememberedRecovery);
            if (!recoveryError.isError()) {
                recoveryError = SecurityIdentity::recoverWithSecret(
                    publicData, rememberedRecovery, &credential);
            }
            rememberedRecovery.clear();
            error = recoveryError;
        }
    }
    if (error.isError())
        return documentError(VaultErrorCode::InvalidCredentials,
                             QStringLiteral("此文件由另一主密码或历史密码保护。"),
                             QStringLiteral("secure-document.foreign-identity"));
    return openWithCredential(filePath, publicData, std::move(credential), snapshot);
}

VaultError SecureDocumentService::openWithPassphrase(
    const QString &filePath, const SecureBuffer &passphraseUtf8,
    SecureDocumentSnapshot *snapshot)
{
    VaultService service(createProductionVaultBackend());
    QVariantMap publicData;
    VaultError error = service.readPublicCustomData(filePath, &publicData);
    SecurityIdentityParameters parameters;
    SecureBuffer credential;
    if (!error.isError())
        error = SecurityIdentity::describe(publicData, &parameters);
    SecureBuffer masterUnlockKey;
    if (!error.isError()) {
        error = SecurityIdentity::deriveMasterUnlockKey(
            passphraseUtf8, parameters, &masterUnlockKey);
    }
    if (!error.isError()) {
        error = SecurityIdentity::unlockWithMasterKey(
            publicData, masterUnlockKey, &credential);
    }
    if (!error.isError())
        error = securitySession_->rememberMasterKey(parameters, masterUnlockKey);
    masterUnlockKey.clear();
    if (error.isError())
        return error;
    return openWithCredential(filePath, publicData, std::move(credential), snapshot);
}

VaultError SecureDocumentService::openWithRecovery(
    const QString &filePath, const QString &recoveryKey,
    SecureDocumentSnapshot *snapshot)
{
    VaultService service(createProductionVaultBackend());
    QVariantMap publicData;
    VaultError error = service.readPublicCustomData(filePath, &publicData);
    SecurityIdentityParameters parameters;
    SecureBuffer credential;
    if (!error.isError())
        error = SecurityIdentity::describe(publicData, &parameters);
    SecureBuffer recoverySecret;
    if (!error.isError())
        error = SecurityIdentity::parseRecoveryKey(recoveryKey, &recoverySecret);
    if (!error.isError()) {
        error = SecurityIdentity::recoverWithSecret(
            publicData, recoverySecret, &credential);
    }
    if (!error.isError()) {
        error = securitySession_->rememberRecoverySecret(
            parameters, recoverySecret);
    }
    recoverySecret.clear();
    if (error.isError())
        return error;
    return openWithCredential(filePath, publicData, std::move(credential), snapshot);
}

VaultError SecureDocumentService::openWithCredential(
    const QString &filePath, QVariantMap publicCustomData,
    SecureBuffer &&databaseCredential, SecureDocumentSnapshot *snapshot)
{
    VaultService service(createProductionVaultBackend());
    VaultError error = service.unlock(filePath, databaseCredential);
    if (error.isError())
        return error;
    VaultError listError;
    const QList<VaultEntrySummary> entries = service.listEntries(&listError);
    if (listError.isError())
        return listError;
    auto it = std::find_if(entries.cbegin(), entries.cend(), [](const VaultEntrySummary &entry) {
        return entry.secureNote || entry.tags.contains(VaultSecureNoteTag);
    });
    if (it == entries.cend()) {
        return documentError(VaultErrorCode::CorruptDatabase,
                             QStringLiteral("安全文档缺少正文记录。"),
                             QStringLiteral("secure-document.payload.missing"));
    }
    VaultError readError;
    auto entry = service.readEntry(it->id, &readError);
    if (!entry)
        return readError;
    const SecureDocumentPayloadKind payloadKind = entry->binaryPayloadPresent
        ? SecureDocumentPayloadKind::Binary : SecureDocumentPayloadKind::Text;
    QString text;
    QByteArray binary;
    if (payloadKind == SecureDocumentPayloadKind::Binary) {
        binary = bytesFrom(entry->binaryPayload);
        entry->binaryPayload.clear();
        entry->notesUtf8.clear();
        error = validatePayload(binary);
    } else {
        text = entry->textPayloadPresent
            ? textFrom(entry->textPayloadUtf8) : textFrom(entry->notesUtf8);
        entry->textPayloadUtf8.clear();
        entry->notesUtf8.clear();
        error = validatePayload(text);
    }
    if (error.isError()) {
        text.fill(QChar(0));
        scrubByteArray(binary);
        return error;
    }
    auto handle = std::make_unique<Handle>();
    SecurityIdentityParameters parameters;
    error = SecurityIdentity::describe(publicCustomData, &parameters, nullptr,
                                       &handle->documentId);
    if (error.isError()) {
        text.fill(QChar(0));
        scrubByteArray(binary);
        return error;
    }
    if (find(handle->documentId)) {
        text.fill(QChar(0));
        scrubByteArray(binary);
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("此安全文档已经打开。"),
                             QStringLiteral("secure-document.already-open"));
    }
    handle->entryId = entry->id;
    handle->filePath = QFileInfo(filePath).absoluteFilePath();
    handle->title = entry->title;
    handle->publicCustomData = std::move(publicCustomData);
    error = securitySession_->sealDocumentCredential(
        databaseCredential, &handle->sealedDatabaseCredential);
    databaseCredential.clear();
    if (error.isError()) {
        text.fill(QChar(0));
        scrubByteArray(binary);
        return error;
    }
    handle->persisted = true;
    handle->draft = handle->filePath.endsWith(QStringLiteral(".nsafe-draft"),
                                               Qt::CaseInsensitive);
    handle->payloadKind = payloadKind;
    service.lock();
    captureDiskFileVersion(handle->filePath, &handle->diskVersion);
    snapshot->documentId = handle->documentId;
    snapshot->filePath = handle->draft ? QString() : handle->filePath;
    snapshot->backingPath = handle->filePath;
    snapshot->title = handle->title;
    snapshot->payloadKind = payloadKind;
    snapshot->text = std::move(text);
    snapshot->binary = std::move(binary);
    snapshot->draft = handle->draft;
    handles_.push_back(std::move(handle));
    return VaultError::success();
}

VaultError SecureDocumentService::reload(const QUuid &documentId,
                                         SecureDocumentSnapshot *snapshot)
{
    Handle *handle = find(documentId);
    if (!handle || !snapshot || handle->filePath.isEmpty()) {
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("安全文档重新加载参数无效。"),
                             QStringLiteral("secure-document.reload.arguments"));
    }

    VaultService service(createProductionVaultBackend());
    QVariantMap publicData;
    VaultError error = service.readPublicCustomData(handle->filePath, &publicData);
    if (error.isError())
        return error;
    SecurityIdentityParameters parameters;
    QUuid diskDocumentId;
    QString documentType;
    error = SecurityIdentity::describe(publicData, &parameters, &documentType,
                                       &diskDocumentId);
    if (error.isError())
        return error;
    if (documentType != QStringLiteral("SecureDocument")
        || diskDocumentId != handle->documentId) {
        return documentError(VaultErrorCode::UnsupportedFormat,
                             QStringLiteral("磁盘文件已不再是同一份安全文档。"),
                             QStringLiteral("secure-document.reload.identity-mismatch"));
    }
    SecureBuffer databaseCredential;
    error = securitySession_->unsealDocumentCredential(
        handle->sealedDatabaseCredential, &databaseCredential);
    if (!error.isError())
        error = service.unlock(handle->filePath, databaseCredential);
    if (error.isError())
        return error;

    VaultError listError;
    const QList<VaultEntrySummary> entries = service.listEntries(&listError);
    if (listError.isError())
        return listError;
    const auto it = std::find_if(entries.cbegin(), entries.cend(),
                                 [](const VaultEntrySummary &entry) {
        return entry.secureNote || entry.tags.contains(VaultSecureNoteTag);
    });
    if (it == entries.cend()) {
        return documentError(VaultErrorCode::CorruptDatabase,
                             QStringLiteral("安全文档缺少正文记录。"),
                             QStringLiteral("secure-document.payload.missing"));
    }
    VaultError readError;
    auto entry = service.readEntry(it->id, &readError);
    if (!entry)
        return readError;
    const SecureDocumentPayloadKind payloadKind = entry->binaryPayloadPresent
        ? SecureDocumentPayloadKind::Binary : SecureDocumentPayloadKind::Text;
    QString text;
    QByteArray binary;
    if (payloadKind == SecureDocumentPayloadKind::Binary) {
        binary = bytesFrom(entry->binaryPayload);
        entry->binaryPayload.clear();
        entry->notesUtf8.clear();
        error = validatePayload(binary);
    } else {
        text = entry->textPayloadPresent
            ? textFrom(entry->textPayloadUtf8) : textFrom(entry->notesUtf8);
        entry->textPayloadUtf8.clear();
        entry->notesUtf8.clear();
        error = validatePayload(text);
    }
    if (error.isError()) {
        text.fill(QChar(0));
        scrubByteArray(binary);
        return error;
    }

    handle->entryId = entry->id;
    handle->title = entry->title;
    handle->publicCustomData = std::move(publicData);
    handle->persisted = true;
    handle->draft = handle->filePath.endsWith(QStringLiteral(".nsafe-draft"),
                                              Qt::CaseInsensitive);
    handle->payloadKind = payloadKind;
    handle->diskVersion = {};
    service.lock();
    captureDiskFileVersion(handle->filePath, &handle->diskVersion);

    snapshot->documentId = handle->documentId;
    snapshot->filePath = handle->draft ? QString() : handle->filePath;
    snapshot->backingPath = handle->filePath;
    snapshot->title = handle->title;
    snapshot->payloadKind = payloadKind;
    snapshot->text = std::move(text);
    snapshot->binary = std::move(binary);
    snapshot->draft = handle->draft;
    return VaultError::success();
}

VaultError SecureDocumentService::persist(Handle *handle,
                                          SecureDocumentPayloadKind payloadKind,
                                          const QString &text,
                                          const QByteArray &binary,
                                          const QString &title,
                                          const QString &targetPath)
{
    VaultError error = payloadKind == SecureDocumentPayloadKind::Binary
                           ? validatePayload(binary) : validatePayload(text);
    if (error.isError())
        return error;
    const QString destination = targetPath.isEmpty() ? handle->filePath
                                                      : QFileInfo(targetPath).absoluteFilePath();
    if (destination.isEmpty()) {
        return documentError(VaultErrorCode::IoError,
                             QStringLiteral("安全文档没有保存路径。"),
                             QStringLiteral("secure-document.path.empty"));
    }
    const QString oldPath = handle->filePath;
    const bool sameDestination = handle->persisted
                                 && sameDiskFilePath(oldPath, destination);
    if (sameDestination) {
        const DiskFileState state = compareDiskFileVersion(destination,
                                                           handle->diskVersion);
        if (state != DiskFileState::Unchanged) {
            QString suffix = QStringLiteral("unreadable");
            QString message = QStringLiteral(
                "无法确认磁盘上的安全文档是否保持不变。为避免覆盖外部内容，保存已停止。");
            if (state == DiskFileState::Changed) {
                suffix = QStringLiteral("changed");
                message = QStringLiteral(
                    "磁盘上的安全文档在打开后已被修改。为避免覆盖外部内容，保存已停止。");
            } else if (state == DiskFileState::Missing) {
                suffix = QStringLiteral("missing");
                message = QStringLiteral(
                    "磁盘上的安全文档已被移动或删除。为避免意外重建原路径，保存已停止。");
            }
            return documentError(VaultErrorCode::IoError, message,
                                 QStringLiteral("secure-document.external-change.") + suffix);
        }
    }
    const bool replacingOtherFile = QFileInfo::exists(destination) && !sameDestination;
    QString writePath = destination;
    if (replacingOtherFile) {
        writePath = QFileInfo(destination).dir().filePath(
            QStringLiteral(".%1.%2.nsafe-new")
                .arg(QFileInfo(destination).fileName(),
                     QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }
    SecureBuffer databaseCredential;
    error = securitySession_->unsealDocumentCredential(
        handle->sealedDatabaseCredential, &databaseCredential);
    if (error.isError())
        return error;
    VaultService service(createProductionVaultBackend());
    const bool create = !sameDestination;
    const bool createdFresh = create && !QFileInfo::exists(writePath);
    if (create) {
        error = service.create(writePath, databaseCredential, {},
                               handle->publicCustomData);
    } else {
        error = service.unlock(writePath, databaseCredential);
    }
    if (error.isError())
        return error;
    VaultEntryData entry;
    entry.recordHistory = false;
    entry.id = handle->entryId;
    entry.title = title.trimmed().isEmpty() ? QStringLiteral("未命名") : title.trimmed();
    entry.tags = {VaultSecureNoteTag};
    if (payloadKind == SecureDocumentPayloadKind::Binary) {
        entry.binaryPayload.assign(binary.constData(), binary.size());
        entry.binaryPayloadPresent = true;
    } else {
        QByteArray utf8 = text.toUtf8();
        entry.notesUtf8.assign(utf8.constData(), utf8.size());
        entry.textPayloadUtf8.assign(utf8.constData(), utf8.size());
        entry.textPayloadPresent = true;
        scrubByteArray(utf8);
    }
    error = service.upsertEntry(std::move(entry));
    if (!error.isError())
        error = service.save();
    if (error.isError()) {
        if (replacingOtherFile || createdFresh)
            QFile::remove(writePath);
        return error;
    }
    if (replacingOtherFile) {
        VaultService candidateVerification(createProductionVaultBackend());
        error = candidateVerification.unlock(writePath, databaseCredential);
        if (error.isError()) {
            QFile::remove(writePath);
            return error;
        }
        QString copyError;
        if (!atomicCopy(writePath, destination, &copyError)) {
            QFile::remove(writePath);
            return documentError(VaultErrorCode::IoError,
                                 QStringLiteral("无法原子替换目标安全文档。"),
                                 QStringLiteral("secure-document.replace: ") + copyError);
        }
        QFile::remove(writePath);
        VaultService verification(createProductionVaultBackend());
        error = verification.unlock(destination, databaseCredential);
        if (error.isError())
            return error;
    }
    handle->filePath = destination;
    handle->title = title;
    handle->persisted = true;
    handle->draft = destination.endsWith(QStringLiteral(".nsafe-draft"),
                                         Qt::CaseInsensitive);
    handle->payloadKind = payloadKind;
    handle->diskVersion = {};
    service.lock();
    captureDiskFileVersion(destination, &handle->diskVersion);
    if (!oldPath.isEmpty() && oldPath != destination
        && oldPath.endsWith(QStringLiteral(".nsafe-draft"), Qt::CaseInsensitive)) {
        QFile::remove(oldPath);
    }
    return VaultError::success();
}

VaultError SecureDocumentService::save(const QUuid &documentId, const QString &text,
                                       const QString &title, const QString &targetPath)
{
    Handle *handle = find(documentId);
    if (!handle) {
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("安全文档句柄不存在。"),
                             QStringLiteral("secure-document.handle.missing"));
    }
    return persist(handle, SecureDocumentPayloadKind::Text, text, {}, title, targetPath);
}

VaultError SecureDocumentService::saveBinary(const QUuid &documentId,
                                             const QByteArray &binary,
                                             const QString &title,
                                             const QString &targetPath)
{
    Handle *handle = find(documentId);
    if (!handle) {
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("安全文档句柄不存在。"),
                             QStringLiteral("secure-document.handle.missing"));
    }
    return persist(handle, SecureDocumentPayloadKind::Binary, {}, binary,
                   title, targetPath);
}

VaultError SecureDocumentService::saveAsSecure(
    const QString &text, const QString &title, const QString &targetPath,
    SecureDocumentSnapshot *snapshot)
{
    if (!snapshot || targetPath.isEmpty()) {
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("另存为安全文档参数无效。"),
                             QStringLiteral("secure-document.save-as.arguments"));
    }
    Handle *handle = nullptr;
    VaultError error = createHandle(QFileInfo(targetPath).absoluteFilePath(), false, &handle);
    if (!error.isError())
    {
        error = persist(handle, SecureDocumentPayloadKind::Text, text, {},
                        title, targetPath);
    }
    if (error.isError()) {
        if (handle)
            close(handle->documentId);
        return error;
    }
    snapshot->documentId = handle->documentId;
    snapshot->filePath = handle->filePath;
    snapshot->backingPath = handle->filePath;
    snapshot->title = handle->title;
    snapshot->text = text;
    snapshot->binary.clear();
    snapshot->payloadKind = SecureDocumentPayloadKind::Text;
    snapshot->draft = false;
    return VaultError::success();
}

VaultError SecureDocumentService::saveAsSecureBinary(
    const QByteArray &binary, const QString &title, const QString &targetPath,
    SecureDocumentSnapshot *snapshot)
{
    if (!snapshot || targetPath.isEmpty()) {
        return documentError(VaultErrorCode::InternalError,
                             QStringLiteral("另存为安全文档参数无效。"),
                             QStringLiteral("secure-document.save-as.arguments"));
    }
    Handle *handle = nullptr;
    VaultError error = createHandle(QFileInfo(targetPath).absoluteFilePath(), false,
                                    &handle);
    if (!error.isError()) {
        error = persist(handle, SecureDocumentPayloadKind::Binary, {}, binary,
                        title, targetPath);
    }
    if (error.isError()) {
        if (handle)
            close(handle->documentId);
        return error;
    }
    snapshot->documentId = handle->documentId;
    snapshot->filePath = handle->filePath;
    snapshot->backingPath = handle->filePath;
    snapshot->title = handle->title;
    snapshot->text.clear();
    snapshot->binary = binary;
    snapshot->payloadKind = SecureDocumentPayloadKind::Binary;
    snapshot->draft = false;
    return VaultError::success();
}

void SecureDocumentService::close(const QUuid &documentId) noexcept
{
    handles_.erase(std::remove_if(handles_.begin(), handles_.end(),
                                  [&documentId](const std::unique_ptr<Handle> &handle) {
                                      return handle->documentId == documentId;
                                  }),
                   handles_.end());
}

void SecureDocumentService::closeAll() noexcept
{
    handles_.clear();
}

QString SecureDocumentService::backingPath(const QUuid &documentId) const
{
    const Handle *handle = find(documentId);
    return handle ? handle->filePath : QString();
}

DiskFileState SecureDocumentService::externalFileState(const QUuid &documentId) const
{
    const Handle *handle = find(documentId);
    if (!handle || !handle->persisted || handle->filePath.isEmpty())
        return DiskFileState::NotTracked;
    return compareDiskFileVersion(handle->filePath, handle->diskVersion);
}
