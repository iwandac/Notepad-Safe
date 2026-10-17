#pragma once

#include "ThemeTokens.h"

#include <QPalette>
#include <QString>

class QsciScintilla;

// Application-wide style sheet built only from tokens.
QString applicationStyleSheet(const ThemeTokens &t);

// Tokens taken from a platform palette, for Windows high contrast: painted
// shell parts then use the user's colors instead of ours.
ThemeTokens tokensFromPalette(const QPalette &palette);

// Palette for widgets the style sheet does not reach.
QPalette applicationPalette(const ThemeTokens &t);

// Paper, ink and editor chrome for one editor. Lexer token colors are left
// alone; only their default paper follows the theme. A secure document tints
// the line-number margin with sealSoft.
void applyEditorTheme(QsciScintilla *editor, const ThemeTokens &t, bool secureGutter);
