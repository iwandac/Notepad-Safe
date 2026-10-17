#pragma once

#include "ThemeTokens.h"

#include <QColor>
#include <QTabBar>
#include <QTabWidget>

#include <functional>

struct TabVisualState {
    enum class Kind { Plain, Secure, ReadOnly, Recovery };
    Kind kind = Kind::Plain;
    bool dirty = false;
    bool secureLocked = false;
};

struct TabPaintSpec {
    QColor background, text, spine, icon;
    bool showDirtyDot = false;
    bool showClose = true;
    int fontWeight = 400;
};

// How one tab looks. The spine is drawn only on the active secure tab; a
// dirty tab shows a dot in place of its close button until hovered.
TabPaintSpec tabPaintSpec(const TabVisualState &s, bool active, bool hovered,
                          const ThemeTokens &t);

// A tab bar painted from tokens: 34 px tabs, rounded top on the active tab,
// a security spine, kind icons and a dirty dot.
class DocumentTabBar final : public QTabBar {
    Q_OBJECT

public:
    using StateProvider = std::function<TabVisualState(int index)>;

    explicit DocumentTabBar(QWidget *parent = nullptr);

    void setTokens(const ThemeTokens &tokens);
    void setStateProvider(StateProvider provider);
    // Re-reads every tab's state, e.g. after a save or a lock.
    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize tabSizeHint(int index) const override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void tabInserted(int index) override;

private:
    TabVisualState stateFor(int index) const;
    void updateCloseButtons();

    ThemeTokens m_tokens = ThemeTokens::light();
    StateProvider m_provider;
    int m_hovered = -1;
};

// QTabWidget with a DocumentTabBar installed.
class DocumentTabWidget final : public QTabWidget {
    Q_OBJECT

public:
    explicit DocumentTabWidget(QWidget *parent = nullptr);
    DocumentTabBar *documentTabBar() const { return m_bar; }

private:
    DocumentTabBar *m_bar = nullptr;
};
