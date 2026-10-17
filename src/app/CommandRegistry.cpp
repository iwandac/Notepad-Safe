#include "CommandRegistry.h"

#include "FeatureRegistry.h"

#include <QAction>
#include <QObject>
#include <QSignalBlocker>

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

void CommandRegistry::setChecked(const QString &commandId, bool checked) const
{
    for (QAction *action : actions(commandId)) {
        const QSignalBlocker blocker(action);
        action->setChecked(checked);
    }
}

void CommandRegistry::applyContext(const CommandContext &context) const
{
    const bool document = context.hasDocument && context.document;
    // Establish the common document-workspace baseline first.  Specialized
    // capabilities below then override it.  Keeping this policy beside the
    // command registry prevents menu, toolbar and shortcut surfaces from
    // drifting into separate enablement implementations.
    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        const QString &id = it.key();
        const bool documentScoped = id == QLatin1String("menu.edit")
            || id == QLatin1String("menu.language")
            || id == QLatin1String("menu.encoding")
            || id.startsWith(QLatin1String("edit."))
            || id.startsWith(QLatin1String("language."))
            || id.startsWith(QLatin1String("bookmark."))
            || id == QLatin1String("search.replace")
            || id == QLatin1String("search.next")
            || id == QLatin1String("search.previous")
            || id.startsWith(QLatin1String("search.brace."))
            || id == QLatin1String("search.bookmarks")
            || id == QLatin1String("view.wordWrap")
            || id.startsWith(QLatin1String("view.symbols"))
            || id.startsWith(QLatin1String("view.folding"))
            || id.startsWith(QLatin1String("view.edge"))
            || id.startsWith(QLatin1String("view.zoom"))
            || id.startsWith(QLatin1String("encoding."));
        if (documentScoped) {
            for (QAction *action : it.value())
                action->setEnabled(document);
        }
    }
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
    // Replace is a text mutation command. Binary/Hex targets (including the
    // permanently read-only B2/B3 providers) must not advertise a top-level
    // command that the real dialog then has to reject. Keep Find available.
    setEnabled(QStringLiteral("search.replace"),
               document && context.canModify
                   && context.document->contentKind == DocumentContentKind::Text);
    setEnabled(QStringLiteral("view.panel.searchResults"), document);

    setEnabled(QStringLiteral("view.wordWrap"),
               document && !context.readOnlyByPolicy);
    const bool tail = document && context.tailAvailable && !context.accountsWorkspace
        && allowedByDocument(QStringLiteral("view.tail.toggle"), context.document);
    setEnabled(QStringLiteral("view.tail.toggle"), tail);
    setChecked(QStringLiteral("view.tail.toggle"), tail && context.tailEnabled);
    setEnabled(QStringLiteral("view.tail.resume"), tail && context.tailCanResume);
    setEnabled(QStringLiteral("edit.hexInsertMode"), context.editableHex);
    setChecked(QStringLiteral("edit.hexInsertMode"),
               context.editableHex && context.hexInsertMode);

    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        const QString &id = it.key();
        bool override = false;
        bool enabled = false;
        if (id.startsWith(QLatin1String("edit.line."))
            || id.startsWith(QLatin1String("edit.multiSelect."))
            || id == QLatin1String("edit.selection.upper")
            || id == QLatin1String("edit.selection.lower")
            || id == QLatin1String("edit.lines.join")
            || id == QLatin1String("edit.lines.split")
            || id == QLatin1String("encoding.eol")
            || id.startsWith(QLatin1String("encoding.eol."))) {
            override = true;
            enabled = document && context.textMutationSupported;
        } else if (id.startsWith(QLatin1String("search.brace."))) {
            override = true;
            enabled = context.nativeTextSurface;
        } else if (id.startsWith(QLatin1String("view.folding"))) {
            override = true;
            enabled = document && context.lexerSupported;
        } else if (id == QLatin1String("menu.language")
                   || id.startsWith(QLatin1String("language."))) {
            override = true;
            enabled = context.nativeTextSurface;
        } else if (id.startsWith(QLatin1String("encoding.display."))) {
            override = true;
            enabled = context.displayEncodingAvailable;
        } else if (id.startsWith(QLatin1String("encoding.convert."))) {
            override = true;
            enabled = context.conversionAvailable;
        }
        if (override) {
            for (QAction *action : it.value())
                action->setEnabled(enabled);
        }
    }

    setEnabled(QStringLiteral("edit.lines.sortAscending"),
               document && context.textMutationSupported);
    setEnabled(QStringLiteral("edit.lines.sortDescending"),
               document && context.textMutationSupported);
    setEnabled(QStringLiteral("menu.encoding"),
               context.displayEncodingAvailable || context.conversionAvailable);
}

bool CommandRegistry::allowedByDocument(const QString &commandId,
                                        const DocumentState *state,
                                        bool hasSelection)
{
    const QString featureId = FeatureRegistry::featureIdForCommand(commandId);
    if (featureId.isEmpty())
        return false;
    if (commandId == QLatin1String("view.tail.toggle")
        || commandId == QLatin1String("view.tail.resume"))
        return state && state->securityClass == DocumentSecurityClass::OrdinaryDocument
            && state->persistence != DocumentPersistence::SecureFile
            && !state->location.isEmpty()
            && state->capabilities.testFlag(DocumentCapability::Read)
            && !state->capabilities.testFlag(DocumentCapability::Edit);
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
    if (commandId == QLatin1String("search.replace"))
        return state->contentKind == DocumentContentKind::Text
            && state->capabilities.testFlag(DocumentCapability::Edit);
    if (commandId.startsWith(QLatin1String("search."))
        || commandId.startsWith(QLatin1String("bookmark.")))
        return state->capabilities.testFlag(DocumentCapability::Search);
    if (commandId == QLatin1String("file.save")
        || commandId == QLatin1String("file.saveAs"))
        return state->capabilities.testFlag(DocumentCapability::Save);
    return state->capabilities.testFlag(DocumentCapability::Read);
}
