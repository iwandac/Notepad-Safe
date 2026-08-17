#include "SecurityIdentity.h"

#include "crypto/Crypto.h"
#include "crypto/CryptoHash.h"
#include "crypto/Random.h"
#include "crypto/SymmetricCipher.h"
#include "crypto/kdf/Argon2Kdf.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

constexpr auto PublicDataName = "NotepadSafe.KeyEnvelope.v1";
constexpr auto EnvelopeFormat = "notepadsafe-key-envelope";
constexpr auto KdfName = "Argon2id";
constexpr auto CipherName = "AES-256-GCM";
constexpr auto SlotMagic = "NotepadSafe.KeySlot.v1";
constexpr int EnvelopeVersion = 1;
constexpr int CredentialBytes = 32;
constexpr int RecoveryBytes = 32;
constexpr int SaltBytes = 32;
constexpr int NonceBytes = 12;
constexpr int MaxEnvelopeBytes = 64 * 1024;

VaultError identityError(VaultErrorCode code, const QString &message, const QString &diagnostic)
{
    return {code, message, diagnostic};
}

void scrub(QByteArray &bytes)
{
    if (!bytes.isEmpty()) {
#ifdef Q_OS_WIN
        ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
        volatile char *cursor = bytes.data();
        for (qsizetype i = 0; i < bytes.size(); ++i)
            cursor[i] = 0;
#endif
    }
    bytes.clear();
}

QByteArray viewOf(const SecureBuffer &buffer)
{
    return QByteArray::fromRawData(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

bool decodeBase64(const QString &text, QByteArray *output)
{
    const auto decoded = QByteArray::fromBase64Encoding(
        text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    *output = decoded.decoded;
    return true;
}

VaultError deriveUnlockKey(const SecureBuffer &passphraseUtf8,
                           const QByteArray &salt,
                           quint64 memoryKiB,
                           int iterations,
                           int parallelism,
                           QByteArray *key)
{
    if (passphraseUtf8.empty()) {
        return identityError(VaultErrorCode::InvalidCredentials,
                             QStringLiteral("安全口令不能为空。"),
                             QStringLiteral("identity.passphrase.empty"));
    }
    if (salt.size() != SaltBytes
        || memoryKiB < SecurityIdentity::DefaultMemoryKiB
        || memoryKiB > 2ULL * 1024ULL * 1024ULL
        || iterations < SecurityIdentity::DefaultIterations || iterations > 100
        || parallelism < 1 || parallelism > 64) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("安全身份参数损坏或超出支持范围。"),
                             QStringLiteral("identity.envelope.kdf-parameters"));
    }

    Argon2Kdf kdf(Argon2Kdf::Type::Argon2id);
    if (!kdf.setVersion(ARGON2_DEFAULT_VERSION)
        || !kdf.setMemory(memoryKiB)
        || !kdf.setRounds(iterations)
        || !kdf.setParallelism(parallelism)
        || !kdf.setSeed(salt)) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("无法初始化安全身份的 Argon2id 参数。"),
                             QStringLiteral("identity.argon2.parameters"));
    }

    QByteArray passwordView = viewOf(passphraseUtf8);
    if (!kdf.transform(passwordView, *key)) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("安全口令派生失败。"),
                             QStringLiteral("identity.argon2.transform"));
    }
    return VaultError::success();
}

QString recoveryText(const QByteArray &recoverySecret)
{
    const QByteArray checksum = CryptoHash::hash(recoverySecret, CryptoHash::Sha256)
                                    .left(4).toHex().toUpper();
    const QByteArray encoded = recoverySecret.toHex().toUpper();
    QStringList groups;
    for (int offset = 0; offset < encoded.size(); offset += 4)
        groups.push_back(QString::fromLatin1(encoded.mid(offset, 4)));
    return QStringLiteral("NSR1-%1-%2")
        .arg(groups.join(QLatin1Char('-')), QString::fromLatin1(checksum));
}

bool isUpperHex(const QString &text)
{
    for (const QChar character : text) {
        if (!character.isDigit() && !(character >= QLatin1Char('A')
                                      && character <= QLatin1Char('F'))) {
            return false;
        }
    }
    return true;
}

VaultError parseRecoveryText(const QString &text, QByteArray *recoverySecret)
{
    const QString normalized = text.trimmed().toUpper();
    if (!normalized.startsWith(QStringLiteral("NSR1-"))) {
        return identityError(VaultErrorCode::InvalidRecoveryKey,
                             QStringLiteral("恢复密钥格式不正确。"),
                             QStringLiteral("identity.recovery.prefix"));
    }
    const QStringList pieces = normalized.mid(5).split(QLatin1Char('-'), Qt::KeepEmptyParts);
    if (pieces.size() != RecoveryBytes * 2 / 4 + 1) {
        return identityError(VaultErrorCode::InvalidRecoveryKey,
                             QStringLiteral("恢复密钥长度不正确。"),
                             QStringLiteral("identity.recovery.length"));
    }

    QString encoded;
    for (int i = 0; i < pieces.size() - 1; ++i) {
        if (pieces.at(i).size() != 4 || !isUpperHex(pieces.at(i))) {
            return identityError(VaultErrorCode::InvalidRecoveryKey,
                                 QStringLiteral("恢复密钥包含无效字符。"),
                                 QStringLiteral("identity.recovery.encoding"));
        }
        encoded += pieces.at(i);
    }
    if (pieces.constLast().size() != 8 || !isUpperHex(pieces.constLast())) {
        return identityError(VaultErrorCode::InvalidRecoveryKey,
                             QStringLiteral("恢复密钥校验码无效。"),
                             QStringLiteral("identity.recovery.checksum-format"));
    }
    QByteArray secret = QByteArray::fromHex(encoded.toLatin1());
    if (secret.size() != RecoveryBytes) {
        scrub(secret);
        return identityError(VaultErrorCode::InvalidRecoveryKey,
                             QStringLiteral("恢复密钥长度不正确。"),
                             QStringLiteral("identity.recovery.decoded-length"));
    }
    const QByteArray checksum = CryptoHash::hash(secret, CryptoHash::Sha256)
                                    .left(4).toHex().toUpper();
    if (pieces.constLast().toLatin1() != checksum) {
        scrub(secret);
        return identityError(VaultErrorCode::InvalidRecoveryKey,
                             QStringLiteral("恢复密钥校验失败，请检查输入。"),
                             QStringLiteral("identity.recovery.checksum"));
    }
    *recoverySecret = secret;
    return VaultError::success();
}

QByteArray slotBinding(const QJsonObject &root, const char *slotType)
{
    QByteArray binding("NotepadSafe.KeySlot.Binding.v1");
    binding.append('\0');
    binding.append(root.value(QStringLiteral("documentId")).toString().toUtf8());
    binding.append('\0');
    binding.append(root.value(QStringLiteral("documentType")).toString().toUtf8());
    binding.append('\0');
    binding.append(root.value(QStringLiteral("identityId")).toString().toUtf8());
    binding.append('\0');
    binding.append(QByteArray::number(root.value(QStringLiteral("keyVersion")).toInt()));
    binding.append('\0');
    binding.append(slotType);
    return CryptoHash::hash(binding, CryptoHash::Sha256);
}

VaultError wrapCredential(const QByteArray &wrappingKey,
                          const QByteArray &binding,
                          const SecureBuffer &credential,
                          QJsonObject *slot)
{
    if (wrappingKey.size() != CredentialBytes || credential.size() != CredentialBytes) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密钥封装参数无效。"),
                             QStringLiteral("identity.slot.key-length"));
    }
    QByteArray nonce = randomGen()->randomArray(NonceBytes);
    QByteArray plaintext(SlotMagic);
    plaintext.append('\0');
    plaintext.append(binding);
    plaintext.append(reinterpret_cast<const char *>(credential.data()), credential.size());

    SymmetricCipher cipher;
    if (!cipher.init(SymmetricCipher::Aes256_GCM, SymmetricCipher::Encrypt,
                     wrappingKey, nonce)
        || !cipher.finish(plaintext)) {
        const QString detail = cipher.errorString();
        scrub(plaintext);
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("无法封装文件密钥。"),
                             QStringLiteral("identity.slot.encrypt: ") + detail);
    }
    slot->insert(QStringLiteral("cipher"), QLatin1String(CipherName));
    slot->insert(QStringLiteral("nonce"), QString::fromLatin1(nonce.toBase64()));
    slot->insert(QStringLiteral("data"), QString::fromLatin1(plaintext.toBase64()));
    scrub(plaintext);
    return VaultError::success();
}

VaultError unwrapCredential(const QJsonObject &slot,
                            const QByteArray &wrappingKey,
                            const QByteArray &binding,
                            VaultErrorCode authenticationCode,
                            const QString &authenticationMessage,
                            SecureBuffer *credential)
{
    QByteArray nonce;
    QByteArray ciphertext;
    if (slot.value(QStringLiteral("cipher")).toString() != QLatin1String(CipherName)
        || !decodeBase64(slot.value(QStringLiteral("nonce")).toString(), &nonce)
        || !decodeBase64(slot.value(QStringLiteral("data")).toString(), &ciphertext)
        || nonce.size() != NonceBytes || ciphertext.isEmpty()
        || wrappingKey.size() != CredentialBytes) {
        scrub(ciphertext);
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("文件密钥封装字段损坏。"),
                             QStringLiteral("identity.slot.fields"));
    }

    SymmetricCipher cipher;
    if (!cipher.init(SymmetricCipher::Aes256_GCM, SymmetricCipher::Decrypt,
                     wrappingKey, nonce)
        || !cipher.finish(ciphertext)) {
        scrub(ciphertext);
        return identityError(authenticationCode, authenticationMessage,
                             QStringLiteral("identity.slot.authentication"));
    }
    QByteArray prefix(SlotMagic);
    prefix.append('\0');
    prefix.append(binding);
    if (ciphertext.size() != prefix.size() + CredentialBytes
        || !ciphertext.startsWith(prefix)) {
        scrub(ciphertext);
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("文件密钥封装与当前文件不匹配。"),
                             QStringLiteral("identity.slot.binding"));
    }
    const QByteArray value = ciphertext.mid(prefix.size(), CredentialBytes);
    credential->assign(value.constData(), value.size());
    scrub(ciphertext);
    return VaultError::success();
}

VaultError readEnvelope(const QVariantMap &publicCustomData, QJsonObject *root)
{
    if (!publicCustomData.contains(SecurityIdentity::publicDataKey())) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("加密文件缺少自包含密钥信息。"),
                             QStringLiteral("identity.envelope.missing"));
    }
    QByteArray serialized = publicCustomData.value(SecurityIdentity::publicDataKey()).toByteArray();
    if (serialized.isEmpty())
        serialized = publicCustomData.value(SecurityIdentity::publicDataKey()).toString().toUtf8();
    if (serialized.isEmpty() || serialized.size() > MaxEnvelopeBytes) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("加密文件的密钥信息大小异常。"),
                             QStringLiteral("identity.envelope.size"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(serialized, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("加密文件的密钥信息无法解析。"),
                             QStringLiteral("identity.envelope.json"));
    }
    *root = document.object();
    if (root->value(QStringLiteral("format")).toString() != QLatin1String(EnvelopeFormat)
        || root->value(QStringLiteral("version")).toInt() != EnvelopeVersion
        || QUuid(root->value(QStringLiteral("documentId")).toString()).isNull()
        || QUuid(root->value(QStringLiteral("identityId")).toString()).isNull()
        || root->value(QStringLiteral("documentType")).toString().isEmpty()
        || root->value(QStringLiteral("keyVersion")).toInt() < 1
        || !root->value(QStringLiteral("passwordSlot")).isObject()
        || !root->value(QStringLiteral("recoverySlot")).isObject()) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("加密文件的密钥格式或版本不受支持。"),
                             QStringLiteral("identity.envelope.format"));
    }
    return VaultError::success();
}

VaultError makePasswordSlot(const QJsonObject &root,
                            const SecurityIdentityParameters &parameters,
                            const SecureBuffer &masterUnlockKey,
                            const SecureBuffer &credential,
                            QJsonObject *passwordSlot)
{
    QJsonObject slot;
    VaultError error = wrapCredential(viewOf(masterUnlockKey),
                                      slotBinding(root, "password"), credential, &slot);
    if (error.isError())
        return error;

    QJsonObject kdf;
    kdf.insert(QStringLiteral("name"), QLatin1String(KdfName));
    kdf.insert(QStringLiteral("version"), ARGON2_DEFAULT_VERSION);
    kdf.insert(QStringLiteral("memoryKiB"), static_cast<double>(parameters.memoryKiB));
    kdf.insert(QStringLiteral("iterations"), parameters.iterations);
    kdf.insert(QStringLiteral("parallelism"), parameters.parallelism);
    kdf.insert(QStringLiteral("salt"), QString::fromLatin1(parameters.kdfSalt.toBase64()));
    slot.insert(QStringLiteral("kdf"), kdf);
    *passwordSlot = slot;
    return VaultError::success();
}

} // namespace

QString SecurityIdentity::publicDataKey()
{
    return QLatin1String(PublicDataName);
}

bool SecurityIdentity::hasEnvelope(const QVariantMap &publicCustomData)
{
    QJsonObject root;
    return !readEnvelope(publicCustomData, &root).isError();
}

SecurityIdentityParameters SecurityIdentity::newIdentityParameters()
{
    Crypto::init();
    SecurityIdentityParameters parameters;
    parameters.identityId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    parameters.keyVersion = 1;
    parameters.kdfSalt = randomGen()->randomArray(SaltBytes);
    parameters.memoryKiB = DefaultMemoryKiB;
    parameters.iterations = DefaultIterations;
    parameters.parallelism = DefaultParallelism;
    return parameters;
}

VaultError SecurityIdentity::describe(const QVariantMap &publicCustomData,
                                      SecurityIdentityParameters *parameters,
                                      QString *documentType,
                                      QUuid *documentId)
{
    if (!parameters) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("安全身份描述参数无效。"),
                             QStringLiteral("identity.describe.arguments"));
    }
    QJsonObject root;
    VaultError error = readEnvelope(publicCustomData, &root);
    if (error.isError())
        return error;
    const QJsonObject kdf = root.value(QStringLiteral("passwordSlot"))
                                .toObject().value(QStringLiteral("kdf")).toObject();
    QByteArray salt;
    if (kdf.value(QStringLiteral("name")).toString() != QLatin1String(KdfName)
        || !decodeBase64(kdf.value(QStringLiteral("salt")).toString(), &salt)
        || salt.size() != SaltBytes) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("安全口令派生参数损坏。"),
                             QStringLiteral("identity.describe.kdf"));
    }
    SecurityIdentityParameters result;
    result.identityId = root.value(QStringLiteral("identityId")).toString();
    result.keyVersion = root.value(QStringLiteral("keyVersion")).toInt();
    result.kdfSalt = salt;
    result.memoryKiB = static_cast<quint64>(kdf.value(QStringLiteral("memoryKiB")).toDouble());
    result.iterations = kdf.value(QStringLiteral("iterations")).toInt();
    result.parallelism = kdf.value(QStringLiteral("parallelism")).toInt();
    if (result.memoryKiB < DefaultMemoryKiB || result.memoryKiB > 2ULL * 1024ULL * 1024ULL
        || result.iterations < DefaultIterations || result.iterations > 100
        || result.parallelism < 1 || result.parallelism > 64) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("安全口令派生参数超出支持范围。"),
                             QStringLiteral("identity.describe.kdf-range"));
    }
    *parameters = result;
    if (documentType)
        *documentType = root.value(QStringLiteral("documentType")).toString();
    if (documentId)
        *documentId = QUuid(root.value(QStringLiteral("documentId")).toString());
    return VaultError::success();
}

VaultError SecurityIdentity::deriveMasterUnlockKey(
    const SecureBuffer &passphraseUtf8,
    const SecurityIdentityParameters &parameters,
    SecureBuffer *masterUnlockKey)
{
    if (!masterUnlockKey || QUuid(parameters.identityId).isNull()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("主密钥派生参数无效。"),
                             QStringLiteral("identity.master-key.arguments"));
    }
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    masterUnlockKey->clear();
    QByteArray key;
    VaultError error = deriveUnlockKey(passphraseUtf8, parameters.kdfSalt,
                                       parameters.memoryKiB, parameters.iterations,
                                       parameters.parallelism, &key);
    if (!error.isError())
        masterUnlockKey->assign(key.constData(), key.size());
    scrub(key);
    return error;
}

QString SecurityIdentity::recoveryKeyText(const SecureBuffer &recoverySecret)
{
    if (recoverySecret.size() != RecoveryBytes)
        return {};
    return recoveryText(viewOf(recoverySecret));
}

VaultError SecurityIdentity::parseRecoveryKey(const QString &text,
                                              SecureBuffer *recoverySecret)
{
    if (!recoverySecret) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("恢复密钥输出参数无效。"),
                             QStringLiteral("identity.recovery.output"));
    }
    recoverySecret->clear();
    QByteArray parsed;
    VaultError error = parseRecoveryText(text, &parsed);
    if (!error.isError())
        recoverySecret->assign(parsed.constData(), parsed.size());
    scrub(parsed);
    return error;
}

VaultError SecurityIdentity::createForIdentity(
    const SecurityIdentityParameters &parameters,
    const SecureBuffer &masterUnlockKey,
    const SecureBuffer &recoverySecret,
    SecureBuffer *databaseCredentialUtf8,
    QVariantMap *publicCustomData,
    const QString &documentType)
{
    if (!databaseCredentialUtf8 || !publicCustomData
        || QUuid(parameters.identityId).isNull() || parameters.keyVersion < 1
        || parameters.kdfSalt.size() != SaltBytes
        || masterUnlockKey.size() != CredentialBytes
        || recoverySecret.size() != RecoveryBytes
        || documentType.trimmed().isEmpty()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("安全文件密钥创建参数无效。"),
                             QStringLiteral("identity.create-for-identity.arguments"));
    }
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    QByteArray credentialBytes = randomGen()->randomArray(CredentialBytes);
    SecureBuffer credential(credentialBytes.constData(), credentialBytes.size());
    scrub(credentialBytes);

    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(EnvelopeFormat));
    root.insert(QStringLiteral("version"), EnvelopeVersion);
    root.insert(QStringLiteral("documentId"),
                QUuid::createUuid().toString(QUuid::WithoutBraces));
    root.insert(QStringLiteral("documentType"), documentType.trimmed());
    root.insert(QStringLiteral("identityId"), parameters.identityId);
    root.insert(QStringLiteral("keyVersion"), parameters.keyVersion);

    QJsonObject passwordSlot;
    VaultError error = makePasswordSlot(root, parameters, masterUnlockKey,
                                        credential, &passwordSlot);
    if (error.isError())
        return error;
    QJsonObject recoverySlot;
    error = wrapCredential(viewOf(recoverySecret), slotBinding(root, "recovery"),
                           credential, &recoverySlot);
    if (error.isError())
        return error;
    recoverySlot.insert(QStringLiteral("keyId"),
                        QString::fromLatin1(CryptoHash::hash(viewOf(recoverySecret),
                                                            CryptoHash::Sha256)
                                                .left(8).toHex()));
    root.insert(QStringLiteral("passwordSlot"), passwordSlot);
    root.insert(QStringLiteral("recoverySlot"), recoverySlot);
    QVariantMap updated = *publicCustomData;
    updated.insert(publicDataKey(), QJsonDocument(root).toJson(QJsonDocument::Compact));
    *publicCustomData = updated;
    *databaseCredentialUtf8 = std::move(credential);
    return VaultError::success();
}

VaultError SecurityIdentity::create(const SecureBuffer &passphraseUtf8,
                                    SecureBuffer *databaseCredentialUtf8,
                                    QString *recoveryKey,
                                    QVariantMap *publicCustomData,
                                    const QString &documentType)
{
    if (!databaseCredentialUtf8 || !recoveryKey || !publicCustomData
        || documentType.trimmed().isEmpty()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("安全身份创建参数无效。"),
                             QStringLiteral("identity.create.arguments"));
    }
    databaseCredentialUtf8->clear();
    recoveryKey->clear();
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }

    const SecurityIdentityParameters parameters = newIdentityParameters();
    SecureBuffer masterUnlockKey;
    VaultError error = deriveMasterUnlockKey(passphraseUtf8, parameters, &masterUnlockKey);
    if (error.isError())
        return error;
    QByteArray recoveryBytes = randomGen()->randomArray(RecoveryBytes);
    SecureBuffer recoverySecret(recoveryBytes.constData(), recoveryBytes.size());
    scrub(recoveryBytes);
    *recoveryKey = recoveryKeyText(recoverySecret);
    error = createForIdentity(parameters, masterUnlockKey, recoverySecret,
                              databaseCredentialUtf8, publicCustomData, documentType);
    masterUnlockKey.clear();
    recoverySecret.clear();
    if (error.isError()) {
        recoveryKey->fill(QChar(0));
        recoveryKey->clear();
    }
    return error;
}

VaultError SecurityIdentity::unlock(const QVariantMap &publicCustomData,
                                    const SecureBuffer &passphraseUtf8,
                                    SecureBuffer *databaseCredentialUtf8)
{
    if (!databaseCredentialUtf8) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("解锁参数无效。"),
                             QStringLiteral("identity.unlock.arguments"));
    }
    databaseCredentialUtf8->clear();
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    QJsonObject root;
    VaultError error = readEnvelope(publicCustomData, &root);
    if (error.isError())
        return error;
    const QJsonObject slot = root.value(QStringLiteral("passwordSlot")).toObject();
    const QJsonObject kdf = slot.value(QStringLiteral("kdf")).toObject();
    QByteArray salt;
    if (kdf.value(QStringLiteral("name")).toString() != QLatin1String(KdfName)
        || !decodeBase64(kdf.value(QStringLiteral("salt")).toString(), &salt)) {
        return identityError(VaultErrorCode::CorruptKeyEnvelope,
                             QStringLiteral("安全口令派生参数损坏。"),
                             QStringLiteral("identity.password-slot.kdf"));
    }
    QByteArray unlockKey;
    error = deriveUnlockKey(passphraseUtf8, salt,
                            static_cast<quint64>(kdf.value(QStringLiteral("memoryKiB")).toDouble()),
                            kdf.value(QStringLiteral("iterations")).toInt(),
                            kdf.value(QStringLiteral("parallelism")).toInt(), &unlockKey);
    if (error.isError())
        return error;
    SecureBuffer masterUnlockKey(unlockKey.constData(), unlockKey.size());
    scrub(unlockKey);
    error = unlockWithMasterKey(publicCustomData, masterUnlockKey,
                                databaseCredentialUtf8);
    masterUnlockKey.clear();
    return error;
}

VaultError SecurityIdentity::unlockWithMasterKey(
    const QVariantMap &publicCustomData,
    const SecureBuffer &masterUnlockKey,
    SecureBuffer *databaseCredentialUtf8)
{
    if (!databaseCredentialUtf8 || masterUnlockKey.size() != CredentialBytes) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("会话密钥解锁参数无效。"),
                             QStringLiteral("identity.session-unlock.arguments"));
    }
    databaseCredentialUtf8->clear();
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    QJsonObject root;
    VaultError error = readEnvelope(publicCustomData, &root);
    if (error.isError())
        return error;
    return unwrapCredential(root.value(QStringLiteral("passwordSlot")).toObject(),
                            viewOf(masterUnlockKey), slotBinding(root, "password"),
                            VaultErrorCode::InvalidCredentials,
                            QStringLiteral("当前会话不能解锁此文件，或文件密钥信息已被篡改。"),
                            databaseCredentialUtf8);
}

VaultError SecurityIdentity::recover(const QVariantMap &publicCustomData,
                                     const QString &recoveryKey,
                                     SecureBuffer *databaseCredentialUtf8)
{
    if (!databaseCredentialUtf8) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("恢复参数无效。"),
                             QStringLiteral("identity.recovery.arguments"));
    }
    databaseCredentialUtf8->clear();
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    SecureBuffer recoverySecret;
    VaultError error = parseRecoveryKey(recoveryKey, &recoverySecret);
    if (error.isError())
        return error;
    error = recoverWithSecret(publicCustomData, recoverySecret,
                              databaseCredentialUtf8);
    recoverySecret.clear();
    return error;
}

VaultError SecurityIdentity::recoverWithSecret(
    const QVariantMap &publicCustomData, const SecureBuffer &recoverySecret,
    SecureBuffer *databaseCredentialUtf8)
{
    if (!databaseCredentialUtf8 || recoverySecret.size() != RecoveryBytes) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("恢复参数无效。"),
                             QStringLiteral("identity.recovery-secret.arguments"));
    }
    databaseCredentialUtf8->clear();
    if (!Crypto::init()) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("密码组件初始化失败。"),
                             QStringLiteral("identity.crypto.init"));
    }
    QJsonObject root;
    VaultError error = readEnvelope(publicCustomData, &root);
    if (error.isError())
        return error;
    return unwrapCredential(root.value(QStringLiteral("recoverySlot")).toObject(),
                            viewOf(recoverySecret), slotBinding(root, "recovery"),
                            VaultErrorCode::InvalidRecoveryKey,
                            QStringLiteral("恢复密钥不正确，或文件密钥信息已被篡改。"),
                            databaseCredentialUtf8);
}

VaultError SecurityIdentity::rebuildPasswordSlot(const QVariantMap &publicCustomData,
                                                 const SecureBuffer &passphraseUtf8,
                                                 const SecureBuffer &databaseCredentialUtf8,
                                                 QVariantMap *updatedPublicCustomData)
{
    if (!updatedPublicCustomData || databaseCredentialUtf8.size() != CredentialBytes) {
        return identityError(VaultErrorCode::InternalError,
                             QStringLiteral("重建安全口令参数无效。"),
                             QStringLiteral("identity.rebuild.arguments"));
    }
    QJsonObject root;
    VaultError error = readEnvelope(publicCustomData, &root);
    if (error.isError())
        return error;
    SecurityIdentityParameters parameters;
    error = describe(publicCustomData, &parameters);
    if (error.isError())
        return error;
    SecureBuffer masterUnlockKey;
    error = deriveMasterUnlockKey(passphraseUtf8, parameters, &masterUnlockKey);
    if (error.isError())
        return error;
    QJsonObject passwordSlot;
    error = makePasswordSlot(root, parameters, masterUnlockKey,
                             databaseCredentialUtf8, &passwordSlot);
    masterUnlockKey.clear();
    if (error.isError())
        return error;
    root.insert(QStringLiteral("passwordSlot"), passwordSlot);
    QVariantMap updated = publicCustomData;
    updated.insert(publicDataKey(), QJsonDocument(root).toJson(QJsonDocument::Compact));
    *updatedPublicCustomData = updated;
    return VaultError::success();
}
