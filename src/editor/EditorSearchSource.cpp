#include "EditorSearchSource.h"
#include "ReadOnlySearchSource.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

#include <QPointer>
#include <QThread>
#include <utility>

EditorSearchSource::EditorSearchSource(QObject *owner, std::function<Resources()> access)
    : QObject(owner), m_access(std::move(access)) {}

EditorSearchSource::~EditorSearchSource() { closeAndOwnerAlive(); }

bool EditorSearchSource::usesWindowed(const Resources &resources)
{
    return resources.ordinaryReadOnly && resources.windowed && resources.windowed->hasSource();
}

bool EditorSearchSource::usesWindowedSource() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return usesWindowed(m_access());
}

bool EditorSearchSource::isReady() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto resources = m_access();
    return usesWindowed(resources) ? m_readOnly && m_readOnly->isReady()
                                  : resources.mapped && resources.mapped->isOpen();
}

LargeFileSearch *EditorSearchSource::engine(bool navigation) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto resources = m_access();
    if (usesWindowed(resources))
        return !m_readOnly ? nullptr : (navigation
            ? m_readOnly->navigationEngine() : m_readOnly->findAllEngine());
    return !resources.mapped ? nullptr : (navigation
        ? resources.mapped->navigationSearchEngine() : resources.mapped->searchEngine());
}

bool EditorSearchSource::ensure(QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto resources = m_access();
    if (!usesWindowed(resources)) {
        const bool ready = resources.mapped && resources.mapped->isOpen();
        if (!ready && error) *error = tr("当前大文件搜索源不可用。");
        return ready;
    }
    const QPointer<EditorSearchSource> self(this);
    if (!resources.windowed->validateCurrent(error) || !self) return false;
    if (!m_readOnly) {
        m_readOnly = new ReadOnlySearchSource(this);
        connect(m_readOnly, &ReadOnlySearchSource::ready, this, [this] {
            const QPointer<EditorSearchSource> guard(this);
            const qint64 preparation = m_readOnly->preparationTaskId();
            QString error;
            const bool accepted = validateCurrent(&error);
            if (!guard || m_readOnly->preparationTaskId() != preparation) return;
            if (accepted) emit ready();
            else emit failed(error);
        });
        connect(m_readOnly, &ReadOnlySearchSource::failed, this, [this](const QString &message) {
            const QPointer<EditorSearchSource> guard(this);
            const qint64 preparation = m_readOnly->preparationTaskId();
            const auto before = m_access();
            const QPointer<WindowedReadOnlyProvider> source(before.windowed);
            const DiskFileStamp expected = source ? source->sourceStamp() : DiskFileStamp{};
            QString error;
            // A preparation may discover a change before the metadata poll.
            // Invalidate through the provider before settling the UI request.
            validateCurrent(&error);
            if (!guard || m_readOnly->preparationTaskId() != preparation || !source) return;
            const auto current = m_access();
            if (current.windowed != source || current.path != before.path
                || !usesWindowed(current) || !sameDiskFileStamp(source->sourceStamp(), expected)) return;
            emit failed(error.isEmpty() ? message : error);
        });
    }
    // submit may synchronously destroy both this adapter and its owner.
    return m_readOnly->ensureReady(resources.path, resources.windowed->readPolicy(),
                                   resources.windowed->sourceStamp(), error);
}

bool EditorSearchSource::validateCurrent(QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto resources = m_access();
    return !usesWindowed(resources) || resources.windowed->validateCurrent(error);
}

bool EditorSearchSource::closeAndOwnerAlive()
{
    const QPointer<QObject> owner(parent());
    // The legacy view continues to own its display mapping and close barrier.
    // Only the independent, owned search resource belongs to this adapter.
    if (m_readOnly) m_readOnly->close();
    return !owner.isNull();
}
