#include "KeePassXCVaultBackend.h"

#include "KdbxFormatProbe.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/PasswordGenerator.h"
#include "core/PasswordHealth.h"
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
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

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

bool verifyDatabaseFile(const QString &path, const QSharedPointer<const CompositeKey> &key,
                        QString *detail)
{
    auto verification = QSharedPointer<Database>::create();
    KeePass2Reader reader;
    const bool ok = reader.readDatabase(path, key, verification.data());
    if (!ok && detail)
        *detail = reader.errorString();
    return ok;
}

bool atomicCopyFile(const QString &sourcePath, const QString &destinationPath,
                    QString *detail)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (detail)
            *detail = source.errorString();
        return false;
    }
    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)) {
        if (detail)
            *detail = destination.errorString();
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 read = source.read(buffer.data(), buffer.size());
        if (read < 0 || destination.write(buffer.constData(), read) != read) {
            if (detail)
                *detail = read < 0 ? source.errorString() : destination.errorString();
            destination.cancelWriting();
            scrubByteArray(buffer);
            return false;
        }
    }
    scrubByteArray(buffer);
    if (!destination.commit()) {
        if (detail)
            *detail = destination.errorString();
        return false;
    }
    return true;
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
                                         const QString &keyFilePath,
                                         const QVariantMap &publicCustomData)
{
    lock();
    if (!Crypto::init()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码组件初始化失败。"),
                            QStringLiteral("vault.crypto.init"));
    }
    if (filePath.isEmpty() || QFileInfo::exists(filePath)) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("加密数据目标为空或文件已经存在。"),
                            QStringLiteral("vault.create.target"));
    }
    const QFileInfo destination(filePath);
    if (!destination.dir().exists()) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("加密数据目标目录不存在。"),
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
                            QStringLiteral("无法生成加密数据主密钥。"),
                            QStringLiteral("vault.create.key: ") + database->keyError());
    }
    database->setCipher(KeePass2::CIPHER_AES256);
    database->setPublicCustomData(publicCustomData);
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

VaultError KeePassXCVaultBackend::readPublicCustomData(
    const QString &filePath, QVariantMap *publicCustomData) const
{
    if (!publicCustomData || filePath.isEmpty()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("读取公开文件头的参数无效。"),
                            QStringLiteral("vault.public-data.arguments"));
    }
    publicCustomData->clear();
    if (!Crypto::init()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码组件初始化失败。"),
                            QStringLiteral("vault.crypto.init"));
    }

    const KdbxHeaderInfo header = KdbxFormatProbe::inspectFile(filePath);
    if (!header.recognized) {
        return backendError(QFileInfo::exists(filePath) ? VaultErrorCode::UnsupportedFormat
                                                       : VaultErrorCode::IoError,
                            QStringLiteral("文件不是受支持的 KDBX 加密数据。"),
                            QStringLiteral("vault.kdbx.header"));
    }

    Database database;
    KeePass2Reader reader;
    if (!reader.readDatabase(filePath, {}, &database)) {
        return backendError(VaultErrorCode::CorruptDatabase,
                            QStringLiteral("无法读取 KDBX 公开文件头。"),
                            QStringLiteral("vault.public-data.read: ") + reader.errorString());
    }
    *publicCustomData = database.publicCustomData();
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
                            QStringLiteral("文件不是受支持的 KDBX 加密数据。"),
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
                                               : QStringLiteral("加密数据已损坏或无法解析。"),
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
        if (!entry->isRecycled())
            result.push_back(summarize(entry));
    }
    if (error)
        *error = VaultError::success();
    return result;
}

QList<VaultEntrySummary> KeePassXCVaultBackend::listRecycledEntries(VaultError *error) const
{
    if (!database_) {
        if (error)
            *error = lockedError();
        return {};
    }

    QList<VaultEntrySummary> result;
    const Group *recycleBin = database_->metadata()->recycleBin();
    if (recycleBin) {
        const auto entries = recycleBin->entriesRecursive(false);
        result.reserve(entries.size());
        for (const Entry *entry : entries)
            result.push_back(summarize(entry));
    }
    if (error)
        *error = VaultError::success();
    return result;
}

QSet<QUuid> KeePassXCVaultBackend::searchEntryIds(const QString &query,
                                                  bool includeRecycled,
                                                  VaultError *error) const
{
    if (!database_) {
        if (error)
            *error = lockedError();
        return {};
    }
    QSet<QUuid> result;
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        if (error)
            *error = VaultError::success();
        return result;
    }
    const auto entries = database_->rootGroup()->entriesRecursive(false);
    for (const Entry *entry : entries) {
        if (!includeRecycled && entry->isRecycled())
            continue;
        bool matches = entry->title().contains(needle, Qt::CaseInsensitive)
                       || entry->username().contains(needle, Qt::CaseInsensitive)
                       || entry->url().contains(needle, Qt::CaseInsensitive)
                       || entry->notes().contains(needle, Qt::CaseInsensitive);
        if (!matches) {
            for (const QString &tag : entry->tagList()) {
                if (tag.contains(needle, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
        }
        if (matches)
            result.insert(entry->uuid());
    }
    if (error)
        *error = VaultError::success();
    return result;
}

VaultEntrySummary KeePassXCVaultBackend::summarize(const Entry *entry)
{
    const QStringList tags = entry->tagList();
    return {entry->uuid(), entry->title(), entry->username(), entry->url(), tags,
            tags.contains(VaultSecureNoteTag), !entry->password().isEmpty()};
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
                                  QStringLiteral("加密条目不存在。"),
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
    data->hasPassword = !entry->password().isEmpty();
    data->notesUtf8 = secureUtf8(entry->notes());
    if (entry->attachments()->hasKey(VaultSecureDocumentPayloadAttachment)) {
        QByteArray payload = entry->attachments()->value(
            VaultSecureDocumentPayloadAttachment);
        data->binaryPayload.assign(payload.constData(), payload.size());
        data->binaryPayloadPresent = true;
        if (!payload.isEmpty())
            payload.fill(0);
        payload.clear();
    }
    if (entry->attachments()->hasKey(VaultSecureDocumentTextAttachment)) {
        QByteArray payload = entry->attachments()->value(
            VaultSecureDocumentTextAttachment);
        data->textPayloadUtf8.assign(payload.constData(), payload.size());
        data->textPayloadPresent = true;
        if (!payload.isEmpty())
            payload.fill(0);
        payload.clear();
    }
    if (const auto settings = entry->totpSettings()) {
        data->totpSeedUtf8 = secureUtf8(settings->key);
        data->hasTotp = !settings->key.isEmpty();
    }
    if (error)
        *error = VaultError::success();
    return data;
}

std::unique_ptr<VaultEntryData> KeePassXCVaultBackend::readEntryMetadata(
    const QUuid &id, VaultError *error) const
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
                                  QStringLiteral("加密条目不存在。"),
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
    data->hasPassword = !entry->password().isEmpty();
    data->hasTotp = !entry->totpSettings().isNull();
    data->notesUtf8 = secureUtf8(entry->notes());
    if (error)
        *error = VaultError::success();
    return data;
}

VaultError KeePassXCVaultBackend::readEntrySecrets(
    const QUuid &id, VaultEntrySecrets *secrets) const
{
    if (!database_)
        return lockedError();
    if (!secrets) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("账号秘密输出参数无效。"),
                            QStringLiteral("vault.entry-secrets.output-null"));
    }
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("加密条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    secrets->passwordUtf8 = secureUtf8(entry->password());
    if (const auto settings = entry->totpSettings())
        secrets->totpSeedUtf8 = secureUtf8(settings->key);
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::currentTotp(const QUuid &id,
                                              VaultTotpSnapshot *snapshot) const
{
    if (!database_)
        return lockedError();
    if (!snapshot) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("验证码输出参数无效。"),
                            QStringLiteral("vault.totp.output-null"));
    }
    snapshot->code.clear();
    snapshot->remainingSeconds = 0;
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("加密条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    const auto settings = entry->totpSettings();
    if (!settings)
        return VaultError::success();
    bool valid = false;
    const QString code = entry->totp(&valid);
    if (!valid || code.isEmpty()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("验证码密钥无效。"),
                            QStringLiteral("vault.totp.invalid"));
    }
    const quint64 now = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
    const quint64 step = qMax<quint64>(1, settings->step);
    snapshot->code = code;
    snapshot->remainingSeconds = static_cast<int>(step - (now % step));
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::generatePassword(int length,
                                                    SecureBuffer *passwordUtf8) const
{
    if (!database_)
        return lockedError();
    if (!passwordUtf8 || length < 12 || length > 128) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码生成参数无效。"),
                            QStringLiteral("vault.password.generate.arguments"));
    }
    PasswordGenerator generator;
    generator.setLength(length);
    PasswordGenerator::CharClasses classes;
    classes |= PasswordGenerator::LowerLetters;
    classes |= PasswordGenerator::UpperLetters;
    classes |= PasswordGenerator::Numbers;
    classes |= PasswordGenerator::SpecialCharacters;
    generator.setCharClasses(classes);
    generator.setFlags(PasswordGenerator::ExcludeLookAlike
                       | PasswordGenerator::CharFromEveryGroup);
    if (!generator.isValid()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码生成器配置无效。"),
                            QStringLiteral("vault.password.generate.invalid-config"));
    }
    QString generated = generator.generatePassword();
    QByteArray utf8 = generated.toUtf8();
    passwordUtf8->assign(utf8.constData(), utf8.size());
    scrubByteArray(utf8);
    generated.fill(QChar(0));
    generated.clear();
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::assessPassword(
    const SecureBuffer &passwordUtf8, VaultPasswordAssessment *assessment) const
{
    if (!database_)
        return lockedError();
    if (!assessment) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("密码强度输出参数无效。"),
                            QStringLiteral("vault.password.assess.output-null"));
    }
    *assessment = {};
    if (passwordUtf8.empty())
        return VaultError::success();
    QString password = transientUtf8(passwordUtf8);
    const PasswordHealth health(password);
    password.fill(QChar(0));
    password.clear();
    assessment->entropyBits = health.entropy();
    switch (health.quality()) {
    case PasswordHealth::Quality::Bad:
        assessment->quality = VaultPasswordQuality::Bad;
        break;
    case PasswordHealth::Quality::Poor:
        assessment->quality = VaultPasswordQuality::Poor;
        break;
    case PasswordHealth::Quality::Weak:
        assessment->quality = VaultPasswordQuality::Weak;
        break;
    case PasswordHealth::Quality::Good:
        assessment->quality = VaultPasswordQuality::Good;
        break;
    case PasswordHealth::Quality::Excellent:
        assessment->quality = VaultPasswordQuality::Excellent;
        break;
    }
    return VaultError::success();
}

QList<VaultEntryHistorySummary> KeePassXCVaultBackend::entryHistory(
    const QUuid &id, VaultError *error) const
{
    QList<VaultEntryHistorySummary> result;
    if (!database_) {
        if (error) *error = lockedError();
        return result;
    }
    Entry *entry = findEntry(id);
    if (!entry) {
        if (error) {
            *error = backendError(VaultErrorCode::InternalError,
                                  QStringLiteral("加密条目不存在。"),
                                  QStringLiteral("vault.entry.missing"));
        }
        return result;
    }
    const QList<Entry *> history = entry->historyItems();
    result.reserve(history.size());
    for (int index = history.size() - 1; index >= 0; --index) {
        const Entry *item = history.at(index);
        result.push_back({index,
                          item->timeInfo().lastModificationTime().toUTC(),
                          item->title(), item->username(),
                          !item->password().isEmpty()});
    }
    if (error) *error = VaultError::success();
    return result;
}

VaultError KeePassXCVaultBackend::restoreEntryHistory(const QUuid &id,
                                                       int historyIndex)
{
    if (!database_)
        return lockedError();
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("加密条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    const QList<Entry *> history = entry->historyItems();
    if (historyIndex < 0 || historyIndex >= history.size()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("所选历史版本已经不可用。"),
                            QStringLiteral("vault.history.index"));
    }
    entry->beginUpdate();
    entry->copyDataFrom(history.at(historyIndex));
    entry->endUpdate();
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::upsertEntry(VaultEntryData &&data)
{
    if (!database_)
        return lockedError();

    Entry *entry = data.id.isNull() ? nullptr : findEntry(data.id);
    const bool existingEntry = entry != nullptr;
    if (!entry) {
        entry = new Entry();
        if (!data.id.isNull())
            entry->setUuid(data.id);
        entry->setGroup(database_->rootGroup());
        data.id = entry->uuid();
    }

    QSharedPointer<Totp::Settings> requestedTotp;
    if (!data.totpSeedUtf8.empty()) {
        QString seed = transientUtf8(data.totpSeedUtf8);
        requestedTotp = Totp::createSettings(seed);
        seed.fill(QChar(0));
        seed.clear();
    }
    const QSharedPointer<Totp::Settings> previousTotp = entry->totpSettings();
    const bool totpChanged = requestedTotp.isNull() != previousTotp.isNull()
        || (!requestedTotp.isNull()
            && requestedTotp->key.trimmed().compare(previousTotp->key.trimmed(),
                                                    Qt::CaseInsensitive) != 0);

    // Entry::setTotp() owns its own beginUpdate()/endUpdate() transaction.
    // When TOTP changes, run it before the other setters so its snapshot is
    // the complete previous account version and do not nest transactions.
    if (existingEntry && data.recordHistory && totpChanged)
        entry->setTotp(requestedTotp);
    const bool outerHistory = existingEntry && data.recordHistory && !totpChanged;
    if (outerHistory)
        entry->beginUpdate();

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

    // Standalone secure documents own this reserved attachment. Account rows
    // do not use the secure-note tag, so their unrelated KeePass attachments
    // remain untouched by ordinary edits.
    if (data.tags.contains(VaultSecureNoteTag)) {
        if (data.binaryPayloadPresent) {
            QByteArray payload(reinterpret_cast<const char *>(data.binaryPayload.data()),
                               data.binaryPayload.size());
            entry->attachments()->set(VaultSecureDocumentPayloadAttachment, payload);
            if (!payload.isEmpty())
                payload.fill(0);
            payload.clear();
            if (entry->attachments()->hasKey(VaultSecureDocumentTextAttachment))
                entry->attachments()->remove(VaultSecureDocumentTextAttachment);
        } else if (data.textPayloadPresent) {
            QByteArray payload(
                reinterpret_cast<const char *>(data.textPayloadUtf8.data()),
                data.textPayloadUtf8.size());
            entry->attachments()->set(VaultSecureDocumentTextAttachment, payload);
            if (!payload.isEmpty())
                payload.fill(0);
            payload.clear();
            if (entry->attachments()->hasKey(
                    VaultSecureDocumentPayloadAttachment)) {
                entry->attachments()->remove(VaultSecureDocumentPayloadAttachment);
            }
        } else {
            if (entry->attachments()->hasKey(
                    VaultSecureDocumentPayloadAttachment)) {
                entry->attachments()->remove(VaultSecureDocumentPayloadAttachment);
            }
            if (entry->attachments()->hasKey(VaultSecureDocumentTextAttachment))
                entry->attachments()->remove(VaultSecureDocumentTextAttachment);
        }
        data.binaryPayload.clear();
        data.textPayloadUtf8.clear();
    }

    if (totpChanged && (!existingEntry || !data.recordHistory)) {
        const int historyCount = entry->historyItems().size();
        entry->setTotp(requestedTotp);
        // New entries and silent secure-document autosaves must not gain a
        // user-visible account history item merely because setTotp() manages
        // its own transaction internally.
        const QList<Entry *> generatedHistory = entry->historyItems().mid(historyCount);
        if (!generatedHistory.isEmpty())
            entry->removeHistoryItems(generatedHistory);
    }
    if (outerHistory)
        entry->endUpdate();
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::recycleEntry(const QUuid &id)
{
    if (!database_)
        return lockedError();
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("加密条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    if (entry->isRecycled()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("条目已经在回收站中。"),
                            QStringLiteral("vault.entry.already-recycled"));
    }
    // Notepad Safe promises recoverable deletion even when importing a KDBX
    // whose recycle bin was disabled by another client.
    if (!database_->metadata()->recycleBinEnabled())
        database_->metadata()->setRecycleBinEnabled(true);
    database_->recycleEntry(entry);
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::restoreEntry(const QUuid &id)
{
    if (!database_)
        return lockedError();
    Entry *entry = findEntry(id);
    if (!entry || !entry->isRecycled()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("回收站条目不存在。"),
                            QStringLiteral("vault.entry.not-recycled"));
    }
    Group *destination = entry->previousParentGroup();
    if (!destination || destination->isRecycled())
        destination = database_->rootGroup();
    entry->setGroup(destination);
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::removeEntry(const QUuid &id)
{
    if (!database_)
        return lockedError();
    Entry *entry = findEntry(id);
    if (!entry) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("加密条目不存在。"),
                            QStringLiteral("vault.entry.missing"));
    }
    if (!entry->isRecycled()) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("只能永久删除回收站中的条目。"),
                            QStringLiteral("vault.entry.permanent-delete-outside-recycle-bin"));
    }
    delete entry;
    return VaultError::success();
}

VaultError KeePassXCVaultBackend::save()
{
    if (!database_)
        return lockedError();
    const auto verificationKey = database_->key();
    if (!verificationKey) {
        return backendError(VaultErrorCode::InternalError,
                            QStringLiteral("数据库验证密钥不可用。"),
                            QStringLiteral("vault.save.key-missing"));
    }

    const QFileInfo target(filePath_);
    QTemporaryFile candidate(target.dir().filePath(QStringLiteral(".notepadsafe-XXXXXX.kdbx")));
    candidate.setAutoRemove(true);
    if (!candidate.open()) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("无法创建加密保存候选文件。"),
                            QStringLiteral("vault.save.candidate-open: ") + candidate.errorString());
    }
    KeePass2Writer writer;
    if (!writer.writeDatabase(&candidate, database_.data()) || !candidate.flush()) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("加密数据写入候选文件失败。"),
                            QStringLiteral("vault.save.write: ") + writer.errorString());
    }
    const QString candidatePath = candidate.fileName();
    candidate.close();

    QString verificationError;
    if (!verifyDatabaseFile(candidatePath, verificationKey, &verificationError)) {
        return backendError(VaultErrorCode::CorruptDatabase,
                            QStringLiteral("新加密数据未通过重新解锁验证，原文件未被替换。"),
                            QStringLiteral("vault.save.candidate-verify: ") + verificationError);
    }

    QString backupPath;
    if (target.exists()) {
        if (!verifyDatabaseFile(filePath_, verificationKey, &verificationError)) {
            return backendError(VaultErrorCode::CorruptDatabase,
                                QStringLiteral("当前加密数据已损坏，拒绝覆盖；请先从备份恢复。"),
                                QStringLiteral("vault.save.current-verify: ") + verificationError);
        }
        const QString backupDirPath = filePath_ + QStringLiteral(".backups");
        if (!QDir().mkpath(backupDirPath)) {
            return backendError(VaultErrorCode::IoError,
                                QStringLiteral("无法创建加密快照目录，原文件未被替换。"),
                                QStringLiteral("vault.backup.directory"));
        }
        backupPath = QDir(backupDirPath).filePath(
            target.completeBaseName() + QLatin1Char('.')
            + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
            + QStringLiteral(".kdbx"));
        QString backupError;
        if (!atomicCopyFile(filePath_, backupPath, &backupError)) {
            return backendError(VaultErrorCode::IoError,
                                QStringLiteral("无法创建保存前加密快照，原文件未被替换。"),
                                QStringLiteral("vault.backup.write: ") + backupError);
        }
    }

    QString commitError;
    if (!atomicCopyFile(candidatePath, filePath_, &commitError)) {
        return backendError(VaultErrorCode::IoError,
                            QStringLiteral("无法原子替换加密数据文件。"),
                            QStringLiteral("vault.save.commit: ") + commitError);
    }
    if (!verifyDatabaseFile(filePath_, verificationKey, &verificationError)) {
        QString restoreError;
        if (!backupPath.isEmpty())
            atomicCopyFile(backupPath, filePath_, &restoreError);
        return backendError(VaultErrorCode::CorruptDatabase,
                            QStringLiteral("保存后的加密数据未通过完整性验证，已尝试恢复上一快照。"),
                            QStringLiteral("vault.save.final-verify: ") + verificationError
                                + (restoreError.isEmpty() ? QString()
                                                         : QStringLiteral("; restore: ") + restoreError));
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
                        QStringLiteral("加密数据已锁定。"),
                        QStringLiteral("vault.locked"));
}
