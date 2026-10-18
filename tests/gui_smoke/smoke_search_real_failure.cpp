#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>

#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "DynamicThresholds.h"
#include "EditorTab.h"
#include "FindOverlay.h"
#include "LargeFileSearch.h"
#include "ReadOnlySearchSource.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

namespace {
QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}
}

class SmokeSearchRealFailure : public QObject {
    Q_OBJECT
private slots:
    void spoolCreationFailureSettlesDomainAndTask()
    {
        // Real filesystem failure, no reportError injection or altered budget.
        // Only this process's temporary path is redirected; the user's shared
        // temp/notepad-safe directory is never touched.
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString profile = fixture.filePath(QStringLiteral("profile"));
        const QString scratch = fixture.filePath(QStringLiteral("scratch"));
        QVERIFY(QDir().mkpath(profile));
        QVERIFY(QDir().mkpath(scratch));
        const QByteArray names[] = {"NOTEPADSAFE_DATADIR", "TMP", "TEMP"};
        QByteArray oldValues[3];
        bool wasSet[3];
        for (int i = 0; i < 3; ++i) {
            wasSet[i] = qEnvironmentVariableIsSet(names[i].constData());
            oldValues[i] = qgetenv(names[i].constData());
            qputenv(names[i].constData(), (i == 0 ? profile : scratch).toUtf8());
        }
        const auto restoreEnvironment = qScopeGuard([&] {
            for (int i = 0; i < 3; ++i) {
                if (wasSet[i]) qputenv(names[i].constData(), oldValues[i]);
                else qunsetenv(names[i].constData());
            }
        });
        QCOMPARE(QDir(QDir::tempPath()).canonicalPath(), QDir(scratch).canonicalPath());
        const QString obstruction = QDir(scratch).filePath(QStringLiteral("notepad-safe"));
        const QByteArray obstructionBytes("Owned spool-directory obstruction.\n");
        {
            QFile file(obstruction);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(file.write(obstructionBytes), qint64(obstructionBytes.size()));
        }
        QVERIFY(QFileInfo(obstruction).isFile());

        constexpr qint64 sourceSize = 33LL * 1024 * 1024;
        constexpr int hitCount = 60050;
        const QByteArray query("X");
        const QByteArray warmup("PREPARE_REAL_SOURCE_251\n");
        QByteArray prefix = warmup;
        for (int i = 0; i < hitCount; ++i)
            prefix += query + '\n';
        QVERIFY(prefix.size() < sourceSize);
        const QString path = fixture.filePath(QStringLiteral("real-spool-failure.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(prefix), qint64(prefix.size()));
            QByteArray padding(1024 * 1024, 'a');
            for (int i = 127; i < padding.size(); i += 128) padding[i] = '\n';
            while (file.pos() < sourceSize) {
                const qint64 count = qMin<qint64>(padding.size(), sourceSize - file.pos());
                QCOMPARE(file.write(padding.constData(), count), count);
            }
        }
        const QByteArray originalHash = fileHash(path);
        QVERIFY(!originalHash.isEmpty());
        MainWindow window;
        window.show();
        window.raise();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->count() == 1);
        auto *target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && target->windowedReadOnlyProvider());
        auto *provider = target->windowedReadOnlyProvider();
        QVERIFY(target->isReadOnlyByPolicy() && !target->isBinaryDocument());
        QVERIFY(!target->isHexPresentation() && !target->isSecureDocument());
        QCOMPARE(target->contentSizeBytes(), sourceSize);
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        QVERIFY(findAction);
        findAction->trigger();
        auto *dialog = window.findChild<FindOverlay *>();
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *whole = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *all = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        QVERIFY(pattern && scope && regex && caseSensitive && hex && whole && next && all);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        // Keep the real dense single-case input on the product default.  The
        // caseless candidate enumerator must not rescan the same remaining
        // suffix for the absent alternate case before every one of 60,050
        // matches.  This still reaches the real spool-creation failure below;
        // no timeout or result budget is relaxed to hide that regression.
        caseSensitive->setChecked(false);
        hex->setChecked(false);
        whole->setChecked(false);
        pattern->setText(QString::fromLatin1(warmup).trimmed());
        QTest::mouseClick(next, Qt::LeftButton);
        auto &manager = AsyncTaskManager::instance();
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled() && !provider->characterCountPending()
            && provider->selectionStartOffset() == 0
            && provider->selectionEndOffset() == warmup.size() - 1
            && manager.queuedCount() == 0 && manager.runningCount() == 0
            && manager.activeThreadCount() == 0, 8000);
        auto *source = target->findChild<ReadOnlySearchSource *>();
        QVERIFY(source && !source->isReady());
        QVERIFY(!source->findAllEngine()->sourceInUse()
                && !source->navigationEngine()->sourceInUse());
        auto *engine = source->findAllEngine();
        QVERIFY(engine);
        const qint64 capacity = DynamicThresholds::instance().MAX_MATCHES_MEM;
        QVERIFY(capacity > 0 && capacity < hitCount);
        const DocumentState before = target->documentState();
        const QString projectionBefore = target->editor()->text();
        QVERIFY(!before.id.isNull() && !before.dirty && !target->isModified());
        QVERIFY(!before.capabilities.testFlag(DocumentCapability::Edit));
        qRegisterMetaType<SearchResultsModel *>("SearchResultsModel*");
        QSignalSpy domainFailure(engine, &LargeFileSearch::failed);
        QSignalSpy domainFinished(target, &EditorTab::findAllL2Finished);
        QSignalSpy added(&manager, &AsyncTaskManager::taskAdded);
        pattern->setText(QString::fromLatin1(query));
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(domainFinished.size(), qsizetype(1), 12000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.queuedCount() == 0
            && manager.runningCount() == 0 && manager.activeThreadCount() == 0, 3000);
        QCOMPARE(domainFailure.size(), qsizetype(1));
        const QString raw = domainFailure.constFirst().constFirst().toString();
        QVERIFY2(raw.contains(QStringLiteral("临时存储写入失败")), qPrintable(raw));
        qint64 searchId = 0;
        for (const auto &args : added) {
            const qint64 id = args.constFirst().toLongLong();
            if (manager.taskById(id).categoryTag == QStringLiteral("Search")) {
                QCOMPARE(searchId, qint64(0));
                searchId = id;
            }
        }
        QVERIFY(searchId > 0);
        auto *model = qvariant_cast<SearchResultsModel *>(domainFinished.constFirst().at(0));
        QVERIFY(model && model->isIncomplete());
        QCOMPARE(model->totalMatches(), capacity);
        QCOMPARE(model->incompleteReason(), raw);
        auto *dock = window.findChild<SearchResultsDock *>();
        QVERIFY(dock && dock->isVisible());
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(tree && summary);
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel *>(model));
        QVERIFY(summary->text().contains(QStringLiteral("未完成")));
        QVERIFY(summary->text().contains(QStringLiteral("重试")));
        QVERIFY(all->isEnabled() && next->isEnabled() && pattern->isEnabled());
        // Check source authority and retained partial ranges before task truth.
        QCOMPARE(fileHash(path), originalHash);
        QCOMPARE(target->documentState().id, before.id);
        QCOMPARE(target->contentRevision(), before.revision);
        QCOMPARE(target->documentState().capabilities, before.capabilities);
        QCOMPARE(target->documentState().dirty, before.dirty);
        QCOMPARE(target->editor()->text(), projectionBefore);
        QVERIFY(target->isReadOnlyByPolicy() && target->editor()->isReadOnly());
        QCOMPARE(provider->sourceBytes(0, prefix.size()), prefix);
        QCOMPARE(model->matchByteOffset(model->index(0, 0)), qint64(warmup.size()));
        QCOMPARE(fileHash(obstruction), QCryptographicHash::hash(
            obstructionBytes, QCryptographicHash::Sha256));

        const AsyncTask task = manager.taskById(searchId);
        QCOMPARE(task.state, AsyncTaskState::Failed);
        QCOMPARE(task.error.message, raw);
        QVERIFY(task.error.userMessage.contains(QStringLiteral("临时")));
        QVERIFY(task.error.userMessage.contains(QStringLiteral("重试")));
        AsyncTaskPanelDialog panel;
        panel.show();
        panel.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        QVERIFY(list && list->model());
        QModelIndex found;
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            const auto index = list->model()->index(row, 0);
            if (index.data(AsyncTaskManager::TaskIdRole).toLongLong() == searchId) found = index;
        }
        QVERIFY(found.isValid());
        list->setCurrentIndex(found);
        auto *taskSummary = panel.findChild<QLabel *>(QStringLiteral("backgroundTaskSummary"));
        QVERIFY(taskSummary);
        QTRY_VERIFY_WITH_TIMEOUT(taskSummary->text().contains(task.error.userMessage), 1000);
        QVERIFY(!taskSummary->text().contains(raw));
        QCOMPARE(fileHash(path), originalHash);
    }
};

QTEST_MAIN(SmokeSearchRealFailure)
#include "smoke_search_real_failure.moc"
