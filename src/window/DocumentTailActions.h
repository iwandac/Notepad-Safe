#pragma once

#include <QObject>
#include <functional>

class CommandRegistry;
struct CommandContext;
class EditorTab;
class QAction;
class ReadOnlyTailController;

// Window-owned command wiring only. The document controller owns every Tail
// state; these actions project current facts and recheck them on execution.
class DocumentTailActions final : public QObject
{
    Q_OBJECT
public:
    using CurrentDocument = std::function<EditorTab *()>;
    using RequestSync = std::function<void()>;

    static void installAndPopulate(DocumentTailActions *&slot, QObject *uiOwner,
                                   CommandRegistry &registry, EditorTab *tab,
                                   CommandContext &context, CurrentDocument current,
                                   RequestSync requestSync);

private:
    DocumentTailActions(QObject *owner, CurrentDocument current, RequestSync requestSync);
    void install(QObject *uiOwner, CommandRegistry &registry);
    void populateContext(EditorTab *tab, CommandContext &context) const;
    static ReadOnlyTailController *controllerFor(EditorTab *tab);
    void execute(bool resume, bool checked);

    CurrentDocument currentDocument_;
    RequestSync requestSync_;
    QAction *toggle_ = nullptr;
    QAction *resume_ = nullptr;
};
