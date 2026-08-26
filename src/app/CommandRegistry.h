#pragma once

#include "DocumentState.h"

#include <QHash>
#include <QStringList>

class QAction;
class QObject;

// One command identity is shared by menu, toolbar, shortcut and context entry.
// Views still own presentation; capability policy is computed here.
class CommandRegistry final {
public:
    void bind(QObject *root);
    QStringList unboundCommandIds() const { return unboundCommandIds_; }
    QList<QAction *> actions(const QString &commandId) const;

    static bool allowedByDocument(const QString &commandId,
                                  const DocumentState *state,
                                  bool hasSelection = false);

private:
    QHash<QString, QList<QAction *>> actions_;
    QStringList unboundCommandIds_;
};

