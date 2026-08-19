#include "VaultService.h"

#include <utility>

VaultService::VaultService(std::unique_ptr<IVaultBackend> backend)
    : backend_(std::move(backend))
{
}

VaultService::~VaultService()
{
    lock();
}

VaultError VaultService::create(const QString &filePath,
                                const SecureBuffer &masterPasswordUtf8,
                                const QString &keyFilePath,
                                const QVariantMap &publicCustomData)
{
    lock();
    if (!backend_) {
        state_ = VaultState::Faulted;
        return {VaultErrorCode::BackendUnavailable,
                QStringLiteral("加密数据后端尚未安装。"),
                QStringLiteral("vault.backend.unavailable")};
    }

    state_ = VaultState::Unlocking;
    VaultError result = backend_->create(filePath, masterPasswordUtf8, keyFilePath,
                                         publicCustomData);
    if (result.isError()) {
        backend_->lock();
        state_ = VaultState::Locked;
        filePath_.clear();
        return result;
    }

    filePath_ = filePath;
    state_ = VaultState::Open;
    diskVersion_ = {};
    captureDiskFileVersion(filePath_, &diskVersion_);
    return VaultError::success();
}

VaultError VaultService::readPublicCustomData(const QString &filePath,
                                              QVariantMap *publicCustomData) const
{
    if (!backend_) {
        return {VaultErrorCode::BackendUnavailable,
                QStringLiteral("加密数据后端尚未安装。"),
                QStringLiteral("vault.backend.unavailable")};
    }
    return backend_->readPublicCustomData(filePath, publicCustomData);
}

VaultError VaultService::unlock(const QString &filePath,
                                const SecureBuffer &masterPasswordUtf8,
                                const QString &keyFilePath)
{
    lock();
    if (!backend_) {
        state_ = VaultState::Faulted;
        return {VaultErrorCode::BackendUnavailable,
                QStringLiteral("加密数据后端尚未安装。"),
                QStringLiteral("vault.backend.unavailable")};
    }

    state_ = VaultState::Unlocking;
    VaultError result = backend_->unlock(filePath, masterPasswordUtf8, keyFilePath);
    if (result.isError()) {
        backend_->lock();
        state_ = VaultState::Locked;
        filePath_.clear();
        return result;
    }

    filePath_ = filePath;
    state_ = VaultState::Open;
    diskVersion_ = {};
    captureDiskFileVersion(filePath_, &diskVersion_);
    return VaultError::success();
}

void VaultService::lock() noexcept
{
    if (backend_)
        backend_->lock();
    filePath_.clear();
    diskVersion_ = {};
    state_ = VaultState::Locked;
}

QList<VaultEntrySummary> VaultService::listEntries(VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error)
            *error = lockedError();
        return {};
    }
    return backend_->listEntries(error);
}

QList<VaultEntrySummary> VaultService::listRecycledEntries(VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error)
            *error = lockedError();
        return {};
    }
    return backend_->listRecycledEntries(error);
}

QSet<QUuid> VaultService::searchEntryIds(const QString &query, bool includeRecycled,
                                         VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error)
            *error = lockedError();
        return {};
    }
    return backend_->searchEntryIds(query, includeRecycled, error);
}

std::unique_ptr<VaultEntryData> VaultService::readEntry(const QUuid &id,
                                                        VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error)
            *error = lockedError();
        return {};
    }
    return backend_->readEntry(id, error);
}

std::unique_ptr<VaultEntryData> VaultService::readEntryMetadata(
    const QUuid &id, VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error)
            *error = lockedError();
        return {};
    }
    return backend_->readEntryMetadata(id, error);
}

VaultError VaultService::readEntrySecrets(const QUuid &id,
                                          VaultEntrySecrets *secrets) const
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->readEntrySecrets(id, secrets);
}

VaultError VaultService::currentTotp(const QUuid &id, VaultTotpSnapshot *snapshot) const
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->currentTotp(id, snapshot);
}

VaultError VaultService::generatePassword(int length, SecureBuffer *passwordUtf8) const
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->generatePassword(length, passwordUtf8);
}

VaultError VaultService::generatePassword(
    const VaultPasswordGenerationOptions &options, SecureBuffer *passwordUtf8) const
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->generatePassword(options, passwordUtf8);
}

VaultError VaultService::assessPassword(const SecureBuffer &passwordUtf8,
                                        VaultPasswordAssessment *assessment) const
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->assessPassword(passwordUtf8, assessment);
}

QList<VaultHealthFinding> VaultService::passwordHealthReport(VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error) *error = lockedError();
        return {};
    }
    return backend_->passwordHealthReport(error);
}

QList<VaultEntryHistorySummary> VaultService::entryHistory(
    const QUuid &id, VaultError *error) const
{
    if (state_ != VaultState::Open || !backend_) {
        if (error) *error = lockedError();
        return {};
    }
    return backend_->entryHistory(id, error);
}

VaultError VaultService::restoreEntryHistory(const QUuid &id, int historyIndex)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->restoreEntryHistory(id, historyIndex);
}

VaultError VaultService::removeEntryHistory(const QUuid &id, int historyIndex)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->removeEntryHistory(id, historyIndex);
}

VaultError VaultService::clearEntryHistory(const QUuid &id)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->clearEntryHistory(id);
}

VaultError VaultService::upsertEntry(VaultEntryData &&entry)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->upsertEntry(std::move(entry));
}

VaultError VaultService::removeEntry(const QUuid &id)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->removeEntry(id);
}

VaultError VaultService::recycleEntry(const QUuid &id)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->recycleEntry(id);
}

VaultError VaultService::restoreEntry(const QUuid &id)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    return backend_->restoreEntry(id);
}

VaultError VaultService::save()
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    const VaultError unchanged = ensureDiskUnchanged();
    if (unchanged.isError())
        return unchanged;
    state_ = VaultState::Saving;
    VaultError result = backend_->save();
    // A failed atomic write does not invalidate the unlocked in-memory
    // database. Keep the service usable so the user can fix the destination
    // and retry instead of being forced into a destructive relock.
    state_ = VaultState::Open;
    if (!result.isError()) {
        diskVersion_ = {};
        captureDiskFileVersion(filePath_, &diskVersion_);
    }
    return result;
}

DiskFileState VaultService::externalFileState() const
{
    if (state_ == VaultState::Locked || filePath_.isEmpty() || !diskVersion_.valid)
        return DiskFileState::NotTracked;
    return compareDiskFileVersion(filePath_, diskVersion_);
}

VaultError VaultService::ensureDiskUnchanged() const
{
    const DiskFileState state = externalFileState();
    if (state == DiskFileState::NotTracked || state == DiskFileState::Unchanged)
        return VaultError::success();
    QString suffix = QStringLiteral("unreadable");
    QString message = QStringLiteral(
        "无法确认磁盘上的 KDBX 是否保持不变。为避免覆盖外部内容，操作已停止。");
    if (state == DiskFileState::Changed) {
        suffix = QStringLiteral("changed");
        message = QStringLiteral(
            "磁盘上的 KDBX 在打开后已被修改。为避免覆盖外部内容，操作已停止。");
    } else if (state == DiskFileState::Missing) {
        suffix = QStringLiteral("missing");
        message = QStringLiteral(
            "磁盘上的 KDBX 已被移动或删除。为避免意外重建原路径，操作已停止。");
    }
    return {VaultErrorCode::IoError, message,
            QStringLiteral("vault.external-change.") + suffix};
}

VaultError VaultService::lockedError() const
{
    return {VaultErrorCode::Locked,
            QStringLiteral("加密数据已锁定。"),
            QStringLiteral("vault.locked")};
}
