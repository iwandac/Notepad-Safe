#include "EditorTab.h"

#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

#include <limits>
#include <utility>

namespace {
void assignError(QString *target, QString message)
{
    if (target)
        *target = std::move(message);
}
}

QString EditorTab::autosavePath() const
{
    return isSecureDocument() ? m_documentSession.secureBackingPath()
                              : m_documentSession.recoveryPath();
}

bool EditorTab::createPersistenceSnapshot(qint64 maximumBytes,
                                          DocumentSnapshot *snapshot,
                                          QString *error)
{
    if (isL2Mode() && isBinaryDocument()
        && !bridgeFlushOverlayToPatches(error)) {
        return false;
    }
    // Locking first removes the interactive surface from view, then the
    // persistence transaction captures the exact revision it must save or
    // checkpoint.  This internal port is the only reader allowed during that
    // transition; public document-tool snapshots remain fail-closed.
    return createDocumentSnapshotImpl(maximumBytes, snapshot, error, true);
}

bool EditorTab::suspendPersistenceBacking(QString *error)
{
    Q_UNUSED(error)
    m_persistenceBackingSuspendedWithEdits = false;
    if (isL2Mode() && m_l2View && m_l2View->isOpen()) {
        if (m_l2View->isMemoryBacked()) {
            m_l2View->closeFile();
        } else {
            m_l2View->closeFile(true, true);
            m_persistenceBackingSuspendedWithEdits = true;
        }
    }
    return true;
}

bool EditorTab::resumePersistenceBacking(const QString &absolutePath,
                                         QString *error)
{
    if (!isL2Mode() || !m_l2View || m_l2View->isOpen())
        return true;
    const bool preserveEdits = m_persistenceBackingSuspendedWithEdits;
    const bool opened = preserveEdits
        ? m_l2View->openFilePreservingEdits(absolutePath, error)
        : m_l2View->openFile(absolutePath, error);
    if (!opened)
        return false;
    m_persistenceBackingSuspendedWithEdits = false;
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    return true;
}

void EditorTab::acceptPersistenceCommit(const QString &absolutePath,
                                        DiskFileVersion version)
{
    // The committed source already contains the snapshot; reopening it must
    // reset the old PatchList rather than applying those edits a second time.
    m_persistenceBackingSuspendedWithEdits = false;
    const bool wasSecure = isSecureDocument();
    m_documentSession.bindPlainFile(absolutePath, wasSecure);
    if (wasSecure) {
        m_documentSession.clearSecureTitle();
        m_documentSession.setSecureMasked(false);
        m_documentSession.setSecureTransitioning(false);
        m_documentSession.setSecurePayloadLimitExclusive(0);
        m_documentSession.clearSecureBackingPath();
    }
    m_documentSession.setDiskVersion(std::move(version));
    setModified(false);
    emit titleChanged(tabTitle());
}

void EditorTab::acceptSecurePersistenceCommit(const QUuid &documentId,
                                              const QString &title,
                                              const QString &absolutePath,
                                              const QString &backingPath)
{
    m_documentSession.bindSecureFile(documentId, absolutePath,
                                     m_documentSession.state().contentKind,
                                     DocumentOrigin::File);
    m_documentSession.setSecureTitle(title);
    m_documentSession.clearDiskVersion();
    setModified(false);
    m_documentSession.clearRecoveryPath();
    m_documentSession.setSecureBackingPath(backingPath);
    emit titleChanged(tabTitle());
}

bool EditorTab::createDocumentSnapshot(qint64 maximumBytes,
                                       DocumentSnapshot *snapshot,
                                       QString *error) const
{
    return createDocumentSnapshotImpl(maximumBytes, snapshot, error, false);
}

bool EditorTab::createDocumentSnapshotImpl(qint64 maximumBytes,
                                           DocumentSnapshot *snapshot,
                                           QString *error,
                                           bool allowSecureTransition) const
{
    if (!snapshot) {
        assignError(error, tr("文档快照输出为空。"));
        return false;
    }
    *snapshot = {};
    DocumentState state = documentState();
    if (allowSecureTransition && m_documentSession.secureTransitioning()
        && !m_documentSession.secureMasked()) {
        // documentState() intentionally removes every public capability while
        // the locking page is active.  Persistence still needs the immutable
        // domain identity and current dirty revision for its private capture.
        state = m_documentSession.state();
        state.dirty = isModified();
    }
    if (m_documentSession.secureMasked()
        || (m_documentSession.secureTransitioning() && !allowSecureTransition)
        || (!allowSecureTransition
            && !state.capabilities.testFlag(DocumentCapability::Read))) {
        assignError(error, tr("文档当前未解锁或不可读取。"));
        return false;
    }
    const qint64 size = contentSizeBytes();
    if (maximumBytes <= 0 || size < 0 || size > maximumBytes
        || size > std::numeric_limits<qsizetype>::max()) {
        assignError(error,
                    tr("文档大小 %1 字节超过本次工具快照上限 %2 字节；请缩小处理范围。")
                        .arg(size).arg(maximumBytes));
        return false;
    }

    DocumentSnapshot result;
    result.documentId = state.id;
    result.revision = state.revision;
    result.contentKind = state.contentKind;
    result.securityClass = state.securityClass;
    result.capabilities = state.capabilities;
    result.encodingName = encodingName(encoding());
    if (!isL2Mode() && state.contentKind == DocumentContentKind::Text) {
        result.text = m_editor->text();
        const qint64 utf8Size = result.text.toUtf8().size();
        if (utf8Size > maximumBytes) {
            assignError(error,
                        tr("编辑后的文本超过本次工具快照上限；请缩小处理范围。"));
            return false;
        }
        result.sourceRange = {{0}, {utf8Size}};
        result.encodingName = QStringLiteral("UTF-8 working text");
    } else {
        QByteArray source;
        if (m_windowedReadOnlyProvider) {
            source = m_windowedReadOnlyProvider->sourceBytes(
                0, static_cast<qsizetype>(size));
        } else if (m_l2View) {
            source = m_l2View->readByteRange(0, size);
        }
        if (source.size() != size) {
            assignError(error, tr("无法建立完整且一致的文档快照。"));
            return false;
        }
        result.sourceRange = {{0}, {size}};
        if (state.contentKind == DocumentContentKind::Binary)
            result.bytes = std::move(source);
        else
            result.text = decodeBytesWithEncoding(source, encoding()).text;
    }
    *snapshot = std::move(result);
    return true;
}
