#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMimeData>
#include <QInputMethodEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextBlock>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include "EditorTab.h"
#include "ByteClipboard.h"
#include "FindReplaceDialog.h"
#include "WindowedReadOnlyProvider.h"
#include "VirtualFileView.h"
#include "AsyncTaskManager.h"

// Historical source filename; this suite now verifies that S2 and S3 stay on
// the same read-only mmap path and never start a background editor load.
class LargeReadOnlySmokeTest final : public QObject {
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

    static void dragScrollBarContinuously(QScrollBar *bar, qreal targetRatio)
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
        const QPoint start = handle.center();
        const QPoint destination = bar->orientation() == Qt::Vertical
            ? QPoint(bar->width() / 2,
                     qBound(2, int(bar->height() * targetRatio), bar->height() - 2))
            : QPoint(qBound(2, int(bar->width() * targetRatio), bar->width() - 2),
                     bar->height() / 2);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 24; ++step) {
            const QPoint point = start + (destination - start) * step / 24;
            QTest::mouseMove(bar, point, 8);
            QCoreApplication::processEvents();
        }
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

    static QByteArray fileSha256(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
                return {};
            hash.addData(chunk);
        }
        return hash.result();
    }

    static int visibleVerticalScrollBars(QWidget *root)
    {
        int count = 0;
        const auto bars = root->findChildren<QScrollBar *>();
        for (QScrollBar *bar : bars) {
            if (bar->orientation() == Qt::Vertical && bar->isVisibleTo(root)
                && bar->maximum() > bar->minimum()) {
                ++count;
            }
        }
        return count;
    }

    static int visibleHorizontalScrollBars(QWidget *root)
    {
        int count = 0;
        const auto bars = root->findChildren<QScrollBar *>();
        for (QScrollBar *bar : bars) {
            if (bar->orientation() == Qt::Horizontal && bar->isVisibleTo(root))
                ++count;
        }
        return count;
    }

    static int nonCharacterCountTasks(const QSignalSpy &spy)
    {
        int count = 0;
        for (const QList<QVariant> &arguments : spy) {
            if (arguments.isEmpty())
                continue;
            const AsyncTask task = AsyncTaskManager::instance().taskById(
                arguments.constFirst().toLongLong());
            if (task.categoryTag != QStringLiteral("CharacterCount"))
                ++count;
        }
        return count;
    }

    static QByteArray scintillaBytes(QsciScintilla *editor)
    {
        const long length = editor->SendScintilla(
            QsciScintillaBase::SCI_GETLENGTH);
        QByteArray bytes(qMax<long>(0, length) + 1, '\0');
        editor->SendScintilla(QsciScintillaBase::SCI_GETTEXT,
                              static_cast<uintptr_t>(bytes.size()), bytes.data());
        bytes.resize(qMax<long>(0, length));
        return bytes;
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

    void tinyBinaryHexRowsReverseSelectionRawSearchAndTextLineNumbers()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../tinytestfile"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QVERIFY(QFileInfo(path).size() > 0);
        QVERIFY(QFileInfo(path).size() < qint64(32) * 1024 * 1024);

        QWidget window;
        window.setWindowTitle(QStringLiteral("tinytestfile Hex contract"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.raise();
        window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);

        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error,
                                      L2Presentation::Hex,
                                      L2ContentKind::Binary), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QVERIFY(!tab->windowedReadOnlyProvider()); // 32 MiB 以下走可编辑 B1。
        QVERIFY(tab->l2View());
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->isOpen(), 5000);

        QsciScintilla *hex = tab->editor();
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(hex && ascii && ascii->isVisibleTo(&window));
        const int bytesPerRow = tab->l2View()->hexBytesPerRow();
        QVERIFY(bytesPerRow >= 8);
        const int sciRowHeight = int(hex->SendScintilla(
            QsciScintillaBase::SCI_TEXTHEIGHT, 0UL));
        QVERIFY(sciRowHeight > 0);
        for (int row = 0; row <= 1000; ++row) {
            const QTextBlock block = ascii->document()->findBlockByNumber(row);
            QVERIFY2(block.isValid(), qPrintable(QStringLiteral("row=%1").arg(row)));
            const QRectF bounds = ascii->document()->documentLayout()
                                      ->blockBoundingRect(block);
            QCOMPARE(qRound(bounds.height()), sciRowHeight);
        }

        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        constexpr qint64 copiedOffset = 0x416;
        QVERIFY(source.seek(copiedOffset));
        const QByteArray expectedBytes = source.read(3);
        QCOMPARE(expectedBytes.size(), 3);
        QVERIFY(source.seek(0));
        const qint64 expectedFirstOffset = source.readAll().indexOf(expectedBytes);
        QVERIFY(expectedFirstOffset >= 0);
        const QString expectedHexText = QString::fromLatin1(
            expectedBytes.toHex(' ').toUpper());
        source.close();

        // Physically drag the ASCII pane from the right boundary back to the
        // left.  The mirror must not rewrite the native anchor during MouseMove.
        const int row = int(copiedOffset / bytesPerRow);
        const int column = int(copiedOffset % bytesPerRow);
        const QTextBlock block = ascii->document()->findBlockByNumber(row);
        QVERIFY(block.isValid());
        tab->gotoByteOffset(copiedOffset, 0);
        QCoreApplication::processEvents();
        QTextCursor leftCursor(ascii->document());
        leftCursor.setPosition(block.position() + column);
        QTextCursor rightCursor(ascii->document());
        rightCursor.setPosition(block.position() + column + 3);
        const QPoint left = ascii->cursorRect(leftCursor).center();
        const QPoint right = ascii->cursorRect(rightCursor).center();
        QVERIFY(ascii->viewport()->rect().contains(left));
        QVERIFY(ascii->viewport()->rect().contains(right));
        QTest::mousePress(ascii->viewport(), Qt::LeftButton,
                          Qt::NoModifier, right);
        QTest::mouseMove(ascii->viewport(), left, 50);
        QTest::mouseRelease(ascii->viewport(), Qt::LeftButton,
                            Qt::NoModifier, left);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(), copiedOffset, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), copiedOffset + 3, 1000);
        QCOMPARE(tab->l2View()->selectedBytes(), expectedBytes);
        ascii->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(ascii->hasFocus());
        QVERIFY(window.grab().save(
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("gui-tiny-hex-aligned-reverse-selection.png"))));
        tab->copy();
        const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mime);
        QCOMPARE(mime->data(QString::fromLatin1(ByteClipboard::RawBytesMime)),
                 expectedBytes);
        QCOMPARE(QGuiApplication::clipboard()->text(), expectedHexText);

        FindReplaceDialog dialog(&window);
        dialog.setActiveTab(tab);
        dialog.showFindMode();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto *pattern = dialog.findChild<QLineEdit *>(
            QStringLiteral("findPatternEdit"));
        auto *hexMode = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
        auto *findNext = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
        QVERIFY(pattern && hexMode && findNext);
        pattern->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(pattern, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(pattern->text(), expectedHexText);
        QVERIFY(hexMode->isChecked());
        QTest::mouseClick(findNext, Qt::LeftButton);
        QVERIFY(tab->l2View()->selectionStart() > copiedOffset);
        QCOMPARE(tab->l2View()->selectedBytes(), expectedBytes);
        tab->resetSearchPosition(true);
        QTest::mouseClick(findNext, Qt::LeftButton);
        QCOMPARE(tab->l2View()->selectionStart(), expectedFirstOffset);
        QCOMPARE(tab->l2View()->selectionEnd(), expectedFirstOffset + 3);
        dialog.close();

        QVERIFY(window.grab().save(
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("gui-tiny-hex-raw-byte-search.png"))));

        // Switch at a non-zero projected window. The gutter must be the real
        // absolute decimal text line, not stale Hex addresses or local 1..N.
        const qint64 textTarget = QFileInfo(path).size() * 3 / 4;
        tab->gotoByteOffset(textTarget, 0);
        const qint64 windowStart = tab->l2View()->activeWindowByteStart();
        QFile prefix(path);
        QVERIFY(prefix.open(QIODevice::ReadOnly));
        qint64 expectedFirstLine = 0;
        constexpr qint64 CountChunk = 4LL * 1024 * 1024;
        for (qint64 offset = 0; offset < windowStart; offset += CountChunk) {
            const QByteArray bytes = prefix.read(
                qMin(CountChunk, windowStart - offset));
            QVERIFY(!bytes.isEmpty());
            expectedFirstLine += bytes.count('\n');
        }
        prefix.close();
        tab->setViewModeL2(ViewMode::Text);
        QTRY_COMPARE_WITH_TIMEOUT(tab->viewModeL2(), ViewMode::Text, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(marginText(tab->editor(), 0),
                                  QString::number(expectedFirstLine + 1), 3000);
        bool decimalOk = false;
        QCOMPARE(marginText(tab->editor(), 0).toLongLong(&decimalOk),
                 qint64(expectedFirstLine + 1));
        QVERIFY(decimalOk);
        QCOMPARE(marginText(tab->editor(), 1),
                 QString::number(expectedFirstLine + 2));
        QVERIFY(window.grab().save(
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("gui-tiny-binary-text-absolute-lines.png"))));
        window.close();
    }

    void tinyBinaryAsciiEditingTextScrollAndWrapAreByteSafe()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../tinytestfile"));
        const QFileInfo sourceInfo(path);
        QVERIFY2(sourceInfo.exists(), qPrintable(path));
        QVERIFY(sourceInfo.size() > 0);
        QVERIFY(sourceInfo.size() < qint64(32) * 1024 * 1024);
        const QByteArray diskHashBefore = fileSha256(path);
        QVERIFY(!diskHashBefore.isEmpty());

        QWidget window;
        window.setWindowTitle(QStringLiteral("tinytestfile editing/scroll/wrap contract"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.raise();
        window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.isActiveWindow(), 1000);

        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error,
                                      L2Presentation::Hex,
                                      L2ContentKind::Binary), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View() && tab->l2View()->isOpen(), 5000);
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(&window));

        // Default application routing opens this fixture in Hex. Reproduce a
        // real continuous thumb gesture before any programmatic go-to call.
        QScrollBar *hexGlobal = tab->l2View()->verticalScrollBar();
        QVERIFY(hexGlobal && hexGlobal->maximum() > 0);
        QSignalSpy hexProjectionChanges(tab->l2View(),
                                        &VirtualFileView::activeWindowChanged);
        dragScrollBarContinuously(hexGlobal, 0.61);
        QTRY_VERIFY_WITH_TIMEOUT(
            tab->l2View()->activeWindowByteStart() > sourceInfo.size() / 2,
            5000);
        QTest::qWait(500); // allow the one release projection to paint
        const int settledHexValue = hexGlobal->value();
        const qint64 settledHexWindow = tab->l2View()->activeWindowByteStart();
        const int settledHexProjections = hexProjectionChanges.count();
        QTest::qWait(3000);
        QCOMPARE(hexGlobal->value(), settledHexValue);
        QCOMPARE(tab->l2View()->activeWindowByteStart(), settledHexWindow);
        QVERIFY2(hexProjectionChanges.count() <= settledHexProjections + 1,
                 "idle Hex viewport kept rebuilding after drag release");

        // Click and type at the actually painted distant row. Programmatic
        // goto/cursor placement cannot expose a stale windowStart or a pane
        // whose internal scroll is no longer aligned with the global thumb.
        const QPoint distantPoint(
            qMax(2, ascii->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 3),
            ascii->viewport()->height() / 2);
        const QTextCursor distantCursor = ascii->cursorForPosition(distantPoint);
        const QTextBlock distantBlock = distantCursor.block();
        QVERIFY(distantBlock.isValid());
        const int distantColumn = distantCursor.position() - distantBlock.position();
        QVERIFY(distantColumn >= 0
                && distantColumn < tab->l2View()->hexBytesPerRow());
        const qint64 distantOffset = tab->l2View()->activeWindowByteStart()
            + qint64(distantBlock.blockNumber()) * tab->l2View()->hexBytesPerRow()
            + distantColumn;
        const QByteArray distantOriginal =
            tab->l2View()->readByteRange(distantOffset, distantOffset + 1);
        QCOMPARE(distantOriginal.size(), 1);
        QTest::mouseClick(ascii->viewport(), Qt::LeftButton,
                          Qt::NoModifier, distantPoint);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->cursorByte(), distantOffset, 1000);
        QElapsedTimer distantTyping;
        distantTyping.start();
        QTest::keyClick(ascii, Qt::Key_Q);
        QVERIFY2(distantTyping.elapsed() < 250,
                 qPrintable(QStringLiteral("one distant ASCII overwrite took %1 ms")
                                .arg(distantTyping.elapsed())));
        QCOMPARE(tab->l2View()->readByteRange(distantOffset, distantOffset + 1),
                 QByteArrayLiteral("q"));
        QCOMPARE(tab->l2View()->cursorByte(), distantOffset + 1);
        tab->undo();
        QCOMPARE(tab->l2View()->readByteRange(distantOffset, distantOffset + 1),
                 distantOriginal);

        QTest::mouseClick(ascii->viewport(), Qt::LeftButton,
                          Qt::NoModifier, distantPoint);
        const qint64 commitOffset = tab->l2View()->cursorByte();
        const QByteArray commitOriginal =
            tab->l2View()->readByteRange(commitOffset, commitOffset + 4);
        QCOMPARE(commitOriginal.size(), 4);
        QInputMethodEvent asciiCommit;
        asciiCommit.setCommitString(QStringLiteral("SAFE"));
        QElapsedTimer commitTimer;
        commitTimer.start();
        QApplication::sendEvent(ascii, &asciiCommit);
        QCoreApplication::processEvents();
        QVERIFY2(commitTimer.elapsed() < 250,
                 qPrintable(QStringLiteral("four-byte ASCII IME commit took %1 ms")
                                .arg(commitTimer.elapsed())));
        QCOMPARE(tab->l2View()->readByteRange(commitOffset, commitOffset + 4),
                 QByteArrayLiteral("SAFE"));
        QCOMPARE(tab->l2View()->cursorByte(), commitOffset + 4);
        tab->undo();
        QCOMPARE(tab->l2View()->readByteRange(commitOffset, commitOffset + 4),
                 commitOriginal);

        constexpr qint64 editOffset = 0x416;
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QVERIFY(source.seek(editOffset));
        const QByteArray original = source.read(8);
        QCOMPARE(original.size(), 8);
        source.close();

        // ASCII pane must support both conventional Hex-editor modes. Insert
        // grows the byte stream; overwrite keeps its size. Every key must move
        // the caret and update both projections without touching the disk file.
        const auto placeAsciiCaret = [tab, ascii, &window](qint64 byteOffset) {
            tab->gotoByteOffset(byteOffset, 0);
            QCoreApplication::processEvents();
            const qint64 local = byteOffset
                - tab->l2View()->activeWindowByteStart();
            const int bytesPerRow = tab->l2View()->hexBytesPerRow();
            const QTextBlock block = ascii->document()->findBlockByNumber(
                int(local / bytesPerRow));
            if (!block.isValid())
                return false;
            QTextCursor cursor(ascii->document());
            cursor.setPosition(block.position() + int(local % bytesPerRow));
            window.raise();
            window.activateWindow();
            ascii->setFocus(Qt::OtherFocusReason);
            ascii->setTextCursor(cursor);
            QCoreApplication::processEvents();
            return true;
        };
        QVERIFY(placeAsciiCaret(editOffset));
        QTRY_VERIFY_WITH_TIMEOUT(ascii->hasFocus(), 1000);
        QTRY_COMPARE(tab->l2View()->cursorByte(), editOffset);
        QVERIFY(tab->hexOverwriteMode());
        QTest::keyClick(ascii, Qt::Key_Insert);
        QVERIFY(!tab->hexOverwriteMode());
        QElapsedTimer typing;
        typing.start();
        QTest::keyClicks(ascii, QStringLiteral("ABC"), Qt::NoModifier, 0);
        QVERIFY2(typing.elapsed() < 3000,
                 qPrintable(QStringLiteral("three ASCII insert keys took %1 ms")
                                .arg(typing.elapsed())));
        QCOMPARE(tab->l2View()->virtualFileSize(), sourceInfo.size() + 3);
        QCOMPARE(tab->l2View()->readByteRange(editOffset, editOffset + 11),
                 QByteArray("ABC") + original);
        QCOMPARE(tab->l2View()->cursorByte(), editOffset + 3);

        tab->undo();
        tab->undo();
        tab->undo();
        QTRY_COMPARE(tab->l2View()->virtualFileSize(), sourceInfo.size());
        QCOMPARE(tab->l2View()->readByteRange(editOffset, editOffset + 8), original);
        QTest::keyClick(ascii, Qt::Key_Insert);
        QVERIFY(tab->hexOverwriteMode());
        QVERIFY(placeAsciiCaret(editOffset));
        QTRY_VERIFY_WITH_TIMEOUT(ascii->hasFocus(), 1000);
        QTRY_COMPARE(tab->l2View()->cursorByte(), editOffset);
        QTest::keyClicks(ascii, QStringLiteral("XYZ"), Qt::NoModifier, 0);
        QCOMPARE(tab->l2View()->virtualFileSize(), sourceInfo.size());
        QCOMPARE(tab->l2View()->readByteRange(editOffset, editOffset + 8),
                 QByteArray("XYZ") + original.mid(3));
        tab->undo();
        tab->undo();
        tab->undo();
        QCOMPARE(tab->l2View()->readByteRange(editOffset, editOffset + 8), original);
        QVERIFY(!tab->l2View()->patches().isModified());
        QCOMPARE(tab->l2View()->virtualFileSize(), sourceInfo.size());

        tab->setViewModeL2(ViewMode::Text);
        QTRY_COMPARE_WITH_TIMEOUT(tab->viewModeL2(), ViewMode::Text, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(visibleVerticalScrollBars(tab), 1, 3000);
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETVSCROLLBAR), 0L);
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETHSCROLLBAR), 0L);
        QScrollBar *global = tab->l2View()->verticalScrollBar();
        QVERIFY(global && global->maximum() > 0);

        QSignalSpy projectionChanges(tab->l2View(),
                                     &VirtualFileView::activeWindowChanged);
        dragScrollBarContinuously(global, 0.75);
        QTRY_VERIFY_WITH_TIMEOUT(
            tab->l2View()->activeWindowByteStart() > sourceInfo.size() / 2, 5000);
        const int settledAt75 = global->value();
        QVERIFY(settledAt75 > global->maximum() / 2);
        const qint64 settledWindowStart = tab->l2View()->activeWindowByteStart();
        const int projectionsAfterRelease = projectionChanges.count();
        QTest::qWait(3000);
        QCOMPARE(global->value(), settledAt75); // no delayed feedback-loop snap-back
        QCOMPARE(tab->l2View()->activeWindowByteStart(), settledWindowStart);
        QVERIFY2(projectionChanges.count() <= projectionsAfterRelease + 1,
                 "an idle viewport kept rebuilding projections after drag release");

        const qint64 sizeBeforeWrap = tab->l2View()->virtualFileSize();
        QCOMPARE(sizeBeforeWrap, sourceInfo.size());
        const int logicalLinesBeforeWrap = tab->editor()->lines();
        const QString projectedTextBeforeWrap = tab->editor()->text();
        const QByteArray projectedBytesBeforeWrap = scintillaBytes(tab->editor());
        QCOMPARE(projectedBytesBeforeWrap,
                 tab->l2View()->readByteRange(
                     tab->l2View()->activeWindowByteStart(),
                     tab->l2View()->activeWindowByteEnd()));
        QVERIFY(!tab->l2View()->patches().isModified());
        tab->setWordWrap(true);
        QTRY_VERIFY_WITH_TIMEOUT(tab->wordWrap(), 1000);
        QCOMPARE(tab->editor()->wrapMode(), QsciScintilla::WrapWord);
        QTRY_COMPARE_WITH_TIMEOUT(visibleHorizontalScrollBars(tab), 0, 1000);
        QCOMPARE(tab->editor()->lines(), logicalLinesBeforeWrap);
        QCOMPARE(tab->editor()->text(), projectedTextBeforeWrap);
        QCOMPARE(scintillaBytes(tab->editor()), projectedBytesBeforeWrap);
        QCOMPARE(tab->l2View()->virtualFileSize(), sizeBeforeWrap);
        QVERIFY(!tab->l2View()->patches().isModified());

        // The persisted application setting may enable wrapping before the
        // user opens/switches this file. Exercise a non-terminal continuous
        // thumb drag while wrap layout notifications are active; EOF-only
        // coverage cannot expose a midpoint feedback loop.
        dragScrollBarContinuously(global, 0.42);
        QTRY_VERIFY_WITH_TIMEOUT(global->value() > global->maximum() / 3, 5000);
        const int wrappedSettledValue = global->value();
        const qint64 wrappedSettledStart = tab->l2View()->activeWindowByteStart();
        const int wrappedProjectionCount = projectionChanges.count();
        QTest::qWait(3000);
        QCOMPARE(global->value(), wrappedSettledValue);
        QCOMPARE(tab->l2View()->activeWindowByteStart(), wrappedSettledStart);
        QVERIFY2(projectionChanges.count() <= wrappedProjectionCount + 1,
                 "wrapped idle viewport kept rebuilding after midpoint drag");

        dragScrollBar(global, 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->activeWindowByteEnd(),
                                  tab->l2View()->virtualFileSize(), 5000);
        QTest::qWait(500);
        QVERIFY(global->value() >= global->maximum() - global->pageStep());
        QVERIFY(!tab->l2View()->patches().isModified());
        QCOMPARE(tab->l2View()->virtualFileSize(), sizeBeforeWrap);

        QVERIFY(window.grab().save(
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("gui-tiny-binary-text-wrap-eof.png"))));
        tab->setWordWrap(false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab->wordWrap(), 1000);
        QVERIFY(!tab->l2View()->patches().isModified());
        window.close();
        QCOMPARE(fileSha256(path), diskHashBefore);
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
        const int windowedRowHeight = int(hex->SendScintilla(
            QsciScintillaBase::SCI_TEXTHEIGHT, 0UL));
        for (QTextBlock block = ascii->document()->firstBlock();
             block.isValid(); block = block.next()) {
            QCOMPARE(qRound(ascii->document()->documentLayout()
                                ->blockBoundingRect(block).height()),
                     windowedRowHeight);
        }

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

        // Exercise the real reverse mouse gesture too. Programmatically
        // assigning QTextCursor cannot expose anchor resets during MouseMove.
        QTextCursor reverseLeft(ascii->document());
        reverseLeft.setPosition(4);
        QTextCursor reverseRight(ascii->document());
        reverseRight.setPosition(7);
        const QPoint reverseLeftPoint = ascii->cursorRect(reverseLeft).center();
        const QPoint reverseRightPoint = ascii->cursorRect(reverseRight).center();
        QTest::mousePress(ascii->viewport(), Qt::LeftButton,
                          Qt::NoModifier, reverseRightPoint);
        QTest::mouseMove(ascii->viewport(), reverseLeftPoint, 50);
        QTest::mouseRelease(ascii->viewport(), Qt::LeftButton,
                            Qt::NoModifier, reverseLeftPoint);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), qint64(4), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(), qint64(7), 1000);

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

    void windowedHexBothPanesWheelInLockstep()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("hex-wheel-lockstep.bin"));
        QByteArray block(1024 * 1024, '\0');
        for (int i = 0; i < block.size(); ++i)
            block[i] = char((i * 17 + 31) & 0xff);
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            for (int i = 0; i < 4; ++i)
                QCOMPARE(source.write(block), qint64(block.size()));
        }

        QWidget window;
        window.resize(1100, 720);
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
        const auto hexFirst = [hex]() {
            return int(hex->SendScintilla(
                QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        };
        const auto asciiFirst = [ascii]() {
            return ascii->cursorForPosition(QPoint(2, 2)).blockNumber();
        };
        const auto assertLockstep = [&]() {
            QCOMPARE(asciiFirst(), hexFirst());
            const qint64 expected = provider->windowStart()
                + qint64(hexFirst()) * provider->hexBytesPerRow();
            QCOMPARE(provider->visibleTopOffset(), expected);
        };

        const int initial = hexFirst();
        wheelLinesDown(hex->viewport());
        QTRY_VERIFY_WITH_TIMEOUT(hexFirst() > initial, 1000);
        assertLockstep();

        const int afterHexWheel = hexFirst();
        wheelLinesDown(ascii->viewport());
        QTRY_VERIFY_WITH_TIMEOUT(hexFirst() > afterHexWheel, 1000);
        assertLockstep();

        const int afterAsciiWheel = hexFirst();
        wheelPixelsDown(ascii->viewport(), hex->fontMetrics().lineSpacing() * 2);
        QTRY_VERIFY_WITH_TIMEOUT(hexFirst() > afterAsciiWheel, 1000);
        assertLockstep();

        // Exercise a real projection-boundary transition from the ASCII pane.
        // The first visible source row must advance continuously and both panes
        // must repaint the same row after the bounded projection is replaced.
        const int lines = int(hex->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT));
        const int onScreen = qMax(1, int(hex->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN)));
        const int lastFirst = qMax(0, lines - onScreen);
        hex->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                           static_cast<unsigned long>(lastFirst));
        QCoreApplication::processEvents();
        const qint64 boundaryTop = provider->visibleTopOffset();
        const qint64 oldWindowStart = provider->windowStart();
        wheelLinesDown(ascii->viewport());
        QTRY_VERIFY_WITH_TIMEOUT(provider->windowStart() > oldWindowStart, 1000);
        QVERIFY(provider->visibleTopOffset() > boundaryTop);
        QVERIFY(provider->visibleTopOffset() - boundaryTop
                <= qint64(QApplication::wheelScrollLines() + 1)
                    * provider->hexBytesPerRow());
        assertLockstep();

        window.close();
    }

    void windowedBinaryTextPresentationUsesTextLineNumberGutter()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("binary-as-text.bin"));
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write(QByteArrayLiteral("alpha\nbeta\ngamma\n")), qint64(17));
        source.close();

        QWidget window;
        window.resize(800, 500);
        auto *layout = new QVBoxLayout(&window);
        auto *provider = new WindowedReadOnlyProvider(&window);
        layout->addWidget(provider);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QString error;
        QVERIFY2(provider->openFile(path,
                                    WindowedReadOnlyProvider::Presentation::Hex,
                                    &error), qPrintable(error));
        QCOMPARE(provider->editor()->marginType(0), QsciScintilla::TextMargin);
        QVERIFY(provider->asciiPreview()->isVisibleTo(provider));

        provider->setPresentation(WindowedReadOnlyProvider::Presentation::Text);
        QCoreApplication::processEvents();
        QCOMPARE(provider->editor()->marginType(0), QsciScintilla::NumberMargin);
        QVERIFY(!provider->asciiPreview()->isVisibleTo(provider));
        QVERIFY(provider->editor()->marginWidth(0) > 0);

        provider->setPresentation(WindowedReadOnlyProvider::Presentation::Hex);
        QCoreApplication::processEvents();
        QCOMPARE(provider->editor()->marginType(0), QsciScintilla::TextMargin);
        QVERIFY(provider->asciiPreview()->isVisibleTo(provider));
        window.close();
    }

    void mmapS2RemainsReadOnlyAndStartsNoBackgroundLoad()
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
        window.setWindowTitle(QStringLiteral("Notepad Safe S2 Read-only Acceptance"));
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
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error),
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

        // Reopening the same S2 file must remain on the same read-only route and
        // must not submit a task or attach a full Scintilla document.
        QSignalSpy tasksAdded(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded);
        tab->shutdownL2BeforeClose();
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        tab->gotoByteOffset(dragged);
        QTest::qWait(500);
        QCOMPARE(nonCharacterCountTasks(tasksAdded), 0);
        QVERIFY(tab->isReadOnlyByPolicy());
        QCOMPARE(tab->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        auto *readOnly = tab->windowedReadOnlyProvider();
        QVERIFY(readOnly);
        const QByteArray before = readOnly->sourceBytes(dragged, 32);
        readOnly->editor()->setFocus();
        QTest::keyClicks(readOnly->editor(), QStringLiteral("EDIT_"));
        QCOMPARE(readOnly->sourceBytes(dragged, 32), before);
        QVERIFY(!tab->isModified());

        // The policy guard must also reject direct calls into the legacy save
        // entry point, so future callers cannot accidentally revive S2 patch
        // editing by bypassing the disabled UI actions.
        const QString forbiddenSave = temporary.filePath(
            QStringLiteral("s2-forbidden-save.log"));
        QString saveError;
        QVERIFY(!tab->saveL2As(forbiddenSave, &saveError));
        QVERIFY(saveError.contains(QStringLiteral("只读")));
        QVERIFY(!QFileInfo::exists(forbiddenSave));
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
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
        QTRY_VERIFY_WITH_TIMEOUT(
            provider->fileSize() - provider->visibleBottomOffset() < 8192, 3000);
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
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

    void s2ExpandedEncodingRemainsReadOnlyWithoutAllocation()
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
        QCOMPARE(tab->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        QVERIFY(tab->windowedReadOnlyProvider());
        QVERIFY(!tab->l2View()->isOpen());
        QVERIFY(stateChanged.count() >= 1);
        window.close();
    }

    void s2DoesNotQueueBackgroundEditLoad()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("no-background-edit-load.log"));
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
        QTest::qWait(500);
        QCOMPARE(nonCharacterCountTasks(added), 0);
        QCOMPARE(tab->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        tab->gotoByteOffset(0);
        QVERIFY(tab->windowedReadOnlyProvider()->editor()->text().startsWith(
            QStringLiteral("CANCEL_TEST_HEAD")));
        window.close();
    }

    void invalidEncodingDoesNotStartBackgroundLoader()
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
        QCOMPARE(tab->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        tab->gotoByteOffset(0);
        QVERIFY(tab->windowedReadOnlyProvider()->editor()->text().startsWith(
            QStringLiteral("VALID_PREVIEW_HEAD")));
        window.close();
    }

    void nearTwoGiBReadOnlyReachesHeadMiddleAndTailWithoutEditLoad()
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
        QVERIFY2(tab->openLargeFileL2(path, &error), qPrintable(error));
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
        QVERIFY2(tab->openLargeFileL2(path, &error,
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

    void realB2BinaryRemainsReadOnlyAndStartsNoBackgroundLoad()
    {
        const QString path = QDir(QStringLiteral(NOTEPADSAFE_SOURCE_DIR))
            .absoluteFilePath(QStringLiteral("../smalltestfile"));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        const qint64 size = QFileInfo(path).size();
        QVERIFY(size > 32LL * 1024 * 1024);
        QVERIFY(size < 2LL * 1024 * 1024 * 1024);

        QWidget window;
        window.setWindowTitle(QStringLiteral("Notepad Safe production B2 read-only"));
        window.resize(1280, 820);
        auto *layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *tab = new EditorTab(&window);
        layout->addWidget(tab);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QSignalSpy tasksAdded(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded);
        QString error;
        QVERIFY2(tab->openLargeFileL2(path, &error,
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
        QCOMPARE(nonCharacterCountTasks(tasksAdded), 0);
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

    void utf16S2RemainsReadOnlyAndUnchanged()
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
        QSignalSpy tasksAdded(&AsyncTaskManager::instance(), &AsyncTaskManager::taskAdded);
        QVERIFY2(tab->openLargeFileL2(sourcePath, &error), qPrintable(error));
        QVERIFY(tab->isL2Mode());
        QVERIFY(tab->isReadOnlyByPolicy());
        QCOMPARE(tab->largeDocumentRuntimeState(),
                 EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis);
        QTest::qWait(500);
        QCOMPARE(nonCharacterCountTasks(tasksAdded), 0);
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        const QByteArray before = provider->sourceBytes(0, 32);
        provider->editor()->setFocus();
        QTest::keyClicks(provider->editor(), QStringLiteral("EDIT_"));
        QCOMPARE(provider->sourceBytes(0, 32), before);
        QCOMPARE(QFileInfo(sourcePath).size(), sourceBytes);
        QVERIFY(!tab->isModified());
        window.close();
    }

    void s2DoesNotExportEditRecoveryData()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString sourcePath = temporary.filePath(QStringLiteral("readonly-base.log"));
        QSaveFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray block(1024 * 1024, 'R');
        for (int i = 0; i < 33; ++i)
            QCOMPARE(source.write(block), qint64(block.size()));
        QVERIFY(source.commit());

        EditorTab tab;
        QString error;
        QVERIFY2(tab.openLargeFileL2(sourcePath, &error), qPrintable(error));
        QVERIFY(tab.isL2Mode());
        QVERIFY(tab.isReadOnlyByPolicy());

        QByteArray payload;
        QVERIFY(!tab.exportRecoveryPayload(&payload, &error));
        QVERIFY(payload.isEmpty());
        QVERIFY(error.contains(QStringLiteral("只读分析")));

        QBuffer checkpoint;
        QVERIFY(checkpoint.open(QIODevice::WriteOnly));
        error.clear();
        QVERIFY(!tab.exportRecoveryCheckpoint(&checkpoint, &error));
        QCOMPARE(checkpoint.size(), qint64(0));
        QVERIFY(error.contains(QStringLiteral("只读分析")));
    }

    void legacyS2RecoveryIsRejected()
    {
        EditorTab tab;
        QString error;
        QByteArray legacyPayload;
        {
            QDataStream stream(&legacyPayload, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::LittleEndian);
            stream << QByteArray("NotepadSafe.LargeEditJournal.v1",
                                 int(sizeof("NotepadSafe.LargeEditJournal.v1")))
                   << quint32(1);
        }
        QVERIFY(!tab.restoreRecoveryPayload(legacyPayload, QString(), &error));
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(!tab.restoreRecoveryCheckpoint(QStringLiteral("disabled.ndraft"),
                                               QStringLiteral("disabled.log"),
                                               TextEncoding::Utf8, 0, &error));
        QVERIFY(error.contains(QStringLiteral("已禁用")));
    }
};

QTEST_MAIN(LargeReadOnlySmokeTest)
#include "smoke_large_readonly.moc"
