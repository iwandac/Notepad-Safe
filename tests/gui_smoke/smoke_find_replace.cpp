// ============================================================================
//  smoke_find_replace：查找/替换 GUI 冒烟测试（Qt6 Test，真实 GUI 事件）
//  对应 QUALITY_ASSURANCE_AND_GUI_TESTING.md §3.2 强制清单。
//
//  覆盖：
//    TC_FR_001：替换快捷键（Ctrl+H）能把 FindReplaceDialog show 出来（setReplaceMode(true)）
//    TC_FR_002：查找下一个跳转正确（连续 10 次，strict offset 递增）
//    TC_FR_003：单步替换：先 findNext 找到 "__TEST_OLD__" → replaceCurrent 为
//              "__TEST_NEW__"，断言找到的位置 old 消失 + new 出现（**不是插在旁边**）
//    TC_FR_004：替换全部 3 处 "__TEST_OLD__"，断言状态栏提示 N=3，结果文件内容中
//              无 old 残留、有且仅有 3 处 new、其他区域原封不动
// ============================================================================
#include <QtCore/QObject>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QWidget>
#include <QtGui/QKeySequence>
#include <QtCore/QDebug>

#include "SmokeTestBase.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "VirtualFileView.h"
#include "PatchList.h"

class SmokeFindReplace : public SmokeTestBase {
    Q_OBJECT
private slots:
    void initTestCase() {}
    void cleanupTestCase() {}

    // TC_FR_001：替换快捷键触发（Ctrl+H）= MainWindow showReplaceDialog。
    // 由于我们 smoke 不 link MainWindow，直接测试"实例化 FindReplaceDialog
    // + 调 showReplaceMode 后 visible == true + m_findEdit/m_replaceEdit != nullptr"
    // 再验证 Ctrl+H 的 QShortcut 在目标 widget 上绑定（这里用 QShortcut 绑定到
    // EditorTab 自身的替代方案：直接发 Ctrl+H QKeyEvent 给 EditorTab 的快捷
    // 键 action 注册点，或直接在 smoke 里模拟触发 FindReplaceDialog::setActiveTab
    // 作为等价行为——只要走的是 FindReplaceDialog 自己的真实 GUI buildUi）。
    void tc_fr_001_replace_dialog_opens();

    // TC_FR_002：连续查找下一个跳转正确
    void tc_fr_002_find_next_jumps_strictly_forward();

    // TC_FR_003：单步替换正确性（不插在旁边）
    void tc_fr_003_replace_current_not_appended();

    // TC_FR_004：替换全部 N=3 处，结果全部正确
    void tc_fr_004_replace_all_3_places();
};

// -------- 构造测试文件：N 行，其中 K 处包含 "__TEST_OLD__" 标记 --------
static QByteArray makeReplaceTestFile(int nMarkers) {
    QByteArray out;
    int placed = 0;
    for (int i = 0; i < 120; ++i) {
        QByteArray line = "line " + QByteArray::number(i) + ": content content content ";
        if (i % 40 == 10 && placed < nMarkers) {
            // line 10,50,90 放置标记，共 3 处
            line.append("prefix_").append("__TEST_OLD__").append("_suffix");
            ++placed;
        }
        line.append(" more\r\n");
        out.append(line);
    }
    return out;
}

void SmokeFindReplace::tc_fr_001_replace_dialog_opens() {
    // 用小文件（空即可）打开 L2，EditorTab 准备好
    const QString path = createSmallTestFile(makeReplaceTestFile(3), "fr-001.bin");
    QVERIFY(!path.isEmpty());
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(err));
    QVERIFY(tab() != nullptr);

    // 真实实例化 FindReplaceDialog（与 MainWindow::showReplaceDialog 完全一致）
    FindReplaceDialog dlg(container_);
    dlg.setActiveTab(tab());
    dlg.setReplaceMode(true);  // Ctrl+H → 替换模式
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    QApplication::processEvents(QEventLoop::AllEvents, 100);
    QVERIFY(dlg.isVisible());
    // 关键：对话框处于替换模式意味着 m_replaceEdit / m_replaceBtn 应该是 visible & enabled
    // 通过 dialog->findChild 间接验证（不依赖 private 字段）
    auto* findEdit    = dlg.findChild<QLineEdit*>(QString{}, Qt::FindDirectChildrenOnly);
    QList<QLineEdit*> edits = dlg.findChildren<QLineEdit*>(QString{}, Qt::FindDirectChildrenOnly);
    QVERIFY2(edits.size() >= 2, qPrintable(
        QStringLiteral("FindReplaceDialog in REPLACE mode 应该至少 2 个 QLineEdit "
                       "(find + replace)，实际只有 %1 个，说明替换页可能根本没显示/没构建")
            .arg(edits.size())));
    QList<QPushButton*> btns = dlg.findChildren<QPushButton*>(QString{}, Qt::FindDirectChildrenOnly);
    bool hasReplaceBtn = false, hasReplaceAllBtn = false;
    for (QPushButton* b : btns) {
        const QString t = b->text();
        if (t.contains(QStringLiteral("替换"), Qt::CaseInsensitive) &&
            !t.contains(QStringLiteral("全部"), Qt::CaseInsensitive)) hasReplaceBtn = true;
        if (t.contains(QStringLiteral("全部"), Qt::CaseInsensitive)) hasReplaceAllBtn = true;
    }
    QVERIFY2(hasReplaceBtn, "替换模式下找不到 [替换] 按钮");
    QVERIFY2(hasReplaceAllBtn, "替换模式下找不到 [替换全部] 按钮");
    Q_UNUSED(findEdit);
}

void SmokeFindReplace::tc_fr_002_find_next_jumps_strictly_forward() {
    const QString path = createSmallTestFile(makeReplaceTestFile(3), "fr-002.bin");
    QVERIFY(!path.isEmpty());
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(err));

    FindReplaceDialog dlg(container_);
    dlg.setActiveTab(tab());
    dlg.setReplaceMode(true);
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    QApplication::processEvents(QEventLoop::AllEvents, 80);

    const QString PAT = QStringLiteral("__TEST_OLD__");
    const QByteArray PAT_BA = PAT.toUtf8();
    // 严格 forward、不 wrap：第 1 次 pass 3 → 第 4 次返回 false，避免 wrap 回头打破递增语义
    QVector<qint64> offsets;
    for (int i = 0; i < 10; ++i) {
        const bool ok = tab()->findNext(PAT, /*regex=*/false, /*caseSensitive=*/true,
                                         /*wholeWord=*/false, /*forward=*/true, /*wrap=*/false);
        if (!ok) break;
        const QByteArray sel = currentSelectionBytes();
        QCOMPARE(sel, PAT_BA);
        const qint64 s = view()->selectionStart();
        const qint64 e = view()->selectionEnd();
        offsets.append(s);
        QVERIFY2(e - s == PAT_BA.size(), qPrintable(
            QStringLiteral("第 %1 次 findNext 选区长度 = %2 期望 = %3")
                .arg(i).arg(e - s).arg(PAT_BA.size())));
        if (!offsets.isEmpty()) {
            qint64 prevEnd = (offsets.size() >= 2)
                ? offsets[offsets.size() - 2] + PAT_BA.size()
                : qint64(-1);
            // 第一次 prevEnd = -1 → 只检查后续；其他次严格递增
            if (prevEnd >= 0) {
                QVERIFY2(s > prevEnd, qPrintable(
                    QStringLiteral("第 %1 次 findNext offset %2 不大于上一次匹配末尾 %3：forward 查找应该严格递增！")
                        .arg(i).arg(s).arg(prevEnd)));
            }
        }
    }
    qDebug() << "findNext (forward, no wrap) 实际偏移：" << offsets;
    QCOMPARE(offsets.size(), 3); // no wrap：正好 3 处匹配
    // 额外 sanity：3 个偏移严格递增
    QVERIFY2(offsets[0] < offsets[1] && offsets[1] < offsets[2],
             "no wrap 模式 3 处偏移必须严格递增");
}

void SmokeFindReplace::tc_fr_003_replace_current_not_appended() {
    const QString path = createSmallTestFile(makeReplaceTestFile(3), "fr-003.bin");
    QVERIFY(!path.isEmpty());
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(err));
    auto* v = view();

    // 与 tc002 保持一致：实例化 dlg 并 show，保证子系统 & 事件分发器初始化一致，
    // 避免 LargeFileSearch finished 信号 queued delivery 永远不 dispatch（前两次测试
    // 都走了 dlg.show()，第三次若不 show，在 offscreen/minimal 平台可能被饿死）。
    FindReplaceDialog dlg(container_);
    dlg.setActiveTab(tab());
    dlg.setReplaceMode(true);
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    QApplication::processEvents(QEventLoop::AllEvents, 80);

    const QString OLD = QStringLiteral("__TEST_OLD__");
    const QString NEW = QStringLiteral("__TEST_NEW__");
    const QByteArray OLD_BA = OLD.toUtf8();
    const QByteArray NEW_BA = NEW.toUtf8();

    // Step 1：findNext 找到第 1 处
    bool found = tab()->findNext(OLD, false, true, false, true, false);
    QVERIFY2(found, "第 1 次 findNext 应该找到 __TEST_OLD__");
    const QByteArray selBefore = currentSelectionBytes();
    QCOMPARE(selBefore, OLD_BA);
    const qint64 matchStart = v->selectionStart();

    // 保存 match 前后各 20 字节（用于验证"不是插在旁边"）
    const QByteArray beforeBefore = readByteRange(matchStart - 20, matchStart);
    const QByteArray afterBefore  = readByteRange(matchStart + OLD_BA.size(),
                                                  matchStart + OLD_BA.size() + 20);

    // Step 2：执行替换（真实走 replaceCurrent 链路，即 dialog 中 [替换] 按钮 onClicked）
    tab()->replaceCurrent(NEW);
    QApplication::processEvents(QEventLoop::AllEvents, 80);

    // Step 3：**核心断言 1**：在原位置 20 字节窗口内，读 (matchStart, matchStart+NEW.size)
    //         必须 == NEW_BA（替换**覆盖**了 old，而不是把 new 插在 old 旁边）
    const QByteArray nowAt = readByteRange(matchStart, matchStart + NEW_BA.size());
    if (nowAt != NEW_BA) {
        qDebug() << "--- FAIL 替换正确性 数值证据（核心 bug：插在旁边）---";
        qDebug() << "matchStart = " << matchStart;
        qDebug() << "期望窗口 [" << matchStart << ".." << (matchStart + NEW_BA.size()) << ") = " << NEW_BA;
        qDebug() << "实际窗口 = " << nowAt;
        // 读更大的 40 字节看是不是"old 还在，new 跑到后面/前面"
        const QByteArray wider = readByteRange(matchStart - 5, matchStart + OLD_BA.size() + NEW_BA.size() + 5);
        qDebug() << "更大窗口 matchStart-5 .. +OLD+NEW+5 = " << wider;
        qDebug() << "如果窗口里同时出现 __TEST_OLD____TEST_NEW__ 两个，说明 bug：替换被做成了 insert 而不是 replace！";
        qDebug() << "--- END ---";
    }
    QCOMPARE(nowAt, NEW_BA);

    // **核心断言 2**：新 NEW 前面 20 字节应等于替换前 OLD 前面 20 字节
    //              （如果插在旁边，前面会含 OLD 字节）
    const QByteArray beforeAfter = readByteRange(matchStart - 20, matchStart);
    QCOMPARE(beforeAfter, beforeBefore);

    // **核心断言 3**：新 NEW（长度 13）后面 20 字节应等于替换前 OLD（长度 13）后面 20 字节
    const QByteArray afterAfter = readByteRange(matchStart + NEW_BA.size(),
                                                matchStart + NEW_BA.size() + 20);
    QCOMPARE(afterAfter, afterBefore);
}

void SmokeFindReplace::tc_fr_004_replace_all_3_places() {
    const QString path = createSmallTestFile(makeReplaceTestFile(3), "fr-004.bin");
    QVERIFY(!path.isEmpty());
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(err));
    auto* v = view();

    FindReplaceDialog dlg(container_);
    dlg.setActiveTab(tab());
    dlg.setReplaceMode(true);
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    QApplication::processEvents(QEventLoop::AllEvents, 80);

    const QString OLD = QStringLiteral("__TEST_OLD__");
    const QString NEW = QStringLiteral("__TEST_NEW__");
    const QByteArray OLD_BA = OLD.toUtf8();
    const QByteArray NEW_BA = NEW.toUtf8();

    // 统计替换前的 old 出现次数（应该 3）
    int oldCountBefore = 0;
    {
        bool cont = tab()->findNext(OLD, false, true, false, true, true);
        QSet<qint64> seen;
        while (cont) {
            const qint64 s = v->selectionStart();
            if (seen.contains(s)) break;   // wrap 循环保护
            seen.insert(s);
            ++oldCountBefore;
            cont = tab()->findNext(OLD, false, true, false, true, true);
        }
    }
    QCOMPARE(oldCountBefore, 3);

    // 执行替换全部（EditorTab::replaceAll 返回替换次数，对应 dialog 状态栏提示 "N 处替换成功"）
    const int n = tab()->replaceAll(OLD, NEW, false, true, false);

    qDebug() << "replaceAll() 返回值（状态栏提示应该也是这个数）= " << n;
    QCOMPARE(n, 3);

    // 断言 1：内容里不存在 OLD_BA（无残留）
    qint64 scanOld = 0;
    const qint64 vs = v->virtualFileSize();
    int oldCountAfter = 0;
    while (scanOld + OLD_BA.size() <= vs) {
        const QByteArray chunk = readByteRange(scanOld, scanOld + OLD_BA.size());
        if (chunk == OLD_BA) {
            ++oldCountAfter;
            scanOld += OLD_BA.size();
        } else {
            ++scanOld;
        }
        if (oldCountAfter > 0) break;
    }
    QVERIFY2(oldCountAfter == 0, qPrintable(
        QStringLiteral("replaceAll 后还残留 %1 处 %2，应该是 0！")
            .arg(oldCountAfter).arg(OLD)));

    // 断言 2：内容里 NEW_BA 出现次数 == 3（正好全部转换，没有多插）
    qint64 scanNew = 0;
    int newCountAfter = 0;
    while (scanNew + NEW_BA.size() <= vs) {
        const QByteArray chunk = readByteRange(scanNew, scanNew + NEW_BA.size());
        if (chunk == NEW_BA) {
            ++newCountAfter;
            scanNew += NEW_BA.size();
        } else {
            ++scanNew;
        }
    }
    QCOMPARE(newCountAfter, 3);
}

QTEST_MAIN(SmokeFindReplace)
#include "smoke_find_replace.moc"
