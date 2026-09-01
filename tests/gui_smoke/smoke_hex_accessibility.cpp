#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QAccessibleInterface>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCursor>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>

#include <cstdio>
#include <utility>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "ByteClipboard.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "VirtualFileView.h"

namespace {

constexpr qint64 kClipboardContractBytes = 64LL * 1024 * 1024;

EditorTab *currentTab(MainWindow *window)
{
    auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
    return tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
}

void closeAllTabs(MainWindow *window)
{
    auto *tabs = window ? window->findChild<QTabWidget *>() : nullptr;
    if (!tabs)
        return;
    while (tabs->count() > 0) {
        QWidget *page = tabs->widget(0);
        if (auto *tab = qobject_cast<EditorTab *>(page))
            tab->shutdownL2BeforeClose();
        tabs->removeTab(0);
        delete page;
    }
}

QString createBinaryFixture(QTemporaryDir &dir, int bytes = 1024 * 1024)
{
    const QString path = dir.filePath(QStringLiteral("hx-1d-layout.bin"));
    QByteArray payload(bytes, Qt::Uninitialized);
    for (int index = 0; index < payload.size(); ++index)
        payload[index] = char(index & 0xff);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.flush()) {
        return {};
    }
    return QDir::toNativeSeparators(path);
}

QStringList visibleActionIds(QMenu *menu);

struct WindowFixture {
    QTemporaryDir profile;
    QByteArray oldDataDir;
    bool hadDataDir = false;
    MainWindow *window = nullptr;

    WindowFixture()
        : oldDataDir(qgetenv("NOTEPADSAFE_DATADIR"))
        , hadDataDir(qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR"))
    {
        qputenv("NOTEPADSAFE_DATADIR", profile.path().toUtf8());
    }

    ~WindowFixture()
    {
        if (window) {
            closeAllTabs(window);
            delete window;
        }
        if (hadDataDir)
            qputenv("NOTEPADSAFE_DATADIR", oldDataDir);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");
    }

    EditorTab *open(const QString &path, const QSize &size = {1400, 900})
    {
        window = new MainWindow;
        window->resize(size);
        window->show();
        if (!QTest::qWaitForWindowExposed(window))
            return nullptr;
        window->openFilesFromExternal({path});
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs)
            return nullptr;
        QElapsedTimer timer;
        timer.start();
        while (tabs->count() != 1 && timer.elapsed() < 10000) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(20);
        }
        EditorTab *tab = currentTab(window);
        if (!tab)
            return nullptr;
        timer.restart();
        while ((!tab->isL2Active() || !tab->isHexPresentation()
                || !tab->l2View() || !tab->l2View()->isOpen()
                || tab->l2View()->virtualFileSize() <= 0
                || !tab->findChild<QPlainTextEdit *>(QStringLiteral("HexAsciiPreview")))
               && timer.elapsed() < 10000) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(20);
        }
        return tab;
    }
};

class MenuObserver final : public QObject {
public:
    explicit MenuObserver(QString targetActionId)
        : targetActionId(std::move(targetActionId))
    {
    }

    QString targetActionId;
    QStringList actionIds;
    bool sawMenu = false;
    bool triggeredEnabledAction = false;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            if (auto *menu = qobject_cast<QMenu *>(watched)) {
                sawMenu = true;
                actionIds = visibleActionIds(menu);
                QTimer::singleShot(0, menu, [this, menu] {
                    for (QAction *action : menu->actions()) {
                        if (action->objectName() == targetActionId
                            && action->isEnabled()) {
                            triggeredEnabledAction = true;
                            action->trigger();
                            break;
                        }
                    }
                    menu->close();
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QColor renderedBackground(QWidget *widget)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget->render(&painter);
    painter.end();
    return image.pixelColor(qMax(1, image.width() - 4),
                            qBound(1, image.height() / 2,
                                   qMax(1, image.height() - 2)));
}

QStringList visibleActionIds(QMenu *menu)
{
    QStringList ids;
    if (!menu)
        return ids;
    for (QAction *action : menu->actions()) {
        if (!action->isSeparator())
            ids.append(action->objectName());
    }
    return ids;
}

} // namespace

class SmokeHexAccessibility final : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        reportLifecycle("begin");
    }

    void cleanup()
    {
        reportLifecycle("end");
    }

    void tc_hx_1d_layout_theme_accessibility()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString path = createBinaryFixture(files);
        QVERIFY(!path.isEmpty());
        WindowFixture fixture;
        EditorTab *tab = fixture.open(path);
        QVERIFY(tab && tab->isL2Active() && tab->isHexPresentation());

        auto *provider = tab->l2View();
        auto *hex = tab->editor();
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        auto *global = provider ? provider->verticalScrollBar() : nullptr;
        auto *mode = fixture.window->findChild<QToolButton *>(
            QStringLiteral("status.hexEditMode"));
        QVERIFY(provider && hex && ascii && global && mode);

        bool dprOk = false;
        const qreal expectedDpr = qEnvironmentVariable(
            "NOTEPADSAFE_EXPECTED_DPR").toDouble(&dprOk);
        QVERIFY2(dprOk, "NOTEPADSAFE_EXPECTED_DPR must identify the DPI process");
        const qreal actualDpr = fixture.window->screen()->devicePixelRatio();
        qInfo().noquote() << "[HX-1D-DPI] expected=" << expectedDpr
                          << "actual=" << actualDpr
                          << "QT_SCALE_FACTOR=" << qgetenv("QT_SCALE_FACTOR");
        QVERIFY2(qAbs(actualDpr - expectedDpr) < 0.02,
                 qPrintable(QStringLiteral("DPR mismatch expected=%1 actual=%2")
                                .arg(expectedDpr).arg(actualDpr)));

        const QList<QSize> sizes = {{900, 700}, {1400, 850}, {1800, 950}};
        fixture.window->hide();
        for (const QSize &size : sizes) {
            for (bool dockVisible : {false, true}) {
                auto *dock = fixture.window->findChild<QDockWidget *>(
                    QStringLiteral("SearchResultsDock"));
                QVERIFY(dock);
                dock->setVisible(dockVisible);
                fixture.window->resize(size);
                QCOMPARE(fixture.window->size(), size);
                QApplication::processEvents(QEventLoop::AllEvents, 100);

                QCOMPARE(hex->geometry().topLeft(), QPoint(0, 0));
                QCOMPARE(ascii->geometry().top(), 0);
                QCOMPARE(hex->geometry().right() + 1, ascii->geometry().left());
                QCOMPARE(ascii->geometry().right() + 1,
                         provider->viewport()->width());
                QVERIFY(hex->width() >= 300);
                QVERIFY(ascii->width() >= 120);
                QCOMPARE(provider->horizontalScrollBarPolicy(),
                         Qt::ScrollBarAlwaysOff);

                const int row = provider->hexBytesPerRow();
                QVERIFY(row >= 8 && row % 4 == 0);
                tab->gotoByteOffset(qint64(1000) * row);
                QApplication::processEvents(QEventLoop::AllEvents, 100);
                const long lowNibble = hex->positionFromLineIndex(1000, (row - 1) * 3 + 1);
                QVERIFY(lowNibble >= 0);
                const int nibbleX = int(hex->SendScintilla(
                    QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, lowNibble));
                QVERIFY2(nibbleX >= 0 && nibbleX < hex->viewport()->width(),
                         qPrintable(QStringLiteral("last nibble clipped: x=%1 viewport=%2 row=%3")
                                        .arg(nibbleX).arg(hex->viewport()->width()).arg(row)));
                const int hexRowHeight = int(hex->SendScintilla(
                    QsciScintillaBase::SCI_TEXTHEIGHT, 0UL));
                const QTextBlock asciiBlock = ascii->document()->findBlockByNumber(1000);
                QVERIFY(asciiBlock.isValid());
                QCOMPARE(asciiBlock.blockFormat().lineHeightType(),
                         int(QTextBlockFormat::FixedHeight));
                QCOMPARE(qRound(asciiBlock.blockFormat().lineHeight()),
                         hexRowHeight);
            }
        }

        fixture.window->resize(900, 700);
        fixture.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window));

        QVERIFY(!hex->accessibleName().isEmpty());
        QVERIFY(!hex->accessibleDescription().isEmpty());
        QVERIFY(!ascii->accessibleName().isEmpty());
        QVERIFY(!ascii->accessibleDescription().isEmpty());
        QVERIFY(!global->accessibleName().isEmpty());
        QVERIFY(!global->accessibleDescription().isEmpty());
        QVERIFY(!mode->accessibleName().isEmpty());
        QVERIFY(!mode->accessibleDescription().isEmpty());
        for (QWidget *widget : {static_cast<QWidget *>(hex),
                                static_cast<QWidget *>(ascii),
                                static_cast<QWidget *>(global),
                                static_cast<QWidget *>(mode)}) {
            QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(widget);
            QVERIFY(iface);
            QVERIFY(iface->role() != QAccessible::NoRole);
        }

        hex->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(hex->hasFocus()
                                     || hex->viewport()->hasFocus(), 1000);
        QTest::keyClick(hex, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(ascii->hasFocus()
                                     || ascii->viewport()->hasFocus(), 1000);
        QTest::keyClick(ascii, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(global->hasFocus(), 1000);

        for (const auto &theme : QList<QPair<QString, bool>>{
                 {QStringLiteral("view.appearance.light"), false},
                 {QStringLiteral("view.appearance.dark"), true}}) {
            QAction *action = fixture.window->findChild<QAction *>(theme.first);
            QVERIFY(action);
            action->trigger();
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            const QColor background = renderedBackground(ascii->viewport());
            if (theme.second)
                QVERIFY2(background.lightness() < 100, qPrintable(background.name()));
            else
                QVERIFY2(background.lightness() > 180, qPrintable(background.name()));
        }
    }

    void tc_hx_1d_hex_ascii_context_menu_routes_commands()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString path = createBinaryFixture(files, 64 * 1024);
        QVERIFY(!path.isEmpty());
        WindowFixture fixture;
        EditorTab *tab = fixture.open(path);
        QVERIFY(tab && tab->isHexPresentation());
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii);

        tab->gotoByteOffset(2, 4);
        ascii->setFocus();
        QApplication::processEvents(QEventLoop::AllEvents, 100);

        const auto triggerContextAction = [ascii](MenuObserver *observer) {
            qApp->installEventFilter(observer);
            const QPoint local(12, 12);
            QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                                    ascii->viewport()->mapToGlobal(local));
            QApplication::sendEvent(ascii->viewport(), &event);
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            qApp->removeEventFilter(observer);
        };

        QApplication::clipboard()->clear();
        MenuObserver copy(QStringLiteral("edit.copy"));
        triggerContextAction(&copy);
        QVERIFY2(copy.sawMenu, "ASCII right-click did not show a menu");
        QVERIFY2(copy.triggeredEnabledAction,
                 "edit.copy was absent or disabled in the ASCII context menu");
        const QStringList ids = copy.actionIds;
        QVERIFY(ids.contains(QStringLiteral("edit.copy")));
        QVERIFY(ids.contains(QStringLiteral("edit.cut")));
        QVERIFY(ids.contains(QStringLiteral("edit.paste")));
        QVERIFY(ids.contains(QStringLiteral("edit.selectAll")));
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QCOMPARE(mime->data(QString::fromLatin1(ByteClipboard::RawBytesMime)).size(), 4);

        auto *provider = tab->l2View();
        QVERIFY(provider);
        const qint64 beforeSize = provider->virtualFileSize();
        const QByteArray before = provider->readByteRange(0, 16);
        MenuObserver cut(QStringLiteral("edit.cut"));
        triggerContextAction(&cut);
        QVERIFY2(cut.triggeredEnabledAction,
                 "edit.cut was absent or disabled in the ASCII context menu");
        QCOMPARE(provider->virtualFileSize(), beforeSize - 4);
        QVERIFY(provider->isModified());

        MenuObserver undo(QStringLiteral("edit.undo"));
        triggerContextAction(&undo);
        QVERIFY2(undo.triggeredEnabledAction,
                 "edit.undo was absent or disabled after the context-menu cut");
        QCOMPARE(provider->virtualFileSize(), beforeSize);
        QCOMPARE(provider->readByteRange(0, 16), before);
    }

    void tc_hx_1d_clipboard_budget_rejects_atomically()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString path = createBinaryFixture(files, 512 * 1024);
        QVERIFY(!path.isEmpty());
        WindowFixture fixture;
        EditorTab *tab = fixture.open(path);
        QVERIFY(tab && tab->isHexPresentation());
        auto *provider = tab->l2View();
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(provider && ascii);

        tab->gotoByteOffset(0, 4);
        const QByteArray before = provider->readByteRange(0, 32);
        const qint64 beforeSize = provider->virtualFileSize();
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(ByteClipboard::RawBytesMime),
                      QByteArray(kClipboardContractBytes + 1, 'Z'));
        mime->setText(QStringLiteral("oversized raw byte payload"));
        QApplication::clipboard()->setMimeData(mime);

        ascii->setFocus();
        QTest::keyClick(ascii, Qt::Key_V, Qt::ControlModifier);
        QApplication::processEvents(QEventLoop::AllEvents, 200);
        QCOMPARE(provider->virtualFileSize(), beforeSize);
        QCOMPARE(provider->readByteRange(0, 32), before);
        QVERIFY(!provider->isModified());
        QVERIFY(fixture.window->statusBar()->currentMessage().contains(
            QStringLiteral("64 MiB")));
        QVERIFY(fixture.window->statusBar()->currentMessage().contains(
            QStringLiteral("未应用")));
    }

private:
    static void reportLifecycle(const char *phase)
    {
        const QByteArray test = QByteArray(QTest::currentTestFunction());
        const QByteArray expected = qgetenv("NOTEPADSAFE_EXPECTED_DPR");
        QScreen *screen = QApplication::primaryScreen();
        const QByteArray actual = screen
            ? QByteArray::number(screen->devicePixelRatio(), 'g', 4)
            : QByteArrayLiteral("no-screen");
        std::fprintf(stderr,
                     "[HX-1D-LIFECYCLE] phase=%s case=%s expectedDpr=%s actualDpr=%s\n",
                     phase, test.constData(), expected.constData(), actual.constData());
        std::fflush(stderr);
    }
};

int main(int argc, char **argv)
{
    std::fprintf(stderr, "[HX-1D-LIFECYCLE] phase=process-start\n");
    std::fflush(stderr);
    QApplication application(argc, argv);
    std::fprintf(stderr, "[HX-1D-LIFECYCLE] phase=qapplication-ready\n");
    std::fflush(stderr);
    SmokeHexAccessibility test;
    const int result = QTest::qExec(&test, argc, argv);
    std::fprintf(stderr, "[HX-1D-LIFECYCLE] phase=process-end result=%d\n", result);
    std::fflush(stderr);
    return result;
}

#include "smoke_hex_accessibility.moc"
