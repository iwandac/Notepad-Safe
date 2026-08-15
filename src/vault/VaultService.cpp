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
                                const QString &keyFilePath)
{
    lock();
    if (!backend_) {
        state_ = VaultState::Faulted;
        return {VaultErrorCode::BackendUnavailable,
                QStringLiteral("保险库后端尚未安装。"),
                QStringLiteral("vault.backend.unavailable")};
    }

    state_ = VaultState::Unlocking;
    VaultError result = backend_->create(filePath, masterPasswordUtf8, keyFilePath);
    if (result.isError()) {
        backend_->lock();
        state_ = VaultState::Locked;
        filePath_.clear();
        return result;
    }

    filePath_ = filePath;
    state_ = VaultState::Open;
    return VaultError::success();
}

VaultError VaultService::unlock(const QString &filePath,
                                const SecureBuffer &masterPasswordUtf8,
                                const QString &keyFilePath)
{
    lock();
    if (!backend_) {
        state_ = VaultState::Faulted;
        return {VaultErrorCode::BackendUnavailable,
                QStringLiteral("保险库后端尚未安装。"),
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
    return VaultError::success();
}

void VaultService::lock() noexcept
{
    if (backend_)
        backend_->lock();
    filePath_.clear();
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

VaultError VaultService::upsertEntry(VaultEntryData &&entry)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->upsertEntry(std::move(entry));
}

VaultError VaultService::removeEntry(const QUuid &id)
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    return backend_->removeEntry(id);
}

VaultError VaultService::save()
{
    if (state_ != VaultState::Open || !backend_)
        return lockedError();
    state_ = VaultState::Saving;
    VaultError result = backend_->save();
    // A failed atomic write does not invalidate the unlocked in-memory
    // database. Keep the service usable so the user can fix the destination
    // and retry instead of being forced into a destructive relock.
    state_ = VaultState::Open;
    return result;
}

VaultError VaultService::lockedError() const
{
    return {VaultErrorCode::Locked,
            QStringLiteral("保险库已锁定。"),
            QStringLiteral("vault.locked")};
}
