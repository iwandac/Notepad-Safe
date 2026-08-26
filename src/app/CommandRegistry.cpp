#include "CommandRegistry.h"

#include "FeatureRegistry.h"

#include <QAction>
#include <QObject>

void CommandRegistry::bind(QObject *root)
{
    actions_.clear();
    unboundCommandIds_.clear();
    if (!root)
        return;
    const auto discovered = root->findChildren<QAction *>();
    for (QAction *action : discovered) {
        const QString commandId = action->objectName();
        if (commandId.isEmpty())
            continue;
        const QString featureId = FeatureRegistry::featureIdForCommand(commandId);
        if (featureId.isEmpty()) {
            unboundCommandIds_.push_back(commandId);
            continue;
        }
        action->setProperty("commandId", commandId);
        action->setProperty("featureId", featureId);
        actions_[commandId].push_back(action);
    }
    unboundCommandIds_.removeDuplicates();
}

QList<QAction *> CommandRegistry::actions(const QString &commandId) const
{
    return actions_.value(commandId);
}

void CommandRegistry::setEnabled(const QString &commandId, bool enabled) const
{
    for (QAction *action : actions(commandId))
        action->setEnabled(enabled);
}

void CommandRegistry::applyContext(const CommandContext &context) const
{
    const bool document = context.hasDocument && context.document;
    setEnabled(QStringLiteral("edit.undo"), document && context.canUndo);
    setEnabled(QStringLiteral("edit.redo"), document && context.canRedo);
    setEnabled(QStringLiteral("edit.cut"), document && context.canModify
                                          && context.hasSelection);
    setEnabled(QStringLiteral("edit.copy"), document && context.hasSelection
        && context.document->capabilities.testFlag(DocumentCapability::Copy));
    setEnabled(QStringLiteral("edit.paste"), document && context.canModify
                                            && context.clipboardHasText);
    setEnabled(QStringLiteral("edit.selectAll"), document && !context.masked);

    setEnabled(QStringLiteral("file.save"),
               (document && context.modifiedOrUntitled)
                   || (context.accountsWorkspace && context.accountCanSave));
    setEnabled(QStringLiteral("file.saveAs"), document);
    setEnabled(QStringLiteral("file.reload"), document && context.reloadable);
    setEnabled(QStringLiteral("file.close"), document);
    setEnabled(QStringLiteral("file.closeAll"), context.canCloseAll);

    setEnabled(QStringLiteral("search.find"), document
        || (context.accountsWorkspace && context.accountCanSearch));
    setEnabled(QStringLiteral("search.replace"), document);
    setEnabled(QStringLiteral("view.panel.searchResults"), document);
}

bool CommandRegistry::allowedByDocument(const QString &commandId,
                                        const DocumentState *state,
                                        bool hasSelection)
{
    if (commandId.startsWith(QLatin1String("menu."))
        || commandId.startsWith(QLatin1String("view."))
        || commandId.startsWith(QLatin1String("help."))
        || commandId == QLatin1String("file.new")
        || commandId == QLatin1String("file.open")
        || commandId == QLatin1String("file.exit")
        || commandId.startsWith(QLatin1String("tools.")))
        return true;
    if (!state)
        return false;
    if (commandId == QLatin1String("edit.copy"))
        return hasSelection && state->capabilities.testFlag(DocumentCapability::Copy);
    if (commandId == QLatin1String("edit.cut"))
        return hasSelection && state->capabilities.testFlag(DocumentCapability::Edit);
    if (commandId.startsWith(QLatin1String("edit.")))
        return state->capabilities.testFlag(DocumentCapability::Edit);
    if (commandId.startsWith(QLatin1String("search."))
        || commandId.startsWith(QLatin1String("bookmark.")))
        return state->capabilities.testFlag(DocumentCapability::Search);
    if (commandId == QLatin1String("file.save")
        || commandId == QLatin1String("file.saveAs"))
        return state->capabilities.testFlag(DocumentCapability::Save);
    return state->capabilities.testFlag(DocumentCapability::Read);
}
