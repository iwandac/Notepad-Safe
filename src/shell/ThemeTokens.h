#pragma once

#include <QColor>

// One token set drives every shell and editor color. Values come from the
// design canvas (设计令牌与组件). Seal marks encrypted or locked content and
// exposed marks an unlocked session; nothing else uses those two colors.
struct ThemeTokens {
    QColor ground, chrome, paper, raised;
    QColor ink, muted, faint;
    QColor line, lineSoft, hover, selection, focus;
    QColor seal, sealSoft, exposed, exposedSoft;
    QColor hit, hitCurrent, danger, dangerSoft;

    // Metrics in logical px at 100% DPI.
    int titleBar = 40;
    int tab = 34;
    int statusBar = 26;
    int control = 28;
    int primaryButton = 36;
    int radiusControl = 6;
    int radiusTab = 8;
    int radiusOverlay = 10;
    int radiusPalette = 12;
    int radiusPill = 14;

    static ThemeTokens light();
    static ThemeTokens dark();
};

// WCAG 2 contrast ratio from relative luminance, 1.0 to 21.0.
double contrastRatio(const QColor &a, const QColor &b);
