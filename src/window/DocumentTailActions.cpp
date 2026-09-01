#include "DocumentTailActions.h"

#include "CommandRegistry.h"
#include "EditorTab.h"
#include "ReadOnlyTailController.h"
#include "WindowedReadOnlyProvider.h"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <utility>

DocumentTailActions::DocumentTailActions(QObject *owner, CurrentDocument current,
                                       RequestSync requestSync)
    : QObject(owner), currentDocument_(std::move(current)),
      requestSync_(std::move(requestSync))
{
}

void DocumentTailActions::installAndPopulate(DocumentTailActions *&slot, QObject *uiOwner,
                                            CommandRegistry &registry, EditorTab *tab,
                                            CommandContext &context, CurrentDocument current,
                                            RequestSync requestSync)
{
    if (!slot) {
        slot = new DocumentTailActions(uiOwner, std::move(current), std::move(requestSync));
        slot->install(uiOwner, registry);
    }
    slot->populateContext(tab, context);
}

void DocumentTailActions::install(QObject *uiOwner, CommandRegistry &registry)
{
    // The owner supplies the UI tree, not a service locator for MainWindow
    // business methods. Both production and small test owners use menu.view.
    auto *menu = uiOwner ? uiOwner->findChild<QMenu *>(QStringLiteral("menu.view")) : nullptr;
    if (!menu) return;
    toggle_ = new QAction(tr("跟随文件末尾"), this);
    toggle_->setObjectName(QStringLiteral("view.tail.toggle"));
    toggle_->setCheckable(true);
    toggle_->setEnabled(false);
    toggle_->setStatusTip(tr("跟随普通只读文件的追加内容；向上浏览或选择内容会暂停跟随。"));
    resume_ = new QAction(tr("回到末尾"), this);
    resume_->setObjectName(QStringLiteral("view.tail.resume"));
    resume_->setEnabled(false);
    resume_->setStatusTip(tr("清除阅读选区，回到当前文件末尾并继续跟随。"));
    QAction *before = uiOwner->findChild<QAction *>(QStringLiteral("view.split"));
    menu->insertAction(before, toggle_);
    menu->insertAction(before, resume_);
    connect(toggle_, &QAction::triggered, this, [this](bool checked) { execute(false, checked); });
    connect(resume_, &QAction::triggered, this, [this] { execute(true, false); });
    // MainWindow's first ordinary bind predates this lazy installation.
    // Register once here; subsequent state updates do not rescan the UI tree.
    registry.bind(uiOwner);
}

ReadOnlyTailController *DocumentTailActions::controllerFor(EditorTab *tab)
{
    if (!tab || !tab->isReadOnlyByPolicy() || !tab->isL2Active()) return nullptr;
    const DocumentState state = tab->documentState();
    if (!CommandRegistry::allowedByDocument(QStringLiteral("view.tail.toggle"), &state))
        return nullptr;
    auto *provider = tab->windowedReadOnlyProvider();
    if (!provider || !provider->hasSource() || tab->editor() != provider->editor()
        || !provider->sourceVersion().valid
        || !sameDiskFilePath(provider->sourceVersion().absolutePath, state.location))
        return nullptr;
    return provider->tailController();
}

void DocumentTailActions::populateContext(EditorTab *tab, CommandContext &context) const
{
    auto *controller = toggle_ && resume_ ? controllerFor(tab) : nullptr;
    context.tailAvailable = controller != nullptr;
    context.tailEnabled = controller && controller->tailEnabled();
    context.tailCanResume = controller && controller->canResume();
}

void DocumentTailActions::execute(bool resume, bool checked)
{
    const QPointer<DocumentTailActions> owner(this);
    const QPointer<ReadOnlyTailController> controller(
        controllerFor(currentDocument_ ? currentDocument_() : nullptr));
    if (controller) {
        if (resume) {
            if (controller->canResume()) controller->resumeAtEnd();
        } else {
            // Disabling remains possible after the file becomes unreadable.
            // Enabling/resuming performs the controller's fresh read admission.
            controller->setTailEnabled(checked);
        }
    }
    if (!owner) return;
    const auto sync = requestSync_;
    if (sync) sync(); // Reacquires the active document after synchronous reentry.
}
