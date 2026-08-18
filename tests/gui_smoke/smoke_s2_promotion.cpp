#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include "EditorTab.h"
#include "LargeFileVerticalSlice.h"
#include "VirtualFileView.h"
#include "DraftStore.h"
#include "SessionManager.h"
#include "AsyncTaskManager.h"

class S2PromotionSmokeTest final : public QObject {
    Q_OBJECT

    static QString marginText(QsciScintilla *editor, int line)
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

    static void dragScrollBar(QScrollBar *bar, qreal targetRatio)
    {
        QStyleOptionSlider option;
        option.initFrom(bar);
        option.rect = bar->rect();
        option.orientation = bar->orientation();
        option.minimum = bar->minimum();
        option.maximum = bar->maximum();
        option.sliderPosition = bar->sliderPosition();
        option.sliderValue = bar->value();
        option.singleStep = bar->singleStep();
        option.pageStep = bar->pageStep();
        const QRect handle = bar->style()->subControlRect(
            QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, bar);
        const QPoint destination = bar->orientation() == Qt::Vertical
            ? QPoint(bar->width() / 2,
                     qBound(2, int(bar->height() * targetRatio), bar->height() - 2))
            : QPoint(qBound(2, int(bar->width() * targetRatio), bar->width() - 2),
                     bar->height() / 2);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, handle.center());
        QTest::mouseMove(bar, destination, 50);
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, destination);
    }

    static void wheelLinesDown(QWidget *target, int notches = 1)
    {
        const QPointF local(target->rect().center());
        QWheelEvent wheel(local, target->mapToGlobal(local.toPoint()), QPoint(),
                          QPoint(0, -120 * notches), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &wheel);
        QCoreApplication::processEvents();
    }

    static void wheelPixelsDown(QWidget *target, int pixels)
    {
        const QPointF local(target->rect().center());
        QWheelEvent wheel(local, target->mapToGlobal(local.toPoint()),
                          QPoint(0, -pixels), QPoint(), Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(target, &wheel);
        QCoreApplication::processEvents();
    }

    static void verifyVisibleTextMatchesSource(WindowedReadOnlyProvider *provider)
    {
        const int firstLine = static_cast<int>(provider->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const QByteArray rendered = provider->editor()->text(firstLine).toUtf8();
        const qint64 topOffset = provider->visibleTopOffset();
        const qsizetype compared = std::min<qsizetype>(64, rendered.size());
        QVERIFY(compared > 0);
        QCOMPARE(rendered.left(compared), provider->sourceBytes(topOffset, compared));
    }

private slots:
    void editorSurfaceBaselineMatchesFullAndWindowedProviders()
    {
        EditorTab full;
        WindowedReadOnlyProvider windowed;
        QsciScintilla *s1 = full.editor();
        QsciScintilla *s2 = windowed.editor();
        QVERIFY(s1 && s2);

        QCOMPARE(s2->font().family(), s1->font().family());
        QCOMPARE(s2->font().pointSizeF(), s1->font().pointSizeF());
        QCOMPARE(s2->font().fixedPitch(), s1->font().fixedPitch());
        QCOMPARE(s2->marginWidth(1), s1->marginWidth(1));

        const auto sci = [](QsciScintilla *editor, unsigned int message) {
            return editor->SendScintilla(message);
        };
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETCARETWIDTH),
                 sci(s1, QsciScintillaBase::SCI_GETCARETWIDTH));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETCARETLINEVISIBLE),
                 sci(s1, QsciScintillaBase::SCI_GETCARETLINEVISIBLE));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETCARETLINEBACK),
                 sci(s1, QsciScintillaBase::SCI_GETCARETLINEBACK));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETTABWIDTH),
                 sci(s1, QsciScintillaBase::SCI_GETTABWIDTH));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETUSETABS),
                 sci(s1, QsciScintillaBase::SCI_GETUSETABS));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETINDENTATIONGUIDES),
                 sci(s1, QsciScintillaBase::SCI_GETINDENTATIONGUIDES));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETSCROLLWIDTHTRACKING),
                 sci(s1, QsciScintillaBase::SCI_GETSCROLLWIDTHTRACKING));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETWRAPVISUALFLAGS),
                 sci(s1, QsciScintillaBase::SCI_GETWRAPVISUALFLAGS));
        QCOMPARE(sci(s2, QsciScintillaBase::SCI_GETWRAPINDENTMODE),
                 sci(s1, QsciScintillaBase::SCI_GETWRAPINDENTMODE));
    }

    void windowedHexUsesUnifiedThreeColumnByteContract()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("unified-hex.bin"));
        QByteArray sourceBytes(4096, '\0');
        for (int i = 0; i < sourceBytes.size(); ++i)
            sourceBytes[i] = char(i & 0xff);
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
        }

        QWidget window;
        window.resize(1200, 760);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *provider = new WindowedReadOnlyProvider(&window);
        layout->addWidget(provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(provider->openFile(path,
                                    WindowedReadOnlyProvider::Presentation::Hex,
                                    &error), qPrintable(error));
        auto *hex = provider->editor();
        auto *ascii = provider->asciiPreview();
        QVERIFY(hex && ascii && ascii->isVisibleTo(provider));
        QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
        QVERIFY(hex->marginWidth(0) > 0);
        QCOMPARE(marginText(hex, 0), QStringLiteral("00000000"));
        QVERIFY(hex->text(0).startsWith(QStringLiteral("00 01 02 03")));
        QVERIFY(!hex->text(0).contains(QLatin1Char('|')));
        QVERIFY(!hex->text(0).startsWith(QStringLiteral("00000000")));
        QCOMPARE(ascii->toPlainText().left(4), QStringLiteral("...."));
        QVERIFY(hex->geometry().right() <= ascii->geometry().left());
        QVERIFY(!hex->verticalScrollBar()->isVisibleTo(provider));
        QVERIFY(!ascii->verticalScrollBar()->isVisibleTo(provider));

        // Hex selection [5, 7) mirrors the same two source bytes in ASCII.
        hex->setFocus(Qt::OtherFocusReason);
        hex->setSelection(0, 5 * 3, 0, 7 * 3);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), qint64(5), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(), qint64(7), 1000);
        QCOMPARE(ascii->textCursor().selectionStart(), 5);
        QCOMPARE(ascii->textCursor().selectionEnd(), 7);

        // ASCII reverse selection [1, 3) mirrors to full Hex byte boundaries.
        QTextCursor asciiSelection(ascii->document());
        asciiSelection.setPosition(3);
        asciiSelection.setPosition(1, QTextCursor::KeepAnchor);
        ascii->setFocus(Qt::OtherFocusReason);
        ascii->setTextCursor(asciiSelection);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), qint64(1), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(), qint64(3), 1000);
        QCOMPARE(hex->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART),
                 hex->positionFromLineIndex(0, 3));
        QCOMPARE(hex->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND),
                 hex->positionFromLineIndex(0, 9));

        // Copy format follows the focused surface, matching editable B1.
        provider->goToOffset(65, 1); // ASCII 'A'
        ascii->setFocus(Qt::OtherFocusReason);
        QVERIFY(provider->copySelection(&error));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("A"));
        hex->setFocus(Qt::OtherFocusReason);
        QVERIFY(provider->copySelection(&error));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("41"));

        // Byte-aware horizontal movement never stops on the separator.
        provider->goToOffset(5);
        hex->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(hex, Qt::Key_Right);
        int line = -1, column = -1;
        hex->getCursorPosition(&line, &column);
        QCOMPARE(column, 5 * 3 + 1);
        QCOMPARE(provider->selectionStartOffset(), qint64(5));
        QCOMPARE(provider->selectionEndOffset(), qint64(5));
        QTest::keyClick(hex, Qt::Key_Right);
        hex->getCursorPosition(&line, &column);
        QCOMPARE(column, 6 * 3);
        QTest::keyClick(hex, Qt::Key_Right, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), qint64(6), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(), qint64(7), 1000);

        QTest::keyClick(hex, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(provider->selectionStartOffset(), qint64(0));
        QCOMPARE(provider->selectionEndOffset(), qint64(sourceBytes.size()));

        const int wideBytesPerRow = provider->hexBytesPerRow();
        window.resize(700, 760);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() < wideBytesPerRow, 2000);
        QVERIFY(provider->hexBytesPerRow() >= 8);
        QCOMPARE(provider->sourceBytes(0, sourceBytes.size()), sourceBytes);
        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-windowed-hex-unified.png"));
        QVERIFY(window.grab().save(screenshot));
        window.close();
    }

    void mmapPreviewPromotesToEditableScintilla()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString sourcePath = temporary.filePath(QStringLiteral("forty-mib.log"));
        QSaveFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray line = QByteArrayLiteral(
            "2026-08-15T12:34:56Z INFO worker=17 marker=LOAD_PROMOTION_OK payload=abcdef\n");
        QByteArray block;
        while (block.size() + line.size() <= 1024 * 1024)
            block += line;
        block += QByteArray(1024 * 1024 - block.size(), 'x');
        for (int i = 0; i < 40; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());
        const qint64 sourceBytes = QFileInfo(sourcePath).size();
        QCOMPARE(sourceBytes, 40LL * 1024 * 1024);

        QWidget window;
        window.setWindowTitle(QStringLiteral("Notepad Safe S2 Promotion Acceptance"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QElapsedTimer opening;
        opening.start();
        QString error;
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error, false),
                 qPrintable(error));
        QVERIFY2(opening.elapsed() < 1000,
                 "S2 open must return with mmap preview instead of waiting for Scintilla");
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        auto *preview = tab->windowedReadOnlyProvider();
        QVERIFY(preview);
        QCOMPARE(preview->visibleVerticalScrollBarCount(), 1);
        dragScrollBar(preview->globalScrollBar(), 0.68);
        QTRY_VERIFY_WITH_TIMEOUT(preview->globalOffset() > sourceBytes * 55 / 100, 3000);
        const qint64 dragged = preview->globalOffset();
        const QByteArray renderedPrefix = preview->editor()->text().toUtf8().left(128);
        QCOMPARE(renderedPrefix, preview->sourceBytes(preview->windowStart(), 128));
        QTest::qWait(1000);
        QCOMPARE(preview->globalOffset(), dragged);

        // A physical wheel notch must move by visible lines, not rebuild the
        // projection tens of KiB away. Validate the bytes actually painted at
        // the new top of the viewport rather than only checking an offset field.
        const qint64 beforeWheel = preview->visibleTopOffset();
        wheelLinesDown(preview->editor()->viewport());
        QTRY_VERIFY_WITH_TIMEOUT(preview->visibleTopOffset() > beforeWheel, 3000);
        QVERIFY2(preview->visibleTopOffset() - beforeWheel < 64 * 1024,
                 "one wheel notch skipped a discontinuous large-file region");
        QCOMPARE(preview->globalOffset(), preview->visibleTopOffset());
        verifyVisibleTextMatchesSource(preview);

        const qint64 beforePixelWheel = preview->visibleTopOffset();
        wheelPixelsDown(preview->editor()->viewport(),
                        preview->editor()->fontMetrics().lineSpacing() * 2);
        QTRY_VERIFY_WITH_TIMEOUT(preview->visibleTopOffset() > beforePixelWheel, 3000);
        QVERIFY(preview->visibleTopOffset() - beforePixelWheel < 64 * 1024);
        verifyVisibleTextMatchesSource(preview);

        // Force the local Scintilla projection to its bottom, then cross the
        // mmap window boundary with a real wheel event. The overlap reload must
        // preserve the old visible row and advance from it without jumping back.
        const int lineCount = static_cast<int>(preview->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETLINECOUNT));
        const int linesOnScreen = static_cast<int>(preview->editor()->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN));
        preview->editor()->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                         std::max(0, lineCount - linesOnScreen));
        const qint64 boundaryTop = preview->visibleTopOffset();
        const qint64 oldWindowStart = preview->windowStart();
        wheelLinesDown(preview->editor()->viewport());
        QTRY_VERIFY_WITH_TIMEOUT(preview->windowStart() > oldWindowStart, 3000);
        QVERIFY(preview->visibleTopOffset() > boundaryTop);
        QVERIFY(preview->visibleTopOffset() - boundaryTop < 64 * 1024);
        verifyVisibleTextMatchesSource(preview);

        // Shift+PageDown at a projection boundary creates one source-byte
        // selection spanning both mmap windows. Copy must come from the file
        // range, not merely from whichever projection remains in Scintilla.
        const int nextLineCount = static_cast<int>(preview->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETLINECOUNT));
        const int nextLinesOnScreen = static_cast<int>(preview->editor()->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN));
        const int nextLastFirst = std::max(0, nextLineCount - nextLinesOnScreen);
        preview->editor()->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                         nextLastFirst);
        const qint64 selectionAnchor = preview->visibleTopOffset();
        const qint64 projectedAnchor = preview->editor()->SendScintilla(
            QsciScintillaBase::SCI_POSITIONFROMLINE, nextLastFirst);
        preview->editor()->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                         static_cast<long>(projectedAnchor));
        const qint64 selectionWindowStart = preview->windowStart();
        QTest::keyClick(preview->editor(), Qt::Key_PageDown, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(preview->windowStart() > selectionWindowStart, 3000);
        QCOMPARE(preview->selectionStartOffset(), selectionAnchor);
        QVERIFY(preview->selectionEndOffset() > selectionAnchor);
        const qint64 selectedBytes = preview->selectionEndOffset()
            - preview->selectionStartOffset();
        QVERIFY(selectedBytes < 64 * 1024);
        tab->copy();
        QCOMPARE(QApplication::clipboard()->text().toUtf8(),
                 preview->sourceBytes(preview->selectionStartOffset(),
                                      static_cast<qsizetype>(selectedBytes)));
        const QString previewScreenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-production-s2-preview-dynamic.png"));
        QVERIFY(window.grab().save(previewScreenshot));

        // Reopen the same S2 file with promotion enabled. The first open above
        // isolates actual preview dynamics from a fast ILoader completion.
        tab->shutdownL2BeforeClose();
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error, true), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        tab->gotoByteOffset(dragged);

        QTRY_VERIFY_WITH_TIMEOUT(!tab->isL2Mode(), 30000);
        QVERIFY(tab->isLargeDocument());
        QVERIFY(!tab->isReadOnlyByPolicy());
        QCOMPARE(tab->editor()->length(), sourceBytes);
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETREADONLY), 0L);
        const qint64 promotedPosition = tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        QVERIFY2(qAbs(promotedPosition - dragged) <= 1,
                 qPrintable(QStringLiteral("promotion changed position: expected %1, actual %2")
                                .arg(dragged).arg(promotedPosition)));

        tab->editor()->setCursorPosition(0, 0);
        tab->editor()->setFocus();
        QTest::keyClicks(tab->editor(), QStringLiteral("EDIT_"));
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text(0).startsWith(QStringLiteral("EDIT_")),
                                 3000);
        QVERIFY(tab->isModified());

        const QString savedPath = temporary.filePath(QStringLiteral("saved.log"));
        QVERIFY2(tab->saveAsFile(savedPath, &error), qPrintable(error));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.read(5), QByteArrayLiteral("EDIT_"));
        QCOMPARE(saved.size(), sourceBytes + 5);
        QVERIFY(!tab->isModified());

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("gui-s2-promotion.png"));
        QVERIFY(window.grab().save(screenshot));
        window.close();
    }

    void s2LongLineHasUsableHorizontalScroll()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("long-lines.log"));
        QSaveFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QByteArray longLine = QByteArrayLiteral("LONG_LINE_BEGIN_");
        longLine += QByteArray(512 * 1024, 'A');
        longLine += QByteArrayLiteral("_LONG_LINE_END\n");
        QCOMPARE(source.write(longLine), qint64(longLine.size()));
        const QByteArray block(1024 * 1024, 'x');
        for (int i = 0; i < 33; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());

        QWidget window;
        window.setWindowTitle(QStringLiteral("Notepad Safe long-line acceptance"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, false), qPrintable(error));
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        auto *horizontal = provider->editor()->horizontalScrollBar();
        QTRY_VERIFY_WITH_TIMEOUT(horizontal->maximum() > 0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(horizontal->isVisible(), 3000);
        dragScrollBar(horizontal, 0.9);
        QTRY_VERIFY_WITH_TIMEOUT(horizontal->value() > horizontal->maximum() / 2, 3000);
        QVERIFY(provider->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET) > 0);

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s2-long-line-horizontal.png"));
        QVERIFY(window.grab().save(screenshot));
        window.close();
    }

    void windowedTextWrapReachesPhysicalEofAndKeepsGutter()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("wrapped-eof.log"));
        QSaveFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QByteArray line(4096, 'w');
        line += '\n';
        qint64 written = 0;
        while (written + line.size() < 6LL * 1024 * 1024) {
            QCOMPARE(source.write(line), qint64(line.size()));
            written += line.size();
        }
        const QByteArray tail("FINAL_PHYSICAL_EOF_MARKER");
        QCOMPARE(source.write(tail), qint64(tail.size()));
        QVERIFY(source.commit());

        QWidget window;
        window.resize(900, 620);
        auto *layout = new QVBoxLayout(&window);
        auto *provider = new WindowedReadOnlyProvider(&window);
        layout->addWidget(provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(provider->openFile(path,
                                    WindowedReadOnlyProvider::Presentation::Text,
                                    &error), qPrintable(error));
        provider->setWordWrap(true);
        QVERIFY(provider->wordWrap());
        QVERIFY2(provider->editor()->marginWidth(0) > 0,
                 "windowed text preview lost its navigation gutter");

        QElapsedTimer dragTimer;
        dragTimer.start();
        dragScrollBar(provider->globalScrollBar(), 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(provider->globalScrollBar()->value(),
                                  provider->globalScrollBar()->maximum(), 3000);
        QVERIFY2(dragTimer.elapsed() < 3000,
                 "word-wrapped global drag stalled the GUI");
        QCOMPARE(provider->windowEnd(), provider->fileSize());
        QVERIFY2(provider->fileSize() - provider->visibleBottomOffset() < 8192,
                 "scrollbar maximum did not put the physical EOF on screen");
        QVERIFY(provider->editor()->text().contains(QString::fromLatin1(tail)));

        const qint64 topAtEnd = provider->visibleTopOffset();
        provider->editor()->setFocus();
        for (int i = 0; i < 64; ++i)
            QTest::keyClick(provider->editor(), Qt::Key_Down);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(provider->visibleTopOffset(), topAtEnd);
        wheelLinesDown(provider->editor()->viewport(), 8);
        QCOMPARE(provider->visibleTopOffset(), topAtEnd);

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-windowed-wrap-physical-eof.png"));
        QVERIFY(window.grab().save(screenshot));
        window.close();
    }

    void windowedProviderRemapsAppendAndTruncate()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("changing.log"));
        QSaveFile initial(path);
        QVERIFY(initial.open(QIODevice::WriteOnly));
        QByteArray line = QByteArrayLiteral("00000000 ORIGINAL log line payload=abcdefghijklmnopqrstuvwxyz\n");
        QByteArray initialBytes;
        while (initialBytes.size() + line.size() <= 1024 * 1024)
            initialBytes += line;
        QCOMPARE(initial.write(initialBytes), qint64(initialBytes.size()));
        QVERIFY(initial.commit());

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *provider = new WindowedReadOnlyProvider(&window);
        layout->addWidget(provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(provider->openFile(path, WindowedReadOnlyProvider::Presentation::Text,
                                    &error), qPrintable(error));
        QSignalSpy changed(provider, &WindowedReadOnlyProvider::sourceFileChanged);
        const qint64 originalSize = provider->fileSize();

        const QByteArray appended = QByteArrayLiteral(
            "APPENDED_AFTER_MMAP marker=FILE_GROWTH_VISIBLE\n");
        QFile appendFile(path);
        QVERIFY(appendFile.open(QIODevice::Append));
        QCOMPARE(appendFile.write(appended), qint64(appended.size()));
        appendFile.close();
        QTRY_COMPARE_WITH_TIMEOUT(provider->fileSize(), originalSize + appended.size(), 5000);
        QVERIFY(changed.count() >= 1);
        provider->goToOffset(originalSize, appended.size());
        QVERIFY(provider->editor()->text().contains(
            QStringLiteral("FILE_GROWTH_VISIBLE")));

        // Position near the former EOF, then atomically replace the path with
        // a much smaller file. Reload must clamp all 64-bit positions and must
        // never dereference the old mapping after it has been unmapped.
        provider->goToOffset(provider->fileSize() - 1);
        QByteArray replacement = QByteArrayLiteral(
            "TRUNCATED_HEAD marker=FILE_SHRINK_VISIBLE\n");
        replacement += QByteArray(256 * 1024, 't');
        replacement += QByteArrayLiteral("\nTRUNCATED_TAIL\n");
        QSaveFile truncated(path);
        QVERIFY(truncated.open(QIODevice::WriteOnly));
        QCOMPARE(truncated.write(replacement), qint64(replacement.size()));
        QVERIFY2(truncated.commit(), qPrintable(truncated.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(provider->fileSize(), qint64(replacement.size()), 5000);
        QVERIFY(changed.count() >= 2);
        QVERIFY(provider->globalOffset() < provider->fileSize());
        QVERIFY(provider->windowEnd() <= provider->fileSize());
        provider->goToOffset(0);
        QVERIFY(provider->editor()->text().startsWith(
            QStringLiteral("TRUNCATED_HEAD marker=FILE_SHRINK_VISIBLE")));
        provider->goToOffset(provider->fileSize() - 16);
        QVERIFY(provider->editor()->text().contains(QStringLiteral("TRUNCATED_TAIL")));

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-windowed-remap-truncated.png"));
        QVERIFY(window.grab().save(screenshot));
        window.close();
    }

    void productionWindowedPathDoesNotHoldLegacyMmap()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("production-changing.log"));
        QSaveFile initial(path);
        QVERIFY(initial.open(QIODevice::WriteOnly));
        const QByteArray block(1024 * 1024, 'p');
        for (int index = 0; index < 33; ++index)
            QCOMPARE(initial.write(block), qint64(block.size()));
        QVERIFY(initial.commit());

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, false), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->windowedReadOnlyProvider());
        QVERIFY(tab->l2View());
        QVERIFY2(!tab->l2View()->isOpen(),
                 "the hidden legacy VirtualFileView must not mmap a windowed document");
        QCOMPARE(tab->l2View()->mmapSize(), qint64(0));

        QByteArray replacement = QByteArrayLiteral(
            "PRODUCTION_REPLACED marker=NO_HIDDEN_MMAP\n");
        replacement += QByteArray(1024 * 1024, 'r');
        QSaveFile changed(path);
        QVERIFY(changed.open(QIODevice::WriteOnly));
        QCOMPARE(changed.write(replacement), qint64(replacement.size()));
        QVERIFY2(changed.commit(), qPrintable(changed.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(tab->windowedReadOnlyProvider()->fileSize(),
                                  qint64(replacement.size()), 5000);
        tab->gotoByteOffset(0);
        QVERIFY(tab->windowedReadOnlyProvider()->editor()->text().startsWith(
            QStringLiteral("PRODUCTION_REPLACED marker=NO_HIDDEN_MMAP")));
        window.close();
    }

    void s2ExpandedEncodingFallsBackBeforeAllocation()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("expanded-utf16.log"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArray::fromHex("fffe41000a00")), qint64(6));
        // 1.5 GiB source remains in the nominal S2 range, but its conservative
        // UTF-16 -> UTF-8 estimate exceeds 2 GiB. resize creates the test's
        // unmaterialized extent without writing 1.5 GiB through the runner.
        constexpr qint64 sourceSize = 1536LL * 1024 * 1024;
        QVERIFY(source.resize(sourceSize));
        source.close();

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy stateChanged(tab, &EditorTab::largeDocumentRuntimeStateChanged);
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, true), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->largeDocumentRuntimeState(),
            EditorTab::LargeDocumentRuntimeState::ReadOnlyFallback, 5000);
        QVERIFY(!tab->isLargeFilePreparationRunning());
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        QVERIFY(tab->windowedReadOnlyProvider());
        QVERIFY(!tab->l2View()->isOpen());
        QVERIFY(tab->largeDocumentFallbackReason().contains(QStringLiteral("2 GiB")));
        QVERIFY(stateChanged.count() >= 2);
        window.close();
    }

    void cancelingQueuedOrRunningPromotionKeepsPreviewUsable()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("cancel-promotion.log"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArrayLiteral("CANCEL_TEST_HEAD\n")), qint64(17));
        QVERIFY(source.resize(128LL * 1024 * 1024));
        source.close();

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy added(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded);
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, true), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!added.isEmpty(), 5000);

        qint64 promotionTask = 0;
        for (const QList<QVariant> &arguments : added) {
            const qint64 id = arguments.at(0).toLongLong();
            if (AsyncTaskManager::instance().taskById(id).type
                == AsyncTaskType::LargeFileLoad) {
                promotionTask = id;
                break;
            }
        }
        QVERIFY(promotionTask > 0);
        AsyncTaskManager::instance().cancel(promotionTask);
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->largeDocumentRuntimeState(),
            EditorTab::LargeDocumentRuntimeState::ReadOnlyFallback, 5000);
        QVERIFY(!tab->isLargeFilePreparationRunning());
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        QVERIFY(tab->largeDocumentFallbackReason().contains(QStringLiteral("取消")));
        tab->gotoByteOffset(0);
        QVERIFY(tab->windowedReadOnlyProvider()->editor()->text().startsWith(
            QStringLiteral("CANCEL_TEST_HEAD")));
        window.close();
    }

    void invalidEncodingDuringPromotionFallsBackWithoutLosingPreview()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("invalid-late-utf8.log"));
        QSaveFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QByteArray block(1024 * 1024, 'a');
        block.replace(0, 20, QByteArrayLiteral("VALID_PREVIEW_HEAD\n"));
        for (int index = 0; index < 20; ++index)
            QCOMPARE(source.write(block), qint64(block.size()));
        block.fill('b');
        block[12345] = char(0xff);
        QCOMPARE(source.write(block), qint64(block.size()));
        block.fill('c');
        for (int index = 21; index < 40; ++index)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, true), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->largeDocumentRuntimeState(),
            EditorTab::LargeDocumentRuntimeState::ReadOnlyFallback, 10000);
        QVERIFY(!tab->isLargeFilePreparationRunning());
        QVERIFY(tab->largeDocumentFallbackReason().contains(QStringLiteral("UTF-8")));
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        tab->gotoByteOffset(0);
        QVERIFY(tab->windowedReadOnlyProvider()->editor()->text().startsWith(
            QStringLiteral("VALID_PREVIEW_HEAD")));
        window.close();
    }

    void nearTwoGiBPreviewReachesHeadMiddleAndTailWithoutPromotion()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("near-two-gib.log"));
        constexpr qint64 size = 2LL * 1024 * 1024 * 1024 - 4096;
        constexpr qint64 middle = size / 2;
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadWrite));
        QVERIFY(source.resize(size));
        QVERIFY(source.seek(0));
        QCOMPARE(source.write(QByteArrayLiteral("S2_HEAD_MARKER\n")), qint64(15));
        QVERIFY(source.seek(middle));
        QCOMPARE(source.write(QByteArrayLiteral("\nS2_MIDDLE_MARKER\n")), qint64(18));
        QVERIFY(source.seek(size - 16));
        QCOMPARE(source.write(QByteArrayLiteral("\nS2_TAIL_MARKER\n")), qint64(16));
        source.close();

        QWidget window;
        window.resize(1000, 700);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QElapsedTimer opening;
        opening.start();
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, false), qPrintable(error));
        QVERIFY2(opening.elapsed() < 1500,
                 "near-2-GiB preview must not load the full document");
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        QCOMPARE(provider->fileSize(), size);
        QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
        QVERIFY(!tab->l2View()->isOpen());
        QVERIFY(provider->editor()->text().contains(QStringLiteral("S2_HEAD_MARKER")));
        tab->gotoByteOffset(middle);
        QVERIFY(provider->editor()->text().contains(QStringLiteral("S2_MIDDLE_MARKER")));
        tab->gotoByteOffset(size - 16);
        QVERIFY(provider->editor()->text().contains(QStringLiteral("S2_TAIL_MARKER")));
        QVERIFY(provider->globalOffset() >= size - 16);
        QVERIFY(provider->windowEnd() == size);
        window.close();
    }

    void s3ProductionProviderDragMatchesRenderedHex()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../bigtestfile"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        const qint64 size = QFileInfo(path).size();
        QVERIFY(size > 2LL * 1024 * 1024 * 1024);

        QWidget window;
        window.setWindowTitle(QStringLiteral("Notepad Safe production S3 provider"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, false,
                                      L2Presentation::Hex,
                                      L2ContentKind::Binary), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isL2Active());
        QVERIFY(tab->isReadOnlyByPolicy());
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() >= 24, 3000);

        window.resize(720, 820);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() <= 16, 3000);
        window.resize(1280, 820);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() >= 24, 3000);

        dragScrollBar(provider->globalScrollBar(), 0.73);
        QTRY_VERIFY_WITH_TIMEOUT(provider->globalOffset() > size * 65 / 100, 3000);
        const qint64 dragged = provider->globalOffset();
        const QString firstLine = provider->editor()->text(0);
        QVERIFY(firstLine.size() >= 2);
        auto *ascii = provider->asciiPreview();
        QVERIFY(ascii && ascii->isVisibleTo(provider));
        const int addressDigits = qMax(
            8, QString::number(size - 1, 16).size());
        QCOMPARE(marginText(provider->editor(), 0),
                 QString::number(provider->windowStart(), 16)
                     .rightJustified(addressDigits, QLatin1Char('0')).toUpper());
        const QByteArray firstByte = provider->sourceBytes(provider->windowStart(), 1).toHex().toUpper();
        QCOMPARE(firstLine.left(2).toLatin1(), firstByte);
        const uchar firstValue = uchar(provider->sourceBytes(provider->windowStart(), 1).at(0));
        QCOMPARE(ascii->toPlainText().left(1),
                 QString(firstValue >= 0x20 && firstValue <= 0x7e
                             ? QChar(firstValue) : QLatin1Char('.')));
        QVERIFY(provider->editor()->geometry().right()
                <= ascii->geometry().left());
        QTest::qWait(1000);
        QCOMPARE(provider->globalOffset(), dragged);
        QCOMPARE(provider->editor()->text(0), firstLine);

        const qint64 exactTarget = size * 3 / 4;
        tab->gotoByteOffset(exactTarget, 1);
        QCOMPARE(provider->globalOffset(), exactTarget);
        const QString exactFirstLine = provider->editor()->text(0);
        QCOMPARE(marginText(provider->editor(), 0),
                 QString::number(provider->windowStart(), 16)
                     .rightJustified(addressDigits, QLatin1Char('0')).toUpper());
        QCOMPARE(exactFirstLine.left(2).toLatin1(),
                 provider->sourceBytes(provider->windowStart(), 1).toHex().toUpper());
        QVERIFY(provider->windowStart() <= exactTarget);
        QVERIFY(provider->windowEnd() > exactTarget);

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-production-s3-dynamic.png"));
        QVERIFY(window.grab().save(screenshot));

        // S3 Hex is read-only but must retain the useful half of an editor:
        // precise byte selection/copy and full-file navigation. Every keyboard
        // and command mutation path remains a source-file no-op.
        const qint64 probe = exactTarget;
        const QByteArray before = provider->sourceBytes(probe, 32);
        tab->gotoByteOffset(probe, 4);
        tab->copy();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QString::fromLatin1(before.left(4).toHex(' ').toUpper()));
        QGuiApplication::clipboard()->setText(QStringLiteral("AA BB"));
        provider->editor()->setFocus(Qt::OtherFocusReason);
        QTest::keyClicks(provider->editor(), QStringLiteral("FF"));
        QTest::keyClick(provider->editor(), Qt::Key_Delete);
        QTest::keyClick(provider->editor(), Qt::Key_Backspace);
        tab->cut();
        tab->paste();
        QCOMPARE(QFileInfo(path).size(), size);
        QCOMPARE(provider->sourceBytes(probe, 32), before);
        QTest::keyClick(provider->editor(), Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(provider->globalOffset(), qint64(0));
        QTest::keyClick(provider->editor(), Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(provider->globalOffset(), size - 1);
        window.close();
    }

    void b2BinaryUsesStableSingleScrollFallback()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../smalltestfile"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        const qint64 size = QFileInfo(path).size();
        QVERIFY(size > 32LL * 1024 * 1024);
        QVERIFY(size < 2LL * 1024 * 1024 * 1024);

        QWidget window;
        window.setWindowTitle(QStringLiteral("Notepad Safe production B2 fallback"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error, false,
                                      L2Presentation::Hex,
                                      L2ContentKind::Binary), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() >= 24, 3000);

        dragScrollBar(provider->globalScrollBar(), 0.57);
        QTRY_VERIFY_WITH_TIMEOUT(provider->globalOffset() > size / 2, 3000);
        const qint64 dragged = provider->globalOffset();
        const QString firstLine = provider->editor()->text(0);
        auto *ascii = provider->asciiPreview();
        QVERIFY(ascii && ascii->isVisibleTo(provider));
        const int addressDigits = qMax(
            8, QString::number(size - 1, 16).size());
        QCOMPARE(marginText(provider->editor(), 0),
                 QString::number(provider->windowStart(), 16)
                     .rightJustified(addressDigits, QLatin1Char('0')).toUpper());
        QCOMPARE(firstLine.left(2).toLatin1(),
                 provider->sourceBytes(provider->windowStart(), 1).toHex().toUpper());
        const uchar firstValue = uchar(provider->sourceBytes(provider->windowStart(), 1).at(0));
        QCOMPARE(ascii->toPlainText().left(1),
                 QString(firstValue >= 0x20 && firstValue <= 0x7e
                             ? QChar(firstValue) : QLatin1Char('.')));
        QVERIFY(provider->editor()->geometry().right()
                <= ascii->geometry().left());
        QTest::qWait(1000);
        QCOMPARE(provider->globalOffset(), dragged);
        QCOMPARE(provider->editor()->text(0), firstLine);

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-production-b2-dynamic.png"));
        QVERIFY(window.grab().save(screenshot));

        const qint64 probe = size * 3 / 5;
        const QByteArray before = provider->sourceBytes(probe, 32);
        tab->gotoByteOffset(probe, 4);
        tab->copy();
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QString::fromLatin1(before.left(4).toHex(' ').toUpper()));
        QGuiApplication::clipboard()->setText(QStringLiteral("AA BB"));
        provider->editor()->setFocus(Qt::OtherFocusReason);
        QTest::keyClicks(provider->editor(), QStringLiteral("FF"));
        QTest::keyClick(provider->editor(), Qt::Key_Delete);
        QTest::keyClick(provider->editor(), Qt::Key_Backspace);
        tab->cut();
        tab->paste();
        QCOMPARE(QFileInfo(path).size(), size);
        QCOMPARE(provider->sourceBytes(probe, 32), before);
        QTest::keyClick(provider->editor(), Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(provider->globalOffset(), qint64(0));
        QTest::keyClick(provider->editor(), Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(provider->globalOffset(), size - 1);
        window.close();
    }

    void utf16PromotesAndSavesInOriginalEncoding()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString sourcePath = temporary.filePath(QStringLiteral("utf16-large.log"));
        QSaveFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("\xff\xfe", 2), 2LL);
        const QString line = QStringLiteral(
            "2026-08-15 INFO marker=UTF16_PROMOTION_OK 中文日志\r\n");
        const QByteArray lineBytes(reinterpret_cast<const char *>(line.utf16()),
                                   line.size() * 2);
        QByteArray block;
        while (block.size() + lineBytes.size() <= 1024 * 1024)
            block += lineBytes;
        block += QByteArray(1024 * 1024 - block.size(), 0);
        for (int i = 0; i < 33; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());
        const qint64 sourceBytes = QFileInfo(sourcePath).size();
        QVERIFY(sourceBytes > 32LL * 1024 * 1024);

        QWidget window;
        window.resize(1200, 760);
        auto *layout = new QVBoxLayout(&window);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QString error;
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error, true), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isL2Mode(), 30000);
        QCOMPARE(tab->encoding(), TextEncoding::Utf16LE);
        QVERIFY(tab->editor()->text(0).contains(QStringLiteral("UTF16_PROMOTION_OK")));
        QVERIFY(tab->editor()->text(0).contains(QStringLiteral("中文日志")));

        tab->editor()->setCursorPosition(0, 0);
        tab->editor()->setFocus();
        QTest::keyClicks(tab->editor(), QStringLiteral("EDIT_"));
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text(0).startsWith(QStringLiteral("EDIT_")),
                                 3000);

        const QString savedPath = temporary.filePath(QStringLiteral("utf16-saved.log"));
        QVERIFY2(tab->saveAsFile(savedPath, &error), qPrintable(error));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray prefix = saved.read(12);
        QCOMPARE(prefix.left(2), QByteArray("\xff\xfe", 2));
        QCOMPARE(prefix.mid(2, 10), QByteArray("E\0D\0I\0T\0_\0", 10));
        QCOMPARE(saved.size(), sourceBytes + 10);
        window.close();
    }

    void encryptedEditJournalRestoresWithoutPlaintextSnapshot()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString sourcePath = temporary.filePath(QStringLiteral("journal-base.log"));
        QSaveFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray line = QByteArrayLiteral(
            "2026-08-15T12:34:56Z INFO service=api request=42 payload=bbbbbbbbbbbbbbbb\n");
        QByteArray block;
        while (block.size() + line.size() <= 1024 * 1024)
            block += line;
        block += QByteArray(1024 * 1024 - block.size(), '\n');
        for (int i = 0; i < 33; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());

        QWidget sourceWindow;
        sourceWindow.resize(1100, 700);
        auto *sourceLayout = new QVBoxLayout(&sourceWindow);
        auto *sourceTab = new EditorTab(&sourceWindow);
        sourceLayout->addWidget(sourceTab);
        sourceWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&sourceWindow));
        QString error;
        QVERIFY2(sourceTab->openLargeFileL2(sourcePath, &error, true), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!sourceTab->isL2Mode(), 30000);

        const QString marker = QStringLiteral("RECOVERED_S2_EDIT_");
        sourceTab->editor()->setCursorPosition(0, 0);
        sourceTab->editor()->setFocus();
        QTest::keyClicks(sourceTab->editor(), marker);
        QTRY_VERIFY_WITH_TIMEOUT(sourceTab->editor()->text(0).startsWith(marker), 3000);
        sourceTab->editor()->setSelection(0, marker.size(), 0, marker.size() + 5);
        QTest::keyClick(sourceTab->editor(), Qt::Key_Delete);
        const QString expectedPrefix = marker + QStringLiteral("08-15");
        QTRY_VERIFY_WITH_TIMEOUT(sourceTab->editor()->text(0).startsWith(expectedPrefix),
                                 3000);

        SessionManager session(temporary.filePath(QStringLiteral("session-data")));
        QVector<EditorTab *> tabs{sourceTab};
        QVERIFY(session.saveSession(tabs, 0, true));
        const SessionData state = session.loadSession();
        QCOMPARE(state.tabs.size(), 1);
        const QString draftPath = state.tabs.first().autosavePath;
        QVERIFY(DraftStore::isEncryptedDraft(draftPath));
        QFile encrypted(draftPath);
        QVERIFY(encrypted.open(QIODevice::ReadOnly));
        const QByteArray diskBytes = encrypted.readAll();
        QVERIFY(!diskBytes.contains(marker.toUtf8()));
        QVERIFY2(diskBytes.size() < 1024 * 1024,
                 "small edit journal must not copy the 33 MiB base file");

        QWidget restoredWindow;
        restoredWindow.setWindowTitle(QStringLiteral("Notepad Safe S2 Recovery Acceptance"));
        restoredWindow.resize(1100, 700);
        auto *restoredLayout = new QVBoxLayout(&restoredWindow);
        auto *restoredTab = new EditorTab(&restoredWindow);
        restoredLayout->addWidget(restoredTab);
        restoredWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restoredWindow));
        QVERIFY(session.restoreAutosave(restoredTab, draftPath, sourcePath));
        QVERIFY(restoredTab->isL2Mode());
        QTRY_VERIFY_WITH_TIMEOUT(!restoredTab->isL2Mode(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(restoredTab->editor()->text(0).startsWith(expectedPrefix),
                                 3000);
        QVERIFY(restoredTab->isModified());

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("gui-s2-encrypted-recovery.png"));
        QVERIFY(restoredWindow.grab().save(screenshot));
        restoredWindow.close();
        sourceWindow.close();
    }

    void chunkedEncryptedCheckpointRestoresDirectlyToScintilla()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString sourcePath = temporary.filePath(QStringLiteral("checkpoint-base.log"));
        QSaveFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray line = QByteArrayLiteral(
            "2026-08-15 WARN component=storage payload=cccccccccccccccccccccccccccc\n");
        QByteArray block;
        while (block.size() + line.size() <= 1024 * 1024)
            block += line;
        block += QByteArray(1024 * 1024 - block.size(), '\n');
        for (int i = 0; i < 33; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());

        QWidget sourceWindow;
        sourceWindow.resize(1100, 700);
        auto *layout = new QVBoxLayout(&sourceWindow);
        auto *tab = new EditorTab(&sourceWindow);
        layout->addWidget(tab);
        sourceWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&sourceWindow));
        QString error;
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error, true), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isL2Mode(), 30000);
        const QString marker = QStringLiteral("CHECKPOINT_RECOVERED_");
        tab->editor()->setCursorPosition(0, 0);
        tab->editor()->setFocus();
        QTest::keyClicks(tab->editor(), marker);
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text(0).startsWith(marker), 3000);

        const QString checkpoint = temporary.filePath(QStringLiteral("full.ndraft"));
        QVERIFY2(DraftStore::writeCheckpoint(
                     checkpoint, int(tab->encoding()),
                     [tab](QIODevice *output, QString *writerError) {
                         return tab->exportRecoveryCheckpoint(output, writerError);
                     }, &error), qPrintable(error));
        DraftStore::SnapshotInfo info;
        QVERIFY2(DraftStore::inspect(checkpoint, &info, &error), qPrintable(error));
        QCOMPARE(info.kind, DraftStore::PayloadKind::FullCheckpoint);
        QCOMPARE(info.plaintextBytes, QFileInfo(sourcePath).size() + marker.toUtf8().size());
        QFile encrypted(checkpoint);
        QVERIFY(encrypted.open(QIODevice::ReadOnly));
        QVERIFY(!encrypted.readAll().contains(marker.toUtf8()));

        QWidget restoredWindow;
        restoredWindow.resize(1100, 700);
        auto *restoredLayout = new QVBoxLayout(&restoredWindow);
        auto *restored = new EditorTab(&restoredWindow);
        restoredLayout->addWidget(restored);
        restoredWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restoredWindow));
        SessionManager session(temporary.filePath(QStringLiteral("checkpoint-session")));
        QVERIFY2(session.restoreAutosave(restored, checkpoint, sourcePath), qPrintable(error));
        QVERIFY(restored->isL2Mode());
        QTRY_VERIFY_WITH_TIMEOUT(!restored->isL2Mode(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(restored->editor()->text(0).startsWith(marker), 3000);
        QVERIFY(restored->isModified());

        const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("gui-s2-checkpoint-recovery.png"));
        QVERIFY(restoredWindow.grab().save(screenshot));
        restoredWindow.close();
        sourceWindow.close();
    }
};

QTEST_MAIN(S2PromotionSmokeTest)
#include "smoke_s2_promotion.moc"
