#include "ThemeStyle.h"
#include "ThemeTokens.h"

#include <QtTest/QTest>

class TestThemeTokens final : public QObject {
    Q_OBJECT

private slots:
    void tokenValuesMatchSpec();
    void textContrastMeetsAA();
    void styleSheetUsesTokens();
    void highContrastTokensFollowPalette();
};

namespace {
void compareColor(const QColor &actual, const char *expected, const char *name)
{
    QVERIFY2(actual == QColor(QString::fromLatin1(expected)),
             qPrintable(QStringLiteral("%1: %2 != %3").arg(
                 QString::fromLatin1(name), actual.name(QColor::HexRgb).toUpper(),
                 QString::fromLatin1(expected))));
}
} // namespace

void TestThemeTokens::tokenValuesMatchSpec()
{
    const ThemeTokens l = ThemeTokens::light();
    compareColor(l.ground, "#E6EAEE", "light.ground");
    compareColor(l.chrome, "#F1F3F6", "light.chrome");
    compareColor(l.paper, "#FCFDFE", "light.paper");
    compareColor(l.raised, "#FFFFFF", "light.raised");
    compareColor(l.ink, "#18212C", "light.ink");
    compareColor(l.muted, "#556171", "light.muted");
    compareColor(l.faint, "#7F8A98", "light.faint");
    compareColor(l.line, "#D5DBE2", "light.line");
    compareColor(l.lineSoft, "#E3E7EC", "light.lineSoft");
    compareColor(l.hover, "#E7EBF0", "light.hover");
    compareColor(l.selection, "#D6E4F5", "light.selection");
    compareColor(l.focus, "#2F6FEB", "light.focus");
    compareColor(l.seal, "#0B6F6A", "light.seal");
    compareColor(l.sealSoft, "#DCEFED", "light.sealSoft");
    compareColor(l.exposed, "#9A5205", "light.exposed");
    compareColor(l.exposedSoft, "#F8E9D6", "light.exposedSoft");
    compareColor(l.hit, "#FCEBA4", "light.hit");
    compareColor(l.hitCurrent, "#F2B33D", "light.hitCurrent");
    compareColor(l.danger, "#A3271B", "light.danger");
    compareColor(l.dangerSoft, "#FBE4E1", "light.dangerSoft");

    const ThemeTokens d = ThemeTokens::dark();
    compareColor(d.ground, "#0F1318", "dark.ground");
    compareColor(d.chrome, "#151A21", "dark.chrome");
    compareColor(d.paper, "#1A2029", "dark.paper");
    compareColor(d.raised, "#212833", "dark.raised");
    compareColor(d.ink, "#E3E8EF", "dark.ink");
    compareColor(d.muted, "#9AA5B4", "dark.muted");
    compareColor(d.faint, "#6F7A89", "dark.faint");
    compareColor(d.line, "#303945", "dark.line");
    compareColor(d.lineSoft, "#252C36", "dark.lineSoft");
    compareColor(d.hover, "#29313C", "dark.hover");
    compareColor(d.selection, "#263E5C", "dark.selection");
    compareColor(d.focus, "#4D7CE0", "dark.focus");
    compareColor(d.seal, "#4FC3B9", "dark.seal");
    compareColor(d.sealSoft, "#14312F", "dark.sealSoft");
    compareColor(d.exposed, "#F2A54A", "dark.exposed");
    compareColor(d.exposedSoft, "#3A2A14", "dark.exposedSoft");
    compareColor(d.hit, "#5A4A16", "dark.hit");
    compareColor(d.hitCurrent, "#B9861F", "dark.hitCurrent");
    compareColor(d.danger, "#F0776A", "dark.danger");
    compareColor(d.dangerSoft, "#3B1C1A", "dark.dangerSoft");

    for (const ThemeTokens &t : {l, d}) {
        QCOMPARE(t.titleBar, 40);
        QCOMPARE(t.tab, 34);
        QCOMPARE(t.statusBar, 26);
        QCOMPARE(t.control, 28);
        QCOMPARE(t.primaryButton, 36);
        QCOMPARE(t.radiusControl, 6);
        QCOMPARE(t.radiusTab, 8);
        QCOMPARE(t.radiusOverlay, 10);
        QCOMPARE(t.radiusPalette, 12);
        QCOMPARE(t.radiusPill, 14);
    }
}

void TestThemeTokens::textContrastMeetsAA()
{
    QCOMPARE(qRound(contrastRatio(Qt::black, Qt::white) * 100), 2100);
    for (const ThemeTokens &t : {ThemeTokens::light(), ThemeTokens::dark()}) {
        QVERIFY(contrastRatio(t.ink, t.paper) >= 4.5);
        QVERIFY(contrastRatio(t.muted, t.paper) >= 4.5);
        QVERIFY(contrastRatio(t.muted, t.chrome) >= 4.5);
        QVERIFY(contrastRatio(t.seal, t.sealSoft) >= 4.5);
        QVERIFY(contrastRatio(t.exposed, t.exposedSoft) >= 4.5);
        QVERIFY(contrastRatio(t.danger, t.paper) >= 4.5);
    }
}

void TestThemeTokens::styleSheetUsesTokens()
{
    const QString light = applicationStyleSheet(ThemeTokens::light()).toUpper();
    QVERIFY(light.contains(QStringLiteral("#FCFDFE")));
    QVERIFY(light.contains(QStringLiteral("#2F6FEB")));
    QVERIFY(!light.contains(QStringLiteral("#2D2D30")));
    const QString dark = applicationStyleSheet(ThemeTokens::dark()).toUpper();
    QVERIFY(dark.contains(QStringLiteral("#1A2029")));
    QVERIFY(!dark.contains(QStringLiteral("#2D2D30")));
}

void TestThemeTokens::highContrastTokensFollowPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Base, QColor("#000000"));
    palette.setColor(QPalette::Text, QColor("#FFFF00"));
    palette.setColor(QPalette::Window, QColor("#101010"));
    palette.setColor(QPalette::WindowText, QColor("#FFFFFF"));
    palette.setColor(QPalette::Highlight, QColor("#00FFFF"));
    const ThemeTokens t = tokensFromPalette(palette);
    QCOMPARE(t.paper, QColor("#000000"));
    QCOMPARE(t.ink, QColor("#FFFF00"));
    QCOMPARE(t.muted, QColor("#FFFF00"));
    QCOMPARE(t.chrome, QColor("#101010"));
    QCOMPARE(t.seal, QColor("#00FFFF"));
    QCOMPARE(t.exposed, QColor("#00FFFF"));
    QCOMPARE(t.tab, 34);
}

QTEST_GUILESS_MAIN(TestThemeTokens)
#include "test_theme_tokens.moc"
