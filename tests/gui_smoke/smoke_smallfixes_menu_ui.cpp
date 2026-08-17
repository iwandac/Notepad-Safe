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
#include <QtGui/QTextCursor>
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
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QListView>
#include <QtWidgets/QPlainTextEdit>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "MainWindow.h"
#include "EditorTab.h"
#include "VirtualFileView.h"
#include "FindReplaceDialog.h"
#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "SessionManager.h"
#include "DraftStore.h"

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

// 编码菜单「转为 XXX」和「以编码显示 XXX」动作定位：
//   先识别 MainWindow 顶层"编码(E)"菜单和"以编码显示"菜单（title 含关键字），
//   再从这两个 menu 的 actions() 里按 data=int+checkable 收集 5+5 项。
//   parent 链直接从 QMenu 取 actions()（避免 findChildren 混入 dialog 内部编码 action）。
struct EncodingActions {
    QVector<QAction*> convert;  // 「转为...」5 项（L2 应全灰）
    QVector<QAction*> display;  // 「以编码显示...」5 项（仅文本呈现可点）
};
static EncodingActions collectEncodingActions(const MainWindow* top) {
    EncodingActions out;
    if (!top) return out;
    QMenu* encMenu = nullptr;
    QMenu* displayMenu = nullptr;
    // QMenuBar::addMenu 返回的 QMenu：parent 实际上是 MainWindow 本身（不是 menuBar()），
    // 所以我们必须在 MainWindow 全 QMenu 里找 title 匹配（而非 menuBar()->findChildren<QMenu*>）。
    const auto menus = top->findChildren<QMenu*>();
    for (QMenu* m : menus) {
        const QString t = m->title();
        if (t.contains(QStringLiteral("编码"))
            && !t.contains(QStringLiteral("显示"))
            && !t.contains(QStringLiteral("重新打开"))) encMenu = m;
        if (t.contains(QStringLiteral("以编码显示"))
            || t.contains(QStringLiteral("显示编码"))
            || t.contains(QStringLiteral("以编码重新打开"))) displayMenu = m;
    }
    auto collectFromMenu = [](QMenu* m, bool wantConvert) -> QVector<QAction*> {
        QVector<QAction*> r;
        if (!m) return r;
        for (QAction* a : m->actions()) {
            if (!a || a->isSeparator()) continue;
            const bool hasEncodingData = (a->data().typeId() == QMetaType::Int) && a->isCheckable();
            if (!hasEncodingData) continue;
            if (wantConvert && a->text().contains(QStringLiteral("转为"))) r.push_back(a);
            else if (!wantConvert
                     && (a->text().contains(QStringLiteral("显示"))
                         || a->text().contains(QStringLiteral("重新打开")))) r.push_back(a);
        }
        return r;
    };
    if (encMenu) out.convert = collectFromMenu(encMenu, true);
    if (displayMenu) out.display = collectFromMenu(displayMenu, false);
    // Fallback（找不到 menu title 时）：退而求其次，直接用 MainWindow 全 QAction 按"转为/显示"
    // + isInMenuOrToolbar 再 + data=int checkable 做筛选（保证测试不挂在结构差异上）
    if (out.convert.isEmpty() || out.display.isEmpty()) {
        const auto all = top->findChildren<QAction*>();
        for (QAction* a : all) {
            if (!isInMenuOrToolbar(a, top)) continue;
            const bool hasEncodingData = (a->data().typeId() == QMetaType::Int) && a->isCheckable();
            if (!hasEncodingData) continue;
            if (out.convert.isEmpty() && a->text().contains(QStringLiteral("转为"))) out.convert.push_back(a);
            else if (out.display.isEmpty()
                     && (a->text().contains(QStringLiteral("显示"))
                         || a->text().contains(QStringLiteral("重新打开")))) out.display.push_back(a);
        }
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
    // 大 fixture 按用例懒创建；指定运行聚焦用例时不再无意义写入 120 MiB。
    QString l2FixtureD001_;
    QString l2FixtureD003_;
    QString l2FixtureD007_;
    QString l1FixtureD007_;

private slots:
    void initTestCase() {
        QVERIFY(tmpDir_.isValid());
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
        //   showReplaceDialog 就 early return 不 show。这里如果 count==0 就触发「文件→新建」
        //   动作造一个空 tab（当前产品默认是加密笔记，等价于 Ctrl+N），是用户点 Ctrl+R
        //   前一定满足的前置。
        if (tabCount(mw_) == 0) {
            auto* actNew = mw_->findChild<QAction *>(QStringLiteral("file.new"));
            QVERIFY2(actNew != nullptr,
                     "SR02-C 需要先造空 tab，但找不到统一的新建命令 file.new");
            actNew->trigger();
            QApplication::processEvents(QEventLoop::AllEvents, 80);
        }
        auto* tabBefore = currentEditorTab(mw_);
        QVERIFY2(tabBefore != nullptr, "SR02-C 前置：MainWindow 当前没有激活的 EditorTab，showReplaceDialog 会 early return 不弹窗");
        QCOMPARE(tabCount(mw_) >= 1, true);
        auto* actReplace = findMenuActionByExactDisplay(mw_, QStringLiteral("替换"));
        QVERIFY(actReplace != nullptr);
        // 触发动作前：所有已存在的 FindReplaceDialog（MainWindow 构造时 new 的长期对象）must not be visible。
        const auto before = mw_->findChildren<FindReplaceDialog*>();
        QVERIFY2(!before.isEmpty(), "MainWindow 应该已经构造了 FindReplaceDialog（长期子对象），但 findChildren 为空——构造函数里 m_findDialog 没 new?");
        bool anyVisibleBefore = false;
        for (auto* d : before) if (d->isVisible()) { anyVisibleBefore = true; break; }
        QVERIFY2(!anyVisibleBefore, "触发替换动作前 FindReplaceDialog 已 visible（上一个 case 误 trigger 了 showReplaceDialog）");
        FindReplaceDialog* expectDlg = before.front();
        QVERIFY(expectDlg != nullptr);
        // 触发动作（等价于用户 Ctrl+R 触发）：trigger() 会经由 MainWindow::showReplaceDialog →
        // m_findDialog->showReplaceMode() → show() / raise() / activateWindow()
        actReplace->trigger();
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
        QVERIFY2(sizeStatus->text().contains(QStringLiteral("B2")),
                 qPrintable(QStringLiteral("大文件状态栏必须显示分级，实际：%1")
                                .arg(sizeStatus->text())));
        QVERIFY2(sizeStatus->text().contains(QStringLiteral("只读分析")),
                 qPrintable(QStringLiteral("B2 Hex 回退必须明确显示运行态，实际：%1")
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

    // ========== D003：L2 Hex 禁用编码；切换 Text 后仅“以编码显示”可用 ==========
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
        // Hex 是原始字节呈现，转换和文本重新解释都不可用。
        QStringList failConvert;
        for (int i = 0; i < encs.convert.size(); ++i) {
            QAction* a = encs.convert[i];
            if (a->isEnabled()) failConvert.push_back(QStringLiteral("[%1]=%2").arg(i).arg(a->text()));
        }
        QStringList failDisplay;
        for (int i = 0; i < encs.display.size(); ++i) {
            QAction* a = encs.display[i];
            if (a->isEnabled()) failDisplay.push_back(QStringLiteral("[%1]=%2").arg(i).arg(a->text()));
        }
        QVERIFY2(failConvert.isEmpty(), qPrintable(
            QStringLiteral("D003 FAIL：L2 下「转为...」这些动作仍 enabled=true → %1")
                .arg(failConvert.join("、"))));
        QVERIFY2(failDisplay.isEmpty(), qPrintable(
            QStringLiteral("D003 FAIL：L2 Hex 下「以编码显示...」仍可点击 → %1")
                .arg(failDisplay.join("、"))));
        QAction *textPresentation = mw_->findChild<QAction *>(
            QStringLiteral("view.presentation.text"));
        QAction *hexPresentation = mw_->findChild<QAction *>(
            QStringLiteral("view.presentation.hex"));
        QAction *presentationToggle = mw_->findChild<QAction *>(
            QStringLiteral("view.presentation.toggle"));
        QVERIFY(textPresentation && hexPresentation && presentationToggle);
        QVERIFY(textPresentation->isEnabled() && hexPresentation->isEnabled());
        QVERIFY(hexPresentation->isChecked() && !textPresentation->isChecked());
        textPresentation->trigger();
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(et->viewModeL2(), ViewMode::Text);
        QVERIFY(textPresentation->isChecked() && !hexPresentation->isChecked());
        const EncodingActions textEncodings = collectEncodingActions(mw_);
        for (QAction *action : textEncodings.convert)
            QVERIFY(!action->isEnabled());
        for (QAction *action : textEncodings.display)
            QVERIFY(action->isEnabled());
        presentationToggle->trigger();
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        QCOMPARE(et->viewModeL2(), ViewMode::Hex);
        QVERIFY(hexPresentation->isChecked() && !textPresentation->isChecked());
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
        QVERIFY2(!sortAsc()->isEnabled(), "D007 阶段2 切 L2：行升序排序应该立即变灰 enabled=false（D002 切 tab 同步）");
        QVERIFY2(!sortDesc()->isEnabled(), "D007 阶段2 切 L2：行降序排序应该立即变灰 enabled=false（D002 切 tab 同步）");
        {
            auto e = encActs();
            for (int i = 0; i < e.convert.size(); ++i) {
                QVERIFY2(!e.convert[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段2 L2：转为[%1] %2 应该 enabled=false，实际 true（D004 切 tab 同步失败）")
                        .arg(i).arg(e.convert[i]->text())));
            }
            // Hex 下文本编码重新解释没有意义，显示组也应禁用。
            for (int i = 0; i < e.display.size(); ++i) {
                QVERIFY2(!e.display[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段2 L2 Hex：显示[%1] %2 应该 disabled").arg(i).arg(e.display[i]->text())));
            }
        }

        // ---- D007-阶段 3：切回 tab0(L1) → 动作全部恢复 enabled=true（D002/D004 反向同步）
        switchToTab(mw_, 0);
        auto* backL1 = currentEditorTab(mw_);
        QVERIFY(backL1 && !backL1->isL2Active());
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
        QVERIFY(asciiPreview->toPlainText().startsWith(QStringLiteral("................")));

        QAction *hexAction = mw_->findChild<QAction *>(
            QStringLiteral("view.presentation.hex"));
        QVERIFY(hexAction && hexAction->isEnabled() && hexAction->isChecked());
        bool rawByteStatus = false;
        bool hexStatus = false;
        for (QLabel *label : mw_->findChildren<QLabel *>()) {
            rawByteStatus |= label->text() == QStringLiteral("原始字节");
            hexStatus |= label->text() == QStringLiteral("十六进制");
        }
        QVERIFY(rawByteStatus);
        QVERIFY(hexStatus);

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
        QTRY_VERIFY_WITH_TIMEOUT(tab->editor()->text().startsWith(
                                     QStringLiteral("FF 41 02 03")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
                                     QStringLiteral(".A.")), 1000);

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
                                     QStringLiteral(".AZ")), 1000);
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
                                     QStringLiteral(".A.")), 1000);
        tab->redo();
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(11),
                                  QStringLiteral("FF 41 5A 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
                                     QStringLiteral(".AZ")), 1000);

        QAction *saveAction = findMenuActionByExactDisplay(mw_, QStringLiteral("保存"));
        QVERIFY(saveAction != nullptr);
        saveAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(tab->l2View()->isOpen(), 5000);
        {
            QFile saved(sourcePath);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), expected);
        }

        // Exercise Save As backing-store handoff, then reopen through the same
        // MainWindow dispatcher used by Open/recent/drag-drop/session paths.
        const QString copyPath = tmpDir_.filePath(QStringLiteral("hex-roundtrip-copy.dat"));
        QString saveAsError;
        QVERIFY2(tab->saveAsFile(copyPath, &saveAsError), qPrintable(saveAsError));
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
        QVERIFY(asciiPreview->toPlainText().startsWith(QStringLiteral(".AZ")));
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET), 0L);

        // Selection in either visual column represents one half-open source
        // byte range and must be reflected immediately in the other column.
        tab->editor()->setFocus(Qt::OtherFocusReason);
        tab->editor()->setSelection(0, 0, 0, 5); // bytes [0, 2)
        QTRY_COMPARE_WITH_TIMEOUT(asciiPreview->textCursor().selectedText(),
                                  QStringLiteral(".A"), 1000);
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
            restoredSizeStatus |= label->text() == QStringLiteral("512 B · B1 · OVR");
        QVERIFY(restoredSizeStatus);

        // Insert toggles a single document-level mode shared by both byte
        // columns. In ASCII it inserts one byte; Undo restores the exact source.
        QTextCursor insertCursor(asciiPreview->document());
        insertCursor.setPosition(3);
        asciiPreview->setTextCursor(insertCursor);
        asciiPreview->setFocus(Qt::OtherFocusReason);
        QTest::keyClick(asciiPreview, Qt::Key_Insert);
        QVERIFY(!tab->hexOverwriteMode());
        QTest::keyClicks(asciiPreview, QStringLiteral("Q"));
        QTRY_COMPARE_WITH_TIMEOUT(tab->editor()->text().left(14),
                                  QStringLiteral("FF 41 5A 51 03"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(asciiPreview->toPlainText().startsWith(
                                     QStringLiteral(".AZQ")), 1000);
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
                                     QStringLiteral("FF 41 5A A0 03")), 1000);
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
                                     QStringLiteral(".AZ")), 2000);
        QCOMPARE(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET), 0L);
        const QString narrowScreenshot = QCoreApplication::applicationDirPath()
                                         + QStringLiteral("/gui-hex-roundtrip-narrow.png");
        QVERIFY2(mw_->grab().save(narrowScreenshot), qPrintable(narrowScreenshot));
        mw_->resize(1400, 900);
        QTRY_COMPARE_WITH_TIMEOUT(tab->l2View()->hexBytesPerRow(), wideBytesPerRow, 2000);
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
        qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void tc_binary_text_presentation_survives_session_restore() {
        const QString dataDir = tmpDir_.filePath(QStringLiteral("binary-text-session-data"));
        QDir().mkpath(dataDir);
        qputenv("NOTEPADSAFE_DATADIR", dataDir.toUtf8());
        const QString sourcePath = tmpDir_.filePath(
            QStringLiteral("binary-presented-as-text.dat"));
        QByteArray bytes("BINARY-AS-TEXT\n", 15);
        bytes.append('\0');
        bytes.append("tail\n", 5);
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(source.write(bytes), qint64(bytes.size()));
        }

        auto *first = new MainWindow;
        first->resize(1050, 680);
        first->show();
        QVERIFY(QTest::qWaitForWindowExposed(first));
        first->openFilesFromExternal({sourcePath});
        QVERIFY(waitForTabCountAndL2(first, 1, true, 5000));
        EditorTab *source = currentEditorTab(first);
        QVERIFY(source && source->viewModeL2() == ViewMode::Hex);
        source->setViewModeL2(ViewMode::Text);
        QTRY_COMPARE_WITH_TIMEOUT(source->viewModeL2(), ViewMode::Text, 2000);
        QVERIFY(source->isBinaryDocument());

        SessionManager session(dataDir);
        QVERIFY(session.saveSession({source}, 0, false));
        const SessionData stored = session.loadSession();
        QCOMPARE(stored.tabs.size(), 1);
        QCOMPARE(stored.tabs.first().presentation, QStringLiteral("text"));

        source->shutdownL2BeforeClose();
        delete first;
        QApplication::processEvents(QEventLoop::AllEvents, 80);

        auto *restoredWindow = new MainWindow;
        restoredWindow->resize(1050, 680);
        restoredWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(restoredWindow));
        restoredWindow->deferredRestoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(restoredWindow), 1, 5000);
        EditorTab *restored = currentEditorTab(restoredWindow);
        QTRY_VERIFY_WITH_TIMEOUT(restored && restored->l2View()
                                 && restored->l2View()->isOpen(), 5000);
        QCOMPARE(restored->viewModeL2(), ViewMode::Text);
        QVERIFY(restored->isBinaryDocument());
        QVERIFY(restored->editor()->text().startsWith(
            QStringLiteral("BINARY-AS-TEXT")));
        QVERIFY(!restored->editor()->text().startsWith(
            QStringLiteral("42 49 4E")));

        const QString screenshot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-binary-text-session.png");
        QVERIFY2(restoredWindow->grab().save(screenshot), qPrintable(screenshot));
        closeAllEditorTabs(restoredWindow);
        // This case verifies session presentation, not the application-wide
        // close transaction (accounts workspace, autosave and security-session
        // shutdown). All document resources have already been stopped above.
        delete restoredWindow;
        qunsetenv("NOTEPADSAFE_DATADIR");
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
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.presentText")));
        QVERIFY(menuAction(menu, QStringLiteral("tabContext.presentHex")));
        QVERIFY(!menuAction(menu, QStringLiteral("tabContext.presentText"))->isEnabled());
        QVERIFY(!menuAction(menu, QStringLiteral("tabContext.presentHex"))->isEnabled());
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
        QAction *presentText = menuAction(menu, QStringLiteral("tabContext.presentText"));
        QAction *presentHex = menuAction(menu, QStringLiteral("tabContext.presentHex"));
        QVERIFY(presentText && presentText->isEnabled());
        QVERIFY(presentHex && !presentHex->isEnabled());
        QPointer<QMenu> binaryHexMenuGuard(menu);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(presentText).center());
        QTRY_VERIFY_WITH_TIMEOUT(binaryHexMenuGuard.isNull()
                                 || !binaryHexMenuGuard->isVisible(), 1000);
        EditorTab *binaryTab = qobject_cast<EditorTab *>(tabs->currentWidget());
        QVERIFY(binaryTab && binaryTab->isBinaryDocument());
        QTRY_COMPARE_WITH_TIMEOUT(binaryTab->viewModeL2(), ViewMode::Text, 2000);
        // Let the previous popup's native close animation finish before
        // measuring the next popup; otherwise Windows can expose only the
        // final scroll strip during a zero-delay reopen.
        QTest::qWait(180);

        openTabMenu(tabs->currentIndex());
        QTRY_VERIFY_WITH_TIMEOUT(visibleTabMenu() != nullptr, 2000);
        menu = visibleTabMenu();
        presentText = menuAction(menu, QStringLiteral("tabContext.presentText"));
        presentHex = menuAction(menu, QStringLiteral("tabContext.presentHex"));
        QVERIFY(presentText && !presentText->isEnabled());
        QVERIFY(presentHex && presentHex->isEnabled());
        QAction *closeOthers = menuAction(menu, QStringLiteral("tabContext.closeOthers"));
        QVERIFY(closeOthers && closeOthers->isEnabled());
        QTest::qWait(80);
        const QString binaryMenuShot = QCoreApplication::applicationDirPath()
                                       + QStringLiteral("/gui-tab-context-binary-text.png");
        QVERIFY2(menu->grab().save(binaryMenuShot), qPrintable(binaryMenuShot));
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(closeOthers).center());
        QTRY_COMPARE_WITH_TIMEOUT(tabCount(mw_), 1, 3000);
        QCOMPARE(currentEditorTab(mw_), binaryTab);
        QVERIFY(binaryTab->isBinaryDocument());
        QVERIFY(binaryTab->viewModeL2() == ViewMode::Text);

        closeAllEditorTabs(mw_);
    }

    void tc_reopen_with_encoding_protects_unsaved_text() {
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

        QAction *reopenAsUtf8Bom = findMenuActionByExactDisplay(
            mw_, QStringLiteral("以 UTF-8 (含 BOM) 重新打开"));
        QVERIFY(reopenAsUtf8Bom && reopenAsUtf8Bom->isEnabled());
        tab->editor()->setText(QStringLiteral("UNSAVED 用户修改"));
        tab->setModified(true);

        bool cancelDialogSeen = false;
        QTimer::singleShot(50, this, [&]() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible()
                    || box->windowTitle() != QStringLiteral("以编码重新打开")) {
                    continue;
                }
                cancelDialogSeen = true;
                const QString screenshot = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/gui-encoding-reopen-unsaved.png");
                box->grab().save(screenshot);
                for (QPushButton *button : box->findChildren<QPushButton *>()) {
                    if (button->text().contains(QStringLiteral("取消"))) {
                        QTest::mouseClick(button, Qt::LeftButton);
                        return;
                    }
                }
            }
        });
        reopenAsUtf8Bom->trigger();
        QVERIFY(cancelDialogSeen);
        QCOMPARE(tab->editor()->text(), QStringLiteral("UNSAVED 用户修改"));
        QVERIFY(tab->isModified());
        QCOMPARE(tab->encoding(), TextEncoding::Utf8);

        bool discardDialogSeen = false;
        QTimer::singleShot(50, this, [&]() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible()
                    || box->windowTitle() != QStringLiteral("以编码重新打开")) {
                    continue;
                }
                discardDialogSeen = true;
                for (QPushButton *button : box->findChildren<QPushButton *>()) {
                    if (button->text().contains(QStringLiteral("放弃修改"))) {
                        QTest::mouseClick(button, Qt::LeftButton);
                        return;
                    }
                }
            }
        });
        reopenAsUtf8Bom->trigger();
        QVERIFY(discardDialogSeen);
        QCOMPARE(tab->editor()->text(), QString::fromUtf8(original));
        QVERIFY(!tab->isModified());
        QCOMPARE(tab->encoding(), TextEncoding::Utf8Bom);
        QCOMPARE(tab->displayEncoding(), TextEncoding::Utf8Bom);
        QVERIFY(reopenAsUtf8Bom->isChecked());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), original);
        }
        closeAllEditorTabs(mw_);
    }

    void tc_encoding_conversion_preserves_detected_line_endings() {
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
        auto *encodingStatus = mw_->findChild<QLabel *>(QStringLiteral("status.encoding"));
        QVERIFY(encodingStatus);
        QVERIFY(encodingStatus->text().contains(QStringLiteral("CRLF")));

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

        QAction *convertUtf16 = findMenuActionByExactDisplay(
            mw_, QStringLiteral("转为 UTF-16 Little Endian"));
        QVERIFY(convertUtf16 && convertUtf16->isEnabled());
        convertUtf16->trigger();
        QCOMPARE(tab->encoding(), TextEncoding::Utf16LE);
        QVERIFY(tab->isModified());
        saveAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tab->isModified(), 3000);
        QByteArray utf16Bytes;
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            utf16Bytes = file.readAll();
        }
        QVERIFY(utf16Bytes.startsWith(QByteArray("\xff\xfe", 2)));
        const DecodeResult decoded = decodeBytes(utf16Bytes);
        QCOMPARE(decoded.encoding, TextEncoding::Utf16LE);
        QVERIFY(decoded.text.contains(QStringLiteral("beta\r\ngamma")));
        QCOMPARE(tab->lineEndingName(), QStringLiteral("CRLF"));
        QVERIFY(encodingStatus->text().contains(QStringLiteral("UTF-16LE")));
        QVERIFY(encodingStatus->text().contains(QStringLiteral("CRLF")));
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
        QAction *toolbarVisibility = window->findChild<QAction *>(
            QStringLiteral("view.toolbar"));
        QAction *statusVisibility = window->findChild<QAction *>(
            QStringLiteral("view.statusBar"));
        QToolBar *toolbar = window->findChild<QToolBar *>(QStringLiteral("MainToolBar"));
        QVERIFY(toolbarVisibility && statusVisibility && toolbar);
        QVERIFY(!toolbarVisibility->isChecked() && !toolbar->isVisible());
        QVERIFY(!statusVisibility->isChecked() && !window->statusBar()->isVisible());
        toolbarVisibility->trigger();
        statusVisibility->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(toolbar->isVisible(), 1000);
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

        QMenu *presentation = window->findChild<QMenu *>(
            QStringLiteral("view.presentation"));
        QMenu *edge = window->findChild<QMenu *>(QStringLiteral("view.edge"));
        QMenu *zoom = window->findChild<QMenu *>(QStringLiteral("view.zoom"));
        QAction *textPresentation = window->findChild<QAction *>(
            QStringLiteral("view.presentation.text"));
        QAction *hexPresentation = window->findChild<QAction *>(
            QStringLiteral("view.presentation.hex"));
        QVERIFY(presentation && edge && zoom && textPresentation && hexPresentation);
        QVERIFY(!textPresentation->isEnabled() && !hexPresentation->isEnabled());

        QMenu *viewMenu = nullptr;
        for (QAction *topAction : window->menuBar()->actions()) {
            if (topAction->menu()
                && stripMnemonicsAndEllipsis(topAction->text()) == QStringLiteral("视图")) {
                viewMenu = topAction->menu();
                break;
            }
        }
        QVERIFY(viewMenu);
        QVERIFY(viewMenu->actions().contains(presentation->menuAction()));
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
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.encoding"))->isVisible());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.context"))->isVisible());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.size"))->isVisible());
        QVERIFY(!window->findChild<QLabel *>(QStringLiteral("status.modified"))->isVisible());
        window->close();
        delete window;
        qunsetenv("NOTEPADSAFE_DATADIR");

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
        qunsetenv("NOTEPADSAFE_DATADIR");

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
                    QThread::msleep(5);
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

        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(activeId).state
                                 == AsyncTaskState::Running, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(failedId).state
                                 == AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(chip->isVisible(), 3000);
        QVERIFY(chip->text().contains(QStringLiteral("失败 1")));

        QTest::mouseClick(chip, Qt::LeftButton);
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
        QVERIFY(cancelAll && clearHistory && failedSummary);
        QVERIFY(cancelAll->isEnabled());
        QVERIFY(clearHistory->isEnabled());
        QCOMPARE(failedSummary->text(), QStringLiteral("失败: 1"));

        const QString runningShot = QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/gui-background-tasks-running.png");
        QVERIFY2(panel->grab().save(runningShot), qPrintable(runningShot));

        QTest::mouseClick(cancelAll, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(activeId).state
                                 == AsyncTaskState::Canceled, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(chip->isVisible(), 2000);
        QCOMPARE(chip->text(), QStringLiteral("任务失败 1"));
        QVERIFY(!cancelAll->isEnabled());

        // FileEncrypt/FileDecrypt rows are always opaque even if a caller
        // accidentally supplies a protected path or account name.
        const qint64 cryptoId = manager.submit(
            AsyncTaskType::FileEncrypt, AsyncTaskPriority::High,
            QStringLiteral("C:/secret/Alice.nsafe"),
            QStringLiteral("password=do-not-show"),
            [](qint64, AsyncCancelPtr,
               const std::function<void(qint64, qint64)> &,
               const std::function<void(const AsyncErrorInfo &)> &) {});
        QTRY_VERIFY_WITH_TIMEOUT(manager.taskById(cryptoId).state
                                 == AsyncTaskState::Finished, 3000);
        const AsyncTask cryptoTask = manager.taskById(cryptoId);
        QCOMPARE(cryptoTask.title, QStringLiteral("保存安全文档"));
        QVERIFY(!cryptoTask.description.contains(QStringLiteral("Alice")));
        QVERIFY(!cryptoTask.description.contains(QStringLiteral("password")));

        const QString failedShot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/gui-background-tasks-failed.png");
        QVERIFY2(panel->grab().save(failedShot), qPrintable(failedShot));

        QTest::mouseClick(clearHistory, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(manager.rowCount(), 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!chip->isVisible(), 2000);
        panel->hide();
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
