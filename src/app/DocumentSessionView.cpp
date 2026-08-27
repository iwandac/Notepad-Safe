#include "DocumentSessionView.h"

#include "EditorTab.h"
#include "SessionDocumentPort.h"

#include <QtGlobal>
#include <climits>

void restoreDocumentSessionView(EditorTab *tab, const SessionTab &state)
{
    if (!tab)
        return;
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
    if (state.splitView && tab->canSplitView()) {
        tab->setSplitView(
            true, state.splitOrientation == QLatin1String("vertical")
                      ? Qt::Vertical : Qt::Horizontal);
    }
}
