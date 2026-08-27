#include "SecureDocumentPersistence.h"

#include "DocumentContentPort.h"
#include "FileTierPolicy.h"
#include "KdbxFormatProbe.h"

#include <utility>

namespace {

VaultError failure(VaultErrorCode code, QString message, QString diagnostic)
{
    return {code, std::move(message), std::move(diagnostic)};
}

void scrub(QString *text)
{
    if (!text)
        return;
    if (!text->isEmpty())
        text->fill(QChar(0));
    text->clear();
}

void scrub(QByteArray *bytes)
{
    if (!bytes)
        return;
    if (!bytes->isEmpty())
        bytes->fill(0);
    bytes->clear();
}

} // namespace

bool SecureDocumentPersistence::available() const noexcept
{
    return service_ && service_->available();
}

PersistenceAdapterDescriptor SecureDocumentPersistence::descriptor() const
{
    return {QStringLiteral("secure.kdbx"), QStringLiteral("KDBX"),
            100, DocumentPersistence::SecureFile};
}

bool SecureDocumentPersistence::recognizes(const QString &path) const
{
    return KdbxFormatProbe::inspectFile(path).recognized
        || path.endsWith(QLatin1String(".nsafe"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".kdbx"), Qt::CaseInsensitive);
}

PersistenceResult SecureDocumentPersistence::save(
    IDocumentContentPort *content, const PersistenceSaveRequest &request)
{
    if (!content || request.targetPath.trimmed().isEmpty()) {
        return {PersistenceStatus::Error,
                QStringLiteral("保存目标或文档内容不可用。"), {}};
    }
    SecurePersistencePayload payload;
    VaultError error = capture(content, &payload);
    if (error.isError()) {
        payload.clear();
        return {PersistenceStatus::Error, error.userMessage, {}};
    }
    const QString title = request.displayTitle.trimmed().isEmpty()
        ? QStringLiteral("Secure Note") : request.displayTitle.trimmed();
    if (payload.state.securityClass == DocumentSecurityClass::SecureDocument
        && !payload.state.id.isNull()) {
        error = saveExisting(payload, title, request.targetPath, true);
        if (!error.isError()) {
            content->acceptSecurePersistenceCommit(
                payload.state.id, title, request.targetPath,
                backingPath(payload.state.id));
        }
    } else {
        SecureDocumentSnapshot snapshot;
        error = saveAsSecure(payload, title, request.targetPath, &snapshot);
        if (!error.isError()) {
            content->acceptSecurePersistenceCommit(
                snapshot.documentId, snapshot.title, snapshot.filePath,
                snapshot.backingPath);
        }
    }
    payload.clear();
    return error.isError()
        ? PersistenceResult{PersistenceStatus::Error, error.userMessage, {}}
        : PersistenceResult{PersistenceStatus::Success, {}, {}};
}

void SecurePersistencePayload::clear()
{
    scrub(&text);
    scrub(&bytes);
    state = {};
}

VaultError SecureDocumentPersistence::unavailableError()
{
    return failure(VaultErrorCode::BackendUnavailable,
                   QStringLiteral("安全文档服务不可用。"),
                   QStringLiteral("secure-persistence.unavailable"));
}

VaultError SecureDocumentPersistence::capture(
    IDocumentContentPort *content, SecurePersistencePayload *payload) const
{
    if (!available() || !content || !payload)
        return unavailableError();
    payload->clear();
    payload->state = content->persistenceState();
    DocumentSnapshot snapshot;
    QString snapshotError;
    if (!content->createPersistenceSnapshot(
            FileTierPolicy::StandardEditLimitBytes, &snapshot, &snapshotError)) {
        return failure(VaultErrorCode::InternalError, snapshotError,
                       QStringLiteral("secure-persistence.snapshot"));
    }
    payload->contentKind = snapshot.contentKind;
    payload->text = std::move(snapshot.text);
    payload->bytes = std::move(snapshot.bytes);
    return {};
}

VaultError SecureDocumentPersistence::saveExisting(
    const SecurePersistencePayload &payload, const QString &title,
    const QString &targetPath, bool explicitRevision)
{
    if (!available())
        return unavailableError();
    if (payload.contentKind == DocumentContentKind::Binary) {
        return service_->saveBinary(payload.state.id, payload.bytes, title,
                                    targetPath, explicitRevision);
    }
    return service_->save(payload.state.id, payload.text, title,
                          targetPath, explicitRevision);
}

VaultError SecureDocumentPersistence::saveAsSecure(
    const SecurePersistencePayload &payload, const QString &title,
    const QString &targetPath, SecureDocumentSnapshot *snapshot)
{
    if (!available() || !snapshot)
        return unavailableError();
    if (payload.contentKind == DocumentContentKind::Binary) {
        return service_->saveAsSecureBinary(payload.bytes, title,
                                            targetPath, snapshot);
    }
    return service_->saveAsSecure(payload.text, title, targetPath, snapshot);
}

VaultError SecureDocumentPersistence::checkpoint(
    const SecurePersistencePayload &payload)
{
    if (!available())
        return unavailableError();
    if (payload.state.securityClass != DocumentSecurityClass::SecureDocument
        || payload.state.id.isNull()) {
        return failure(VaultErrorCode::InternalError,
                       QStringLiteral("文档当前不可恢复。"),
                       QStringLiteral("secure-persistence.checkpoint"));
    }
    if (payload.contentKind == DocumentContentKind::Binary) {
        return service_->checkpointBinary(payload.state.id, payload.bytes);
    }
    return service_->checkpointText(payload.state.id, payload.text);
}

VaultError SecureDocumentPersistence::newDraft(SecureDocumentSnapshot *snapshot)
{
    return available() ? service_->newDraft(snapshot) : unavailableError();
}

VaultError SecureDocumentPersistence::open(const QString &path,
                                           SecureDocumentSnapshot *snapshot)
{
    return available() ? service_->open(path, snapshot) : unavailableError();
}

VaultError SecureDocumentPersistence::openWithPassphrase(
    const QString &path, const SecureBuffer &passphrase,
    SecureDocumentSnapshot *snapshot)
{
    return available() ? service_->openWithPassphrase(path, passphrase, snapshot)
                       : unavailableError();
}

VaultError SecureDocumentPersistence::openWithRecovery(
    const QString &path, const QString &recoveryKey,
    SecureDocumentSnapshot *snapshot)
{
    return available() ? service_->openWithRecovery(path, recoveryKey, snapshot)
                       : unavailableError();
}

VaultError SecureDocumentPersistence::reload(
    const QUuid &documentId, SecureDocumentSnapshot *snapshot)
{
    return available() ? service_->reload(documentId, snapshot)
                       : unavailableError();
}

VaultError SecureDocumentPersistence::inspectFile(
    const QString &path, QString *documentType, QUuid *documentId,
    SecurityIdentityParameters *parameters) const
{
    return available()
        ? service_->inspectFile(path, documentType, documentId, parameters)
        : unavailableError();
}

DiskFileState SecureDocumentPersistence::externalFileState(
    const QUuid &documentId) const
{
    return available() ? service_->externalFileState(documentId)
                       : DiskFileState::Unreadable;
}

QString SecureDocumentPersistence::backingPath(const QUuid &documentId) const
{
    return available() ? service_->backingPath(documentId) : QString{};
}

void SecureDocumentPersistence::close(const QUuid &documentId) noexcept
{
    if (service_)
        service_->close(documentId);
}

void SecureDocumentPersistence::closeAll() noexcept
{
    if (service_)
        service_->closeAll();
}
