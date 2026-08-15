#pragma once

#include "VaultTypes.h"

#include <memory>

// Adapter seam for the pinned KeePassXC/KDBX implementation. UI code must
// depend on VaultService, never on KeePassXC Database/Entry classes directly.
class IVaultBackend {
public:
    virtual ~IVaultBackend() = default;

    virtual VaultError create(const QString &filePath,
                              const SecureBuffer &masterPasswordUtf8,
                              const QString &keyFilePath) = 0;
    virtual VaultError unlock(const QString &filePath,
                              const SecureBuffer &masterPasswordUtf8,
                              const QString &keyFilePath) = 0;
    virtual void lock() noexcept = 0;
    virtual QList<VaultEntrySummary> listEntries(VaultError *error) const = 0;
    virtual std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                                      VaultError *error) const = 0;
    virtual VaultError upsertEntry(VaultEntryData &&entry) = 0;
    virtual VaultError removeEntry(const QUuid &id) = 0;
    virtual VaultError save() = 0;
};
