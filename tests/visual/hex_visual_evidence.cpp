#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QAction>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>

#include <Qsci/qsciscintillabase.h>

#include <cstdio>
#include <functional>

#include "EditorTab.h"
#include "MainWindow.h"
#include "VirtualFileView.h"

namespace {

struct Options {
    QString outputDirectory;
    QString scaleLabel;
    qreal expectedDpr = 0.0;
};

bool parseOptions(const QStringList &arguments, Options *options, QString *error)
{
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--output") && index + 1 < arguments.size())
            options->outputDirectory = arguments.at(++index);
        else if (argument == QStringLiteral("--scale-label") && index + 1 < arguments.size())
            options->scaleLabel = arguments.at(++index);
        else if (argument == QStringLiteral("--expected-dpr") && index + 1 < arguments.size()) {
            bool ok = false;
            options->expectedDpr = arguments.at(++index).toDouble(&ok);
            if (!ok)
                options->expectedDpr = 0.0;
        } else {
            *error = QStringLiteral("Unknown or incomplete argument: %1").arg(argument);
            return false;
        }
    }
    if (options->outputDirectory.isEmpty() || options->scaleLabel.isEmpty()
        || options->expectedDpr <= 0.0) {
        *error = QStringLiteral("--output, --scale-label and --expected-dpr are required");
        return false;
    }
    return true;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return predicate();
}

QString createB1Fixture(QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("hx-1d-b1-visual.bin"));
    QByteArray bytes(1024 * 1024, Qt::Uninitialized);
    for (int index = 0; index < bytes.size(); ++index) {
        const int row = index / 32;
        const int column = index % 32;
        bytes[index] = char((row * 17 + column * 29) & 0xff);
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
        || !file.flush()) {
        return {};
    }
    return QDir::toNativeSeparators(path);
}

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

QString themeName(bool dark)
{
    return dark ? QStringLiteral("dark") : QStringLiteral("light");
}

QString marginText(QsciScintilla *editor, int line)
{
    const long length = editor->SendScintilla(
        QsciScintillaBase::SCI_MARGINGETTEXT,
        static_cast<unsigned long>(line), static_cast<void *>(nullptr));
    QByteArray buffer(qMax<long>(0, length) + 1, '\0');
    editor->SendScintilla(QsciScintillaBase::SCI_MARGINGETTEXT,
                          static_cast<unsigned long>(line),
                          static_cast<void *>(buffer.data()));
    return QString::fromUtf8(buffer.constData(), qMax<long>(0, length));
}

QJsonObject sizeObject(const QSize &size)
{
    return {{QStringLiteral("width"), size.width()},
            {QStringLiteral("height"), size.height()}};
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    Options options;
    QString error;
    if (!parseOptions(application.arguments(), &options, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    if (!QDir().mkpath(options.outputDirectory)) {
        std::fprintf(stderr, "Cannot create output directory: %s\n",
                     qPrintable(options.outputDirectory));
        return 3;
    }

    QTemporaryDir profile;
    QTemporaryDir fixtureDirectory;
    if (!profile.isValid() || !fixtureDirectory.isValid()) {
        std::fprintf(stderr, "Cannot create isolated visual-evidence directories\n");
        return 4;
    }
    qputenv("NOTEPADSAFE_DATADIR", profile.path().toUtf8());
    const QString fixturePath = createB1Fixture(fixtureDirectory);
    if (fixturePath.isEmpty()) {
        std::fprintf(stderr, "Cannot create B1 visual fixture\n");
        return 5;
    }

    MainWindow window;
    window.resize(1400, 850);
    window.show();
    if (!waitUntil([&window] { return window.isVisible() && window.screen(); }, 10000)) {
        std::fprintf(stderr, "Main window did not become visible\n");
        return 6;
    }
    const qreal actualDpr = window.screen()->devicePixelRatio();
    if (qAbs(actualDpr - options.expectedDpr) >= 0.02) {
        std::fprintf(stderr, "DPR mismatch: expected %.3f actual %.3f\n",
                     options.expectedDpr, actualDpr);
        return 7;
    }

    window.openFilesFromExternal({fixturePath});
    if (!waitUntil([&window] {
            EditorTab *tab = currentTab(&window);
            return tab && tab->isL2Active() && tab->isHexPresentation()
                   && tab->l2View() && tab->l2View()->isOpen()
                   && tab->l2View()->virtualFileSize() == 1024 * 1024;
        }, 15000)) {
        std::fprintf(stderr, "B1 Hex document did not become ready\n");
        return 8;
    }

    EditorTab *tab = currentTab(&window);
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("SearchResultsDock"));
    auto *mode = window.findChild<QToolButton *>(QStringLiteral("status.hexEditMode"));
    if (!tab || !dock || !mode) {
        std::fprintf(stderr, "Required product surfaces are missing\n");
        return 9;
    }

    const QList<QSize> logicalSizes = {{900, 700}, {1400, 850}, {1800, 950}};
    QJsonArray captures;
    for (const QSize &logicalSize : logicalSizes) {
        for (bool dark : {false, true}) {
            QAction *theme = window.findChild<QAction *>(
                dark ? QStringLiteral("view.appearance.dark")
                     : QStringLiteral("view.appearance.light"));
            if (!theme) {
                std::fprintf(stderr, "Theme action is missing\n");
                return 10;
            }
            theme->trigger();
            for (bool dockVisible : {false, true}) {
                // A fixed visible top-level window is allowed to extend beyond
                // the current monitor work area. It preserves exact review
                // geometry while still delivering real resize/show events to
                // the product's adaptive bytes-per-row implementation.
                window.hide();
                window.setFixedSize(logicalSize);
                dock->setVisible(dockVisible);
                if (dockVisible)
                    window.resizeDocks({dock}, {qMax(170, logicalSize.height() / 4)},
                                       Qt::Vertical);
                window.show();
                if (!waitUntil([&window, logicalSize] {
                        return window.isVisible() && window.size() == logicalSize;
                    }, 5000)) {
                    std::fprintf(stderr,
                                 "Logical size mismatch: requested %dx%d actual %dx%d\n",
                                 logicalSize.width(), logicalSize.height(),
                                 window.width(), window.height());
                    return 11;
                }

                const bool wantInsert = dockVisible;
                if (tab->hexOverwriteMode() == wantInsert)
                    tab->toggleHexOverwriteMode();
                int bytesPerRow = 0;
                int stableSamples = 0;
                if (!waitUntil([&] {
                        const int current = tab->l2View()->hexBytesPerRow();
                        if (current == bytesPerRow)
                            ++stableSamples;
                        else {
                            bytesPerRow = current;
                            stableSamples = 0;
                        }
                        return bytesPerRow >= 8 && stableSamples >= 3;
                    }, 5000)) {
                    std::fprintf(stderr, "Hex reflow did not stabilize\n");
                    return 12;
                }
                const qint64 row1000Offset = qint64(1000) * bytesPerRow;
                tab->gotoByteOffset(row1000Offset + 2, 12);
                tab->editor()->setFocus(Qt::OtherFocusReason);

                const QString caseId = QStringLiteral("HX-1D-B1-VIS-DPR%1-W%2-%3-DOCK%4")
                                           .arg(options.scaleLabel)
                                           .arg(logicalSize.width())
                                           .arg(themeName(dark).toUpper())
                                           .arg(dockVisible ? 1 : 0);
                window.setWindowTitle(QStringLiteral("Notepad Safe - %1").arg(caseId));
                window.statusBar()->showMessage(
                    QStringLiteral("%1 | B1 | row 1000 | DPR %2 | scale %3")
                        .arg(caseId)
                        .arg(actualDpr, 0, 'f', 2)
                        .arg(QString::fromUtf8(qgetenv("QT_SCALE_FACTOR"))),
                    0);
                QApplication::processEvents(QEventLoop::AllEvents, 250);

                if (window.size() != logicalSize
                    || dock->isVisible() != dockVisible || !mode->isVisible()
                    || mode->text() != (wantInsert ? QStringLiteral("INS")
                                                   : QStringLiteral("OVR"))) {
                    std::fprintf(stderr, "Visible state mismatch for %s\n",
                                 qPrintable(caseId));
                    return 13;
                }
                const qint64 selectionStart = tab->l2View()->selectionStart();
                const qint64 selectionEnd = tab->l2View()->selectionEnd();
                if (selectionStart != row1000Offset + 2
                    || selectionEnd != row1000Offset + 14) {
                    std::fprintf(stderr, "Selection mismatch for %s: %lld..%lld\n",
                                 qPrintable(caseId), selectionStart, selectionEnd);
                    return 14;
                }
                if (tab->l2View()->hexBytesPerRow() != bytesPerRow) {
                    std::fprintf(stderr, "Hex row width changed before capture: %s\n",
                                 qPrintable(caseId));
                    return 15;
                }
                const long caretPosition = tab->editor()->SendScintilla(
                    QsciScintillaBase::SCI_GETCURRENTPOS);
                const int caretLine = int(tab->editor()->SendScintilla(
                    QsciScintillaBase::SCI_LINEFROMPOSITION, caretPosition));
                const QString expectedAddress = QString::number(row1000Offset, 16)
                                                    .rightJustified(8, QLatin1Char('0'))
                                                    .toUpper();
                const QString actualAddress = marginText(tab->editor(), caretLine);
                if (actualAddress != expectedAddress) {
                    std::fprintf(stderr,
                                 "Visible row mismatch for %s: expected %s actual %s\n",
                                 qPrintable(caseId), qPrintable(expectedAddress),
                                 qPrintable(actualAddress));
                    return 16;
                }

                const QPixmap screenshot = window.grab();
                const QString fileName = caseId.toLower() + QStringLiteral(".png");
                const QString path = QDir(options.outputDirectory).filePath(fileName);
                if (screenshot.isNull() || !screenshot.save(path, "PNG")) {
                    std::fprintf(stderr, "Cannot save screenshot: %s\n", qPrintable(path));
                    return 17;
                }
                captures.append(QJsonObject{
                    {QStringLiteral("case_id"), caseId},
                    {QStringLiteral("path"), fileName},
                    {QStringLiteral("actual_dpr"), actualDpr},
                    {QStringLiteral("qt_scale_factor"),
                     QString::fromUtf8(qgetenv("QT_SCALE_FACTOR"))},
                    {QStringLiteral("theme"), themeName(dark)},
                    {QStringLiteral("dock_visible"), dockVisible},
                    {QStringLiteral("edit_mode"), wantInsert ? QStringLiteral("INS")
                                                             : QStringLiteral("OVR")},
                    {QStringLiteral("logical_window_size"), sizeObject(window.size())},
                    {QStringLiteral("physical_artifact_size"), sizeObject(screenshot.size())},
                    {QStringLiteral("artifact_device_pixel_ratio"), screenshot.devicePixelRatio()},
                    {QStringLiteral("bytes_per_row"), bytesPerRow},
                    {QStringLiteral("visible_reference_row"), 1000},
                    {QStringLiteral("selection_start"), double(selectionStart)},
                    {QStringLiteral("selection_end"), double(selectionEnd)},
                    {QStringLiteral("fixture_tier"), QStringLiteral("B1")}
                });
            }
        }
    }

    const QJsonObject run{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("expected_dpr"), options.expectedDpr},
        {QStringLiteral("actual_dpr"), actualDpr},
        {QStringLiteral("qt_scale_factor"), QString::fromUtf8(qgetenv("QT_SCALE_FACTOR"))},
        {QStringLiteral("capture_count"), captures.size()},
        {QStringLiteral("captures"), captures}
    };
    const QString runPath = QDir(options.outputDirectory).filePath(
        QStringLiteral("capture-dpr-%1.json").arg(options.scaleLabel));
    QFile runFile(runPath);
    if (!runFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || runFile.write(QJsonDocument(run).toJson(QJsonDocument::Indented)) <= 0
        || !runFile.flush()) {
        std::fprintf(stderr, "Cannot write run metadata: %s\n", qPrintable(runPath));
        return 18;
    }

    std::fprintf(stdout, "Captured %lld B1 full-window cases at DPR %.2f\n",
                 static_cast<long long>(captures.size()), actualDpr);
    closeAllTabs(&window);
    return 0;
}
