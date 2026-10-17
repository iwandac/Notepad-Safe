#pragma once

#include "DocumentState.h"

#include <QHash>
#include <QStringList>

class QAction;
class QObject;

struct CommandContext {
    const DocumentState *document = nullptr;
    bool hasDocument = false;
    bool canModify = false;
    bool hasSelection = false;
    bool clipboardHasText = false;
    bool canUndo = false;
    bool canRedo = false;
    bool masked = false;
    bool modifiedOrUntitled = false;
    bool accountsWorkspace = false;
    bool accountCanSave = false;
    bool accountCanSearch = false;
    bool reloadable = false;
    bool canCloseAll = false;
    bool readOnlyByPolicy = false;
    bool textMutationSupported = false;
    bool nativeTextSurface = false;
    bool lexerSupported = false;
    bool editableHex = false;
    bool hexInsertMode = false;
    bool displayEncodingAvailable = false;
    bool conversionAvailable = false;
    bool dualPresentation = false;
    bool tailAvailable = false;
    bool tailEnabled = false;
    bool tailCanResume = false;
};

// One command identity is shared by menu, toolbar, shortcut and context entry.
// Views still own presentation; capability policy is computed here.
class CommandRegistry final {
public:
    void bind(QObject *root);
    QStringList unboundCommandIds() const { return unboundCommandIds_; }
    QList<QAction *> actions(const QString &commandId) const;
    void applyContext(const CommandContext &context) const;

    static bool allowedByDocument(const QString &commandId,
                                  const DocumentState *state,
                                  bool hasSelection = false);

private:
    void setEnabled(const QString &commandId, bool enabled) const;
    void setChecked(const QString &commandId, bool checked) const;
    QHash<QString, QList<QAction *>> actions_;
    QStringList unboundCommandIds_;
};
