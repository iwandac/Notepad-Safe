#include "KeePassXCVaultBackend.h"

#include "KdbxFormatProbe.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"
#include "crypto/CryptoHash.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
#include "format/KeePass2.h"
#include "keys/CompositeKey.h"
#include "keys/FileKey.h"
#include "keys/PasswordKey.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

QString transientUtf8(const SecureBuffer &value)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(value.data()), value.size());
}

VaultError backendError(VaultErrorCode code, const QString &message,
                        const QString &diagnostic)
{
    return {code, message, diagnostic};
}

void scrubByteArray(QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
#ifdef Q_OS_WIN
    ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
    volatile char *cursor = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        cursor[i] = 0;
#endif
    bytes.clear();
}

SecureBuffer secureUtf8(const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    SecureBuffer result(utf8.constData(), utf8.size());
    scrubByteArray(utf8);
    return result;
}

VaultError buildCompositeKey(const SecureBuffer &masterPasswordUtf8,
                             const QString &keyFilePath,
                             QSharedPointer<CompositeKey> *result)
{
    if (masterPasswordUtf8.empty() && keyFilePath.isEmpty()) {
        return backendError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("主密码和密钥文件不能同时为空。"),
                            QStringLiteral("vault.key.empty"));
    }

    auto key = QSharedPointer<CompositeKey>::create();
    if (!masterPasswordUtf8.empty()) {
        const QByteArray passwordView = QByteArray::fromRawData(
            reinterpret_cast<const char *>(masterPasswordUtf8.data()), masterPasswordUtf8.size());
        QByteArray passwordHash = CryptoHash::hash(passwordView, CryptoHash::Sha256);
        key->addKey(PasswordKey::fromRawKey(passwordHash));
        scrubByteArray(passwordHash);
    }

    if (!keyFilePath.isEmpty()) {
        auto fileKey = QSharedPointer<FileKey>::create();
        if (!fileKey->load(keyFilePath)) {
            return backendError(VaultErrorCode::IoError,
                                QStringLiteral("无法读取密钥文件。"),
                                QStringLiteral("vault.keyfile.read"));
        }
        key->addKey(fileKey);
    }

    *result = std::move(key);
    return VaultError::success();
}

} // namespace

KeePassXCVaultBackend::KeePassXCVaultBackend() = default;

KeePassXCVaultBackend::~KeePassXCVaultBackend()
{
    lock();
}

VaultError KeePassXCVaultBackend::create(const QString &filePath,
                                         const SecureBuffer &masterPasswordUtf8,
                                         const QString &keyFilePath)
{
    lock();
    if (!Crypto::init()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码组件初始化失败。"),
                            QStringLiteral("vault.crypto.init"));
    }
    if (filePath.isEmpty() || QFileInfo::exists(filePath)) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("保险库目标为空或文件已经存在。"),
                            QStringLiteral("vault.create.target"));
    }
    const QFileInfo destination(filePath);
    if (!destination.dir().exists()) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("保险库目标目录不存在。"),
                            QStringLiteral("vault.create.directory"));
    }

    QSharedPointer<CompositeKey> key;
    VaultError keyError = buildCompositeKey(masterPasswordUtf8, keyFilePath, &key);
    if (keyError.isError())
        return keyError;

    auto database = QSharedPointer<Database>::create(filePath);
    auto kdf = KeePass2::uuidToKdf(KeePass2::KDF_ARGON2ID);
    if (!kdf) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("无法初始化 Argon2id。"),
                            QStringLiteral("vault.create.kdf"));
    }
    database->setKdf(kdf);
    if (!database->setKey(key)) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("无法生成保险库主密钥。"),
                            QStringLiteral("vault.create.key: ") + database->keyError());
    }
    database->setCipher(KeePass2::CIPHER_AES256);
    database->rootGroup()->setName(QStringLiteral("Notepad Safe"));

    database_ = std::move(database);
    filePath_ = filePath;
    VaultError saveError = save();
    if (saveError.isError()) {
        lock();
        return saveError;
    }
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::unlock(const QString &filePath,
                                         const SecureBuffer &masterPasswordUtf8,
                                         const QString &keyFilePath)
{
    lock();
    if (!Crypto::init()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码组件初始化失败。"),
                            QStringLiteral("vault.crypto.init"));
    }

    const KdbxHeaderInfo header = KdbxFormatProbe::inspectFile(filePath);
    if (!header.recognized) {
        return backendError(QFileInfo::exists(filePath) ? VaultErrorCode::UnsupportedFormat
                                                       : VaultErrorCode::IoError,
                            QStringLiteral("文件不是受支持的 KDBX 保险库。"),
                            QStringLiteral("vault.kdbx.header"));
    }

    QSharedPointer<CompositeKey> key;
    VaultError keyError = buildCompositeKey(masterPasswordUtf8, keyFilePath, &key);
    if (keyError.isError())
        return keyError;

    auto database = QSharedPointer<Database>::create();
    KeePass2Reader reader;
    if (!reader.readDatabase(filePath, key, database.data())) {
        const QString detail = reader.errorString();
        const bool invalidCredentials = detail.contains(QStringLiteral("Invalid credentials"),
                                                        Qt::CaseInsensitive);
        return backendError(invalidCredentials ? VaultErrorCode::InvalidCredentials
                                               : VaultErrorCode::CorruptDatabase,
                            invalidCredentials ? QStringLiteral("主密码或密钥文件不正确。")
                                               : QStringLiteral("保险库已损坏或无法解析。"),
                            QStringLiteral("vault.kdbx.read: ") + detail);
    }

    database_ = std::move(database);
    filePath_ = filePath;
    return VaultError::success();
}

void KeePassXCVaultBackend::lock() noexcept
{
    database_.clear();
    filePath_.clear();
}

QList<VaultEntrySummary> KeePassXCVaultBackend::listEntries(VaultError *error) const
{
    if (!database_) {
        if (error)
            *error = lockedError();
        return {};
    }

    QList<VaultEntrySummary> result;
    const auto entries = database_->rootGroup()->entriesRecursive(false);
    result.reserve(entries.size());
    for (const Entry *entry : entries) {
        const QStringList tags = entry->tagList();
        result.push_back({entry->uuid(), entry->title(), entry->username(), entry->url(), tags,
                          tags.contains(VaultSecureNoteTag), !entry->password().isEmpty()});
    }
    if (error)
        *error = VaultError::success();
    return result;
}

std::unique_ptr<VaultEntryData> KeePassXCVaultBackend::readEntry(const QUuid &id,
                                                                 VaultError *error) const
{
    if (!database_) {
        if (error)
            *error = lockedError();
        return {};
    }
    Entry *entry = findEntry(id);
    if (!entry) {
        if (error) {
            *error = backendError(VaultErrorCode::InternalError,
                                  QStringLiteral("保险库条目不存在。"),
                                  QStringLiteral("vault.entry.missing"));
        }
        return {};
    }

    auto data = std::make_unique<VaultEntryData>();
    data->id = entry->uuid();
    data->title = entry->title();
    data->username = entry->username();
    data->url = entry->url();
    data->tags = entry->tagList();
    data->passwordUtf8 = secureUtf8(entry->password());
    data->notesUtf8 = secureUtf8(entry->notes());
    if (const auto settings = entry->totpSettings())
        data->totpSeedUtf8 = secureUtf8(settings->key);
    if (error)
        *error = VaultError::success();
    return data;
}

VaultError KeePassXCVaultBackend::upsertEntry(VaultEntryData &&data)
{
    if (!database_)
        return lockedError();

    Entry *entry = data.id.isNull() ? nullptr : findEntry(data.id);
    if (!entry) {
        entry = new Entry();
        if (!data.id.isNull())
            entry->setUuid(data.id);
        entry->setGroup(database_->rootGroup());
        data.id = entry->uuid();
    }

    entry->setTitle(data.title);
    entry->setUsername(data.username);
    entry->setUrl(data.url);
    entry->setTags(data.tags.join(QLatin1Char(',')));

    QString password = transientUtf8(data.passwordUtf8);
    entry->setPassword(password);
    password.fill(QChar(0));
    password.clear();

    QString notes = transientUtf8(data.notesUtf8);
    entry->setNotes(notes);
    notes.fill(QChar(0));
    notes.clear();

    if (data.totpSeedUtf8.empty()) {
        entry->setTotp({});
    } else {
        QString seed = transientUtf8(data.totpSeedUtf8);
        entry->setTotp(Totp::createSettings(seed));
        seed.fill(QChar(0));
        seed.clear();
    }
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::removeEntry(const QUuid &id)
{
    if (!database_)
        return lockedError();
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("保险库条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    delete entry;
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::save()
{
    if (!database_)
        return lockedError();

    QSaveFile output(filePath_);
    if (!output.open(QIODevice::WriteOnly)) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("无法创建保险库临时文件。"),
                            QStringLiteral("vault.save.open: ") + output.errorString());
    }
    KeePass2Writer writer;
    if (!writer.writeDatabase(&output, database_.data())) {
        output.cancelWriting();
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("保险库写入失败。"),
                            QStringLiteral("vault.save.write: ") + writer.errorString());
    }
    if (!output.commit()) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("无法原子替换保险库文件。"),
                            QStringLiteral("vault.save.commit: ") + output.errorString());
    }
    return VaultError::success();
}

Entry *KeePassXCVaultBackend::findEntry(const QUuid &id) const
{
    if (!database_)
        return nullptr;
    for (Entry *entry : database_->rootGroup()->entriesRecursive(false)) {
        if (entry->uuid() == id)
            return entry;
    }
    return nullptr;
}

VaultError KeePassXCVaultBackend::lockedError()
{
    return backendError(VaultErrorCode::Locked,
                        QStringLiteral("保险库已锁定。"),
                        QStringLiteral("vault.locked"));
}
