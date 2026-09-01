#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QTextBlock>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabWidget>

#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include "ByteClipboard.h"
#include "EditorTab.h"
#include "MainWindow.h"
#include "WindowedReadOnlyProvider.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <winioctl.h>
#include <io.h>
#endif

namespace {

QByteArray fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(8 * 1024 * 1024, Qt::Uninitialized);
    while (!file.atEnd()) {
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0)
            return {};
        if (read > 0)
            hash.addData(QByteArrayView(buffer.constData(), read));
    }
    return hash.result();
}

quint64 allocatedBytes(const QString &path)
{
#ifdef Q_OS_WIN
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(
        reinterpret_cast<LPCWSTR>(path.utf16()), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return 0;
    return (quint64(high) << 32) | low;
#else
    Q_UNUSED(path);
    return 0;
#endif
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

QPoint pointForHexPosition(QsciScintilla *hex, long position)
{
    return {
        int(hex->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION,
                               0UL, position)) + 1,
        int(hex->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION,
                               0UL, position))
            + hex->fontMetrics().height() / 2
    };
}

int projectionWindowTransitions(const QSignalSpy &spy, qint64 initialStart)
{
    int transitions = 0;
    qint64 previousStart = initialStart;
    for (const QList<QVariant> &arguments : spy) {
        if (arguments.isEmpty())
            continue;
        const qint64 start = arguments.constFirst().toLongLong();
        if (start != previousStart) {
            ++transitions;
            previousStart = start;
        }
    }
    return transitions;
}

} // namespace

class SmokeWindowedHexMouse final : public QObject {
    Q_OBJECT

    QString portableB2Path()
    {
        const QString path = fixtures_.filePath(QStringLiteral("hx-b2-40m.bin"));
        constexpr qint64 size = 40LL * 1024 * 1024;
        if (QFileInfo(path).size() == size)
            return path;
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate))
            return {};
#ifdef Q_OS_WIN
        DWORD returned = 0;
        if (!DeviceIoControl(reinterpret_cast<HANDLE>(
                                 _get_osfhandle(file.handle())),
                             FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                             &returned, nullptr)) {
            return {};
        }
#endif
        if (!file.resize(size))
            return {};
        QByteArray marker(1024 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < marker.size(); ++i)
            marker[i] = char((i * 29 + 17) & 0xff);
        if (!file.seek(size * 3 / 5)
            || file.write(marker) != marker.size()) {
            return {};
        }
        file.close();
        return path;
    }

    QString portableB3Path()
    {
        const QString path = fixtures_.filePath(QStringLiteral("hx-b3-sparse.bin"));
        constexpr qint64 size = 4LL * 1024 * 1024 * 1024
                                + 64LL * 1024 * 1024;
        if (QFileInfo(path).size() == size)
            return path;
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate))
            return {};
#ifdef Q_OS_WIN
        DWORD returned = 0;
        if (!DeviceIoControl(reinterpret_cast<HANDLE>(
                                 _get_osfhandle(file.handle())),
                             FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                             &returned, nullptr)) {
            return {};
        }
#endif
        if (!file.resize(size))
            return {};
        QByteArray marker(1024 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < marker.size(); ++i)
            marker[i] = char((i * 43 + 23) & 0xff);
        if (!file.seek(size * 3 / 4)
            || file.write(marker) != marker.size()) {
            return {};
        }
        file.close();
        return path;
    }

    void exercise(const QString &path, const char *caseId)
    {
        QVERIFY2(!path.isEmpty(), caseId);
        const QFileInfo originalInfo(path);
        const QByteArray originalHash = fileSha256(path);
        QVERIFY2(!originalHash.isEmpty(), caseId);

        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        const QByteArray previousDataDir = qgetenv("NOTEPADSAFE_DATADIR");
        const bool hadDataDir = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile.path().toUtf8());

        auto *window = new MainWindow;
        window->resize(1280, 820);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->openFilesFromExternal({path});
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
        EditorTab *tab = currentEditorTab(window);
        QTRY_VERIFY_WITH_TIMEOUT(tab && tab->windowedReadOnlyProvider(), 10000);
        auto *provider = tab->windowedReadOnlyProvider();
        auto *hex = provider->editor();
        auto *ascii = provider->asciiPreview();
        QVERIFY(hex && ascii && ascii->isVisibleTo(provider));
        QVERIFY(tab->isReadOnlyByPolicy());
        QCOMPARE(provider->fileSize(), originalInfo.size());
        QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(provider->hexBytesPerRow() >= 24, 3000);

        const qint64 probe = originalInfo.size() * 3 / 4;
        provider->goToOffset(probe);
        const qint64 firstWindowStart = provider->windowStart();
        const qint64 firstWindowEnd = provider->windowEnd();
        QVERIFY(firstWindowEnd > firstWindowStart);
        QVERIFY(firstWindowEnd < provider->fileSize());
        const int bytesPerRow = provider->hexBytesPerRow();
        const int rowsOnScreen = qMax(4, int(hex->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN)));
        const int lastFirst = qMax(0, hex->lines() - rowsOnScreen);
        hex->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                           static_cast<unsigned long>(qMax(0, lastFirst - 1)));
        QCoreApplication::processEvents();
        const int firstVisible = int(hex->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const int anchorLine = qBound(0, firstVisible + rowsOnScreen / 2,
                                      qMax(0, hex->lines() - 2));
        constexpr int byteColumn = 4;
        const long anchorPosition = hex->positionFromLineIndex(
            anchorLine, byteColumn * 3);
        const QPoint anchorPoint = pointForHexPosition(hex, anchorPosition);
        const qint64 forwardAnchor = firstWindowStart
            + qint64(anchorLine) * bytesPerRow + byteColumn;
        const QPoint bottomEdge(
            qBound(2, anchorPoint.x(), hex->viewport()->width() - 3),
            hex->viewport()->height() - 2);
        QSignalSpy windowReady(provider,
                               &WindowedReadOnlyProvider::windowReady);
        windowReady.clear();
        QTest::mousePress(hex->viewport(), Qt::LeftButton, Qt::NoModifier,
                          anchorPoint);
        for (int i = 0; i < 36; ++i) {
            QTest::mouseMove(hex->viewport(), bottomEdge, 25);
            QCoreApplication::processEvents();
        }
        QTRY_VERIFY2_WITH_TIMEOUT(provider->windowStart() > firstWindowStart,
                                  caseId, 5000);

        const int returnedFirst = int(hex->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const int returnedLine = qBound(0, returnedFirst + rowsOnScreen / 2,
                                        qMax(0, hex->lines() - 1));
        const long returnedPosition = hex->positionFromLineIndex(
            returnedLine, byteColumn * 3);
        const QPoint returnedPoint = pointForHexPosition(hex, returnedPosition);
        const qint64 forwardCaret = provider->windowStart()
            + qint64(returnedLine) * provider->hexBytesPerRow() + byteColumn;
        QTest::mouseMove(hex->viewport(), returnedPoint, 50);
        QCoreApplication::processEvents();
        QTest::mouseRelease(hex->viewport(), Qt::LeftButton, Qt::NoModifier,
                            returnedPoint);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(),
                                  forwardAnchor, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                                  forwardCaret, 3000);
        QCOMPARE(provider->selectionAnchorOffset(), forwardAnchor);
        QCOMPARE(provider->selectionCaretOffset(), forwardCaret);
        const int forwardWindowTransitions = projectionWindowTransitions(
            windowReady, firstWindowStart);
        QVERIFY(forwardWindowTransitions >= 1);
        QVERIFY(ascii->textCursor().hasSelection());
        QVERIFY(hex->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART)
                != hex->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));

        hex->setFocus(Qt::OtherFocusReason);
        tab->copy();
        const QByteArray expectedForward = provider->sourceBytes(
            forwardAnchor, qsizetype(forwardCaret - forwardAnchor));
        const QMimeData *copied = QApplication::clipboard()->mimeData();
        QVERIFY(copied);
        QCOMPARE(copied->data(QString::fromLatin1(ByteClipboard::RawBytesMime)),
                 expectedForward);

        const qint64 stableStart = provider->selectionStartOffset();
        const qint64 stableEnd = provider->selectionEndOffset();
        ascii->setFocus(Qt::OtherFocusReason);
        window->resize(1120, 760);
        QCoreApplication::processEvents();
        hex->setFocus(Qt::OtherFocusReason);
        QCOMPARE(provider->selectionStartOffset(), stableStart);
        QCOMPARE(provider->selectionEndOffset(), stableEnd);

        // Reverse drag is a real ASCII-surface gesture across the preceding
        // bounded projection and must retain direction in the source model.
        const qint64 reverseWindowStart = provider->windowStart();
        QVERIFY(reverseWindowStart > 0);
        hex->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 1UL);
        QCoreApplication::processEvents();
        QTextBlock anchorBlock = ascii->document()->findBlockByNumber(3);
        QVERIFY(anchorBlock.isValid() && anchorBlock.text().size() > byteColumn);
        QTextCursor anchorCursor(ascii->document());
        anchorCursor.setPosition(anchorBlock.position() + byteColumn);
        const QPoint reverseAnchorPoint = ascii->cursorRect(anchorCursor).center();
        const qint64 reverseAnchor = provider->windowStart()
            + qint64(anchorBlock.blockNumber()) * provider->hexBytesPerRow()
            + byteColumn;
        const QPoint topEdge(
            qBound(2, reverseAnchorPoint.x(), ascii->viewport()->width() - 3), 1);
        windowReady.clear();
        QTest::mousePress(ascii->viewport(), Qt::LeftButton, Qt::NoModifier,
                          reverseAnchorPoint);
        for (int i = 0; i < 36; ++i) {
            QTest::mouseMove(ascii->viewport(), topEdge, 25);
            QCoreApplication::processEvents();
        }
        QTRY_VERIFY2_WITH_TIMEOUT(provider->windowStart() < reverseWindowStart,
                                  caseId, 5000);
        QTest::mouseRelease(ascii->viewport(), Qt::LeftButton,
                            Qt::NoModifier, topEdge);
        QVERIFY(provider->selectionStartOffset() < reverseWindowStart);
        QCOMPARE(provider->selectionEndOffset(), reverseAnchor);
        QCOMPARE(provider->selectionAnchorOffset(), reverseAnchor);
        QCOMPARE(provider->selectionCaretOffset(),
                 provider->selectionStartOffset());
        const int reverseWindowTransitions = projectionWindowTransitions(
            windowReady, reverseWindowStart);
        QVERIFY(reverseWindowTransitions >= 1);
        const qint64 reverseStart = provider->selectionStartOffset();
        const qint64 reverseEnd = provider->selectionEndOffset();
        QVERIFY(provider->globalScrollBar()->value()
                < provider->globalScrollBar()->maximum());
        auto *positionStatus = window->findChild<QLabel *>(
            QStringLiteral("status.position"));
        QVERIFY(positionStatus);
        const QString expectedStatusOffset = QString::number(
            tab->currentByteOffset(), 16)
                .rightJustified(8, QLatin1Char('0')).toUpper();
        QTRY_VERIFY2_WITH_TIMEOUT(
            positionStatus->text().contains(expectedStatusOffset),
            qPrintable(QStringLiteral("status='%1' expected=%2")
                           .arg(positionStatus->text(), expectedStatusOffset)),
            1000);

        // Every mutation entrance stays fail-closed after the gesture. These
        // are real shortcut, right-click and drag/drop events; direct replace
        // calls exercise the command endpoint even without opening a dialog.
        QApplication::clipboard()->setText(QStringLiteral("AA BB"));
        for (const auto key : {Qt::Key_Delete, Qt::Key_Backspace})
            QTest::keyClick(hex, key);
        QTest::keyClick(hex, Qt::Key_X, Qt::ControlModifier);
        QTest::keyClick(hex, Qt::Key_V, Qt::ControlModifier);
        tab->replaceCurrent(QStringLiteral("MUTATE"));
        QCOMPARE(tab->replaceAll(QStringLiteral("00"), QStringLiteral("FF"),
                                 false, false, false), 0);

        bool menuSeen = false;
        bool mutationDisabled = true;
        QTimer::singleShot(80, window, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            menuSeen = true;
            for (QAction *action : menu->actions()) {
                const QString text = action->text();
                if (text.contains(QStringLiteral("Cut"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Paste"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("Delete"), Qt::CaseInsensitive)
                    || text.contains(QStringLiteral("剪切"))
                    || text.contains(QStringLiteral("粘贴"))
                    || text.contains(QStringLiteral("删除"))) {
                    mutationDisabled = mutationDisabled && !action->isEnabled();
                }
            }
            menu->close();
        });
        const QPoint menuPoint = hex->viewport()->rect().center();
        QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, menuPoint,
                                       hex->viewport()->mapToGlobal(menuPoint));
        QApplication::sendEvent(hex->viewport(), &contextEvent);
        QVERIFY(menuSeen);
        QVERIFY(mutationDisabled);

        QMimeData dropped;
        dropped.setText(QStringLiteral("MUST_NOT_MUTATE_WINDOWED_HEX"));
        QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &dropped,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hex->viewport(), &enter);
        QVERIFY(!enter.isAccepted());
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &dropped,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(hex->viewport(), &drop);
        QVERIFY(!drop.isAccepted());

        closeAllTabs(window);
        window->close();
        delete window;
        if (hadDataDir)
            qputenv("NOTEPADSAFE_DATADIR", previousDataDir);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");

        QCOMPARE(QFileInfo(path).size(), originalInfo.size());
        QCOMPARE(fileSha256(path), originalHash);
        qInfo().noquote()
            << caseId
            << "logical_bytes=" << originalInfo.size()
            << "allocated_bytes=" << allocatedBytes(path)
            << "sha256=" << originalHash.toHex()
            << "forward_range=[" << forwardAnchor << ',' << forwardCaret << ')'
            << "forward_window_ready=" << forwardWindowTransitions
            << "reverse_range=[" << reverseStart << ',' << reverseEnd << ')'
            << "reverse_window_ready=" << reverseWindowTransitions;
    }

private slots:
    void tc_hx_1a_b2_real_mouse_cross_window_readonly()
    {
        exercise(portableB2Path(), "HX-1A-B2");
    }

    void tc_hx_1a_b3_real_mouse_cross_window_readonly()
    {
        exercise(portableB3Path(), "HX-1A-B3");
    }

private:
    QTemporaryDir fixtures_;
};

QTEST_MAIN(SmokeWindowedHexMouse)
#include "smoke_windowed_hex_mouse.moc"
