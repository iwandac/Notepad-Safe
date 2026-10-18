#include "AltMenuTracker.h"

#include <QApplication>
#include <QKeyEvent>
#include <QWidget>

#include <utility>

AltMenuTracker::Outcome AltMenuTracker::keyPress(int key, Qt::KeyboardModifiers modifiers)
{
    const Qt::KeyboardModifiers others = modifiers & ~(Qt::AltModifier | Qt::KeypadModifier);
    if (key == Qt::Key_Alt) {
        m_armed = others == Qt::NoModifier;
        return {};
    }
    m_armed = false;
    if (key == Qt::Key_F10 && (modifiers & ~Qt::KeypadModifier) == Qt::NoModifier)
        return {Result::OpenMainMenu, {}};
    if (modifiers.testFlag(Qt::AltModifier) && others == Qt::NoModifier
        && key >= Qt::Key_A && key <= Qt::Key_Z) {
        return {Result::OpenSubmenu, QChar(u'A' + (key - Qt::Key_A))};
    }
    return {};
}

AltMenuTracker::Outcome AltMenuTracker::keyRelease(int key, Qt::KeyboardModifiers)
{
    if (key != Qt::Key_Alt || !m_armed)
        return {};
    m_armed = false;
    return {Result::OpenMainMenu, {}};
}

AltMenuController::AltMenuController(QWidget *window,
                                     std::function<bool()> openMainMenu,
                                     std::function<bool(QChar)> openSubmenu)
    : QObject(window), m_window(window), m_openMainMenu(std::move(openMainMenu)),
      m_openSubmenu(std::move(openSubmenu))
{
    qApp->installEventFilter(this);
}

bool AltMenuController::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseButtonPress || type == QEvent::FocusOut) {
        m_tracker.reset();
        return false;
    }
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease)
        return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    // Only keys aimed at this window; popups and dialogs keep their own Alt.
    if (!widget || !m_window || widget->window() != m_window
        || QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->isAutoRepeat())
        return false;
    const AltMenuTracker::Outcome outcome = type == QEvent::KeyPress
        ? m_tracker.keyPress(key->key(), key->modifiers())
        : m_tracker.keyRelease(key->key(), key->modifiers());
    switch (outcome.result) {
    case AltMenuTracker::Result::OpenMainMenu:
        return m_openMainMenu && m_openMainMenu();
    case AltMenuTracker::Result::OpenSubmenu:
        return m_openSubmenu && m_openSubmenu(outcome.mnemonic);
    case AltMenuTracker::Result::None:
        break;
    }
    return false;
}
