#include "EditorTab.h"
#include "LargeFileSearch.h"
#include "MainWindow.h"
#include "RecoveryIssuePage.h"
#include "SearchResultsController.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "SessionManager.h"
#include "VirtualFileView.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <memory>

// 2B3 document presentation only. These tests intentionally compile against
// the old product. They do not alter routing, size authorities, recovery
// actions, search generation, byte ranges, task manager or task panel state.
// Proposed presentation anchors for the reviewed implementation:
// recoveryIssueDetailsToggle / searchResultsDetailsToggle (keyboard buttons).
namespace {

const QString kRawSearch = QStringLiteral(
    "RAW_SEARCH_FAILURE mmap Provider.cpp:714 [search.fixture.incomplete]");

struct ClipboardRestore {
    std::unique_ptr<QMimeData> previous = std::make_unique<QMimeData>();
    ClipboardRestore()
    {
        if (const QMimeData *mime = QApplication::clipboard()->mimeData()) {
            for (const QString &format : mime->formats())
                previous->setData(format, mime->data(format));
        }
    }
    ~ClipboardRestore()
    {
        // Restore every original MIME format, including on an early QtTest
        // assertion return. Never print the user's clipboard data.
        QApplication::clipboard()->setMimeData(previous.release());
    }
};

bool applyEvidenceTheme(MainWindow &window)
{
    const QString theme = QString::fromUtf8(qgetenv("NOTEPADSAFE_DOCUMENT_THEME"));
    if (theme.isEmpty()) return true;
    auto *action = window.findChild<QAction *>(QStringLiteral("view.appearance.") + theme);
    if (!action) return false;
    action->trigger();
    return action->isChecked();
}

bool saveDocumentEvidence(QWidget &widget, const QString &name)
{
    const QString theme = QString::fromUtf8(qgetenv("NOTEPADSAFE_DOCUMENT_THEME"));
    if (theme.isEmpty() && qgetenv("NOTEPADSAFE_DOCUMENT_CAPTURE") != QByteArray("1"))
        return true;
    QElapsedTimer deadline;
    deadline.start();
    QVector<QRect> previous;
    int stable = 0;
    while (deadline.elapsed() < 1500 && stable < 3) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(10);
        QVector<QRect> geometry{widget.geometry()};
        for (QWidget *child : widget.findChildren<QWidget *>()) {
            if (child->isVisibleTo(&widget))
                geometry << QRect(child->mapTo(&widget, QPoint()), child->size());
        }
        stable = geometry == previous ? stable + 1 : 0;
        previous = std::move(geometry);
    }
    const QString directory = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("document-product-language-evidence/native-")
        + (theme.isEmpty() ? QStringLiteral("platform") : theme));
    if (!QDir().mkpath(directory)) return false;
    const QString path = QDir(directory).filePath(name + QStringLiteral(".png"));
    const bool saved = widget.grab().save(path);
    bool readableRecoveryLabels = true;
    for (QLabel *label : widget.findChildren<QLabel *>()) {
        const QString name = label->objectName();
        if (!label->isVisibleTo(&widget)
            || (name != QStringLiteral("recoveryIssueSummary")
                && name != QStringLiteral("recoveryIssueCause")
                && name != QStringLiteral("recoveryIssueDetail"))) continue;
        const int requiredHeight = label->hasHeightForWidth()
            ? label->heightForWidth(label->width()) : label->sizeHint().height();
        if (label->height() < requiredHeight) {
            readableRecoveryLabels = false;
            qWarning() << "Recovery text is clipped:" << name << label->geometry()
                       << "requiredHeight=" << requiredHeight;
        }
    }
    qInfo().noquote() << "2B3 document screenshot (manual review required):" << path
        << "geometry=" << widget.geometry() << "stable=" << stable;
    const int lightness = QApplication::palette().color(QPalette::Base).lightness();
    const bool themeMatches = theme.isEmpty()
        || (theme == QStringLiteral("dark") ? lightness < 100 : lightness > 180);
    return saved && stable >= 3 && themeMatches && readableRecoveryLabels;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}

struct WindowFixture {
    QTemporaryDir directory;
    QByteArray oldData = qgetenv("NOTEPADSAFE_DATADIR");
    bool hadData = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
    std::unique_ptr<MainWindow> window;

    WindowFixture()
    {
        qputenv("NOTEPADSAFE_DATADIR", directory.filePath(QStringLiteral("profile")).toUtf8());
        window = std::make_unique<MainWindow>();
        applyEvidenceTheme(*window);
        window->resize(1400, 900);
        window->show();
        window->raise();
        window->activateWindow();
    }
    ~WindowFixture()
    {
        auto *tabs = window->findChild<QTabWidget *>();
        while (tabs && tabs->count() > 0) {
            QWidget *page = tabs->widget(0);
            if (auto *tab = qobject_cast<EditorTab *>(page)) tab->shutdownL2BeforeClose();
            tabs->removeTab(0);
            delete page;
        }
        window.reset();
        if (hadData) qputenv("NOTEPADSAFE_DATADIR", oldData);
        else qunsetenv("NOTEPADSAFE_DATADIR");
    }
    EditorTab *current() const
    {
        auto *tabs = window->findChild<QTabWidget *>();
        return tabs ? qobject_cast<EditorTab *>(tabs->currentWidget()) : nullptr;
    }
};

QString fieldText(QWidget *widget)
{
    if (!widget) return {};
    QString text = widget->toolTip() + QLatin1Char('\n')
        + widget->accessibleName() + QLatin1Char('\n') + widget->accessibleDescription();
    if (auto *label = qobject_cast<QLabel *>(widget)) text += QLatin1Char('\n') + label->text();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) text += QLatin1Char('\n') + button->text();
    if (auto *edit = qobject_cast<QTextEdit *>(widget)) text += QLatin1Char('\n') + edit->toPlainText();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) text += QLatin1Char('\n') + edit->toPlainText();
    return text;
}

QString visibleText(QWidget *root)
{
    QString text = fieldText(root);
    for (QWidget *widget : root->findChildren<QWidget *>()) {
        if (widget->isVisibleTo(root)) text += QLatin1Char('\n') + fieldText(widget);
    }
    return text;
}

bool hasEngineeringWords(QString text, const QString &userFileName = {})
{
    // Only an exact fixture-owned user field may be excluded. Never delete
    // arbitrary Provider/encoding/language words from product prose.
    if (!userFileName.isEmpty()) text.replace(userFileName, QStringLiteral("用户文件"));
    static const QRegularExpression words(
        QStringLiteral("\\b(?:S[123]|B[123]|mmap|overlay|Scintilla|Provider|Adapter|Binary|Hex|Text)\\b|低价值|字节窗口"));
    return words.match(text).hasMatch();
}

QString accessibleDescription(QWidget *widget)
{
    auto *accessible = QAccessible::queryAccessibleInterface(widget);
    return accessible ? accessible->text(QAccessible::Description) : QString();
}

QVector<LargeFileSearchMatch> twoMatches()
{
    QVector<LargeFileSearchMatch> matches;
    for (int index = 0; index < 2; ++index) {
        LargeFileSearchMatch match;
        match.byteOffset = index * 256;
        match.length = 3;
        match.line = index;
        match.col = 0;
        match.contextByteOffset = match.byteOffset;
        match.lineContext = QByteArray("hit preview");
        matches.append(match);
    }
    return matches;
}

SearchResultsModel *makeResults(QObject *parent, bool incomplete = false)
{
    auto *model = new SearchResultsModel(parent);
    model->setBytePositionMode(true);
    model->setResults(twoMatches(), {}, 0);
    if (incomplete) model->setIncompleteReason(kRawSearch);
    return model;
}

} // namespace

class DocumentProductLanguageSmoke final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir themeProfile_;
    std::unique_ptr<MainWindow> themeHost_;
    QByteArray previousData_;
    bool hadData_ = false;
    QPalette previousPalette_;
    QString previousStyleSheet_;

private slots:
    void initTestCase()
    {
        qputenv("QTEST_FUNCTION_TIMEOUT", QByteArray("30000"));
        qRegisterMetaType<SearchResultsModel *>("SearchResultsModel*");
        const QByteArray theme = qgetenv("NOTEPADSAFE_DOCUMENT_THEME");
        QVERIFY(theme.isEmpty() || theme == "light" || theme == "dark");
        previousPalette_ = QApplication::palette();
        previousStyleSheet_ = qApp->styleSheet();
        hadData_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        previousData_ = qgetenv("NOTEPADSAFE_DATADIR");
        QVERIFY(themeProfile_.isValid());
        qputenv("NOTEPADSAFE_DATADIR", themeProfile_.path().toUtf8());
        if (!theme.isEmpty()) {
            QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
            QVERIFY(!QApplication::style()->objectName().contains(QStringLiteral("fusion"), Qt::CaseInsensitive));
            themeHost_ = std::make_unique<MainWindow>();
            QVERIFY(applyEvidenceTheme(*themeHost_));
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            const QSettings saved(QDir(themeProfile_.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
            QCOMPARE(saved.value(QStringLiteral("appearanceMode")).toString(), QString::fromUtf8(theme));
            const int lightness = QApplication::palette().color(QPalette::Base).lightness();
            QVERIFY(theme == "dark" ? lightness < 100 : lightness > 180);
        }
        qInfo() << "document evidence theme=" << theme << "native style=" << QApplication::style()->objectName();
    }

    void cleanupTestCase()
    {
        themeHost_.reset();
        QApplication::setPalette(previousPalette_);
        qApp->setStyleSheet(previousStyleSheet_);
        if (hadData_) qputenv("NOTEPADSAFE_DATADIR", previousData_);
        else qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void savedNonUtf8DirtySizeExplainsBaselineAndKeepsCharacterCount()
    {
        WindowFixture fixture;
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString path = fixture.directory.filePath(QStringLiteral("Provider.cpp"));
        const QByteArray bytes = QByteArray::fromHex("636166e90d0a");
        QVERIFY(writeBytes(path, bytes));
        const auto hash = fileHash(path);
        fixture.window->openFilesFromExternal({path});
        auto *tab = fixture.current();
        QVERIFY(tab);
        auto *encoding = fixture.window->findChild<QAction *>(QStringLiteral("encoding.display.windows1252"));
        QVERIFY(encoding && encoding->isEnabled());
        encoding->trigger();
        QCOMPARE(tab->encoding(), TextEncoding::Windows1252);
        tab->editor()->append(QStringLiteral("新增字符"));
        QVERIFY(tab->isModified());
        QCOMPARE(tab->contentSizeBytes(), qint64(bytes.size()));
        QCOMPARE(fileHash(path), hash);
        const qint64 characters = tab->editor()->text().toUcs4().size();
        QCOMPARE(tab->contentCharacterCount(), characters);
        auto *size = fixture.window->findChild<QLabel *>(QStringLiteral("status.size"));
        auto *count = fixture.window->findChild<QLabel *>(QStringLiteral("status.characters"));
        QVERIFY(size && count);
        QTRY_VERIFY_WITH_TIMEOUT(count->text().contains(QLocale().toString(characters)), 2000);
        QVERIFY(size->text().contains(QStringLiteral("6 B")));
        auto *tabs = fixture.window->findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->indexOf(tab) >= 0);
        QVERIFY(tabs->tabText(tabs->indexOf(tab)).contains(QStringLiteral("Provider.cpp")));
        QVERIFY2(size->toolTip().contains(QStringLiteral("未保存"))
                     && (size->toolTip().contains(QStringLiteral("打开"))
                         || size->toolTip().contains(QStringLiteral("上次保存"))),
                 "saved dirty text size must explain its open/save baseline, not current disk or UTF-8 payload");
        QVERIFY(accessibleDescription(size).contains(QStringLiteral("未保存")));
        QVERIFY(!hasEngineeringWords(fieldText(size) + fieldText(count)));
        QVERIFY(saveDocumentEvidence(*fixture.window, QStringLiteral("saved-dirty-status")));
    }

    void editableBinaryInsertionReportsCurrentBytesWithoutChangingSource()
    {
        WindowFixture fixture;
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString path = fixture.directory.filePath(QStringLiteral("editable.bin"));
        QVERIFY(writeBytes(path, QByteArray::fromHex("00112233")));
        const auto hash = fileHash(path);
        fixture.window->openFilesFromExternal({path});
        auto *tab = fixture.current();
        QVERIFY(tab);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View() && tab->l2View()->isOpen(), 5000);
        auto *insert = fixture.window->findChild<QAction *>(QStringLiteral("edit.hexInsertMode"));
        QVERIFY(insert && insert->isEnabled());
        insert->trigger();
        QVERIFY(!tab->hexOverwriteMode());
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_End, Qt::ControlModifier);
        QTest::keyClicks(tab->editor(), QStringLiteral("A"));
        QCOMPARE(tab->contentSizeBytes(), qint64(5));
        QCOMPARE(tab->contentCharacterCount(), qint64(5));
        QVERIFY(tab->isModified());
        QCOMPARE(fileHash(path), hash);
        auto *size = fixture.window->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(size);
        QTRY_VERIFY_WITH_TIMEOUT(size->text().contains(QStringLiteral("5 B")), 2000);
        QVERIFY2(size->toolTip().contains(QStringLiteral("当前"))
                     && !size->toolTip().contains(QStringLiteral("源文件实际")),
                 "editable Binary size must describe the edited byte content");
        QVERIFY(!hasEngineeringWords(fieldText(size)));
        QVERIFY(saveDocumentEvidence(*fixture.window, QStringLiteral("editable-byte-status")));
    }

    void inMemoryTextSizeIsExplicitlyUtf8_data()
    {
        QTest::addColumn<bool>("secure");
        QTest::newRow("untitled") << false;
        QTest::newRow("secure-text-surface") << true;
    }

    void inMemoryTextSizeIsExplicitlyUtf8()
    {
        QFETCH(bool, secure);
        WindowFixture fixture;
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString seed = fixture.directory.filePath(QStringLiteral("seed.txt"));
        QVERIFY(writeBytes(seed, QByteArray("seed")));
        fixture.window->openFilesFromExternal({seed});
        auto *tab = fixture.current();
        QVERIFY(tab);
        const QString body = QStringLiteral("内存内容é");
        // Public surface fixtures, not a credential/persistence acceptance:
        // the real MainWindow observes the same tab metadata signals.
        if (secure) tab->initializeSecureDocument(QUuid::createUuid(), QStringLiteral("内存文档"), body);
        else {
            tab->convertToPlainDocument({});
            tab->editor()->setText(body);
        }
        QCOMPARE(tab->contentSizeBytes(), qint64(body.toUtf8().size()));
        QCOMPARE(tab->contentCharacterCount(), qint64(body.toUcs4().size()));
        auto *size = fixture.window->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(size);
        QTRY_VERIFY_WITH_TIMEOUT(size->text().contains(QString::number(body.toUtf8().size()) + QStringLiteral(" B")), 2000);
        QVERIFY2(size->toolTip().contains(QStringLiteral("UTF-8"))
                     && size->toolTip().contains(QStringLiteral("内存")),
                 "memory text size must not claim to be a saved source-file size");
        QVERIFY(accessibleDescription(size).contains(QStringLiteral("UTF-8")));
    }

    void recoveryKnownCauseSurvivesCollapsedDiagnostics_data()
    {
        QTest::addColumn<bool>("original");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("detail");
        QTest::addColumn<QString>("cause");
        QTest::newRow("missing-original") << true << QStringLiteral("[recovery.original.missing]")
            << QStringLiteral("原文件不存在。请检查文件的位置后重试打开。") << QStringLiteral("找不到可打开");
        QTest::newRow("missing-draft") << false << QStringLiteral("[recovery.draft.missing]")
            << QStringLiteral("未找到此项未保存内容的恢复稿，不能用磁盘原文件替代。原文件未被修改。") << QStringLiteral("未找到");
        QTest::newRow("invalid-draft") << false << QStringLiteral("[recovery.draft.invalid]")
            << QStringLiteral("恢复稿格式无效，无法恢复未保存内容。 RAW_RECOVERY_INVALID DraftStore.cpp:714 Provider mmap invalid header")
            << QStringLiteral("格式无效");
        QTest::newRow("source-changed") << false << QStringLiteral("[recovery.source.changed]")
            << QStringLiteral("原文件已经变化，未保存内容尚未恢复。 RAW_SOURCE_CHANGED VirtualFileView.cpp:714 mmap Provider baseline mismatch")
            << QStringLiteral("变化");
        QTest::newRow("currently-secure") << true << QStringLiteral("[recovery.original.secure]")
            << QStringLiteral("此路径当前是安全文件，不能按旧的普通文件记录恢复。请在窗口内用文件→打开；本次不会解锁。") << QStringLiteral("安全文件");
        QTest::newRow("unknown-diagnostic") << false << QStringLiteral("[recovery.fixture.unknown]")
            << QStringLiteral("RAW_RECOVERY_UNKNOWN Provider.cpp:714 mmap backend exception")
            << QStringLiteral("尚未恢复");
    }

    void recoveryKnownCauseSurvivesCollapsedDiagnostics()
    {
        QFETCH(bool, original);
        QFETCH(QString, code);
        QFETCH(QString, detail);
        QFETCH(QString, cause);
        SessionTab state;
        state.filePath = QStringLiteral("Provider.cpp"); // User filename is not banned product prose.
        state.modified = !original;
        RecoveryIssuePage page(state, original ? RecoveryIssuePage::Kind::OriginalFile
                                               : RecoveryIssuePage::Kind::UnsavedDraft, code, detail);
        page.resize(980, 700);
        page.show();
        page.raise();
        page.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        const QString initial = visibleText(&page);
        QVERIFY(initial.contains(QStringLiteral("Provider.cpp")) && initial.contains(cause));
        QVERIFY(initial.contains(QStringLiteral("原文件")) && initial.contains(QStringLiteral("重试")));
        QVERIFY2(!initial.contains(code), "raw recovery code is visible before the user asks for technical details");
        QVERIFY(!hasEngineeringWords(initial, QStringLiteral("Provider.cpp")));
        const QString evidenceName = QStringLiteral("recovery-") + QString::fromLatin1(QTest::currentDataTag());
        QVERIFY(saveDocumentEvidence(page, evidenceName + QStringLiteral("-default")));
        auto *toggle = page.findChild<QAbstractButton *>(QStringLiteral("recoveryIssueDetailsToggle"));
        QVERIFY(toggle && toggle->isVisible());
        toggle->setFocus();
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(visibleText(&page).contains(code));
        QVERIFY(visibleText(&page).contains(detail));
        QVERIFY(saveDocumentEvidence(page, evidenceName + QStringLiteral("-expanded")));
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(!visibleText(&page).contains(code));
        QSignalSpy retry(&page, &RecoveryIssuePage::retryRequested);
        QSignalSpy remove(&page, &RecoveryIssuePage::originalRemovalRequested);
        QSignalSpy discard(&page, &RecoveryIssuePage::discardConfirmed);
        auto *retryButton = page.findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        QVERIFY(retryButton);
        QTest::mouseClick(retryButton, Qt::LeftButton);
        QCOMPARE(retry.count(), 1);
        QCOMPARE(remove.count(), 0);
        QCOMPARE(discard.count(), 0);
        QCOMPARE(page.captureSessionTab().filePath, state.filePath);
        QCOMPARE(page.captureSessionTab().modified, state.modified);
    }

    void recoveryLongDiagnosticRemainsReadableAndCopyableInNarrowPage()
    {
        const QString firstMarker = QStringLiteral("RAW_RECOVERY_LONG_BEGIN");
        const QString lastMarker = QStringLiteral("RAW_RECOVERY_LONG_END");
        QString raw = firstMarker + QLatin1Char('\n');
        for (int line = 0; line < 80; ++line)
            raw += QStringLiteral("%1: Provider.cpp:714 原始诊断 <b>原样保留</b>；长恢复说明不可丢失或省略。\n").arg(line);
        raw += lastMarker;
        SessionTab state;
        state.filePath = QStringLiteral("窄窗口恢复验证.txt");
        state.modified = true;
        const QString code = QStringLiteral("[recovery.draft.invalid]");
        RecoveryIssuePage page(state, RecoveryIssuePage::Kind::UnsavedDraft, code, raw);
        QSignalSpy retry(&page, &RecoveryIssuePage::retryRequested);
        QSignalSpy discard(&page, &RecoveryIssuePage::discardConfirmed);
        QSignalSpy remove(&page, &RecoveryIssuePage::originalRemovalRequested);
        QVERIFY(retry.isValid() && discard.isValid() && remove.isValid());
        page.resize(480, 440);
        page.show();
        page.raise();
        page.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QCOMPARE(page.size(), QSize(480, 440));
        auto *toggle = page.findChild<QAbstractButton *>(QStringLiteral("recoveryIssueDetailsToggle"));
        auto *detail = page.findChild<QLabel *>(QStringLiteral("recoveryIssueDetail"));
        auto *detailScroll = page.findChild<QScrollArea *>(QStringLiteral("recoveryIssueDetailScroll"));
        auto *pageScroll = page.findChild<QScrollArea *>(QStringLiteral("recoveryIssuePageScroll"));
        auto *retryButton = page.findChild<QPushButton *>(QStringLiteral("recoveryIssueRetry"));
        auto *discardButton = page.findChild<QPushButton *>(QStringLiteral("recoveryIssueDiscard"));
        QVERIFY(toggle && detail && detailScroll && pageScroll && retryButton && discardButton);
        QVERIFY(!toggle->isChecked() && !detail->isVisibleTo(&page) && !detailScroll->isVisibleTo(&page));
        QCOMPARE(detail->text(), raw);
        QVERIFY(!visibleText(&page).contains(firstMarker) && !visibleText(&page).contains(code));
        QVERIFY(saveDocumentEvidence(page, QStringLiteral("recovery-narrow-long-default")));

        toggle->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(toggle->hasFocus(), 1000);
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(detail->isVisibleTo(&page) && detailScroll->isVisibleTo(&page), 1000);
        QCOMPARE(detail->textFormat(), Qt::PlainText);
        QCOMPARE(detail->text(), raw);
        QVERIFY(detail->hasHeightForWidth());
        QTRY_VERIFY_WITH_TIMEOUT(detail->height() >= detail->heightForWidth(detail->width()), 1500);
        QVERIFY(detailScroll->viewport()->height() > 0 && detailScroll->viewport()->height() <= 180);
        QTRY_VERIFY_WITH_TIMEOUT(detailScroll->verticalScrollBar()->maximum() > 0, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(pageScroll->verticalScrollBar()->maximum() > 0, 1500);
        QCOMPARE(pageScroll->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(page.size(), QSize(480, 440));
        QVERIFY(saveDocumentEvidence(page, QStringLiteral("recovery-narrow-long-expanded")));

        {
            ClipboardRestore restoreClipboard;
            detail->setFocus(Qt::TabFocusReason);
            QTRY_VERIFY_WITH_TIMEOUT(detail->hasFocus(), 1000);
            QTest::keyClick(detail, Qt::Key_A, Qt::ControlModifier);
            // QTextCursor exposes paragraph separators; clipboard plain text
            // must still reproduce the original diagnostic's newlines.
            QString selected = detail->selectedText();
            selected.replace(QChar(0x2029), QLatin1Char('\n'));
            QCOMPARE(selected, raw);
            QTest::keyClick(detail, Qt::Key_C, Qt::ControlModifier);
            QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), raw, 1000);
        }

        auto *detailBar = detailScroll->verticalScrollBar();
        detailBar->setFocus(Qt::TabFocusReason);
        QTest::keyClick(detailBar, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(detailBar->value(), detailBar->maximum(), 1000);
        const int bottomInViewport = detail->mapTo(detailScroll->viewport(),
            QPoint(0, detail->height() - 1)).y();
        QVERIFY(bottomInViewport >= 0 && bottomInViewport < detailScroll->viewport()->height());
        QVERIFY(detail->text().endsWith(lastMarker));
        auto *pageBar = pageScroll->verticalScrollBar();
        pageBar->setFocus(Qt::TabFocusReason);
        QTest::keyClick(pageBar, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(pageBar->value(), pageBar->maximum(), 1000);
        for (QPushButton *button : {retryButton, discardButton}) {
            button->setFocus(Qt::TabFocusReason);
            QTRY_VERIFY_WITH_TIMEOUT(button->hasFocus(), 1000);
            const QRect visibleRect(button->mapTo(pageScroll->viewport(), QPoint()), button->size());
            QVERIFY(button->isVisibleTo(&page));
            QVERIFY(pageScroll->viewport()->rect().contains(visibleRect));
        }
        QVERIFY(saveDocumentEvidence(page, QStringLiteral("recovery-narrow-long-bottom")));
        toggle->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(toggle->hasFocus(), 1000);
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!detail->isVisibleTo(&page) && !detailScroll->isVisibleTo(&page), 1000);
        QVERIFY(!visibleText(&page).contains(firstMarker) && !visibleText(&page).contains(code));
        QCOMPARE(retry.count(), 0);
        QCOMPARE(discard.count(), 0);
        QCOMPARE(remove.count(), 0);
        QCOMPARE(page.captureSessionTab().filePath, state.filePath);
        QCOMPARE(page.captureSessionTab().modified, state.modified);
        QCOMPARE(detail->text(), raw);
        QVERIFY(saveDocumentEvidence(page, QStringLiteral("recovery-narrow-long-collapsed")));
    }

    void searchTerminalSummaryPreservesCountsAndAccessibility_data()
    {
        QTest::addColumn<QString>("state");
        for (const char *state : {"running", "complete", "cancelled", "incomplete", "changed", "closed"})
            QTest::newRow(state) << QString::fromLatin1(state);
    }

    void searchTerminalSummaryPreservesCountsAndAccessibility()
    {
        QFETCH(QString, state);
        SearchResultsController controller;
        QMainWindow host;
        auto *dock = new SearchResultsDock(controller, &host);
        host.addDockWidget(Qt::BottomDockWidgetArea, dock);
        host.resize(1100, 720);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const SearchResultTarget target{QUuid::createUuid(), 17, QStringLiteral("Provider.cpp")};
        if (state == QLatin1String("running")) {
            dock->beginSingleFileResults(QStringLiteral("hit"), target.displayName, target, TextEncoding::Utf8, true);
            dock->appendSingleFilePreview(target, twoMatches(), TextEncoding::Utf8, true);
        } else if (state == QLatin1String("cancelled")) {
            dock->beginMultiFileResults(QStringLiteral("hit"), 1);
            dock->appendMultiFileResults(target, makeResults(dock));
            dock->finishMultiFileResults(true);
        } else {
            dock->setResultsModel(makeResults(dock, state == QLatin1String("incomplete")),
                                  QStringLiteral("hit"), target.displayName, target);
            if (state == QLatin1String("changed")) dock->invalidateResultsFor(target.documentId);
            if (state == QLatin1String("closed")) dock->closeResultsFor(target.documentId);
        }
        dock->show();
        auto *summary = dock->findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(summary && summary->isVisible());
        const QString text = summary->text();
        if (state == QLatin1String("running")) QVERIFY(!text.contains(QStringLiteral("完成")));
        else QVERIFY2(text.contains(QStringLiteral("2")), qPrintable(text));
        if (state == QLatin1String("complete")) QVERIFY(text.contains(QStringLiteral("预览片段")));
        if (state == QLatin1String("cancelled")) QVERIFY(text.contains(QStringLiteral("停止")) || text.contains(QStringLiteral("取消")));
        if (state == QLatin1String("incomplete")) QVERIFY(text.contains(QStringLiteral("至少")));
        if (state == QLatin1String("changed")) QVERIFY(text.contains(QStringLiteral("变化")) || text.contains(QStringLiteral("修改")));
        if (state == QLatin1String("closed")) QVERIFY(text.contains(QStringLiteral("关闭")));
        QVERIFY2(!hasEngineeringWords(text, target.displayName), qPrintable(text));
        QVERIFY(!text.contains(kRawSearch));
        QCOMPARE(summary->accessibleName(), QStringLiteral("查找状态"));
        QVERIFY2(accessibleDescription(summary).contains(text), "screen-reader description must track the current summary without reselection");
        QVERIFY(saveDocumentEvidence(host, QStringLiteral("search-") + state));
    }

    void searchRawReasonIsOnlyInKeyboardTechnicalDetails_data()
    {
        QTest::addColumn<bool>("multi");
        QTest::newRow("single") << false;
        QTest::newRow("all-open") << true;
    }

    void searchRawReasonIsOnlyInKeyboardTechnicalDetails()
    {
        QFETCH(bool, multi);
        SearchResultsController controller;
        QMainWindow host;
        auto *dock = new SearchResultsDock(controller, &host);
        host.addDockWidget(Qt::BottomDockWidgetArea, dock);
        host.resize(1100, 720);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        const SearchResultTarget target{QUuid::createUuid(), 5, QStringLiteral("Provider.cpp")};
        if (multi) {
            dock->beginMultiFileResults(QStringLiteral("hit"), 1);
            dock->appendMultiFileResults(target, makeResults(dock, true));
            dock->finishMultiFileResults();
        } else dock->setResultsModel(makeResults(dock, true), QStringLiteral("hit"), target.displayName, target);
        dock->show();
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(tree && tree->model());
        QString defaultFields = visibleText(dock);
        if (multi) defaultFields += tree->model()->index(0, 0).data(Qt::ToolTipRole).toString();
        QVERIFY2(!defaultFields.contains(kRawSearch), "raw incomplete reason leaked through summary, tooltip or accessibility");
        QVERIFY(!hasEngineeringWords(defaultFields, target.displayName));
        const QString evidenceName = multi ? QStringLiteral("search-all-open") : QStringLiteral("search-single");
        QVERIFY(saveDocumentEvidence(host, evidenceName + QStringLiteral("-default")));
        auto *toggle = dock->findChild<QAbstractButton *>(QStringLiteral("searchResultsDetailsToggle"));
        QVERIFY(toggle && toggle->isVisible());
        toggle->setFocus();
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY2(visibleText(dock).contains(kRawSearch), "expanded visible details must retain the original diagnostic");
        QVERIFY(saveDocumentEvidence(host, evidenceName + QStringLiteral("-expanded")));
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(!visibleText(dock).contains(kRawSearch));
        QCOMPARE(multi ? tree->model()->index(0, SearchResultsModel::COL_MATCHCOUNT).data().toLongLong()
                       : qobject_cast<SearchResultsModel *>(tree->model())->totalMatches(), qint64(2));
    }

    void externalFailureDialogButtonsAreChineseAndStillKeyboardOperable()
    {
        WindowFixture fixture;
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        QStringList paths;
        for (int index = 0; index < 5; ++index) {
            const QString path = fixture.directory.filePath(QStringLiteral("Provider-%1.nsafe").arg(index));
            QVERIFY(writeBytes(path, QByteArray("protected fixture")));
            paths << path;
        }
        fixture.window->openFilesFromExternal(paths);
        auto *review = fixture.window->findChild<QToolButton *>(QStringLiteral("externalOpenFailuresButton"));
        QVERIFY(review && review->isVisible());
        QStringList captions;
        bool expanded = false;
        bool timedOut = false;
        int stage = 0;
        QElapsedTimer deadline;
        deadline.start();
        QTimer guard;
        guard.setInterval(20);
        connect(&guard, &QTimer::timeout, &guard, [&]() {
            auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (deadline.elapsed() > 4000) {
                timedOut = true;
                if (modal) modal->reject();
                return;
            }
            auto *box = qobject_cast<QMessageBox *>(modal);
            if (!box) return;
            QAbstractButton *details = nullptr;
            for (QAbstractButton *button : box->buttons()) {
                captions << button->text();
                if (box->buttonRole(button) == QMessageBox::ActionRole) details = button;
            }
            if (stage++ == 0 && details) {
                details->setFocus();
                QTest::keyClick(details, Qt::Key_Space);
                return;
            }
            for (QTextEdit *edit : box->findChildren<QTextEdit *>())
                expanded = expanded || (edit->isVisible() && edit->isReadOnly()
                    && edit->toPlainText().contains(paths.last()));
            box->accept();
        });
        guard.start();
        review->setFocus();
        QTest::keyClick(review, Qt::Key_Space);
        guard.stop();
        QVERIFY2(!timedOut, "unexpected modal or unavailable details control exceeded the fixture deadline");
        QVERIFY(expanded);
        const QString labels = captions.join(QLatin1Char('\n'));
        QVERIFY2(!labels.contains(QStringLiteral("OK")) && !labels.contains(QStringLiteral("Show Details"))
                     && !labels.contains(QStringLiteral("Hide Details")), qPrintable(labels));
        QVERIFY(labels.contains(QStringLiteral("确定")) && labels.contains(QStringLiteral("详情")));
    }

    void realLargeSearchCancellationKeepsStoppedReason_data()
    {
        QTest::addColumn<bool>("synchronous");
        QTest::newRow("sync") << true;
        QTest::newRow("async") << false;
    }

    void realLargeSearchCancellationKeepsStoppedReason()
    {
        QFETCH(bool, synchronous);
        WindowFixture fixture;
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window.get()));
        const QString path = fixture.directory.filePath(QStringLiteral("cancel-product.bin"));
        const QByteArray marker("CANCEL_PRODUCT_MARKER");
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.resize(256LL * 1024 * 1024));
        QVERIFY(source.seek(4096));
        QCOMPARE(source.write(marker), qint64(marker.size()));
        source.close();
        const QByteArray originalHash = fileHash(path);
        QVERIFY(!originalHash.isEmpty());
        fixture.window->openFilesFromExternal({path});
        EditorTab *tab = fixture.current();
        QVERIFY(tab && tab->isBinaryDocument() && tab->isReadOnlyByPolicy());
        const auto state = tab->documentState();
        const QString query = QString::fromLatin1(marker.toHex(' '));

        // A real first-hit navigation prepares the lazy search source and
        // returns near BOF. Its callback is posted only after releaseWorker;
        // this is not a substitute test source or a Manager-Running guess.
        bool prepared = false;
        bool found = false;
        QString preparationError;
        QVERIFY(tab->findNextL2Async(query, false, true, false, true, true, true,
            fixture.window.get(), [&](bool matched, QString error) {
                found = matched;
                preparationError = std::move(error);
                prepared = true;
            }));
        QTRY_VERIFY_WITH_TIMEOUT(prepared, 10000);
        QVERIFY2(found && preparationError.isEmpty(), qPrintable(preparationError));
        // A 256 MiB read-only binary opens in the windowed read-only source.
        LargeFileSearch *engine = tab->searchEngine();
        QVERIFY(engine);

        bool cancelledAfterDomainProgress = false;
        qint64 observedDone = -1;
        qint64 observedTotal = -1;
        const auto progressConnection = connect(engine, &LargeFileSearch::progress,
            fixture.window.get(), [&](qint64 done, qint64 total) {
                if (cancelledAfterDomainProgress || done <= 0 || done >= total) return;
                // Only the domain worker publishes this progress. At this
                // point its release guard exists; never cancel/join Queued.
                cancelledAfterDomainProgress = true;
                observedDone = done;
                observedTotal = total;
                tab->cancelFindAllL2();
            });
        QSignalSpy failures(engine, &LargeFileSearch::failed);
        QSignalSpy finished(tab, &EditorTab::findAllL2Finished);
        SearchResultsModel *model = nullptr;
        if (synchronous) {
            model = tab->findAllL2(query, false, true, false, fixture.window.get(), true);
        } else {
            QVERIFY(tab->findAllL2Async(query, false, true, false, fixture.window.get(), true));
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
            model = qvariant_cast<SearchResultsModel *>(finished.constFirst().at(0));
        }
        disconnect(progressConnection);
        // Reaching a real terminal callback makes this join safe even if a
        // very fast worker completed before our cancellation. Such a run is
        // an explicit fixture failure below, never claimed as a cancel pass.
        engine->cancelAndWait();
        QCOMPARE(engine->lastTaskId(), qint64(0));
        QVERIFY(cancelledAfterDomainProgress);
        QVERIFY(observedDone > 0 && observedDone < observedTotal);
        QVERIFY(model);
        QVERIFY(model->isIncomplete());
        QCOMPARE(model->totalMatches(), qint64(1));
        QCOMPARE(failures.count(), 1);
        const QString raw = failures.constFirst().at(0).toString();
        QCOMPARE(raw, QStringLiteral("已取消。"));
        QCOMPARE(model->incompleteReason(), raw);
        QCOMPARE(tab->lastSearchError(), raw);
        const QString immediate = fixture.window->statusBar()->currentMessage();
        QCOMPARE(fileHash(path), originalHash);
        QCOMPARE(tab->documentState().id, state.id);
        QCOMPARE(tab->documentState().revision, state.revision);
        QCOMPARE(tab->documentState().capabilities, state.capabilities);
        QVERIFY(!tab->isModified());

        SearchResultsController controller;
        SearchResultsDock dock(controller);
        dock.resize(1100, 380);
        dock.setResultsModel(model, query, tab->fileName(),
            {state.id, state.revision, tab->fileName()});
        dock.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dock));
        auto *summary = dock.findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
        QVERIFY(summary);
        const auto stopped = [](const QString &text) {
            return text.contains(QStringLiteral("停止")) || text.contains(QStringLiteral("取消"));
        };
        QVERIFY2(stopped(immediate), qPrintable(immediate));
        QVERIFY2(stopped(summary->text()), qPrintable(summary->text()));
        QVERIFY(summary->text().contains(QStringLiteral("1")));
        QVERIFY(summary->text().contains(QStringLiteral("重新")) || summary->text().contains(QStringLiteral("重试")));
        QVERIFY(!hasEngineeringWords(summary->text()));
        QVERIFY(accessibleDescription(summary).contains(summary->text()));
        QVERIFY(saveDocumentEvidence(dock, synchronous ? QStringLiteral("real-cancel-sync")
                                                       : QStringLiteral("real-cancel-async")));
    }

};

QTEST_MAIN(DocumentProductLanguageSmoke)
#include "smoke_document_product_language.moc"
