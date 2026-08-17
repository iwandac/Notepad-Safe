#pragma once

#include "IVaultBackend.h"
#include "DiskFileVersion.h"

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
                      const QString &keyFilePath = {},
                      const QVariantMap &publicCustomData = {});
    VaultError readPublicCustomData(const QString &filePath,
                                    QVariantMap *publicCustomData) const;
    VaultError unlock(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath = {});
    void lock() noexcept;
    QList<VaultEntrySummary> listEntries(VaultError *error = nullptr) const;
    QList<VaultEntrySummary> listRecycledEntries(VaultError *error = nullptr) const;
    QSet<QUuid> searchEntryIds(const QString &query, bool includeRecycled = true,
                               VaultError *error = nullptr) const;
    std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                              VaultError *error = nullptr) const;
    std::unique_ptr<VaultEntryData> readEntryMetadata(
        const QUuid &id, VaultError *error = nullptr) const;
    VaultError readEntrySecrets(const QUuid &id, VaultEntrySecrets *secrets) const;
    VaultError currentTotp(const QUuid &id, VaultTotpSnapshot *snapshot) const;
    VaultError generatePassword(int length, SecureBuffer *passwordUtf8) const;
    VaultError assessPassword(const SecureBuffer &passwordUtf8,
                              VaultPasswordAssessment *assessment) const;
    QList<VaultEntryHistorySummary> entryHistory(
        const QUuid &id, VaultError *error = nullptr) const;
    VaultError restoreEntryHistory(const QUuid &id, int historyIndex);
    VaultError upsertEntry(VaultEntryData &&entry);
    VaultError recycleEntry(const QUuid &id);
    VaultError restoreEntry(const QUuid &id);
    VaultError removeEntry(const QUuid &id);
    VaultError save();
    DiskFileState externalFileState() const;

private:
    VaultError lockedError() const;
    VaultError ensureDiskUnchanged() const;

    std::unique_ptr<IVaultBackend> backend_;
    VaultState state_ = VaultState::Locked;
    QString filePath_;
    DiskFileVersion diskVersion_;
};
