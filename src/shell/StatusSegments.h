#pragma once

#include <QToolButton>

#include <functional>

class QMenu;

// A flat 26 px status bar button; the application style sheet gives it
// padding and the hover color. A segment with a menu opens it on click.
class StatusSegment final : public QToolButton {
    Q_OBJECT

public:
    StatusSegment(const QString &objectName, const QString &accessibleName,
                  QWidget *parent = nullptr);
};

// The window's actions whose object names start with `prefix`, in creation
// order, as a menu. The actions are the same objects the main menu uses.
QMenu *actionMenu(QWidget *window, const QString &prefix, QWidget *parent);

// Menus for the encoding (display encodings plus 转换编码另存), line ending,
// language and zoom segments. onZoomChanged runs after a zoom action.
void installStatusMenus(QWidget *window, StatusSegment *encoding, StatusSegment *eol,
                        StatusSegment *language, StatusSegment *zoom,
                        const std::function<void()> &onZoomChanged);
