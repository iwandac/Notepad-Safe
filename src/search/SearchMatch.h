#pragma once

#include <QString>

// Editable-text search result expressed independently of an editor widget.
// Search presentation can consume it without including EditorTab.h.
struct ScintillaSearchMatch {
    int line = 0;
    int index = 0;
    int length = 0;
    QString text;
    QString context;
    qint64 byteOffset = -1;
    qint32 byteLength = -1;
};
using SearchMatch = ScintillaSearchMatch;
