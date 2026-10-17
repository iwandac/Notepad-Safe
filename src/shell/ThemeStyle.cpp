#include "ThemeStyle.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexer.h>

namespace {
QString c(const QColor &color)
{
    return color.name(QColor::HexRgb).toUpper();
}
} // namespace

QString applicationStyleSheet(const ThemeTokens &t)
{
    // %1 ground  %2 chrome  %3 paper  %4 raised  %5 ink  %6 muted  %7 line
    // %8 hover  %9 selection  %10 focus  %11 faint
    // %12 radiusControl  %13 radiusOverlay  %14 control
    return QStringLiteral(
        "QMainWindow, QDialog { background:%2; color:%5; }"
        "QMenuBar { background:%2; color:%5; }"
        "QMenuBar::item:selected, QMenuBar::item:pressed { background:%8; color:%5; }"
        "QMenu { background:%4; color:%5; border:1px solid %7; border-radius:%13px; padding:4px; }"
        "QMenu::item { padding:5px 24px 5px 12px; border-radius:%12px; }"
        "QMenu::item:selected { background:%8; color:%5; }"
        "QMenu::item:disabled { color:%11; }"
        "QMenu::separator { height:1px; background:%7; margin:4px 8px; }"
        "QStatusBar { background:%2; color:%6; border-top:1px solid %7; }"
        "QStatusBar QLabel { color:%6; }"
        "StatusSegment { color:%6; background:transparent; border:0; border-radius:0;"
        " padding:0 8px; }"
        "StatusSegment:hover { background:%8; color:%5; }"
        "StatusSegment::menu-indicator { image:none; width:0; }"
        "QTabWidget::pane { border:0; background:%3; }"
        "QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox {"
        " background:%3; color:%5; border:1px solid %7; border-radius:%12px;"
        " selection-background-color:%9; selection-color:%5; }"
        "QLineEdit, QComboBox, QSpinBox { min-height:%14px; padding:0 8px; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus { border-color:%10; }"
        "QLineEdit:disabled, QComboBox:disabled { color:%11; background:%2; }"
        "QPushButton { background:%4; color:%5; border:1px solid %7; border-radius:%12px;"
        " min-height:%14px; padding:0 12px; }"
        "QPushButton:hover { background:%8; }"
        "QPushButton:focus { border-color:%10; }"
        "QPushButton:disabled { color:%11; }"
        "QCheckBox, QRadioButton, QLabel { color:%5; }"
        "QCheckBox:disabled, QRadioButton:disabled, QLabel:disabled { color:%11; }"
        "QTreeView, QListView, QTableView { background:%3; color:%5; border:1px solid %7;"
        " selection-background-color:%9; selection-color:%5; }"
        "QHeaderView::section { background:%2; color:%6; border:0; border-bottom:1px solid %7; padding:4px 8px; }"
        "QDockWidget { color:%5; }"
        "QToolTip { background:%4; color:%5; border:1px solid %7; padding:4px 6px; }"
        "QScrollBar:vertical { background:transparent; width:12px; margin:0; }"
        "QScrollBar:horizontal { background:transparent; height:12px; margin:0; }"
        "QScrollBar::handle { background:%7; border-radius:4px; min-height:24px; min-width:24px; margin:2px; }"
        "QScrollBar::handle:hover { background:%11; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }")
        .arg(c(t.ground), c(t.chrome), c(t.paper), c(t.raised), c(t.ink),
             c(t.muted), c(t.line), c(t.hover), c(t.selection))
        .arg(c(t.focus), c(t.faint))
        .arg(t.radiusControl).arg(t.radiusOverlay).arg(t.control);
}

ThemeTokens tokensFromPalette(const QPalette &palette)
{
    ThemeTokens t;
    t.paper = palette.color(QPalette::Base);
    t.raised = t.paper;
    t.ink = palette.color(QPalette::Text);
    t.muted = t.ink;
    t.faint = t.ink;
    t.chrome = palette.color(QPalette::Window);
    t.ground = t.chrome;
    t.line = palette.color(QPalette::WindowText);
    t.lineSoft = t.line;
    t.hover = palette.color(QPalette::Highlight);
    t.selection = t.hover;
    t.focus = t.hover;
    t.seal = t.hover;
    t.exposed = t.hover;
    t.sealSoft = t.paper;
    t.exposedSoft = t.paper;
    t.hit = t.hover;
    t.hitCurrent = t.hover;
    t.danger = t.ink;
    t.dangerSoft = t.paper;
    return t;
}

QPalette applicationPalette(const ThemeTokens &t)
{
    QPalette p;
    p.setColor(QPalette::Window, t.chrome);
    p.setColor(QPalette::WindowText, t.ink);
    p.setColor(QPalette::Base, t.paper);
    p.setColor(QPalette::AlternateBase, t.hover);
    p.setColor(QPalette::Text, t.ink);
    p.setColor(QPalette::Button, t.raised);
    p.setColor(QPalette::ButtonText, t.ink);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.ink);
    p.setColor(QPalette::ToolTipBase, t.raised);
    p.setColor(QPalette::ToolTipText, t.ink);
    p.setColor(QPalette::PlaceholderText, t.faint);
    p.setColor(QPalette::Link, t.focus);
    p.setColor(QPalette::Mid, t.line);
    p.setColor(QPalette::Midlight, t.lineSoft);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text,
                                           QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, t.faint);
    }
    return p;
}

void applyEditorTheme(QsciScintilla *editor, const ThemeTokens &t, bool secureGutter)
{
    if (!editor)
        return;
    editor->setPaper(t.paper);
    editor->setColor(t.ink);
    editor->setMarginsBackgroundColor(secureGutter ? t.sealSoft : t.chrome);
    editor->setMarginsForegroundColor(t.faint);
    editor->setCaretLineBackgroundColor(t.hover);
    editor->setCaretForegroundColor(t.ink);
    editor->setSelectionBackgroundColor(t.selection);
    editor->setSelectionForegroundColor(t.ink);
    editor->setFoldMarginColors(t.chrome, t.chrome);
    editor->setIndentationGuidesBackgroundColor(t.paper);
    editor->setIndentationGuidesForegroundColor(t.lineSoft);
    if (QsciLexer *lexer = editor->lexer()) {
        lexer->setDefaultPaper(t.paper);
        lexer->setPaper(t.paper, -1);
    }
}
