#include "DraftStore.h"

#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QUuid>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <wincrypt.h>
#endif

namespace {

constexpr char FileMagic[] = {'N', 'P', 'S', 'D', 'R', 'A', 'F', 'T', '\x1a'};
constexpr qsizetype FileMagicSize = sizeof(FileMagic);
constexpr quint32 WholePayloadVersion = 1;
constexpr quint32 ChunkedVersion = 2;
constexpr char PayloadMagic[] = "NotepadSafe.DraftStore.Payload.v1";
constexpr char ChunkMagic[] = "NotepadSafe.DraftStore.Chunk.v2";
constexpr char EntropyText[] = "NotepadSafe.DraftStore.DPAPI.v1";
constexpr quint64 MaximumSealedBytes = 64ULL * 1024ULL * 1024ULL;
constexpr qsizetype StreamIdBytes = 16;
constexpr qsizetype PlainChunkBytes = 1024 * 1024;
constexpr qint64 HeaderTotalOffset = FileMagicSize + sizeof(quint32) + sizeof(quint8)
                                     + sizeof(qint32) + StreamIdBytes;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

void scrub(QByteArray &bytes)
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

bool hasMagic(QIODevice *device)
{
    return device && device->isReadable()
           && device->peek(FileMagicSize) == QByteArray(FileMagic, FileMagicSize);
}

QByteArray entropyFor(const QByteArray &streamId = {})
{
    QByteArray entropy(EntropyText, sizeof(EntropyText));
    entropy.append(streamId);
    return entropy;
}

bool protectBytes(const QByteArray &plaintext, const QByteArray &entropyBytes,
                  QByteArray *sealed, QString *error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(plaintext)
    Q_UNUSED(entropyBytes)
    Q_UNUSED(sealed)
    setError(error, QStringLiteral("当前平台尚未提供本机加密恢复存储。"));
    return false;
#else
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.constData()))};
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
                      reinterpret_cast<BYTE *>(const_cast<char *>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!::CryptProtectData(&input, L"Notepad Safe recovery draft", &entropy,
                            nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        setError(error, QStringLiteral("无法加密恢复数据（DPAPI %1）。").arg(GetLastError()));
        return false;
    }
    *sealed = QByteArray(reinterpret_cast<const char *>(output.pbData),
                         static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return true;
#endif
}

bool unprotectBytes(const QByteArray &sealed, const QByteArray &entropyBytes,
                    QByteArray *plaintext, QString *error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(sealed)
    Q_UNUSED(entropyBytes)
    Q_UNUSED(plaintext)
    setError(error, QStringLiteral("当前平台尚未提供本机加密恢复存储。"));
    return false;
#else
    DATA_BLOB input{static_cast<DWORD>(sealed.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(sealed.constData()))};
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()),
                      reinterpret_cast<BYTE *>(const_cast<char *>(entropyBytes.constData()))};
    DATA_BLOB output{};
    if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        setError(error, QStringLiteral("恢复数据不能在当前 Windows 用户下解锁，或内容已损坏。"));
        return false;
    }
    *plaintext = QByteArray(reinterpret_cast<const char *>(output.pbData),
                            static_cast<qsizetype>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return true;
#endif
}

struct ChunkHeader {
    DraftStore::PayloadKind kind = DraftStore::PayloadKind::SmallOrJournal;
    int encoding = 0;
    QByteArray streamId;
    qint64 totalBytes = -1;
};

bool readChunkHeader(QFile *file, ChunkHeader *header, QString *error)
{
    if (!file || !header || !file->isOpen() || !hasMagic(file)) {
        setError(error, QStringLiteral("恢复快照格式无效。"));
        return false;
    }
    file->read(FileMagicSize);
    QDataStream stream(file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 version = 0;
    quint8 kind = 0;
    qint32 encoding = 0;
    stream >> version;
    if (version != ChunkedVersion) {
        setError(error, QStringLiteral("恢复快照不是分块检查点。"));
        return false;
    }
    stream >> kind >> encoding;
    header->streamId = file->read(StreamIdBytes);
    stream >> header->totalBytes;
    if (stream.status() != QDataStream::Ok
        || (kind != quint8(DraftStore::PayloadKind::SmallOrJournal)
            && kind != quint8(DraftStore::PayloadKind::FullCheckpoint)
            && kind != quint8(DraftStore::PayloadKind::BinaryPatchJournal))
        || encoding < 0 || encoding > 4
        || header->streamId.size() != StreamIdBytes || header->totalBytes < 0) {
        setError(error, QStringLiteral("分块恢复快照头损坏。"));
        return false;
    }
    header->kind = DraftStore::PayloadKind(kind);
    header->encoding = encoding;
    return true;
}

class ChunkEncryptingDevice final : public QIODevice {
public:
    ChunkEncryptingDevice(QSaveFile *file, QByteArray streamId, int encoding,
                          DraftStore::PayloadKind kind)
        : file_(file), streamId_(std::move(streamId)), entropy_(entropyFor(streamId_)),
          encoding_(encoding), kind_(kind)
    {
        open(QIODevice::WriteOnly);
    }

    ~ChunkEncryptingDevice() override { scrub(pending_); }

    bool finalize(QString *error)
    {
        if (failed_) {
            setError(error, error_);
            return false;
        }
        if (!pending_.isEmpty() && !sealDataChunk(pending_, error))
            return false;
        scrub(pending_);
        QByteArray terminal;
        QDataStream payload(&terminal, QIODevice::WriteOnly);
        payload.setByteOrder(QDataStream::LittleEndian);
        payload << QByteArray(ChunkMagic, sizeof(ChunkMagic)) << streamId_
                << quint8(kind_) << qint32(encoding_)
                << chunkIndex_ << quint8(2) << totalBytes_;
        if (payload.status() != QDataStream::Ok || !writeSealedRecord(terminal, error)) {
            scrub(terminal);
            return false;
        }
        scrub(terminal);
        return true;
    }

    qint64 totalBytes() const { return totalBytes_; }

protected:
    qint64 readData(char *, qint64) override { return -1; }

    qint64 writeData(const char *data, qint64 size) override
    {
        if (failed_ || size < 0)
            return -1;
        qint64 consumed = 0;
        while (consumed < size) {
            const qsizetype take = qMin<qint64>(PlainChunkBytes - pending_.size(),
                                                size - consumed);
            pending_.append(data + consumed, take);
            consumed += take;
            if (pending_.size() == PlainChunkBytes) {
                QString localError;
                if (!sealDataChunk(pending_, &localError)) {
                    failed_ = true;
                    error_ = localError;
                    return -1;
                }
                scrub(pending_);
            }
        }
        return size;
    }

private:
    bool sealDataChunk(const QByteArray &chunk, QString *error)
    {
        QByteArray payloadBytes;
        QDataStream payload(&payloadBytes, QIODevice::WriteOnly);
        payload.setByteOrder(QDataStream::LittleEndian);
        payload << QByteArray(ChunkMagic, sizeof(ChunkMagic)) << streamId_
                << quint8(kind_) << qint32(encoding_)
                << chunkIndex_ << quint8(1) << chunk;
        if (payload.status() != QDataStream::Ok || !writeSealedRecord(payloadBytes, error)) {
            scrub(payloadBytes);
            return false;
        }
        scrub(payloadBytes);
        totalBytes_ += chunk.size();
        ++chunkIndex_;
        return true;
    }

    bool writeSealedRecord(const QByteArray &payload, QString *error)
    {
        QByteArray sealed;
        if (!protectBytes(payload, entropy_, &sealed, error))
            return false;
        QDataStream stream(file_);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << quint32(sealed.size());
        const bool ok = stream.status() == QDataStream::Ok
                        && file_->write(sealed) == sealed.size();
        if (!ok)
            setError(error, QStringLiteral("写入加密恢复数据块失败。"));
        return ok;
    }

    QSaveFile *file_ = nullptr;
    QByteArray streamId_;
    QByteArray entropy_;
    QByteArray pending_;
    quint64 chunkIndex_ = 0;
    qint64 totalBytes_ = 0;
    bool failed_ = false;
    QString error_;
    int encoding_ = 0;
    DraftStore::PayloadKind kind_ = DraftStore::PayloadKind::FullCheckpoint;
};

bool writeChunkedDraft(const QString &path, int encoding,
                       DraftStore::PayloadKind kind,
                       const DraftStore::PlaintextWriter &writer, QString *error)
{
    if (!writer) {
        setError(error, QStringLiteral("恢复检查点写入器无效。"));
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(FileMagic, FileMagicSize) != FileMagicSize) {
        setError(error, QStringLiteral("无法建立加密恢复检查点：%1").arg(file.errorString()));
        return false;
    }
    const QByteArray streamId = QUuid::createUuid().toRfc4122();
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    header << ChunkedVersion << quint8(kind) << qint32(encoding);
    file.write(streamId);
    header << qint64(0);
    if (header.status() != QDataStream::Ok) {
        file.cancelWriting();
        setError(error, QStringLiteral("写入恢复检查点头失败。"));
        return false;
    }

    ChunkEncryptingDevice device(&file, streamId, encoding, kind);
    QString writerError;
    if (!writer(&device, &writerError) || !device.finalize(&writerError)) {
        file.cancelWriting();
        setError(error, writerError.isEmpty() ? QStringLiteral("生成恢复检查点失败。")
                                               : writerError);
        return false;
    }
    const qint64 end = file.pos();
    if (!file.seek(HeaderTotalOffset)) {
        file.cancelWriting();
        setError(error, QStringLiteral("无法完成恢复检查点长度记录。"));
        return false;
    }
    QDataStream patch(&file);
    patch.setByteOrder(QDataStream::LittleEndian);
    patch << device.totalBytes();
    if (patch.status() != QDataStream::Ok || !file.seek(end) || !file.commit()) {
        file.cancelWriting();
        setError(error, QStringLiteral("原子提交恢复检查点失败：%1").arg(file.errorString()));
        return false;
    }
    return true;
}

} // namespace

bool DraftStore::isEncryptedDraft(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && hasMagic(&file);
}

bool DraftStore::writeSnapshot(const QString &path, const QByteArray &plaintext,
                               QString *error)
{
    if (plaintext.size() >= MaximumPlaintextBytes) {
        setError(error, QStringLiteral("恢复快照必须小于 32 MiB。"));
        return false;
    }
    QByteArray payload(PayloadMagic, sizeof(PayloadMagic));
    payload.append(plaintext);
    QByteArray sealed;
    const bool protectedOk = protectBytes(payload, entropyFor(), &sealed, error);
    scrub(payload);
    if (!protectedOk)
        return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(FileMagic, FileMagicSize) != FileMagicSize) {
        setError(error, QStringLiteral("无法建立加密恢复快照：%1").arg(file.errorString()));
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << WholePayloadVersion << quint64(sealed.size());
    if (stream.status() != QDataStream::Ok || file.write(sealed) != sealed.size()
        || !file.commit()) {
        file.cancelWriting();
        setError(error, QStringLiteral("原子提交加密恢复快照失败：%1").arg(file.errorString()));
        return false;
    }
    return true;
}

bool DraftStore::writeCheckpoint(const QString &path, int encoding,
                                 const PlaintextWriter &writer, QString *error)
{
    return writeChunkedDraft(path, encoding, PayloadKind::FullCheckpoint,
                             writer, error);
}

bool DraftStore::writeBinaryPatchJournal(const QString &path,
                                         const PlaintextWriter &writer,
                                         QString *error)
{
    return writeChunkedDraft(path, 0, PayloadKind::BinaryPatchJournal,
                             writer, error);
}

bool DraftStore::inspect(const QString &path, SnapshotInfo *info, QString *error)
{
    if (!info) {
        setError(error, QStringLiteral("恢复快照信息参数无效。"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !hasMagic(&file)) {
        setError(error, QStringLiteral("恢复快照格式无效。"));
        return false;
    }
    file.read(FileMagicSize);
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 version = 0;
    stream >> version;
    if (version == WholePayloadVersion) {
        info->kind = PayloadKind::SmallOrJournal;
        info->plaintextBytes = -1;
        info->encoding = 0;
        info->formatVersion = int(version);
        return stream.status() == QDataStream::Ok;
    }
    file.seek(0);
    ChunkHeader header;
    if (!readChunkHeader(&file, &header, error))
        return false;
    info->kind = header.kind;
    info->plaintextBytes = header.totalBytes;
    info->encoding = header.encoding;
    info->formatVersion = int(ChunkedVersion);
    return true;
}

bool DraftStore::readChunks(const QString &path, const ChunkConsumer &consumer,
                            QString *error)
{
    if (!consumer) {
        setError(error, QStringLiteral("恢复数据消费者无效。"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("无法打开恢复检查点。"));
        return false;
    }
    ChunkHeader header;
    if (!readChunkHeader(&file, &header, error))
        return false;
    const QByteArray entropy = entropyFor(header.streamId);
    quint64 expectedIndex = 0;
    qint64 delivered = 0;
    bool terminalSeen = false;
    while (!file.atEnd()) {
        QDataStream record(&file);
        record.setByteOrder(QDataStream::LittleEndian);
        quint32 sealedSize = 0;
        record >> sealedSize;
        if (record.status() != QDataStream::Ok || sealedSize == 0
            || sealedSize > PlainChunkBytes + 64 * 1024
            || qint64(sealedSize) > file.bytesAvailable()) {
            setError(error, QStringLiteral("恢复检查点数据块长度无效。"));
            return false;
        }
        QByteArray sealed = file.read(sealedSize);
        QByteArray payload;
        if (!unprotectBytes(sealed, entropy, &payload, error))
            return false;
        QDataStream decoded(payload);
        decoded.setByteOrder(QDataStream::LittleEndian);
        QByteArray magic;
        QByteArray streamId;
        quint8 payloadKind = 0;
        qint32 payloadEncoding = 0;
        quint64 index = 0;
        quint8 recordKind = 0;
        decoded >> magic >> streamId >> payloadKind >> payloadEncoding >> index >> recordKind;
        if (decoded.status() != QDataStream::Ok
            || magic != QByteArray(ChunkMagic, sizeof(ChunkMagic))
            || streamId != header.streamId || payloadKind != quint8(header.kind)
            || payloadEncoding != header.encoding || index != expectedIndex) {
            scrub(payload);
            setError(error, QStringLiteral("恢复检查点数据块顺序或绑定无效。"));
            return false;
        }
        if (recordKind == 1) {
            QByteArray chunk;
            decoded >> chunk;
            if (decoded.status() != QDataStream::Ok || chunk.isEmpty() || !decoded.atEnd()) {
                scrub(payload);
                scrub(chunk);
                setError(error, QStringLiteral("恢复检查点明文块损坏。"));
                return false;
            }
            QString consumerError;
            const bool accepted = consumer(chunk, &consumerError);
            delivered += chunk.size();
            scrub(chunk);
            scrub(payload);
            if (!accepted) {
                setError(error, consumerError.isEmpty() ? QStringLiteral("恢复数据消费者拒绝数据块。")
                                                        : consumerError);
                return false;
            }
            ++expectedIndex;
        } else if (recordKind == 2) {
            qint64 terminalTotal = -1;
            decoded >> terminalTotal;
            const bool valid = decoded.status() == QDataStream::Ok && decoded.atEnd()
                               && terminalTotal == delivered
                               && terminalTotal == header.totalBytes && file.atEnd();
            scrub(payload);
            if (!valid) {
                setError(error, QStringLiteral("恢复检查点结束记录或总长度无效。"));
                return false;
            }
            terminalSeen = true;
            break;
        } else {
            scrub(payload);
            setError(error, QStringLiteral("恢复检查点包含未知记录类型。"));
            return false;
        }
    }
    if (!terminalSeen) {
        setError(error, QStringLiteral("恢复检查点不完整，缺少结束记录。"));
        return false;
    }
    return true;
}

bool DraftStore::readSnapshot(const QString &path, QByteArray *plaintext,
                              QString *error)
{
    if (!plaintext) {
        setError(error, QStringLiteral("恢复快照输出参数无效。"));
        return false;
    }
    plaintext->clear();
    SnapshotInfo info;
    if (!inspect(path, &info, error))
        return false;
    if (info.formatVersion == int(ChunkedVersion)) {
        if (info.plaintextBytes >= MaximumPlaintextBytes) {
            setError(error, QStringLiteral("分块恢复检查点过大，必须流式恢复。"));
            return false;
        }
        return readChunks(path, [plaintext](const QByteArray &chunk, QString *) {
            plaintext->append(chunk);
            return true;
        }, error);
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)
        || file.read(FileMagicSize) != QByteArray(FileMagic, FileMagicSize)) {
        setError(error, QStringLiteral("恢复快照格式无效。"));
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 version = 0;
    quint64 sealedSize = 0;
    stream >> version >> sealedSize;
    if (stream.status() != QDataStream::Ok || version != WholePayloadVersion
        || sealedSize == 0 || sealedSize > MaximumSealedBytes
        || sealedSize != quint64(file.bytesAvailable())) {
        setError(error, QStringLiteral("恢复快照版本、长度或完整性无效。"));
        return false;
    }
    QByteArray sealed = file.read(static_cast<qint64>(sealedSize));
    QByteArray payload;
    if (!unprotectBytes(sealed, entropyFor(), &payload, error))
        return false;
    const QByteArray prefix(PayloadMagic, sizeof(PayloadMagic));
    if (!payload.startsWith(prefix)
        || payload.size() - prefix.size() >= MaximumPlaintextBytes) {
        scrub(payload);
        setError(error, QStringLiteral("恢复快照明文标识或大小无效。"));
        return false;
    }
    *plaintext = payload.mid(prefix.size());
    scrub(payload);
    return true;
}
