#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QAction>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeView>

#include "EditorTab.h"
#include "FindOverlay.h"
#include "MainWindow.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "WindowedReadOnlyProvider.h"

namespace {

QByteArray fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    while (!file.atEnd()) {
        const qint64 read = file.read(chunk.data(), chunk.size());
        if (read < 0)
            return {};
        if (read > 0)
            hash.addData(QByteArrayView(chunk.constData(), read));
    }
    return hash.result();
}

QString retainedB2Path()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.absoluteFilePath(QStringLiteral("../testfiles/smalltestfile")),
        appDir.absoluteFilePath(QStringLiteral("../../testfiles/smalltestfile")),
        QDir::current().absoluteFilePath(QStringLiteral("testfiles/smalltestfile"))
    };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.size() > 32LL * 1024 * 1024
            && info.size() <= 2LL * 1024 * 1024 * 1024) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

EditorTab *currentEditorTab(MainWindow *window)
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

QString plainMenuText(QString text)
{
    text.remove(QLatin1Char('&'));
    text.remove(QStringLiteral("..."));
    text.remove(QChar(0x2026));
    return text.trimmed();
}

} // namespace

class SmokeHexReadonlySearch final : public QObject {
    Q_OBJECT

private slots:
    void tc_hx_1b_b2_readonly_commands_search_and_dock()
    {
        QTemporaryDir profile;
        QVERIFY2(profile.isValid(), "HX-1B-B2: isolated profile unavailable");
        const QByteArray oldDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.path().toUtf8());

        const QString sourcePath = retainedB2Path();
        QVERIFY2(!sourcePath.isEmpty(),
                 "HX-1B-B2: retained testfiles/smalltestfile is required; do not synthesize or copy B2");
        const QFileInfo sourceInfo(sourcePath);
        const QByteArray sourceHash = fileSha256(sourcePath);
        QVERIFY2(!sourceHash.isEmpty(), "HX-1B-B2: source SHA-256 failed");

        auto *window = new MainWindow;
        window->resize(1400, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({sourcePath});
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
        EditorTab *tab = currentEditorTab(window);
        QTRY_VERIFY_WITH_TIMEOUT(tab && tab->isL2Active(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(tab->windowedReadOnlyProvider(), 10000);
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(tab->isHexPresentation());
        QVERIFY(tab->isReadOnlyByPolicy());
        QCOMPARE(provider->fileSize(), sourceInfo.size());

        // The retained Parquet footer gives this real B2 sample a stable,
        // distinctive bounded marker without reading the file wholesale.
        const qint64 markerOffset = sourceInfo.size() - 64;
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QVERIFY(source.seek(markerOffset));
        const QByteArray marker = source.read(64);
        QCOMPARE(marker.size(), 64);
        source.close();
        const QString query = QString::fromLatin1(marker.toHex(' ').toUpper());

        const auto assertSourceUnchanged = [&]() {
            QCOMPARE(QFileInfo(sourcePath).size(), sourceInfo.size());
            QCOMPARE(fileSha256(sourcePath), sourceHash);
        };
        const auto action = [&](const char *id) {
            return window->findChild<QAction *>(QString::fromLatin1(id));
        };
        QAction *findAction = action("search.find");
        QAction *replaceAction = action("search.replace");
        QAction *cutAction = action("edit.cut");
        QAction *copyAction = action("edit.copy");
        QAction *pasteAction = action("edit.paste");
        QAction *deleteLineAction = action("edit.line.delete");
        QAction *hexInsertAction = action("edit.hexInsertMode");
        QVERIFY(findAction && replaceAction && cutAction && copyAction
                && pasteAction && deleteLineAction && hexInsertAction);

        provider->goToOffset(markerOffset, marker.size());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), markerOffset, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                                  markerOffset + marker.size(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(copyAction->isEnabled(), 2000);
        QVERIFY(findAction->isEnabled());
        QVERIFY(!replaceAction->isEnabled());
        QVERIFY(!cutAction->isEnabled());
        QVERIFY(!pasteAction->isEnabled());
        QVERIFY(!deleteLineAction->isEnabled());
        QVERIFY(!hexInsertAction->isEnabled() && !hexInsertAction->isChecked());
        QVERIFY(!findAction->isCheckable() && !replaceAction->isCheckable()
                && !cutAction->isCheckable() && !copyAction->isCheckable()
                && !pasteAction->isCheckable() && !deleteLineAction->isCheckable());

        QApplication::clipboard()->clear();
        copyAction->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), query, 1000);
        assertSourceUnchanged();
        for (QAction *blocked : {replaceAction, cutAction, pasteAction,
                                 deleteLineAction, hexInsertAction}) {
            blocked->trigger();
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            assertSourceUnchanged();
        }

        QWidget *hexSurface = provider->editor()->viewport();
        QVERIFY(hexSurface);
        provider->editor()->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(provider->editor()->hasFocus(), 1000);
        struct KeyStroke {
            Qt::Key key;
            Qt::KeyboardModifiers modifiers;
        };
        const KeyStroke blockedKeys[] = {
            {Qt::Key_X, Qt::ControlModifier},
            {Qt::Key_V, Qt::ControlModifier},
            {Qt::Key_Delete, Qt::NoModifier}
        };
        for (const KeyStroke &key : blockedKeys) {
            QTest::keyClick(provider->editor(), key.key, key.modifiers);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            assertSourceUnchanged();
        }

        provider->goToOffset(markerOffset, marker.size());
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), markerOffset, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(copyAction->isEnabled(), 2000);
        bool contextMenuSeen = false;
        bool contextCopyEnabled = false;
        bool contextMutationDisabled = true;
        QTimer::singleShot(80, window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            contextMenuSeen = true;
            for (QAction *entry : menu->actions()) {
                const QString text = plainMenuText(entry->text());
                if (entry->objectName() == QStringLiteral("edit.copy")
                    || text.contains(QStringLiteral("复制"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Copy"), Qt::CaseInsensitive)) {
                    contextCopyEnabled = entry->isEnabled();
                }
                if (entry->objectName() == QStringLiteral("edit.cut")
                    || entry->objectName() == QStringLiteral("edit.paste")
                    || text.contains(QStringLiteral("剪切"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("粘贴"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("删除"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Cut"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Paste"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Delete"), Qt::CaseInsensitive)) {
                    contextMutationDisabled = contextMutationDisabled && !entry->isEnabled();
                }
            }
            menu->close();
        });
        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, QPoint(12, 12),
                                       hexSurface->mapToGlobal(QPoint(12, 12)));
        QApplication::sendEvent(hexSurface, &contextEvent);
        QVERIFY(contextMenuSeen);
        QVERIFY(contextCopyEnabled);
        QVERIFY(contextMutationDisabled);
        assertSourceUnchanged();

        QMimeData textDrop;
        textDrop.setText(QStringLiteral("MUST_NOT_MUTATE_B2"));
        QDragEnterEvent dragEnter(QPoint(8, 8), Qt::CopyAction, &textDrop,
                                  Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hexSurface, &dragEnter);
        QVERIFY(!dragEnter.isAccepted());
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &textDrop,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hexSurface, &drop);
        QVERIFY(!drop.isAccepted());
        assertSourceUnchanged();

        findAction->trigger();
        auto *dialog = window->findChild<FindOverlay *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(
            QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(
            QStringLiteral("wholeWordSearchCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *replace = dialog->findChild<QPushButton *>(QStringLiteral("replaceButton"));
        auto *replaceAll = dialog->findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        QVERIFY(pattern && hex && regex && caseSensitive && wholeWord && wrap
                && next && previous && findAll && replace && replaceAll && status);
        pattern->setText(query);
        if (!hex->isChecked())
            QTest::mouseClick(hex, Qt::LeftButton);
        QVERIFY(hex->isChecked() && hex->isEnabled());
        QVERIFY(!regex->isEnabled() && !regex->isChecked());
        QVERIFY(!caseSensitive->isEnabled() && !caseSensitive->isChecked());
        QVERIFY(!wholeWord->isEnabled() && !wholeWord->isChecked());
        QVERIFY(!replace->isEnabled() && !replaceAll->isEnabled());
        for (QCheckBox *blocked : {regex, caseSensitive, wholeWord})
            QTest::mouseClick(blocked, Qt::LeftButton);
        QVERIFY(!regex->isChecked() && !caseSensitive->isChecked()
                && !wholeWord->isChecked());
        QTest::mouseClick(replace, Qt::LeftButton);
        QTest::mouseClick(replaceAll, Qt::LeftButton);
        assertSourceUnchanged();

        wrap->setChecked(false);
        provider->goToOffset(sourceInfo.size());
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled(), 10000);
        QVERIFY(status->text().contains(QStringLiteral("未找到"))
                || status->text().contains(QStringLiteral("边界")));
        provider->goToOffset(markerOffset + marker.size());
        QTest::mouseClick(previous, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), markerOffset, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                                  markerOffset + marker.size(), 15000);
        QCOMPARE(provider->sourceBytes(markerOffset, marker.size()), marker);
        wrap->setChecked(true);
        provider->goToOffset(sourceInfo.size());
        QTest::mouseClick(next, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), markerOffset, 15000);
        assertSourceUnchanged();

        auto *dock = window->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        auto *tree = dock ? dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree")) : nullptr;
        QVERIFY(dock && tree);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible() && tree->model(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled(), 30000);
        auto *model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY(model);
        QCOMPARE(model->totalMatches(), qint64(1));
        const QModelIndex result = model->index(0, SearchResultsModel::COL_CONTEXT);
        QVERIFY(result.isValid());

        // Leave the frozen B2 result visible while another projection becomes
        // current. A real Dock activation must resolve the original document
        // identity/revision and bring its Hex projection back to the front.
        const QString otherPath = profile.filePath(QStringLiteral("other-projection.txt"));
        QFile other(otherPath);
        QVERIFY(other.open(QIODevice::WriteOnly));
        QCOMPARE(other.write("ordinary text projection\n"), qint64(25));
        other.close();
        const quint64 expectedRevision = tab->contentRevision();
        window->openFilesFromExternal({otherPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(tabs->currentWidget() != tab, 5000);

        provider->goToOffset(0);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), qint64(0), 5000);
        QSignalSpy jumpSpy(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        tree->scrollTo(result, QAbstractItemView::PositionAtCenter);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QRect resultRect = tree->visualRect(result);
        QVERIFY(resultRect.isValid());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                          Qt::NoModifier, resultRect.center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                           Qt::NoModifier, resultRect.center());
        QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 3000);
        QCOMPARE(jumpSpy.first().at(0).toUuid(), tab->documentState().id);
        QCOMPARE(jumpSpy.first().at(1).toULongLong(), expectedRevision);
        QCOMPARE(jumpSpy.first().at(2).toLongLong(), markerOffset);
        QCOMPARE(jumpSpy.first().at(3).toInt(), marker.size());
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentWidget(), static_cast<QWidget *>(tab), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), markerOffset, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                                  markerOffset + marker.size(), 5000);
        QCOMPARE(provider->sourceBytes(markerOffset, marker.size()), marker);
        assertSourceUnchanged();

        dialog->close();
        closeAllTabs(window);
        window->close();
        delete window;
        if (hadDataDir)
            qputenv("NOTEPADSAFE_DATADIR", oldDataDir);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");
    }
};

QTEST_MAIN(SmokeHexReadonlySearch)
#include "smoke_hex_readonly_search.moc"
