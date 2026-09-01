#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtGui/QAction>
#include <QtGui/QPalette>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeView>

#include <memory>
#include <utility>

#include "AsyncTaskManager.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"

namespace {
constexpr int RetainedHits = 200000;
constexpr qint64 LastRetainedOffset = 399998;

bool terminal(const AsyncTask &task)
{
    return task.id > 0 && task.state != AsyncTaskState::Queued
        && task.state != AsyncTaskState::Running;
}

// Only a temporary source/profile and this fixture's production tasks are
// owned here. No pool overrides, synthetic results or private cache access.
class CapacityDocument final {
public:
    ~CapacityDocument()
    {
        QObject::disconnect(added_);
        auto &manager = AsyncTaskManager::instance();
        for (qint64 id : std::as_const(tasks)) manager.cancel(id);
        const bool drained = QTest::qWaitFor([&]() {
            for (qint64 id : std::as_const(tasks)) {
                if (!terminal(manager.taskById(id))) return false;
            }
            return true;
        }, 5000);
        if (!drained) qWarning("capacity fixture did not drain its owned tasks");
        if (window) {
            if (auto *tabs = window->findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *editor = qobject_cast<EditorTab *>(page))
                        editor->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
            window.reset();
        }
        if (profileSet_) {
            if (hadProfile_) qputenv("NOTEPADSAFE_DATADIR", previousProfile_);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        }
    }

    bool open(int hitCount, bool keyboardDirty = false)
    {
        if (!temporary_.isValid()) return false;
        const QString profile = temporary_.filePath(QStringLiteral("profile"));
        if (!QDir().mkpath(profile)) return false;
        hadProfile_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        previousProfile_ = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        profileSet_ = true;
        path = temporary_.filePath(QStringLiteral("capacity-l1.txt"));
        source = QByteArray("x\n").repeated(hitCount);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size())
            return false;
        file.close();
        sourceHash = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        window = std::make_unique<MainWindow>();
        auto &manager = AsyncTaskManager::instance();
        added_ = QObject::connect(&manager, &AsyncTaskManager::taskAdded, window.get(),
            [this, &manager](qint64 id) {
                const AsyncTask task = manager.taskById(id);
                if ((task.categoryTag == QStringLiteral("SearchFindAll")
                     || task.categoryTag == QStringLiteral("SearchNavigation"))
                    && task.title.contains(QFileInfo(path).fileName())) tasks.append(id);
            });
        window->resize(1100, 760);
        window->show();
        window->raise();
        window->activateWindow();
        if (!QTest::qWaitForWindowExposed(window.get())) return false;
        window->openFilesFromExternal({path});
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs || !QTest::qWaitFor([&]() { return tabs->count() == 1; }, 3000))
            return false;
        tab = qobject_cast<EditorTab *>(tabs->currentWidget());
        if (!tab || tab->isL2Mode() || tab->isModified()
            || tab->editor()->text().toUtf8() != source) return false;
        // Preserve the exact source hit count, while making dirty/revision
        // preservation observable rather than merely checking a clean file.
        if (keyboardDirty) {
            auto *editor = tab->editor();
            editor->setFocus();
            QTest::keyClick(editor, Qt::Key_End, Qt::ControlModifier);
            editor->SendScintilla(QsciScintillaBase::SCI_BEGINUNDOACTION);
            QTest::keyClicks(editor, QStringLiteral("DIRTY"));
            QTest::keyClick(editor, Qt::Key_Return);
            editor->SendScintilla(QsciScintillaBase::SCI_ENDUNDOACTION);
        } else {
            tab->editor()->append(QStringLiteral("DIRTY\n"));
        }
        body = source + QByteArray("DIRTY\n");
        state = tab->documentState();
        return safetyError().isEmpty();
    }

    QString safetyError() const
    {
        if (!tab || tab->isL2Mode()) return QStringLiteral("lost S1 document");
        auto *tabs = window->findChild<QTabWidget *>();
        if (!tabs || tabs->currentWidget() != tab.data())
            return QStringLiteral("active document changed");
        if (tab->documentState().id != state.id || tab->contentRevision() != state.revision
            || tab->documentState().capabilities != state.capabilities)
            return QStringLiteral("document identity/revision/capabilities changed");
        if (!tab->isModified() || tab->editor()->text().toUtf8() != body)
            return QStringLiteral("dirty text changed");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("source cannot be read");
        const QByteArray actual = file.readAll();
        if (actual != source || QCryptographicHash::hash(actual, QCryptographicHash::Sha256) != sourceHash)
            return QStringLiteral("source bytes/hash changed");
        return {};
    }

    std::unique_ptr<MainWindow> window;
    QPointer<EditorTab> tab;
    QString path;
    QByteArray source, body, sourceHash;
    DocumentState state;
    QVector<qint64> tasks;
private:
    QTemporaryDir temporary_;
    bool profileSet_ = false, hadProfile_ = false;
    QByteArray previousProfile_;
    QMetaObject::Connection added_;
};

qint64 selectionStart(EditorTab *tab)
{
    return tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
}

bool selectedAt(EditorTab *tab, qint64 offset)
{
    return selectionStart(tab) == offset
        && tab->editor()->selectedText() == QStringLiteral("x")
        && tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == offset + 1;
}

struct NavigationStep { bool forward, wrap; qint64 offset; bool found; };

QVector<NavigationStep> navigationSteps(int hits)
{
    const qint64 last = qint64(hits - 1) * 2;
    QVector<NavigationStep> steps;
    if (hits > RetainedHits) {
        // This match lies beyond the retained Find All prefix.
        steps.append({true, false, 400000, true});
        steps.append({false, false, LastRetainedOffset, true});
        steps.append({true, false, 400000, true});
    } else {
        steps.append({false, false, LastRetainedOffset - 2, true});
        steps.append({true, false, LastRetainedOffset, true});
    }
    steps.append({true, false, last, false}); // no forward wrap at the last hit
    steps.append({true, true, 0, true});
    steps.append({false, true, last, true}); // reverse wrap must include overflow
    steps.append({true, true, 0, true});
    steps.append({false, false, 0, false}); // no reverse wrap at the first hit
    return steps;
}

void addCapacityData()
{
    QTest::addColumn<bool>("regex");
    QTest::addColumn<int>("hits");
    QTest::newRow("literal-exact-200000") << false << 200000;
    QTest::newRow("literal-overflow-200001") << false << 200001;
    QTest::newRow("regex-exact-200000") << true << 200000;
    QTest::newRow("regex-overflow-200001") << true << 200001;
}
} // namespace

class SmokeSearchCapacity : public QObject {
    Q_OBJECT
private slots:
    void uiFindAllCapacityAndNavigation_data() { addCapacityData(); }

    void uiFindAllCapacityAndNavigation()
    {
        QFETCH(bool, regex);
        QFETCH(int, hits);
        CapacityDocument document;
        QVERIFY(document.open(hits));
        const QString theme = qEnvironmentVariable("NOTEPADSAFE_SEARCH_CAPACITY_THEME");
        if (!theme.isEmpty()) {
            QVERIFY(theme == QStringLiteral("light") || theme == QStringLiteral("dark"));
            QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
            QVERIFY(!QApplication::style()->objectName().contains(QStringLiteral("fusion"), Qt::CaseInsensitive));
            auto *appearance = document.window->findChild<QAction *>(
                QStringLiteral("view.appearance.") + theme);
            QVERIFY(appearance);
            appearance->trigger();
            QVERIFY(appearance->isChecked());
            const int lightness = QApplication::palette().color(QPalette::Base).lightness();
            QVERIFY(theme == QStringLiteral("dark") ? lightness < 100 : lightness > 180);
        }
        auto *findAction = document.window->findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = document.window->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = document.window->findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regexCheck = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseCheck = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(pattern && scope && regexCheck && caseCheck && wholeWord && hex && wrap
                && findAll && next && previous && cancel && status && summary && tree);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regexCheck->setChecked(regex);
        caseCheck->setChecked(true);
        wholeWord->setChecked(false);
        hex->setChecked(false);
        wrap->setChecked(false);
        pattern->setText(QStringLiteral("x"));
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(document.tasks.size(), qsizetype(1), 1000);
        auto &manager = AsyncTaskManager::instance();
        const qint64 taskId = document.tasks.first();
        QTRY_VERIFY_WITH_TIMEOUT(terminal(manager.taskById(taskId)), 15000);
        QVERIFY(manager.taskById(taskId).state != AsyncTaskState::Canceled);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && findAll->isEnabled()
                                     && pattern->isEnabled(), 15000);
        // These guards deliberately precede the capacity expectations.
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QPointer<SearchResultsModel> model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY2(model, "Find All must publish its real bounded result model");
        QCOMPARE(model->totalMatches(), qint64(RetainedHits));
        QCOMPARE(model->rowCount(), RetainedHits);
        QCOMPARE(model->matchByteOffset(model->index(0, 0)), qint64(0));
        QCOMPARE(model->matchByteOffset(model->index(RetainedHits - 1, 0)), LastRetainedOffset);
        QCOMPARE(model->matchLength(model->index(0, 0)), 1);
        QCOMPARE(model->matchLength(model->index(RetainedHits - 1, 0)), 1);
        const bool partial = hits > RetainedHits;
        QCOMPARE(model->isIncomplete(), partial);
        QVERIFY(summary->isVisible());
        const QString visible = summary->text() + QLatin1Char('\n')
            + (status->isVisible() ? status->text() : QString());
        QVERIFY(visible.contains(QStringLiteral("200000")) || visible.contains(QStringLiteral("200,000")));
        if (partial) {
            QVERIFY(visible.contains(QStringLiteral("至少")));
            QVERIFY(visible.contains(QStringLiteral("未完成")) || visible.contains(QStringLiteral("未查完")));
            QVERIFY(visible.contains(QStringLiteral("缩小")));
        } else {
            QVERIFY(document.tab->lastSearchError().isEmpty());
            QVERIFY(model->incompleteReason().isEmpty());
            QVERIFY(!visible.contains(QStringLiteral("至少")) && !visible.contains(QStringLiteral("超过")));
            QCOMPARE(manager.taskById(taskId).state, AsyncTaskState::Finished);
        }
        const QString evidence = qEnvironmentVariable("NOTEPADSAFE_SEARCH_CAPACITY_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            QVERIFY(document.window->grab().save(QDir(evidence).filePath(
                QStringLiteral("capacity-%1-%2.png").arg(regex ? "regex" : "literal").arg(hits))));
            QVERIFY(dialog->grab().save(QDir(evidence).filePath(
                QStringLiteral("capacity-dialog-%1-%2.png").arg(regex ? "regex" : "literal").arg(hits))));
        }

        document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                              long(LastRetainedOffset), long(LastRetainedOffset + 1));
        for (const NavigationStep &step : navigationSteps(hits)) {
            wrap->setChecked(step.wrap);
            QPushButton *button = step.forward ? next : previous;
            QVERIFY(button->isEnabled());
            QTest::mouseClick(button, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && previous->isEnabled()
                && selectedAt(document.tab, step.offset)
                && (step.found ? status->text().isEmpty()
                               : status->text().contains(QStringLiteral("未找到"))), 5000);
        }
        // EOF is past the final newline/dirty suffix, not merely the last hit.
        const qint64 eof = document.body.size();
        document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL, long(eof), long(eof));
        wrap->setChecked(false);
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && selectionStart(document.tab) == eof
            && document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND) == eof
            && status->text().contains(QStringLiteral("未找到")), 5000);
        wrap->setChecked(true);
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && selectedAt(document.tab, 0)
            && status->text().isEmpty(), 5000);
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QVERIFY(model && tree->model() == model.data());
        QCOMPARE(model->totalMatches(), qint64(RetainedHits));
        QCOMPARE(model->isIncomplete(), partial);
    }

    void publicSynchronousFindAllCapacity_data() { addCapacityData(); }

    void publicSynchronousFindAllCapacity()
    {
        QFETCH(bool, regex);
        QFETCH(int, hits);
        CapacityDocument document;
        QVERIFY(document.open(hits));
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        const QVector<SearchMatch> matches = document.tab->findAll(
            QStringLiteral("x"), regex, true, false);
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QCOMPARE(matches.size(), qsizetype(RetainedHits));
        QCOMPARE(matches.first().byteOffset, qint64(0));
        QCOMPARE(matches.last().byteOffset, LastRetainedOffset);
        QCOMPARE(matches.first().byteLength, qint32(1));
        QCOMPARE(matches.last().byteLength, qint32(1));
        QCOMPARE(document.tab->lastFindAllWasTruncated(), hits > RetainedHits);
        if (hits == RetainedHits) QVERIFY(document.tab->lastSearchError().isEmpty());
        document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                              long(LastRetainedOffset), long(LastRetainedOffset + 1));
        for (const NavigationStep &step : navigationSteps(hits)) {
            QCOMPARE(document.tab->findNext(QStringLiteral("x"), regex, true, false,
                                            step.forward, step.wrap), step.found);
            QVERIFY(selectedAt(document.tab, step.offset));
            QVERIFY(document.tab->lastSearchError().isEmpty());
        }
        const qint64 eof = document.body.size();
        document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL, long(eof), long(eof));
        QVERIFY(!document.tab->findNext(QStringLiteral("x"), regex, true, false, true, false));
        QCOMPARE(selectionStart(document.tab), eof);
        QCOMPARE(document.tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND), long(eof));
        QVERIFY(document.tab->findNext(QStringLiteral("x"), regex, true, false, true, true));
        QVERIFY(selectedAt(document.tab, 0));
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
    }

    // Supplemental refusal/undo regression, not one of the original eight
    // capacity red rows. A bounded preview must never authorize partial edits.
    void partialReplaceAllRefusesWithoutChanges_data()
    {
        QTest::addColumn<bool>("regex");
        QTest::newRow("literal-overflow-refusal") << false;
        QTest::newRow("regex-overflow-refusal") << true;
    }

    void partialReplaceAllRefusesWithoutChanges()
    {
        QFETCH(bool, regex);
        CapacityDocument document;
        QVERIFY(document.open(200001, true));
        auto *editor = document.tab->editor();
        editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, long(16), long(17));
        const bool couldUndo = editor->SendScintilla(QsciScintillaBase::SCI_CANUNDO) != 0;
        const bool couldRedo = editor->SendScintilla(QsciScintillaBase::SCI_CANREDO) != 0;
        QVERIFY(couldUndo);
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QCOMPARE(document.tab->replaceAll(QStringLiteral("x"), QStringLiteral("y"),
                                          regex, true, false), 0);
        QVERIFY(document.tab->lastFindAllWasTruncated());
        const QString reason = document.tab->lastSearchError();
        QVERIFY(reason.contains(QStringLiteral("未完整"))
                && reason.contains(QStringLiteral("替换"))
                && reason.contains(QStringLiteral("缩小")));
        QVERIFY2(document.safetyError().isEmpty(), qPrintable(document.safetyError()));
        QVERIFY(selectedAt(document.tab, 16));
        QCOMPARE(editor->SendScintilla(QsciScintillaBase::SCI_CANUNDO) != 0, couldUndo);
        QCOMPARE(editor->SendScintilla(QsciScintillaBase::SCI_CANREDO) != 0, couldRedo);

        // Refusal must not insert an empty undo group or disturb the user's
        // previous edit. Undo/Redo themselves legitimately advance revision.
        editor->undo();
        QCOMPARE(editor->text().toUtf8(), document.source);
        QVERIFY(!document.tab->isModified());
        const quint64 undoRevision = document.tab->contentRevision();
        QVERIFY(undoRevision > document.state.revision);
        QVERIFY(editor->SendScintilla(QsciScintillaBase::SCI_CANREDO) != 0);
        editor->redo();
        QCOMPARE(editor->text().toUtf8(), document.body);
        QVERIFY(document.tab->isModified());
        QVERIFY(document.tab->contentRevision() > undoRevision);
        QCOMPARE(document.tab->documentState().id, document.state.id);
        QCOMPARE(document.tab->documentState().capabilities, document.state.capabilities);
        const quint64 revisionBeforeEmptyQuery = document.tab->contentRevision();
        QCOMPARE(document.tab->replaceAll(QString(), QStringLiteral("y"), regex, true, false), 0);
        QVERIFY(document.tab->lastSearchError().contains(QStringLiteral("为空")));
        QCOMPARE(editor->text().toUtf8(), document.body);
        QVERIFY(document.tab->isModified());
        QCOMPARE(document.tab->contentRevision(), revisionBeforeEmptyQuery);
        QFile file(document.path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray actual = file.readAll();
        QCOMPARE(actual, document.source);
        QCOMPARE(QCryptographicHash::hash(actual, QCryptographicHash::Sha256), document.sourceHash);
    }
};

QTEST_MAIN(SmokeSearchCapacity)
#include "smoke_search_capacity.moc"
