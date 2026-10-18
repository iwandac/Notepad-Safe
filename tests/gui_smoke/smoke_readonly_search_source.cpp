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
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>
#include <utility>

#include "AsyncTaskManager.h"
#include "EditorTab.h"
#include "FindOverlay.h"
#include "LargeFileSearch.h"
#include "MainWindow.h"
#include "ReadOnlySearchSource.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
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

class SmokeReadOnlySearchSource final : public QObject {
    Q_OBJECT
private slots:
    void completedSpoolSurvivesCachedNavigationAndDifferentQuery()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const bool hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray oldProfile = qgetenv("NOTEPADSAFE_DATADIR");
        const QString profile = fixture.filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(profile));
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreProfile = qScopeGuard([&] {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", oldProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        constexpr qint64 SourceSize = 33LL * 1024 * 1024;
        constexpr int HitCount = 60050;
        constexpr qint64 OtherOffset = 2LL * 1024 * 1024 + 8;
        const QByteArray other("Z_NAV_SECOND_629");
        QByteArray bytes = (QByteArray(127, 'p') + '\n').repeated(int(SourceSize / 128));
        bytes.replace(0, HitCount * 2, QByteArray("X\n").repeated(HitCount));
        bytes.replace(OtherOffset, other.size(), other);
        const QString path = fixture.filePath(QStringLiteral("ordinary-spool-cache.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), SourceSize);
        }
        const QByteArray expectedHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        QCOMPARE(fileHash(path), expectedHash);
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        window.raise();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->count() == 1);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && target->windowedReadOnlyProvider());
        QPointer<WindowedReadOnlyProvider> provider = target->windowedReadOnlyProvider();
        const DocumentState initial = target->documentState();
        QVERIFY(target->isReadOnlyByPolicy() && !target->isBinaryDocument());
        QVERIFY(!target->isSecureDocument() && !target->isHexPresentation());
        QVERIFY(!initial.id.isNull() && !initial.dirty && !target->isModified());
        QVERIFY(!initial.capabilities.testFlag(DocumentCapability::Edit));
        QCOMPARE(target->contentSizeBytes(), SourceSize);
        auto &manager = AsyncTaskManager::instance();
        QSignalSpy terminals(&manager, &AsyncTaskManager::taskFinished);
        QVector<qint64> ownedTasks;
        const auto added = connect(&manager, &AsyncTaskManager::taskAdded, &window,
            [&](qint64 id) {
                if (manager.taskById(id).title.contains(QFileInfo(path).fileName()))
                    ownedTasks.append(id);
            });
        const auto cleanup = qScopeGuard([&] {
            disconnect(added);
            for (qint64 id : std::as_const(ownedTasks)) manager.cancel(id);
            if (target) target->shutdownL2BeforeClose();
        });
        const auto guards = [&] {
            return target && provider && target->documentState().id == initial.id
                && target->contentRevision() == initial.revision
                && target->documentState().capabilities == initial.capabilities
                && !target->isModified() && target->isReadOnlyByPolicy()
                && provider == target->windowedReadOnlyProvider()
                && target->contentSizeBytes() == SourceSize
                && fileHash(path) == expectedHash && !QApplication::activeModalWidget();
        };
        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        QVERIFY(findAction);
        findAction->trigger();
        auto *dialog = window.findChild<FindOverlay *>();
        QVERIFY(dialog);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *all = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *dock = window.findChild<SearchResultsDock *>();
        QVERIFY(pattern && caseSensitive && wrap && next && previous && all && cancel && status && dock);
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(tree);
        caseSensitive->setChecked(true); // This case isolates ownership, not dense case folding.
        pattern->setText(QStringLiteral("X"));
        QTest::mouseClick(all, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible() && tree->model(), 15000);
        QPointer<SearchResultsModel> model = qobject_cast<SearchResultsModel *>(tree->model());
        QPointer<ReadOnlySearchSource> source = target->findChild<ReadOnlySearchSource *>();
        QVERIFY(model && source && !model->isIncomplete());
        auto *engine = source->findAllEngine();
        QCOMPARE(model->totalMatches(), qint64(HitCount));
        qint64 findAllTaskId = 0;
        for (qint64 id : std::as_const(ownedTasks)) {
            if (manager.taskById(id).categoryTag == QStringLiteral("Search")) {
                QCOMPARE(findAllTaskId, qint64(0));
                findAllTaskId = id;
            }
        }
        QVERIFY(findAllTaskId > 0);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(findAllTaskId).state,
                                  AsyncTaskState::Finished, 2000);
        QVERIFY(model->rowCount() > 50000); // The inspected tail is a virtual spool row.
        QVERIFY(engine->memoryMatches().size() <= 50000 && engine->overrunCount() > 0);
        const QString spoolPath = engine->overrunFilePath();
        const QByteArray spoolHash = fileHash(spoolPath);
        QVERIFY(!spoolPath.isEmpty() && !spoolHash.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!source->isReady() && !engine->sourceInUse()
            && !source->navigationEngine()->sourceInUse(), 2000);
        QVERIFY(guards());
        const int tailRow = model->rowCount() - 1;
        constexpr qint64 TailOffset = 2LL * (HitCount - 1);
        const auto tail = model->groupedLineAt(tailRow);
        QCOMPARE(tail.firstByteOffset, TailOffset);
        QCOMPARE(tail.firstLength, 1);
        provider->goToOffset(OtherOffset, other.size());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), OtherOffset, 2000);
        QVERIFY(provider->selectionStartOffset() != TailOffset);
        const QModelIndex tailIndex = model->index(tailRow, SearchResultsModel::COL_CONTEXT);
        tree->scrollTo(tailIndex, QAbstractItemView::PositionAtCenter);
        QTRY_VERIFY_WITH_TIMEOUT(tree->visualRect(tailIndex).isValid(), 2000);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                         tree->visualRect(tailIndex).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          tree->visualRect(tailIndex).center());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), TailOffset, 2000);
        QCOMPARE(provider->selectionEndOffset(), TailOffset + 1);
        const qsizetype beforeCached = ownedTasks.size();
        wrap->setChecked(false);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset);
        QVERIFY(status->text().contains(QStringLiteral("未找到")));
        wrap->setChecked(true);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), qint64(0));
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset);
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(provider->selectionStartOffset(), TailOffset - 2);
        QCOMPARE(ownedTasks.size(), beforeCached);
        QVERIFY(!source->isReady());
        QVERIFY(guards());
        pattern->setText(QString::fromLatin1(other));
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled()
            && provider->selectionStartOffset() == OtherOffset, 8000);
        QCOMPARE(provider->selectionEndOffset(), OtherOffset + other.size());
        int preparations = 0;
        int navigations = 0;
        for (qsizetype i = beforeCached; i < ownedTasks.size(); ++i) {
            const AsyncTask task = manager.taskById(ownedTasks.at(i));
            if (task.categoryTag == QStringLiteral("Mmap")) ++preparations;
            if (task.categoryTag == QStringLiteral("SearchNavigation")) ++navigations;
            QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Finished, 2000);
        }
        QCOMPARE(preparations, 1);
        QCOMPARE(navigations, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!source->isReady(), 2000);
        QVERIFY(guards());
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel *>(model.data()));
        QCOMPARE(model->totalMatches(), qint64(HitCount));
        QCOMPARE(model->groupedLineAt(tailRow).firstByteOffset, TailOffset);
        QCOMPARE(model->groupedLineAt(tailRow).matchByteRanges, tail.matchByteRanges);
        QCOMPARE(fileHash(spoolPath), spoolHash);
        QCOMPARE(provider->sourceBytes(OtherOffset, other.size()), other);
        for (qint64 id : std::as_const(ownedTasks)) {
            const QString category = manager.taskById(id).categoryTag;
            if (category != QStringLiteral("Search") && category != QStringLiteral("SearchNavigation")
                && category != QStringLiteral("Mmap")) continue;
            int count = 0;
            for (const auto &args : terminals)
                if (args.constFirst().toLongLong() == id) ++count;
            QCOMPARE(count, 1);
        }
    }
};

QTEST_MAIN(SmokeReadOnlySearchSource)
#include "smoke_readonly_search_source.moc"
