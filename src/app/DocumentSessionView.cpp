#include "DocumentSessionView.h"

#include "EditorTab.h"
#include "SessionDocumentPort.h"

#include <QtGlobal>
#include <climits>

void restoreDocumentSessionView(EditorTab *tab, const SessionTab &state)
{
    if (!tab)
        return;
    // SessionManager owns legacy syntax validation/clamping. Apply that saved
    // presentation choice after both clean loading and recovery-payload retry.
    tab->setSyntax(static_cast<Syntax>(state.syntax));
    if (state.presentation == QLatin1String("hex") && state.hexInsertMode
        && tab->isHexPresentation() && tab->hexOverwriteMode()) {
        tab->toggleHexOverwriteMode();
    }
    if (state.cursorByteOffset >= 0) {
        const qint64 selectionStart = qMin(state.selectionStartByte,
                                           state.selectionEndByte);
        const qint64 selectionEnd = qMax(state.selectionStartByte,
                                         state.selectionEndByte);
        if (selectionStart >= 0 && selectionEnd > selectionStart) {
            tab->gotoByteOffset(selectionStart, int(qMin<qint64>(
                selectionEnd - selectionStart, INT_MAX)));
        } else {
            tab->gotoByteOffset(state.cursorByteOffset);
        }
    } else {
        tab->setCursorPosition(state.cursorLine, state.cursorIndex);
    }
}
