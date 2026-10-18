#include "FindLiveSearch.h"
#include "DocumentSearchSurface.h"

#include <algorithm>
#include <utility>

FindLiveSearch::FindLiveSearch(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(DebounceMs);
    connect(&m_timer, &QTimer::timeout, this, &FindLiveSearch::run);
}

bool FindLiveSearch::eligible(DocumentSearchSurface *tab)
{
    if (!tab || tab->isL2Mode() || tab->isHexPresentation() || tab->isBinaryDocument()
        || tab->isSecureDocumentMasked() || tab->isSecureDocumentTransitioning())
        return false;
    const qint64 bytes = tab->searchCursor().documentBytes;
    return bytes >= 0 && bytes <= MaxDocumentBytes;
}

void FindLiveSearch::schedule(DocumentSearchSurface *tab, const Query &query)
{
    if (tab != m_tab)
        reset();
    m_tab = tab;
    m_query = query;
    m_timer.start();
}

void FindLiveSearch::cancel()
{
    m_timer.stop();
    ++m_generation;
    m_pending = false;
    if (m_inFlight && m_tab)
        m_tab->cancelFindAllL2();
    m_inFlight = false;
}

void FindLiveSearch::reset()
{
    cancel();
    clearHighlights();
    m_matches.clear();
    m_error.clear();
    m_index = -1;
    m_truncated = false;
    m_resultsValid = false;
    emit updated();
}

void FindLiveSearch::clearHighlights()
{
    if (m_ownsHighlights && m_tab)
        m_tab->setSearchHighlights({});
    m_ownsHighlights = false;
}

void FindLiveSearch::run()
{
    if (m_suspended)
        return;
    if (!eligible(m_tab) || m_query.pattern.isEmpty()) {
        const QPointer<DocumentSearchSurface> tab = m_tab;
        reset();
        m_tab = tab;
        return;
    }
    if (m_inFlight) {
        m_pending = true;
        return;
    }
    const int generation = ++m_generation;
    m_inFlight = true;
    m_revision = m_tab->contentRevision();
    const bool started = m_tab->findAllL1Async(
        m_query.pattern, m_query.regex, m_query.caseSensitive, m_query.wholeWord, this,
        [this, generation](QVector<SearchMatch> matches, bool truncated, QString error) {
            finish(generation, std::move(matches), truncated, std::move(error));
        });
    // Another Find All owns the document; the next pause tries again.
    if (!started && generation == m_generation)
        m_inFlight = false;
}

void FindLiveSearch::finish(int generation, QVector<SearchMatch> matches,
                            bool truncated, QString error)
{
    if (generation != m_generation)
        return;
    m_inFlight = false;
    if (m_pending) {
        m_pending = false;
        run();
        return;
    }
    if (!m_tab)
        return;
    clearHighlights();
    m_error = error;
    m_matches = error.isEmpty() ? std::move(matches) : QVector<SearchMatch>{};
    m_truncated = truncated;
    m_resultsValid = true;
    m_index = -1;
    if (!m_matches.isEmpty()) {
        m_tab->setSearchHighlights(m_matches);
        m_ownsHighlights = true;
        const qint64 anchor = qMax<qint64>(0, m_tab->searchCursor().selectionStart);
        const auto first = std::lower_bound(
            m_matches.cbegin(), m_matches.cend(), anchor,
            [](const SearchMatch &match, qint64 offset) { return match.byteOffset < offset; });
        m_index = first == m_matches.cend() ? 0 : int(first - m_matches.cbegin());
        const SearchMatch &hit = m_matches.at(m_index);
        m_tab->selectSearchHit(hit.byteOffset, hit.byteLength);
    }
    emit updated();
}

void FindLiveSearch::refreshPosition()
{
    if (!m_tab || m_query.pattern.isEmpty() || !m_resultsValid
        || m_inFlight || m_timer.isActive())
        return;
    if (m_tab->contentRevision() != m_revision) {
        m_timer.stop();
        run();
        return;
    }
    const qint64 start = m_tab->searchCursor().selectionStart;
    const auto found = std::lower_bound(
        m_matches.cbegin(), m_matches.cend(), start,
        [](const SearchMatch &match, qint64 offset) { return match.byteOffset < offset; });
    m_index = found != m_matches.cend() && found->byteOffset == start
        ? int(found - m_matches.cbegin()) : -1;
    emit updated();
}

QString FindLiveSearch::positionText() const
{
    if (!m_tab || m_query.pattern.isEmpty())
        return {};
    if (!m_error.isEmpty())
        return m_error;
    if (m_matches.isEmpty())
        return m_inFlight || m_timer.isActive() ? QString() : QStringLiteral("未找到匹配");
    const QString total = m_truncated ? QStringLiteral("%1+").arg(m_matches.size())
                                      : QString::number(m_matches.size());
    return m_index >= 0 ? QStringLiteral("第 %1 / %2 处").arg(m_index + 1).arg(total)
                        : QStringLiteral("%1 处").arg(total);
}
