#include "TitleBar.h"
#include "WindowChrome.h"

#include <QtTest/QTest>

Q_DECLARE_METATYPE(ChromeHit)

class TestShellLogic final : public QObject {
    Q_OBJECT

private slots:
    void hitTestRegions_data();
    void hitTestRegions();
    void securityPillShowsSessionState();
};

namespace {
constexpr int Border = 6;
const QSize Window(1200, 800);
// Title bar 40 px tall; window buttons 46 px each at the right edge.
const QRect Caption(0, 0, 1200, 40);
const QRect MaxButton(1200 - 92, 0, 46, 40);
// Menu button and command-center button are interactive holes in the caption.
const QVector<QRect> Interactive{QRect(8, 6, 28, 28), QRect(400, 6, 400, 28),
                                 QRect(1200 - 138, 0, 46, 40), MaxButton,
                                 QRect(1200 - 46, 0, 46, 40)};
} // namespace

void TestShellLogic::hitTestRegions_data()
{
    QTest::addColumn<QPoint>("point");
    QTest::addColumn<bool>("maximized");
    QTest::addColumn<ChromeHit>("expected");

    QTest::newRow("top-left corner") << QPoint(2, 3) << false << ChromeHit::TopLeft;
    QTest::newRow("top-right corner") << QPoint(1197, 2) << false << ChromeHit::TopRight;
    QTest::newRow("bottom-left corner") << QPoint(4, 797) << false << ChromeHit::BottomLeft;
    QTest::newRow("bottom-right corner") << QPoint(1199, 799) << false << ChromeHit::BottomRight;
    QTest::newRow("top edge") << QPoint(600, 2) << false << ChromeHit::Top;
    QTest::newRow("top edge maximized") << QPoint(200, 2) << true << ChromeHit::Caption;
    QTest::newRow("left edge") << QPoint(1, 400) << false << ChromeHit::Left;
    QTest::newRow("right edge") << QPoint(1198, 400) << false << ChromeHit::Right;
    QTest::newRow("bottom edge") << QPoint(600, 798) << false << ChromeHit::Bottom;
    QTest::newRow("edges off when maximized") << QPoint(1, 400) << true << ChromeHit::Client;
    QTest::newRow("max button") << QPoint(1200 - 70, 20) << false << ChromeHit::MaxButton;
    QTest::newRow("max button maximized") << QPoint(1200 - 70, 2) << true << ChromeHit::MaxButton;
    QTest::newRow("caption") << QPoint(200, 20) << false << ChromeHit::Caption;
    QTest::newRow("menu button is client") << QPoint(20, 20) << false << ChromeHit::Client;
    QTest::newRow("command center is client") << QPoint(600, 20) << false << ChromeHit::Client;
    QTest::newRow("close button is client") << QPoint(1200 - 20, 20) << false << ChromeHit::Client;
    QTest::newRow("body") << QPoint(600, 400) << false << ChromeHit::Client;
}

void TestShellLogic::hitTestRegions()
{
    QFETCH(QPoint, point);
    QFETCH(bool, maximized);
    QFETCH(ChromeHit, expected);
    QCOMPARE(WindowChrome::hitTest(point, Window, Border, Caption, MaxButton,
                                   maximized, Interactive),
             expected);
}

void TestShellLogic::securityPillShowsSessionState()
{
    const QDateTime now(QDate(2026, 10, 17), QTime(9, 30));
    const QDateTime later(QDate(2026, 10, 20), QTime(18, 5));
    const QDateTime tonight(QDate(2026, 10, 17), QTime(21, 15));

    QCOMPARE(securityPillView(false, false, {}, now).state, SecurityPillState::Hidden);

    const SecurityPillView locked = securityPillView(true, false, later, now);
    QCOMPARE(locked.state, SecurityPillState::Locked);
    QCOMPARE(locked.text, QStringLiteral("已锁定"));

    const SecurityPillView unlocked = securityPillView(true, true, later, now);
    QCOMPARE(unlocked.state, SecurityPillState::Unlocked);
    QCOMPARE(unlocked.text, QStringLiteral("已解锁"));
    QCOMPARE(unlocked.toolTip, QStringLiteral("本设备会话有效至 10-20 18:05，点击立即锁定"));

    const SecurityPillView today = securityPillView(true, true, tonight, now);
    QCOMPARE(today.state, SecurityPillState::ExpiringToday);
    QCOMPARE(today.text, QStringLiteral("今天 21:15 到期"));
    QCOMPARE(today.toolTip, QStringLiteral("本设备会话有效至 10-17 21:15，点击立即锁定"));
}

QTEST_GUILESS_MAIN(TestShellLogic)
#include "test_shell_logic.moc"
