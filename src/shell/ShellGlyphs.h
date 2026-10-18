#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

// The few shell glyphs, drawn as strokes. Icon fonts are avoided on purpose:
// resolving "Segoe Fluent Icons" with its fallback costs ~200 ms at startup.
enum class ShellGlyph { Menu, Minimize, Maximize, Restore, Close, Lock, Eye, Warning };

// Draws the glyph centred in box, 1 px strokes scaled to the box.
void paintShellGlyph(QPainter *painter, ShellGlyph glyph, const QRectF &box,
                     const QColor &color);

// An icon of the glyph: `normal` color, and `active` (hovered) color.
QIcon shellGlyphIcon(ShellGlyph glyph, const QColor &normal, const QColor &active,
                     int size = 16);
