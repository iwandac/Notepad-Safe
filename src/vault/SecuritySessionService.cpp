#include "SecuritySessionService.h"

#include "crypto/Crypto.h"
#include "crypto/Random.h"
#include "crypto/SymmetricCipher.h"

#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <limits>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <wincrypt.h>
#endif

namespace {

constexpr int KeyBytes = 32;
constexpr int NonceBytes = 12;
constexpr int SessionDays = 7;
constexpr auto DescriptorFileName = "security-identity-v1.json";
constexpr auto CapsuleFileName = "security-session-v1.bin";
constexpr auto DescriptorFormat = "notepadsafe-security-identity";
constexpr auto CapsuleFormat = "notepadsafe-sealed-session";
constexpr auto CapsuleMagicV3 = "NotepadSafe.SealedSession.v3";
constexpr auto CapsuleMagicV2 = "NotepadSafe.SealedSession.v2";
constexpr auto RecoveryMagic = "NotepadSafe.RecoverySeed.v1";
constexpr auto SessionEntropy = "NotepadSafe.SecuritySession.v1";
constexpr auto DocumentCredentialEntropy = "NotepadSafe.DocumentCredential.v1";
constexpr int MaxRememberedKeys = 32;

VaultError sessionError(VaultErrorCode code, const QString &message,
                        const QString &diagnostic)
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

QString defaultDataDirectory()
{
    const QString override = QProcessEnvironment::systemEnvironment()
                                 .value(QStringLiteral("NOTEPADSAFE_DATADIR")).trimmed();
    if (!override.isEmpty() && QFileInfo(override).isAbsolute())
        return QDir::cleanPath(override);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("NotepadSafe"));
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

VaultError protectForCurrentUser(const QByteArray &plain, const char *entropyText,
                                 const wchar_t *description,
                                 QByteArray *protectedBytes)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    QByteArray entropyBytes(entropyText);
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
                      reinterpret_cast<BYTE *>(entropyBytes.data())};
    DATA_BLOB output{};
    if (!::CryptProtectData(&input, description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("无法建立本机安全会话。"),
                            QStringLiteral("session.dpapi.protect.%1").arg(GetLastError()));
    }
    *protectedBytes = QByteArray(reinterpret_cast<const char *>(output.pbData),
                                 static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return VaultError::success();
#else
    Q_UNUSED(plain)
    Q_UNUSED(entropyText)
    Q_UNUSED(description)
    Q_UNUSED(protectedBytes)
    return sessionError(VaultErrorCode::BackendUnavailable,
                        QStringLiteral("当前平台尚未提供设备密封会话。"),
                        QStringLiteral("session.dpapi.unsupported"));
#endif
}

VaultError unprotectForCurrentUser(const QByteArray &protectedBytes,
                                   const char *entropyText, QByteArray *plain)
{
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(protectedBytes.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(protectedBytes.constData()))};
    QByteArray entropyBytes(entropyText);
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
                      reinterpret_cast<BYTE *>(entropyBytes.data())};
    DATA_BLOB output{};
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("本机安全会话无效，需要重新登录。"),
                            QStringLiteral("session.dpapi.unprotect.%1").arg(GetLastError()));
    }
    *plain = QByteArray(reinterpret_cast<const char *>(output.pbData),
                        static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return VaultError::success();
#else
    Q_UNUSED(protectedBytes)
    Q_UNUSED(entropyText)
    Q_UNUSED(plain)
    return sessionError(VaultErrorCode::BackendUnavailable,
                        QStringLiteral("当前平台尚未提供设备密封会话。"),
                        QStringLiteral("session.dpapi.unsupported"));
#endif
}

VaultError encryptRecovery(const SecureBuffer &masterUnlockKey,
                           const SecureBuffer &recoverySecret,
                           QByteArray *nonce, QByteArray *encrypted)
{
    *nonce = randomGen()->randomArray(NonceBytes);
    QByteArray plaintext(RecoveryMagic);
    plaintext.append('\0');
    plaintext.append(viewOf(recoverySecret));
    SymmetricCipher cipher;
    if (!cipher.init(SymmetricCipher::Aes256_GCM, SymmetricCipher::Encrypt,
                     viewOf(masterUnlockKey), *nonce)
        || !cipher.finish(plaintext)) {
        scrub(plaintext);
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("无法保护身份恢复种子。"),
                            QStringLiteral("session.recovery.encrypt"));
    }
    *encrypted = plaintext;
    scrub(plaintext);
    return VaultError::success();
}

} // namespace

SecuritySessionService::SecuritySessionService(const QString &dataDirectory)
    : dataDirectory_(dataDirectory.trimmed().isEmpty() ? defaultDataDirectory()
                                                       : QDir::cleanPath(dataDirectory))
{
    QDir().mkpath(dataDirectory_);
    loadDescriptor();
    restoreSealedSession();
}

SecuritySessionService::~SecuritySessionService()
{
    clearMemory();
}

bool SecuritySessionService::hasActiveSession()
{
    return validateActiveSession();
}

bool SecuritySessionService::validateActiveSession()
{
    if (!active_)
        return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!expiresAtUtc_.isValid() || now >= expiresAtUtc_
        || (lastObservedUtc_.isValid() && now.addSecs(300) < lastObservedUtc_)) {
        endSession(true);
        return false;
    }
    if (!QFileInfo::exists(capsulePath())) {
        // The DPAPI capsule is the durable authority for a remembered device
        // session. Never leave a stale in-process flag capable of matching a
        // document identity after the capsule has disappeared.
        clearMemory();
        return false;
    }
    return true;
}

QString SecuritySessionService::descriptorPath() const
{
    return QDir(dataDirectory_).filePath(QLatin1String(DescriptorFileName));
}

QString SecuritySessionService::capsulePath() const
{
    return QDir(dataDirectory_).filePath(QLatin1String(CapsuleFileName));
}

bool SecuritySessionService::loadDescriptor()
{
    identityLoaded_ = false;
    recoveryVerifiedAtUtc_ = {};
    QFile file(descriptorPath());
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 64 * 1024)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    const QJsonObject kdf = root.value(QStringLiteral("kdf")).toObject();
    const QJsonObject recovery = root.value(QStringLiteral("encryptedRecoverySeed")).toObject();
    QByteArray salt;
    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("format")).toString() != QLatin1String(DescriptorFormat)
        || root.value(QStringLiteral("version")).toInt() != 1
        || QUuid(root.value(QStringLiteral("identityId")).toString()).isNull()
        || !decodeBase64(kdf.value(QStringLiteral("salt")).toString(), &salt)
        || !decodeBase64(recovery.value(QStringLiteral("nonce")).toString(), &recoveryNonce_)
        || !decodeBase64(recovery.value(QStringLiteral("data")).toString(),
                         &encryptedRecoverySeed_)
        || salt.size() != KeyBytes || recoveryNonce_.size() != NonceBytes
        || encryptedRecoverySeed_.isEmpty()) {
        return false;
    }
    parameters_.identityId = root.value(QStringLiteral("identityId")).toString();
    parameters_.keyVersion = root.value(QStringLiteral("keyVersion")).toInt();
    parameters_.kdfSalt = salt;
    parameters_.memoryKiB = static_cast<quint64>(kdf.value(QStringLiteral("memoryKiB")).toDouble());
    parameters_.iterations = kdf.value(QStringLiteral("iterations")).toInt();
    parameters_.parallelism = kdf.value(QStringLiteral("parallelism")).toInt();
    const QDateTime verifiedAt = QDateTime::fromString(
        root.value(QStringLiteral("recoveryVerifiedAtUtc")).toString(),
        Qt::ISODateWithMs);
    if (verifiedAt.isValid())
        recoveryVerifiedAtUtc_ = verifiedAt.toUTC();
    identityLoaded_ = parameters_.keyVersion > 0
                      && parameters_.memoryKiB >= SecurityIdentity::DefaultMemoryKiB
                      && parameters_.iterations >= SecurityIdentity::DefaultIterations
                      && parameters_.parallelism > 0;
    return identityLoaded_;
}

VaultError SecuritySessionService::persistDescriptor(
    const SecureBuffer &masterUnlockKey, const SecureBuffer &recoverySecret)
{
    VaultError error = encryptRecovery(masterUnlockKey, recoverySecret,
                                       &recoveryNonce_, &encryptedRecoverySeed_);
    if (error.isError())
        return error;
    return writeDescriptor();
}

VaultError SecuritySessionService::writeDescriptor()
{
    QJsonObject kdf;
    kdf.insert(QStringLiteral("name"), QStringLiteral("Argon2id"));
    kdf.insert(QStringLiteral("salt"), QString::fromLatin1(parameters_.kdfSalt.toBase64()));
    kdf.insert(QStringLiteral("memoryKiB"), static_cast<double>(parameters_.memoryKiB));
    kdf.insert(QStringLiteral("iterations"), parameters_.iterations);
    kdf.insert(QStringLiteral("parallelism"), parameters_.parallelism);
    QJsonObject recovery;
    recovery.insert(QStringLiteral("cipher"), QStringLiteral("AES-256-GCM"));
    recovery.insert(QStringLiteral("nonce"),
                    QString::fromLatin1(recoveryNonce_.toBase64()));
    recovery.insert(QStringLiteral("data"),
                    QString::fromLatin1(encryptedRecoverySeed_.toBase64()));
    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(DescriptorFormat));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("identityId"), parameters_.identityId);
    root.insert(QStringLiteral("keyVersion"), parameters_.keyVersion);
    root.insert(QStringLiteral("kdf"), kdf);
    root.insert(QStringLiteral("encryptedRecoverySeed"), recovery);
    if (recoveryVerifiedAtUtc_.isValid()) {
        root.insert(QStringLiteral("recoveryVerifiedAtUtc"),
                    recoveryVerifiedAtUtc_.toUTC().toString(Qt::ISODateWithMs));
    }
    QSaveFile file(descriptorPath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        return sessionError(VaultErrorCode::IoError,
                            QStringLiteral("无法保存本地安全身份。"),
                            QStringLiteral("session.identity.write: ") + file.errorString());
    }
    identityLoaded_ = true;
    return VaultError::success();
}

VaultError SecuritySessionService::decryptRecoverySeed(
    const SecureBuffer &masterUnlockKey, SecureBuffer *recoverySecret) const
{
    QByteArray plaintext = encryptedRecoverySeed_;
    SymmetricCipher cipher;
    if (!cipher.init(SymmetricCipher::Aes256_GCM, SymmetricCipher::Decrypt,
                     viewOf(masterUnlockKey), recoveryNonce_)
        || !cipher.finish(plaintext)) {
        scrub(plaintext);
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("主密码不正确，或安全身份已损坏。"),
                            QStringLiteral("session.recovery.decrypt"));
    }
    QByteArray prefix(RecoveryMagic);
    prefix.append('\0');
    if (!plaintext.startsWith(prefix) || plaintext.size() != prefix.size() + KeyBytes) {
        scrub(plaintext);
        return sessionError(VaultErrorCode::CorruptKeyEnvelope,
                            QStringLiteral("安全身份恢复信息已损坏。"),
                            QStringLiteral("session.recovery.payload"));
    }
    QByteArray secret = plaintext.mid(prefix.size(), KeyBytes);
    recoverySecret->assign(secret.constData(), secret.size());
    scrub(secret);
    scrub(plaintext);
    return VaultError::success();
}

VaultError SecuritySessionService::createIdentity(const SecureBuffer &passphraseUtf8,
                                                  QString *recoveryKey)
{
    if (!recoveryKey || passphraseUtf8.empty()) {
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("主密码不能为空。"),
                            QStringLiteral("session.identity.arguments"));
    }
    if (!Crypto::init()) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("密码组件初始化失败。"),
                            QStringLiteral("session.crypto.init"));
    }
    clearMemory();
    parameters_ = SecurityIdentity::newIdentityParameters();
    recoveryVerifiedAtUtc_ = {};
    SecureBuffer masterUnlockKey;
    SecureBuffer recoverySecret;
    VaultError error = SecurityIdentity::deriveMasterUnlockKey(
        passphraseUtf8, parameters_, &masterUnlockKey);
    if (error.isError())
        return error;
    QByteArray recovery = randomGen()->randomArray(KeyBytes);
    recoverySecret.assign(recovery.constData(), recovery.size());
    scrub(recovery);
    *recoveryKey = SecurityIdentity::recoveryKeyText(recoverySecret);
    error = persistDescriptor(masterUnlockKey, recoverySecret);
    if (error.isError()) {
        recoveryKey->fill(QChar(0));
        recoveryKey->clear();
        return error;
    }
    issuedAtUtc_ = QDateTime::currentDateTimeUtc();
    expiresAtUtc_ = issuedAtUtc_.addDays(SessionDays);
    lastObservedUtc_ = issuedAtUtc_;
    active_ = true;
    error = sealSession(masterUnlockKey, recoverySecret);
    if (error.isError())
        clearMemory();
    return error;
}

VaultError SecuritySessionService::unlockIdentity(const SecureBuffer &passphraseUtf8)
{
    if (!identityLoaded_ && !loadDescriptor()) {
        return sessionError(VaultErrorCode::CorruptKeyEnvelope,
                            QStringLiteral("本地安全身份不存在或已损坏。"),
                            QStringLiteral("session.identity.missing"));
    }
    clearMemory();
    SecureBuffer masterUnlockKey;
    SecureBuffer recoverySecret;
    VaultError error = SecurityIdentity::deriveMasterUnlockKey(
        passphraseUtf8, parameters_, &masterUnlockKey);
    if (!error.isError())
        error = decryptRecoverySeed(masterUnlockKey, &recoverySecret);
    if (error.isError()) {
        return error;
    }
    issuedAtUtc_ = QDateTime::currentDateTimeUtc();
    expiresAtUtc_ = issuedAtUtc_.addDays(SessionDays);
    lastObservedUtc_ = issuedAtUtc_;
    active_ = true;
    error = sealSession(masterUnlockKey, recoverySecret);
    if (error.isError())
        clearMemory();
    return error;
}

VaultError SecuritySessionService::changeMasterPassword(
    const SecureBuffer &currentPassphraseUtf8,
    const SecureBuffer &newPassphraseUtf8)
{
    if (currentPassphraseUtf8.empty() || newPassphraseUtf8.empty()) {
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("当前主密码和新主密码都不能为空。"),
                            QStringLiteral("session.identity.rotate.arguments"));
    }
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，请先重新登录。"),
                            QStringLiteral("session.identity.rotate.inactive"));
    }
    if (parameters_.keyVersion == (std::numeric_limits<int>::max)()) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("密钥版本已达上限，无法继续更改。"),
                            QStringLiteral("session.identity.rotate.version-limit"));
    }

    SecureBuffer currentMaster;
    SecureBuffer recoverySecret;
    std::vector<RememberedKey> rememberedKeys;
    VaultError error = unsealSession(&currentMaster, &recoverySecret,
                                     &rememberedKeys, true);
    if (error.isError()) {
        clearMemory();
        return error;
    }

    SecureBuffer presentedMaster;
    error = SecurityIdentity::deriveMasterUnlockKey(
        currentPassphraseUtf8, parameters_, &presentedMaster);
    const bool currentMatches = !error.isError()
                                && presentedMaster.equals(currentMaster.data(),
                                                          currentMaster.size());
    presentedMaster.clear();
    if (!currentMatches) {
        currentMaster.clear();
        recoverySecret.clear();
        rememberedKeys.clear();
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("当前主密码不正确。"),
                            QStringLiteral("session.identity.rotate.current-invalid"));
    }

    const SecurityIdentityParameters previousParameters = parameters_;
    SecurityIdentityParameters nextParameters = SecurityIdentity::newIdentityParameters();
    nextParameters.identityId = previousParameters.identityId;
    nextParameters.keyVersion = previousParameters.keyVersion + 1;

    SecureBuffer nextMaster;
    error = SecurityIdentity::deriveMasterUnlockKey(
        newPassphraseUtf8, nextParameters, &nextMaster);
    if (error.isError()) {
        currentMaster.clear();
        recoverySecret.clear();
        rememberedKeys.clear();
        return error;
    }

    const auto sameParameters = [](const SecurityIdentityParameters &left,
                                   const SecurityIdentityParameters &right) {
        return left.identityId == right.identityId
               && left.keyVersion == right.keyVersion
               && left.kdfSalt == right.kdfSalt
               && left.memoryKiB == right.memoryKiB
               && left.iterations == right.iterations
               && left.parallelism == right.parallelism;
    };
    const auto rememberPrevious = [&](RememberedKeyKind kind,
                                      const SecureBuffer &secret) {
        RememberedKey *target = nullptr;
        for (RememberedKey &remembered : rememberedKeys) {
            if (remembered.kind == kind
                && sameParameters(remembered.parameters, previousParameters)) {
                target = &remembered;
                break;
            }
        }
        if (!target) {
            if (rememberedKeys.size() >= MaxRememberedKeys)
                rememberedKeys.erase(rememberedKeys.begin());
            RememberedKey remembered;
            remembered.parameters = previousParameters;
            remembered.kind = kind;
            rememberedKeys.push_back(std::move(remembered));
            target = &rememberedKeys.back();
        }
        target->secret.assign(secret.data(), secret.size());
    };
    rememberPrevious(RememberedKeyKind::Master, currentMaster);
    rememberPrevious(RememberedKeyKind::Recovery, recoverySecret);

    const QByteArray previousRecoveryNonce = recoveryNonce_;
    const QByteArray previousEncryptedRecovery = encryptedRecoverySeed_;
    parameters_ = nextParameters;
    error = persistDescriptor(nextMaster, recoverySecret);
    if (error.isError()) {
        parameters_ = previousParameters;
        recoveryNonce_ = previousRecoveryNonce;
        encryptedRecoverySeed_ = previousEncryptedRecovery;
        currentMaster.clear();
        nextMaster.clear();
        recoverySecret.clear();
        rememberedKeys.clear();
        return error;
    }

    issuedAtUtc_ = QDateTime::currentDateTimeUtc();
    expiresAtUtc_ = issuedAtUtc_.addDays(SessionDays);
    lastObservedUtc_ = issuedAtUtc_;
    active_ = true;
    error = sealSession(nextMaster, recoverySecret, rememberedKeys);
    currentMaster.clear();
    nextMaster.clear();
    recoverySecret.clear();
    rememberedKeys.clear();
    if (error.isError()) {
        // The descriptor is already atomically committed with the new
        // password. Never pretend it rolled back: remove a stale capsule and
        // require a fresh login with the new password.
        QFile::remove(capsulePath());
        active_ = false;
        return sessionError(
            VaultErrorCode::InternalError,
            QStringLiteral("主密码已更改，但无法建立新的本机会话。请使用新主密码重新登录。"),
            QStringLiteral("session.identity.rotate.capsule"));
    }
    return VaultError::success();
}

VaultError SecuritySessionService::verifyRecoverySecret(
    const SecureBuffer &recoverySecret)
{
    if (recoverySecret.size() != KeyBytes) {
        return sessionError(VaultErrorCode::InvalidRecoveryKey,
                            QStringLiteral("恢复密钥格式无效。"),
                            QStringLiteral("session.recovery.verify-size"));
    }

    SecurityOperationLease lease;
    VaultError error = acquireOperationLease(&lease);
    if (error.isError())
        return error;
    const bool matches = recoverySecret.equals(lease.recoverySecret().data(),
                                               lease.recoverySecret().size());
    lease.clear();
    if (!matches) {
        return sessionError(VaultErrorCode::InvalidRecoveryKey,
                            QStringLiteral("恢复密钥与当前本地身份不匹配。"),
                            QStringLiteral("session.recovery.verify-mismatch"));
    }

    const QDateTime previousVerifiedAt = recoveryVerifiedAtUtc_;
    recoveryVerifiedAtUtc_ = QDateTime::currentDateTimeUtc();
    error = writeDescriptor();
    if (error.isError())
        recoveryVerifiedAtUtc_ = previousVerifiedAt;
    return error;
}

VaultError SecuritySessionService::sealSession(
    const SecureBuffer &masterUnlockKey, const SecureBuffer &recoverySecret,
    const std::vector<RememberedKey> &rememberedKeys)
{
    if (masterUnlockKey.size() != KeyBytes || recoverySecret.size() != KeyBytes
        || rememberedKeys.size() > MaxRememberedKeys) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("安全会话密钥长度无效。"),
                            QStringLiteral("session.capsule.key-length"));
    }
    QByteArray plain;
    QDataStream stream(&plain, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    const QByteArray magic(CapsuleMagicV3);
    stream.writeRawData(magic.constData(), magic.size());
    stream << quint16(3) << parameters_.identityId << qint32(parameters_.keyVersion)
           << qint64(issuedAtUtc_.toMSecsSinceEpoch())
           << qint64(expiresAtUtc_.toMSecsSinceEpoch())
           << qint64(lastObservedUtc_.toMSecsSinceEpoch())
           << quint32(KeyBytes);
    stream.writeRawData(reinterpret_cast<const char *>(masterUnlockKey.data()), KeyBytes);
    stream << quint32(KeyBytes);
    stream.writeRawData(reinterpret_cast<const char *>(recoverySecret.data()), KeyBytes);
    stream << quint16(rememberedKeys.size());
    for (const RememberedKey &remembered : rememberedKeys) {
        if (remembered.secret.size() != KeyBytes
            || remembered.parameters.identityId.isEmpty()
            || remembered.parameters.identityId.size() > 128
            || remembered.parameters.keyVersion <= 0
            || remembered.parameters.kdfSalt.size() != KeyBytes) {
            scrub(plain);
            return sessionError(VaultErrorCode::InternalError,
                                QStringLiteral("历史会话密钥参数无效。"),
                                QStringLiteral("session.capsule.key-ring.invalid"));
        }
        stream << quint8(remembered.kind)
               << remembered.parameters.identityId
               << qint32(remembered.parameters.keyVersion)
               << remembered.parameters.kdfSalt
               << quint64(remembered.parameters.memoryKiB)
               << qint32(remembered.parameters.iterations)
               << qint32(remembered.parameters.parallelism)
               << quint32(KeyBytes);
        stream.writeRawData(reinterpret_cast<const char *>(remembered.secret.data()),
                            KeyBytes);
    }
    if (stream.status() != QDataStream::Ok) {
        scrub(plain);
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("无法序列化本机安全会话。"),
                            QStringLiteral("session.capsule.serialize"));
    }
    QByteArray protectedBytes;
    VaultError error = protectForCurrentUser(plain, SessionEntropy,
                                             L"Notepad Safe session",
                                             &protectedBytes);
    scrub(plain);
    if (error.isError())
        return error;
    QSaveFile file(capsulePath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(protectedBytes) != protectedBytes.size() || !file.commit()) {
        scrub(protectedBytes);
        return sessionError(VaultErrorCode::IoError,
                            QStringLiteral("无法保存本机安全会话。"),
                            QStringLiteral("session.capsule.write: ") + file.errorString());
    }
    scrub(protectedBytes);
    return VaultError::success();
}

VaultError SecuritySessionService::unsealSession(
    SecureBuffer *masterUnlockKey, SecureBuffer *recoverySecret,
    std::vector<RememberedKey> *rememberedKeys,
    bool removeInvalidCapsule)
{
    if (!masterUnlockKey || !recoverySecret) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("安全会话租约参数无效。"),
                            QStringLiteral("session.lease.arguments"));
    }
    masterUnlockKey->clear();
    recoverySecret->clear();
    if (rememberedKeys)
        rememberedKeys->clear();
    QFile file(capsulePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
        || file.size() > 128 * 1024) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("本机安全会话不存在，需要重新登录。"),
                            QStringLiteral("session.capsule.missing"));
    }
    QByteArray protectedBytes = file.readAll();
    QByteArray plain;
    VaultError error = unprotectForCurrentUser(protectedBytes, SessionEntropy, &plain);
    scrub(protectedBytes);
    if (error.isError()) {
        if (removeInvalidCapsule)
            QFile::remove(capsulePath());
        return error;
    }
    QByteArray master;
    QByteArray recovery;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime issued;
    QDateTime expires;
    QDateTime observed;
    bool decoded = false;
    std::vector<RememberedKey> decodedRememberedKeys;
    const QByteArray magicV3(CapsuleMagicV3);
    const QByteArray magicV2(CapsuleMagicV2);
    if (plain.startsWith(magicV3)) {
        QDataStream stream(plain);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::LittleEndian);
        QByteArray readMagic(magicV3.size(), Qt::Uninitialized);
        const int magicBytes = stream.readRawData(readMagic.data(), readMagic.size());
        quint16 version = 0;
        QString identityId;
        qint32 keyVersion = 0;
        qint64 issuedMs = 0;
        qint64 expiresMs = 0;
        qint64 observedMs = 0;
        quint32 masterSize = 0;
        quint32 recoverySize = 0;
        stream >> version >> identityId >> keyVersion
               >> issuedMs >> expiresMs >> observedMs >> masterSize;
        if (stream.status() == QDataStream::Ok && magicBytes == magicV3.size()
            && readMagic == magicV3 && version == 3
            && identityId == parameters_.identityId
            && keyVersion == parameters_.keyVersion && masterSize == KeyBytes) {
            master.resize(KeyBytes);
            if (stream.readRawData(master.data(), KeyBytes) == KeyBytes) {
                stream >> recoverySize;
                if (stream.status() == QDataStream::Ok && recoverySize == KeyBytes) {
                    recovery.resize(KeyBytes);
                    if (stream.readRawData(recovery.data(), KeyBytes) == KeyBytes) {
                        quint16 rememberedCount = 0;
                        stream >> rememberedCount;
                        bool ringValid = stream.status() == QDataStream::Ok
                                         && rememberedCount <= MaxRememberedKeys;
                        for (quint16 index = 0; ringValid && index < rememberedCount; ++index) {
                            quint8 kindValue = 0;
                            QString rememberedIdentity;
                            qint32 rememberedVersion = 0;
                            QByteArray rememberedSalt;
                            quint64 memoryKiB = 0;
                            qint32 iterations = 0;
                            qint32 parallelism = 0;
                            quint32 secretSize = 0;
                            stream >> kindValue >> rememberedIdentity >> rememberedVersion
                                   >> rememberedSalt >> memoryKiB >> iterations
                                   >> parallelism >> secretSize;
                            ringValid = stream.status() == QDataStream::Ok
                                        && (kindValue == quint8(RememberedKeyKind::Master)
                                            || kindValue == quint8(RememberedKeyKind::Recovery))
                                        && !rememberedIdentity.isEmpty()
                                        && rememberedIdentity.size() <= 128
                                        && rememberedVersion > 0
                                        && rememberedSalt.size() == KeyBytes
                                        && memoryKiB > 0 && iterations > 0
                                        && parallelism > 0 && secretSize == KeyBytes;
                            QByteArray rememberedSecret;
                            if (ringValid) {
                                rememberedSecret.resize(KeyBytes);
                                ringValid = stream.readRawData(rememberedSecret.data(),
                                                               KeyBytes) == KeyBytes;
                            }
                            if (ringValid) {
                                RememberedKey remembered;
                                remembered.kind = static_cast<RememberedKeyKind>(kindValue);
                                remembered.parameters.identityId = rememberedIdentity;
                                remembered.parameters.keyVersion = rememberedVersion;
                                remembered.parameters.kdfSalt = rememberedSalt;
                                remembered.parameters.memoryKiB = memoryKiB;
                                remembered.parameters.iterations = iterations;
                                remembered.parameters.parallelism = parallelism;
                                remembered.secret.assign(rememberedSecret.constData(),
                                                           rememberedSecret.size());
                                decodedRememberedKeys.push_back(std::move(remembered));
                            }
                            scrub(rememberedSecret);
                        }
                        decoded = ringValid && stream.status() == QDataStream::Ok
                                  && stream.atEnd();
                    }
                }
            }
        }
        issued = QDateTime::fromMSecsSinceEpoch(issuedMs, Qt::UTC);
        expires = QDateTime::fromMSecsSinceEpoch(expiresMs, Qt::UTC);
        observed = QDateTime::fromMSecsSinceEpoch(observedMs, Qt::UTC);
    } else if (plain.startsWith(magicV2)) {
        QDataStream stream(plain);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::LittleEndian);
        QByteArray readMagic(magicV2.size(), Qt::Uninitialized);
        const int magicBytes = stream.readRawData(readMagic.data(), readMagic.size());
        quint16 version = 0;
        QString identityId;
        qint32 keyVersion = 0;
        qint64 issuedMs = 0;
        qint64 expiresMs = 0;
        qint64 observedMs = 0;
        quint32 masterSize = 0;
        quint32 recoverySize = 0;
        stream >> version >> identityId >> keyVersion
               >> issuedMs >> expiresMs >> observedMs >> masterSize;
        if (stream.status() == QDataStream::Ok && magicBytes == magicV2.size()
            && readMagic == magicV2 && version == 2
            && identityId == parameters_.identityId
            && keyVersion == parameters_.keyVersion && masterSize == KeyBytes) {
            master.resize(KeyBytes);
            if (stream.readRawData(master.data(), KeyBytes) == KeyBytes) {
                stream >> recoverySize;
                if (stream.status() == QDataStream::Ok && recoverySize == KeyBytes) {
                    recovery.resize(KeyBytes);
                    decoded = stream.readRawData(recovery.data(), KeyBytes) == KeyBytes
                              && stream.status() == QDataStream::Ok && stream.atEnd();
                }
            }
        }
        issued = QDateTime::fromMSecsSinceEpoch(issuedMs, Qt::UTC);
        expires = QDateTime::fromMSecsSinceEpoch(expiresMs, Qt::UTC);
        observed = QDateTime::fromMSecsSinceEpoch(observedMs, Qt::UTC);
    } else {
        // Read-only migration path for v1 JSON capsules. A successful restore
        // immediately reseals as v3, so no new JSON secret copies are written.
        QJsonParseError parseError;
        const QJsonObject root = QJsonDocument::fromJson(plain, &parseError).object();
        issued = QDateTime::fromString(
            root.value(QStringLiteral("issuedAtUtc")).toString(), Qt::ISODateWithMs);
        expires = QDateTime::fromString(
            root.value(QStringLiteral("expiresAtUtc")).toString(), Qt::ISODateWithMs);
        observed = QDateTime::fromString(
            root.value(QStringLiteral("lastObservedUtc")).toString(), Qt::ISODateWithMs);
        decoded = parseError.error == QJsonParseError::NoError
                  && root.value(QStringLiteral("format")).toString()
                         == QLatin1String(CapsuleFormat)
                  && root.value(QStringLiteral("version")).toInt() == 1
                  && root.value(QStringLiteral("identityId")).toString()
                         == parameters_.identityId
                  && root.value(QStringLiteral("keyVersion")).toInt()
                         == parameters_.keyVersion
                  && decodeBase64(root.value(QStringLiteral("masterUnlockKey")).toString(),
                                  &master)
                  && decodeBase64(root.value(QStringLiteral("recoverySecret")).toString(),
                                  &recovery);
    }
    const bool valid = decoded && master.size() == KeyBytes && recovery.size() == KeyBytes
                       && issued.isValid() && expires.isValid() && observed.isValid()
                       && now < expires && now.addSecs(300) >= observed;
    scrub(plain);
    if (!valid) {
        scrub(master);
        scrub(recovery);
        decodedRememberedKeys.clear();
        if (removeInvalidCapsule)
            QFile::remove(capsulePath());
        return sessionError(VaultErrorCode::InvalidCredentials,
                            QStringLiteral("本机安全会话无效，需要重新登录。"),
                            QStringLiteral("session.capsule.invalid"));
    }
    masterUnlockKey->assign(master.constData(), master.size());
    recoverySecret->assign(recovery.constData(), recovery.size());
    scrub(master);
    scrub(recovery);
    if (rememberedKeys)
        *rememberedKeys = std::move(decodedRememberedKeys);
    else
        decodedRememberedKeys.clear();
    issuedAtUtc_ = issued;
    expiresAtUtc_ = expires;
    lastObservedUtc_ = observed;
    return VaultError::success();
}

bool SecuritySessionService::restoreSealedSession()
{
    clearMemory();
    if (!identityLoaded_ && !loadDescriptor())
        return false;
    SecureBuffer masterUnlockKey;
    SecureBuffer recoverySecret;
    std::vector<RememberedKey> rememberedKeys;
    VaultError error = unsealSession(&masterUnlockKey, &recoverySecret,
                                     &rememberedKeys, true);
    if (error.isError())
        return false;
    lastObservedUtc_ = QDateTime::currentDateTimeUtc();
    active_ = true;
    error = sealSession(masterUnlockKey, recoverySecret, rememberedKeys);
    rememberedKeys.clear();
    if (error.isError()) {
        clearMemory();
        return false;
    }
    return true;
}

VaultError SecuritySessionService::acquireOperationLease(
    SecurityOperationLease *lease)
{
    if (!lease) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("安全会话租约参数无效。"),
                            QStringLiteral("session.lease.output"));
    }
    lease->clear();
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，需要重新登录。"),
                            QStringLiteral("session.lease.inactive"));
    }
    SecureBuffer masterUnlockKey;
    SecureBuffer recoverySecret;
    std::vector<RememberedKey> rememberedKeys;
    VaultError error = unsealSession(&masterUnlockKey, &recoverySecret,
                                     &rememberedKeys, true);
    if (error.isError()) {
        clearMemory();
        return error;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!lastObservedUtc_.isValid() || lastObservedUtc_.secsTo(now) >= 15 * 60) {
        lastObservedUtc_ = now;
        error = sealSession(masterUnlockKey, recoverySecret, rememberedKeys);
        if (error.isError()) {
            rememberedKeys.clear();
            clearMemory();
            return error;
        }
    }
    rememberedKeys.clear();
    *lease = SecurityOperationLease(std::move(masterUnlockKey),
                                    std::move(recoverySecret));
    return VaultError::success();
}

VaultError SecuritySessionService::acquireRememberedKey(
    const SecurityIdentityParameters &parameters, RememberedKeyKind kind,
    SecureBuffer *secret)
{
    if (!secret) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("历史会话密钥输出参数无效。"),
                            QStringLiteral("session.key-ring.output"));
    }
    secret->clear();
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，需要重新登录。"),
                            QStringLiteral("session.key-ring.inactive"));
    }
    SecureBuffer currentMaster;
    SecureBuffer currentRecovery;
    std::vector<RememberedKey> rememberedKeys;
    VaultError error = unsealSession(&currentMaster, &currentRecovery,
                                     &rememberedKeys, true);
    if (error.isError()) {
        clearMemory();
        return error;
    }
    const auto sameParameters = [](const SecurityIdentityParameters &left,
                                   const SecurityIdentityParameters &right) {
        return left.identityId == right.identityId
               && left.keyVersion == right.keyVersion
               && left.kdfSalt == right.kdfSalt
               && left.memoryKiB == right.memoryKiB
               && left.iterations == right.iterations
               && left.parallelism == right.parallelism;
    };
    // The recovery seed belongs to the logical identity, not to one password
    // KDF version. Master-password rotation intentionally keeps this seed so
    // a fresh vN session can still open v1..vN-1 files without retaining all
    // old master passwords. A future recovery-seed rotation must introduce a
    // separate recovery generation instead of weakening this invariant.
    const bool currentIdentityRecovery = kind == RememberedKeyKind::Recovery
                                         && parameters.identityId
                                                == parameters_.identityId;
    if (sameParameters(parameters, parameters_) || currentIdentityRecovery) {
        const SecureBuffer &current = kind == RememberedKeyKind::Master
                                          ? currentMaster : currentRecovery;
        secret->assign(current.data(), current.size());
        rememberedKeys.clear();
        return VaultError::success();
    }
    for (const RememberedKey &remembered : rememberedKeys) {
        if (remembered.kind == kind
            && sameParameters(remembered.parameters, parameters)) {
            secret->assign(remembered.secret.data(), remembered.secret.size());
            rememberedKeys.clear();
            return VaultError::success();
        }
    }
    rememberedKeys.clear();
    return sessionError(VaultErrorCode::InvalidCredentials,
                        QStringLiteral("本次会话尚未解锁这个密码版本。"),
                        QStringLiteral("session.key-ring.miss"));
}

VaultError SecuritySessionService::rememberKey(
    const SecurityIdentityParameters &parameters, RememberedKeyKind kind,
    const SecureBuffer &secret)
{
    if (secret.size() != KeyBytes) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("历史会话密钥长度无效。"),
                            QStringLiteral("session.key-ring.key-length"));
    }
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，需要重新登录。"),
                            QStringLiteral("session.key-ring.inactive"));
    }
    SecureBuffer currentMaster;
    SecureBuffer currentRecovery;
    std::vector<RememberedKey> rememberedKeys;
    VaultError error = unsealSession(&currentMaster, &currentRecovery,
                                     &rememberedKeys, true);
    if (error.isError()) {
        clearMemory();
        return error;
    }
    const auto sameParameters = [](const SecurityIdentityParameters &left,
                                   const SecurityIdentityParameters &right) {
        return left.identityId == right.identityId
               && left.keyVersion == right.keyVersion
               && left.kdfSalt == right.kdfSalt
               && left.memoryKiB == right.memoryKiB
               && left.iterations == right.iterations
               && left.parallelism == right.parallelism;
    };
    if (!sameParameters(parameters, parameters_)) {
        RememberedKey *target = nullptr;
        for (RememberedKey &remembered : rememberedKeys) {
            if (remembered.kind == kind
                && sameParameters(remembered.parameters, parameters)) {
                target = &remembered;
                break;
            }
        }
        if (!target) {
            if (rememberedKeys.size() >= MaxRememberedKeys)
                rememberedKeys.erase(rememberedKeys.begin());
            RememberedKey remembered;
            remembered.parameters = parameters;
            remembered.kind = kind;
            rememberedKeys.push_back(std::move(remembered));
            target = &rememberedKeys.back();
        }
        target->secret.assign(secret.data(), secret.size());
    }
    error = sealSession(currentMaster, currentRecovery, rememberedKeys);
    rememberedKeys.clear();
    if (error.isError())
        clearMemory();
    return error;
}

VaultError SecuritySessionService::acquireRememberedMasterKey(
    const SecurityIdentityParameters &parameters, SecureBuffer *masterUnlockKey)
{
    return acquireRememberedKey(parameters, RememberedKeyKind::Master,
                                masterUnlockKey);
}

VaultError SecuritySessionService::acquireRememberedRecoverySecret(
    const SecurityIdentityParameters &parameters, SecureBuffer *recoverySecret)
{
    return acquireRememberedKey(parameters, RememberedKeyKind::Recovery,
                                recoverySecret);
}

VaultError SecuritySessionService::rememberMasterKey(
    const SecurityIdentityParameters &parameters,
    const SecureBuffer &masterUnlockKey)
{
    return rememberKey(parameters, RememberedKeyKind::Master, masterUnlockKey);
}

VaultError SecuritySessionService::rememberRecoverySecret(
    const SecurityIdentityParameters &parameters,
    const SecureBuffer &recoverySecret)
{
    return rememberKey(parameters, RememberedKeyKind::Recovery, recoverySecret);
}

VaultError SecuritySessionService::sealDocumentCredential(
    const SecureBuffer &credential, QByteArray *sealedCredential)
{
    if (!sealedCredential || credential.empty()) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("文档密钥保护参数无效。"),
                            QStringLiteral("session.document-credential.arguments"));
    }
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，需要重新登录。"),
                            QStringLiteral("session.document-credential.inactive"));
    }
    return protectForCurrentUser(viewOf(credential), DocumentCredentialEntropy,
                                 L"Notepad Safe document credential",
                                 sealedCredential);
}

VaultError SecuritySessionService::unsealDocumentCredential(
    const QByteArray &sealedCredential, SecureBuffer *credential)
{
    if (!credential || sealedCredential.isEmpty()) {
        return sessionError(VaultErrorCode::InternalError,
                            QStringLiteral("文档密钥解封参数无效。"),
                            QStringLiteral("session.document-credential.arguments"));
    }
    credential->clear();
    if (!validateActiveSession()) {
        return sessionError(VaultErrorCode::Locked,
                            QStringLiteral("安全会话已结束，需要重新登录。"),
                            QStringLiteral("session.document-credential.inactive"));
    }
    QByteArray plain;
    VaultError error = unprotectForCurrentUser(sealedCredential,
                                               DocumentCredentialEntropy, &plain);
    if (!error.isError())
        credential->assign(plain.constData(), plain.size());
    scrub(plain);
    return error;
}

bool SecuritySessionService::matches(
    const SecurityIdentityParameters &parameters)
{
    return validateActiveSession()
           && parameters.identityId == parameters_.identityId
           && parameters.keyVersion == parameters_.keyVersion
           && parameters.kdfSalt == parameters_.kdfSalt
           && parameters.memoryKiB == parameters_.memoryKiB
           && parameters.iterations == parameters_.iterations
           && parameters.parallelism == parameters_.parallelism;
}

void SecuritySessionService::clearMemory() noexcept
{
    active_ = false;
}

void SecuritySessionService::endSession(bool removeDeviceSession) noexcept
{
    clearMemory();
    if (removeDeviceSession)
        QFile::remove(capsulePath());
}
