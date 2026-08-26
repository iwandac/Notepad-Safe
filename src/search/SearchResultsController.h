#pragma once

#include "EditorTab.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>

// Owns the lifetime and revision contract of a presented search result set.
// Views render models; they do not decide whether a result is still valid.
class SearchResultsController final : public QObject {
    Q_OBJECT
public:
    explicit SearchResultsController(QObject *parent = nullptr);
    ~SearchResultsController() override;

    void clear();
    void beginSingle(EditorTab *target);
    void beginMulti(const QString &pattern);

    bool isMultiFile() const noexcept { return multiFileMode_; }
    QString pattern() const { return multiPattern_; }
    qint64 matchCount() const noexcept { return multiMatchCount_; }
    int staleCount() const noexcept { return multiStaleCount_; }
    int incompleteCount() const noexcept { return multiIncompleteCount_; }
    void addMatches(qint64 count) noexcept { multiMatchCount_ += count; }
    void markIncomplete() noexcept { ++multiIncompleteCount_; }
    void markStale() noexcept { ++multiStaleCount_; }

    EditorTab *singleTarget() const noexcept { return singleTarget_.data(); }
    EditorTab *rerunTarget() const noexcept { return rerunTarget_.data(); }
    void setRerunTarget(EditorTab *target) { rerunTarget_ = target; }
    QVector<EditorTab *> targets() const;

    QString ensureMultiTarget(EditorTab *target);
    QString targetId(EditorTab *target) const;
    EditorTab *target(const QString &targetId) const;
    bool isCurrent(EditorTab *target,
                   const QString &targetId = QString()) const;
    void invalidateRevision(const QString &targetId);

signals:
    void targetContentChanged(EditorTab *target);
    void targetDecorationReady(EditorTab *target);
    void targetClosed(const QString &targetId, bool multiFile);

private:
    void disconnectTargets();
    void watch(EditorTab *target, const QString &targetId);

    bool multiFileMode_ = false;
    QString multiPattern_;
    qint64 multiMatchCount_ = 0;
    int multiStaleCount_ = 0;
    int multiIncompleteCount_ = 0;
    QHash<QString, QPointer<EditorTab>> multiTargets_;
    QHash<QString, quint64> targetRevisions_;
    QPointer<EditorTab> singleTarget_;
    QPointer<EditorTab> rerunTarget_;
    quint64 singleRevision_ = 0;
    QVector<QMetaObject::Connection> connections_;
};
