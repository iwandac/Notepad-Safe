#pragma once

#include "IVaultBackend.h"

#include <memory>

class VaultService final {
public:
    explicit VaultService(std::unique_ptr<IVaultBackend> backend);
    ~VaultService();

    VaultService(const VaultService &) = delete;
    VaultService &operator=(const VaultService &) = delete;

    VaultState state() const noexcept { return state_; }
    QString filePath() const { return filePath_; }

    VaultError create(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath = {});
    VaultError unlock(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath = {});
    void lock() noexcept;
    QList<VaultEntrySummary> listEntries(VaultError *error = nullptr) const;
    std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                              VaultError *error = nullptr) const;
    VaultError upsertEntry(VaultEntryData &&entry);
    VaultError removeEntry(const QUuid &id);
    VaultError save();

private:
    VaultError lockedError() const;

    std::unique_ptr<IVaultBackend> backend_;
    VaultState state_ = VaultState::Locked;
    QString filePath_;
};
