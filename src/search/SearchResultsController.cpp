#include "SearchResultsController.h"

#include <QUuid>
#include <limits>

SearchResultsController::SearchResultsController(QObject *parent)
    : QObject(parent)
{
}

SearchResultsController::~SearchResultsController()
{
    disconnectTargets();
}

void SearchResultsController::disconnectTargets()
{
    for (const auto &connection : std::as_const(connections_))
        QObject::disconnect(connection);
    connections_.clear();
}

void SearchResultsController::clear()
{
    disconnectTargets();
    multiFileMode_ = false;
    multiPattern_.clear();
    multiMatchCount_ = 0;
    multiStaleCount_ = 0;
    multiIncompleteCount_ = 0;
    multiTargets_.clear();
    targetRevisions_.clear();
    singleTarget_.clear();
    singleRevision_ = 0;
    rerunTarget_.clear();
}

void SearchResultsController::beginSingle(EditorTab *target)
{
    clear();
    if (!target)
        return;
    singleTarget_ = target;
    singleRevision_ = target->contentRevision();
    watch(target, {});
}

void SearchResultsController::beginMulti(const QString &pattern)
{
    clear();
    multiFileMode_ = true;
    multiPattern_ = pattern;
}

QString SearchResultsController::ensureMultiTarget(EditorTab *target)
{
    const QString existing = targetId(target);
    if (!existing.isEmpty() || !target)
        return existing;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    multiTargets_.insert(id, target);
    targetRevisions_.insert(id, target->contentRevision());
    watch(target, id);
    return id;
}

QString SearchResultsController::targetId(EditorTab *target) const
{
    for (auto it = multiTargets_.cbegin(); it != multiTargets_.cend(); ++it) {
        if (it.value() == target)
            return it.key();
    }
    return {};
}

EditorTab *SearchResultsController::target(const QString &targetId) const
{
    return multiTargets_.value(targetId).data();
}

QVector<EditorTab *> SearchResultsController::targets() const
{
    QVector<EditorTab *> result;
    result.reserve(multiTargets_.size());
    for (const auto &target : multiTargets_) {
        if (target)
            result.push_back(target.data());
    }
    return result;
}

bool SearchResultsController::isCurrent(EditorTab *target,
                                        const QString &targetId) const
{
    if (!target)
        return false;
    if (targetId.isEmpty())
        return singleTarget_ == target
            && singleRevision_ == target->contentRevision();
    return multiTargets_.value(targetId) == target
        && targetRevisions_.value(targetId,
             std::numeric_limits<quint64>::max()) == target->contentRevision();
}

void SearchResultsController::invalidateRevision(const QString &targetId)
{
    targetRevisions_.remove(targetId);
}

void SearchResultsController::watch(EditorTab *target,
                                    const QString &targetId)
{
    QPointer<EditorTab> guard(target);
    connections_.push_back(connect(
        target, &EditorTab::contentChanged, this,
        [this, guard](quint64) {
            if (guard)
                emit targetContentChanged(guard.data());
        }));
    connections_.push_back(connect(
        target, &EditorTab::l2LineBuildFinished, this,
        [this, guard](bool ok) {
            if (ok && guard)
                emit targetDecorationReady(guard.data());
        }));
    connections_.push_back(connect(
        target, &QObject::destroyed, this,
        [this, targetId]() {
            const bool wasMulti = multiFileMode_ && !targetId.isEmpty();
            if (wasMulti) {
                multiTargets_.remove(targetId);
                targetRevisions_.remove(targetId);
            } else {
                singleTarget_.clear();
                singleRevision_ = 0;
            }
            emit targetClosed(targetId, wasMulti);
        }));
}
