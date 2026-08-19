#pragma once

#include "VaultTypes.h"

#include <QVariantMap>
#include <QSet>

#include <memory>
#include <utility>

// Adapter seam for the pinned KeePassXC/KDBX implementation. UI code must
// depend on VaultService, never on KeePassXC Database/Entry classes directly.
class IVaultBackend {
public:
    virtual ~IVaultBackend() = default;

    virtual VaultError create(const QString &filePath,
                              const SecureBuffer &masterPasswordUtf8,
                              const QString &keyFilePath,
                              const QVariantMap &publicCustomData) = 0;
    virtual VaultError readPublicCustomData(const QString &filePath,
                                            QVariantMap *publicCustomData) const = 0;
    virtual VaultError unlock(const QString &filePath,
                              const SecureBuffer &masterPasswordUtf8,
                              const QString &keyFilePath) = 0;
    virtual void lock() noexcept = 0;
    virtual QList<VaultEntrySummary> listEntries(VaultError *error) const = 0;
    virtual QList<VaultEntrySummary> listRecycledEntries(VaultError *error) const = 0;
    virtual QSet<QUuid> searchEntryIds(const QString &query, bool includeRecycled,
                                       VaultError *error) const = 0;
    virtual std::unique_ptr<VaultEntryData> readEntry(const QUuid &id,
                                                      VaultError *error) const = 0;
    virtual std::unique_ptr<VaultEntryData> readEntryMetadata(const QUuid &id,
                                                              VaultError *error) const
    {
        auto data = readEntry(id, error);
        if (data) {
            data->hasPassword = !data->passwordUtf8.empty();
            data->hasTotp = !data->totpSeedUtf8.empty();
            data->passwordUtf8.clear();
            data->totpSeedUtf8.clear();
        }
        return data;
    }
    virtual VaultError readEntrySecrets(const QUuid &id,
                                        VaultEntrySecrets *secrets) const
    {
        if (!secrets) {
            return {VaultErrorCode::InternalError,
                    QStringLiteral("账号秘密输出参数无效。"),
                    QStringLiteral("vault.entry-secrets.output-null")};
        }
        VaultError error;
        auto data = readEntry(id, &error);
        if (!data)
            return error;
        secrets->passwordUtf8 = std::move(data->passwordUtf8);
        secrets->totpSeedUtf8 = std::move(data->totpSeedUtf8);
        return VaultError::success();
    }
    virtual VaultError currentTotp(const QUuid &id, VaultTotpSnapshot *snapshot) const = 0;
    virtual VaultError generatePassword(int length, SecureBuffer *passwordUtf8) const = 0;
    virtual VaultError generatePassword(const VaultPasswordGenerationOptions &options,
                                        SecureBuffer *passwordUtf8) const
    {
        return generatePassword(options.length, passwordUtf8);
    }
    virtual VaultError assessPassword(const SecureBuffer &passwordUtf8,
                                      VaultPasswordAssessment *assessment) const = 0;
    virtual QList<VaultHealthFinding> passwordHealthReport(VaultError *error) const
    {
        if (error) {
            *error = {VaultErrorCode::UnsupportedFormat,
                      QStringLiteral("当前后端不支持密码健康检查。"),
                      QStringLiteral("vault.health.unsupported")};
        }
        return {};
    }
    virtual QList<VaultEntryHistorySummary> entryHistory(
        const QUuid &, VaultError *error) const
    {
        if (error) {
            *error = {VaultErrorCode::UnsupportedFormat,
                      QStringLiteral("当前后端不支持账号历史。"),
                      QStringLiteral("vault.history.unsupported")};
        }
        return {};
    }
    virtual VaultError restoreEntryHistory(const QUuid &, int)
    {
        return {VaultErrorCode::UnsupportedFormat,
                QStringLiteral("当前后端不支持账号历史恢复。"),
                QStringLiteral("vault.history.restore.unsupported")};
    }
    virtual VaultError removeEntryHistory(const QUuid &, int)
    {
        return {VaultErrorCode::UnsupportedFormat,
                QStringLiteral("不支持删除条目历史。"),
                QStringLiteral("vault.history.remove.unsupported")};
    }
    virtual VaultError clearEntryHistory(const QUuid &)
    {
        return {VaultErrorCode::UnsupportedFormat,
                QStringLiteral("不支持清理条目历史。"),
                QStringLiteral("vault.history.clear.unsupported")};
    }
    virtual VaultError upsertEntry(VaultEntryData &&entry) = 0;
    virtual VaultError recycleEntry(const QUuid &id) = 0;
    virtual VaultError restoreEntry(const QUuid &id) = 0;
    // Permanent deletion is deliberately a separate backend operation. UI code
    // must only expose it from the recycle-bin context.
    virtual VaultError removeEntry(const QUuid &id) = 0;
    virtual VaultError save() = 0;
};
