#pragma once

#include "SecureBuffer.h"

#include <QList>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUuid>

inline const QString VaultSecureNoteTag = QStringLiteral("notepadsafe/secure-note");
inline const QString VaultSecureDocumentPayloadAttachment =
    QStringLiteral("notepadsafe.payload.bin");
inline const QString VaultSecureDocumentTextAttachment =
    QStringLiteral("notepadsafe.text-utf8.bin");

enum class VaultState {
    Locked,
    Unlocking,
    Open,
    Saving,
    Faulted
};

enum class VaultErrorCode {
    None,
    Locked,
    InvalidCredentials,
    InvalidRecoveryKey,
    CorruptKeyEnvelope,
    UnsupportedFormat,
    CorruptDatabase,
    IoError,
    Cancelled,
    BackendUnavailable,
    InternalError
};

struct VaultError {
    VaultErrorCode code = VaultErrorCode::None;
    QString userMessage;
    QString diagnosticCode;

    bool isError() const { return code != VaultErrorCode::None; }
    static VaultError success() { return {}; }
};

struct VaultEntrySummary {
    QUuid id;
    QString title;
    QString username;
    QString url;
    QStringList tags;
    bool secureNote = false;
    bool hasPassword = false;
};

struct VaultEntryHistorySummary {
    int index = -1;
    QDateTime modifiedAtUtc;
    QString title;
    QString username;
    bool hasPassword = false;
};

// Non-secret, short-lived presentation data. The seed never crosses this API.
struct VaultTotpSnapshot {
    QString code;
    int remainingSeconds = 0;
};

enum class VaultPasswordQuality {
    Empty,
    Bad,
    Poor,
    Weak,
    Good,
    Excellent
};

struct VaultPasswordAssessment {
    VaultPasswordQuality quality = VaultPasswordQuality::Empty;
    double entropyBits = 0.0;
};

// Full entry data is move-only because it can own secret fields.
struct VaultEntryData {
    QUuid id;
    QString title;
    QString username;
    QString url;
    QStringList tags;
    SecureBuffer passwordUtf8;
    SecureBuffer notesUtf8;
    // Reserved raw payload used by a standalone Notepad Safe binary document.
    // Normal account updates leave attachments untouched; secure-note updates
    // explicitly synchronize this one attachment through the presence flag.
    SecureBuffer binaryPayload;
    bool binaryPayloadPresent = false;
    // Exact UTF-8 mirror for standalone secure text documents. KDBX XML
    // normalizes CR/LF in Notes, so Notes remains the interoperable KeePassXC
    // view while this encrypted attachment preserves editor byte semantics.
    SecureBuffer textPayloadUtf8;
    bool textPayloadPresent = false;
    SecureBuffer totpSeedUtf8;
    bool hasPassword = false;
    bool hasTotp = false;
    // Account edits use KeePassXC Entry History. High-frequency standalone
    // secure-document autosaves explicitly disable it to avoid unbounded
    // history growth every 1.2 seconds.
    bool recordHistory = true;
};

// Secrets are requested separately from presentation metadata. The object is
// move-only through SecureBuffer and is expected to live for one UI action or
// one explicit edit session only.
struct VaultEntrySecrets {
    SecureBuffer passwordUtf8;
    SecureBuffer totpSeedUtf8;
};
