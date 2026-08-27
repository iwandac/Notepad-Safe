#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

enum class SecureDocumentPayloadKind {
    Text,
    Binary,
};

// Backend-neutral transfer object used at the application/security boundary.
// It describes a decrypted in-memory document, not KDBX implementation state.
struct SecureDocumentSnapshot {
    QUuid documentId;
    QString filePath;
    QString backingPath;
    QString title;
    QString text;
    QByteArray binary;
    SecureDocumentPayloadKind payloadKind = SecureDocumentPayloadKind::Text;
    bool draft = false;
    bool recoveredUnsaved = false;
};
