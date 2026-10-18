#include "CommandPaletteModel.h"

#include <QRegularExpression>

#include <algorithm>

namespace {
constexpr int EmptyQueryPerGroup = 5;

// "保存(&S)" -> "保存", "&Find" -> "Find"; "&&" stays a literal ampersand.
QString withoutMnemonic(const QString &input)
{
    static const QRegularExpression suffix(QStringLiteral("\\(&[^&)]\\)"));
    const QString text = QString(input).remove(suffix);
    QString result;
    result.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                result += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        result += text.at(i);
    }
    return result;
}
} // namespace

void CommandPaletteModel::setEntries(QVector<PaletteEntry> entries)
{
    beginResetModel();
    m_entries = std::move(entries);
    for (PaletteEntry &entry : m_entries)
        entry.title = withoutMnemonic(entry.title);
    m_visible.clear();
    endResetModel();
}

void CommandPaletteModel::setQuery(const QString &query)
{
    beginResetModel();
    m_visible.clear();
    const QString needle = withoutMnemonic(query).trimmed();
    if (needle.isEmpty()) {
        for (const auto group : {PaletteEntry::Group::OpenDocument,
                                 PaletteEntry::Group::Command,
                                 PaletteEntry::Group::Recent}) {
            int shown = 0;
            for (int i = 0; i < m_entries.size() && shown < EmptyQueryPerGroup; ++i) {
                if (m_entries.at(i).group == group) {
                    m_visible.push_back(i);
                    ++shown;
                }
            }
        }
    } else {
        struct Match { int rank; int group; int index; };
        QVector<Match> matches;
        for (int i = 0; i < m_entries.size(); ++i) {
            const QString &title = m_entries.at(i).title;
            const int at = title.indexOf(needle, 0, Qt::CaseInsensitive);
            if (at >= 0)
                matches.push_back({at == 0 ? 0 : 1, int(m_entries.at(i).group), i});
        }
        std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
            return a.rank != b.rank ? a.rank < b.rank : a.group < b.group;
        });
        for (const Match &match : matches)
            m_visible.push_back(match.index);
    }
    endResetModel();
}

bool CommandPaletteModel::activate(int row)
{
    if (row < 0 || row >= m_visible.size())
        return false;
    const PaletteEntry &entry = m_entries.at(m_visible.at(row));
    if (!entry.enabled || !entry.activate)
        return false;
    // The action may rebuild the palette's entries; run a copy.
    const std::function<void()> run = entry.activate;
    run();
    return true;
}

int CommandPaletteModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_visible.size());
}

QVariant CommandPaletteModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_visible.size())
        return {};
    const PaletteEntry &entry = m_entries.at(m_visible.at(index.row()));
    switch (role) {
    case Qt::DisplayRole: return entry.title;
    case SubtitleRole: return entry.subtitle;
    case ShortcutRole: return entry.shortcut;
    case GroupRole: return int(entry.group);
    case EnabledRole: return entry.enabled;
    default: return {};
    }
}

Qt::ItemFlags CommandPaletteModel::flags(const QModelIndex &index) const
{
    const Qt::ItemFlags base = QAbstractListModel::flags(index);
    if (!index.isValid() || index.row() >= m_visible.size())
        return base;
    return m_entries.at(m_visible.at(index.row())).enabled ? base : base & ~Qt::ItemIsEnabled;
}
