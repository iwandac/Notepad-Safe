#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>
#include "SearchMatch.h"

class DocumentSearchSurface;

// Search-as-you-type for ordinary editable text. A short pause after typing
// runs one background Find All on the current document, highlights every hit,
// selects the first one from the caret and keeps "match i of n" current.
// Large, windowed and hex documents are not eligible: they search on Enter.
class FindLiveSearch : public QObject {
    Q_OBJECT
public:
    struct Query {
        QString pattern;
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWord = false;
    };

    static constexpr qint64 MaxDocumentBytes = 4 * 1024 * 1024;
    static constexpr int DebounceMs = 150;

    explicit FindLiveSearch(QObject *parent = nullptr);

    static bool eligible(DocumentSearchSurface *tab);
    void schedule(DocumentSearchSurface *tab, const Query &query);
    // Drops the pending run and any task in flight; results already shown stay.
    void cancel();
    // cancel() plus forgetting results and the highlights this object drew.
    void reset();
    // Find All drew its own highlights; closing the panel must keep them.
    void releaseHighlights() { m_ownsHighlights = false; }
    void setSuspended(bool suspended) { m_suspended = suspended; }
    // Re-reads the selection after navigation; reruns when the text changed.
    void refreshPosition();
    QString positionText() const;
    bool active() const { return m_tab && !m_query.pattern.isEmpty(); }

signals:
    void updated();

private:
    QTimer m_timer;
    QPointer<DocumentSearchSurface> m_tab;
    Query m_query;
    QVector<SearchMatch> m_matches;
    QString m_error;
    quint64 m_revision = 0;
    int m_generation = 0;
    int m_index = -1;
    bool m_truncated = false;
    bool m_inFlight = false;
    bool m_pending = false;
    bool m_suspended = false;
    bool m_ownsHighlights = false;
    bool m_resultsValid = false;

    void run();
    void finish(int generation, QVector<SearchMatch> matches, bool truncated,
                QString error);
    void clearHighlights();
};
