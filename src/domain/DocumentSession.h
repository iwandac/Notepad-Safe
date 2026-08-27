#pragma once

#include "DiskFileVersion.h"
#include "DocumentState.h"

#include <QString>
#include <utility>

// Single non-visual owner for a document's identity and lifecycle metadata.
// Views may project this state but must not keep parallel copies of it.
class DocumentSession final {
public:
    const DocumentState &state() const noexcept { return state_; }

    void setDirty(bool dirty) noexcept { state_.dirty = dirty; }
    void setContentKind(DocumentContentKind kind) noexcept
    {
        state_.contentKind = kind;
    }
    void setLocation(QString path)
    {
        state_.location = std::move(path);
    }
    void bindPlainFile(QString path, bool renewIdentity = false)
    {
        if (renewIdentity || state_.id.isNull())
            state_.id = QUuid::createUuid();
        state_.location = std::move(path);
        state_.origin = state_.location.isEmpty() ? DocumentOrigin::Untitled
                                                  : DocumentOrigin::File;
        state_.persistence = state_.location.isEmpty()
            ? DocumentPersistence::None : DocumentPersistence::PlainFile;
        state_.securityClass = DocumentSecurityClass::OrdinaryDocument;
    }
    void bindSecureFile(const QUuid &id, QString path,
                        DocumentContentKind contentKind,
                        DocumentOrigin origin = DocumentOrigin::SecureStore)
    {
        state_.id = id;
        state_.location = std::move(path);
        state_.origin = origin;
        state_.persistence = DocumentPersistence::SecureFile;
        state_.securityClass = DocumentSecurityClass::SecureDocument;
        state_.contentKind = contentKind;
    }
    quint64 advanceRevision() noexcept { return ++state_.revision; }

    const DiskFileVersion &diskVersion() const noexcept { return diskVersion_; }
    void setDiskVersion(DiskFileVersion version) { diskVersion_ = std::move(version); }
    void clearDiskVersion() noexcept { diskVersion_ = {}; }

    const QString &recoveryPath() const noexcept { return recoveryPath_; }
    void setRecoveryPath(QString path) { recoveryPath_ = std::move(path); }
    void clearRecoveryPath() { recoveryPath_.clear(); }

    const QString &secureBackingPath() const noexcept { return secureBackingPath_; }
    void setSecureBackingPath(QString path) { secureBackingPath_ = std::move(path); }
    void clearSecureBackingPath() { secureBackingPath_.clear(); }

    const QString &secureTitle() const noexcept { return secureTitle_; }
    void setSecureTitle(QString title) { secureTitle_ = std::move(title); }
    void clearSecureTitle()
    {
        if (!secureTitle_.isEmpty())
            secureTitle_.fill(QChar(0));
        secureTitle_.clear();
    }

    bool secureMasked() const noexcept { return secureMasked_; }
    void setSecureMasked(bool masked) noexcept { secureMasked_ = masked; }
    bool secureTransitioning() const noexcept { return secureTransitioning_; }
    void setSecureTransitioning(bool transitioning) noexcept
    {
        secureTransitioning_ = transitioning;
    }

    qint64 securePayloadLimitExclusive() const noexcept
    {
        return securePayloadLimitExclusive_;
    }
    void setSecurePayloadLimitExclusive(qint64 limit) noexcept
    {
        securePayloadLimitExclusive_ = limit;
    }

private:
    DocumentState state_;
    DiskFileVersion diskVersion_;
    QString recoveryPath_;
    QString secureBackingPath_;
    QString secureTitle_;
    bool secureMasked_ = false;
    bool secureTransitioning_ = false;
    qint64 securePayloadLimitExclusive_ = 0;
};
