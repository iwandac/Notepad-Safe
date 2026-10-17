#include "MainMenu.h"

#include "AltMenuTracker.h"
#include "TitleBar.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>

#include <functional>

namespace {
void keepShortcuts(QWidget *window, QMenu *menu)
{
    for (QAction *action : menu->actions()) {
        if (action->menu())
            keepShortcuts(window, action->menu());
        else if (!action->isSeparator())
            window->addAction(action);
    }
}

void popupBelow(QToolButton *anchor, QMenu *menu)
{
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height())));
}
} // namespace

QMenu *installMainMenu(QWidget *window, QMenuBar *menuBar, TitleBar *titleBar)
{
    auto *mainMenu = new QMenu(window);
    mainMenu->setObjectName(QStringLiteral("menu.main"));
    for (const char *id : {"menu.file", "menu.edit", "menu.search", "menu.view",
                           "menu.encoding", "menu.language", "menu.tools", "menu.help"}) {
        if (auto *menu = window->findChild<QMenu *>(QString::fromLatin1(id)))
            mainMenu->addMenu(menu);
    }
    mainMenu->addSeparator();
    if (auto *lockNow = window->findChild<QAction *>(QStringLiteral("tools.lockNow")))
        mainMenu->addAction(lockNow);
    QAction *settings = mainMenu->addAction(QStringLiteral("设置"));
    settings->setObjectName(QStringLiteral("view.settings"));
    settings->setEnabled(false);

    for (QAction *top : menuBar->actions()) {
        if (top->menu())
            keepShortcuts(window, top->menu());
    }
    menuBar->hide();

    auto *palette = new QAction(QStringLiteral("命令面板"), window);
    palette->setObjectName(QStringLiteral("view.commandPalette"));
    palette->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    QObject::connect(palette, &QAction::triggered, titleBar,
                     &TitleBar::commandPaletteRequested);
    window->addAction(palette);

    QToolButton *anchor = titleBar->menuButton();
    QObject::connect(titleBar, &TitleBar::mainMenuRequested, mainMenu,
                     [anchor, mainMenu] { popupBelow(anchor, mainMenu); });
    new AltMenuController(window, [anchor, mainMenu] {
        popupBelow(anchor, mainMenu);
        return true;
    }, [anchor, mainMenu](QChar mnemonic) {
        for (QAction *top : mainMenu->actions()) {
            const QString text = top->text();
            const int amp = text.indexOf(QLatin1Char('&'));
            if (top->menu() && amp >= 0 && amp + 1 < text.size()
                && text.at(amp + 1).toUpper() == mnemonic) {
                popupBelow(anchor, top->menu());
                return true;
            }
        }
        return false;
    });
    return mainMenu;
}
