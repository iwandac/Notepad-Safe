#pragma once

#include "SecureBuffer.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUuid>

inline const QString VaultSecureNoteTag = QStringLiteral("notepadsafe/secure-note");

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

// Full entry data is move-only because it can own secret fields.
struct VaultEntryData {
    QUuid id;
    QString title;
    QString username;
    QString url;
    QStringList tags;
    SecureBuffer passwordUtf8;
    SecureBuffer notesUtf8;
    SecureBuffer totpSeedUtf8;
};
