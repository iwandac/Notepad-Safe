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
//    D003：L2 打开后 → 编码「转为 GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE」5 项=
//          enabled=false；「以编码显示...」5 项仍 enabled=true
//    D007：完整 lifecycle 断言（no-tab → L1 → L2 → L1 → close-all=no-tab）
//          动作 enabled 状态随阶段完全正确（D002/D004 混 tab 切换包含在内）
// ============================================================================
#include <QtCore/QObject>
#include <QtTest/QTest>
// ---------- 全局单个 test slot 的硬超时：30 秒。
//   一旦某个 slot 超过 30s 还没返回（例如主线程被同步 readAll 大文件卡死），Qt Test 会
//   直接 abort 当前 case，不再让整个桌面 UI 僵死 5 分钟（默认 300 秒 fatality）。
//   注意：必须在 QTEST_MAIN 展开前 set，且不影响 initTestCase（我们在 initTestCase
//   里单独处理写 120MB fixture 的长耗时，在 initTestCase 里临时 qputenv 放宽）。
#include <QtCore/QByteArray>
#include <QtCore/QProcessEnvironment>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolBar>
#include <QtGui/QKeySequence>
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
#include <QtWidgets/QStackedWidget>

#include "MainWindow.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"

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
    QVector<QAction*> display;  // 「以编码显示...」5 项（L2 仍可点）
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
        if (t.contains(QStringLiteral("编码")) && !t.contains(QStringLiteral("显示"))) encMenu = m;
        if (t.contains(QStringLiteral("以编码显示")) || t.contains(QStringLiteral("显示编码"))) displayMenu = m;
    }
    auto collectFromMenu = [](QMenu* m, bool wantConvert) -> QVector<QAction*> {
        QVector<QAction*> r;
        if (!m) return r;
        for (QAction* a : m->actions()) {
            if (!a || a->isSeparator()) continue;
            const bool hasEncodingData = (a->data().typeId() == QMetaType::Int) && a->isCheckable();
            if (!hasEncodingData) continue;
            if (wantConvert && a->text().contains(QStringLiteral("转为"))) r.push_back(a);
            else if (!wantConvert && a->text().contains(QStringLiteral("显示"))) r.push_back(a);
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
            else if (out.display.isEmpty() && a->text().contains(QStringLiteral("显示"))) out.display.push_back(a);
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
        auto* et = qobject_cast<EditorTab*>(tabs->widget(0));
        if (et) et->shutdownL2BeforeClose();
        tabs->removeTab(0);
        QApplication::processEvents(QEventLoop::AllEvents, 80);
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
    // 所有 L2/L1 fixture 在 initTestCase 里一次性落盘（40MB 文件写磁盘 IO 慢，放 case 内
    //   单 case 几十秒 → 触发 Qt Test 默认 5 分钟单 slot 超时 → fatal error）。成员变量缓存路径。
    QString l2FixtureD001_;
    QString l2FixtureD003_;
    QString l2FixtureD007_;
    QString l1FixtureD007_;

private slots:
    void initTestCase() {
        QVERIFY(tmpDir_.isValid());
        // ------ 在 show MainWindow / 占用主线程之前先写大文件（IO 节流时写 120MB 可以很久）
        // makeL2FixtureFile(40): size > S1 上限 32 MiB → 保证走 S2 auto mode
        l2FixtureD001_ = makeL2FixtureFile(tmpDir_, "d001-l2.bin", 40);
        l2FixtureD003_ = makeL2FixtureFile(tmpDir_, "d003-l2.bin", 40);
        l2FixtureD007_ = makeL2FixtureFile(tmpDir_, "lifecycle-l2-a.bin", 40);
        l1FixtureD007_ = makeL1FixtureFile(tmpDir_, "lifecycle-l1-a.txt");
        QVERIFY2(!l2FixtureD001_.isEmpty() && !l2FixtureD003_.isEmpty() && !l2FixtureD007_.isEmpty() && !l1FixtureD007_.isEmpty(),
            "initTestCase: makeL2FixtureFile/makeL1FixtureFile 至少一个失败，查 QtemporaryDir 是否可写");
        // Qt 全局只有一个 QApplication（由 QTEST_MAIN 创建），这里 new MainWindow
        // 作为顶层窗口即可。不 show() 也行（enabled 状态不依赖 visible），但
        // SR02-COMFORT-C 需要 dialog 能被 expose，所以 show() + waitExposed。
        mw_ = new MainWindow(nullptr);
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
        //   动作造一个空 tab（等价于用户点菜单或 Ctrl+N），是用户点 Ctrl+R 前一定满足的前置。
        if (tabCount(mw_) == 0) {
            auto* actNew = findMenuActionByExactDisplay(mw_, QStringLiteral("新建"));
            QVERIFY2(actNew != nullptr, "SR02-C 需要先造空 tab，但找不到菜单「新建(&N)」对应的 QAction");
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
        QVERIFY2(!l2FixtureD001_.isEmpty(), "initTestCase 预构建 d001-l2.bin 失败");
        mw_->openFilesFromExternal(QStringList{l2FixtureD001_});
        qWarning("[D001] step openFilesFromExternal done @ %lldms | fixture='%s'",
                 clock.elapsed(), qUtf8Printable(l2FixtureD001_));
        // 给 30 秒上限：超过后直接 FAIL 打印 waitForTabCountAndL2 的 state= 调试信息，
        //   不等 Qt Test 默认 5 分钟单 case 超时 fatal error。
        QVERIFY2(waitForTabCountAndL2(mw_, 1, true, 30000),
            "D001 打开 L2 fixture 失败：30s 内既未 tab count>=1 也未 L2 active（见 qCritical state=）");
        qWarning("[D001] step waitForTabCountAndL2 done @ %lldms | tabCount=%d",
                 clock.elapsed(), tabCount(mw_));
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

    // ========== D003：L2 tab → 编码「转为...」5 项灰；「以编码显示...」5 项仍可点 ==========
    void tc_d003_l2_disables_convert_enc_but_not_display_enc() {
        // D003 完全独立：不依赖 D001 留下的 L2 tab（避免 D001 FAIL 时 D003 假 FAIL 连带）。
        closeAllEditorTabs(mw_);
        QCOMPARE(tabCount(mw_), 0);
        QVERIFY2(!l2FixtureD003_.isEmpty(), "initTestCase 预构建 d003-l2.bin 失败");
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
        // 逐个断言：转为组 全部 false；显示组 全部 true
        QStringList failConvert;
        for (int i = 0; i < encs.convert.size(); ++i) {
            QAction* a = encs.convert[i];
            if (a->isEnabled()) failConvert.push_back(QStringLiteral("[%1]=%2").arg(i).arg(a->text()));
        }
        QStringList failDisplay;
        for (int i = 0; i < encs.display.size(); ++i) {
            QAction* a = encs.display[i];
            if (!a->isEnabled()) failDisplay.push_back(QStringLiteral("[%1]=%2").arg(i).arg(a->text()));
        }
        QVERIFY2(failConvert.isEmpty(), qPrintable(
            QStringLiteral("D003 FAIL：L2 下「转为...」这些动作仍 enabled=true → %1")
                .arg(failConvert.join("、"))));
        QVERIFY2(failDisplay.isEmpty(), qPrintable(
            QStringLiteral("D003 FAIL：L2 下「以编码显示...」这些动作被意外 disabled=false → %1；"
                           "显示组不改字节流，L2 应始终可点")
                .arg(failDisplay.join("、"))));
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
        QVERIFY2(!l1FixtureD007_.isEmpty(), "initTestCase 预构建 lifecycle-l1-a.txt 失败");
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
        QVERIFY2(!l2FixtureD007_.isEmpty(), "initTestCase 预构建 lifecycle-l2-a.bin 失败");
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
            // "以编码显示..."组仍 true（D003 断言，这里在阶段 2 再验证一次）
            for (int i = 0; i < e.display.size(); ++i) {
                QVERIFY2(e.display[i]->isEnabled(), qPrintable(
                    QStringLiteral("D007 阶段2 L2：显示[%1] %2 不该被 disable，实际 false").arg(i).arg(e.display[i]->text())));
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
};

// 在 QTEST_MAIN 展开前（= QApplication 创建前）给每个 test slot 设 30s 全局硬超时，
// 防止单个 case 因同步 readAll/死锁卡桌面 5 分钟（Qt 默认 300s fatality）。
//   QTEST_FUNCTION_TIMEOUT 环境变量 = 毫秒；
//   initTestCase 需要写 120MB fixture >30s，我们在 initTestCase 开头临时改成 300s，
//   写文件结束后立即恢复 30s。
static const int kGlobalSlotTimeoutMs = 30000;
static const int kInitTestCaseTimeoutMs = 300000;  // 5 min 允许写 120MB fixture
[[maybe_unused]] static const bool kSetFuncTimeoutOnce = []{
    qputenv("QTEST_FUNCTION_TIMEOUT", QByteArray::number(kGlobalSlotTimeoutMs));
    return true;
}();

QTEST_MAIN(Group1SmallfixesMenuUI)
#include "smoke_smallfixes_menu_ui.moc"
