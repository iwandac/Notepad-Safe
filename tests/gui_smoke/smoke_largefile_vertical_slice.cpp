#include <QtTest/QTest>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <Qsci/qsciscintilla.h>

#include "LargeFileVerticalSlice.h"

class LargeFileVerticalSliceSmoke final : public QObject
{
    Q_OBJECT

private:
    static void dragScrollBar(QScrollBar *bar, qreal targetRatio)
    {
        QStyleOptionSlider option;
        option.initFrom(bar);
        option.rect = bar->rect();
        option.orientation = Qt::Vertical;
        option.minimum = bar->minimum();
        option.maximum = bar->maximum();
        option.sliderPosition = bar->sliderPosition();
        option.sliderValue = bar->value();
        option.singleStep = bar->singleStep();
        option.pageStep = bar->pageStep();
        option.upsideDown = false;
        const QRect handle = bar->style()->subControlRect(
            QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
        const QPoint start = handle.center();
        const QPoint destination(bar->width() / 2,
                                 qBound(2, int(bar->height() * targetRatio),
                                        bar->height() - 2));
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(bar, destination, 50);
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, destination);
    }

    static QString createS2Fixture(QTemporaryDir &temporary)
    {
        const QString path = temporary.filePath(QStringLiteral("s2-256mib.log"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        QByteArray block;
        block.reserve(1024 * 1024);
        for (int line = 0; block.size() < 1024 * 1024; ++line) {
            const QByteArray record = QByteArray::number(line).rightJustified(8, '0')
                + " 2026-08-17T12:00:00Z INFO service=vertical-slice marker=S2_WINDOW payload=abcdefghijklmnopqrstuvwxyz\n";
            block += record;
        }
        block.truncate(1024 * 1024 - 1);
        block += '\n';
        for (int mib = 0; mib < 256; ++mib) {
            if (file.write(block) != block.size())
                return {};
        }
        file.close();
        return path;
    }

private slots:
    void s2PreviewPromotesAndPreservesPosition()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = createS2Fixture(temporary);
        QVERIFY(!path.isEmpty());
        QCOMPARE(QFileInfo(path).size(), 256LL * 1024 * 1024);

        LargeFileVerticalSlice slice;
        slice.setWindowTitle(QStringLiteral("S2 single-scroll + ILoader vertical slice"));
        slice.resize(1280, 820);
        slice.show();
        QVERIFY(QTest::qWaitForWindowExposed(&slice));

        QElapsedTimer openTimer;
        openTimer.start();
        QString error;
        QVERIFY2(slice.openS2Text(path, &error, false), qPrintable(error));
        const qint64 openMs = openTimer.elapsed();
        qInfo().noquote() << "S2_OPEN_MS=" << openMs;
        QVERIFY2(openTimer.elapsed() < 1000, "mmap preview must not wait for full load");
        QCOMPARE(slice.state(), LargeFileVerticalSlice::State::PreviewReady);
        QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);

        QScrollBar *bar = slice.preview()->globalScrollBar();
        dragScrollBar(bar, 0.70);
        QTRY_VERIFY_WITH_TIMEOUT(slice.preview()->globalOffset() > QFileInfo(path).size() * 60 / 100,
                                 3000);
        const qint64 draggedOffset = slice.preview()->globalOffset();
        QTest::qWait(1000);
        QCOMPARE(slice.preview()->globalOffset(), draggedOffset);

        const qint64 target = QFileInfo(path).size() * 7 / 10;
        slice.preview()->goToOffset(target, 16);
        QCOMPARE(slice.preview()->globalOffset(), target);
        QVERIFY(slice.preview()->windowStart() <= target);
        QVERIFY(slice.preview()->windowEnd() > target);
        QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);

        bar->setFocus();
        QTest::keyClick(bar, Qt::Key_End);
        QTRY_VERIFY_WITH_TIMEOUT(slice.preview()->globalOffset() > QFileInfo(path).size() * 99 / 100,
                                 3000);
        const qint64 endOffset = slice.preview()->globalOffset();
        QTest::qWait(1000);
        QCOMPARE(slice.preview()->globalOffset(), endOffset);
        QTest::keyClick(bar, Qt::Key_Home);
        QTRY_COMPARE_WITH_TIMEOUT(slice.preview()->globalOffset(), 0LL, 3000);

        slice.preview()->goToOffset(target, 16);
        const QString previewShot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s2-slice-preview.png"));
        QVERIFY(slice.grab().save(previewShot));

        QVector<LargeFileVerticalSlice::State> phases;
        connect(&slice, &LargeFileVerticalSlice::stateChanged, &slice,
                [&phases](LargeFileVerticalSlice::State state) { phases.push_back(state); });
        QElapsedTimer promotionTimer;
        promotionTimer.start();
        slice.startPromotion();
        QTRY_VERIFY_WITH_TIMEOUT(slice.state() == LargeFileVerticalSlice::State::Reading
                                     || slice.state() == LargeFileVerticalSlice::State::BuildingDocument
                                     || slice.state() == LargeFileVerticalSlice::State::Attaching
                                     || slice.state() == LargeFileVerticalSlice::State::Editable,
                                 5000);
        if (slice.state() != LargeFileVerticalSlice::State::Editable) {
            const QString loadingShot = QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("gui-s2-slice-loading.png"));
            QVERIFY(slice.grab().save(loadingShot));
        }
        QTRY_COMPARE_WITH_TIMEOUT(slice.state(), LargeFileVerticalSlice::State::Editable, 120000);
        qInfo().noquote() << "S2_PROMOTION_MS=" << promotionTimer.elapsed();
        QVERIFY(phases.contains(LargeFileVerticalSlice::State::Reading));
        QVERIFY(phases.contains(LargeFileVerticalSlice::State::BuildingDocument));
        QVERIFY(phases.contains(LargeFileVerticalSlice::State::Attaching));
        QVERIFY(phases.contains(LargeFileVerticalSlice::State::Editable));
        QCOMPARE(slice.fullDocumentLength(), QFileInfo(path).size());
        QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);
        QVERIFY(qAbs(slice.fullEditorPosition() - target) <= 1);

        slice.fullEditor()->setFocus();
        QTest::keyClicks(slice.fullEditor(), QStringLiteral("EDIT_"));
        QCOMPARE(slice.fullDocumentLength(), QFileInfo(path).size() + 5);
        const QString editableShot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s2-slice-editable.png"));
        QVERIFY(slice.grab().save(editableShot));

        LargeFileVerticalSlice fallback;
        fallback.resize(900, 600);
        fallback.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fallback));
        QVERIFY2(fallback.openS2Text(path, &error, false), qPrintable(error));
        fallback.preview()->goToOffset(target);
        fallback.startPromotion();
        fallback.cancelPromotion();
        QTRY_COMPARE_WITH_TIMEOUT(fallback.state(),
                                  LargeFileVerticalSlice::State::ReadOnlyFallback, 10000);
        QCOMPARE(fallback.visibleVerticalScrollBarCount(), 1);
        QVERIFY(fallback.preview()->editor()->isReadOnly());
        QCOMPARE(fallback.preview()->globalOffset(), target);
        const QString fallbackShot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s2-slice-fallback.png"));
        QVERIFY(fallback.grab().save(fallbackShot));
    }

    void s3RealFileBrowsesAndSearchesWithoutRebound()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../bigtestfile"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QVERIFY(QFileInfo(path).size() > 2LL * 1024 * 1024 * 1024);

        LargeFileVerticalSlice slice;
        slice.setWindowTitle(QStringLiteral("S3 4.59 GiB single-scroll readonly vertical slice"));
        slice.resize(1280, 820);
        slice.show();
        QVERIFY(QTest::qWaitForWindowExposed(&slice));

        QElapsedTimer openTimer;
        openTimer.start();
        QString error;
        QVERIFY2(slice.openS3Hex(path, &error), qPrintable(error));
        const qint64 openMs = openTimer.elapsed();
        qInfo().noquote() << "S3_OPEN_MS=" << openMs;
        QVERIFY2(openTimer.elapsed() < 1000, "S3 mmap preview must be immediate");
        QCOMPARE(slice.state(), LargeFileVerticalSlice::State::PreviewReady);
        QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);
        QVERIFY(slice.preview()->editor()->isReadOnly());

        const qint64 size = QFileInfo(path).size();
        for (int quarter = 0; quarter <= 4; ++quarter) {
            const qint64 offset = quarter == 4 ? size - 1 : size * quarter / 4;
            const QByteArray expected = slice.preview()->sourceBytes(offset, 1);
            QCOMPARE(expected.size(), 1);
            slice.preview()->goToOffset(offset, 1);
            QCOMPARE(slice.preview()->globalOffset(), offset);
            QVERIFY(slice.preview()->windowStart() <= offset);
            QVERIFY(slice.preview()->windowEnd() > offset);
            QCOMPARE(slice.preview()->sourceBytes(offset, 1), expected);
            QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);
        }

        QScrollBar *bar = slice.preview()->globalScrollBar();
        slice.preview()->goToOffset(0);
        dragScrollBar(bar, 0.74);
        QTRY_VERIFY_WITH_TIMEOUT(slice.preview()->globalOffset() > size * 65 / 100, 3000);
        const qint64 draggedOffset = slice.preview()->globalOffset();
        QTest::qWait(1000);
        QCOMPARE(slice.preview()->globalOffset(), draggedOffset);
        slice.preview()->goToOffset(0);
        bar->setFocus();
        QTest::keyClick(bar, Qt::Key_End);
        QTRY_VERIFY_WITH_TIMEOUT(slice.preview()->globalOffset() > size * 99 / 100, 3000);
        const qint64 endOffset = slice.preview()->globalOffset();
        QTest::qWait(1000);
        QCOMPARE(slice.preview()->globalOffset(), endOffset);

        const qint64 target = size * 3 / 4;
        const QByteArray signature = slice.preview()->sourceBytes(target, 32);
        QCOMPARE(signature.size(), 32);
        QElapsedTimer searchCall;
        searchCall.start();
        slice.preview()->findBytesAsync(signature, 0);
        const qint64 dispatchMs = searchCall.elapsed();
        QVERIFY2(dispatchMs < 100, "whole-file search must not block the GUI call");
        QElapsedTimer searchTimer;
        searchTimer.start();
        QTRY_VERIFY_WITH_TIMEOUT(!slice.preview()->searchRunning(), 120000);
        qInfo().noquote() << "S3_SEARCH_TO_75_PERCENT_MS=" << searchTimer.elapsed()
                          << "DISPATCH_MS=" << dispatchMs;
        QCOMPARE(slice.preview()->lastSearchResult(), target);
        QCOMPARE(slice.preview()->globalOffset(), target);
        QCOMPARE(slice.preview()->sourceBytes(slice.preview()->lastSearchResult(), 32), signature);
        QCOMPARE(slice.visibleVerticalScrollBarCount(), 1);

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s3-slice-real.png"));
        QVERIFY(slice.grab().save(screenshot));
    }
};

QTEST_MAIN(LargeFileVerticalSliceSmoke)
#include "smoke_largefile_vertical_slice.moc"
