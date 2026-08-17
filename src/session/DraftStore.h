#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

class QIODevice;

// Internal crash-recovery storage for ordinary S1 documents.
//
// This is deliberately not a user document format.  The payload is sealed for
// the current Windows user with DPAPI, is versioned, and is committed through
// QSaveFile.  Copying an .ndraft file to another account/device does not expose
// its plaintext and cannot be used as a substitute for Save As.
class DraftStore final {
public:
    enum class PayloadKind : quint8 {
        SmallOrJournal = 1,
        FullCheckpoint = 2,
        BinaryPatchJournal = 3,
    };
    struct SnapshotInfo {
        PayloadKind kind = PayloadKind::SmallOrJournal;
        qint64 plaintextBytes = -1;
        int encoding = 0;
        int formatVersion = 0;
    };
    using PlaintextWriter = std::function<bool(QIODevice *, QString *)>;
    using ChunkConsumer = std::function<bool(const QByteArray &, QString *)>;

    static constexpr qint64 MaximumPlaintextBytes = 32LL * 1024 * 1024;

    static bool isEncryptedDraft(const QString &path);
    static bool writeSnapshot(const QString &path, const QByteArray &plaintext,
                              QString *error = nullptr);
    static bool readSnapshot(const QString &path, QByteArray *plaintext,
                             QString *error = nullptr);
    static bool writeCheckpoint(const QString &path, int encoding,
                                const PlaintextWriter &writer,
                                QString *error = nullptr);
    static bool writeBinaryPatchJournal(const QString &path,
                                        const PlaintextWriter &writer,
                                        QString *error = nullptr);
    static bool inspect(const QString &path, SnapshotInfo *info,
                        QString *error = nullptr);
    static bool readChunks(const QString &path, const ChunkConsumer &consumer,
                           QString *error = nullptr);

private:
    DraftStore() = delete;
};
