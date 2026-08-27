#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QUuid>

struct SessionTab {
    static constexpr int MaximumPersistedSyntaxValue = 22;
    QString filePath;
    QString encoding;
    int syntax = 0;
    int cursorLine = 0;
    int cursorIndex = 0;
    qint64 cursorByteOffset = -1;
    qint64 selectionStartByte = -1;
    qint64 selectionEndByte = -1;
    bool modified = false;
    QString autosavePath;
    bool secureDocument = false;
    QString documentId;
    QString presentation;
    bool splitView = false;
    QString splitOrientation;
};

enum class SessionRecoveryKind {
    SmallOrJournal,
    FullCheckpoint,
    BinaryPatchJournal,
};

struct SessionRecoveryPayload {
    SessionRecoveryKind kind = SessionRecoveryKind::SmallOrJournal;
    int encoding = 0;
    QByteArray bytes;
};

struct SessionRestoreRequest {
    SessionRecoveryKind kind = SessionRecoveryKind::SmallOrJournal;
    QString encryptedPath;
    QString realFilePath;
    int encoding = 0;
    qint64 plaintextBytes = 0;
    QByteArray plaintext;
    bool legacyPlaintextFile = false;
};

// The session runtime sees documents only through this use-case-oriented port.
// Widget, Scintilla and mmap details remain inside the editor adapter.
class ISessionDocumentPort {
public:
    virtual ~ISessionDocumentPort() = default;

    virtual SessionTab captureSessionTab() const = 0;
    virtual QUuid sessionInstanceId() const = 0;
    virtual quint64 sessionRevision() const = 0;
    virtual bool needsRecoverySnapshot() const = 0;
    virtual bool isEmptyUntitledDocument() const = 0;
    virtual bool isLargeRecoveryDocument() const = 0;
    virtual QString recoveryPath() const = 0;
    virtual void acceptRecoveryPath(const QString &path) = 0;
    virtual bool captureRecoveryPayload(SessionRecoveryPayload *payload,
                                        QString *error) = 0;
    virtual bool restoreSessionRecovery(const SessionRestoreRequest &request,
                                        QString *error) = 0;
};
