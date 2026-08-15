#pragma once

#include "IVaultBackend.h"

#include <QSharedPointer>

class Database;
class Entry;

// Production KDBX adapter. No KeePassXC type is exposed across this boundary.
class KeePassXCVaultBackend final : public IVaultBackend {
public:
    KeePassXCVaultBackend();
    ~KeePassXCVaultBackend() override;

    VaultError create(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath) override;
    VaultError unlock(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath) override;
    void lock() noexcept override;
    QList<VaultEntrySummary> listEntries(VaultError *error) const override;
    std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                              VaultError *error) const override;
    VaultError upsertEntry(VaultEntryData &&entry) override;
    VaultError removeEntry(const QUuid &id) override;
    VaultError save() override;

private:
    Entry *findEntry(const QUuid &id) const;
    static VaultError lockedError();

    QSharedPointer<Database> database_;
    QString filePath_;
};
