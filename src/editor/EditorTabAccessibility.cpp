#include "EditorTab.h"

#include <QEvent>
#include <QKeyEvent>

bool EditorTab::handleHexAccessibleFocusTraversal(QEvent *event)
{
    if (!event || event->type() != QEvent::KeyPress)
        return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    const bool plainTab = key->key() == Qt::Key_Tab
        && !(key->modifiers()
             & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (!plainTab && key->key() != Qt::Key_Backtab)
        return false;
    if (key->key() == Qt::Key_Backtab
        || key->modifiers().testFlag(Qt::ShiftModifier)) {
        focusPreviousChild();
    } else {
        focusNextChild();
    }
    return true;
}
