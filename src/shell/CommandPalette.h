#pragma once

#include "CommandPaletteModel.h"
#include "ThemeTokens.h"

#include <QFrame>

class QLineEdit;
class QListView;

// The Ctrl+K popup: a query field over the palette list. Enter runs the
// current entry, Esc closes, Up/Down move. 620 px wide, 64 px below the top
// of the window, 44 px rows.
class CommandPalette final : public QFrame {
    Q_OBJECT

public:
    explicit CommandPalette(QWidget *window);

    void setTokens(const ThemeTokens &tokens);
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
    CommandPaletteModel *m_model = nullptr;
    QLineEdit *m_query = nullptr;
    QListView *m_list = nullptr;
};
