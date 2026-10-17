#include "ThemeStyle.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexer.h>

#include <utility>

namespace {
QString c(const QColor &color)
{
    return color.name(QColor::HexRgb).toUpper();
}
} // namespace

QString applicationStyleSheet(const ThemeTokens &t)
{
    // Named placeholders: a numbered arg() chain shifts every color when one
    // number goes unused.
    QString sheet = QStringLiteral(
        "QMainWindow, QDialog { background:@chrome; color:@ink; }"
        "QMenuBar { background:@chrome; color:@ink; }"
        "QMenuBar::item:selected, QMenuBar::item:pressed { background:@hover; color:@ink; }"
        "QMenu { background:@raised; color:@ink; border:1px solid @line; border-radius:@radiusOverlaypx; padding:4px; }"
        "QMenu::item { padding:5px 24px 5px 12px; border-radius:@radiusControlpx; }"
        "QMenu::item:selected { background:@hover; color:@ink; }"
        "QMenu::item:disabled { color:@faint; }"
        "QMenu::separator { height:1px; background:@line; margin:4px 8px; }"
        "QStatusBar { background:@chrome; color:@muted; border-top:1px solid @line; }"
        "QStatusBar::item { border:0; }"
        "QStatusBar QLabel { color:@muted; }"
        "StatusSegment { color:@muted; background:transparent; border:0; border-radius:0;"
        " padding:0 8px; }"
        "StatusSegment:hover { background:@hover; color:@ink; }"
        "StatusSegment::menu-indicator { image:none; width:0; }"
        "QTabWidget::pane { border:0; background:@paper; }"
        "QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox {"
        " background:@paper; color:@ink; border:1px solid @line; border-radius:@radiusControlpx;"
        " selection-background-color:@selection; selection-color:@ink; }"
        "QLineEdit, QComboBox, QSpinBox { min-height:@controlpx; padding:0 8px; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus { border-color:@focus; }"
        "QLineEdit:disabled, QComboBox:disabled { color:@faint; background:@chrome; }"
        "QPushButton { background:@raised; color:@ink; border:1px solid @line; border-radius:@radiusControlpx;"
        " min-height:@controlpx; padding:0 12px; }"
        "QPushButton:hover { background:@hover; }"
        "QPushButton:focus { border-color:@focus; }"
        "QPushButton:disabled { color:@faint; }"
        "QCheckBox, QRadioButton, QLabel { color:@ink; }"
        "QCheckBox:disabled, QRadioButton:disabled, QLabel:disabled { color:@faint; }"
        "QTreeView, QListView, QTableView { background:@paper; color:@ink; border:1px solid @line;"
        " selection-background-color:@selection; selection-color:@ink; }"
        "QHeaderView::section { background:@chrome; color:@muted; border:0; border-bottom:1px solid @line; padding:4px 8px; }"
        "QDockWidget { color:@ink; }"
        "QToolTip { background:@raised; color:@ink; border:1px solid @line; padding:4px 6px; }"
        "QScrollBar:vertical { background:transparent; width:12px; margin:0; }"
        "QScrollBar:horizontal { background:transparent; height:12px; margin:0; }"
        "QScrollBar::handle { background:@line; border-radius:4px; min-height:24px; min-width:24px; margin:2px; }"
        "QScrollBar::handle:hover { background:@faint; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }");
    const std::pair<const char *, QString> values[] = {
        {"@ground", c(t.ground)}, {"@chrome", c(t.chrome)}, {"@paper", c(t.paper)},
        {"@raised", c(t.raised)}, {"@ink", c(t.ink)}, {"@muted", c(t.muted)},
        {"@line", c(t.line)}, {"@hover", c(t.hover)}, {"@selection", c(t.selection)},
        {"@focus", c(t.focus)}, {"@faint", c(t.faint)},
        {"@radiusControl", QString::number(t.radiusControl)},
        {"@radiusOverlay", QString::number(t.radiusOverlay)},
        {"@control", QString::number(t.control)},
    };
    for (const auto &[name, value] : values)
        sheet.replace(QLatin1String(name), value);
    return sheet;
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
