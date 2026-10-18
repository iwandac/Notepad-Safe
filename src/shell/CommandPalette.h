#pragma once

#include "CommandPaletteModel.h"
#include "ThemeTokens.h"

#include <QFrame>

class QLineEdit;
class QListView;
class QMenu;

// Every named, visible command action of the window (no menus, no tab
// context actions, not the palette itself), once per command id.
QVector<PaletteEntry> commandPaletteEntries(QWidget *window);
// The recent-file actions of a recent-files menu.
QVector<PaletteEntry> recentFileEntries(QMenu *recentMenu);

// The Ctrl+K popup: a query field over the palette list. Enter runs the
// current entry, Esc closes, Up/Down move. 620 px wide, 64 px below the top
// of the window, 44 px rows.
class CommandPalette final : public QFrame {
    Q_OBJECT

public:
    explicit CommandPalette(QWidget *window);

    // Without styling (Windows high contrast) the palette uses the platform palette.
    void setTokens(const ThemeTokens &tokens, bool styled = true);
    void open(QVector<PaletteEntry> entries);
    CommandPaletteModel *model() const { return m_model; }
    QLineEdit *queryEdit() const { return m_query; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void activateCurrent();
    void applyStyle();

    QWidget *m_window = nullptr;
    ThemeTokens m_tokens = ThemeTokens::light();
    bool m_styled = true;
    CommandPaletteModel *m_model = nullptr;
    QLineEdit *m_query = nullptr;
    QListView *m_list = nullptr;
};
