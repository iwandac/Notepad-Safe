#pragma once

#include "IVaultBackend.h"

#include <QSharedPointer>

class Database;
class Entry;

// Production KDBX adapter. No KeePassXC type is exposed across this boundary.
class KeePassXCVaultBackend final : public IVaultBackend {
public:
    // Disable only for private, disposable staging/recovery files. User files
    // retain one verified pre-save version beside the original.
    explicit KeePassXCVaultBackend(bool keepPreviousVersion = true);
    ~KeePassXCVaultBackend() override;

    VaultError create(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath,
                      const QVariantMap &publicCustomData) override;
    VaultError readPublicCustomData(const QString &filePath,
                                    QVariantMap *publicCustomData) const override;
    VaultError unlock(const QString &filePath,
                      const SecureBuffer &masterPasswordUtf8,
                      const QString &keyFilePath) override;
    void lock() noexcept override;
    QList<VaultEntrySummary> listEntries(VaultError *error) const override;
    QList<VaultEntrySummary> listRecycledEntries(VaultError *error) const override;
    QSet<QUuid> searchEntryIds(const QString &query, bool includeRecycled,
                               VaultError *error) const override;
    std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                              VaultError *error) const override;
    std::unique_ptr<VaultEntryData> readEntryMetadata(const QUuid &id,
                                                      VaultError *error) const override;
    VaultError readEntrySecrets(const QUuid &id,
                                VaultEntrySecrets *secrets) const override;
    VaultError currentTotp(const QUuid &id, VaultTotpSnapshot *snapshot) const override;
    VaultError generatePassword(int length, SecureBuffer *passwordUtf8) const override;
    VaultError generatePassword(const VaultPasswordGenerationOptions &options,
                                SecureBuffer *passwordUtf8) const override;
    VaultError assessPassword(const SecureBuffer &passwordUtf8,
                              VaultPasswordAssessment *assessment) const override;
    QList<VaultHealthFinding> passwordHealthReport(VaultError *error) const override;
    QList<VaultEntryHistorySummary> entryHistory(
        const QUuid &id, VaultError *error = nullptr) const override;
    VaultError restoreEntryHistory(const QUuid &id, int historyIndex) override;
    VaultError removeEntryHistory(const QUuid &id, int historyIndex) override;
    VaultError clearEntryHistory(const QUuid &id) override;
    VaultError upsertEntry(VaultEntryData &&entry) override;
    VaultError recycleEntry(const QUuid &id) override;
    VaultError restoreEntry(const QUuid &id) override;
    VaultError removeEntry(const QUuid &id) override;
    VaultError save() override;

private:
    Entry *findEntry(const QUuid &id) const;
    static VaultEntrySummary summarize(const Entry *entry);
    static VaultError lockedError();

    QSharedPointer<Database> database_;
    QString filePath_;
    const bool keepPreviousVersion_;
};
