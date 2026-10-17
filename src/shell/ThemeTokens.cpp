#include "ThemeTokens.h"

#include <QtMath>

namespace {
QColor hex(const char *value)
{
    return QColor(QString::fromLatin1(value));
}

double channel(double c)
{
    return c <= 0.03928 ? c / 12.92 : qPow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &color)
{
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
        + 0.0722 * channel(color.blueF());
}
} // namespace

ThemeTokens ThemeTokens::light()
{
    ThemeTokens t;
    t.ground = hex("#E6EAEE");
    t.chrome = hex("#F1F3F6");
    t.paper = hex("#FCFDFE");
    t.raised = hex("#FFFFFF");
    t.ink = hex("#18212C");
    t.muted = hex("#556171");
    t.faint = hex("#7F8A98");
    t.line = hex("#D5DBE2");
    t.lineSoft = hex("#E3E7EC");
    t.hover = hex("#E7EBF0");
    t.selection = hex("#D6E4F5");
    t.focus = hex("#2F6FEB");
    t.seal = hex("#0B6F6A");
    t.sealSoft = hex("#DCEFED");
    t.exposed = hex("#9A5205");
    t.exposedSoft = hex("#F8E9D6");
    t.hit = hex("#FCEBA4");
    t.hitCurrent = hex("#F2B33D");
    t.danger = hex("#A3271B");
    t.dangerSoft = hex("#FBE4E1");
    return t;
}

ThemeTokens ThemeTokens::dark()
{
    ThemeTokens t;
    t.ground = hex("#0F1318");
    t.chrome = hex("#151A21");
    t.paper = hex("#1A2029");
    t.raised = hex("#212833");
    t.ink = hex("#E3E8EF");
    t.muted = hex("#9AA5B4");
    t.faint = hex("#6F7A89");
    t.line = hex("#303945");
    t.lineSoft = hex("#252C36");
    t.hover = hex("#29313C");
    t.selection = hex("#263E5C");
    t.focus = hex("#4D7CE0");
    t.seal = hex("#4FC3B9");
    t.sealSoft = hex("#14312F");
    t.exposed = hex("#F2A54A");
    t.exposedSoft = hex("#3A2A14");
    t.hit = hex("#5A4A16");
    t.hitCurrent = hex("#B9861F");
    t.danger = hex("#F0776A");
    t.dangerSoft = hex("#3B1C1A");
    return t;
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}
