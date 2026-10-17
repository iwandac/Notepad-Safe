#include "StatusSegments.h"

#include <QAction>
#include <QMenu>

StatusSegment::StatusSegment(const QString &objectName, const QString &accessibleName,
                             QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(objectName);
    setAccessibleName(accessibleName);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setPopupMode(QToolButton::InstantPopup);
    setFixedHeight(26);
}

QMenu *actionMenu(QWidget *window, const QString &prefix, QWidget *parent)
{
    auto *menu = new QMenu(parent);
    for (QAction *action : window->findChildren<QAction *>()) {
        if (action->objectName().startsWith(prefix))
            menu->addAction(action);
    }
    return menu;
}

void installStatusMenus(QWidget *window, StatusSegment *encoding, StatusSegment *eol,
                        StatusSegment *language, StatusSegment *zoom,
                        const std::function<void()> &onZoomChanged)
{
    QMenu *encodingMenu = actionMenu(window, QStringLiteral("encoding.display."), encoding);
    encodingMenu->addSeparator();
    QMenu *convert = actionMenu(window, QStringLiteral("encoding.convert."), encodingMenu);
    convert->setTitle(QStringLiteral("转换编码另存"));
    encodingMenu->addMenu(convert);
    encoding->setMenu(encodingMenu);

    eol->setMenu(actionMenu(window, QStringLiteral("encoding.eol."), eol));
    language->setMenu(actionMenu(window, QStringLiteral("language."), language));

    QMenu *zoomMenu = actionMenu(window, QStringLiteral("view.zoom."), zoom);
    for (QAction *action : zoomMenu->actions())
        QObject::connect(action, &QAction::triggered, zoom, [onZoomChanged] { onZoomChanged(); });
    zoom->setMenu(zoomMenu);
}
