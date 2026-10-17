#pragma once

class QMenu;
class QMenuBar;
class QWidget;
class TitleBar;

// Moves the window's menus behind the title bar's main menu button: builds
// the main menu (文件、编辑、搜索、视图、编码、语言、工具、帮助, then 立即锁定
// and a disabled 设置), keeps every menu action on the window so shortcuts
// still fire, hides the menu bar, adds Ctrl+K for the command palette and
// keeps Alt / Alt+letter opening the menus.
QMenu *installMainMenu(QWidget *window, QMenuBar *menuBar, TitleBar *titleBar);
