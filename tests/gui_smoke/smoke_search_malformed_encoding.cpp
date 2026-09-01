#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QAction>
#include <QtGui/QFontMetrics>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeView>

#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "MainWindow.h"
#include "SearchResultsDelegate.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "VirtualFileView.h"

class SmokeSearchMalformedEncoding : public QObject {
    Q_OBJECT

private slots:
    void malformedLiteralSearchPreservesSourceByteRanges_data()
    {
        QTest::addColumn<int>("encodingValue");
        QTest::newRow("utf16le-malformed") << int(TextEncoding::Utf16LE);
        QTest::newRow("utf16be-malformed") << int(TextEncoding::Utf16BE);
        QTest::newRow("gbk-orphan-tail") << int(TextEncoding::GBK);
    }

    void malformedLiteralSearchPreservesSourceByteRanges()
    {
        QFETCH(int, encodingValue);
        const auto encoding = static_cast<TextEncoding>(encodingValue);
        struct QueryOracle {
            QString text;
            QVector<QPair<qint64, int>> byteRanges;
            QVector<int> displayStarts;
        };
        QVector<QueryOracle> queries;
        QByteArray tail;
        QString encodingActionName;
        QString expectedProjection;
        if (encoding == TextEncoding::GBK) {
            tail = QByteArray::fromHex("41d6d0427c41d6d0427c81");
            encodingActionName = QStringLiteral("encoding.display.gbk");
            expectedProjection = QString(64, QChar(0x0001))
                + QStringLiteral("A\u4e2dB|A\u4e2dB|\ufffd");
            queries.append({QStringLiteral("\u4e2d"),
                            {{65, 2}, {70, 2}}, {65, 69}});
            queries.append({QStringLiteral("\ufffd"), {{74, 1}}, {72}});
        } else {
            QVERIFY(encoding == TextEncoding::Utf16LE
                    || encoding == TextEncoding::Utf16BE);
            const bool littleEndian = encoding == TextEncoding::Utf16LE;
            tail = QByteArray::fromHex(littleEndian
                ? "41003dd800de7c0000d842007c0000dc43007c00fdff44007c007f"
                : "0041d83dde00007cd8000042007cdc000043007cfffd0044007c7f");
            encodingActionName = littleEndian
                ? QStringLiteral("encoding.display.utf16le")
                : QStringLiteral("encoding.display.utf16be");
            // Manual oracle: 32 U+0101 prefix characters, then A, a valid
            // surrogate pair, |, lone high surrogate, B, |, lone low surrogate,
            // C, |, actual U+FFFD, D, |, and one odd EOF byte. No product codec
            // is used to generate either expected text or expected positions.
            expectedProjection = QString(32, QChar(0x0101))
                + QStringLiteral("A\U0001f600|\ufffdB|\ufffdC|\ufffdD|\ufffd");
            queries.append({QStringLiteral("\ufffd"),
                            {{72, 2}, {78, 2}, {84, 2}, {90, 1}},
                            {36, 39, 42, 45}});
        }
        // The nonzero control prefix forces real Binary classification without
        // a BOM/UTF-16 heuristic routing this small fixture into decoded L1.
        const QByteArray original = QByteArray(64, char(0x01)) + tail;
        QCOMPARE(original.size(), encoding == TextEncoding::GBK ? 75 : 91);
        for (const QueryOracle &query : queries) {
            QCOMPARE(query.byteRanges.size(), query.displayStarts.size());
            for (int index = 0; index < query.displayStarts.size(); ++index) {
                QCOMPARE(expectedProjection.mid(query.displayStarts.at(index),
                                                query.text.size()), query.text);
            }
        }

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profile = temporary.filePath(QStringLiteral("app-data"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8());
        const auto restoreDataDir = qScopeGuard([&]() {
            if (hadDataDir)
                qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
            else
                qunsetenv("NOTEPADSAFE_DATADIR");
        });
        const QString path = temporary.filePath(QStringLiteral("malformed-source.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QCOMPARE(source.write(original), qint64(original.size()));
        }
        const auto diskBytes = [&]() {
            QFile source(path);
            return source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray{};
        };
        const auto sourceHash = [&]() {
            return QCryptographicHash::hash(diskBytes(), QCryptographicHash::Sha256);
        };
        QCOMPARE(diskBytes(), original);
        const QByteArray originalHash = sourceHash();

        MainWindow window;
        const auto cleanupWindow = qScopeGuard([&]() {
            // Do not run Save/Discard or keep background workers alive past
            // their source. This is the existing MainWindow smoke teardown.
            if (auto *tabs = window.findChild<QTabWidget *>()) {
                while (tabs->count() > 0) {
                    QWidget *page = tabs->widget(0);
                    if (auto *tab = qobject_cast<EditorTab *>(page))
                        tab->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    delete page;
                }
            }
        });
        bool sawModal = false;
        QTimer modalGuard;
        modalGuard.setInterval(10);
        connect(&modalGuard, &QTimer::timeout, &window, [&]() {
            if (QApplication::activeModalWidget())
                sawModal = true;
            for (auto *box : window.findChildren<QMessageBox *>()) {
                if (box->isVisible()) {
                    sawModal = true;
                    box->reject();
                }
            }
        });
        modalGuard.start();
        window.resize(1280, 820);
        window.showNormal();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openFilesFromExternal({path});
        auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 3000);
        QPointer<EditorTab> target = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(target && target->isL2Mode() && target->isBinaryDocument());
        QTRY_VERIFY_WITH_TIMEOUT(target->l2View() && target->l2View()->isOpen(), 5000);
        QPointer<VirtualFileView> view = target->l2View();
        QCOMPARE(target->viewModeL2(), ViewMode::Hex);
        QCOMPARE(view->readByteRange(0, original.size()), original);
        const QUuid documentId = target->documentState().id;
        const quint64 revision = target->contentRevision();

        auto *textAction = window.findChild<QAction *>(QStringLiteral("view.presentation.text"));
        auto *encodingAction = window.findChild<QAction *>(encodingActionName);
        QVERIFY(textAction && textAction->isEnabled());
        textAction->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(target->viewModeL2(), ViewMode::Text, 2000);
        QVERIFY(encodingAction && encodingAction->isEnabled());
        encodingAction->trigger();
        QCOMPARE(target->displayEncoding(), encoding);
        QCOMPARE(target->encodingOrigin(), EncodingOrigin::UserSelected);
        QVERIFY(textAction->isChecked() && encodingAction->isChecked());
        QCOMPARE(target->documentState().id, documentId);
        QCOMPARE(target->contentRevision(), revision);
        QCOMPARE(sourceHash(), originalHash);
        QCOMPARE(view->readByteRange(0, original.size()), original);
        QVERIFY(!target->isModified() && !view->isModified());
        QVERIFY(target->isBinaryDocument());
        QVERIFY(target->editor()->isReadOnly() && !target->canModifyCommand());
        const DocumentCapabilities capabilities = target->documentState().capabilities;
        QVERIFY(capabilities.testFlag(DocumentCapability::Read));
        QVERIFY(capabilities.testFlag(DocumentCapability::Search));
        QVERIFY(capabilities.testFlag(DocumentCapability::Copy));
        QVERIFY(!capabilities.testFlag(DocumentCapability::Edit));
        // B1 Binary/Text is a read-only interpretation, not B2 size policy.
        // Do not impose the B2 Save/isReadOnlyByPolicy contract on this B1 tab.
        QTRY_COMPARE_WITH_TIMEOUT(target->editor()->text(), expectedProjection, 3000);

        const auto sourceGuards = [&]() {
            return target && view && target->l2View() == view.data()
                && tabs->currentWidget() == target.data()
                && target->documentState().id == documentId
                && target->contentRevision() == revision
                && target->documentState().capabilities == capabilities
                && target->isBinaryDocument() && target->viewModeL2() == ViewMode::Text
                && target->displayEncoding() == encoding
                && target->contentSizeBytes() == original.size()
                && view->virtualFileSize() == original.size()
                && !target->isModified() && !view->isModified()
                && target->editor()->isReadOnly() && !target->canModifyCommand()
                && view->readByteRange(0, original.size()) == original
                && diskBytes() == original && sourceHash() == originalHash
                && !sawModal;
        };
        QVERIFY(sourceGuards());

        auto *findAction = window.findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = window.findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        auto *dialog = window.findChild<FindReplaceDialog *>();
        QVERIFY(dialog);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *hex = dialog->findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *caseSensitive = dialog->findChild<QCheckBox *>(QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog->findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
        auto *wrap = dialog->findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog->findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(pattern && scope && regex && hex && caseSensitive && wholeWord
                && wrap && next && previous && findAll && cancel && status && tree);
        QVERIFY(scope->findData(0) >= 0);
        scope->setCurrentIndex(scope->findData(0));
        regex->setChecked(false);
        hex->setChecked(false);
        wholeWord->setChecked(false);
        caseSensitive->setChecked(true);

        for (const QueryOracle &query : queries) {
            findAction->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
            pattern->setText(query.text);
            wrap->setChecked(false);
            // Initial caret placement only; every query/navigation below is
            // activated by its real Find dialog button, not a search API.
            target->resetSearchPosition(true);
            QVERIFY(sourceGuards());
            const auto selected = [&](int index) {
                const auto expected = query.byteRanges.at(index);
                return view->selectionStart() == expected.first
                    && view->selectionEnd() == expected.first + expected.second
                    && view->selectedBytes() == original.mid(expected.first, expected.second)
                    && target->editor()->selectedText() == query.text;
            };
            const int last = int(query.byteRanges.size()) - 1;
            for (int index = 0; index <= last; ++index) {
                QVERIFY(next->isEnabled());
                QTest::mouseClick(next, Qt::LeftButton);
                QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled(), 5000);
                QVERIFY(sourceGuards());
                QTRY_VERIFY_WITH_TIMEOUT(selected(index), 3000);
                QVERIFY(target->lastSearchError().isEmpty());
                QVERIFY(status->text().isEmpty());
            }
            QTest::mouseClick(next, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled(), 5000);
            QVERIFY(sourceGuards());
            QVERIFY(selected(last));
            QVERIFY(target->lastSearchError().isEmpty());
            QVERIFY(!status->text().isEmpty()); // terminal no-more-matches feedback

            for (int index = last - 1; index >= 0; --index) {
                QVERIFY(previous->isEnabled());
                QTest::mouseClick(previous, Qt::LeftButton);
                QTRY_VERIFY_WITH_TIMEOUT(previous->isEnabled(), 5000);
                QVERIFY(sourceGuards());
                QTRY_VERIFY_WITH_TIMEOUT(selected(index), 3000);
                QVERIFY(target->lastSearchError().isEmpty());
                QVERIFY(status->text().isEmpty());
            }
            QTest::mouseClick(previous, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(previous->isEnabled(), 5000);
            QVERIFY(sourceGuards());
            QVERIFY(selected(0));
            QVERIFY(target->lastSearchError().isEmpty());
            QVERIFY(!status->text().isEmpty());

            QTest::mouseClick(wrap, Qt::LeftButton);
            QVERIFY(wrap->isChecked());
            QTest::mouseClick(previous, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(previous->isEnabled(), 5000);
            QVERIFY(sourceGuards());
            QTRY_VERIFY_WITH_TIMEOUT(selected(last), 3000);
            QVERIFY(target->lastSearchError().isEmpty() && status->text().isEmpty());
            QTest::mouseClick(next, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(next->isEnabled(), 5000);
            QVERIFY(sourceGuards());
            QTRY_VERIFY_WITH_TIMEOUT(selected(0), 3000);
            QVERIFY(target->lastSearchError().isEmpty() && status->text().isEmpty());

            QVERIFY(findAll->isEnabled());
            QTest::mouseClick(findAll, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled() && !cancel->isVisible(), 5000);
            QVERIFY(sourceGuards());
            auto *model = qobject_cast<SearchResultsModel *>(tree->model());
            QVERIFY(model);
            QVERIFY(!model->isIncomplete());
            QCOMPARE(model->textEncoding(), encoding);
            QCOMPARE(model->totalMatches(), qint64(query.byteRanges.size()));
            QVector<QPair<qint64, int>> actualRanges;
            struct VisibleMatch { QModelIndex index; int matchInRow; int oracleIndex; };
            QVector<VisibleMatch> visibleMatches;
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex context = model->index(row, SearchResultsModel::COL_CONTEXT);
                const auto &group = model->groupedLineAt(row);
                // These <=91-byte fixtures fit one full source context. The
                // independently counted UTF-16 positions test the highlight
                // mapping as well as the source-coordinate result list.
                QCOMPARE(group.contextByteOffset, qint64(0));
                QCOMPARE(context.data(SearchResultsModel::ContextRole).toString(), expectedProjection);
                const QVariantList ranges = context.data(SearchResultsModel::MatchByteRangesRole).toList();
                const QVariantList positions = context.data(SearchResultsModel::MatchPositionsRole).toList();
                QCOMPARE(ranges.size(), positions.size());
                for (int matchInRow = 0; matchInRow < ranges.size(); ++matchInRow) {
                    const int oracleIndex = int(actualRanges.size());
                    QVERIFY(oracleIndex < query.byteRanges.size());
                    const QVariantMap range = ranges.at(matchInRow).toMap();
                    actualRanges.append({range.value(QStringLiteral("byteOffset")).toLongLong(),
                                         range.value(QStringLiteral("length")).toInt()});
                    const QVariantMap position = positions.at(matchInRow).toMap();
                    QCOMPARE(position.value(QStringLiteral("start")).toInt(), query.displayStarts.at(oracleIndex));
                    QCOMPARE(position.value(QStringLiteral("length")).toInt(), int(query.text.size()));
                    visibleMatches.append({context, matchInRow, oracleIndex});
                }
            }
            QCOMPARE(actualRanges, query.byteRanges);
            dialog->hide();
            dock->show();
            dock->raise();
            window.activateWindow();
            tree->header()->setSectionResizeMode(SearchResultsModel::COL_CONTEXT, QHeaderView::Stretch);
            QSignalSpy jumpSpy(dock, &SearchResultsDock::jumpToDocumentByteOffset);
            for (const VisibleMatch &match : visibleMatches) {
                tree->scrollTo(match.index);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                const QRect cell = tree->visualRect(match.index);
                const QRect clickable = cell.intersected(tree->viewport()->rect());
                QVERIFY(cell.isValid() && clickable.isValid());
                const QFontMetrics metrics(tree->font());
                QPoint click;
                bool foundClickableHighlight = false;
                // Use the actual delegate's visible/elided hit geometry, not
                // the historical model-only pixel approximation. This only
                // locates a click; the expected jump stays the manual oracle.
                for (int x = clickable.left(); x <= clickable.right(); ++x) {
                    if (hitTestSearchResultMatch(match.index, x - cell.left(),
                                                cell.width(), metrics) == match.matchInRow) {
                        click = QPoint(x, clickable.center().y());
                        foundClickableHighlight = true;
                        break;
                    }
                }
                QVERIFY2(foundClickableHighlight, "oracle match has no actually clickable highlight in the viewport");
                QVERIFY(tree->viewport()->rect().contains(click));
                QCOMPARE(tree->indexAt(click), match.index);
                const int previousJumps = jumpSpy.count();
                // An unrelated selection prevents the signal-only test from
                // accidentally passing when the queued MainWindow jump fails.
                target->gotoByteOffset(0, 2);
                QCOMPARE(view->selectionStart(), qint64(0));
                QCOMPARE(view->selectionEnd(), qint64(2));
                QCOMPARE(view->selectedBytes(), original.first(2));
                QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
                QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
                QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), previousJumps + 1, 3000);
                QVERIFY(sourceGuards());
                const auto expected = query.byteRanges.at(match.oracleIndex);
                const QList<QVariant> request = jumpSpy.last();
                QCOMPARE(request.at(0).toUuid(), documentId);
                QCOMPARE(request.at(1).toULongLong(), revision);
                QCOMPARE(request.at(2).toLongLong(), expected.first);
                QCOMPARE(request.at(3).toInt(), expected.second);
                QTRY_VERIFY_WITH_TIMEOUT(selected(match.oracleIndex), 3000);
                QVERIFY(sourceGuards());
            }
        }
        QVERIFY(sourceGuards());
        QCOMPARE(target->editor()->text(), expectedProjection);
        QCOMPARE(diskBytes(), original);
        QCOMPARE(sourceHash(), originalHash);
    }
};

QTEST_MAIN(SmokeSearchMalformedEncoding)
#include "smoke_search_malformed_encoding.moc"
