#pragma once

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <QColor>
#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QtMath>

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

// QScintilla and QPlainTextEdit calculate leading differently even when they
// share the same QFont.  Hex and ASCII panes must use one integer row grid or
// a one-pixel difference accumulates into multi-row drift further down.
inline int alignPlainTextRowsToScintilla(QPlainTextEdit *plain,
                                         QsciScintilla *scintilla)
{
    if (!plain || !scintilla)
        return 0;
    plain->setFont(scintilla->font());
    plain->document()->setDefaultFont(scintilla->font());
    int rowHeight = qMax(1, int(scintilla->SendScintilla(
        QsciScintillaBase::SCI_TEXTHEIGHT, 0UL)));
    const QTextBlock firstBlock = plain->document()->firstBlock();
    const int plainNaturalHeight = firstBlock.isValid()
        ? qMax(1, qCeil(plain->document()->documentLayout()
                           ->blockBoundingRect(firstBlock).height()))
        : rowHeight;
    if (plainNaturalHeight > rowHeight) {
        const int currentExtraDescent = int(scintilla->SendScintilla(
            QsciScintillaBase::SCI_GETEXTRADESCENT));
        scintilla->SendScintilla(
            QsciScintillaBase::SCI_SETEXTRADESCENT,
            static_cast<unsigned long>(currentExtraDescent
                                       + plainNaturalHeight - rowHeight));
        rowHeight = qMax(1, int(scintilla->SendScintilla(
            QsciScintillaBase::SCI_TEXTHEIGHT, 0UL)));
    }
    rowHeight = qMax(rowHeight, plainNaturalHeight);
    QTextCursor cursor(plain->document());
    cursor.select(QTextCursor::Document);
    QTextBlockFormat rows;
    rows.setLineHeight(rowHeight, QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(rows);
    return rowHeight;
}
