#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <functional>

struct PaletteEntry {
    enum class Group { OpenDocument, Command, Recent };
    Group group = Group::Command;
    QString title;
    QString subtitle;
    QString shortcut;
    bool enabled = true;
    std::function<void()> activate;
};

// Entries for the Ctrl+K palette, filtered by a query. Matching ignores case
// and & mnemonics; titles starting with the query rank above titles that
// only contain it, and within a rank the groups stay in order (documents,
// commands, recent files). An empty query lists at most five per group.
class CommandPaletteModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role { SubtitleRole = Qt::UserRole + 1, ShortcutRole, GroupRole, EnabledRole };

    using QAbstractListModel::QAbstractListModel;

    void setEntries(QVector<PaletteEntry> entries);
    void setQuery(const QString &query);
    // Runs the entry's action; a disabled entry does nothing and returns false.
    bool activate(int row);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    QVector<PaletteEntry> m_entries;
    QVector<int> m_visible;
};
