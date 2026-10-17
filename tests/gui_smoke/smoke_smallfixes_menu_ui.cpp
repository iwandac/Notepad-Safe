// ============================================================================
//  smoke_smallfixes_menu_ui：§16 组 1 · 小修复组 GUI 冒烟测试（Qt6 Test）
//  目标：最快速度拿 6 条 P0 PASS。仅实例化真实 MainWindow 做纯 UI 层断言（菜单项
//       enabled 状态 / 动作 shortcut/text / 对话框弹出）。绝不驱动任何函数内部
//       行为（符合 D005 作废的铁律：只靠 UI setEnabled=false 禁用）。
//
//  覆盖（L2 TC §16 组 1 · 6 P0）：
//    SR02-COMFORT-A：菜单"替换"动作 text 含 (&R)
//    SR02-COMFORT-B：替换动作 shortcut == QKeySequence("Ctrl+R") 精确（不能是 Ctrl+H）
//    SR02-COMFORT-C：触发替换动作 → FindReplaceDialog 真实 show 出来 + Replace 模式
//    D001：L2 smalltestfile tab 打开后 → sortAsc/sortDesc enabled=false（灰）
//    D003：L2 Hex 下编码转换/重新打开均禁用；切到 Text 后仅重新打开可用
//    D007：完整 lifecycle 断言（no-tab → L1 → L2 → L1 → close-all=no-tab）
//          动作 enabled 状态随阶段完全正确（D002/D004 混 tab 切换包含在内）
// ============================================================================
#include <QtCore/QObject>
#include <QtTest/QTest>
#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QSignalSpy>
// ---------- 全局单个 test slot 的硬超时：30 秒。
//   一旦某个 slot 超过 30s 还没返回（例如主线程被同步 readAll 大文件卡死），Qt Test 会
//   直接 abort 当前 case，不再让整个桌面 UI 僵死 5 分钟（默认 300 秒 fatality）。
//   注意：必须在 QTEST_MAIN 展开前 set，且不影响 initTestCase（我们在 initTestCase
//   里单独处理写 120MB fixture 的长耗时，在 initTestCase 里临时 qputenv 放宽）。
#include <QtCore/QByteArray>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSettings>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolBar>
#include <QtGui/QKeySequence>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextBlock>
#include <QtGui/QWheelEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QScreen>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QMetaType>
#include <QtCore/QThread>
#include <QtCore/QCryptographicHash>
#include <QtCore/QScopeGuard>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QListView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionSlider>
#include <atomic>
#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "MainWindow.h"
#include "AppComposition.h"
#include "EditorTab.h"
#include "ThemeTokens.h"
#include "TitleBar.h"
#include "WindowedReadOnlyProvider.h"
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "TextEncoding.h"
#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "SessionManager.h"
#include "DraftStore.h"
#include "GuiContractMatrix.h"
#include "PlainFilePersistence.h"

static bool persistPlain(EditorTab *tab, const QString &target, QString *error)
{
    if (!tab) {
        if (error) *error = QStringLiteral("文档不可用");
        return false;
    }
    if (!tab->isModified()
        && QFileInfo(tab->filePath()).absoluteFilePath()
            == QFileInfo(target).absoluteFilePath()) {
        return true;
    }
    PlainFilePersistence persistence;
    const bool sameTarget = QFileInfo(tab->filePath()).absoluteFilePath()
        == QFileInfo(target).absoluteFilePath();
    const PersistenceResult result = persistence.save(
        tab, {target, {}, tab->persistenceBaseline(), sameTarget});
    if (!result.succeeded() && error)
        *error = result.error;
    return result.succeeded();
}

// Mnemonic / 省略号去除：「替换(&R)...」→「替换」
//   关键：不能只 remove '&'，否则留下 "替换(R)"；必须**整个匹配 `(&[A-Za-z0-9])` 模式全删**。
//   同时兼容 ASCII "..."（3 个句点）和 Unicode HORIZONTAL ELLIPSIS U+2026（1 个 QChar，Windows
//   中文版菜单常见）。这是 SR02-COMFORT-A/B/C 前三断言持续抓不到 menu 替换动作的最终根因——
//   A0（replaceAct，shortcut=Ctrl+R text="替换(&R)..."）strip 后变成「替换(R)」≠ target「替换」，
//   导致 fallback 返回 toolbar 动作（text="替换" shortcut=空 无 &R）→ SR02 三条全 FAIL。
static QString stripMnemonicsAndEllipsis(QString s) {
    // 1. 先去「(&X)」整个 mnemonic 括号组（Qt 菜单访问键标准格式）。
    //    正则：\(&\w\) 匹配「(&R)」「(&S)」。
    static const QRegularExpression rxMnemonic(QStringLiteral("\\(&[A-Za-z0-9]\\)"));
    s.remove(rxMnemonic);
    // 2. 剩下的散落的 &（如果有没被括号包的裸 &，即 Notepad++ 风格「显示(&N)但 &Others」）。
    s.remove('&');
    // 3. 去末尾省略号（Unicode / ASCII 都尝试）。
    while (s.endsWith(QChar(0x2026))) s.chop(1);
    if (s.endsWith("...")) s.chop(3);
    return s.trimmed();
}

// ---------------------------------------------------------------------------
// 工具：在 MainWindow 全部 QAction 中筛选**真正挂在主菜单/工具栏体系里的动作**
//       （排除 FindReplaceDialog / SearchResultsDock 等 child 内部自建的 action）。
//   判定规则（白名单）：满足任一即判定来自菜单/工具栏：
//     a) associatedWidgets() 包含任何 QMenu 或 QToolBar；
//     b) shortcut() 非空（FindReplaceDialog 内部"替换"按钮 action 从不设 shortcut）；
//     c) text() 中包含 & 助记符（FindReplaceDialog 按钮 action 无 &）。
//   这是 SR02 前 3 子断言从 FAIL→PASS 的关键修复（之前全局 findChildren 命中了
//   FindReplaceDialog 内部的 action，它 shortcut=空 & text="替换" 无 mnemonic，
//   恰好和「菜单替换动作」同 display 文本导致错误匹配）。
// ---------------------------------------------------------------------------
static bool isInMenuOrToolbar(QAction* a, const MainWindow* top) {
    if (!a || !top) return false;
    const auto aw = a->associatedWidgets();
    for (QWidget* w : aw) {
        if (qobject_cast<QMenu*>(w) || qobject_cast<QToolBar*>(w)) return true;
    }
    if (!a->shortcut().isEmpty()) return true;
    if (a->text().contains('&')) return true;
    return false;
}
static QAction* findMenuActionByTextContains(const MainWindow* top, const QString& textFragment) {
    if (!top) return nullptr;
    const auto all = top->findChildren<QAction*>();
    for (QAction* a : all) {
        if (!isInMenuOrToolbar(a, top)) continue;
        if (a->text().contains(textFragment, Qt::CaseInsensitive)) return a;
    }
    return nullptr;
}
static QAction* findMenuActionByExactDisplay(const MainWindow* top, const QString& display) {
    if (!top) return nullptr;
    const QString target = stripMnemonicsAndEllipsis(display);
    const auto all = top->findChildren<QAction*>();
    QAction* fallback = nullptr;
    for (QAction* a : all) {
        if (!isInMenuOrToolbar(a, top)) continue;
        if (stripMnemonicsAndEllipsis(a->text()) == target) {
            // 优先挑 shortcut 非空的那个（MainWindow::createMenus 里 replaceAct 有 shortcut，
            // 工具栏 452 行 "替换" action 没 shortcut → fallback 用 menu 那个）
            if (!a->shortcut().isEmpty()) return a;
            if (!fallback) fallback = a;
        }
    }
    return fallback;
}

// Encoding actions have stable product object names. Display interpretation
// stays directly in the Encoding menu and is checkable; destructive conversion
// lives in the folded "转换为" submenu and deliberately is not checkable.
struct EncodingActions {
    QVector<QAction*> convert;  // 「转为...」5 项（L2 应全灰）
    QVector<QAction*> display;  // 「以编码显示...」5 项（仅文本呈现可点）
};
static EncodingActions collectEncodingActions(const MainWindow* top) {
    EncodingActions out;
    if (!top) return out;
    const auto all = top->findChildren<QAction*>();
    for (QAction *action : all) {
        if (!action || !isInMenuOrToolbar(action, top)
            || action->data().typeId() != QMetaType::Int) {
            continue;
        }
        if (action->objectName().startsWith(QStringLiteral("encoding.convert.")))
            out.convert.push_back(action);
        else if (action->objectName().startsWith(
                     QStringLiteral("encoding.display.")))
            out.display.push_back(action);
    }
    std::sort(out.convert.begin(), out.convert.end(), [](QAction* a, QAction* b){ return a->text() < b->text(); });
    std::sort(out.display.begin(), out.display.end(), [](QAction* a, QAction* b){ return a->text() < b->text(); });
    return out;
}

// 取得 MainWindow 当前激活 tab（同 MainWindow::currentTab，但 currentTab 是非 public，
// 这里走 m_tabs（= QTabWidget）→ currentWidget → qobject_cast<EditorTab*> 作为等价路径。
// 如果没找到 findChild<QTabWidget*>，则用 findChildren<EditorTab*> 里 isVisibleTo(top) 的那一个。
static EditorTab* currentEditorTab(const MainWindow* mw) {
    if (!mw) return nullptr;
    auto* tabs = mw->findChild<QTabWidget*>();
    if (!tabs) return nullptr;
    return qobject_cast<EditorTab*>(tabs->currentWidget());
}

static int tabCount(const MainWindow* mw) {
    auto* tabs = mw->findChild<QTabWidget*>();
    return tabs ? tabs->count() : 0;
}

static QByteArray dropFixtureHash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}
static EditorTab* tabAt(const MainWindow* mw, int idx) {
    auto* tabs = mw->findChild<QTabWidget*>();
    if (!tabs) return nullptr;
    if (idx < 0 || idx >= tabs->count()) return nullptr;
    return qobject_cast<EditorTab*>(tabs->widget(idx));
}
static void switchToTab(MainWindow* mw, int idx) {
    auto* tabs = mw->findChild<QTabWidget*>();
    if (!tabs) { qCritical() << "switchToTab: QTabWidget not found"; return; }
    if (idx < 0 || idx >= tabs->count()) { qCritical() << "switchToTab: idx out of range" << idx << "count=" << tabs->count(); return; }
    tabs->setCurrentIndex(idx);
    QApplication::processEvents(QEventLoop::AllEvents, 100);
}

static void dragScrollBar(QScrollBar *bar, qreal targetRatio) {
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
    const QPoint destination(bar->width() / 2,
        qBound(2, int(bar->height() * targetRatio), bar->height() - 2));
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, handle.center());
    QTest::mouseMove(bar, destination, 80);
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, destination);
}

// 一次性关闭 MainWindow 所有 tab（D007 的阶段 0 逻辑抽取），保证 case 间不互相残留状态。
// 对每个 L2 tab 先 shutdownL2BeforeClose()，然后 removeTab 直到 count=0。
static void closeAllEditorTabs(MainWindow* mw) {
    auto* tabs = mw ? mw->findChild<QTabWidget*>() : nullptr;
    if (!tabs) return;
    while (tabs->count() > 0) {
        QWidget* page = tabs->widget(0);
        auto* et = qobject_cast<EditorTab*>(page);
        if (et) et->shutdownL2BeforeClose();
        tabs->removeTab(0);
        delete page;
    }
}

// 写一个小文件到临时目录（内容=sizeMB MiB，确保超过 FileTierPolicy 的 S1 上限 → 走 S2 auto mode；
// 路径返回绝对路径，失败返回空字符串。openFile 走 MainWindow::openFilesFromExternal 用户真实入口，
// 确保真正触发 tab 内部 L1→L2 切换 → emit l2ModeStatusChanged → syncL2SensitiveActions 链路。
static QString makeL2FixtureFile(QTemporaryDir& td, const QString& name, int sizeMB = 64) {
    const QString p = td.filePath(name);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "makeL2FixtureFile: open failed" << p << f.errorString();
        return {};
    }
    // 200B 每行、交替 GET / POST 方便调试，偶尔夹杂 NUL 确保二进制也走 L2。
    const char* patterns[] = {
        "GET / HTTP/1.1\r\nHost: example.com\r\nUser-Agent: smoke-smallfixes/1.0\r\nX-Token: abcdef0123456789\r\n\r\n",
        "POST /api HTTP/1.1\r\nHost: example.com\r\nContent-Length: 36\r\nUser-Agent: smoke-smallfixes/1.0\r\n\r\n{\"k\":\"value_test_0123456789\"}",
    };
    const int lineA = (int)strlen(patterns[0]);
    const int lineB = (int)strlen(patterns[1]);
    qint64 written = 0;
    const qint64 target = qint64(sizeMB) * 1024 * 1024;
    int i = 0;
    while (written < target) {
        const char* pat = patterns[i & 1];
        const int len = (i & 1) ? lineB : lineA;
        QByteArray buf(pat, len);
        if ((i % 97) == 0) buf.append('\0');
        f.write(buf);
        written += buf.size();
        ++i;
    }
    f.close();
    return QFileInfo(p).absoluteFilePath();
}

// L1 小文件 fixture（< L1 阈值，保证 EditorTab::openFile 走 L1）
static QString makeL1FixtureFile(QTemporaryDir& td, const QString& name) {
    const QString p = td.filePath(name);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "makeL1FixtureFile: open failed" << p << f.errorString();
        return {};
    }
    for (int i = 0; i < 40; ++i) {
        f.write("line ");
        f.write(QByteArray::number(i));
        f.write(": small L1 content for menu disable toggle test\r\n");
    }
    f.close();
    return QFileInfo(p).absoluteFilePath();
}

struct EncodedSearchFixture {
    QString path;
    QString markerText;
    QString regexQuery;
    QByteArray markerBytes;
    qint64 markerOffset = -1;
};

// A real S2 text document whose only non-ASCII record is encoded as GB18030.
// Keeping the marker near the beginning makes both the initial visible
// projection and the search result independently observable in the GUI.
static EncodedSearchFixture makeGb18030SearchFixture(QTemporaryDir& td) {
    EncodedSearchFixture fixture;
    const char32_t supplementary = 0x20000;
    const QString rareCharacter = QString::fromUcs4(&supplementary, 1);
    fixture.markerText = QStringLiteral("用户") + rareCharacter
        + QStringLiteral(" id=4821 END");
    fixture.regexQuery = QStringLiteral("用户") + rareCharacter
        + QStringLiteral("\\s+id=\\d+\\s+END");
    bool encodedExactly = false;
    fixture.markerBytes = encodeTextFragment(
        fixture.markerText, TextEncoding::GB18030, &encodedExactly);
    if (!encodedExactly || fixture.markerBytes.isEmpty())
        return fixture;

    fixture.path = td.filePath(QStringLiteral("gui-search-gb18030-s2.log"));
    QFile file(fixture.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fixture.path.clear();
        return fixture;
    }
    const QByteArray prefix("boot complete\nrequest accepted\n");
    if (file.write(prefix) != prefix.size()) {
        fixture.path.clear();
        return fixture;
    }
    fixture.markerOffset = prefix.size();
    if (file.write(fixture.markerBytes) != fixture.markerBytes.size()
        || file.write("\n", 1) != 1) {
        fixture.path.clear();
        return fixture;
    }
    QByteArray filler;
    filler.reserve(1024 * 1024);
    while (filler.size() < 1024 * 1024)
        filler.append("ordinary log record status=ok sequence=00000000\n");
    const qint64 target = 33LL * 1024 * 1024;
    while (file.pos() < target) {
        const qint64 remaining = target - file.pos();
        const qsizetype amount = static_cast<qsizetype>(
            qMin<qint64>(remaining, filler.size()));
        if (file.write(filler.constData(), amount) != amount) {
            fixture.path.clear();
            return fixture;
        }
    }
    file.close();
    fixture.path = QFileInfo(fixture.path).absoluteFilePath();
    return fixture;
}

// openFilesFromExternal 是异步/阻塞未知？给一段等待直到 tab 出现 & L2 标记切换：
static bool waitForTabCountAndL2(MainWindow* mw, int expectedCount, bool wantL2OnActive,
                                  int timeoutMs = 8000) {
    QElapsedTimer t; t.start();
    EditorTab* lastSeen = nullptr;
    while (t.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        if (tabCount(mw) >= expectedCount) {
            auto* et = currentEditorTab(mw);
            lastSeen = et;
            const bool okL2 = !wantL2OnActive || (et && et->isL2Active());
            if (wantL2OnActive) {
                if (okL2) return true;
            } else {
                return true;
            }
        }
        QTest::qWait(20);
    }
    // 超时：留一条调试信息，方便定位是 tab 没加还是 L2 没切成功（openLargeFileL2 失败→回退 L1
    //   的 readAll 会导致读盘几分钟，这里提前给出提示）。
    auto* tabs = mw ? mw->findChild<QTabWidget*>() : nullptr;
    const int n = tabs ? tabs->count() : -1;
    QString state = QStringLiteral("count=%1 (>=%2? %3) lastSeenTab=")
        .arg(n).arg(expectedCount).arg(n >= expectedCount ? "YES":"NO");
    if (lastSeen) {
        state += QStringLiteral(" [path='%1' sizeKB=%2 isL2Active=%3]")
            .arg(QDir::toNativeSeparators(lastSeen->filePath()))
            .arg(QString::number(QFileInfo(lastSeen->filePath()).size() / 1024))
            .arg(lastSeen->isL2Active());
    } else {
        state += QStringLiteral("null");
    }
    qCritical() << "waitForTabCountAndL2 TIMEOUT (wantL2OnActive=" << wantL2OnActive
                << " timeoutMs=" << timeoutMs << ") state=" << state;
    return false;
}

// ---------------------------------------------------------------------------
//  组 1 Test Suite：6 P0 分 6 个 slots
// ---------------------------------------------------------------------------
class Group1SmallfixesMenuUI : public QObject {
    Q_OBJECT
private:
    QTemporaryDir tmpDir_;
    MainWindow* mw_ = nullptr;
    QString primaryTestDataDir_;
    QByteArray originalDataDir_;
    bool originalDataDirWasSet_ = false;
    // 大 fixture 按用例懒创建；指定运行聚焦用例时不再无意义写入 120 MiB。
    QString l2FixtureD001_;
    QString l2FixtureD003_;
    QString l2FixtureD007_;
    QString l1FixtureD007_;
    EncodedSearchFixture gb18030SearchFixture_;

    bool ensureGb18030SearchFixture() {
        if (!gb18030SearchFixture_.path.isEmpty()
            && QFileInfo::exists(gb18030SearchFixture_.path)) {
            return true;
        }
        gb18030SearchFixture_ = makeGb18030SearchFixture(tmpDir_);
        return !gb18030SearchFixture_.path.isEmpty()
            && gb18030SearchFixture_.markerOffset >= 0;
    }

    static QPushButton *buttonWithText(QWidget *parent, const QString &text) {
        if (!parent) return nullptr;
        for (QPushButton *button : parent->findChildren<QPushButton *>()) {
            if (button->text() == text)
                return button;
        }
        return nullptr;
    }

    bool displayCurrentDocumentAs(const QString &encodingId) {
        auto *action = mw_->findChild<QAction *>(
            QStringLiteral("encoding.display.%1").arg(encodingId));
        QMenu *menu = nullptr;
        for (QMenu *candidate : mw_->findChildren<QMenu *>()) {
            if (candidate->actions().contains(action)) {
                menu = candidate;
                break;
            }
        }
        if (!menu || !action || !action->isEnabled())
            return false;
        menu->popup(mw_->mapToGlobal(QPoint(180, mw_->menuBar()->height() + 40)));
        if (!QTest::qWaitForWindowExposed(menu))
            return false;
        const QRect geometry = menu->actionGeometry(action);
        if (!geometry.isValid())
            return false;
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, geometry.center());
        QElapsedTimer timer;
        timer.start();
        while (menu->isVisible() && timer.elapsed() < 1000) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(10);
        }
        return !menu->isVisible();
    }

private slots:
    void initTestCase() {
        QVERIFY(tmpDir_.isValid());
        // Never read or overwrite the developer's real window/session state.
        // This also makes screenshots deterministic: a Search Results dock
        // left open by a prior manual run must not leak into an unrelated
        // clean-profile journey.
        originalDataDirWasSet_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        originalDataDir_ = qgetenv("NOTEPADSAFE_DATADIR");
        primaryTestDataDir_ = tmpDir_.filePath(QStringLiteral("primary-app-data"));
        QVERIFY(QDir().mkpath(primaryTestDataDir_));
        qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());
        // Qt 全局只有一个 QApplication（由 QTEST_MAIN 创建），这里 new MainWindow
        // 作为顶层窗口即可。不 show() 也行（enabled 状态不依赖 visible），但
        // SR02-COMFORT-C 需要 dialog 能被 expose，所以 show() + waitExposed。
        mw_ = new MainWindow(nullptr);
        // MainWindow intentionally restores the user's last window state. GUI
        // tests that exercise responsive layout need a deterministic normal
        // window, otherwise resize() only changes the restore geometry while a
        // persisted maximized state keeps the visible viewport unchanged.
        mw_->showNormal();
        mw_->resize(1400, 900);
        mw_->show();
        QVERIFY(QTest::qWaitForWindowExposed(mw_));
        QApplication::processEvents(QEventLoop::AllEvents, 200);
    }

    void cleanupTestCase() {
        // A failed isolated-settings case may return before restoring the
        // environment.  Point teardown back at the suite-owned directory so
        // MainWindow::saveSettings can never touch the user's profile.
        qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());
        // 依次 close 所有 tab 防止 autosave 弹窗阻塞
        if (mw_) {
            auto* tabs = mw_->findChild<QTabWidget*>();
            if (tabs) {
                while (tabs->count() > 0) {
                    auto* et = qobject_cast<EditorTab*>(tabs->widget(0));
                    if (et) et->shutdownL2BeforeClose();
                    tabs->removeTab(0);
                    QApplication::processEvents(QEventLoop::AllEvents, 50);
                }
            }
            mw_->close();
            delete mw_;
            mw_ = nullptr;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        if (originalDataDirWasSet_)
            qputenv("NOTEPADSAFE_DATADIR", originalDataDir_);
        else
            qunsetenv("NOTEPADSAFE_DATADIR");
    }

    // ========== SR02-COMFORT（替换快捷键一致性 3 子断言）==========
    // L2 TC §16 组 1 SR02-COMFORT：text 含 (&R) + shortcut===Ctrl+R + Ctrl+R 触发能打开
    void tc_sr02_comfort_a_replace_text_contains_R() {
        auto* actReplace = findMenuActionByExactDisplay(mw_, QStringLiteral("替换"));
        QVERIFY2(actReplace != nullptr,
            "找不到「替换」动作：检查 createMenus 是否真的 addAction(QStringLiteral(\"替换(&R)...\"))");
        const QString t = actReplace->text();
        // 精确断言：text 中必须包含 &R（访问键字母 = R）；
        // stripMnemonicsAndEllipsis 已经把 & 去掉用于显示匹配，这里再查原 text
        QVERIFY2(t.contains("&R"), qPrintable(
            QStringLiteral("替换动作 text=%1 应该包含 '&R'（与括号标注一致），实际找不到 &R").arg(t)));
    }

    void tc_sr02_comfort_b_replace_shortcut_exactly_ctrl_r() {
        auto* actReplace = findMenuActionByExactDisplay(mw_, QStringLiteral("替换"));
        QVERIFY(actReplace != nullptr);
        const QKeySequence ctrlR(QStringLiteral("Ctrl+R"));
        const QKeySequence ctrlH(QStringLiteral("Ctrl+H"));
        const QKeySequence actual = actReplace->shortcut();
        // 精确 == Ctrl+R（字符串级匹配，硬断言）
        QVERIFY2(actual.matches(ctrlR) == QKeySequence::ExactMatch, qPrintable(
            QStringLiteral("替换动作 shortcut 应该精确是 Ctrl+R；实际=%1 (count=%2)")
                .arg(actual.toString(), QString::number(actual.count()))));
        // 明确禁止是 Ctrl+H（虽然 matches(ctrlR) 已排除，但为了用户原话硬断言保留）
        QVERIFY2(actual.matches(ctrlH) != QKeySequence::ExactMatch,
            "替换动作 shortcut 不能是 Ctrl+H（用户强制：与菜单(&R)括号对齐，Notepad++ 习惯 Ctrl+R）");
    }

    void tc_sr02_comfort_c_trigger_replace_action_shows_dialog() {
        // showReplaceDialog 会先拿 currentTab()：若 MainWindow 没有任何 tab（MainWindow 构造
        //   函数本身不 new 空 tab，空 tab 只在 restoreSession() 结束时或 newFile 时补），
        //   showReplaceDialog 就 early return 不 show。不要用「文件→新建」造前置：产品默认
        //   SecureDraft，在隔离测试身份下会正确打开首次安全设置模态框并阻塞本用例。
        //   改为通过统一外部打开入口载入 QTemporaryDir 内的普通文本，仍然覆盖真实 MainWindow。
        if (tabCount(mw_) == 0) {
            const QString fixture = makeL1FixtureFile(
                tmpDir_, QStringLiteral("sr02-replace-entry.txt"));
            QVERIFY2(!fixture.isEmpty(), "SR02-C 无法创建隔离的普通文本 fixture");
            mw_->openFilesFromExternal(QStringList{fixture});
            QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        }
        auto* tabBefore = currentEditorTab(mw_);
        QVERIFY2(tabBefore != nullptr, "SR02-C 前置：MainWindow 当前没有激活的 EditorTab，showReplaceDialog 会 early return 不弹窗");
        QCOMPARE(tabCount(mw_) >= 1, true);
        auto* actReplace = findMenuActionByExactDisplay(mw_, QStringLiteral("替换"));
        QVERIFY(actReplace != nullptr);
        // FindReplaceDialog is intentionally lazy: an unused search surface must
        // not add work before first paint. Existing instances, if any, are hidden.
        const auto before = mw_->findChildren<FindReplaceDialog*>();
        bool anyVisibleBefore = false;
        for (auto* d : before) if (d->isVisible()) { anyVisibleBefore = true; break; }
        QVERIFY2(!anyVisibleBefore, "触发替换动作前 FindReplaceDialog 已 visible（上一个 case 误 trigger 了 showReplaceDialog）");
        // 触发动作（等价于用户 Ctrl+R 触发）：trigger() 会经由 MainWindow::showReplaceDialog →
        // m_findDialog->showReplaceMode() → show() / raise() / activateWindow()
        actReplace->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(mw_->findChild<FindReplaceDialog *>() != nullptr, 1000);
        FindReplaceDialog *expectDlg = mw_->findChild<FindReplaceDialog *>();
        // show() → native window 需要事件循环 + 系统 DWM 响应 map，给 2s 容忍度避免环境 flaky
        bool gotVisible = false;
        QElapsedTimer t; t.start();
        while (t.elapsed() < 2000 && !gotVisible) {
            QApplication::processEvents(QEventLoop::AllEvents, 30);
            if (expectDlg->isVisible()) { gotVisible = true; break; }
            QTest::qWait(20);
        }
        QVERIFY2(gotVisible,
            "trigger(菜单替换动作) 后 2s 内 FindReplaceDialog 仍 !isVisible() → showReplaceMode() 没有被执行（或 currentTab==nullptr 导致 early return）");
        // 关对话框，给下一个用例干净状态
        expectDlg->close();
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        // SR02 组不留下 tab，避免影响 D001 nBefore 计算（tabCount 不再因 SR02 新建了空 tab 而 =1）
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);
    }

    // ========== D001：L2 tab → sortAsc/sortDesc 灰（enabled=false）==========
    void tc_d001_l2_disables_sort_actions() {
        QElapsedTimer clock; clock.start();
        // D001 完全独立：关残留 tab → 开 initTestCase 预构建的 L2 fixture → 断言排序灰 → 自清理。
        closeAllEditorTabs(mw_);
        qWarning("[D001] step closeAllEditorTabs done @ %lldms", clock.elapsed());
        QCOMPARE(tabCount(mw_), 0);
        if (l2FixtureD001_.isEmpty())
            l2FixtureD001_ = makeL2FixtureFile(tmpDir_, "d001-l2.bin", 40);
        QVERIFY2(!l2FixtureD001_.isEmpty(), "构建 d001-l2.bin 失败");
        mw_->openFilesFromExternal(QStringList{l2FixtureD001_});
        qWarning("[D001] step openFilesFromExternal done @ %lldms | fixture='%s'",
                 clock.elapsed(), qUtf8Printable(l2FixtureD001_));
        // 给 30 秒上限：超过后直接 FAIL 打印 waitForTabCountAndL2 的 state= 调试信息，
        //   不等 Qt Test 默认 5 分钟单 case 超时 fatal error。
        QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 30000),
            "D001 打开 L2 fixture 失败：30s 内既未 tab count>=1 也未 L2 active（见 qCritical state=）");
        qWarning("[D001] step waitForTabCountAndL2 done @ %lldms | tabCount=%d",
                 clock.elapsed(), tabCount(mw_));
        auto *sizeStatus = mw_->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(sizeStatus != nullptr);
        QVERIFY2(sizeStatus->text().contains(QStringLiteral("40.00 MB")),
                 qPrintable(QStringLiteral("窗口化 mmap 打开后状态栏必须显示真实文件大小，实际：%1")
                                .arg(sizeStatus->text())));
        QVERIFY2(sizeStatus->text().contains(QStringLiteral("二进制 · 十六进制")),
                 qPrintable(QStringLiteral("状态栏必须显示文档类型与呈现方式，实际：%1")
                                .arg(sizeStatus->text())));
        QVERIFY2(sizeStatus->text().contains(QStringLiteral("只读")),
                 qPrintable(QStringLiteral("Binary Hex 必须明确显示只读能力，实际：%1")
                                .arg(sizeStatus->text())));
        auto* sortAsc = findMenuActionByExactDisplay(mw_, QStringLiteral("行升序排序"));
        auto* sortDesc = findMenuActionByExactDisplay(mw_, QStringLiteral("行降序排序"));
        QVERIFY(sortAsc != nullptr);
        QVERIFY(sortDesc != nullptr);
        // D001 强制硬断言：L2 下两个排序动作必须 enabled=false（变灰点不动）
        QVERIFY2(!sortAsc->isEnabled(),
            "L2 tab 激活时「行升序排序」仍 enabled=true（用户体验：可点击、点击后空操作）");
        QVERIFY2(!sortDesc->isEnabled(),
            "L2 tab 激活时「行降序排序」仍 enabled=true");
        // 自清理：不让 D001 留下 L2 tab 影响 D003（D003 已独立，双保险）
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);
    }

    // ========== D003：L2 禁止转码；Hex/Text 均允许选择投影编码 ==========
    void tc_d003_l2_disables_convert_enc_but_not_display_enc() {
        // D003 完全独立：不依赖 D001 留下的 L2 tab（避免 D001 FAIL 时 D003 假 FAIL 连带）。
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);
        if (l2FixtureD003_.isEmpty())
            l2FixtureD003_ = makeL2FixtureFile(tmpDir_, "d003-l2.bin", 40);
        QVERIFY2(!l2FixtureD003_.isEmpty(), "构建 d003-l2.bin 失败");
        mw_->openFilesFromExternal(QStringList{l2FixtureD003_});
        QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 15000),
            "D003 打开 L2 fixture 失败：可能阈值太大或 LineIndex 构建 >15s 未完成（见 qCritical state=）");
        auto* et = currentEditorTab(mw_);
        QVERIFY2(et && et->isL2Active(), "D003 前置：当前激活 tab 应该是 L2");
        const EncodingActions encs = collectEncodingActions(mw_);
        QVERIFY2(encs.convert.size() >= 5, qPrintable(
            QStringLiteral("D003 找到「转为...」动作数量=%1，少于 5（GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE），collectEncodingActions 可能识别失败")
                .arg(encs.convert.size())));
        QVERIFY2(encs.display.size() >= 5, qPrintable(
            QStringLiteral("D003 找到「以编码显示...」动作数量=%1，少于 5，识别失败").arg(encs.display.size())));
        // Hex 数据面始终是原始字节，不能转码；右侧字符投影与 Text
        // 使用同一解码器，所以“以编码显示”在两种呈现中都必须可用。
        QStringList failConvert;
        for (int i = 0; i < encs.convert.size(); ++i) {
            QAction* a = encs.convert[i];
            if (a->isEnabled()) failConvert.push_back(QStringLiteral("[%1]=%2").arg(i).arg(a->text()));
        }
        QVERIFY2(failConvert.isEmpty(), qPrintable(
            QStringLiteral("D003 FAIL：L2 下「转为...」这些动作仍 enabled=true → %1")
                .arg(failConvert.join("、"))));
        for (QAction *action : encs.display)
            QVERIFY(action->isEnabled());
        // Content decides the view: no in-tab presentation commands remain.
        QVERIFY(mw_->findChild<QAction *>(QStringLiteral("view.presentation.text")) == nullptr);
        QVERIFY(mw_->findChild<QAction *>(QStringLiteral("view.presentation.toggle")) == nullptr);
        QCOMPARE(et->viewModeL2(), ViewMode::Hex);
        // 自清理：D003 结束不残留 tab，避免影响 D007（D007 阶段 0 自己也会清，但这里保持独立）
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);
    }

    // ========== D007：Lifecycle no-tab → L1 → L2 → L1 → close-all/no-tab ==========
    // 同时覆盖 D002（sort 动作 tab 切换 true→false→true）+ D004（编码转为组 tab 切换同步）
    void tc_d007_lifecycle_actions_enabled_states() {
        // ---- D007-阶段 0：关闭所有 tab → 期望动作全 disabled
        auto* tabs = mw_->findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        while (tabs->count() > 0) {
            auto* et = qobject_cast<EditorTab*>(tabs->widget(0));
            if (et) et->shutdownL2BeforeClose();
            tabs->removeTab(0);
            QApplication::processEvents(QEventLoop::AllEvents, 80);
        }
        QCOMPARE(tabCount(mw_), 0);

        auto sortAsc = [&]{ return findMenuActionByExactDisplay(mw_, QStringLiteral("行升序排序")); };
        auto sortDesc = [&]{ return findMenuActionByExactDisplay(mw_, QStringLiteral("行降序排序")); };
        auto encActs = [&]{ return collectEncodingActions(mw_); };
        auto *wordWrap = mw_->findChild<QAction *>(QStringLiteral("view.wordWrap"));
        QVERIFY(wordWrap != nullptr);

        // 阶段 0 断言：无 tab → sortAsc/sortDesc + 转为组 5 项全 false
        QVERIFY(sortAsc() && !sortAsc()->isEnabled());
        QVERIFY(sortDesc() && !sortDesc()->isEnabled());
        {
            auto e = encActs();
            for (int i = 0; i < e.convert.size(); ++i) {
                QVERIFY2(!e.convert[i]->isEnabled(), qPrintable(
                    QStringLiteral("no-tab 阶段「转为[%1] %2」意外 enabled=true").arg(i).arg(e.convert[i]->text())));
            }
        }

        // ---- D007-阶段 1：新建 L1 tab → sort+转为组全部 enabled=true
        if (l1FixtureD007_.isEmpty())
            l1FixtureD007_ = makeL1FixtureFile(tmpDir_, "lifecycle-l1-a.txt");
        QVERIFY2(!l1FixtureD007_.isEmpty(), "构建 lifecycle-l1-a.txt 失败");
        mw_->openFilesFromExternal(QStringList{l1FixtureD007_});
        QVERIFY(waitForTabCountAndL2(mw_, 1, false, 3000));
        auto* etL1 = currentEditorTab(mw_);
        QVERIFY(etL1 && !etL1->isL2Active());
        QVERIFY(wordWrap->isEnabled());
        if (!wordWrap->isChecked())
            wordWrap->trigger();
        QVERIFY(wordWrap->isChecked());
        QVERIFY(etL1->wordWrap());
        QVERIFY2(sortAsc()->isEnabled(), "D007 阶段1 L1 打开：行升序排序应 enabled=true（L1 可执行排序）");
        QVERIFY2(sortDesc()->isEnabled(), "D007 阶段1 L1 打开：行降序排序应 enabled=true");
        {
            auto e = encActs();
            QVERIFY(e.convert.size() >= 5);
            for (int i = 0; i < e.convert.size(); ++i) {
                QVERIFY2(e.convert[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段1 L1：转为[%1] %2 应 enabled=true，实际 false")
                        .arg(i).arg(e.convert[i]->text())));
            }
        }

        // ---- D007-阶段 2：打开 L2 tab（tab1），此时 current index 自动切到 L2 → 动作全 false
        if (l2FixtureD007_.isEmpty())
            l2FixtureD007_ = makeL2FixtureFile(tmpDir_, "lifecycle-l2-a.bin", 40);
        QVERIFY2(!l2FixtureD007_.isEmpty(), "构建 lifecycle-l2-a.bin 失败");
        mw_->openFilesFromExternal(QStringList{l2FixtureD007_});
        QVERIFY(waitForTabCountAndL2(mw_, 2, true, 15000));
        QCOMPARE(tabCount(mw_), 2);
        QVERIFY(!wordWrap->isEnabled());
        QVERIFY(!wordWrap->isChecked());
        QVERIFY(!currentEditorTab(mw_)->wordWrap());
        QVERIFY2(!sortAsc()->isEnabled(), "D007 阶段2 切 L2：行升序排序应该立即变灰 enabled=false（D002 切 tab 同步）");
        QVERIFY2(!sortDesc()->isEnabled(), "D007 阶段2 切 L2：行降序排序应该立即变灰 enabled=false（D002 切 tab 同步）");
        {
            auto e = encActs();
            for (int i = 0; i < e.convert.size(); ++i) {
                QVERIFY2(!e.convert[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段2 L2：转为[%1] %2 应该 enabled=false，实际 true（D004 切 tab 同步失败）")
                        .arg(i).arg(e.convert[i]->text())));
            }
            // Hex 右侧字符栏按所选编码投影；只有会改写源字节的转换禁用。
            for (int i = 0; i < e.display.size(); ++i) {
                QVERIFY2(e.display[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段2 L2 Hex：显示[%1] %2 应该 enabled").arg(i).arg(e.display[i]->text())));
            }
        }

        // ---- D007-阶段 3：切回 tab0(L1) → 动作全部恢复 enabled=true（D002/D004 反向同步）
        switchToTab(mw_, 0);
        auto* backL1 = currentEditorTab(mw_);
        QVERIFY(backL1 && !backL1->isL2Active());
        QVERIFY(wordWrap->isEnabled());
        QVERIFY(wordWrap->isChecked());
        QVERIFY(backL1->wordWrap());
        wordWrap->trigger();
        QVERIFY(!wordWrap->isChecked());
        QVERIFY(!backL1->wordWrap());
        QVERIFY2(sortAsc()->isEnabled(), "D007 阶段3 切回 L1：行升序排序应恢复 enabled=true（D002 false→true）");
        QVERIFY2(sortDesc()->isEnabled(), "D007 阶段3 切回 L1：行降序排序应恢复 enabled=true");
        {
            auto e = encActs();
            for (int i = 0; i < e.convert.size(); ++i) {
                QVERIFY2(e.convert[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段3 切回 L1：转为[%1] %2 应恢复 enabled=true（D004 false→true）")
                        .arg(i).arg(e.convert[i]->text())));
            }
        }

        // ---- D007-阶段 4：切 tab1(L2) → 再关 tab0 → 剩 L2 → 关最后一个 → 0 tab
        //       每一步断言 enabled 状态不跳变
        switchToTab(mw_, 1);
        QVERIFY(currentEditorTab(mw_) && currentEditorTab(mw_)->isL2Active());
        QVERIFY(!wordWrap->isEnabled() && !wordWrap->isChecked());
        QVERIFY(!sortAsc()->isEnabled() && !sortDesc()->isEnabled());
        {
            auto e = encActs();
            for (QAction* a : e.convert) QVERIFY(!a->isEnabled());
        }
        tabs->removeTab(0);  // 关 tab0(L1)，现在只留 tab1(L2，新 index=0)
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        QCOMPARE(tabCount(mw_), 1);
        // 只留 L2 tab → 仍 false
        QVERIFY(!sortAsc()->isEnabled() && !sortDesc()->isEnabled());
        {
            auto e = encActs();
            for (QAction* a : e.convert) QVERIFY(!a->isEnabled());
        }
        // 关最后一个 → 0 tab → 仍 false
        if (auto* et = qobject_cast<EditorTab*>(tabs->widget(0))) et->shutdownL2BeforeClose();
        tabs->removeTab(0);
        QApplication::processEvents(QEventLoop::AllEvents, 80);
        QCOMPARE(tabCount(mw_), 0);
        QVERIFY(sortAsc() && !sortAsc()->isEnabled());
        QVERIFY(sortDesc() && !sortDesc()->isEnabled());
        {
            auto e = encActs();
            for (QAction* a : e.convert) QVERIFY(!a->isEnabled());
        }
    }

    void tc_hex_binary_open_edit_save_as_reopen_roundtrip() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        QByteArray expected;
        expected.reserve(512);
        for (int pass = 0; pass < 2; ++pass) {
            for (int value = 0; value < 256; ++value)
                expected.append(char(value));
        }
        const QString sourcePath = tmpDir_.filePath(QStringLiteral("hex-roundtrip-source.bin"));
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(expected), qint64(expected.size()));
        }

        mw_->openFilesFromExternal(QStringList{sourcePath});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View() && tab->l2View()->isOpen(), 5000);
        QVERIFY(tab->viewModeL2() == ViewMode::Hex);
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("00 01 02 03 04 05 06 07")), 5000);
        QVERIFY(tab->l2View()->hexBytesPerRow() >= 16);
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET), 0L);
        QCOMPARE(tab->editor()->marginType(0), QsciScintilla::TextMargin);
        auto *asciiPreview = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(asciiPreview != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->isVisible(), 1000);
        QVERIFY(!asciiPreview->isReadOnly());
        QVERIFY(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(16), TextEncoding::Utf8, 16)));

        QCOMPARE(tab->viewModeL2(), ViewMode::Hex);
        bool rawByteStatus = false;
        bool hexStatus = false;
        for (QLabel *label : mw_->findChildren<QLabel *>()) {
            rawByteStatus |= label->text().startsWith(
                QStringLiteral("原始字节 · 字符区 UTF-8"));
            hexStatus |= label->text() == QStringLiteral("十六进制");
        }
        QVERIFY(rawByteStatus);
        QVERIFY(hexStatus);

        // An unchanged binary document is already durable. Save must be a
        // literal no-op: no rewrite, no replacement file identity change and
        // no legacy visible backup beside the source. This is the same route
        // used by a 9 MiB PDF/unknown binary document.
        QAction *saveAction = findMenuActionByExactDisplay(mw_, QStringLiteral("保存"));
        QVERIFY(saveAction != nullptr);
        QVERIFY(!saveAction->isEnabled());
        DiskFileVersion unchangedBefore;
        QCOMPARE(captureDiskFileVersion(sourcePath, &unchangedBefore),
                 DiskFileState::Unchanged);
        QString unchangedSaveError;
        QVERIFY2(persistPlain(tab, tab->filePath(), &unchangedSaveError),
                 qPrintable(unchangedSaveError));
        DiskFileVersion unchangedAfter;
        QCOMPARE(captureDiskFileVersion(sourcePath, &unchangedAfter),
                 DiskFileState::Unchanged);
        QCOMPARE(unchangedAfter.fileIdentity, unchangedBefore.fileIdentity);
        QCOMPARE(unchangedAfter.modifiedMs, unchangedBefore.modifiedMs);
        QVERIFY(!QFileInfo::exists(sourcePath
                                  + QStringLiteral(".notepad-safe-save-tmp")));

        // Real GUI input: byte overwrite does not mutate the formatted text
        // projection. Two nibbles typed at the byte caret become one raw byte.
        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->setCursorPosition(0, 0);
        QTest::keyClicks(tab->editor(), QStringLiteral("FF"));
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QVERIFY(tab->isModified());
        expected[0] = char(0xff);
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(11),
                                  QStringLiteral("FF 01 02 03"), 1000);

        // A selected byte can be replaced and the separate ASCII column is
        // regenerated from the same byte source immediately.
        tab->editor()->setSelection(0, 3, 0, 5);
        QTest::keyClicks(tab->editor(), QStringLiteral("41"));
        expected[1] = 'A';
        const QByteArray afterHexEdit = expected;
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 02 03")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 1000);

        // The character column is a second input surface for the same bytes,
        // not a decorative preview. Editing byte 2 must update the Hex grid,
        // dirty state and later disk round-trip through the shared patch model.
        QTextCursor asciiCursor = asciiPreview->textCursor();
        asciiCursor.setPosition(2);
        asciiPreview->setTextCursor(asciiCursor);
        asciiPreview->setFocus(Qt::OtherFocusReason);
        QTest::keyClicks(asciiPreview, QStringLiteral("Z"));
        expected[2] = 'Z';
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(11),
                                  QStringLiteral("FF 41 5A 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 1000);
        bool byteOffsetStatus = false;
        for (QLabel *label : mw_->findChildren<QLabel *>())
            byteOffsetStatus |= label->text() == QStringLiteral("偏移: 0x00000003");
        QVERIFY(byteOffsetStatus);

        // Both input surfaces must participate in one document-level history.
        // Undo/redo is invoked through EditorTab, exactly as the Edit menu does.
        tab->undo();
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(11),
                                  QStringLiteral("FF 41 02 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(afterHexEdit.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 1000);
        tab->redo();
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(11),
                                  QStringLiteral("FF 41 5A 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 1000);

        QTRY_VERIFY_WITH_TIMEOUT(saveAction->isEnabled(), 1000);
        saveAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->isOpen(), 5000);
        {
            QFile saved(sourcePath);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), expected);
        }
        QVERIFY(!QFileInfo::exists(sourcePath
                                  + QStringLiteral(".notepad-safe-save-tmp")));

        // Exercise Save As backing-store handoff, then reopen through the same
        // MainWindow dispatcher used by Open/recent/drag-drop/session paths.
        const QString copyPath = tmpDir_.filePath(QStringLiteral("hex-roundtrip-copy.dat"));
        QString saveAsError;
        QVERIFY2(persistPlain(tab, copyPath, &saveAsError), qPrintable(saveAsError));
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->isOpen(), 5000);
        QCOMPARE(QFileInfo(tab->filePath()).absoluteFilePath(),
                 QFileInfo(copyPath).absoluteFilePath());
        {
            QFile copy(copyPath);
            QVERIFY(copy.open(QIODevice::ReadOnly));
            QCOMPARE(copy.readAll(), expected);
        }

        closeAllEditorTabs(mw_);
        mw_->openFilesFromExternal(QStringList{copyPath});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        tab = currentEditorTab(mw_);
        QVERIFY(tab != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View() && tab->l2View()->isOpen(), 5000);
        QVERIFY(tab->viewModeL2() == ViewMode::Hex);
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 5A 03 04 05 06 07")), 5000);
        asciiPreview = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(asciiPreview && asciiPreview->isVisible());
        QVERIFY(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())));
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET), 0L);

        // Selection in either visual column represents one half-open source
        // byte range and must be reflected immediately in the other column.
        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->setSelection(0, 0, 0, 5); // bytes [0, 2)
        QTRY_COMPARE_WITH_TIMEOUT(asciiPreview->textCursor().selectedText(),
            decodeByteGridText(expected.left(2), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow()), 1000);
        QTextCursor asciiSelection(asciiPreview->document());
        asciiSelection.setPosition(1);
        asciiSelection.setPosition(3, QTextCursor::KeepAnchor); // bytes [1, 3)
        asciiPreview->setFocus(Qt::OtherFocusReason);
        asciiPreview->setTextCursor(asciiSelection);
        const long expectedHexStart = tab->editor()->positionFromLineIndex(0, 3);
        const long expectedHexEnd = tab->editor()->positionFromLineIndex(0, 9);
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART),
            expectedHexStart, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND),
            expectedHexEnd, 1000);
        tab->gotoByteOffset(0);

        bool restoredSizeStatus = false;
        for (QLabel *label : mw_->findChildren<QLabel *>())
            restoredSizeStatus |= label->text()
                == QStringLiteral("512 B · 二进制 · 十六进制 · 可编辑");
        QVERIFY(restoredSizeStatus);
        auto *hexModeStatus = mw_->findChild<QToolButton *>(
            QStringLiteral("status.hexEditMode"));
        QVERIFY(hexModeStatus && hexModeStatus->isVisible());
        QCOMPARE(hexModeStatus->text(), QStringLiteral("OVR"));

        // Insert toggles a single document-level mode shared by both byte
        // columns. In ASCII it inserts one byte; Undo restores the exact source.
        QTextCursor insertCursor(asciiPreview->document());
        insertCursor.setPosition(3);
        asciiPreview->setTextCursor(insertCursor);
        asciiPreview->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(asciiPreview, Qt::Key_Insert);
        QVERIFY(!tab->hexOverwriteMode());
        QTest::keyClicks(asciiPreview, QStringLiteral("Q"));
        QByteArray insertedExpected = expected;
        insertedExpected.insert(3, 'Q');
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(14),
                                  QStringLiteral("FF 41 5A 51 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(insertedExpected.left(4), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 1000);
        tab->undo();
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 5A 03")), 1000);
        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->setCursorPosition(0, 9); // byte 3, high nibble
        QTest::keyClicks(tab->editor(), QStringLiteral("AB"));
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 5A AB 03")), 1000);
        tab->undo();
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 5A 03")), 1000);
        QTest::keyClick(tab->editor(), Qt::Key_Insert);
        QVERIFY(tab->hexOverwriteMode());

        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-hex-roundtrip.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));

        // A real resize must reflow whole bytes (never split a nibble), keep
        // byte zero visible and preserve both projections without changing data.
        mw_->showNormal();
        const int wideBytesPerRow = tab->l2View()->hexBytesPerRow();
        mw_->resize(900, 700);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->hexBytesPerRow() < wideBytesPerRow, 2000);
        QCOMPARE(tab->l2View()->hexBytesPerRow() % 4, 0);
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 5A 03")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
            decodeByteGridText(expected.left(3), TextEncoding::Utf8,
                               tab->l2View()->hexBytesPerRow())), 2000);
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET), 0L);
        const QString narrowScreenshot = QCoreApplication::applicationDirPath()
                                         + QStringLiteral("/gui-hex-roundtrip-narrow.png");
        QVERIFY2(mw_->grab().save(narrowScreenshot), qPrintable(narrowScreenshot));
        mw_->resize(1400, 900);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->hexBytesPerRow(), wideBytesPerRow, 2000);
        closeAllEditorTabs(mw_);
    }

    void tc_hex_delete_backspace_pairwise_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);

        const QVector<Dimension> dimensions = {
            {QStringLiteral("surface"), {QStringLiteral("hex"), QStringLiteral("ascii")}},
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("key"), {QStringLiteral("Delete"), QStringLiteral("Backspace")}},
            {QStringLiteral("selection"), {QStringLiteral("none"),
                                             QStringLiteral("forward"),
                                             QStringLiteral("reverse")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"),
                                            QStringLiteral("middle"),
                                            QStringLiteral("end")}},
        };
        const QVector<Assignment> cases = pairwise(dimensions);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions));
        QVERIFY2(cases.size() < 72,
                 "Pairwise runner should be materially smaller than full Cartesian product");

        // Include a NUL so the real unified-open classifier routes this small
        // fixture to Binary/Hex while leaving the remaining bytes readable in
        // the ASCII projection.
        const QByteArray original("\0bcdefghijklmnop", 16);
        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-CONTRACT]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-delete-contract-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }

            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->l2View()->isOpen(),
                     caseLabel.constData());
            QCOMPARE(tab->viewModeL2(), ViewMode::Hex);
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), caseLabel.constData());

            QWidget *surface = contract.value(QStringLiteral("surface"))
                    == QStringLiteral("hex")
                ? static_cast<QWidget *>(tab->editor())
                : static_cast<QWidget *>(ascii);
            surface->setFocus(Qt::OtherFocusReason);
            if (contract.value(QStringLiteral("mode")) == QStringLiteral("INS"))
                QTest::keyClick(surface, Qt::Key_Insert);
            QCOMPARE(tab->hexOverwriteMode(),
                     contract.value(QStringLiteral("mode")) == QStringLiteral("OVR"));

            const QString boundary = contract.value(QStringLiteral("boundary"));
            const QString selection = contract.value(QStringLiteral("selection"));
            const int caret = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 8 : original.size();
            const int rangeStart = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 7 : original.size() - 2;
            const int rangeEnd = rangeStart + 2;

            if (surface == tab->editor()) {
                const auto hexColumn = [](int byte, bool rangeEndColumn) {
                    return rangeEndColumn && byte > 0 ? byte * 3 - 1 : byte * 3;
                };
                if (selection == QStringLiteral("none")) {
                    tab->editor()->setCursorPosition(0, hexColumn(caret, caret > 0));
                } else {
                    const int startColumn = hexColumn(rangeStart, false);
                    const int endColumn = hexColumn(rangeEnd, true);
                    if (selection == QStringLiteral("forward"))
                        tab->editor()->setSelection(0, startColumn, 0, endColumn);
                    else
                        tab->editor()->setSelection(0, endColumn, 0, startColumn);
                }
            } else {
                QTextCursor cursor(ascii->document());
                if (selection == QStringLiteral("none")) {
                    cursor.setPosition(caret);
                } else if (selection == QStringLiteral("forward")) {
                    cursor.setPosition(rangeStart);
                    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(rangeEnd);
                    cursor.setPosition(rangeStart, QTextCursor::KeepAnchor);
                }
                ascii->setTextCursor(cursor);
            }

            QByteArray expected = original;
            if (selection != QStringLiteral("none")) {
                expected.remove(rangeStart, rangeEnd - rangeStart);
            } else if (contract.value(QStringLiteral("key")) == QStringLiteral("Delete")) {
                if (caret < expected.size())
                    expected.remove(caret, 1);
            } else if (caret > 0) {
                expected.remove(caret - 1, 1);
            }

            const Qt::Key key = contract.value(QStringLiteral("key"))
                    == QStringLiteral("Delete")
                ? Qt::Key_Delete : Qt::Key_Backspace;
            QTest::keyClick(surface, key);
            QApplication::processEvents(QEventLoop::AllEvents, 50);

            const QByteArray actual = tab->l2View()->readByteRange(
                0, tab->l2View()->virtualFileSize());
            QCOMPARE(actual, expected);
            QCOMPARE(tab->l2View()->virtualFileSize(), qint64(expected.size()));

            QString expectedHex;
            for (int i = 0; i < expected.size(); ++i) {
                if (i > 0) expectedHex.append(QLatin1Char(' '));
                expectedHex.append(QStringLiteral("%1").arg(
                    quint8(expected.at(i)), 2, 16, QLatin1Char('0')).toUpper());
            }
            QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(), expectedHex, 1000);
            QString expectedAscii;
            expectedAscii.reserve(expected.size());
            for (const char byte : expected) {
                const uchar value = uchar(byte);
                expectedAscii.append(value >= 0x20 && value <= 0x7e
                                         ? QChar(value) : QLatin1Char('.'));
            }
            QTRY_COMPARE_WITH_TIMEOUT(ascii->toPlainText(), expectedAscii, 1000);

            // Both projections are views of one source.  Neither may regain an
            // independent scrollbar while exercising a different input path.
            QVERIFY2(!tab->editor()->verticalScrollBar()->isVisibleTo(tab),
                     caseLabel.constData());
            QVERIFY2(!ascii->verticalScrollBar()->isVisibleTo(tab),
                     caseLabel.constData());
            QCOMPARE(tab->isModified(), actual != original);

            if (actual != original) {
                tab->undo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          original, 1000);
                tab->redo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          expected, 1000);
            }
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_keyboard_navigation_contract() {
        closeAllEditorTabs(mw_);
        QByteArray bytes(128, '\0');
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char(i);
        const QString path = tmpDir_.filePath(QStringLiteral("hex-navigation.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(bytes), qint64(bytes.size()));
        }
        mw_->openFilesFromExternal(QStringList{path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
        auto *ascii = tab->findChild<QPlainTextEdit *>(QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(tab));
        tab->editor()->setFocus(Qt::OtherFocusReason);

        const int byte = 5;
        tab->editor()->setCursorPosition(0, byte * 3); // high nibble
        QTest::keyClick(tab->editor(), Qt::Key_Right);
        int line = -1, column = -1;
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 0);
        QCOMPARE(column, byte * 3 + 1); // low nibble
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(), byte, 1000);

        QTest::keyClick(tab->editor(), Qt::Key_Right);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 0);
        QCOMPARE(column, (byte + 1) * 3); // skip separator space
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(), byte + 1, 1000);

        QTest::keyClick(tab->editor(), Qt::Key_Left);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(column, byte * 3 + 1);
        QTest::keyClick(tab->editor(), Qt::Key_Left);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(column, byte * 3);

        const int bytesPerRow = tab->l2View()->hexBytesPerRow();
        QVERIFY(bytesPerRow > 8 && bytes.size() > bytesPerRow * 2);
        tab->editor()->setCursorPosition(1, 3 * 3); // row 1, byte 3 high
        QTest::keyClick(tab->editor(), Qt::Key_End);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 1);
        QCOMPARE(column, (bytesPerRow - 1) * 3 + 1); // final low nibble, not after text
        QTest::keyClick(tab->editor(), Qt::Key_Home);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 1);
        QCOMPARE(column, 0);

        // Vertical movement is byte-row movement, not movement through the
        // three-character projection. Preserve the nibble column and mirror
        // the resulting source byte into the ASCII pane.
        tab->editor()->setCursorPosition(1, 3 * 3 + 1);
        QTest::keyClick(tab->editor(), Qt::Key_Up);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 0);
        QCOMPARE(column, 3 * 3 + 1);
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(), 3, 1000);
        QTest::keyClick(tab->editor(), Qt::Key_Down);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 1);
        QCOMPARE(column, 3 * 3 + 1);
        const QTextBlock secondAsciiRow =
            ascii->document()->findBlockByNumber(1);
        QVERIFY(secondAsciiRow.isValid());
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(),
                                  secondAsciiRow.position() + 3, 1000);

        // A real pointer hit on the separator after byte 5 must snap to the
        // following source byte. It may never leave a visible caret on a
        // character that cannot be edited as a nibble.
        const long separator = tab->editor()->positionFromLineIndex(0, 5 * 3 + 2);
        const int separatorX = int(tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, separator));
        const int separatorY = int(tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, separator));
        const int glyphWidth = qMax(1, tab->editor()->fontMetrics().horizontalAdvance(
                                          QLatin1Char('0')));
        QTest::mouseClick(tab->editor()->viewport(), Qt::LeftButton,
                          Qt::NoModifier,
                          QPoint(separatorX + glyphWidth / 2,
                                 separatorY + tab->editor()->fontMetrics().height() / 2));
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS),
            tab->editor()->positionFromLineIndex(0, 6 * 3), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(), 6, 1000);

        // File and page boundaries are explicit byte boundaries. The short
        // final row is clamped to a real byte, never to projection whitespace.
        tab->editor()->setCursorPosition(0, 0);
        QTest::keyClick(tab->editor(), Qt::Key_PageDown);
        int pagedLine = -1, pagedColumn = -1;
        tab->editor()->getCursorPosition(&pagedLine, &pagedColumn);
        QVERIFY(pagedLine >= 0);
        QVERIFY(pagedColumn >= 0);
        QCOMPARE(pagedColumn % 3, 0);
        QTest::keyClick(tab->editor(), Qt::Key_End, Qt::ControlModifier);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(tab->currentByteOffset(), qint64(bytes.size()));
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETCURRENTPOS),
                 tab->editor()->length());
        const QTextBlock lastAsciiRow = ascii->document()->findBlockByNumber(
            (bytes.size() - 1) / bytesPerRow);
        QVERIFY(lastAsciiRow.isValid());
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(),
                                  lastAsciiRow.position()
                                      + lastAsciiRow.text().size(),
                                  1000);
        QTest::keyClick(tab->editor(), Qt::Key_Home, Qt::ControlModifier);
        tab->editor()->getCursorPosition(&line, &column);
        QCOMPARE(line, 0);
        QCOMPARE(column, 0);
        QTRY_COMPARE_WITH_TIMEOUT(ascii->textCursor().position(), 0, 1000);

        // Navigation is a pure view operation.
        QCOMPARE(tab->l2View()->readByteRange(0, bytes.size()), bytes);
        QVERIFY(!tab->isModified());
        closeAllEditorTabs(mw_);
    }

    void tc_hex_byte_selection_contract() {
        closeAllEditorTabs(mw_);
        QByteArray bytes(128, '\0');
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char(i);
        const QString path = tmpDir_.filePath(QStringLiteral("hex-selection.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(bytes), qint64(bytes.size()));
        }
        mw_->openFilesFromExternal(QStringList{path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(tab));
        const int bytesPerRow = tab->l2View()->hexBytesPerRow();
        QVERIFY(bytesPerRow > 8);
        tab->editor()->setFocus(Qt::OtherFocusReason);

        // Shift+Right grows in whole source bytes and both projections expose
        // the same half-open range.
        tab->editor()->setCursorPosition(0, 5 * 3);
        QTest::keyClick(tab->editor(), Qt::Key_Right, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(), qint64(5), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), qint64(6), 1000);
        QCOMPARE(ascii->textCursor().selectionStart(), 5);
        QCOMPARE(ascii->textCursor().selectionEnd(), 6);
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETSELECTIONSTART),
                 tab->editor()->positionFromLineIndex(0, 5 * 3));
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETSELECTIONEND),
                 tab->editor()->positionFromLineIndex(0, 6 * 3));

        QTest::keyClick(tab->editor(), Qt::Key_Right, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), qint64(7), 1000);
        QTest::keyClick(tab->editor(), Qt::Key_Left, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), qint64(6), 1000);

        // Reverse selection retains direction in Scintilla but publishes the
        // same sorted source range to editing commands and the ASCII mirror.
        tab->editor()->setCursorPosition(0, 5 * 3);
        QTest::keyClick(tab->editor(), Qt::Key_Left, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(), qint64(4), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), qint64(5), 1000);
        QVERIFY(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETANCHOR)
                > tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
        QCOMPARE(ascii->textCursor().selectionStart(), 4);
        QCOMPARE(ascii->textCursor().selectionEnd(), 5);

        // Cross-row and file-boundary extension must not stop at projection
        // separators or the final short-row padding.
        tab->editor()->setCursorPosition(0, (bytesPerRow - 1) * 3);
        QTest::keyClick(tab->editor(), Qt::Key_Down, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(),
                                  qint64(bytesPerRow - 1), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(),
                                  qint64(bytesPerRow * 2 - 1), 1000);
        tab->editor()->setCursorPosition(0, 2 * 3);
        QTest::keyClick(tab->editor(), Qt::Key_End,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(), qint64(2), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(),
                                  qint64(bytes.size()), 1000);

        // Real pointer drag from byte 2 to the start of byte 6 is normalized
        // to [2, 6), then Copy returns exactly those four source bytes.
        QTest::keyClick(tab->editor(), Qt::Key_Home, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS),
            tab->editor()->positionFromLineIndex(0, 0), 1000);
        const long dragStartPos = tab->editor()->positionFromLineIndex(0, 2 * 3);
        const long dragEndPos = tab->editor()->positionFromLineIndex(0, 6 * 3);
        const auto pointForPosition = [tab](long position) {
            const int x = int(tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, position));
            const int y = int(tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, position));
            return QPoint(x + 1, y + tab->editor()->fontMetrics().height() / 2);
        };
        QTest::mousePress(tab->editor()->viewport(), Qt::LeftButton,
                          Qt::NoModifier, pointForPosition(dragStartPos));
        QTest::mouseMove(tab->editor()->viewport(), pointForPosition(dragEndPos), 80);
        QTest::mouseRelease(tab->editor()->viewport(), Qt::LeftButton,
                            Qt::NoModifier, pointForPosition(dragEndPos));
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(), qint64(2), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), qint64(6), 1000);
        tab->copy();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("02 03 04 05"));

        QCOMPARE(tab->l2View()->readByteRange(0, bytes.size()), bytes);
        QVERIFY(!tab->isModified());
        closeAllEditorTabs(mw_);
    }

    void tc_hex_mouse_drag_autoscroll_crosses_active_window_contract() {
        closeAllEditorTabs(mw_);
        QByteArray bytes(9 * 1024 * 1024, '\0');
        for (qsizetype i = 0; i < bytes.size(); ++i)
            bytes[i] = char((i * 37 + 11) & 0xff);
        const QByteArray sourceHash = QCryptographicHash::hash(
            bytes, QCryptographicHash::Sha256);
        const QString path = tmpDir_.filePath(
            QStringLiteral("hex-mouse-edge-autoscroll.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(bytes), qint64(bytes.size()));
        }

        mw_->openFilesFromExternal(QStringList{path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 10000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
        auto *hex = tab->editor();
        auto *ascii = tab->findChild<QPlainTextEdit *>(
            QStringLiteral("HexAsciiPreview"));
        QScrollBar *global = tab->l2View()->verticalScrollBar();
        QVERIFY(hex && ascii && ascii->isVisibleTo(tab) && global);
        const int bytesPerRow = tab->l2View()->hexBytesPerRow();
        const int rowsOnScreen = qMax(4, int(hex->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN)));
        const qint64 firstWindowStart = tab->l2View()->activeWindowByteStart();
        const qint64 firstWindowEnd = tab->l2View()->activeWindowByteEnd();
        QVERIFY(firstWindowEnd > firstWindowStart);
        QVERIFY(firstWindowEnd < tab->l2View()->virtualFileSize());

        // Put the final rows of the current projection on screen without
        // replacing the projection. The selection itself must then be created
        // solely by a real Hex-surface pointer drag.
        const qint64 bottomTopRow = qMax<qint64>(
            firstWindowStart / bytesPerRow,
            firstWindowEnd / bytesPerRow - rowsOnScreen - 2);
        global->setValue(int(qMin<qint64>(global->maximum(), bottomTopRow)));
        QTRY_VERIFY_WITH_TIMEOUT(
            hex->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE)
                >= hex->lines() - rowsOnScreen - 3,
            2000);

        const int firstVisible = int(hex->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const int anchorLine = qBound(0, firstVisible + rowsOnScreen / 2,
                                      qMax(0, hex->lines() - 2));
        const int anchorColumn = qMin(2, bytesPerRow - 1) * 3;
        const long anchorPosition = hex->positionFromLineIndex(
            anchorLine, anchorColumn);
        const int anchorX = int(hex->SendScintilla(
            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, anchorPosition)) + 1;
        const int anchorY = int(hex->SendScintilla(
            QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, anchorPosition))
            + hex->fontMetrics().height() / 2;
        const qint64 forwardAnchor = firstWindowStart
            + qint64(anchorLine) * bytesPerRow + anchorColumn / 3;
        const QPoint bottomEdge(qBound(2, anchorX, hex->viewport()->width() - 3),
                                hex->viewport()->height() - 2);
        QTest::mousePress(hex->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(anchorX, anchorY));
        for (int step = 0; step < 36; ++step) {
            QTest::mouseMove(hex->viewport(), bottomEdge, 25);
            QCoreApplication::processEvents();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            tab->l2View()->activeWindowByteStart() > firstWindowStart, 5000);
        const int returnedFirst = int(hex->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        const int returnedLine = qBound(
            0, returnedFirst + rowsOnScreen / 2, qMax(0, hex->lines() - 1));
        const long returnedPosition = hex->positionFromLineIndex(
            returnedLine, anchorColumn);
        const QPoint returnedInside(
            int(hex->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION,
                                   0UL, returnedPosition)) + 1,
            int(hex->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION,
                                   0UL, returnedPosition))
                + hex->fontMetrics().height() / 2);
        const long returnedAddressLength = hex->SendScintilla(
            QsciScintillaBase::SCI_MARGINGETTEXT,
            static_cast<unsigned long>(returnedLine),
            static_cast<void *>(nullptr));
        QByteArray returnedAddressText(qMax<long>(0, returnedAddressLength) + 1,
                                       '\0');
        hex->SendScintilla(QsciScintillaBase::SCI_MARGINGETTEXT,
                           static_cast<unsigned long>(returnedLine),
                           static_cast<void *>(returnedAddressText.data()));
        bool returnedAddressOk = false;
        const qint64 returnedCaret = QString::fromLatin1(
            returnedAddressText.constData(), qMax<long>(0, returnedAddressLength))
            .toLongLong(&returnedAddressOk, 16) + anchorColumn / 3;
        QVERIFY(returnedAddressOk);
        QVERIFY(returnedCaret > firstWindowEnd);
        QTest::mouseMove(hex->viewport(), returnedInside, 50);
        QCoreApplication::processEvents();
        QTest::mouseRelease(hex->viewport(), Qt::LeftButton,
                            Qt::NoModifier, returnedInside);

        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(),
                                  forwardAnchor, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionEnd(), returnedCaret,
                                  3000);
        QCOMPARE(tab->l2View()->selectionAnchorByte(), forwardAnchor);
        QCOMPARE(tab->l2View()->selectionCaretByte(), returnedCaret);
        QVERIFY(global->value() > int(bottomTopRow));

        // The visible ASCII mirror represents the intersection of the global
        // source selection and the current bounded projection. Switching focus
        // and reflowing must not change the authoritative source endpoints.
        const qint64 forwardStart = tab->l2View()->selectionStart();
        const qint64 forwardEnd = tab->l2View()->selectionEnd();
        const qint64 forwardCaret = tab->l2View()->selectionCaretByte();
        ascii->setFocus(Qt::OtherFocusReason);
        mw_->resize(mw_->width() - 140, mw_->height());
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->hexBytesPerRow() > 0, 1000);
        QCOMPARE(tab->l2View()->selectionStart(), forwardStart);
        QCOMPARE(tab->l2View()->selectionEnd(), forwardEnd);
        QCOMPARE(tab->l2View()->selectionAnchorByte(), forwardAnchor);
        QCOMPARE(tab->l2View()->selectionCaretByte(), forwardCaret);
        hex->setFocus(Qt::OtherFocusReason);
        tab->copy();
        QCOMPARE(QGuiApplication::clipboard()->text().split(
                     QLatin1Char(' '), Qt::SkipEmptyParts).size(),
                 int(forwardEnd - forwardStart));
        QCOMPARE(tab->l2View()->selectionAnchorByte(), forwardAnchor);
        QCOMPARE(tab->l2View()->selectionCaretByte(), forwardCaret);
        QTest::keyClick(hex, Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(),
                                  forwardEnd, 1000);
        QCOMPARE(tab->l2View()->selectionEnd(), forwardEnd);
        QCOMPARE(tab->l2View()->selectionAnchorByte(), forwardEnd);
        QCOMPARE(tab->l2View()->selectionCaretByte(), forwardEnd);

        // Now start in the ASCII surface and drag upward. This is a genuinely
        // reverse selection and must cross the preceding projection window
        // without sorting away anchor/caret direction.
        const qint64 reverseWindowStart = tab->l2View()->activeWindowByteStart();
        const int reverseBytesPerRow = tab->l2View()->hexBytesPerRow();
        QVERIFY(reverseWindowStart > 0);
        const qint64 nearWindowTopRow = reverseWindowStart / reverseBytesPerRow + 2;
        global->setValue(int(qMin<qint64>(global->maximum(), nearWindowTopRow)));
        QTRY_VERIFY_WITH_TIMEOUT(
            ascii->cursorForPosition(QPoint(1, 1)).block().blockNumber() <= 4,
            2000);
        QTextBlock reverseBlock = ascii->document()->findBlockByNumber(
            qMin(3, ascii->document()->blockCount() - 1));
        QVERIFY(reverseBlock.isValid() && reverseBlock.text().size() > 4);
        QTextCursor reverseCursor(ascii->document());
        reverseCursor.setPosition(reverseBlock.position() + 4);
        const QPoint reverseAnchorPoint = ascii->cursorRect(reverseCursor).center();
        const QTextCursor reverseHit = ascii->cursorForPosition(reverseAnchorPoint);
        const int reverseHitLine = reverseHit.block().blockNumber();
        const long addressLength = hex->SendScintilla(
            QsciScintillaBase::SCI_MARGINGETTEXT,
            static_cast<unsigned long>(reverseHitLine),
            static_cast<void *>(nullptr));
        QByteArray addressText(qMax<long>(0, addressLength) + 1, '\0');
        hex->SendScintilla(QsciScintillaBase::SCI_MARGINGETTEXT,
                           static_cast<unsigned long>(reverseHitLine),
                           static_cast<void *>(addressText.data()));
        bool addressOk = false;
        const qint64 reverseRowStart = QString::fromLatin1(
            addressText.constData(), qMax<long>(0, addressLength))
            .toLongLong(&addressOk, 16);
        QVERIFY(addressOk);
        const qint64 reverseAnchor = reverseRowStart
            + qint64(reverseHit.position() - reverseHit.block().position());
        const QPoint topEdge(qBound(2, reverseAnchorPoint.x(),
                                    ascii->viewport()->width() - 3), 1);
        QTest::mousePress(ascii->viewport(), Qt::LeftButton, Qt::NoModifier,
                          reverseAnchorPoint);
        for (int step = 0; step < 36; ++step) {
            QTest::mouseMove(ascii->viewport(), topEdge, 25);
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(ascii->viewport(), Qt::LeftButton,
                            Qt::NoModifier, topEdge);

        QTRY_VERIFY_WITH_TIMEOUT(
            tab->l2View()->activeWindowByteStart() < reverseWindowStart, 5000);
        QCOMPARE(tab->l2View()->selectionAnchorByte(), reverseAnchor);
        QVERIFY(tab->l2View()->selectionCaretByte() < reverseWindowStart);
        QCOMPARE(tab->l2View()->selectionStart(),
                 tab->l2View()->selectionCaretByte());
        QCOMPARE(tab->l2View()->selectionEnd(), reverseAnchor);
        QVERIFY(tab->currentByteOffset() < reverseWindowStart);
        auto *positionStatus = mw_->findChild<QLabel *>(
            QStringLiteral("status.position"));
        QVERIFY(positionStatus);
        QTRY_VERIFY_WITH_TIMEOUT(positionStatus->text().contains(
            QString::number(tab->currentByteOffset(), 16)
                .rightJustified(8, QLatin1Char('0')).toUpper()), 1000);

        // Copy reads the complete authoritative source range. A subsequent
        // Shift+arrow extends from the global caret, while plain Right hands
        // authority back by collapsing to the high selection boundary.
        const qint64 reverseCaret = tab->l2View()->selectionCaretByte();
        hex->setFocus(Qt::OtherFocusReason);
        tab->copy();
        QCOMPARE(tab->l2View()->selectionAnchorByte(), reverseAnchor);
        QCOMPARE(tab->l2View()->selectionCaretByte(), reverseCaret);
        QTest::keyClick(hex, Qt::Key_Left, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionAnchorByte(),
                                  reverseAnchor, 1000);
        QCOMPARE(tab->l2View()->selectionCaretByte(), reverseCaret - 1);
        QTest::keyClick(hex, Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->selectionStart(),
                                  reverseAnchor, 1000);
        QCOMPARE(tab->l2View()->selectionEnd(), reverseAnchor);

        QCOMPARE(tab->l2View()->readByteRange(0, bytes.size()), bytes);
        QFile unchanged(path);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCryptographicHash diskHash(QCryptographicHash::Sha256);
        while (!unchanged.atEnd()) {
            const QByteArray chunk = unchanged.read(1024 * 1024);
            QVERIFY(!chunk.isEmpty() || unchanged.atEnd());
            diskHash.addData(chunk);
        }
        QCOMPARE(diskHash.result(), sourceHash);
        QVERIFY(!tab->isModified());
        closeAllEditorTabs(mw_);
    }

    void tc_hex_insert_mode_discoverable_and_eof_append() {
        closeAllEditorTabs(mw_);
        const QByteArray original = QByteArray::fromHex("00112233");
        const QString path = tmpDir_.filePath(QStringLiteral("hex-insert-discoverable.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(original), qint64(original.size()));
        }
        mw_->openFilesFromExternal({path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->viewModeL2() == ViewMode::Hex);

        auto *modeButton = mw_->findChild<QToolButton *>(
            QStringLiteral("status.hexEditMode"));
        QVERIFY2(modeButton,
                 "Hex OVR/INS must be a persistent clickable status control");
        QCOMPARE(modeButton->text(), QStringLiteral("OVR"));
        QVERIFY(modeButton->isVisibleTo(mw_));
        QVERIFY(modeButton->toolTip().contains(QStringLiteral("Insert")));

        QTest::mouseClick(modeButton, Qt::LeftButton);
        QVERIFY(!tab->hexOverwriteMode());
        QCOMPARE(modeButton->text(), QStringLiteral("INS"));
        auto *modeAction = mw_->findChild<QAction *>(
            QStringLiteral("edit.hexInsertMode"));
        QVERIFY(modeAction && modeAction->isEnabled() && modeAction->isChecked());
        modeAction->trigger();
        QVERIFY(tab->hexOverwriteMode());
        QTRY_COMPARE_WITH_TIMEOUT(modeButton->text(), QStringLiteral("OVR"), 1000);
        modeAction->trigger();
        QVERIFY(!tab->hexOverwriteMode());
        QTRY_COMPARE_WITH_TIMEOUT(modeButton->text(), QStringLiteral("INS"), 1000);

        // This must use normal user navigation. The historical implementation
        // mapped Ctrl+End back onto the final low nibble, making an EOF insert
        // caret unreachable even though tests could set a synthetic position.
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(tab->editor(), Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(tab->currentByteOffset(), qint64(original.size()));
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETCURRENTPOS),
                 tab->editor()->length());
        QTest::keyClicks(tab->editor(), QStringLiteral("A"));
        QCOMPARE(tab->l2View()->virtualFileSize(), qint64(original.size() + 1));
        QCOMPARE(tab->l2View()->readByteRange(original.size(), original.size() + 1),
                 QByteArray::fromHex("A0"));
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETCURRENTPOS),
                 tab->editor()->positionFromLineIndex(0, original.size() * 3 + 1));
        QTest::keyClicks(tab->editor(), QStringLiteral("B"));

        QByteArray expected = original;
        expected.append(char(0xab));
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->virtualFileSize(),
                                  qint64(expected.size()), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, expected.size()),
                                  expected, 1000);
        QVERIFY(tab->isModified());

        QString saveError;
        QVERIFY2(persistPlain(tab, tab->filePath(), &saveError), qPrintable(saveError));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), expected);
        auto *sizeStatus = mw_->findChild<QLabel *>(QStringLiteral("status.size"));
        QVERIFY(sizeStatus);
        QTRY_VERIFY_WITH_TIMEOUT(sizeStatus->text().startsWith(
                                     QStringLiteral("5 B · 二进制 · 十六进制 · 可编辑")), 1000);
        QVERIFY(mw_->grab().save(QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/gui-hex-insert-mode.png")));
        closeAllEditorTabs(mw_);
    }

    void tc_hex_selected_direct_typing_replaces_complete_range() {
        const QByteArray original = QByteArray::fromHex("0001020304050607");
        for (const bool overwrite : {true, false}) {
            closeAllEditorTabs(mw_);
            const QString path = tmpDir_.filePath(QStringLiteral(
                "hex-selection-type-%1.bin").arg(overwrite ? "ovr" : "ins"));
            {
                QFile source(path);
                QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal({path});
            QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
            if (!overwrite) {
                tab->editor()->setFocus(Qt::OtherFocusReason);
                QTest::keyClick(tab->editor(), Qt::Key_Insert);
            }
            QCOMPARE(tab->hexOverwriteMode(), overwrite);

            // Direct typing over a byte selection has the same replace-range
            // semantics in OVR and INS: [2,6) becomes exactly one byte AB.
            tab->editor()->setSelection(0, 2 * 3, 0, 6 * 3 - 1);
            QVERIFY(tab->hasSelectionCommand());
            QTest::keyClicks(tab->editor(), QStringLiteral("A"));
            QCOMPARE(tab->l2View()->readByteRange(0, original.size()),
                     QByteArray::fromHex("0001A00607"));
            QCOMPARE(tab->editor()->SendScintilla(
                         QsciScintillaBase::SCI_GETCURRENTPOS),
                     tab->editor()->positionFromLineIndex(0, 2 * 3 + 1));
            QTest::keyClicks(tab->editor(), QStringLiteral("B"));
            QByteArray expected = original;
            expected.replace(2, 4, QByteArray(1, char(0xab)));
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->virtualFileSize(),
                                      qint64(expected.size()), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, expected.size()),
                                      expected, 1000);

            // A high+low nibble pair is one byte-entry undo unit. Undo/redo
            // must never expose an overlapping A0 patch or split projection
            // state from the byte model.
            tab->undo();
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, original.size()),
                                      original, 1000);
            tab->redo();
            QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(0, expected.size()),
                                      expected, 1000);

            QString saveError;
            QVERIFY2(persistPlain(tab, tab->filePath(), &saveError), qPrintable(saveError));
            QFile saved(path);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), expected);
        }
        closeAllEditorTabs(mw_);
    }

    void tc_hex_nibble_input_pairwise_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);
        const QVector<Dimension> dimensions = {
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("nibble"), {QStringLiteral("high"), QStringLiteral("low")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"),
                                            QStringLiteral("middle"),
                                            QStringLiteral("last")}},
            {QStringLiteral("digit"), {QStringLiteral("0"),
                                         QStringLiteral("A"),
                                         QStringLiteral("f")}},
        };
        const QVector<Assignment> cases = pairwise(dimensions);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions));

        const QByteArray original("\0bcdefghijklmnop", 16);
        const auto formattedHex = [](const QByteArray &bytes, int row) {
            QString text;
            for (int i = 0; i < bytes.size(); ++i) {
                if (i > 0)
                    text.append(i % row == 0 ? QLatin1Char('\n') : QLatin1Char(' '));
                text.append(QStringLiteral("%1").arg(
                    quint8(bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper());
            }
            return text;
        };
        const auto formattedAscii = [](const QByteArray &bytes, int row) {
            return decodeByteGridText(bytes, TextEncoding::Utf8, row);
        };

        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-NIBBLE]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-nibble-contract-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex,
                     caseLabel.constData());
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), caseLabel.constData());
            tab->editor()->setFocus(Qt::OtherFocusReason);

            if (contract.value(QStringLiteral("mode")) == QStringLiteral("INS"))
                QTest::keyClick(tab->editor(), Qt::Key_Insert);
            auto *modeStatus = mw_->findChild<QToolButton *>(
                QStringLiteral("status.hexEditMode"));
            QVERIFY(modeStatus);
            QTRY_COMPARE_WITH_TIMEOUT(modeStatus->text(),
                                      contract.value(QStringLiteral("mode")), 1000);
            const int target = contract.value(QStringLiteral("boundary"))
                    == QStringLiteral("start") ? 0
                : contract.value(QStringLiteral("boundary"))
                    == QStringLiteral("middle") ? 8 : original.size() - 1;
            const bool high = contract.value(QStringLiteral("nibble"))
                              == QStringLiteral("high");
            tab->editor()->setCursorPosition(0, target * 3 + (high ? 0 : 1));

            const QChar digit = contract.value(QStringLiteral("digit")).at(0);
            const int nibbleValue = digit.isDigit()
                ? digit.digitValue() : 10 + digit.toUpper().unicode() - QChar('A').unicode();
            QByteArray expected = original;
            if (!tab->hexOverwriteMode() && high) {
                expected.insert(target, char(nibbleValue << 4));
            } else {
                const uchar old = uchar(expected.at(target));
                expected[target] = high
                    ? char((nibbleValue << 4) | (old & 0x0f))
                    : char((old & 0xf0) | nibbleValue);
            }

            QTest::keyClicks(tab->editor(), QString(digit));
            const int row = tab->l2View()->hexBytesPerRow();
            QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(),
                                      formattedHex(expected, row), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(ascii->toPlainText(),
                                      formattedAscii(expected, row), 1000);
            QCOMPARE(tab->l2View()->readByteRange(
                         0, tab->l2View()->virtualFileSize()), expected);
            QVERIFY2(tab->isModified(), caseLabel.constData());

            // Undo/redo must update both projections regardless of whether the
            // edit was an overlay nibble replacement or a VFV insertion.
            tab->undo();
            QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(),
                                      formattedHex(original, row), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(ascii->toPlainText(),
                                      formattedAscii(original, row), 1000);
            tab->redo();
            QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(),
                                      formattedHex(expected, row), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(ascii->toPlainText(),
                                      formattedAscii(expected, row), 1000);

            QString saveError;
            QVERIFY2(persistPlain(tab, tab->filePath(), &saveError), qPrintable(saveError));
            QFile saved(path);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), expected);
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_copy_cut_pairwise_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);
        const QVector<Dimension> dimensions = {
            {QStringLiteral("surface"), {QStringLiteral("hex"), QStringLiteral("ascii")}},
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("operation"), {QStringLiteral("copy"), QStringLiteral("cut")}},
            {QStringLiteral("selection"), {QStringLiteral("none"),
                                             QStringLiteral("forward"),
                                             QStringLiteral("reverse")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"),
                                            QStringLiteral("middle"),
                                            QStringLiteral("end")}},
        };
        const QVector<Assignment> cases = pairwise(dimensions);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions));

        QByteArray original("abcdefghijklmnopqrstuvwxyzABCDE", 31);
        original.append('\0'); // unified-open binary classifier, outside selected ranges
        const auto hexText = [](const QByteArray &bytes) {
            QString text;
            for (int i = 0; i < bytes.size(); ++i) {
                if (i) text.append(QLatin1Char(' '));
                text.append(QStringLiteral("%1").arg(
                    quint8(bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper());
            }
            return text;
        };

        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-CLIPBOARD]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-clipboard-contract-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex,
                     caseLabel.constData());
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), caseLabel.constData());
            QWidget *surface = contract.value(QStringLiteral("surface"))
                    == QStringLiteral("hex")
                ? static_cast<QWidget *>(tab->editor())
                : static_cast<QWidget *>(ascii);
            surface->setFocus(Qt::OtherFocusReason);
            if (contract.value(QStringLiteral("mode")) == QStringLiteral("INS"))
                QTest::keyClick(surface, Qt::Key_Insert);

            const QString boundary = contract.value(QStringLiteral("boundary"));
            const int rangeStart = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 15 : original.size() - 3;
            const int rangeEnd = rangeStart + 2;
            const QString selection = contract.value(QStringLiteral("selection"));
            if (surface == tab->editor()) {
                if (selection == QStringLiteral("none")) {
                    const int caret = boundary == QStringLiteral("start") ? 0
                        : boundary == QStringLiteral("middle") ? 16 : original.size();
                    tab->editor()->setCursorPosition(0, caret * 3 - (caret ? 1 : 0));
                } else {
                    const int first = rangeStart * 3;
                    const int last = rangeEnd * 3 - 1;
                    if (selection == QStringLiteral("forward"))
                        tab->editor()->setSelection(0, first, 0, last);
                    else
                        tab->editor()->setSelection(0, last, 0, first);
                }
            } else {
                QTextCursor cursor(ascii->document());
                if (selection == QStringLiteral("none")) {
                    const int caret = boundary == QStringLiteral("start") ? 0
                        : boundary == QStringLiteral("middle") ? 16 : original.size();
                    cursor.setPosition(caret);
                } else if (selection == QStringLiteral("forward")) {
                    cursor.setPosition(rangeStart);
                    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(rangeEnd);
                    cursor.setPosition(rangeStart, QTextCursor::KeepAnchor);
                }
                ascii->setTextCursor(cursor);
            }

            const QString sentinel = QStringLiteral("clipboard-sentinel");
            QGuiApplication::clipboard()->setText(sentinel);
            const bool cut = contract.value(QStringLiteral("operation"))
                             == QStringLiteral("cut");
            QTest::keyClick(surface, cut ? Qt::Key_X : Qt::Key_C,
                            Qt::ControlModifier);
            QApplication::processEvents(QEventLoop::AllEvents, 50);

            QByteArray expected = original;
            if (selection == QStringLiteral("none")) {
                QCOMPARE(QGuiApplication::clipboard()->text(), sentinel);
            } else {
                const QByteArray selected = original.mid(rangeStart,
                                                         rangeEnd - rangeStart);
                const QString expectedClipboard = surface == tab->editor()
                    ? hexText(selected) : QString::fromLatin1(selected);
                QCOMPARE(QGuiApplication::clipboard()->text(), expectedClipboard);
                if (cut)
                    expected.remove(rangeStart, rangeEnd - rangeStart);
            }
            QCOMPARE(tab->l2View()->readByteRange(
                         0, tab->l2View()->virtualFileSize()), expected);
            QCOMPARE(tab->isModified(), cut && selection != QStringLiteral("none"));

            if (cut && selection != QStringLiteral("none")) {
                tab->undo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          original, 1000);
                tab->redo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          expected, 1000);
            }
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_paste_pairwise_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);
        const QVector<Dimension> dimensions = {
            {QStringLiteral("surface"), {QStringLiteral("hex"), QStringLiteral("ascii")}},
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("selection"), {QStringLiteral("none"),
                                             QStringLiteral("forward"),
                                             QStringLiteral("reverse")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"),
                                            QStringLiteral("middle"),
                                            QStringLiteral("end")}},
            {QStringLiteral("payload"), {QStringLiteral("valid"),
                                           QStringLiteral("invalid")}},
        };
        const QVector<Assignment> cases = pairwise(dimensions);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions));

        const QByteArray original("\0bcdefghijklmnop", 16);
        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-PASTE]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-paste-contract-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex,
                     caseLabel.constData());
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), caseLabel.constData());
            QWidget *surface = contract.value(QStringLiteral("surface"))
                    == QStringLiteral("hex")
                ? static_cast<QWidget *>(tab->editor())
                : static_cast<QWidget *>(ascii);
            surface->setFocus(Qt::OtherFocusReason);
            const bool overwrite = contract.value(QStringLiteral("mode"))
                                   == QStringLiteral("OVR");
            if (!overwrite)
                QTest::keyClick(surface, Qt::Key_Insert);

            const QString boundary = contract.value(QStringLiteral("boundary"));
            const QString selection = contract.value(QStringLiteral("selection"));
            const int caret = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 8 : original.size();
            const int rangeStart = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 7 : original.size() - 2;
            const int rangeEnd = rangeStart + 2;
            if (surface == tab->editor()) {
                if (selection == QStringLiteral("none")) {
                    tab->editor()->setCursorPosition(0,
                        caret == original.size() ? tab->editor()->text().size()
                                                 : caret * 3);
                } else {
                    const int first = rangeStart * 3;
                    const int last = rangeEnd * 3 - 1;
                    if (selection == QStringLiteral("forward"))
                        tab->editor()->setSelection(0, first, 0, last);
                    else
                        tab->editor()->setSelection(0, last, 0, first);
                }
            } else {
                QTextCursor cursor(ascii->document());
                if (selection == QStringLiteral("none")) {
                    cursor.setPosition(caret);
                } else if (selection == QStringLiteral("forward")) {
                    cursor.setPosition(rangeStart);
                    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
                } else {
                    cursor.setPosition(rangeEnd);
                    cursor.setPosition(rangeStart, QTextCursor::KeepAnchor);
                }
                ascii->setTextCursor(cursor);
            }

            const bool valid = contract.value(QStringLiteral("payload"))
                               == QStringLiteral("valid");
            const QByteArray inserted = surface == tab->editor()
                ? QByteArray::fromHex("AABB")
                : (valid ? QByteArray("XY") : QStringLiteral("中").toUtf8());
            QGuiApplication::clipboard()->setText(valid
                ? (surface == tab->editor() ? QStringLiteral("AA BB")
                                            : QStringLiteral("XY"))
                : (surface == tab->editor() ? QStringLiteral("GG")
                                            : QStringLiteral("中")));

            QByteArray expected = original;
            // Hex digits still reject malformed input. The character pane is
            // the encoded-text surface, so representable multibyte text is a
            // valid payload rather than an "ASCII-only" rejection case.
            const bool accepted = valid || surface != tab->editor();
            if (accepted) {
                if (selection != QStringLiteral("none")) {
                    expected.replace(rangeStart, rangeEnd - rangeStart, inserted);
                } else if (!overwrite) {
                    expected.insert(caret, inserted);
                } else if (caret < expected.size()) {
                    const int replaceLength = qMin(inserted.size(), expected.size() - caret);
                    expected.replace(caret, replaceLength, inserted.left(replaceLength));
                }
                // OVR at EOF is deliberately a no-op: it must not silently
                // become insertion mode and change the file length.
            }

            QTest::keyClick(surface, Qt::Key_V, Qt::ControlModifier);
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            const QByteArray actual = tab->l2View()->readByteRange(
                0, tab->l2View()->virtualFileSize());
            QCOMPARE(actual, expected);
            QCOMPARE(tab->l2View()->virtualFileSize(), qint64(expected.size()));
            QCOMPARE(tab->isModified(), expected != original);

            if (expected != original) {
                tab->undo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          original, 1000);
                tab->redo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          expected, 1000);
            }
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_invalid_direct_input_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);
        const QVector<Dimension> dimensions = {
            {QStringLiteral("surface"), {QStringLiteral("hex"), QStringLiteral("ascii")}},
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("input"), {QStringLiteral("G"), QStringLiteral("space"),
                                         QStringLiteral("enter"), QStringLiteral("control"),
                                         QStringLiteral("unicode")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"), QStringLiteral("end")}},
        };
        const Constraint validCase = [](const Assignment &row) {
            if (row.value(QStringLiteral("surface")) == QStringLiteral("hex"))
                return true;
            const QString input = row.value(QStringLiteral("input"));
            return input == QStringLiteral("enter") || input == QStringLiteral("control");
        };
        const QVector<Assignment> cases = pairwise(dimensions, validCase);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions, validCase));

        const QByteArray original("\0bcdefghijklmnop", 16);
        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-INVALID]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-invalid-contract-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex,
                     caseLabel.constData());
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), caseLabel.constData());
            QWidget *surface = contract.value(QStringLiteral("surface"))
                    == QStringLiteral("hex")
                ? static_cast<QWidget *>(tab->editor())
                : static_cast<QWidget *>(ascii);
            surface->setFocus(Qt::OtherFocusReason);
            if (contract.value(QStringLiteral("mode")) == QStringLiteral("INS"))
                QTest::keyClick(surface, Qt::Key_Insert);
            const bool atEnd = contract.value(QStringLiteral("boundary"))
                               == QStringLiteral("end");
            if (surface == tab->editor()) {
                tab->editor()->setCursorPosition(
                    0, atEnd ? tab->editor()->text().size() : 0);
            } else {
                QTextCursor cursor(ascii->document());
                cursor.setPosition(atEnd ? original.size() : 0);
                ascii->setTextCursor(cursor);
            }
            const QString beforeHex = tab->editor()->text();
            const QString beforeAscii = ascii->toPlainText();
            const QString input = contract.value(QStringLiteral("input"));
            if (input == QStringLiteral("G"))
                QTest::keyClicks(surface, QStringLiteral("G"));
            else if (input == QStringLiteral("space"))
                QTest::keyClick(surface, Qt::Key_Space);
            else if (input == QStringLiteral("enter"))
                QTest::keyClick(surface, Qt::Key_Return);
            else if (input == QStringLiteral("control")) {
                QKeyEvent controlEvent(QEvent::KeyPress, Qt::Key_unknown,
                                       Qt::NoModifier, QString(QChar(1)));
                QApplication::sendEvent(surface, &controlEvent);
            }
            else {
                QKeyEvent unicodeEvent(QEvent::KeyPress, Qt::Key_unknown,
                                       Qt::NoModifier, QStringLiteral("中"));
                QApplication::sendEvent(surface, &unicodeEvent);
            }
            QApplication::processEvents(QEventLoop::AllEvents, 80);

            QCOMPARE(tab->l2View()->readByteRange(
                         0, tab->l2View()->virtualFileSize()), original);
            QCOMPARE(tab->editor()->text(), beforeHex);
            QCOMPARE(ascii->toPlainText(), beforeAscii);
            QVERIFY2(!tab->isModified(), caseLabel.constData());
            QVERIFY2(!mw_->statusBar()->currentMessage().isEmpty(), caseLabel.constData());
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_ime_commit_preserves_byte_grid_contract() {
        closeAllEditorTabs(mw_);
        const QByteArray original = QByteArray::fromHex(
            "4885c00f8499008b01488bf8010f8f8e");
        const auto formattedHex = [](const QByteArray &bytes, int row) {
            QString text;
            for (int i = 0; i < bytes.size(); ++i) {
                if (i > 0)
                    text.append(i % row == 0 ? QLatin1Char('\n')
                                             : QLatin1Char(' '));
                text.append(QStringLiteral("%1").arg(
                    quint8(bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper());
            }
            return text;
        };

        for (const bool overwrite : {true, false}) {
            const QString mode = overwrite ? QStringLiteral("OVR")
                                           : QStringLiteral("INS");
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-ime-%1.bin").arg(mode.toLower()));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         qPrintable(source.errorString()));
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), qPrintable(mode));
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY2(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex,
                     qPrintable(mode));
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY2(ascii && ascii->isVisibleTo(tab), qPrintable(mode));

            // Native composition must never be rendered into either fixed
            // byte projection. The Hex pane rejects non-nibbles; the character
            // pane consumes a complete commit through the selected encoding.
            QVERIFY(!tab->editor()->testAttribute(Qt::WA_InputMethodEnabled));
            QVERIFY(ascii->testAttribute(Qt::WA_InputMethodEnabled));
            tab->editor()->setFocus(Qt::OtherFocusReason);
            if (!overwrite)
                QTest::keyClick(tab->editor(), Qt::Key_Insert);
            tab->editor()->setCursorPosition(0, 3); // byte 1, high nibble

            const QString pristineProjection = tab->editor()->text();
            QInputMethodEvent preedit(QStringLiteral("D8DB"), {});
            QApplication::sendEvent(tab->editor(), &preedit);
            QApplication::processEvents(QEventLoop::AllEvents, 30);
            QCOMPARE(tab->editor()->text(), pristineProjection);
            QCOMPARE(tab->l2View()->readByteRange(0, original.size()), original);

            // The whole IME commit is validated before the first byte is
            // changed. A valid-looking prefix plus Chinese/non-Hex tail must
            // not partially overwrite the file or leak into the grid.
            QInputMethodEvent invalidCommit;
            invalidCommit.setCommitString(QStringLiteral("D8中G"));
            QApplication::sendEvent(tab->editor(), &invalidCommit);
            QApplication::processEvents(QEventLoop::AllEvents, 30);
            QCOMPARE(tab->editor()->text(), pristineProjection);
            QCOMPARE(tab->l2View()->readByteRange(0, original.size()), original);
            QVERIFY(!tab->isModified());
            QVERIFY(!mw_->statusBar()->currentMessage().isEmpty());

            QInputMethodEvent validCommit;
            validCommit.setCommitString(QStringLiteral("D8DB"));
            QApplication::sendEvent(tab->editor(), &validCommit);
            QApplication::processEvents(QEventLoop::AllEvents, 80);

            QByteArray expected = original;
            if (overwrite) {
                expected[1] = char(0xd8);
                expected[2] = char(0xdb);
            } else {
                expected.insert(1, QByteArray::fromHex("d8db"));
            }
            QCOMPARE(tab->l2View()->virtualFileSize(), qint64(expected.size()));
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->editor()->text(),
                formattedHex(expected, tab->l2View()->hexBytesPerRow()), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->l2View()->readByteRange(0, expected.size()), expected, 1000);
            QVERIFY(tab->isModified());

            // A multibyte character commit is one byte transaction in the
            // character pane. OVR consumes its encoded byte count; INS inserts
            // it. No partial character or Unicode re-encoding reaches disk.
            QTextCursor characterCaret(ascii->document());
            characterCaret.setPosition(3);
            ascii->setTextCursor(characterCaret);
            ascii->setFocus(Qt::OtherFocusReason);
            QInputMethodEvent characterCommit;
            characterCommit.setCommitString(QStringLiteral("中"));
            QApplication::sendEvent(ascii, &characterCommit);
            QApplication::processEvents(QEventLoop::AllEvents, 30);
            const QByteArray chinese = QStringLiteral("中").toUtf8();
            if (overwrite)
                expected.replace(3, chinese.size(), chinese);
            else
                expected.insert(3, chinese);
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->l2View()->readByteRange(0, expected.size()), expected, 1000);
            QVERIFY(tab->editor()->text().contains(
                QStringLiteral("E4 B8 AD")));

            // Copy exposes the same decoded text externally and retains an
            // authoritative private raw-byte flavor for lossless in-app paste.
            QTextCursor chineseSelection(ascii->document());
            chineseSelection.setPosition(3);
            chineseSelection.setPosition(6, QTextCursor::KeepAnchor);
            ascii->setTextCursor(chineseSelection);
            QTest::keyClick(ascii, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("中"));
            const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
            QVERIFY(mime && mime->hasFormat(
                QString::fromLatin1(ByteClipboard::RawBytesMime)));
            QCOMPARE(mime->data(QString::fromLatin1(ByteClipboard::RawBytesMime)),
                     chinese);

            tab->undo();
            QByteArray beforeCharacterCommit = expected;
            if (overwrite)
                beforeCharacterCommit.replace(3, chinese.size(),
                                              original.mid(3, chinese.size()));
            else
                beforeCharacterCommit.remove(3, chinese.size());
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->l2View()->readByteRange(0, beforeCharacterCommit.size()),
                beforeCharacterCommit, 1000);
            tab->redo();
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->l2View()->readByteRange(0, expected.size()), expected, 1000);

            if (overwrite) {
                const QString screenshot = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/gui-hex-ime-safe.png");
                QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
            }

            // The byte-editor policy must not leak into ordinary text mode;
            QTRY_COMPARE_WITH_TIMEOUT(
                tab->l2View()->readByteRange(0, expected.size()), expected, 1000);
            QVERIFY(!tab->editor()->testAttribute(Qt::WA_InputMethodEnabled));

            QString saveError;
            QVERIFY2(persistPlain(tab, tab->filePath(), &saveError), qPrintable(saveError));
            QFile saved(path);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), expected);
            saved.close();

            closeAllEditorTabs(mw_);
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), qPrintable(mode));
            EditorTab *reopened = currentEditorTab(mw_);
            QVERIFY(reopened && reopened->l2View());
            QTRY_VERIFY_WITH_TIMEOUT(reopened->l2View()->isOpen(), 5000);
            QCOMPARE(reopened->l2View()->readByteRange(
                         0, reopened->l2View()->virtualFileSize()), expected);
            auto *reopenedCharacter = reopened->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY(reopenedCharacter);
            QVERIFY(reopenedCharacter->toPlainText().contains(
                QStringLiteral("中")));
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_ascii_direct_input_pairwise_contract() {
        using namespace GuiContractMatrix;
        closeAllEditorTabs(mw_);
        const QVector<Dimension> dimensions = {
            {QStringLiteral("mode"), {QStringLiteral("OVR"), QStringLiteral("INS")}},
            {QStringLiteral("selection"), {QStringLiteral("none"),
                                             QStringLiteral("forward"),
                                             QStringLiteral("reverse")}},
            {QStringLiteral("boundary"), {QStringLiteral("start"),
                                            QStringLiteral("middle"),
                                            QStringLiteral("end")}},
            {QStringLiteral("character"), {QStringLiteral("Q"), QStringLiteral("~")}},
        };
        const QVector<Assignment> cases = pairwise(dimensions);
        QVERIFY(!cases.isEmpty());
        QVERIFY(coversEveryValidPair(cases, dimensions));
        const QByteArray original("\0bcdefghijklmnop", 16);

        for (int caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const Assignment &contract = cases.at(caseIndex);
            const QByteArray caseLabel = caseId(contract).toUtf8();
            qInfo().noquote() << "[HEX-ASCII-TYPE]" << caseLabel;
            const QString path = tmpDir_.filePath(
                QStringLiteral("hex-ascii-type-%1.bin").arg(caseIndex));
            {
                QFile source(path);
                QVERIFY2(source.open(QIODevice::WriteOnly | QIODevice::Truncate),
                         caseLabel.constData());
                QCOMPARE(source.write(original), qint64(original.size()));
            }
            mw_->openFilesFromExternal(QStringList{path});
            QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 5000), caseLabel.constData());
            EditorTab *tab = currentEditorTab(mw_);
            QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
            auto *ascii = tab->findChild<QPlainTextEdit *>(
                QStringLiteral("HexAsciiPreview"));
            QVERIFY(ascii && ascii->isVisibleTo(tab));
            ascii->setFocus(Qt::OtherFocusReason);
            const bool overwrite = contract.value(QStringLiteral("mode"))
                                   == QStringLiteral("OVR");
            if (!overwrite)
                QTest::keyClick(ascii, Qt::Key_Insert);

            const QString boundary = contract.value(QStringLiteral("boundary"));
            const QString selection = contract.value(QStringLiteral("selection"));
            const int caret = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 8 : original.size();
            const int rangeStart = boundary == QStringLiteral("start") ? 0
                : boundary == QStringLiteral("middle") ? 7 : original.size() - 2;
            const int rangeEnd = rangeStart + 2;
            QTextCursor cursor(ascii->document());
            if (selection == QStringLiteral("none")) {
                cursor.setPosition(caret);
            } else if (selection == QStringLiteral("forward")) {
                cursor.setPosition(rangeStart);
                cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
            } else {
                cursor.setPosition(rangeEnd);
                cursor.setPosition(rangeStart, QTextCursor::KeepAnchor);
            }
            ascii->setTextCursor(cursor);

            const QByteArray typed = contract.value(QStringLiteral("character")).toLatin1();
            QByteArray expected = original;
            if (selection != QStringLiteral("none")) {
                expected.replace(rangeStart, rangeEnd - rangeStart, typed);
            } else if (!overwrite) {
                expected.insert(caret, typed);
            } else if (caret < expected.size()) {
                expected.replace(caret, 1, typed);
            }
            QTest::keyClicks(ascii, QString::fromLatin1(typed));
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QCOMPARE(tab->l2View()->readByteRange(
                         0, tab->l2View()->virtualFileSize()), expected);
            QCOMPARE(tab->l2View()->virtualFileSize(), qint64(expected.size()));
            QCOMPARE(tab->isModified(), expected != original);
            if (expected != original) {
                tab->undo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          original, 1000);
                tab->redo();
                QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                              0, tab->l2View()->virtualFileSize()),
                                          expected, 1000);
            }
            closeAllEditorTabs(mw_);
        }
    }

    void tc_hex_mainwindow_clipboard_actions_follow_focused_surface() {
        closeAllEditorTabs(mw_);
        const QByteArray original("\0bcdefghijklmnop", 16);
        const QString path = tmpDir_.filePath(QStringLiteral("hex-action-focus.bin"));
        {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(original), qint64(original.size()));
        }
        mw_->openFilesFromExternal(QStringList{path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->l2View() && tab->viewModeL2() == ViewMode::Hex);
        auto *ascii = tab->findChild<QPlainTextEdit *>(QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(tab));
        QAction *copyAction = findMenuActionByExactDisplay(mw_, QStringLiteral("复制"));
        QAction *cutAction = findMenuActionByExactDisplay(mw_, QStringLiteral("剪切"));
        QAction *pasteAction = findMenuActionByExactDisplay(mw_, QStringLiteral("粘贴"));
        QVERIFY(copyAction && cutAction && pasteAction);

        // Earlier pairwise cases intentionally trigger invalid-input beeps;
        // on Windows that can leave the test top-level inactive. Reactivate
        // the real window before asserting focus-routed QAction behavior.
        mw_->show();
        mw_->raise();
        mw_->activateWindow();
        QTRY_VERIFY(mw_->isActiveWindow());

        QTextCursor asciiSelection(ascii->document());
        asciiSelection.setPosition(1);
        asciiSelection.setPosition(3, QTextCursor::KeepAnchor);
        ascii->setTextCursor(asciiSelection);
        ascii->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(ascii->hasFocus());
        copyAction->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("bc"));

        cutAction->trigger();
        QByteArray afterCut = original;
        afterCut.remove(1, 2);
        QCOMPARE(tab->l2View()->readByteRange(
                     0, tab->l2View()->virtualFileSize()), afterCut);
        tab->undo();
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->readByteRange(
                                      0, tab->l2View()->virtualFileSize()),
                                  original, 1000);

        QTextCursor asciiCaret(ascii->document());
        asciiCaret.setPosition(1);
        ascii->setTextCursor(asciiCaret);
        ascii->setFocus(Qt::OtherFocusReason);
        QGuiApplication::clipboard()->setText(QStringLiteral("XY"));
        pasteAction->trigger();
        QByteArray afterPaste = original;
        afterPaste.replace(1, 2, QByteArray("XY"));
        QCOMPARE(tab->l2View()->readByteRange(
                     0, tab->l2View()->virtualFileSize()), afterPaste);

        tab->editor()->setSelection(0, 3, 0, 8); // bytes [1, 3)
        tab->editor()->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(tab->editor()->hasFocus());
        copyAction->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("58 59"));
        closeAllEditorTabs(mw_);
    }

    void tc_nine_mib_unknown_binary_unchanged_save_is_noop() {
        closeAllEditorTabs(mw_);
        const QString sourcePath = tmpDir_.filePath(
            QStringLiteral("nine-mib-unknown.dat"));
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QByteArray block(1024 * 1024, '\0');
            for (int i = 0; i < block.size(); i += 4096)
                block[i] = char((i / 4096) & 0xff);
            for (int i = 0; i < 9; ++i)
                QCOMPARE(source.write(block), qint64(block.size()));
        }
        QCOMPARE(QFileInfo(sourcePath).size(), 9LL * 1024 * 1024);

        mw_->openFilesFromExternal(QStringList{sourcePath});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 5000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->isBinaryDocument());
        QCOMPARE(tab->viewModeL2(), ViewMode::Hex);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View() && tab->l2View()->isOpen(), 5000);
        QVERIFY(!tab->isModified());

        // User journey HEX-BROWSE-001: the file opens as one coherent editor,
        // not a scroll area containing a second independently scrollable editor.
        auto visibleBars = [tab](Qt::Orientation orientation) {
            int count = 0;
            for (QScrollBar *bar : tab->findChildren<QScrollBar *>()) {
                if (bar->orientation() == orientation && bar->isVisibleTo(tab))
                    ++count;
            }
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(visibleBars(Qt::Vertical), 1, 2000);
        QCOMPARE(visibleBars(Qt::Horizontal), 0);
        QVERIFY(!tab->editor()->verticalScrollBar()->isVisibleTo(tab));
        QVERIFY(!tab->editor()->horizontalScrollBar()->isVisibleTo(tab));
        auto *ascii = tab->findChild<QPlainTextEdit *>(QStringLiteral("HexAsciiPreview"));
        QVERIFY(ascii && ascii->isVisibleTo(tab));
        QWidget *viewport = tab->l2View()->viewport();
        QCOMPARE(tab->editor()->geometry().topLeft(), QPoint(0, 0));
        QCOMPARE(ascii->geometry().top(), 0);
        QCOMPARE(ascii->geometry().right() + 1, viewport->width());
        QCOMPARE(tab->editor()->geometry().right() + 1, ascii->geometry().left());
        QCOMPARE(tab->editor()->height(), viewport->height());
        QCOMPARE(ascii->height(), viewport->height());

        // Drag the only scrollbar to a distant region. The address painted on
        // the first visible row must match that global row, then a real wheel
        // gesture must advance both the thumb and the rendered address.
        QScrollBar *global = tab->l2View()->verticalScrollBar();
        QVERIFY(global->isVisibleTo(tab));
        dragScrollBar(global, 0.70);
        QTRY_VERIFY_WITH_TIMEOUT(global->value() > global->maximum() / 2, 5000);
        auto visibleAddress = [tab]() -> qint64 {
            const int first = int(tab->editor()->SendScintilla(
                QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
            QByteArray margin(64, '\0');
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_MARGINGETTEXT,
                                         first, margin.data());
            bool ok = false;
            const qint64 address = QString::fromLatin1(margin.constData())
                                       .toLongLong(&ok, 16);
            return ok ? address : -1;
        };
        QTRY_COMPARE_WITH_TIMEOUT(visibleAddress(),
            qint64(global->value()) * tab->l2View()->hexBytesPerRow(), 5000);
        const int beforeWheel = global->value();
        const QPointF local(tab->editor()->viewport()->rect().center());
        QWheelEvent wheel(local,
                          tab->editor()->viewport()->mapToGlobal(local.toPoint()),
                          QPoint(), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(tab->editor()->viewport(), &wheel);
        QTRY_VERIFY_WITH_TIMEOUT(global->value() > beforeWheel, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(visibleAddress(),
            qint64(global->value()) * tab->l2View()->hexBytesPerRow(), 5000);

        // Put the visible projection on its final local row and then wheel from
        // the ASCII pane. A mature Hex editor must replace the bounded window
        // during the gesture; waiting for an old debounce timer produces a
        // visible pause followed by a jump and leaves the two byte panes stale.
        const int projectedLines = int(tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETLINECOUNT));
        const int linesOnScreen = qMax(1, int(tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN)));
        const int localLastFirst = qMax(0, projectedLines - linesOnScreen);
        const qint64 windowFirstAddress = [&]() {
            QByteArray margin(64, '\0');
            tab->editor()->SendScintilla(QsciScintillaBase::SCI_MARGINGETTEXT,
                                         0UL, static_cast<void *>(margin.data()));
            bool ok = false;
            const qint64 result = QString::fromLatin1(margin.constData())
                                      .toLongLong(&ok, 16);
            return ok ? result : qint64(-1);
        }();
        QVERIFY(windowFirstAddress >= 0);
        global->setValue(int(windowFirstAddress / tab->l2View()->hexBytesPerRow())
                         + localLastFirst);
        QTRY_COMPARE_WITH_TIMEOUT(visibleAddress(),
            qint64(global->value()) * tab->l2View()->hexBytesPerRow(), 5000);
        const qint64 beforeBoundaryWheelAddress = visibleAddress();
        const QPointF asciiLocal(ascii->viewport()->rect().center());
        QWheelEvent asciiWheel(
            asciiLocal, ascii->viewport()->mapToGlobal(asciiLocal.toPoint()),
            QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
            Qt::ScrollUpdate, false);
        QApplication::sendEvent(ascii->viewport(), &asciiWheel);
        QCoreApplication::processEvents();
        QVERIFY(global->value() * tab->l2View()->hexBytesPerRow()
                > beforeBoundaryWheelAddress);
        QCOMPARE(visibleAddress(),
                 qint64(global->value()) * tab->l2View()->hexBytesPerRow());
        QCOMPARE(ascii->cursorForPosition(QPoint(2, 2)).blockNumber(),
                 int(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETFIRSTVISIBLELINE)));

        const QString journeyScreenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-hex-nine-mib-single-scroll.png");
        QVERIFY2(mw_->grab().save(journeyScreenshot), qPrintable(journeyScreenshot));

        QAction *saveAction = findMenuActionByExactDisplay(mw_, QStringLiteral("保存"));
        QVERIFY(saveAction && !saveAction->isEnabled());
        DiskFileVersion before;
        QCOMPARE(captureDiskFileVersion(sourcePath, &before),
                 DiskFileState::Unchanged);
        QString error;
        QVERIFY2(persistPlain(tab, tab->filePath(), &error), qPrintable(error));
        DiskFileVersion after;
        QCOMPARE(captureDiskFileVersion(sourcePath, &after),
                 DiskFileState::Unchanged);
        QCOMPARE(after.fileIdentity, before.fileIdentity);
        QCOMPARE(after.modifiedMs, before.modifiedMs);
        QCOMPARE(after.sampleHash, before.sampleHash);
        QVERIFY(!QFileInfo::exists(sourcePath
                                  + QStringLiteral(".notepad-safe-save-tmp")));
        closeAllEditorTabs(mw_);
    }

    void tc_hex_binary_crash_recovery_restores_bytes_and_position() {
        const QString dataDir = tmpDir_.filePath(QStringLiteral("hex-session-data"));
        QDir().mkpath(dataDir);
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString sourcePath = tmpDir_.filePath(QStringLiteral("hex-session-source.bin"));
        QByteArray original(96, '\0');
        original[0] = 'B';
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(original), qint64(original.size()));
        }

        auto *first = new MainWindow;
        first->resize(1100, 700);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first));
        first->openFilesFromExternal({sourcePath});
        QVERIFY(waitForTabCountAndL2(first, 1, true, 5000));
        EditorTab *edited = currentEditorTab(first);
        QVERIFY(edited && edited->viewModeL2() == ViewMode::Hex);
        edited->gotoByteOffset(20);
        edited->editor()->setFocus(Qt::OtherFocusReason);
        QTest::keyClicks(edited->editor(), QStringLiteral("A5"));
        // Moving to an explicit byte selection crosses the same overlay ->
        // PatchList boundary used by navigation/save, so the provider can now
        // be checked without reaching into EditorTab internals.
        edited->gotoByteOffset(20, 1);
        QCOMPARE(edited->l2View()->readByteRange(20, 21),
                 QByteArray(1, char(0xa5)));
        QCOMPARE(edited->l2View()->selectionStart(), qint64(20));
        QCOMPARE(edited->l2View()->selectionEnd(), qint64(21));

        SessionManager session(dataDir);
        QVERIFY(session.saveSession({edited}, 0, true));
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.version, 2);
        QCOMPARE(stored.tabs.size(), 1);
        QCOMPARE(stored.tabs.first().presentation, QStringLiteral("hex"));
        QCOMPARE(stored.tabs.first().selectionStartByte, qint64(20));
        QCOMPARE(stored.tabs.first().selectionEndByte, qint64(21));
        DraftStore::SnapshotInfo draftInfo;
        QString inspectError;
        QVERIFY2(DraftStore::inspect(stored.tabs.first().autosavePath,
                                     &draftInfo, &inspectError), qPrintable(inspectError));
        QCOMPARE(draftInfo.kind, DraftStore::PayloadKind::BinaryPatchJournal);

        edited->shutdownL2BeforeClose();
        delete first; // Simulate process loss after the encrypted recovery point committed.
        QApplication::processEvents(QEventLoop::AllEvents, 100);

        auto *restoredWindow = new MainWindow;
        restoredWindow->resize(1100, 700);
        restoredWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restoredWindow));
        restoredWindow->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(restoredWindow), 1, 5000);
        EditorTab *restored = currentEditorTab(restoredWindow);
        QTRY_VERIFY_WITH_TIMEOUT(restored && restored->isBinaryDocument()
                                 && restored->l2View() && restored->l2View()->isOpen(), 5000);
        QCOMPARE(restored->viewModeL2(), ViewMode::Hex);
        QCOMPARE(restored->l2View()->readByteRange(20, 21),
                 QByteArray(1, char(0xa5)));
        QCOMPARE(restored->l2View()->selectionStart(), qint64(20));
        QCOMPARE(restored->l2View()->selectionEnd(), qint64(21));
        QVERIFY(restored->isModified());
        QTRY_VERIFY_WITH_TIMEOUT(restoredWindow->statusBar()->currentMessage().contains(
            QStringLiteral("含未保存修改")), 2000);
        QFile unchanged(sourcePath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), original);

        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-hex-session-recovery.png");
        QVERIFY2(restoredWindow->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(restoredWindow);
        restoredWindow->close();
        delete restoredWindow;
        qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());
    }

    void tc_unified_open_focuses_existing_document() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        const QString textPath = tmpDir_.filePath(QStringLiteral("open-once.log"));
        {
            QFile file(textPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray contents("INFO first\nERROR keep this selection\n");
            QCOMPARE(file.write(contents), qint64(contents.size()));
        }

        mw_->openFilesFromExternal({textPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *textTab = currentEditorTab(mw_);
        QVERIFY(textTab && !textTab->isL2Active());
        textTab->editor()->setSelection(1, 0, 1, 5);

        // The same Open action must focus the live editor without creating a
        // competing tab or disturbing the user's current selection.
        mw_->openFilesFromExternal({textPath});
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_), textTab);
        int fromLine = -1, fromIndex = -1, toLine = -1, toIndex = -1;
        textTab->editor()->getSelection(&fromLine, &fromIndex, &toLine, &toIndex);
        QCOMPARE(fromLine, 1);
        QCOMPARE(fromIndex, 0);
        QCOMPARE(toLine, 1);
        QCOMPARE(toIndex, 5);
        QCOMPARE(mw_->statusBar()->currentMessage(),
                 QStringLiteral("文件已在当前窗口中打开"));

#ifdef Q_OS_WIN
        // A hard-link alias is a different path to the same underlying file.
        // Windows file identity must still route it to the existing tab.
        const QString aliasPath = tmpDir_.filePath(QStringLiteral("open-once-alias.log"));
        const std::wstring aliasNative = QDir::toNativeSeparators(aliasPath).toStdWString();
        const std::wstring sourceNative = QDir::toNativeSeparators(textPath).toStdWString();
        QVERIFY2(::CreateHardLinkW(aliasNative.c_str(), sourceNative.c_str(), nullptr),
                 "CreateHardLinkW failed for duplicate-open GUI fixture");
        mw_->openFilesFromExternal({aliasPath});
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_), textTab);
#endif

        QByteArray binaryBytes(256, '\0');
        for (int i = 0; i < binaryBytes.size(); ++i)
            binaryBytes[i] = char(i);
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("open-once.bin"));
        {
            QFile file(binaryPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(binaryBytes), qint64(binaryBytes.size()));
        }
        mw_->openFilesFromExternal({binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        EditorTab *hexTab = currentEditorTab(mw_);
        QVERIFY(hexTab && hexTab->isL2Active());
        QVERIFY(hexTab->viewModeL2() == ViewMode::Hex);

        mw_->openFilesFromExternal({binaryPath});
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(tabCount(mw_), 2);
        QCOMPARE(currentEditorTab(mw_), hexTab);
        QVERIFY(hexTab->viewModeL2() == ViewMode::Hex);

        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-unified-open-existing.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_recent_files_deduplicate_identity_and_remove_missing_nonmodal() {
        closeAllEditorTabs(mw_);
        QMenu *recent = mw_->findChild<QMenu *>(QStringLiteral("file.recent"));
        QVERIFY(recent != nullptr);
        QAction *clearRecent = nullptr;
        for (QAction *action : recent->actions()) {
            if (action->text().contains(QStringLiteral("清除列表"))) {
                clearRecent = action;
                break;
            }
        }
        if (clearRecent) {
            clearRecent->trigger();
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        for (QAction *action : recent->actions())
            QVERIFY(action->data().toString().isEmpty());

        const QString sourcePath = tmpDir_.filePath(QStringLiteral("recent-source.log"));
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write("recent identity\n"), qint64(16));
        }
        const QString aliasPath = tmpDir_.filePath(QStringLiteral("recent-alias.log"));
#ifdef Q_OS_WIN
        const std::wstring aliasNative = QDir::toNativeSeparators(aliasPath).toStdWString();
        const std::wstring sourceNative = QDir::toNativeSeparators(sourcePath).toStdWString();
        QVERIFY2(::CreateHardLinkW(aliasNative.c_str(), sourceNative.c_str(), nullptr),
                 "CreateHardLinkW failed for recent-file identity fixture");
#else
        QVERIFY(QFile::link(sourcePath, aliasPath));
#endif

        mw_->openFilesFromExternal({sourcePath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        mw_->openFilesFromExternal({aliasPath});
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(tabCount(mw_), 1);

        QList<QAction *> fileActions;
        for (QAction *action : recent->actions()) {
            if (!action->data().toString().isEmpty())
                fileActions.append(action);
        }
        QCOMPARE(fileActions.size(), 1);
        QVERIFY(sameDiskFilePath(fileActions.first()->data().toString(), aliasPath));

        QVERIFY(QFile::remove(aliasPath));
        fileActions.first()->trigger();
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        fileActions.clear();
        for (QAction *action : recent->actions()) {
            if (!action->data().toString().isEmpty())
                fileActions.append(action);
        }
        QCOMPARE(fileActions.size(), 0);
        QVERIFY(mw_->statusBar()->currentMessage().contains(
            QStringLiteral("已从最近打开中移除")));
        closeAllEditorTabs(mw_);
    }

    void tc_tab_context_menu_complete_user_flow() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        const QString textPath = tmpDir_.filePath(QStringLiteral("tab-context.txt"));
        {
            QFile file(textPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("plain tab\n"), qint64(10));
        }
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("tab-context.bin"));
        {
            QByteArray bytes("BIN", 3);
            bytes.append('\0');
            bytes.append("payload", 7);
            QFile file(binaryPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(bytes), qint64(bytes.size()));
        }
        mw_->openFilesFromExternal({textPath, binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 4000);

        auto *tabs = mw_->findChild<QTabWidget *>();
        QVERIFY(tabs && tabs->tabBar());
        int textIndex = -1;
        int binaryIndex = -1;
        for (int i = 0; i < tabs->count(); ++i) {
            EditorTab *tab = qobject_cast<EditorTab *>(tabs->widget(i));
            if (!tab)
                continue;
            if (QFileInfo(tab->filePath()).absoluteFilePath()
                == QFileInfo(textPath).absoluteFilePath()) {
                textIndex = i;
            } else if (QFileInfo(tab->filePath()).absoluteFilePath()
                       == QFileInfo(binaryPath).absoluteFilePath()) {
                binaryIndex = i;
            }
        }
        QVERIFY(textIndex >= 0);
        QVERIFY(binaryIndex >= 0);

        auto visibleTabMenu = [this]() -> QMenu * {
            for (QMenu *menu : mw_->findChildren<QMenu *>()) {
                if (menu->objectName() == QStringLiteral("tabContextMenu")
                    && menu->isVisible()) {
                    return menu;
                }
            }
            return nullptr;
        };
        auto menuAction = [](QMenu *menu, const QString &name) -> QAction * {
            if (!menu)
                return nullptr;
            for (QAction *action : menu->actions()) {
                if (action->objectName() == name)
                    return action;
            }
            return nullptr;
        };
        auto openTabMenu = [tabs, &visibleTabMenu](int index) {
            const QPoint local = tabs->tabBar()->tabRect(index).center();
            QTest::mouseClick(tabs->tabBar(), Qt::RightButton,
                              Qt::NoModifier, local);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            // QTest's synthetic mouse release does not synthesize the native
            // QContextMenuEvent on every Windows Qt platform plugin. Deliver
            // that final event only when the real mouse sequence did not.
            if (!visibleTabMenu()) {
                QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                                        tabs->tabBar()->mapToGlobal(local));
                QApplication::sendEvent(tabs->tabBar(), &event);
            }
        };

        openTabMenu(textIndex);
        QTRY_VERIFY_WITH_TIMEOUT(visibleTabMenu() != nullptr, 2000);
        QMenu *menu = visibleTabMenu();
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.save")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.saveAs")));
        QAction *closeAction = menuAction(menu, QStringLiteral("tabContext.close"));
        QVERIFY(closeAction);
        QCOMPARE(closeAction->shortcut(), QKeySequence(QStringLiteral("Ctrl+W")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.closeOthers")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.closeAll")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.copyPath")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.reveal")));
        // Content decides the view; small text has no presentation switch.
        QVERIFY(!menuAction(menu, QStringLiteral("tabContext.switchPresentation")));
        QTest::qWait(80);
        const QString textMenuShot = QCoreApplication::applicationDirPath()
                                     + QStringLiteral("/gui-tab-context-text.png");
        QVERIFY2(menu->grab().save(textMenuShot), qPrintable(textMenuShot));

        QAction *copyPath = menuAction(menu, QStringLiteral("tabContext.copyPath"));
        QPointer<QMenu> firstMenuGuard(menu);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(copyPath).center());
        QTRY_VERIFY_WITH_TIMEOUT(firstMenuGuard.isNull() || !firstMenuGuard->isVisible(), 1000);
        QCOMPARE(QApplication::clipboard()->text(), QDir::toNativeSeparators(textPath));
        QCOMPARE(tabs->currentIndex(), textIndex);

        // Tab indices are stable because no tab was closed above.
        openTabMenu(binaryIndex);
        QTRY_VERIFY_WITH_TIMEOUT(visibleTabMenu() != nullptr, 2000);
        menu = visibleTabMenu();
        // A small binary stays in Hex; there is no switch to a text view.
        QVERIFY(!menuAction(menu, QStringLiteral("tabContext.switchPresentation")));
        EditorTab *binaryTab = qobject_cast<EditorTab *>(tabs->widget(binaryIndex));
        QVERIFY(binaryTab && binaryTab->isBinaryDocument());
        QCOMPARE(binaryTab->viewModeL2(), ViewMode::Hex);
        QAction *closeOthers = menuAction(menu, QStringLiteral("tabContext.closeOthers"));
        QVERIFY(closeOthers && closeOthers->isEnabled());
        QTest::qWait(80);
        const QString binaryMenuShot = QCoreApplication::applicationDirPath()
                                       + QStringLiteral("/gui-tab-context-binary.png");
        QVERIFY2(menu->grab().save(binaryMenuShot), qPrintable(binaryMenuShot));
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(closeOthers).center());
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        QCOMPARE(currentEditorTab(mw_), binaryTab);
        QVERIFY(binaryTab->isBinaryDocument());
        QVERIFY(binaryTab->viewModeL2() == ViewMode::Hex);

        closeAllEditorTabs(mw_);
    }

    void tc_gui_display_encoding_modified_small_file_uses_save_or_cancel() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        const QString path = tmpDir_.filePath(QStringLiteral("encoding-reopen.txt"));
        const QByteArray original = QStringLiteral("alpha 原始\n").toUtf8();
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(original), qint64(original.size()));
        }
        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Active());
        QCOMPARE(tab->encoding(), TextEncoding::Utf8);

        QAction *reopenAsUtf8Bom = mw_->findChild<QAction *>(
            QStringLiteral("encoding.display.utf8bom"));
        QVERIFY(reopenAsUtf8Bom && reopenAsUtf8Bom->isEnabled());
        tab->editor()->setText(QStringLiteral("UNSAVED 用户修改"));
        tab->setModified(true);

        bool cancelDialogSeen = false;
        QTimer cancelPoll;
        cancelPoll.setInterval(20);
        connect(&cancelPoll, &QTimer::timeout, this, [&]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->isVisible()
                && box->windowTitle() == QStringLiteral("保存修改")) {
                cancelDialogSeen = true;
                const QString screenshot = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/gui-encoding-reopen-unsaved.png");
                box->grab().save(screenshot);
                auto *button = qobject_cast<QPushButton *>(
                    box->button(QMessageBox::Cancel));
                if (button) QTest::mouseClick(button, Qt::LeftButton);
            }
        });
        cancelPoll.start();
        reopenAsUtf8Bom->trigger();
        cancelPoll.stop();
        QVERIFY(cancelDialogSeen);
        QCOMPARE(tab->editor()->text(), QStringLiteral("UNSAVED 用户修改"));
        QVERIFY(tab->isModified());
        QCOMPARE(tab->encoding(), TextEncoding::Utf8);

        bool saveDialogSeen = false;
        QTimer savePoll;
        savePoll.setInterval(20);
        connect(&savePoll, &QTimer::timeout, this, [&]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box && box->isVisible()
                && box->windowTitle() == QStringLiteral("保存修改")) {
                saveDialogSeen = true;
                auto *button = qobject_cast<QPushButton *>(box->button(QMessageBox::Save));
                if (button) QTest::mouseClick(button, Qt::LeftButton);
            }
        });
        savePoll.start();
        reopenAsUtf8Bom->trigger();
        savePoll.stop();
        QVERIFY(saveDialogSeen);
        QCOMPARE(tab->editor()->text(), QStringLiteral("UNSAVED 用户修改"));
        QVERIFY(!tab->isModified());
        QCOMPARE(tab->encoding(), TextEncoding::Utf8Bom);
        QCOMPARE(tab->displayEncoding(), TextEncoding::Utf8Bom);
        QVERIFY(reopenAsUtf8Bom->isChecked());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QStringLiteral("UNSAVED 用户修改").toUtf8());
        }
        closeAllEditorTabs(mw_);
    }

    void tc_gui_encoding_conversion_saves_new_file_and_opens_it() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("encoding-eol.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray initial("alpha\r\nbeta\r\n");
            QCOMPARE(file.write(initial), qint64(initial.size()));
        }

        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Active());
        QCOMPARE(tab->lineEndingName(), QStringLiteral("CRLF"));
        auto *eolStatus = mw_->findChild<QToolButton *>(QStringLiteral("status.eol"));
        QVERIFY(eolStatus);
        QVERIFY(eolStatus->text().contains(QStringLiteral("CRLF")));

        tab->editor()->setFocus();
        tab->editor()->setCursorPosition(1, 4);
        QTest::keyClick(tab->editor(), Qt::Key_End);
        QTest::keyClick(tab->editor(), Qt::Key_Return);
        QTest::keyClicks(tab->editor(), QStringLiteral("gamma"));
        QVERIFY(tab->isModified());
        QAction *saveAction = mw_->findChild<QAction *>(QStringLiteral("file.save"));
        QVERIFY(saveAction && saveAction->isEnabled());
        saveAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 3000);

        QByteArray utf8Bytes;
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            utf8Bytes = file.readAll();
        }
        QVERIFY(utf8Bytes.contains("beta\r\ngamma"));
        for (qsizetype i = 0; i < utf8Bytes.size(); ++i) {
            if (utf8Bytes.at(i) == '\n')
                QVERIFY2(i > 0 && utf8Bytes.at(i - 1) == '\r', "found a lone LF in CRLF document");
        }

        QAction *convertUtf16 = mw_->findChild<QAction *>(
            QStringLiteral("encoding.convert.utf16le"));
        QVERIFY(convertUtf16 && convertUtf16->isEnabled());
        const QString convertedPath = tmpDir_.filePath(
            QStringLiteral("encoding-eol-converted.txt"));
        bool saveAsSeen = false;
        QTimer saveAsPoll;
        saveAsPoll.setInterval(20);
        connect(&saveAsPoll, &QTimer::timeout, this, [&]() {
            auto *dialog = mw_->findChild<QFileDialog *>(
                QStringLiteral("encodingConversionSaveAsDialog"));
            if (!dialog || !dialog->isVisible())
                return;
            saveAsSeen = true;
            dialog->selectFile(convertedPath);
            static_cast<QDialog *>(dialog)->accept();
        });
        saveAsPoll.start();
        convertUtf16->trigger();
        saveAsPoll.stop();
        QVERIFY(saveAsSeen);
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        EditorTab *convertedTab = currentEditorTab(mw_);
        QVERIFY(convertedTab && convertedTab != tab);
        QCOMPARE(QFileInfo(convertedTab->filePath()).absoluteFilePath(),
                 QFileInfo(convertedPath).absoluteFilePath());
        QCOMPARE(convertedTab->encoding(), TextEncoding::Utf16LE);
        QCOMPARE(tab->encoding(), TextEncoding::Utf8);
        QVERIFY(!tab->isModified());
        QByteArray utf16Bytes;
        {
            QFile file(convertedPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            utf16Bytes = file.readAll();
        }
        QVERIFY(utf16Bytes.startsWith(QByteArray("\xff\xfe", 2)));
        const DecodeResult decoded = decodeBytes(utf16Bytes);
        QCOMPARE(decoded.encoding, TextEncoding::Utf16LE);
        QVERIFY(decoded.text.contains(QStringLiteral("beta\r\ngamma")));
        QCOMPARE(convertedTab->lineEndingName(), QStringLiteral("CRLF"));
        auto *encodingStatus = mw_->findChild<QToolButton *>(QStringLiteral("status.encoding"));
        QVERIFY(encodingStatus && encodingStatus->text().contains(QStringLiteral("UTF-16LE")));
        QVERIFY(eolStatus->text().contains(QStringLiteral("CRLF")));
        closeAllEditorTabs(mw_);
    }

    void tc_about_reports_pinned_supply_chain() {
        QAction *about = mw_->findChild<QAction *>(QStringLiteral("help.about"));
        QVERIFY(about);
        bool inspected = false;
        QTimer::singleShot(80, mw_, [&]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            const QString text = box->text();
            QVERIFY(text.contains(QStringLiteral("QScintilla：</b>2.14.1")));
            QVERIFY(text.contains(QStringLiteral("KeePassXC Core 2.7.12")));
            QVERIFY(text.contains(QStringLiteral("Botan 3.12.0")));
            QVERIFY(text.contains(QStringLiteral("PCRE2 10.47")));
            QVERIFY(text.contains(QStringLiteral("GNU GPL v3")));
            const QString screenshot = QCoreApplication::applicationDirPath()
                                       + QStringLiteral("/gui-about-supply-chain.png");
            QVERIFY2(box->grab().save(screenshot), qPrintable(screenshot));
            inspected = true;
            box->accept();
        });
        about->trigger();
        QVERIFY(inspected);
    }

    void tc_edit_commands_follow_real_selection_history_and_readonly_state() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("command-capabilities.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("alpha beta\n"), qint64(11));
        }
        QApplication::clipboard()->clear();
        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Active());
        auto *undo = mw_->findChild<QAction *>(QStringLiteral("edit.undo"));
        auto *redo = mw_->findChild<QAction *>(QStringLiteral("edit.redo"));
        auto *cut = mw_->findChild<QAction *>(QStringLiteral("edit.cut"));
        auto *copy = mw_->findChild<QAction *>(QStringLiteral("edit.copy"));
        auto *paste = mw_->findChild<QAction *>(QStringLiteral("edit.paste"));
        auto *selectAll = mw_->findChild<QAction *>(QStringLiteral("edit.selectAll"));
        auto *duplicate = mw_->findChild<QAction *>(QStringLiteral("edit.line.duplicate"));
        QVERIFY(undo && redo && cut && copy && paste && selectAll && duplicate);
        QVERIFY(!undo->isEnabled() && !redo->isEnabled());
        QVERIFY(!cut->isEnabled() && !copy->isEnabled() && !paste->isEnabled());
        QVERIFY(selectAll->isEnabled() && duplicate->isEnabled());

        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->setCursorPosition(0, 0);
        for (int i = 0; i < 5; ++i)
            QTest::keyClick(tab->editor(), Qt::Key_Right, Qt::ShiftModifier);
        QTRY_VERIFY(cut->isEnabled() && copy->isEnabled());
        QTest::keyClick(tab->editor(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("alpha"));
        QTRY_VERIFY(paste->isEnabled());

        QTest::keyClick(tab->editor(), Qt::Key_X, Qt::ControlModifier);
        QTRY_COMPARE(tab->editor()->text(), QStringLiteral(" beta\n"));
        QTRY_VERIFY(undo->isEnabled() && !redo->isEnabled());
        undo->trigger();
        QTRY_COMPARE(tab->editor()->text(), QStringLiteral("alpha beta\n"));
        QTRY_VERIFY(redo->isEnabled());

        tab->editor()->setReadOnly(true);
        tab->editor()->selectAll();
        QTRY_VERIFY(copy->isEnabled());
        QVERIFY(!cut->isEnabled() && !paste->isEnabled());
        QVERIFY(!duplicate->isEnabled());
        tab->editor()->setReadOnly(false);
        closeAllEditorTabs(mw_);
    }

    void removedCommandsAreAbsent() {
        for (const char *id : {"edit.line.transpose", "edit.lines.split", "view.toolbar"}) {
            QVERIFY2(mw_->findChild<QAction *>(QString::fromLatin1(id)) == nullptr, id);
        }
        QVERIFY(mw_->findChild<QToolBar *>(QStringLiteral("MainToolBar")) == nullptr);
        QMenu *edge = mw_->findChild<QMenu *>(QStringLiteral("view.edge"));
        QVERIFY(edge);
        QList<int> columns;
        for (QAction *action : edge->actions()) {
            if (action->objectName().startsWith(QStringLiteral("view.edge.column.")))
                columns.push_back(action->data().toInt());
        }
        QCOMPARE(columns, (QList<int>{80, 100, 120}));
    }

    void tc_view_appearance_and_panels_are_unified() {
        const QString dataDir = tmpDir_.filePath(QStringLiteral("appearance-settings"));
        QDir().mkpath(dataDir);
        {
            QSettings legacy(QDir(dataDir).filePath(QStringLiteral("settings.ini")),
                             QSettings::IniFormat);
            legacy.setValue(QStringLiteral("darkMode"), true);
            legacy.setValue(QStringLiteral("toolbarVisible"), false);
            legacy.setValue(QStringLiteral("statusBarVisible"), false);
            legacy.sync();
        }
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QAction *system = window->findChild<QAction *>(
            QStringLiteral("view.appearance.system"));
        QAction *light = window->findChild<QAction *>(
            QStringLiteral("view.appearance.light"));
        QAction *dark = window->findChild<QAction *>(
            QStringLiteral("view.appearance.dark"));
        QVERIFY(system && light && dark);
        QVERIFY(dark->isChecked());
        QVERIFY(!light->isChecked() && !system->isChecked());
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
        // A legacy toolbarVisible value is ignored: there is no toolbar.
        QAction *statusVisibility = window->findChild<QAction *>(
            QStringLiteral("view.statusBar"));
        QVERIFY(statusVisibility);
        QVERIFY(window->findChild<QToolBar *>(QStringLiteral("MainToolBar")) == nullptr);
        QVERIFY(!statusVisibility->isChecked() && !window->statusBar()->isVisible());
        statusVisibility->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window->statusBar()->isVisible(), 1000);

        const QString textPath = tmpDir_.filePath(QStringLiteral("appearance-document.txt"));
        {
            QFile file(textPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("appearance\n"), qint64(11));
        }
        window->openFilesFromExternal({textPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(window), 1, 3000);
        EditorTab *tab = currentEditorTab(window);
        QVERIFY(tab);
        QVERIFY(tab->editor()->paper().lightness() < 128);

        QMenu *edge = window->findChild<QMenu *>(QStringLiteral("view.edge"));
        QMenu *zoom = window->findChild<QMenu *>(QStringLiteral("view.zoom"));
        QVERIFY(edge && zoom);
        QVERIFY(window->findChild<QMenu *>(QStringLiteral("view.presentation")) == nullptr);

        QMenu *viewMenu = nullptr;
        for (QAction *topAction : window->menuBar()->actions()) {
            if (topAction->menu()
                && stripMnemonicsAndEllipsis(topAction->text()) == QStringLiteral("视图")) {
                viewMenu = topAction->menu();
                break;
            }
        }
        QVERIFY(viewMenu);
        QVERIFY(viewMenu->actions().contains(edge->menuAction()));
        QVERIFY(viewMenu->actions().contains(zoom->menuAction()));
        viewMenu->popup(window->mapToGlobal(QPoint(160, 90)));
        QTRY_VERIFY_WITH_TIMEOUT(viewMenu->isVisible(), 1000);
        QTest::qWait(80);
        const QString viewShot = QCoreApplication::applicationDirPath()
                                 + QStringLiteral("/gui-view-menu-structure.png");
        QVERIFY2(viewMenu->grab().save(viewShot), qPrintable(viewShot));
        viewMenu->close();

        QMenu *appearance = window->findChild<QMenu *>(QStringLiteral("view.appearance"));
        QVERIFY(appearance);
        appearance->popup(window->mapToGlobal(QPoint(160, 90)));
        QTRY_VERIFY_WITH_TIMEOUT(appearance->isVisible(), 1000);
        QTest::qWait(80);
        const QString appearanceShot = QCoreApplication::applicationDirPath()
                                       + QStringLiteral("/gui-view-appearance-menu.png");
        QVERIFY2(appearance->grab().save(appearanceShot), qPrintable(appearanceShot));
        appearance->close();

        light->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(light->isChecked(), 1000);
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= 128);
        QVERIFY(tab->editor()->paper().lightness() >= 128);
        {
            QSettings migrated(QDir(dataDir).filePath(QStringLiteral("settings.ini")),
                               QSettings::IniFormat);
            QCOMPARE(migrated.value(QStringLiteral("appearanceMode")).toString(),
                     QStringLiteral("light"));
            QVERIFY(!migrated.contains(QStringLiteral("darkMode")));
        }

        QMenu *panels = window->findChild<QMenu *>(QStringLiteral("view.panels"));
        QAction *searchResults = window->findChild<QAction *>(
            QStringLiteral("view.panel.searchResults"));
        QAction *tasks = window->findChild<QAction *>(QStringLiteral("tools.tasks"));
        QVERIFY(panels && searchResults && tasks);
        QVERIFY(panels->actions().contains(searchResults));
        QVERIFY(panels->actions().contains(tasks));
        QMenu *toolsMenu = nullptr;
        for (QAction *topAction : window->menuBar()->actions()) {
            if (topAction->menu()
                && stripMnemonicsAndEllipsis(topAction->text()) == QStringLiteral("工具")) {
                toolsMenu = topAction->menu();
                break;
            }
        }
        QVERIFY(toolsMenu && toolsMenu->actions().contains(tasks));

        panels->popup(window->mapToGlobal(QPoint(160, 90)));
        QTRY_VERIFY_WITH_TIMEOUT(panels->isVisible(), 1000);
        QTest::qWait(80);
        const QString panelsShot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-view-panels-menu.png");
        QVERIFY2(panels->grab().save(panelsShot), qPrintable(panelsShot));
        panels->close();

        searchResults->trigger();
        auto *searchDock = window->findChild<QDockWidget *>(
            QStringLiteral("SearchResultsDock"));
        QTRY_VERIFY_WITH_TIMEOUT(searchDock && searchDock->isVisible(), 1500);
        searchResults->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!searchDock->isVisible(), 1500);

        tasks->trigger();
        auto *taskPanel = window->findChild<AsyncTaskPanelDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(taskPanel && taskPanel->isVisible(), 1500);
        taskPanel->close();

        system->trigger();
        QVERIFY(system->isChecked());
        const QString windowShot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-view-system-appearance.png");
        QVERIFY2(window->grab().save(windowShot), qPrintable(windowShot));

        closeAllEditorTabs(window);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QVERIFY(!searchResults->isEnabled());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.position"))->isVisible());
        QVERIFY(!window->findChild<QToolButton *>(QStringLiteral("status.encoding"))->isVisible());
        QVERIFY(!window->findChild<QToolButton *>(QStringLiteral("status.context"))->isVisible());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.size"))->isVisible());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.modified"))->isVisible());
        window->close();
        delete window;
        qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());

        // Restore the primary test window's selected appearance after the
        // isolated settings-migration window changed the application palette.
        for (const QString &name : {QStringLiteral("view.appearance.system"),
                                    QStringLiteral("view.appearance.light"),
                                    QStringLiteral("view.appearance.dark")}) {
            QAction *action = mw_->findChild<QAction *>(name);
            if (action && action->isChecked()) {
                action->trigger();
                break;
            }
        }
    }

    // Every editor surface follows the theme tokens: editable text, windowed
    // read-only text and hex. Search hits use the token hit color.
    void appearanceRepaintsEverySurface() {
        closeAllEditorTabs(mw_);
        const QString textPath = tmpDir_.filePath(QStringLiteral("theme-text.txt"));
        const QString largePath = tmpDir_.filePath(QStringLiteral("theme-large.log"));
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("theme-small.bin"));
        {
            QFile text(textPath);
            QVERIFY(text.open(QIODevice::WriteOnly | QIODevice::Truncate));
            text.write("theme marker line\n");
            QFile large(largePath);
            QVERIFY(large.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray line(1023, 'x');
            for (int i = 0; i < 40 * 1024; ++i) {
                large.write(line);
                large.write("\n");
            }
            QFile binary(binaryPath);
            QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::Truncate));
            binary.write(QByteArray(64, '\0'));
        }
        mw_->openFilesFromExternal({textPath, largePath, binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 3, 5000);
        auto *tabs = mw_->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QList<EditorTab *> opened;
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *tab = qobject_cast<EditorTab *>(tabs->widget(i)))
                opened.push_back(tab);
        }
        QCOMPARE(opened.size(), 3);
        const auto surfacesOf = [](EditorTab *tab) {
            QList<QsciScintilla *> surfaces{tab->editor()};
            if (auto *provider = tab->windowedReadOnlyProvider())
                surfaces.push_back(provider->editor());
            return surfaces;
        };
        QAction *dark = mw_->findChild<QAction *>(QStringLiteral("view.appearance.dark"));
        QAction *light = mw_->findChild<QAction *>(QStringLiteral("view.appearance.light"));
        QVERIFY(dark && light);

        for (const bool useDark : {true, false}) {
            (useDark ? dark : light)->trigger();
            const ThemeTokens tokens = useDark ? ThemeTokens::dark() : ThemeTokens::light();
            QApplication::processEvents();
            QVERIFY(qApp->styleSheet().contains(tokens.paper.name(QColor::HexRgb),
                                                Qt::CaseInsensitive));
            for (EditorTab *tab : std::as_const(opened)) {
                for (QsciScintilla *surface : surfacesOf(tab)) {
                    QVERIFY2(surface && surface->paper() == tokens.paper,
                             qPrintable(tab->fileName()));
                }
            }
            EditorTab *text = opened.front();
            SearchMatch match;
            match.line = 0;
            match.index = 0;
            match.length = 5;
            text->setSearchHighlights({match});
            const long fore = text->editor()->SendScintilla(
                QsciScintillaBase::SCI_INDICGETFORE, 8UL);
            const QColor hit(int(fore & 0xff), int((fore >> 8) & 0xff),
                             int((fore >> 16) & 0xff));
            QCOMPARE(hit, tokens.hit);
            text->clearSearchHighlights();
        }
        closeAllEditorTabs(mw_);
    }

    // The menu bar is hidden behind the title bar's main menu button; every
    // shortcut, Alt and Alt+letter still work.
    void shortcutsWorkWithHiddenMenuBar() {
        closeAllEditorTabs(mw_);
        QVERIFY(!mw_->menuBar()->isVisible());
        QVERIFY(mw_->mainMenu());
        const QString path = tmpDir_.filePath(QStringLiteral("hidden-menu.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("alpha\n");
        }
        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab);
        mw_->activateWindow();
        tab->editor()->setFocus();
        QTest::keyClicks(tab->editor(), QStringLiteral("beta "));
        QVERIFY(tab->isModified());
        QTest::keyClick(tab->editor(), Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 3000);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.readAll().startsWith("beta "));
        }

        QAction *next = mw_->findChild<QAction *>(QStringLiteral("search.next"));
        QVERIFY(next);
        QSignalSpy nextSpy(next, &QAction::triggered);
        QTest::keyClick(tab->editor(), Qt::Key_F3);
        QCOMPARE(nextSpy.count(), 1);

        QTest::keyClick(tab->editor(), Qt::Key_R, Qt::ControlModifier);
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 2000);
        auto *replaceEdit = dialog->findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
        QVERIFY(replaceEdit && replaceEdit->isVisible());
        dialog->close();

        tab->editor()->setFocus();
        QTest::keyPress(tab->editor(), Qt::Key_Alt, Qt::AltModifier);
        QTest::keyRelease(tab->editor(), Qt::Key_Alt, Qt::NoModifier);
        QTRY_VERIFY_WITH_TIMEOUT(mw_->mainMenu()->isVisible(), 1000);
        mw_->mainMenu()->close();
        QTRY_VERIFY(!mw_->mainMenu()->isVisible());

        tab->editor()->setFocus();
        QTest::keyPress(tab->editor(), Qt::Key_Alt, Qt::AltModifier);
        QTest::keyClick(tab->editor(), Qt::Key_F, Qt::AltModifier);
        QTest::keyRelease(tab->editor(), Qt::Key_Alt, Qt::NoModifier);
        auto *fileMenu = mw_->findChild<QMenu *>(QStringLiteral("menu.file"));
        QVERIFY(fileMenu);
        QTRY_VERIFY_WITH_TIMEOUT(fileMenu->isVisible(), 1000);
        QVERIFY(!mw_->mainMenu()->isVisible());
        fileMenu->close();

        QSignalSpy paletteSpy(mw_->titleBar(), &TitleBar::commandPaletteRequested);
        tab->editor()->setFocus();
        QTest::keyClick(tab->editor(), Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(paletteSpy.count(), 1);
        closeAllEditorTabs(mw_);
    }

    // Status segments open menus made of the existing command actions, so
    // the status bar and the main menu always agree.
    void statusSegmentsReuseCommandActions() {
        closeAllEditorTabs(mw_);
        auto *encoding = mw_->findChild<QToolButton *>(QStringLiteral("status.encoding"));
        QVERIFY(encoding && encoding->menu());
        QSet<QAction *> displayActions;
        for (QAction *action : mw_->findChildren<QAction *>()) {
            if (action->objectName().startsWith(QStringLiteral("encoding.display.")))
                displayActions.insert(action);
        }
        QVERIFY(!displayActions.isEmpty());
        QSet<QAction *> segmentActions;
        QMenu *convert = nullptr;
        for (QAction *action : encoding->menu()->actions()) {
            if (action->menu())
                convert = action->menu();
            else if (!action->isSeparator())
                segmentActions.insert(action);
        }
        QCOMPARE(segmentActions, displayActions);
        QVERIFY(convert);
        QCOMPARE(convert->title(), QStringLiteral("转换编码另存"));
        for (QAction *action : convert->actions())
            QVERIFY(action->objectName().startsWith(QStringLiteral("encoding.convert.")));

        for (const auto &[segment, prefix] : {
                 std::pair{QStringLiteral("status.eol"), QStringLiteral("encoding.eol.")},
                 std::pair{QStringLiteral("status.context"), QStringLiteral("language.")},
                 std::pair{QStringLiteral("status.zoom"), QStringLiteral("view.zoom.")}}) {
            auto *button = mw_->findChild<QToolButton *>(segment);
            QVERIFY2(button && button->menu(), qPrintable(segment));
            for (QAction *action : button->menu()->actions()) {
                QVERIFY2(action->isSeparator()
                             || action->objectName().startsWith(prefix),
                         qPrintable(action->objectName()));
            }
        }

        const QString textPath = tmpDir_.filePath(QStringLiteral("segments.txt"));
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("segments.bin"));
        {
            QFile text(textPath);
            QVERIFY(text.open(QIODevice::WriteOnly | QIODevice::Truncate));
            text.write("segments\r\n");
            QFile binary(binaryPath);
            QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::Truncate));
            binary.write(QByteArray(32, '\0'));
        }
        auto *hexMode = mw_->findChild<QToolButton *>(QStringLiteral("status.hexEditMode"));
        auto *eol = mw_->findChild<QToolButton *>(QStringLiteral("status.eol"));
        QVERIFY(hexMode && eol);
        mw_->openFilesFromExternal({textPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        QTRY_VERIFY(!hexMode->isVisible());
        QTRY_COMPARE(eol->text(), QStringLiteral("CRLF"));

        mw_->openFilesFromExternal({binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        EditorTab *binary = currentEditorTab(mw_);
        QVERIFY(binary && binary->isBinaryDocument());
        QTRY_VERIFY(hexMode->isVisible());
        const bool overwrite = binary->hexOverwriteMode();
        QTest::mouseClick(hexMode, Qt::LeftButton);
        QCOMPARE(binary->hexOverwriteMode(), !overwrite);
        closeAllEditorTabs(mw_);
    }

    void tc_system_high_contrast_keeps_platform_palette() {
        const QString dataDir = tmpDir_.filePath(QStringLiteral("high-contrast-settings"));
        QDir().mkpath(dataDir);
        {
            QSettings settings(QDir(dataDir).filePath(QStringLiteral("settings.ini")),
                               QSettings::IniFormat);
            settings.setValue(QStringLiteral("appearanceMode"), QStringLiteral("system"));
            settings.sync();
        }
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        qputenv("NOTEPADSAFE_FORCE_HIGH_CONTRAST", QByteArrayLiteral("1"));

        auto *window = new MainWindow;
        window->resize(1100, 720);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(window->findChild<QAction *>(QStringLiteral("view.appearance.system"))
                    ->isChecked());
        QVERIFY2(qApp->styleSheet().isEmpty(),
                 "高对比度模式不能叠加应用自定义深色菜单 stylesheet");

        const QString path = tmpDir_.filePath(QStringLiteral("high-contrast.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("high contrast editor\n"), qint64(21));
        file.close();
        window->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(window), 1, 3000);
        EditorTab *tab = currentEditorTab(window);
        QVERIFY(tab != nullptr);
        const QPalette platform = QApplication::style()->standardPalette();
        QCOMPARE(tab->editor()->paper(), platform.color(QPalette::Base));
        QCOMPARE(tab->editor()->color(), platform.color(QPalette::Text));
        QVERIFY(qAbs(tab->editor()->paper().lightness()
                     - tab->editor()->color().lightness()) >= 128);

        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-system-high-contrast.png");
        QVERIFY2(window->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(window);
        window->close();
        delete window;
        qunsetenv("NOTEPADSAFE_FORCE_HIGH_CONTRAST");
        qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());

        // The palette is process-global; restore the primary window after the
        // isolated accessibility run.
        for (const QString &name : {QStringLiteral("view.appearance.system"),
                                    QStringLiteral("view.appearance.light"),
                                    QStringLiteral("view.appearance.dark")}) {
            QAction *action = mw_->findChild<QAction *>(name);
            if (action && action->isChecked()) {
                action->trigger();
                break;
            }
        }
    }

    void tc_external_change_save_conflict_reload_roundtrip() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        const QString path = tmpDir_.filePath(QStringLiteral("external-conflict.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("disk version 1\n"), qint64(15));
        }
        mw_->openFilesFromExternal(QStringList{path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab != nullptr);
        QVERIFY(tab->externalFileState() == EditorTab::ExternalFileState::Unchanged);

        // Real user editing path: focus Scintilla, select all and type.
        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->selectAll();
        QTest::keyClicks(tab->editor(), QStringLiteral("local unsaved edit"));
        QTRY_VERIFY_WITH_TIMEOUT(tab->isModified(), 1000);

        const QByteArray externalBytes("external writer wins\n");
        {
            QFile external(path);
            QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(external.write(externalBytes), qint64(externalBytes.size()));
        }
        QVERIFY(tab->externalFileState() == EditorTab::ExternalFileState::Changed);

        QAction *reloadAction = mw_->findChild<QAction *>(QStringLiteral("file.reload"));
        QVERIFY(reloadAction && reloadAction->isEnabled());
        QVERIFY(reloadAction->shortcut().isEmpty());
        QAction *saveAction = findMenuActionByExactDisplay(mw_, QStringLiteral("保存"));
        QVERIFY(saveAction != nullptr);

        bool conflictHandled = false;
        QTimer::singleShot(100, mw_, [this, &conflictHandled]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            const QString screenshot = QCoreApplication::applicationDirPath()
                                       + QStringLiteral("/gui-external-save-conflict.png");
            box->grab().save(screenshot);
            for (QPushButton *button : box->findChildren<QPushButton *>()) {
                if (button->text().contains(QStringLiteral("重新加载磁盘版本"))) {
                    conflictHandled = true;
                    QTest::mouseClick(button, Qt::LeftButton);
                    return;
                }
            }
        });
        saveAction->trigger();
        QVERIFY2(conflictHandled, "保存未显示外部修改冲突对话框，或缺少重新加载按钮");

        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text(), QString::fromUtf8(externalBytes), 3000);
        QVERIFY(!tab->isModified());
        QVERIFY(tab->externalFileState() == EditorTab::ExternalFileState::Unchanged);
        {
            QFile unchanged(path);
            QVERIFY(unchanged.open(QIODevice::ReadOnly));
            QCOMPARE(unchanged.readAll(), externalBytes);
        }
        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-external-save-reloaded.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_file_close_commands_and_ctrl_w_ownership() {
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);

        QAction *closeTab = mw_->findChild<QAction *>(QStringLiteral("file.close"));
        QAction *closeAll = mw_->findChild<QAction *>(QStringLiteral("file.closeAll"));
        QAction *wordWrap = findMenuActionByExactDisplay(mw_, QStringLiteral("自动换行"));
        QVERIFY(closeTab && closeAll && wordWrap);
        QVERIFY(!closeTab->isEnabled());
        QVERIFY(!closeAll->isEnabled());
        QVERIFY(closeTab->shortcut().matches(QKeySequence(QStringLiteral("Ctrl+W")))
                == QKeySequence::ExactMatch);
        QVERIFY(wordWrap->shortcut().isEmpty());

        const QString textPath = tmpDir_.filePath(QStringLiteral("close-clean.txt"));
        {
            QFile text(textPath);
            QVERIFY(text.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(text.write("clean text\n"), qint64(11));
        }
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("close-clean.bin"));
        {
            QFile binary(binaryPath);
            QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QByteArray bytes(128, '\0');
            for (int i = 0; i < bytes.size(); ++i)
                bytes[i] = char(i);
            QCOMPARE(binary.write(bytes), qint64(bytes.size()));
        }
        mw_->openFilesFromExternal(QStringList{textPath, binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(currentEditorTab(mw_)
                                 && currentEditorTab(mw_)->isL2Active(), 3000);
        QVERIFY(closeTab->isEnabled() && closeAll->isEnabled());

        bool menuCaptured = false;
        for (QMenu *menu : mw_->findChildren<QMenu *>()) {
            if (stripMnemonicsAndEllipsis(menu->title()) == QStringLiteral("文件")) {
                menu->popup(mw_->mapToGlobal(QPoint(8, mw_->menuBar()->height())));
                QTest::qWait(100);
                const QString screenshot = QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/gui-file-close-menu.png");
                QVERIFY2(menu->grab().save(screenshot), qPrintable(screenshot));
                menu->hide();
                menuCaptured = true;
                break;
            }
        }
        QVERIFY(menuCaptured);

        closeTab->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        QVERIFY(currentEditorTab(mw_) && !currentEditorTab(mw_)->isL2Active());
        closeAll->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 0, 3000);
        QVERIFY(!closeTab->isEnabled());
        QVERIFY(!closeAll->isEnabled());
    }

    void tc_mainwindow_find_shortcuts_preserve_query_scope_and_tab_order() {
        closeAllEditorTabs(mw_);
        const QString firstPath = tmpDir_.filePath(QStringLiteral("search-shortcut-a.txt"));
        const QString secondPath = tmpDir_.filePath(QStringLiteral("search-shortcut-b.txt"));
        {
            QFile first(firstPath);
            QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray firstBytes("head\nTARGET first\nmiddle\nTARGET second\ntail\n");
            QCOMPARE(first.write(firstBytes), qint64(firstBytes.size()));
            QFile second(secondPath);
            QVERIFY(second.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray secondBytes("prefix\nTARGET third\nsuffix\n");
            QCOMPARE(second.write(secondBytes), qint64(secondBytes.size()));
        }
        mw_->openFilesFromExternal({firstPath, secondPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        auto *tabs = mw_->findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        switchToTab(mw_, 0);
        auto *first = currentEditorTab(mw_);
        QVERIFY(first && !first->isL2Active());

        auto *findAction = mw_->findChild<QAction *>(QStringLiteral("search.find"));
        auto *nextAction = mw_->findChild<QAction *>(QStringLiteral("search.next"));
        auto *previousAction = mw_->findChild<QAction *>(QStringLiteral("search.previous"));
        QVERIFY(findAction && nextAction && previousAction);
        QCOMPARE(nextAction->shortcut(), QKeySequence(QStringLiteral("F3")));
        QCOMPARE(previousAction->shortcut(), QKeySequence(QStringLiteral("Shift+F3")));

        findAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(mw_->findChild<FindReplaceDialog *>() != nullptr, 1000);
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        QVERIFY(pattern && scope);
        pattern->setText(QStringLiteral("TARGET"));
        const int currentScope = scope->findData(0);
        const int allOpenScope = scope->findData(1);
        QVERIFY(currentScope >= 0 && allOpenScope >= 0);
        QCOMPARE(scope->itemText(allOpenScope), QStringLiteral("全部打开文件 (2)"));
        scope->setCurrentIndex(currentScope);
        dialog->close();

        first->resetSearchPosition(true);
        first->editor()->setFocus();
        QTest::keyClick(first->editor(), Qt::Key_F3);
        QTRY_COMPARE_WITH_TIMEOUT(first->editor()->selectedText(),
                                  QStringLiteral("TARGET"), 1000);
        const long firstHit = first->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        QTest::keyClick(first->editor(), Qt::Key_F3);
        QTRY_VERIFY_WITH_TIMEOUT(first->editor()->SendScintilla(
                                     QsciScintillaBase::SCI_GETSELECTIONSTART) > firstHit,
                                 1000);
        QTest::keyClick(first->editor(), Qt::Key_F3, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(first->editor()->SendScintilla(
                                      QsciScintillaBase::SCI_GETSELECTIONSTART),
                                  firstHit, 1000);

        findAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        QCOMPARE(pattern->text(), QStringLiteral("TARGET"));
        scope->setCurrentIndex(allOpenScope);
        dialog->close();

        // First F3 selects the second match in A; the next one must cross to B.
        const long allOpenFirstHit = first->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        QTest::keyClick(first->editor(), Qt::Key_F3);
        QTRY_VERIFY_WITH_TIMEOUT(
            tabs->currentIndex() == 0
                && first->editor()->SendScintilla(
                       QsciScintillaBase::SCI_GETSELECTIONSTART) > allOpenFirstHit,
            1000);
        QTest::keyClick(first->editor(), Qt::Key_F3);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), 1, 1000);
        auto *second = currentEditorTab(mw_);
        QVERIFY(second != nullptr);
        QCOMPARE(second->editor()->selectedText(), QStringLiteral("TARGET"));

        QTest::keyClick(second->editor(), Qt::Key_F3, Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), 0, 1000);
        QCOMPARE(first->editor()->selectedText(), QStringLiteral("TARGET"));
        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-mainwindow-search-shortcuts-scope.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_mainwindow_all_open_find_all_utf8_utf16_dock_jump() {
        closeAllEditorTabs(mw_);
        const QString firstPath = tmpDir_.filePath(QStringLiteral("search-dock-a.txt"));
        const QString secondPath = tmpDir_.filePath(QStringLiteral("search-dock-b.txt"));
        const QString firstText = QStringLiteral("前缀😀 TOKEN first\r\n尾行\r\n");
        const QString secondText = QStringLiteral("表头\r\n中文😀 TOKEN second\r\n结尾\r\n");
        {
            QFile first(firstPath);
            QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray bytes = firstText.toUtf8();
            QCOMPARE(first.write(bytes), qint64(bytes.size()));

            QByteArray utf16le("\xff\xfe", 2);
            utf16le.reserve(2 + secondText.size() * 2);
            for (QChar ch : secondText) {
                const ushort value = ch.unicode();
                utf16le.append(char(value & 0xff));
                utf16le.append(char(value >> 8));
            }
            QFile second(secondPath);
            QVERIFY(second.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(second.write(utf16le), qint64(utf16le.size()));
        }

        mw_->openFilesFromExternal({firstPath, secondPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        auto *tabs = mw_->findChild<QTabWidget *>();
        auto *findAction = mw_->findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = mw_->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        QVERIFY(tabs && findAction && dock);
        switchToTab(mw_, 1);
        auto *second = currentEditorTab(mw_);
        QVERIFY(second && !second->isL2Active());
        QCOMPARE(second->encoding(), TextEncoding::Utf16LE);

        findAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(mw_->findChild<FindReplaceDialog *>() != nullptr, 1000);
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *tree = dock->findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY(pattern && scope && findAll && tree);
        pattern->setText(QStringLiteral("TOKEN"));
        scope->setCurrentIndex(scope->findData(1));
        QTest::mouseClick(findAll, Qt::LeftButton);

        QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 2, 3000);
        QCOMPARE(scope->currentData().toInt(), 1);
        QCOMPARE(scope->itemText(scope->currentIndex()),
                 QStringLiteral("全部打开文件 (2)"));

        int secondGroupRow = -1;
        for (int row = 0; row < tree->model()->rowCount(); ++row) {
            const QModelIndex group = tree->model()->index(row, 0);
            if (group.data().toString().contains(QStringLiteral("search-dock-b.txt"))) {
                secondGroupRow = row;
                break;
            }
        }
        QVERIFY(secondGroupRow >= 0);
        const QModelIndex secondGroup = tree->model()->index(secondGroupRow, 0);
        QCOMPARE(tree->model()->rowCount(secondGroup), 1);
        tree->expand(secondGroup);
        const QModelIndex context = tree->model()->index(
            0, SearchResultsModel::COL_CONTEXT, secondGroup);
        QVERIFY(context.isValid());
        QCOMPARE(context.data(SearchResultsModel::ContextRole).toString(),
                 QStringLiteral("中文😀 TOKEN second"));
        const QVariantList ranges =
            context.data(SearchResultsModel::MatchByteRangesRole).toList();
        QCOMPARE(ranges.size(), 1);

        QSignalSpy jumpByteSpy(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        switchToTab(mw_, 0);
        const QModelIndex jumpIndex = tree->model()->index(
            0, SearchResultsModel::COL_LINE, secondGroup);
        QVERIFY(jumpIndex.isValid());
        tree->scrollTo(jumpIndex, QAbstractItemView::PositionAtCenter);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QRect rect = tree->visualRect(jumpIndex);
        QVERIFY(rect.isValid());
        QVERIFY(tree->viewport()->rect().contains(rect.center()));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                          Qt::NoModifier, rect.center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                           Qt::NoModifier, rect.center());
        QTRY_COMPARE_WITH_TIMEOUT(jumpByteSpy.count(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentIndex(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(second->editor()->selectedText(),
                                  QStringLiteral("TOKEN"), 2000);
        QCOMPARE(second->editor()->SendScintilla(
                     QsciScintillaBase::SCI_INDICATORVALUEAT, 8,
                     second->editor()->SendScintilla(
                         QsciScintillaBase::SCI_GETSELECTIONSTART)), 1L);

        dialog->close();
        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-mainwindow-search-utf16-dock-jump.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_external_open_preserves_dirty_and_active_search() {
        closeAllEditorTabs(mw_);
        const QString firstPath = tmpDir_.filePath(
            QStringLiteral("external-boundary-a.txt"));
        const QString secondPath = tmpDir_.filePath(
            QStringLiteral("external-boundary-b.txt"));
        const QString externalPath = tmpDir_.filePath(
            QStringLiteral("external-boundary-new.txt"));
        const auto writeText = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write(bytes) == bytes.size();
        };
        QVERIFY(writeText(firstPath, QByteArray("TOKEN first on disk\n")));
        QVERIFY(writeText(secondPath, QByteArray("TOKEN second on disk\n")));
        QVERIFY(writeText(externalPath,
                          QByteArray("TOKEN external joins active search\n")));

        mw_->openFilesFromExternal({firstPath, secondPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 3000);
        auto *tabs = mw_->findChild<QTabWidget *>();
        QVERIFY(tabs);
        switchToTab(mw_, 0);
        auto *dirty = currentEditorTab(mw_);
        QVERIFY(dirty);
        dirty->editor()->append(QStringLiteral("unsaved dirty marker"));
        dirty->setModified(true);
        const QString dirtyText = dirty->editor()->text();
        QFile disk(firstPath);
        QVERIFY(disk.open(QIODevice::ReadOnly));
        const QByteArray diskBefore = disk.readAll();
        disk.close();

        auto *findAction = mw_->findChild<QAction *>(QStringLiteral("search.find"));
        auto *dock = mw_->findChild<SearchResultsDock *>(
            QStringLiteral("SearchResultsDock"));
        QVERIFY(findAction && dock);
        findAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(
            mw_->findChild<FindReplaceDialog *>() != nullptr, 1000);
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(
            QStringLiteral("findPatternEdit"));
        auto *scope = dialog->findChild<QComboBox *>(
            QStringLiteral("searchScopeCombo"));
        auto *findAll = dialog->findChild<QPushButton *>(
            QStringLiteral("findAllButton"));
        auto *cancel = dialog->findChild<QPushButton *>(
            QStringLiteral("cancelSearchButton"));
        auto *tree = dock->findChild<QTreeView *>(
            QStringLiteral("searchResultsTree"));
        auto *summary = dock->findChild<QLabel *>(
            QStringLiteral("searchResultsSummary"));
        QVERIFY(pattern && scope && findAll && cancel && tree && summary);
        QSignalSpy clearSpy(dock,
                            &SearchResultsDock::clearSearchHighlightsRequested);
        pattern->setText(QStringLiteral("TOKEN"));
        scope->setCurrentIndex(scope->findData(1));
        QTest::mouseClick(findAll, Qt::LeftButton);
        QVERIFY(cancel->isVisible() && cancel->isEnabled());
        QAbstractItemModel *const frozenModel = tree->model();
        QVERIFY(frozenModel);

        mw_->openFilesFromExternal({externalPath});
        QCOMPARE(tabCount(mw_), 3);
        QCOMPARE(clearSpy.count(), 0);
        QVERIFY2(cancel->isVisible(),
                 "external open cancelled the active all-open search");
        QCOMPARE(tabs->indexOf(dirty) >= 0, true);
        QVERIFY(dirty->isModified());
        QCOMPARE(dirty->editor()->text(), dirtyText);
        QVERIFY(disk.open(QIODevice::ReadOnly));
        QCOMPARE(disk.readAll(), diskBefore);
        disk.close();

        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 5000);
        QVERIFY(!summary->text().contains(QStringLiteral("已取消")));
        QCOMPARE(tree->model(), frozenModel);
        QCOMPARE(frozenModel->rowCount(), 3);
        const auto verifyResultGroup = [frozenModel](const QString &fileName) {
            for (int row = 0; row < frozenModel->rowCount(); ++row) {
                const QModelIndex group = frozenModel->index(row, 0);
                if (group.data().toString().contains(fileName)) {
                    return frozenModel->rowCount(group) > 0;
                }
            }
            return false;
        };
        QVERIFY(verifyResultGroup(QStringLiteral("external-boundary-a.txt")));
        QVERIFY(verifyResultGroup(QStringLiteral("external-boundary-b.txt")));
        QVERIFY(verifyResultGroup(QStringLiteral("external-boundary-new.txt")));
        QCOMPARE(clearSpy.count(), 0);
        dialog->close();
        closeAllEditorTabs(mw_);
    }

    void tc_background_task_phased_progress_contract() {
        auto &manager = AsyncTaskManager::instance();
        manager.cancelAllByType();
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        manager.clearHistory();

        const auto release = std::make_shared<std::atomic_bool>(false);
        const qint64 taskId = manager.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Normal,
            QStringLiteral("分段进度契约"), QStringLiteral("验证精确进度和阶段历史"),
            [release](qint64, AsyncCancelPtr cancel,
                      const std::function<void(qint64, qint64)> &,
                      const std::function<void(const AsyncErrorInfo &)> &) {
                while (!release->load(std::memory_order_acquire)
                       && !(cancel && cancel->load(std::memory_order_acquire))) {
                    QThread::msleep(10);
                }
            });
        QTRY_VERIFY_WITH_TIMEOUT(
            manager.taskById(taskId).state == AsyncTaskState::Running, 3000);

        manager.updateTaskPhase(taskId, QStringLiteral("扫描源数据"),
                                1, 2, 1234, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskId).percentHundredths(),
                                  1234, 1000);
        AsyncTask phaseOne = manager.taskById(taskId);
        QCOMPARE(phaseOne.progressPhase, QStringLiteral("扫描源数据"));
        QCOMPARE(phaseOne.progressPhaseIndex, 1);
        QCOMPARE(phaseOne.progressPhaseCount, 2);
        QCOMPARE(phaseOne.estimatedRemainingMs(), qint64(-1));

        // A late report in the same phase cannot make visible progress regress.
        manager.updateTaskProgress(taskId, 1000, 10000);
        QCOMPARE(manager.taskById(taskId).percentHundredths(), 1234);

        manager.updateTaskPhase(taskId, QStringLiteral("整理结果"),
                                2, 2, 1, 3);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(taskId).progressPhaseIndex,
                                  2, 1000);
        const AsyncTask phaseTwo = manager.taskById(taskId);
        QCOMPARE(phaseTwo.percentHundredths(), 3333);
        QVERIFY(phaseTwo.completedPhases.contains(QStringLiteral("扫描源数据")));
        QCOMPARE(phaseTwo.estimatedRemainingMs(), qint64(-1));

        AsyncTaskPanelDialog panel(mw_);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>();
        auto *details = panel.findChild<QPlainTextEdit *>();
        QVERIFY(list && details);
        QModelIndex taskIndex;
        for (int row = 0; row < manager.rowCount(); ++row) {
            const QModelIndex candidate = manager.index(row, 0);
            if (candidate.data(AsyncTaskManager::TaskIdRole).toLongLong()
                == taskId) {
                taskIndex = candidate;
                break;
            }
        }
        QVERIFY(taskIndex.isValid());
        list->setCurrentIndex(taskIndex);
        QTRY_VERIFY_WITH_TIMEOUT(
            details->toPlainText().contains(QStringLiteral("当前阶段: 2/2  整理结果")),
            1000);
        QVERIFY(details->toPlainText().contains(QStringLiteral("已完成阶段: 扫描源数据")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("33.33%")));
        panel.close();

        release->store(true, std::memory_order_release);
        QTRY_VERIFY_WITH_TIMEOUT(
            manager.taskById(taskId).state == AsyncTaskState::Finished, 3000);
        manager.clearHistory();
    }

    void tc_background_task_center_lifecycle() {
        auto &manager = AsyncTaskManager::instance();
        QAbstractItemModelTester modelTester(
            &manager, QAbstractItemModelTester::FailureReportingMode::Fatal, this);
        manager.cancelAllByType();
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        manager.clearHistory();

        auto *chip = mw_->findChild<QToolButton *>(
            QStringLiteral("StatusBarTaskWidget_btn"));
        QVERIFY(chip != nullptr);
        QVERIFY(!chip->isVisible());

        const qint64 activeId = manager.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Low,
            QStringLiteral("可取消后台维护"), QStringLiteral("测试取消收敛"),
            [](qint64, AsyncCancelPtr cancel,
               const std::function<void(qint64, qint64)> &reportProgress,
               const std::function<void(const AsyncErrorInfo &)> &) {
                for (qint64 step = 0; step < 500; ++step) {
                    if (cancel && cancel->load(std::memory_order_acquire))
                        return;
                    reportProgress(step, 500);
                    QThread::msleep(100);
                }
            });
        const qint64 failedId = manager.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Normal,
            QStringLiteral("失败任务"), QStringLiteral("可见错误摘要"),
            [](qint64, AsyncCancelPtr,
               const std::function<void(qint64, qint64)> &,
               const std::function<void(const AsyncErrorInfo &)> &) {
                throw std::runtime_error("simulated task failure");
            });
        const qint64 reportedFailureId = manager.submit(
            AsyncTaskType::Other, AsyncTaskPriority::Normal,
            QStringLiteral("业务失败任务"), QStringLiteral("结构化错误"),
            [](qint64, AsyncCancelPtr,
               const std::function<void(qint64, qint64)> &,
               const std::function<void(const AsyncErrorInfo &)> &reportError) {
                AsyncErrorInfo error;
                error.message = QStringLiteral("simulated reported failure");
                reportError(error);
            });

        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(activeId).state
                                 == AsyncTaskState::Running, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(failedId).state
                                 == AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(reportedFailureId).state
                                 == AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(chip->isVisible(), 3000);
        QVERIFY(chip->text().contains(QStringLiteral("失败 2")));

        QFrame *longBubble = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (longBubble = mw_->findChild<QFrame *>(
                 QStringLiteral("longTaskNotification")))
                && longBubble->isVisible(),
            7000);
        auto *viewLongTask = longBubble->findChild<QPushButton *>(
            QStringLiteral("viewLongTaskButton"));
        QVERIFY(viewLongTask && viewLongTask->isVisible());
        QTest::mouseClick(viewLongTask, Qt::LeftButton);
        auto *panel = mw_->findChild<AsyncTaskPanelDialog *>(
            QStringLiteral("backgroundTaskPanel"));
        QVERIFY(panel != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible(), 2000);
        auto *cancelAll = panel->findChild<QPushButton *>(
            QStringLiteral("cancelAllTasksButton"));
        auto *clearHistory = panel->findChild<QPushButton *>(
            QStringLiteral("clearTaskHistoryButton"));
        auto *failedSummary = panel->findChild<QLabel *>(
            QStringLiteral("taskSummaryFailed"));
        auto *threadSummary = panel->findChild<QLabel *>(
            QStringLiteral("taskSummaryThreads"));
        QVERIFY(cancelAll && clearHistory && failedSummary && threadSummary);
        auto *technicalDetails = panel->findChild<QAbstractButton *>(
            QStringLiteral("backgroundTaskDetailsToggle"));
        QVERIFY(technicalDetails && technicalDetails->isVisible());
        QVERIFY(!threadSummary->isVisible());
        technicalDetails->setFocus();
        QTest::keyClick(technicalDetails, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(threadSummary->isVisible(), 1000);
        QVERIFY(cancelAll->isEnabled());
        QVERIFY(clearHistory->isEnabled());
        QCOMPARE(failedSummary->text(), QStringLiteral("失败: 2"));
        const int expectedThreads = qBound(
            1, QThread::idealThreadCount() - 1, 12);
        QCOMPARE(manager.maximumThreadCount(), expectedThreads);
        QTRY_VERIFY_WITH_TIMEOUT(
            threadSummary->text().endsWith(
                QStringLiteral("/%1").arg(expectedThreads)), 2000);
        QVERIFY(manager.activeThreadCount() >= 1);
        QVERIFY(manager.activeThreadCount() <= expectedThreads);
        // Thread occupancy remains exact in the opt-in technical view, not
        // the default product tooltip. Do not relabel threads as tasks.
        QVERIFY(!chip->toolTip().contains(QStringLiteral("工作线程")));
        QVERIFY(!chip->toolTip().contains(
            QStringLiteral("/%1").arg(expectedThreads)));
        QVERIFY(chip->toolTip().contains(QStringLiteral("活跃任务 1")));
        QVERIFY(chip->toolTip().contains(QStringLiteral("预计")));

        const QString runningShot = QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/gui-background-tasks-running.png");
        QVERIFY2(panel->grab().save(runningShot), qPrintable(runningShot));

        QTest::mouseClick(cancelAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(activeId).state
                                 == AsyncTaskState::Canceled, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(chip->isVisible(), 2000);
        QCOMPARE(chip->text(), QStringLiteral("任务失败 2"));
        QVERIFY(!cancelAll->isEnabled());

        const QString failedShot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-background-tasks-failed.png");
        QVERIFY2(panel->grab().save(failedShot), qPrintable(failedShot));

        QTest::mouseClick(clearHistory, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(manager.rowCount(), 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!chip->isVisible(), 2000);
        panel->hide();
        mw_->show();
        mw_->raise();
        mw_->activateWindow();
    }

    void tc_native_editor_commands_and_capability_routing() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("native-commands.js"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray source("alpha alpha\r\nif (ready) {\r\n  beta\r\n}\r\n");
        QCOMPARE(file.write(source), qint64(source.size()));
        file.close();
        mw_->openFilesFromExternal({path});
        auto *tabs = mw_->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 2000);
        auto *tab = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(tab != nullptr);

        auto action = [this](const char *name) {
            return mw_->findChild<QAction *>(QString::fromLatin1(name));
        };
        auto *whitespace = action("view.symbols.whitespace");
        auto *eolVisible = action("view.symbols.eol");
        auto *toLf = action("encoding.eol.lf");
        auto *upper = action("edit.selection.upper");
        auto *selectAllSame = action("edit.multiSelect.all");
        auto *javascript = action("language.javascript");
        auto *foldAll = action("view.folding.collapseAll");
        auto *useTabs = action("edit.indentation.useTabs");
        auto *tabWidth8 = action("edit.indentation.tabWidth.8");
        auto *completion = action("edit.completion.words");
        QVERIFY(whitespace && eolVisible && toLf && upper && selectAllSame
                && javascript && foldAll && useTabs && tabWidth8
                && completion);

        whitespace->setChecked(true);
        eolVisible->setChecked(true);
        QCOMPARE(tab->whitespaceVisible(), true);
        QCOMPARE(tab->eolVisible(), true);
        QVERIFY(!tab->isModified());

        javascript->trigger();
        QCOMPARE(tab->syntax(), Syntax::JavaScript);
        QVERIFY(tab->editor()->lexer() != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(foldAll->isEnabled(), 1000);

        tab->editor()->setSelection(0, 0, 0, 5);
        upper->trigger();
        QVERIFY(tab->editor()->text().startsWith(QStringLiteral("ALPHA alpha")));
        QVERIFY(tab->isModified());

        tab->editor()->setText(QStringLiteral("alpha alpha\r\n"));
        tab->editor()->setSelection(0, 0, 0, 5);
        selectAllSame->trigger();
        QCOMPARE(tab->editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETSELECTIONS), 2L);
        QTest::keyClicks(tab->editor(), QStringLiteral("X"));
        QCOMPARE(tab->editor()->text(), QStringLiteral("X X\r\n"));

        toLf->trigger();
        QCOMPARE(tab->editor()->eolMode(), QsciScintilla::EolUnix);
        QCOMPARE(tab->editor()->text(), QStringLiteral("X X\n"));
        useTabs->setChecked(true);
        tabWidth8->trigger();
        completion->setChecked(false);
        QCOMPARE(tab->useTabs(), true);
        QCOMPARE(tab->tabWidth(), 8);
        QCOMPARE(tab->wordCompletionEnabled(), false);

        if (auto *searchDock = mw_->findChild<QDockWidget *>(
                QStringLiteral("SearchResultsDock"))) {
            searchDock->hide();
        }
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-native-editor-commands.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));

        closeAllEditorTabs(mw_);
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("native-capability.bin"));
        QFile binary(binaryPath);
        QVERIFY(binary.open(QIODevice::WriteOnly));
        QCOMPARE(binary.write(QByteArray::fromHex("00414243")), qint64(4));
        binary.close();
        mw_->openFilesFromExternal({binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!upper->isEnabled() && !toLf->isEnabled()
                                 && !foldAll->isEnabled(), 2000);
        closeAllEditorTabs(mw_);
    }

    void tc_status_bar_reports_unicode_characters_and_hex_bytes() {
        closeAllEditorTabs(mw_);
        auto *statusCharacters = mw_->findChild<QLabel *>(
            QStringLiteral("status.characters"));
        QVERIFY(statusCharacters != nullptr);

        const QString textPath = tmpDir_.filePath(QStringLiteral("character-count.txt"));
        QFile textFile(textPath);
        QVERIFY(textFile.open(QIODevice::WriteOnly));
        QCOMPARE(textFile.write(QStringLiteral("A中😀\n").toUtf8()), qint64(9));
        textFile.close();

        mw_->openFilesFromExternal({textPath});
        auto *tabs = mw_->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 2000);
        auto *textTab = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(textTab != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(statusCharacters->text(),
                                  QStringLiteral("字符数: 4"), 2000);
        textTab->editor()->append(QStringLiteral("Z"));
        QTRY_COMPARE_WITH_TIMEOUT(statusCharacters->text(),
                                  QStringLiteral("字符数: 5"), 2000);

        closeAllEditorTabs(mw_);
        const QString binaryPath = tmpDir_.filePath(QStringLiteral("character-count.bin"));
        QFile binaryFile(binaryPath);
        QVERIFY(binaryFile.open(QIODevice::WriteOnly));
        const QByteArray binary = QByteArray::fromHex("000102FF");
        QCOMPARE(binaryFile.write(binary), qint64(binary.size()));
        binaryFile.close();

        mw_->openFilesFromExternal({binaryPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(statusCharacters->text(),
                                  QStringLiteral("字节数: 4"), 2000);
        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-status-character-count.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_gui_search_assistant_builds_and_runs_a_real_query() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("gui-search-assistant.log"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray contents(
                "event.id=4821 level=info\n"
                "eventXid=99 level=noise\n"
                "event.id=77 level=warn\n");
            QCOMPARE(file.write(contents), qint64(contents.size()));
        }
        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Active());

        auto *findAction = mw_->findChild<QAction *>(QStringLiteral("search.find"));
        QVERIFY(findAction != nullptr);
        findAction->trigger();
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *toggle = dialog->findChild<QPushButton *>(QStringLiteral("regexAssistantToggle"));
        auto *assistant = dialog->findChild<QWidget *>(QStringLiteral("regexAssistant"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *hint = dialog->findChild<QLabel *>(QStringLiteral("regexQueryHint"));
        auto *scope = dialog->findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *close = dialog->findChild<QPushButton *>(QStringLiteral("closeSearchButton"));
        QVERIFY(pattern && toggle && assistant && regex && hint && scope
                && findAll && close);
        const int currentFileScope = scope->findData(0);
        QVERIFY(currentFileScope >= 0);
        scope->setCurrentIndex(currentFileScope);
        QVERIFY(!assistant->isVisible());

        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(toggle->isChecked());
        QVERIFY(assistant->isVisible());
        QVERIFY(regex->isChecked());
        QPushButton *literal = buttonWithText(assistant, QStringLiteral("原样文本"));
        QPushButton *digits = buttonWithText(assistant, QStringLiteral("数字"));
        QVERIFY(literal && digits);

        QTest::mouseClick(pattern, Qt::LeftButton);
        QTest::keyClick(pattern, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(pattern, QStringLiteral("event.id="));
        QTRY_COMPARE_WITH_TIMEOUT(pattern->text(), QStringLiteral("event.id="), 1000);
        pattern->selectAll();
        QCOMPARE(pattern->selectedText(), QStringLiteral("event.id="));
        QTest::mouseClick(literal, Qt::LeftButton);
        QCOMPARE(pattern->text(), QStringLiteral("event\\.id\\="));
        QTest::mouseClick(digits, Qt::LeftButton);
        QCOMPARE(pattern->text(), QStringLiteral("event\\.id\\=\\d+"));
        QTRY_VERIFY_WITH_TIMEOUT(
            hint->text().contains(QStringLiteral("正则有效")), 1000);

        auto *dock = mw_->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        auto *tree = dock ? dock->findChild<QTreeView *>(
            QStringLiteral("searchResultsTree")) : nullptr;
        QVERIFY(dock && tree);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible() && tree->model(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 2, 3000);

        const QString dialogScreenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-search-assistant-user-query.png");
        QVERIFY2(dialog->grab().save(dialogScreenshot), qPrintable(dialogScreenshot));
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), 1000);

        QSignalSpy jumpSpy(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        const QModelIndex first = tree->model()->index(
            0, SearchResultsModel::COL_CONTEXT);
        tree->scrollTo(first);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QRect firstRect = tree->visualRect(first);
        QVERIFY(firstRect.isValid());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                          Qt::NoModifier, firstRect.center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                           Qt::NoModifier, firstRect.center());
        QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->selectedText(),
                                  QStringLiteral("event.id=4821"), 2000);
        const QString resultScreenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-search-assistant-result-jump.png");
        QVERIFY2(mw_->grab().save(resultScreenshot), qPrintable(resultScreenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_gui_gb18030_regex_search_returns_raw_byte_result() {
        closeAllEditorTabs(mw_);
        QVERIFY2(ensureGb18030SearchFixture(), "unable to create GB18030 S2 fixture");
        mw_->openFilesFromExternal({gb18030SearchFixture_.path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 8000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->isL2Active());
        QVERIFY(tab->viewModeL2() == ViewMode::Text);
        QCOMPARE(tab->displayEncoding(), TextEncoding::GB18030);
        QVERIFY2(displayCurrentDocumentAs(QStringLiteral("gb18030")),
                 "GB18030 reopen menu action was not user-clickable");
        QCOMPARE(tab->displayEncoding(), TextEncoding::GB18030);

        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider);
        mw_->show();
        mw_->raise();
        mw_->activateWindow();
        provider->editor()->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(provider->editor()->hasFocus(), 1000);
        QTest::keyClick(provider->editor(), Qt::Key_F, Qt::ControlModifier);
        auto *dialog = mw_->findChild<FindReplaceDialog *>();
        QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 2000);
        auto *pattern = dialog->findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *regex = dialog->findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *hint = dialog->findChild<QLabel *>(QStringLiteral("regexQueryHint"));
        auto *findNext = dialog->findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *findAll = dialog->findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *close = dialog->findChild<QPushButton *>(QStringLiteral("closeSearchButton"));
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *dock = mw_->findChild<SearchResultsDock *>(QStringLiteral("SearchResultsDock"));
        auto *tree = dock ? dock->findChild<QTreeView *>(
            QStringLiteral("searchResultsTree")) : nullptr;
        auto *summary = dock ? dock->findChild<QLabel *>(
            QStringLiteral("searchResultsSummary")) : nullptr;
        QVERIFY(pattern && regex && hint && findNext && findAll && close
                && status && dock && tree && summary);

        QApplication::clipboard()->setText(gb18030SearchFixture_.regexQuery);
        pattern->setFocus();
        QTest::keyClick(pattern, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(pattern, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(pattern->text(),
                                  gb18030SearchFixture_.regexQuery, 1000);
        if (!regex->isChecked())
            QTest::mouseClick(regex, Qt::LeftButton);
        QVERIFY(regex->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(
            hint->text().contains(QStringLiteral("正则有效")), 1000);

        QElapsedTimer searchElapsed;
        searchElapsed.start();
        QTest::mouseClick(findNext, Qt::LeftButton);
        QVERIFY2(searchElapsed.elapsed() < 3000,
                 "GB18030 Find Next did not return within an interactive bound");
        QVERIFY2(tab->lastSearchError().isEmpty(),
                 qPrintable(tab->lastSearchError()));
        QVERIFY(!status->text().contains(QStringLiteral("正则无效")));
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(),
                                  gb18030SearchFixture_.markerOffset, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                                  gb18030SearchFixture_.markerOffset
                                      + gb18030SearchFixture_.markerBytes.size(),
                                  3000);
        QCOMPARE(provider->sourceBytes(provider->selectionStartOffset(),
                                       gb18030SearchFixture_.markerBytes.size()),
                 gb18030SearchFixture_.markerBytes);
        const QString dialogScreenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-search-gb18030-regex-query.png");
        QVERIFY2(dialog->grab().save(dialogScreenshot), qPrintable(dialogScreenshot));

        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible() && tree->model(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(findAll->isEnabled(), 5000);
        const QModelIndex result = tree->model()->index(
            0, SearchResultsModel::COL_CONTEXT);
        QVERIFY(result.isValid());
        QVERIFY(result.data(SearchResultsModel::ContextRole).toString().contains(
            gb18030SearchFixture_.markerText));
        QVERIFY(!summary->text().contains(QStringLiteral("失效")));

        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), 1000);
        QSignalSpy jumpSpy(dock, &SearchResultsDock::jumpToDocumentByteOffset);
        tree->scrollTo(result, QAbstractItemView::PositionAtCenter);
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        const QRect resultRect = tree->visualRect(result);
        QVERIFY(resultRect.isValid());
        QVERIFY(tree->viewport()->rect().contains(resultRect.center()));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                          Qt::NoModifier, resultRect.center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                           Qt::NoModifier, resultRect.center());
        QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(),
                                  gb18030SearchFixture_.markerOffset, 2000);
        QVERIFY(!summary->text().contains(QStringLiteral("失效")));
        const QString resultScreenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-search-gb18030-result-jump.png");
        QVERIFY2(mw_->grab().save(resultScreenshot), qPrintable(resultScreenshot));
        closeAllEditorTabs(mw_);
    }

    void tc_gui_small_file_detects_gb18030_without_manual_switch() {
        closeAllEditorTabs(mw_);
        const char32_t supplementary = 0x20000;
        const QString expected = QStringLiteral("日志 用户")
            + QString::fromUcs4(&supplementary, 1)
            + QStringLiteral(" 编码检测\r\n");
        bool encodedExactly = false;
        const QByteArray bytes = encodeTextFragment(
            expected, TextEncoding::GB18030, &encodedExactly);
        QVERIFY(encodedExactly);
        const QString path = tmpDir_.filePath(QStringLiteral("detect-small-gb18030.log"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        const EncodingDetection directDetection = detectEncoding(bytes);
        QCOMPARE(directDetection.encoding, TextEncoding::GB18030);

        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Active());
        QCOMPARE(tab->encoding(), TextEncoding::GB18030);
        QCOMPARE(tab->editor()->text(), expected);
        closeAllEditorTabs(mw_);
    }

    void tc_gui_gb18030_reopen_updates_the_visible_large_file_text() {
        closeAllEditorTabs(mw_);
        QVERIFY2(ensureGb18030SearchFixture(), "unable to create GB18030 S2 fixture");
        mw_->openFilesFromExternal({gb18030SearchFixture_.path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 8000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->viewModeL2() == ViewMode::Text);
        QCOMPARE(tab->displayEncoding(), TextEncoding::GB18030);
        auto *provider = tab->windowedReadOnlyProvider();
        QVERIFY(provider && provider->editor());
        QVERIFY2(displayCurrentDocumentAs(QStringLiteral("gb18030")),
                 "GB18030 reopen menu action was not user-clickable");
        QCOMPARE(tab->displayEncoding(), TextEncoding::GB18030);
        QApplication::processEvents(QEventLoop::AllEvents, 200);

        const QString screenshot = QCoreApplication::applicationDirPath()
            + QStringLiteral("/gui-search-gb18030-visible-projection.png");
        QVERIFY2(mw_->grab().save(screenshot), qPrintable(screenshot));
        QVERIFY2(provider->editor()->text().contains(gb18030SearchFixture_.markerText),
                 qPrintable(QStringLiteral(
                     "菜单已勾选 GB18030，但用户可见的大文件文本未按 GB18030 重新解码。当前可见前缀：%1")
                     .arg(provider->editor()->text().left(160))));
        closeAllEditorTabs(mw_);
    }

    void tc_gui_large_encoding_conversion_streams_to_new_file_and_opens_it() {
        closeAllEditorTabs(mw_);
        QVERIFY2(ensureGb18030SearchFixture(), "unable to create GB18030 S2 fixture");
        const QByteArray originalPrefix = [&]() {
            QFile file(gb18030SearchFixture_.path);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            return file.read(256);
        }();
        QVERIFY(!originalPrefix.isEmpty());
        mw_->openFilesFromExternal({gb18030SearchFixture_.path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 8000));
        EditorTab *sourceTab = currentEditorTab(mw_);
        QVERIFY(sourceTab && sourceTab->displayEncoding() == TextEncoding::GB18030);

        QAction *convertUtf8 = mw_->findChild<QAction *>(
            QStringLiteral("encoding.convert.utf8"));
        QVERIFY(convertUtf8 && convertUtf8->isEnabled());
        const QString target = tmpDir_.filePath(
            QStringLiteral("gui-search-gb18030-converted-utf8.log"));
        bool saveAsSeen = false;
        QTimer saveAsPoll;
        saveAsPoll.setInterval(20);
        connect(&saveAsPoll, &QTimer::timeout, this, [&]() {
            auto *dialog = mw_->findChild<QFileDialog *>(
                QStringLiteral("encodingConversionSaveAsDialog"));
            if (!dialog || !dialog->isVisible()) return;
            saveAsSeen = true;
            dialog->selectFile(target);
            static_cast<QDialog *>(dialog)->accept();
        });
        saveAsPoll.start();
        convertUtf8->trigger();
        saveAsPoll.stop();
        QVERIFY(saveAsSeen);

        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 8000);
        EditorTab *convertedTab = currentEditorTab(mw_);
        QVERIFY(convertedTab && convertedTab != sourceTab);
        QVERIFY(convertedTab->isL2Active());
        QCOMPARE(convertedTab->encoding(), TextEncoding::Utf8);
        auto *provider = convertedTab->windowedReadOnlyProvider();
        QVERIFY(provider && provider->editor());
        QVERIFY(provider->editor()->text().contains(gb18030SearchFixture_.markerText));
        {
            QFile source(gb18030SearchFixture_.path);
            QVERIFY(source.open(QIODevice::ReadOnly));
            QCOMPARE(source.read(originalPrefix.size()), originalPrefix);
        }
        closeAllEditorTabs(mw_);
    }

    void tc_gui_detection_uses_middle_and_rejects_invalid_real_eof() {
        closeAllEditorTabs(mw_);
        QByteArray distributed(2 * 1024 * 1024, 'A');
        const char32_t supplementary = 0x20000;
        bool exact = false;
        const QByteArray marker = encodeTextFragment(
            QString::fromUcs4(&supplementary, 1), TextEncoding::GB18030, &exact);
        QVERIFY(exact && marker.size() == 4);
        const qsizetype middle = distributed.size() / 2;
        distributed.replace(middle, marker.size(), marker);
        const QString distributedPath = tmpDir_.filePath(
            QStringLiteral("encoding-distributed-gb18030.log"));
        QFile distributedFile(distributedPath);
        QVERIFY(distributedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(distributedFile.write(distributed), qint64(distributed.size()));
        distributedFile.close();
        QCOMPARE(detectFileEncoding(distributedPath).encoding, TextEncoding::GB18030);

        mw_->openFilesFromExternal({distributedPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 5000);
        QCOMPARE(currentEditorTab(mw_)->encoding(), TextEncoding::GB18030);
        closeAllEditorTabs(mw_);

        QByteArray invalidTail("ordinary utf8 text\n");
        invalidTail.append(char(0xff));
        const QString invalidPath = tmpDir_.filePath(
            QStringLiteral("encoding-invalid-real-eof.log"));
        QFile invalidFile(invalidPath);
        QVERIFY(invalidFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(invalidFile.write(invalidTail), qint64(invalidTail.size()));
        invalidFile.close();

        mw_->openFilesFromExternal({invalidPath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *invalidTab = currentEditorTab(mw_);
        QVERIFY(invalidTab);
        QCOMPARE(invalidTab->encoding(), TextEncoding::Utf8);
        QCOMPARE(invalidTab->encodingOrigin(), EncodingOrigin::Fallback);
        QVERIFY(invalidTab->encodingHasDecodeErrors());
        auto *status = mw_->findChild<QToolButton *>(QStringLiteral("status.encoding"));
        QVERIFY(status && status->text().contains(QStringLiteral("包含非法字节")));
        closeAllEditorTabs(mw_);
    }

    void tc_gui_conversion_reopens_ambiguous_ascii_with_selected_encoding() {
        closeAllEditorTabs(mw_);
        const QByteArray sourceBytes("plain ascii stays ambiguous after conversion\r\n");
        const QString sourcePath = tmpDir_.filePath(QStringLiteral("encoding-ascii-source.txt"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
        source.close();
        mw_->openFilesFromExternal({sourcePath});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);

        QAction *convert = mw_->findChild<QAction *>(
            QStringLiteral("encoding.convert.windows1252"));
        QVERIFY(convert && convert->isEnabled());
        const QString target = tmpDir_.filePath(QStringLiteral("encoding-ascii-cp1252.txt"));
        bool dialogSeen = false;
        QTimer dialogPoll;
        dialogPoll.setInterval(20);
        connect(&dialogPoll, &QTimer::timeout, this, [&]() {
            auto *dialog = mw_->findChild<QFileDialog *>(
                QStringLiteral("encodingConversionSaveAsDialog"));
            if (!dialog || !dialog->isVisible()) return;
            dialogSeen = true;
            dialog->selectFile(target);
            static_cast<QDialog *>(dialog)->accept();
        });
        dialogPoll.start();
        convert->trigger();
        dialogPoll.stop();
        QVERIFY(dialogSeen);
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 5000);
        EditorTab *converted = currentEditorTab(mw_);
        QVERIFY(converted);
        QCOMPARE(converted->encoding(), TextEncoding::Windows1252);
        QCOMPARE(converted->encodingOrigin(), EncodingOrigin::UserSelected);
        QFile targetFile(target);
        QVERIFY(targetFile.open(QIODevice::ReadOnly));
        QCOMPARE(targetFile.readAll(), sourceBytes);
        closeAllEditorTabs(mw_);
    }

    void tc_gui_legacy_save_refuses_unrepresentable_user_edit() {
        closeAllEditorTabs(mw_);
        const QString originalText = QStringLiteral("用户文件编码测试\r\n");
        bool exact = false;
        const QByteArray originalBytes = encodeTextFragment(
            originalText, TextEncoding::GBK, &exact);
        QVERIFY(exact);
        const QString path = tmpDir_.filePath(QStringLiteral("encoding-gbk-save.log"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(originalBytes), qint64(originalBytes.size()));
        file.close();
        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->encoding() == TextEncoding::GBK);
        tab->editor()->setCursorPosition(0, originalText.size() - 2);
        QInputMethodEvent emojiCommit;
        emojiCommit.setCommitString(QString::fromUtf8("\xF0\x9F\x98\x80"));
        QApplication::sendEvent(tab->editor(), &emojiCommit);
        QVERIFY(tab->isModified());

        bool warningSeen = false;
        QString warningText;
        QTimer warningPoll;
        warningPoll.setInterval(20);
        connect(&warningPoll, &QTimer::timeout, this, [&]() {
            for (QMessageBox *box : mw_->findChildren<QMessageBox *>()) {
                if (!box->isVisible()) continue;
                warningSeen = true;
                warningText = box->text() + QLatin1Char(' ') + box->informativeText();
                box->accept();
            }
        });
        warningPoll.start();
        QVERIFY(QMetaObject::invokeMethod(mw_, "saveFile", Qt::DirectConnection));
        warningPoll.stop();
        QVERIFY(warningSeen);
        QVERIFY(warningText.contains(QStringLiteral("无法保存文件")));
        QFile unchanged(path);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), originalBytes);
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_invalid_only_local_drop_is_an_open_intent_data() {
        QTest::addColumn<bool>("editorSurface");
        QTest::addColumn<bool>("directory");
        QTest::newRow("mainwindow-missing") << false << false;
        QTest::newRow("mainwindow-directory") << false << true;
        QTest::newRow("editor-missing") << true << false;
        QTest::newRow("editor-directory") << true << true;
    }

    void tc_2b2_invalid_only_local_drop_is_an_open_intent() {
        QFETCH(bool, editorSurface);
        QFETCH(bool, directory);
        closeAllEditorTabs(mw_);
        const QString source = tmpDir_.filePath(QStringLiteral("invalid-drop-source.txt"));
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        mw_->openFilesFromExternal({source});
        auto *tab = currentEditorTab(mw_);
        QVERIFY(tab);
        const auto id = tab->documentState().id;
        const auto hash = dropFixtureHash(source);
        const QString path = tmpDir_.filePath(directory ? QStringLiteral("invalid-only-directory")
                                                      : QStringLiteral("invalid-only-missing.txt"));
        if (directory) QVERIFY(QDir().mkpath(path));
        QWidget *surface = editorSurface ? tab->editor()->viewport()
                                         : static_cast<QWidget *>(mw_);
        QString feedback;
        QTimer guard;
        guard.setInterval(10);
        connect(&guard, &QTimer::timeout, this, [&]() {
            for (QMessageBox *box : mw_->findChildren<QMessageBox *>()) {
                if (!box->isVisible()) continue;
                feedback += box->text() + box->informativeText();
                box->accept();
            }
        });
        guard.start();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &enter);
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &drop);
        guard.stop();
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_)->documentState().id, id);
        QCOMPARE(dropFixtureHash(source), hash);
        QVERIFY2(enter.isAccepted() && drop.isAccepted(),
                 "invalid-only local URL was rejected before its open intent reached feedback");
        QVERIFY(feedback.contains(QFileInfo(path).fileName()));
        QVERIFY(feedback.contains(directory ? QStringLiteral("文件夹") : QStringLiteral("不存在")));
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_mixed_local_drop_preserves_documents_and_feedback_data() {
        QTest::addColumn<bool>("editorSurface");
        QTest::newRow("mainwindow") << false;
        QTest::newRow("editor-viewport") << true;
    }

    void tc_2b2_mixed_local_drop_preserves_documents_and_feedback() {
        QFETCH(bool, editorSurface);
        closeAllEditorTabs(mw_);
        const QString base = tmpDir_.filePath(QStringLiteral("drop-dirty.txt"));
        const QString plain = tmpDir_.filePath(QStringLiteral("drop-valid.txt"));
        const QString binary = tmpDir_.filePath(QStringLiteral("drop-readonly.bin"));
        const QString missing = tmpDir_.filePath(QStringLiteral("drop-missing.txt"));
        const QString directory = tmpDir_.filePath(QStringLiteral("drop-directory"));
        const QString secure = tmpDir_.filePath(QStringLiteral("drop-protected.nsafe"));
        for (const QString &path : {base, plain, secure}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        QVERIFY(QDir().mkpath(directory));
        {
            QFile file(binary);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QVERIFY(file.resize(33LL * 1024 * 1024));
        }
        const QByteArray baseHash = dropFixtureHash(base);
        const QByteArray plainHash = dropFixtureHash(plain);
        const QByteArray binaryHash = dropFixtureHash(binary);
        const QByteArray secureHash = dropFixtureHash(secure);
        QVERIFY(!baseHash.isEmpty() && !binaryHash.isEmpty());
        mw_->openFilesFromExternal({base});
        auto *dirty = currentEditorTab(mw_);
        QVERIFY(dirty);
        dirty->editor()->append(QStringLiteral("unsaved user text"));
        QVERIFY(dirty->isModified());
        const QString dirtyText = dirty->editor()->text();
        const DocumentState dirtyState = dirty->documentState();
        auto *stack = mw_->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        QVERIFY(stack);
        QWidget *const workspace = stack->currentWidget();
        QWidget *surface = editorSurface ? dirty->editor()->viewport()
                                         : static_cast<QWidget *>(mw_);
        QStringList visibleErrors;
        int credentialDialogs = 0;
        QTimer modalGuard;
        modalGuard.setInterval(10);
        connect(&modalGuard, &QTimer::timeout, this, [&]() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || !dialog->isModal()) continue;
                if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
                    visibleErrors << box->text() + QLatin1Char(' ') + box->informativeText();
                    box->accept();
                } else {
                    ++credentialDialogs;
                    dialog->reject();
                }
            }
        });
        modalGuard.start();
        QMimeData mime;
        // Secure/missing/directory precede later successes. A final duplicate
        // must preserve identity without erasing the batch's failure feedback.
        mime.setUrls({QUrl::fromLocalFile(secure), QUrl::fromLocalFile(missing),
                      QUrl::fromLocalFile(directory), QUrl::fromLocalFile(plain),
                      QUrl::fromLocalFile(binary), QUrl::fromLocalFile(base)});
        QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &enter);
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &drop);
        modalGuard.stop();
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 3, 5000);
        EditorTab *binaryTab = nullptr;
        for (int i = 0; i < tabCount(mw_); ++i) {
            if (tabAt(mw_, i)->filePath() == binary) binaryTab = tabAt(mw_, i);
        }
        QVERIFY(binaryTab);
        QTRY_COMPARE_WITH_TIMEOUT(binaryTab->contentSizeBytes(), QFileInfo(binary).size(), 5000);
        QVERIFY(enter.isAccepted() && drop.isAccepted());
        QCOMPARE(currentEditorTab(mw_), dirty);
        QCOMPARE(dirty->documentState().id, dirtyState.id);
        QCOMPARE(dirty->documentState().capabilities, dirtyState.capabilities);
        QCOMPARE(dirty->editor()->text(), dirtyText);
        QVERIFY(dirty->isModified());
        QVERIFY(binaryTab->isBinaryDocument() && binaryTab->isReadOnlyByPolicy());
        QVERIFY(!binaryTab->documentState().capabilities.testFlag(DocumentCapability::Edit));
        QVERIFY(!binaryTab->documentState().capabilities.testFlag(DocumentCapability::Save));
        QCOMPARE(stack->currentWidget(), workspace);
        QCOMPARE(dropFixtureHash(base), baseHash);
        QCOMPARE(dropFixtureHash(plain), plainHash);
        QCOMPARE(dropFixtureHash(binary), binaryHash);
        QCOMPARE(dropFixtureHash(secure), secureHash);
        QCOMPARE(credentialDialogs, 0);
        const QString feedback = visibleErrors.join(QLatin1Char('\n'))
            + QLatin1Char('\n') + mw_->statusBar()->currentMessage();
        qInfo().noquote() << "2B2 preserved dirty/id/hash/readonly/workspace; batch feedback:" << feedback;
        QVERIFY2(feedback.contains(QFileInfo(missing).fileName()),
                 "missing local URL disappeared without a visible failure");
        QVERIFY2(feedback.contains(QStringLiteral("不存在")), "missing reason must be factual");
        QVERIFY2(feedback.contains(QFileInfo(directory).fileName())
                     && feedback.contains(QStringLiteral("文件夹")),
                 "directory intent disappeared without its applicable reason");
        QVERIFY2(feedback.contains(QFileInfo(secure).fileName())
                     && feedback.contains(QStringLiteral("文件→打开")),
                 "secure refusal and next step were erased by a later successful open");
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_remote_only_drop_does_not_insert_urls_data() {
        QTest::addColumn<bool>("editorSurface");
        QTest::newRow("mainwindow") << false;
        QTest::newRow("editor-viewport") << true;
    }

    void tc_2b2_remote_only_drop_does_not_insert_urls() {
        QFETCH(bool, editorSurface);
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("drop-remote-guard.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        mw_->openFilesFromExternal({path});
        auto *tab = currentEditorTab(mw_);
        QVERIFY(tab);
        tab->editor()->append(QStringLiteral("unsaved text"));
        const QString body = tab->editor()->text();
        const auto state = tab->documentState();
        const QByteArray hash = dropFixtureHash(path);
        QWidget *surface = editorSurface ? tab->editor()->viewport()
                                         : static_cast<QWidget *>(mw_);
        QMimeData mime;
        const QString url = QStringLiteral("https://drop.invalid/must-not-load-or-insert");
        mime.setUrls({QUrl(url)});
        mime.setText(url); // Native editor text fallback must not insert this.
        QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &enter);
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &drop);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(tab->documentState().id, state.id);
        QCOMPARE(tab->documentState().capabilities, state.capabilities);
        QCOMPARE(tab->editor()->text(), body);
        QVERIFY(tab->isModified());
        QCOMPARE(dropFixtureHash(path), hash);
        QVERIFY2(!enter.isAccepted() && !drop.isAccepted(), "remote-only URL drop was accepted");
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_external_batch_keeps_secure_refusal_after_success() {
        closeAllEditorTabs(mw_);
        const QString plain = tmpDir_.filePath(QStringLiteral("drop-feedback-valid.txt"));
        const QString secure = tmpDir_.filePath(QStringLiteral("drop-feedback-protected.nsafe"));
        for (const QString &path : {plain, secure}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        mw_->openFilesFromExternal({plain});
        const auto id = currentEditorTab(mw_)->documentState().id;
        const auto hash = dropFixtureHash(secure);
        mw_->openFilesFromExternal({secure, plain});
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_)->documentState().id, id);
        QCOMPARE(dropFixtureHash(secure), hash);
        const QString feedback = mw_->statusBar()->currentMessage();
        qInfo().noquote() << "2B2 final batch feedback:" << feedback;
        QVERIFY2(feedback.contains(QFileInfo(secure).fileName())
                     && feedback.contains(QStringLiteral("文件→打开")),
                 "later success erased the protected-file refusal and next step");
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_external_authorization_survives_second_probe_type_change() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const QString fixture = QFINDTESTDATA("../fixtures/kdbx/Format400.kdbx");
        QFile fixtureFile(fixture);
        QVERIFY(fixtureFile.open(QIODevice::ReadOnly));
        const QByteArray secureBytes = fixtureFile.readAll();
        QVERIFY(!secureBytes.isEmpty());
        const QByteArray expectedHash = QCryptographicHash::hash(secureBytes, QCryptographicHash::Sha256);
        const QString changedPath = data.filePath(QStringLiteral("reclassified.dat"));
        {
            QFile file(changedPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("ordinary initial bytes\n"), qint64(23));
        }
        mw_->hide();
        qputenv("NOTEPADSAFE_DATADIR", data.path().toUtf8());
        AppComposition composition;
        auto window = std::make_unique<MainWindow>(nullptr, &composition);
        const auto cleanup = qScopeGuard([&]() {
            closeAllEditorTabs(window.get());
            window.reset();
            qputenv("NOTEPADSAFE_DATADIR", primaryTestDataDir_.toUtf8());
            mw_->show();
        });
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        auto *stack = window->findChild<QStackedWidget *>(QStringLiteral("WorkspaceStack"));
        auto *tabs = window->findChild<QTabWidget *>();
        QVERIFY(stack && tabs && stack->currentWidget() == tabs);
        int probes = 0;
        int writes = 0;
        bool replacementSucceeded = false;
        QVERIFY(composition.persistenceAdapters().registerRoute(
            {QStringLiteral("test.2b2.reclassification"), QStringLiteral("test"),
             10000, DocumentPersistence::PlainFile},
            [&](const QString &path) {
                if (path != changedPath) return false;
                if (++probes == 2) {
                    QFile file(path);
                    replacementSucceeded = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && file.write(secureBytes) == secureBytes.size();
                    ++writes;
                }
                return false; // Both classifications are performed by real routes.
            }));
        int credentialDialogs = 0;
        QTimer guard;
        guard.setInterval(10);
        connect(&guard, &QTimer::timeout, this, [&]() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || !dialog->isModal()) continue;
                if (!qobject_cast<QMessageBox *>(dialog)) ++credentialDialogs;
                dialog->reject();
            }
        });
        const int factoriesBefore = composition.heavyFactoryCalls();
        guard.start();
        window->openFilesFromExternal({changedPath});
        guard.stop();
        QVERIFY(replacementSucceeded);
        QCOMPARE(writes, 1);
        QCOMPARE(dropFixtureHash(changedPath), expectedHash);
        QCOMPARE(tabs->count(), 0);
        qInfo() << "2B2 second-probe mutation count=" << writes
                << "security factories=" << composition.heavyFactoryCalls() - factoriesBefore
                << "credential dialogs=" << credentialDialogs;
        QCOMPARE(credentialDialogs, 0);
        QCOMPARE(composition.heavyFactoryCalls(), factoriesBefore);
        QCOMPARE(stack->currentWidget(), tabs);
        QVERIFY(!QFileInfo::exists(data.filePath(QStringLiteral("security-session-v1.bin"))));
        QVERIFY(window->statusBar()->currentMessage().contains(QFileInfo(changedPath).fileName()));
        QVERIFY(window->statusBar()->currentMessage().contains(QStringLiteral("文件→打开")));
        auto *review = window->findChild<QToolButton *>(QStringLiteral("externalOpenFailuresButton"));
        QVERIFY(review && review->isVisible());
    }

    void tc_2b2_failure_details_keyboard_and_batch_lifetime() {
        closeAllEditorTabs(mw_);
        const QString plain = tmpDir_.filePath(QStringLiteral("batch-success.txt"));
        const QString directory = tmpDir_.filePath(QStringLiteral("多字节&路径-只读复查-多字节&路径"));
        QVERIFY(QDir().mkpath(directory));
        const QString first = QDir(directory).filePath(QStringLiteral("第一份&不解锁.nsafe"));
        const QString second = QDir(directory).filePath(QStringLiteral("第二份&不解锁.nsafe"));
        const QString replacement = QDir(directory).filePath(QStringLiteral("新批次&不解锁.nsafe"));
        for (const QString &path : {plain, first, second, replacement}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        const auto firstHash = dropFixtureHash(first);
        const auto secondHash = dropFixtureHash(second);
        const auto replacementHash = dropFixtureHash(replacement);
        mw_->openFilesFromExternal({plain});
        const auto id = currentEditorTab(mw_)->documentState().id;
        int dialogs = 0;
        int credentials = 0;
        bool plainText = false;
        QString details;
        QTimer guard;
        guard.setInterval(10);
        connect(&guard, &QTimer::timeout, this, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
                ++dialogs;
                plainText = box->textFormat() == Qt::PlainText;
                details = box->text() + QLatin1Char('\n') + box->informativeText();
                box->accept();
            } else {
                ++credentials;
                dialog->reject();
            }
        });
        guard.start();
        mw_->openFilesFromExternal({first, second, plain});
        QCOMPARE(dialogs, 0); // Secure-only rejection remains non-interrupting.
        auto *button = mw_->findChild<QToolButton *>(QStringLiteral("externalOpenFailuresButton"));
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->text(), QStringLiteral("未打开文件（2）"));
        QVERIFY(button->focusPolicy() == Qt::StrongFocus);
        QVERIFY(button->accessibleDescription().contains(QStringLiteral("不会重新打开")));
        mw_->activateWindow();
        button->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(button->hasFocus(), 2000);
        QTest::keyClick(button, Qt::Key_Space);
        QCOMPARE(dialogs, 1);
        QVERIFY(plainText);
        QVERIFY(details.contains(first) && details.contains(second));
        QVERIFY(details.contains(QStringLiteral("文件→打开")));
        QCOMPARE(currentEditorTab(mw_)->documentState().id, id);
        mw_->openFilesFromExternal({});
        QCOMPARE(button->text(), QStringLiteral("未打开文件（2）"));
        QVERIFY(button->isVisible());
        mw_->openFilesFromExternal({replacement, plain});
        QCOMPARE(dialogs, 1);
        QCOMPARE(button->text(), QStringLiteral("未打开文件（1）"));
        button->setFocus();
        QTest::keyClick(button, Qt::Key_Space);
        QCOMPARE(dialogs, 2);
        QVERIFY(details.contains(replacement));
        QVERIFY(!details.contains(first) && !details.contains(second));
        mw_->openFilesFromExternal({plain});
        QVERIFY(!button->isVisible());
        button->click(); // Even a stale activation cannot replay the old batch.
        QCOMPARE(dialogs, 2);
        QCOMPARE(credentials, 0);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_)->documentState().id, id);
        QCOMPARE(dropFixtureHash(first), firstHash);
        QCOMPARE(dropFixtureHash(second), secondHash);
        QCOMPARE(dropFixtureHash(replacement), replacementHash);
        guard.stop();
        closeAllEditorTabs(mw_);
    }

    void tc_2b2_large_failure_details_expand_with_keyboard() {
        closeAllEditorTabs(mw_);
        const QString directory = tmpDir_.filePath(QStringLiteral("较长的多字节&路径用于复查失败明细-较长的多字节&路径用于复查失败明细"));
        QVERIFY(QDir().mkpath(directory));
        QStringList paths;
        QList<QByteArray> hashes;
        for (int index = 0; index < 6; ++index) {
            const QString path = QDir(directory).filePath(
                QStringLiteral("%1-用户文件&不会自动解锁-用户文件&不会自动解锁.nsafe").arg(index));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
            file.close();
            paths << path;
            hashes << dropFixtureHash(path);
        }
        mw_->openFilesFromExternal(paths);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        auto *review = mw_->findChild<QToolButton *>(QStringLiteral("externalOpenFailuresButton"));
        QVERIFY(review && review->isVisible());
        QCOMPARE(review->text(), QStringLiteral("未打开文件（6）"));
        int stage = 0;
        bool plainText = false;
        bool boundedPreview = false;
        bool initiallyCollapsed = false;
        bool expandedVisible = false;
        bool insideScreen = false;
        bool screenshotSaved = false;
        QString visibleDetails;
        const QString screenshot = QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("gui-2b2-large-failure-details.png"));
        QTimer guard;
        guard.setInterval(20);
        connect(&guard, &QTimer::timeout, this, [&]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            if (stage == 0) {
                plainText = box->textFormat() == Qt::PlainText;
                boundedPreview = box->informativeText().size() < 1200
                    && box->informativeText().contains(QStringLiteral("6"))
                    && box->informativeText().contains(QStringLiteral("展开"));
                initiallyCollapsed = true;
                for (QTextEdit *editor : box->findChildren<QTextEdit *>())
                    initiallyCollapsed = initiallyCollapsed && !editor->isVisible();
                QAbstractButton *expand = nullptr;
                for (QAbstractButton *button : box->buttons()) {
                    if (box->buttonRole(button) == QMessageBox::ActionRole) expand = button;
                }
                stage = 1;
                if (!expand) { box->reject(); return; }
                expand->setFocus();
                QTest::keyClick(expand, Qt::Key_Space);
                return;
            }
            for (QTextEdit *editor : box->findChildren<QTextEdit *>()) {
                if (!editor->isVisible()) continue;
                expandedVisible = editor->isReadOnly();
                visibleDetails = editor->toPlainText();
            }
            insideScreen = box->screen()->availableGeometry().contains(box->frameGeometry());
            screenshotSaved = box->grab().save(screenshot);
            stage = 2;
            box->accept();
        });
        guard.start();
        review->setFocus();
        QTest::keyClick(review, Qt::Key_Space);
        guard.stop();
        QCOMPARE(stage, 2);
        QVERIFY(plainText && boundedPreview && initiallyCollapsed);
        QVERIFY2(expandedVisible, "keyboard expansion must reveal the real read-only text widget");
        for (int index = 0; index < paths.size(); ++index) {
            QVERIFY(visibleDetails.contains(paths[index]));
            QCOMPARE(dropFixtureHash(paths[index]), hashes[index]);
        }
        QVERIFY(visibleDetails.contains(QStringLiteral("文件→打开")));
        QVERIFY(insideScreen);
        QVERIFY(screenshotSaved);
        QCOMPARE(tabCount(mw_), 0);
        qInfo().noquote() << "2B2 keyboard-expanded read-only details screenshot:" << screenshot;
        mw_->openFilesFromExternal({});
    }

    void tc_2b2_open_dirty_document_reuses_identity_while_disk_is_exclusive() {
#ifndef Q_OS_WIN
        QSKIP("Windows exclusive-handle file sharing fixture");
#else
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("drop-existing-exclusive.txt"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        mw_->openFilesFromExternal({path});
        auto *tab = currentEditorTab(mw_);
        QVERIFY(tab);
        tab->editor()->append(QStringLiteral("unsaved edits stay authoritative"));
        const auto state = tab->documentState();
        const QString body = tab->editor()->text();
        const auto hash = dropFixtureHash(path);
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        std::unique_ptr<void, decltype(&CloseHandle)> exclusive(handle, CloseHandle);
        int dialogs = 0;
        QTimer guard;
        guard.setInterval(10);
        connect(&guard, &QTimer::timeout, this, [&]() {
            for (QMessageBox *box : mw_->findChildren<QMessageBox *>()) {
                if (!box->isVisible()) continue;
                ++dialogs;
                box->accept();
            }
        });
        guard.start();
        mw_->openFilesFromExternal({path});
        guard.stop();
        exclusive.reset();
        QCOMPARE(dropFixtureHash(path), hash);
        QCOMPARE(dialogs, 0);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_), tab);
        QCOMPARE(tab->documentState().id, state.id);
        QCOMPARE(tab->documentState().capabilities, state.capabilities);
        QCOMPARE(tab->editor()->text(), body);
        QVERIFY(tab->isModified());
        closeAllEditorTabs(mw_);
#endif
    }

    void tc_2b2_exclusive_unreadable_batch_keeps_valid_file() {
#ifndef Q_OS_WIN
        QSKIP("Windows exclusive-handle file sharing fixture");
#else
        closeAllEditorTabs(mw_);
        const QString locked = tmpDir_.filePath(QStringLiteral("drop-unreadable.txt"));
        const QString valid = tmpDir_.filePath(QStringLiteral("drop-after-unreadable.txt"));
        for (const QString &path : {locked, valid}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("original source\n"), qint64(16));
        }
        const auto lockedHash = dropFixtureHash(locked);
        const auto validHash = dropFixtureHash(valid);
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(locked.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        std::unique_ptr<void, decltype(&CloseHandle)> exclusive(handle, CloseHandle);
        int dialogs = 0;
        QString feedback;
        QTimer guard;
        guard.setInterval(10);
        connect(&guard, &QTimer::timeout, this, [&]() {
            for (QMessageBox *box : mw_->findChildren<QMessageBox *>()) {
                if (!box->isVisible()) continue;
                ++dialogs;
                feedback = box->text() + QLatin1Char('\n') + box->informativeText();
                box->accept();
            }
        });
        guard.start();
        mw_->openFilesFromExternal({locked, valid});
        guard.stop();
        QCOMPARE(dialogs, 1);
        QCOMPARE(tabCount(mw_), 1);
        QCOMPARE(currentEditorTab(mw_)->filePath(), valid);
        QVERIFY(!currentEditorTab(mw_)->isModified());
        exclusive.reset();
        QCOMPARE(dropFixtureHash(locked), lockedHash);
        QCOMPARE(dropFixtureHash(valid), validHash);
        QVERIFY(feedback.contains(QFileInfo(locked).fileName()));
        QVERIFY(feedback.contains(QStringLiteral("无法读取")));
        QVERIFY(feedback.contains(QStringLiteral("重试")));
        QVERIFY(!feedback.contains(QStringLiteral("不存在")));
        mw_->openFilesFromExternal({locked});
        QCOMPARE(tabCount(mw_), 2);
        QCOMPARE(currentEditorTab(mw_)->filePath(), locked);
        QCOMPARE(dropFixtureHash(locked), lockedHash);
        closeAllEditorTabs(mw_);
#endif
    }

    void tc_gui_hex_surfaces_route_external_file_drop_to_main_window() {
        const QString targetPath = tmpDir_.filePath(
            QStringLiteral("hex-drop-target.txt"));
        {
            QFile target(targetPath);
            QVERIFY(target.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(target.write("opened-through-hex-drop\n"), qint64(24));
        }

        const QString smallBinary = tmpDir_.filePath(
            QStringLiteral("hex-drop-b1.bin"));
        {
            QFile source(smallBinary);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(QByteArray::fromHex("00414243")), qint64(4));
        }
        const QString largeBinary = tmpDir_.filePath(
            QStringLiteral("hex-drop-b2.bin"));
        {
            QFile source(largeBinary);
            QVERIFY(source.open(QIODevice::ReadWrite | QIODevice::Truncate));
            QVERIFY(source.resize(33LL * 1024 * 1024));
        }

        for (const QString &sourcePath : {smallBinary, largeBinary}) {
            closeAllEditorTabs(mw_);
            mw_->openFilesFromExternal({sourcePath});
            QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 5000);
            EditorTab *sourceTab = currentEditorTab(mw_);
            QVERIFY(sourceTab && sourceTab->isHexPresentation());
            QTRY_COMPARE_WITH_TIMEOUT(sourceTab->contentSizeBytes(),
                                      QFileInfo(sourcePath).size(), 5000);

            QPlainTextEdit *asciiSurface = nullptr;
            if (sourceTab->windowedReadOnlyProvider())
                asciiSurface = sourceTab->windowedReadOnlyProvider()->asciiPreview();
            if (!asciiSurface) {
                asciiSurface = sourceTab->findChild<QPlainTextEdit *>(
                    QStringLiteral("HexAsciiPreview"));
            }
            QTRY_VERIFY_WITH_TIMEOUT(asciiSurface && asciiSurface->isVisible(), 5000);
            QWidget *dropSurface = asciiSurface->viewport();
            QVERIFY(dropSurface);
            QVERIFY2(!asciiSurface->acceptDrops() && !dropSurface->acceptDrops(),
                     "lazy Hex child still accepts and swallows OS file drops");
            const QString tier = QFileInfo(sourcePath).size()
                    < 32LL * 1024 * 1024
                ? QStringLiteral("b1") : QStringLiteral("b2");
            QVERIFY(mw_->grab().save(
                QDir(QCoreApplication::applicationDirPath()).filePath(
                    QStringLiteral("gui-hex-drop-%1-surface.png").arg(tier))));

            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(targetPath)});
            QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropSurface, &enter);
            QVERIFY(enter.isAccepted());
            QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropSurface, &drop);
            QVERIFY(drop.isAccepted());

            QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 2, 5000);
            EditorTab *opened = currentEditorTab(mw_);
            QVERIFY(opened);
            QCOMPARE(QFileInfo(opened->filePath()).canonicalFilePath(),
                     QFileInfo(targetPath).canonicalFilePath());
        }
        closeAllEditorTabs(mw_);
    }

    void tc_gui_s1_bottom_thumb_gesture_reaches_physical_eof() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(
            QStringLiteral("s1-scroll-physical-eof.log"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray line(520, 'x');
        line.append('\n');
        for (int i = 0; i < 12000; ++i)
            QCOMPARE(file.write(line), qint64(line.size()));
        QCOMPARE(file.write("PHYSICAL_EOF_MARKER"), qint64(19));
        file.close();

        mw_->openFilesFromExternal({path});
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 5000);
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && !tab->isL2Mode());
        tab->setWordWrap(true);
        QsciScintilla *editor = tab->editor();
        QScrollBar *bar = editor->verticalScrollBar();
        QTRY_VERIFY_WITH_TIMEOUT(bar->maximum() > 0 && bar->isVisible(), 5000);

        // Reproduce the important race deterministically: while the thumb is
        // held at the current maximum, the wrapped display range grows.  A
        // plain QScrollBar release stays at the obsolete maximum.
        bar->setSliderDown(true);
        bar->setSliderPosition(bar->maximum());
        const int oldMaximum = bar->maximum();
        QByteArray appended;
        for (int i = 0; i < 1200; ++i)
            appended.append("late wrapped layout row xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
        editor->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                              static_cast<uintptr_t>(appended.size()),
                              appended.constData());
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QVERIFY(bar->maximum() > oldMaximum);
        bar->setSliderDown(false);
        QTRY_COMPARE_WITH_TIMEOUT(bar->value(), bar->maximum(), 2000);

        const int lastLine = editor->lines() - 1;
        const long lastDisplayStart = editor->SendScintilla(
            QsciScintillaBase::SCI_VISIBLEFROMDOCLINE,
            static_cast<unsigned long>(lastLine));
        const long lastWrapCount = qMax<long>(1, editor->SendScintilla(
            QsciScintillaBase::SCI_WRAPCOUNT,
            static_cast<unsigned long>(lastLine)));
        const long firstVisible = editor->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        const long linesOnScreen = qMax<long>(1, editor->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN));
        QVERIFY2(firstVisible + linesOnScreen
                     >= lastDisplayStart + lastWrapCount,
                 qPrintable(QStringLiteral(
                     "EOF not visible: first=%1 screen=%2 last=%3 wraps=%4 max=%5 value=%6")
                     .arg(firstVisible).arg(linesOnScreen)
                     .arg(lastDisplayStart).arg(lastWrapCount)
                     .arg(bar->maximum()).arg(bar->value())));
        QVERIFY(mw_->grab().save(
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("gui-s1-scroll-physical-eof.png"))));
        tab->setModified(false);
        closeAllEditorTabs(mw_);
    }

    void tc_gui_large_conversion_rejects_unsampled_invalid_source_byte() {
        closeAllEditorTabs(mw_);
        const QString path = tmpDir_.filePath(QStringLiteral("encoding-large-invalid.log"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray block(1024 * 1024, 'A');
        for (int i = 0; i < 33; ++i) {
            if (i == 10) block[12345] = char(0xff);
            QCOMPARE(file.write(block), qint64(block.size()));
            if (i == 10) block[12345] = 'A';
        }
        file.close();
        mw_->openFilesFromExternal({path});
        QVERIFY(waitForTabCountAndL2(mw_, 1, true, 8000));
        EditorTab *tab = currentEditorTab(mw_);
        QVERIFY(tab && tab->encoding() == TextEncoding::Utf8);
        QVERIFY(!tab->encodingHasDecodeErrors());

        QAction *convert = mw_->findChild<QAction *>(
            QStringLiteral("encoding.convert.utf16le"));
        QVERIFY(convert && convert->isEnabled());
        const QString target = tmpDir_.filePath(QStringLiteral("encoding-large-invalid-out.txt"));
        bool dialogSeen = false;
        bool warningSeen = false;
        QString warningText;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, this, [&]() {
            auto *dialog = mw_->findChild<QFileDialog *>(
                QStringLiteral("encodingConversionSaveAsDialog"));
            if (dialog && dialog->isVisible()) {
                dialogSeen = true;
                dialog->selectFile(target);
                static_cast<QDialog *>(dialog)->accept();
            }
            for (QMessageBox *box : mw_->findChildren<QMessageBox *>()) {
                if (!box->isVisible()) continue;
                warningSeen = true;
                warningText = box->text() + QLatin1Char(' ') + box->informativeText();
                box->accept();
            }
        });
        poll.start();
        convert->trigger();
        poll.stop();
        QVERIFY(dialogSeen);
        QVERIFY(warningSeen);
        QVERIFY(warningText.contains(QStringLiteral("不符合所选的 UTF-8 编码")));
        QVERIFY(!QFileInfo::exists(target));
        closeAllEditorTabs(mw_);
    }
};

// 在 QTEST_MAIN 展开前（= QApplication 创建前）给每个 test slot 设 30s 全局硬超时，
// 防止单个 case 因同步 readAll/死锁卡桌面 5 分钟（Qt 默认 300s fatality）。
//   QTEST_FUNCTION_TIMEOUT 环境变量 = 毫秒；
//   大文件 fixture 已改为各自用例懒创建，聚焦运行 Hex 用例不会产生额外大文件 IO。
static const int kGlobalSlotTimeoutMs = 30000;
[[maybe_unused]] static const bool kSetFuncTimeoutOnce = []{
    qputenv("QTEST_FUNCTION_TIMEOUT", QByteArray::number(kGlobalSlotTimeoutMs));
    return true;
}();

QTEST_MAIN(Group1SmallfixesMenuUI)
#include "smoke_smallfixes_menu_ui.moc"
