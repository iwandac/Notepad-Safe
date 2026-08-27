#pragma once

#include <QByteArray>
#include <QFlags>
#include <QString>
#include <QUuid>

// Positions are deliberately not interchangeable. SourceBytePosition addresses
// the immutable source payload; EditorPosition addresses a rendered editor.
struct SourceBytePosition {
    qint64 value = 0;
};

struct EditorPosition {
    qint64 value = 0;
};

struct SourceByteRange {
    SourceBytePosition start;
    SourceBytePosition end;

    bool isValid() const noexcept
    {
        return start.value >= 0 && end.value >= start.value;
    }
};

enum class DocumentOrigin {
    Untitled,
    File,
    Recovery,
    SecureStore,
};

enum class DocumentPersistence {
    None,
    PlainFile,
    SecureFile,
};

enum class DocumentContentKind {
    Text,
    Binary,
};

enum class DocumentSecurityClass {
    OrdinaryDocument,
    SecureDocument,
    AccountSecret,
};

enum class DocumentCapability : quint32 {
    None = 0,
    Read = 1u << 0,
    Edit = 1u << 1,
    Search = 1u << 2,
    Copy = 1u << 3,
    Save = 1u << 4,
    AnalyzeLocally = 1u << 5,
    AnalyzeRemotelyWithConsent = 1u << 6,
};
Q_DECLARE_FLAGS(DocumentCapabilities, DocumentCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(DocumentCapabilities)

struct DocumentState {
    QUuid id = QUuid::createUuid();
    QString location;
    DocumentOrigin origin = DocumentOrigin::Untitled;
    DocumentPersistence persistence = DocumentPersistence::None;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    DocumentSecurityClass securityClass = DocumentSecurityClass::OrdinaryDocument;
    // Capabilities are an observed runtime fact, not a permissive default.
    // The active document surface must populate them from its current state;
    // a detached/default state is deliberately denied until proven available.
    DocumentCapabilities capabilities = DocumentCapability::None;
    quint64 revision = 0;
    bool dirty = false;
};

struct DocumentSnapshot {
    QUuid documentId;
    quint64 revision = 0;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    DocumentSecurityClass securityClass = DocumentSecurityClass::OrdinaryDocument;
    DocumentCapabilities capabilities = DocumentCapability::None;
    QString encodingName;
    QString text;
    QByteArray bytes;
    SourceByteRange sourceRange;

    bool mayEnterDocumentTools() const noexcept
    {
        return securityClass != DocumentSecurityClass::AccountSecret
            && capabilities.testFlag(DocumentCapability::Read);
    }
};
