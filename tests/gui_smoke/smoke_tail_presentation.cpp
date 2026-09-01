#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QColor>
#include <QtGui/QFontMetrics>
#include <QtGui/QPixmap>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <Qsci/qsciscintilla.h>
#include "AppComposition.h"
#include "AsyncTaskManager.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "OrdinaryReadSource.h"
#include "WindowedReadOnlyProvider.h"

namespace {
QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray sourceHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result();
}

class ModalGuard final : public QObject {
public:
    ModalGuard() { qApp->installEventFilter(this); }
    ~ModalGuard() override { qApp->removeEventFilter(this); }
    QStringList shown;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QDialog *>(watched);
            if (dialog && dialog->isModal()) {
                shown.append(dialog->windowTitle());
                const QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, this, [guard] { if (guard) guard->reject(); });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

struct Source {
    QString path;
    QByteArray bytes;
    QPointer<EditorTab> tab;
    QPointer<WindowedReadOnlyProvider> provider;
    DocumentState initial;
    DiskFileStamp stamp;
};

class PresentationWindow final {
public:
    ~PresentationWindow()
    {
        QObject::disconnect(added_);
        auto &manager = AsyncTaskManager::instance();
        for (qint64 id : std::as_const(tasks_)) manager.cancel(id);
        if (!QTest::qWaitFor([&] {
                for (qint64 id : std::as_const(tasks_)) {
                    const auto task = manager.taskById(id);
                    if (task.state == AsyncTaskState::Queued || task.state == AsyncTaskState::Running)
                        return false;
                }
                return true;
            }, 5000)) qWarning("Presentation fixture did not drain its owned tasks");
        if (window) {
            if (auto *tabs = window->findChild<QTabWidget *>()) {
                while (tabs->count()) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page)) tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
            window.reset();
        }
        composition.reset();
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", oldProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool start()
    {
        if (!temporary.isValid()) return fail(temporary.errorString());
        const QString profile = temporary.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile)) return fail(QStringLiteral("cannot create isolated profile"));
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        profileSet_ = qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        if (!profileSet_) return fail(QStringLiteral("cannot isolate profile"));
        composition = std::make_unique<AppComposition>();
        window = std::make_unique<MainWindow>(nullptr, composition.get());
        added_ = QObject::connect(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded,
            window.get(), [this](qint64 id) { tasks_.append(id); });
        window->resize(1200, 800);
        window->show();
        window->raise();
        window->activateWindow();
        return QTest::qWaitForWindowExposed(window.get()) || fail(QStringLiteral("native window not exposed"));
    }

    bool clickMenu(const QString &name)
    {
        auto *action = window->findChild<QAction *>(name);
        if (!action || !action->isEnabled()) return fail(QStringLiteral("menu action absent/disabled: ") + name);
        for (auto *menu : window->findChildren<QMenu *>()) {
            if (!menu->actions().contains(action)) continue;
            menu->popup(window->mapToGlobal(QPoint(120, 100)));
            if (!QTest::qWaitFor([&] { return menu->isVisible(); }, 1000))
                return fail(QStringLiteral("menu not visible"));
            const QRect area = menu->actionGeometry(action);
            if (area.isEmpty() || !menu->rect().contains(area.center())) {
                menu->close();
                return fail(QStringLiteral("menu action has no clickable geometry"));
            }
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, area.center());
            return true;
        }
        return fail(QStringLiteral("action is not in a production menu: ") + name);
    }

    bool appearance(const QString &theme)
    {
        const QString name = QStringLiteral("view.appearance.") + theme;
        return clickMenu(name) && (window->findChild<QAction *>(name)->isChecked()
            || fail(QStringLiteral("appearance menu did not select theme")));
    }

    bool open(Source &source, const QString &name, const QByteArray &bytes, bool readOnly)
    {
        source.path = temporary.filePath(name);
        source.bytes = bytes;
        QFile file(source.path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
            return fail(file.errorString());
        file.close();
        if (captureDiskFileStamp(source.path, &source.stamp) != DiskFileState::Unchanged
            || source.stamp.fileIdentity.isEmpty()) return fail(QStringLiteral("source stamp unavailable"));
        window->openFilesFromExternal({source.path});
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs) return fail(QStringLiteral("tab workspace missing"));
        source.tab = qobject_cast<EditorTab *>(tabs->currentWidget());
        if (!source.tab || source.tab->documentState().location != source.path)
            return fail(QStringLiteral("source did not open as current real document"));
        source.provider = source.tab->windowedReadOnlyProvider();
        if (readOnly) {
            if (!source.provider || source.tab->editor() != source.provider->editor())
                return fail(QStringLiteral("ordinary S2 provider is not active"));
            // All fixture bytes are ASCII, including both CR and LF: independent scalar-count oracle.
            if (!QTest::qWaitFor([&] { return !source.provider->characterCountPending()
                    && source.provider->characterCount() == source.bytes.size(); }, 15000))
                return fail(QStringLiteral("initial exact character statistics did not complete"));
        }
        source.initial = source.tab->documentState();
        return guard(source, readOnly).isEmpty() || fail(guard(source, readOnly));
    }

    QString guard(const Source &source, bool readOnly = true, bool allowNewRevision = false) const
    {
        if (!source.tab || sourceHash(source.path) != sha256(source.bytes))
            return QStringLiteral("source hash/document lifetime changed");
        DiskFileStamp current;
        if (captureDiskFileStamp(source.path, &current) != DiskFileState::Unchanged
            || current.fileIdentity != source.stamp.fileIdentity || current.size != source.bytes.size())
            return QStringLiteral("source identity/size changed unexpectedly");
        const auto state = source.tab->documentState();
        if (state.id != source.initial.id || state.location != source.initial.location
            || state.origin != source.initial.origin || state.persistence != source.initial.persistence
            || state.securityClass != DocumentSecurityClass::OrdinaryDocument
            || state.contentKind != DocumentContentKind::Text || state.dirty || source.tab->isModified()
            || state.capabilities != source.initial.capabilities || !state.capabilities.testFlag(DocumentCapability::Read)
            || source.tab->isSecureDocument()
            || (allowNewRevision ? state.revision < source.initial.revision : state.revision != source.initial.revision))
            return QStringLiteral("document identity/revision/dirty/capabilities changed unexpectedly");
        if (readOnly && (!source.provider || source.tab->windowedReadOnlyProvider() != source.provider
                || source.tab->editor() != source.provider->editor() || !source.tab->isReadOnlyByPolicy()
                || !source.provider->editor()->isReadOnly()
                || state.capabilities.testFlag(DocumentCapability::Edit)
                || state.capabilities.testFlag(DocumentCapability::Save)))
            return QStringLiteral("ordinary S2 lost readonly projection/capability boundary");
        if (!readOnly && (source.tab->editor()->isReadOnly()
                || !state.capabilities.testFlag(DocumentCapability::Edit)))
            return QStringLiteral("small editable document lost Edit capability");
        if (!modals.shown.isEmpty() || QApplication::activeModalWidget())
            return QStringLiteral("unexpected modal: ") + modals.shown.join(QLatin1Char(';'));
        return {};
    }

    bool screenshot(const QString &phase)
    {
        const QStringList names = {QStringLiteral("status.position"), QStringLiteral("status.characters"),
                                   QStringLiteral("status.size")};
        QList<QPointer<QLabel>> labels;
        for (const QString &name : names) labels.append(window->findChild<QLabel *>(name));
        const auto fits = [](const QLabel *label) {
            if (!label) return false;
            const QFontMetrics metrics(label->font());
            const QSize minimum = label->minimumSizeHint();
            // QLabel paints through its own layout/alignment. The ink rectangle's
            // baseline origin is not its paint origin: inkRight below is diagnostic,
            // not a clipping oracle. Actual clipping still requires screenshot review.
            return label->rect().width() >= minimum.width()
                && label->rect().height() >= minimum.height()
                && label->contentsRect().width() >= metrics.horizontalAdvance(label->text())
                && label->contentsRect().height() >= metrics.height();
        };
        // One bounded layout wait for these three real labels, not a screenshot timing sleep.
        const bool layoutFits = QTest::qWaitFor([&] {
            return std::all_of(labels.cbegin(), labels.cend(), [&](const auto &label) { return fits(label.data()); });
        }, 2000);
        QStringList insufficient;
        for (qsizetype index = 0; index < labels.size(); ++index) {
            const auto &label = labels.at(index);
            if (!label) { insufficient.append(names.at(index) + QStringLiteral(" missing")); continue; }
            const QFontMetrics metrics(label->font());
            const QRect ink = metrics.tightBoundingRect(label->text());
            qInfo() << "STATUS_LABEL_LAYOUT" << phase << "name" << label->objectName() << "text" << label->text()
                    << "rect" << label->rect() << "contentsRect" << label->contentsRect()
                    << "minimumSizeHint" << label->minimumSizeHint() << "sizeHint" << label->sizeHint()
                    << "horizontalAdvance" << metrics.horizontalAdvance(label->text()) << "fontHeight" << metrics.height()
                    << "tightBoundingRect" << ink << "inkRight" << ink.right()
                    << "margin" << label->margin() << "frameWidth" << label->frameWidth()
                    << "devicePixelRatio" << label->devicePixelRatioF() << "fits" << fits(label.data());
            if (!fits(label.data())) insufficient.append(label->objectName());
        }
        if (!layoutFits) qWarning() << "STATUS_LABEL_LAYOUT_INSUFFICIENT" << phase << insufficient;
        const QString directory = qEnvironmentVariable("NOTEPADSAFE_TAIL_PRESENTATION_EVIDENCE_DIR");
        if (!directory.isEmpty()) {
            if (!QDir::isAbsolutePath(directory) || !QDir().mkpath(directory))
                return fail(QStringLiteral("evidence directory must be writable and absolute"));
            if (!window->grab().save(QDir(directory).filePath(phase + QStringLiteral(".png"))))
                return fail(QStringLiteral("native window screenshot failed"));
        }
        return layoutFits || fail(phase + QStringLiteral(": status label contents still smaller than font after 2s: ")
                                 + insufficient.join(QLatin1Char(',')));
    }

    QTemporaryDir temporary;
    ModalGuard modals;
    std::unique_ptr<AppComposition> composition;
    std::unique_ptr<MainWindow> window;
    QString error;
private:
    bool fail(const QString &message) { error = message; return false; }
    bool hadProfile_ = false;
    bool profileSet_ = false;
    QByteArray oldProfile_;
    QList<qint64> tasks_;
    QMetaObject::Connection added_;
};

QByteArray longLineSource(char marker = 'p')
{
    return (QByteArray(1022, marker) + QByteArrayLiteral("\r\n")).repeated(33 * 1024);
}

QColor scintillaColor(long color)
{
    return QColor(int(color & 0xff), int((color >> 8) & 0xff), int((color >> 16) & 0xff));
}

double luminance(const QColor &color)
{
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

double contrast(const QColor &foreground, const QColor &background)
{
    const double first = luminance(foreground), second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

QString inspectContrast(QsciScintilla *editor, const QString &phase, bool dark)
{
    const long length = editor->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    long position = editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
    const auto fixtureByte = [&](long at) {
        return at >= 0 && at < length ? editor->SendScintilla(QsciScintillaBase::SCI_GETCHARAT, at) : -1L;
    };
    if (fixtureByte(position) < 33 || fixtureByte(position) > 126) {
        const long visibleLine = editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        const long documentLine = editor->SendScintilla(QsciScintillaBase::SCI_DOCLINEFROMVISIBLE, visibleLine);
        position = editor->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, documentLine);
    }
    if (fixtureByte(position) < 33 || fixtureByte(position) > 126)
        return phase + QStringLiteral(": fixture has no visible ASCII character for style sampling");
    // STYLE_DEFAULT (32) is a template, not necessarily the style of rendered plain text.
    const long styleId = editor->SendScintilla(QsciScintillaBase::SCI_GETSTYLEAT, position);
    const auto foreground = scintillaColor(editor->SendScintilla(QsciScintillaBase::SCI_STYLEGETFORE, styleId));
    const auto background = scintillaColor(editor->SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK, styleId));
    const auto caretBackground = scintillaColor(editor->SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACK));
    const int alpha = int(editor->SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACKALPHA));
    const bool caretVisible = editor->hasFocus()
        && editor->SendScintilla(QsciScintillaBase::SCI_GETCARETLINEVISIBLE);
    // Scintilla's 256 is SC_ALPHA_NOALPHA (opaque), otherwise 0..255 blends over the paper.
    const double opacity = alpha == 256 ? 1.0 : std::clamp(alpha / 255.0, 0.0, 1.0);
    const auto blend = [opacity](int over, int under) { return qRound(over * opacity + under * (1.0 - opacity)); };
    const QColor visibleCaret = caretVisible
        ? QColor(blend(caretBackground.red(), background.red()), blend(caretBackground.green(), background.green()),
                 blend(caretBackground.blue(), background.blue())) : background;
    const double paperRatio = contrast(foreground, background), caretRatio = contrast(foreground, visibleCaret);
    qInfo() << "NATIVE_S2_CONTRAST" << phase << "position" << position << "fixtureByte" << fixtureByte(position)
            << "styleId" << styleId << "foreground" << foreground << "paper" << background
            << "caretLine" << caretBackground << "alpha" << alpha << "visible" << caretVisible
            << "paperRatio" << paperRatio << "caretRatio" << caretRatio;
    // Bundled QScintilla has fold-margin setters but no public SCI_GETFOLDMARGINCOLOUR.
    // Its native pixels remain part of screenshot review; no invented/private message is sent.
    if (paperRatio < 4.5 || caretRatio < 4.5)
        return QStringLiteral("%1: WCAG contrast below 4.5 (paper=%2 currentLine=%3)")
            .arg(phase).arg(paperRatio, 0, 'f', 3).arg(caretRatio, 0, 'f', 3);
    if (dark != (luminance(background) < luminance(foreground)))
        return phase + QStringLiteral(": actual editor colors do not reflect the selected appearance");
    return {};
}

QString browsingPosition(WindowedReadOnlyProvider *provider)
{
    return QStringLiteral("浏览偏移: 0x%1")
        .arg(QString::number(provider->globalOffset(), 16).rightJustified(8, QLatin1Char('0')).toUpper());
}
} // namespace

class SmokeTailPresentation final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY2(QGuiApplication::platformName() == QStringLiteral("windows"), "Native Windows GUI is required");
        QVERIFY2(!QApplication::style()->objectName().contains(QStringLiteral("fusion"), Qt::CaseInsensitive),
                 "Do not substitute Fusion for native appearance acceptance");
    }
    void darkBeforeOpenAndThemeChangesKeepReadonlyTextReadable();
    void tailAndOrdinaryReadonlyPositionUseBrowsingOffset();
};

void SmokeTailPresentation::darkBeforeOpenAndThemeChangesKeepReadonlyTextReadable()
{
    PresentationWindow fixture;
    QVERIFY2(fixture.start(), qPrintable(fixture.error));
    QVERIFY2(fixture.appearance(QStringLiteral("dark")), qPrintable(fixture.error));
    Source first, second;
    QVERIFY2(fixture.open(first, QStringLiteral("first-long-lines.log"), longLineSource(), true), qPrintable(fixture.error));
    QStringList failures;
    const auto inspect = [&](Source &source, const QString &phase, bool dark) {
        auto *editor = source.provider->editor();
        editor->setFocus();
        if (!QTest::qWaitFor([&] { return editor->hasFocus(); }, 2000)) {
            failures.append(phase + QStringLiteral(": editor never gained focus"));
            return;
        }
        QTest::keyClick(editor, Qt::Key_Home);
        const QString problem = inspectContrast(editor, phase, dark);
        if (!problem.isEmpty()) failures.append(problem);
        if (!fixture.screenshot(phase)) failures.append(fixture.error);
    };
    QVERIFY2(fixture.guard(first).isEmpty(), qPrintable(fixture.guard(first)));
    inspect(first, QStringLiteral("dark-before-open-first"), true);
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        QVERIFY2(fixture.appearance(theme), qPrintable(fixture.error));
        QVERIFY2(fixture.guard(first).isEmpty(), qPrintable(fixture.guard(first)));
        inspect(first, theme + QStringLiteral("-first-existing"), theme == QStringLiteral("dark"));
    }
    // The second provider is created after dark is already selected, not recolored by a later toggle.
    QVERIFY2(fixture.open(second, QStringLiteral("second-long-lines.log"), longLineSource('q'), true), qPrintable(fixture.error));
    QVERIFY2(fixture.guard(first).isEmpty(), qPrintable(fixture.guard(first)));
    QVERIFY2(fixture.guard(second).isEmpty(), qPrintable(fixture.guard(second)));
    inspect(second, QStringLiteral("dark-before-open-second"), true);
    for (const QString &theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        QVERIFY2(fixture.appearance(theme), qPrintable(fixture.error));
        QVERIFY2(fixture.guard(first).isEmpty(), qPrintable(fixture.guard(first)));
        QVERIFY2(fixture.guard(second).isEmpty(), qPrintable(fixture.guard(second)));
        inspect(second, theme + QStringLiteral("-second-existing"), theme == QStringLiteral("dark"));
    }
    QVERIFY2(fixture.guard(first).isEmpty(), qPrintable(fixture.guard(first)));
    QVERIFY2(fixture.guard(second).isEmpty(), qPrintable(fixture.guard(second)));
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

void SmokeTailPresentation::tailAndOrdinaryReadonlyPositionUseBrowsingOffset()
{
    PresentationWindow fixture;
    QVERIFY2(fixture.start(), qPrintable(fixture.error));
    Source source;
    QVERIFY2(fixture.open(source, QStringLiteral("tail-position.log"), longLineSource(), true), qPrintable(fixture.error));
    auto *provider = source.provider.data();
    auto *position = fixture.window->findChild<QLabel *>(QStringLiteral("status.position"));
    auto *tailStatus = provider->findChild<QLabel *>(QStringLiteral("tailStatusLabel"));
    auto *toggle = fixture.window->findChild<QAction *>(QStringLiteral("view.tail.toggle"));
    QVERIFY(position && tailStatus && toggle);
    QVERIFY(!toggle->isChecked());
    provider->goToOffset(source.bytes.size()); // Public source navigation, not a nonexistent Go To dialog.
    QTRY_VERIFY_WITH_TIMEOUT(provider->windowEnd() == source.bytes.size()
        && provider->visibleBottomOffset() >= source.bytes.size(), 5000);
    QVERIFY2(fixture.clickMenu(QStringLiteral("view.tail.toggle")), qPrintable(fixture.error));
    QTRY_VERIFY_WITH_TIMEOUT(toggle->isChecked() && tailStatus->text().contains(QStringLiteral("正在跟随")), 2000);
    const QByteArray appended = QByteArrayLiteral("APPENDED_TAIL_POSITION_") + QByteArray(1800, 'z') + QByteArrayLiteral("\r\n");
    QFile writer(source.path);
    QVERIFY(writer.open(QIODevice::Append));
    QCOMPARE(writer.write(appended), qint64(appended.size()));
    QVERIFY(writer.flush());
    writer.close();
    source.bytes += appended;
    QTRY_VERIFY_WITH_TIMEOUT(provider->fileSize() == source.bytes.size()
        && provider->windowEnd() == source.bytes.size()
        && provider->visibleBottomOffset() >= source.bytes.size(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(source.tab->documentState().revision > source.initial.revision, 2000);
    QVERIFY(provider->globalOffset() > 0);
    QCOMPARE(provider->sourceBytes(source.bytes.size() - appended.size(), appended.size()), appended);
    QVERIFY2(fixture.guard(source, true, true).isEmpty(), qPrintable(fixture.guard(source, true, true)));
    QStringList failures;
    const auto inspectPosition = [&](const QString &phase) {
        const bool matched = QTest::qWaitFor([&] { return position->text() == browsingPosition(provider); }, 2000);
        qInfo() << "S2_BROWSING_POSITION" << phase << "actual" << position->text()
                << "expected" << browsingPosition(provider) << "sourceOffset" << provider->globalOffset()
                << "characters" << provider->characterCount() << "pending" << provider->characterCountPending();
        if (!matched) failures.append(phase + QStringLiteral(": expected ") + browsingPosition(provider)
                                    + QStringLiteral(", actual ") + position->text());
        if (!fixture.screenshot(phase)) failures.append(fixture.error);
    };
    inspectPosition(QStringLiteral("tail-follow-after-append"));
    QVERIFY2(fixture.clickMenu(QStringLiteral("view.tail.toggle")), qPrintable(fixture.error));
    QVERIFY(!toggle->isChecked());
    QVERIFY2(fixture.guard(source, true, true).isEmpty(), qPrintable(fixture.guard(source, true, true)));
    inspectPosition(QStringLiteral("tail-off-readonly"));
    QTRY_VERIFY_WITH_TIMEOUT(!provider->characterCountPending()
        && provider->characterCount() == source.bytes.size(), 15000);
    provider->goToOffset(17LL * 1024 * 1024 + 73);
    QTRY_VERIFY_WITH_TIMEOUT(provider->globalOffset() >= 16LL * 1024 * 1024
        && provider->globalOffset() < 19LL * 1024 * 1024, 2000);
    QVERIFY2(fixture.guard(source, true, true).isEmpty(), qPrintable(fixture.guard(source, true, true)));
    inspectPosition(QStringLiteral("readonly-public-source-navigation"));

    Source editable;
    QVERIFY2(fixture.open(editable, QStringLiteral("editable-position.txt"), QByteArrayLiteral("alpha\nbeta\n"), false),
             qPrintable(fixture.error));
    auto *editor = editable.tab->editor();
    editor->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(editor->hasFocus(), 2000);
    QTest::keyClick(editor, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Down);
    QTest::keyClick(editor, Qt::Key_Right);
    QTest::keyClick(editor, Qt::Key_Right);
    int line = -1, column = -1;
    editor->getCursorPosition(&line, &column);
    QCOMPARE(line, 1);
    QCOMPARE(column, 2);
    QVERIFY2(fixture.guard(editable, false).isEmpty(), qPrintable(fixture.guard(editable, false)));
    QCOMPARE(editor->text(), QString::fromLatin1(editable.bytes));
    QTRY_COMPARE_WITH_TIMEOUT(position->text(), QStringLiteral("行: 2, 列: 3"), 2000);
    QVERIFY2(fixture.guard(source, true, true).isEmpty(), qPrintable(fixture.guard(source, true, true)));
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

QTEST_MAIN(SmokeTailPresentation)
#include "smoke_tail_presentation.moc"
