#pragma once

#include <Qsci/qsciscintilla.h>

#include <QColor>
#include <QFont>

// User-visible editor chrome must not depend on the backing provider.  S1's
// full Scintilla document and S2/S3's bounded projection call this same
// baseline before applying capability-specific behaviour (read-only,
// folding, margin contents, and the global scroll adapter).
inline void applySharedEditorSurfaceStyle(QsciScintilla *editor)
{
    if (!editor)
        return;

    QFont font(QStringLiteral("Consolas"), 10);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    editor->setFont(font);
    editor->setMarginsFont(font);

    editor->setMarginsBackgroundColor(QColor(240, 240, 240));
    editor->setMarginsForegroundColor(QColor(120, 120, 120));
    editor->setMarginType(1, QsciScintilla::SymbolMargin);
    editor->setMarginWidth(1, 16);

    editor->setAutoIndent(true);
    editor->setIndentationsUseTabs(false);
    editor->setTabWidth(4);
    editor->setIndentationGuides(true);

    editor->setCaretLineVisible(true);
    editor->setCaretLineBackgroundColor(QColor(245, 245, 220));
    editor->setCaretWidth(2);

    editor->setScrollWidth(1);
    editor->setScrollWidthTracking(true);
    editor->setSelectionBackgroundColor(QColor(51, 153, 255));
    editor->setSelectionForegroundColor(QColor(255, 255, 255));

    editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
    editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
}
