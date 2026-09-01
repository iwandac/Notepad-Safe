#pragma once

#include <QString>

class EditorTab;

// Ephemeral presentation of the current document facts, never a size or
// capability authority. Adds no independent reads, document changes or cache.
struct DocumentStatusPresentation {
    QString sizeText;
    QString sizeDescription;
    QString characterText;
    QString characterDescription;
};

DocumentStatusPresentation documentStatusPresentation(const EditorTab &tab);
QString documentPositionText(const EditorTab *tab, int line, int index,
                             QString *description = nullptr);
