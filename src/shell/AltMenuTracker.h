#pragma once

#include <QChar>
#include <QObject>
#include <Qt>

#include <functional>

class QWidget;

// Keeps Alt working once the menu bar is hidden: tapping Alt alone opens the
// main menu (so does F10), Alt+letter opens the submenu with that mnemonic. Any other key
// or a mouse click while Alt is down cancels the tap.
class AltMenuTracker final {
public:
    enum class Result { None, OpenMainMenu, OpenSubmenu };
    struct Outcome {
        Result result = Result::None;
        QChar mnemonic;
    };

    Outcome keyPress(int key, Qt::KeyboardModifiers modifiers);
    Outcome keyRelease(int key, Qt::KeyboardModifiers modifiers);
    void reset() { m_armed = false; }

private:
    bool m_armed = false;
};

// Feeds application key and mouse events for one window into a tracker.
// Each handler returns whether it opened a menu; only then is the key used.
class AltMenuController final : public QObject {
    Q_OBJECT

public:
    AltMenuController(QWidget *window, std::function<bool()> openMainMenu,
                      std::function<bool(QChar)> openSubmenu);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_window = nullptr;
    std::function<bool()> m_openMainMenu;
    std::function<bool(QChar)> m_openSubmenu;
    AltMenuTracker m_tracker;
};
