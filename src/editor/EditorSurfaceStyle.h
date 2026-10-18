#pragma once

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <QColor>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QtMath>

// Search hit fill. The shell theme sets it; the default keeps the legacy amber.
inline QColor &searchHitColor()
{
    static QColor color(0xFF, 0xB7, 0x00);
    return color;
}

// Scintilla takes colors as 0xBBGGRR.
inline long scintillaColor(const QColor &color)
{
    return long(color.red()) | (long(color.green()) << 8) | (long(color.blue()) << 16);
}

inline void applyEditorSurfacePalette(QsciScintilla *editor, const QPalette &palette)
{
    if (!editor)
        return;
    // These Scintilla APIs leave lexer-owned token styles intact. Only the
    // non-lexer paper/text and common editor chrome follow the app palette.
    editor->setPaper(palette.color(QPalette::Base));
    editor->setColor(palette.color(QPalette::Text));
    editor->setMarginsBackgroundColor(palette.color(QPalette::Window));
    editor->setMarginsForegroundColor(palette.color(QPalette::WindowText));
    editor->setCaretLineBackgroundColor(palette.color(QPalette::AlternateBase));
    editor->setSelectionBackgroundColor(palette.color(QPalette::Highlight));
    editor->setSelectionForegroundColor(palette.color(QPalette::HighlightedText));
    // Both fold-margin checkerboard colors are backgrounds, not glyph colors.
    editor->setFoldMarginColors(palette.color(QPalette::Window), palette.color(QPalette::Window));
}

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

    applyEditorSurfacePalette(editor, QApplication::palette());
    editor->setMarginType(1, QsciScintilla::SymbolMargin);
    editor->setMarginWidth(1, 16);

    editor->setAutoIndent(true);
    editor->setIndentationsUseTabs(false);
    editor->setTabWidth(4);
    editor->setIndentationGuides(true);

    editor->setCaretLineVisible(true);
    editor->setCaretWidth(2);

    editor->setScrollWidth(1);
    editor->setScrollWidthTracking(true);

    editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
    editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    // 转置当前行 was removed; drop Scintilla's built-in Ctrl+T line transpose.
    editor->SendScintilla(QsciScintillaBase::SCI_CLEARCMDKEY,
                          static_cast<unsigned long>('T' + (QsciScintillaBase::SCMOD_CTRL << 16)));
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
