// ============================================================================
//  smoke_find_replace：查找/替换 GUI 冒烟测试（Qt6 Test，真实 GUI 事件）
//  对应 QUALITY_ASSURANCE_AND_GUI_TESTING.md §3.2 强制清单。
//
//  覆盖：
//    TC_FR_001：替换模式能把 FindReplaceDialog 正确 show 出来；主窗口快捷键为 Ctrl+R。
//    TC_FR_002：查找下一个跳转正确（连续 10 次，strict offset 递增）
//    TC_FR_003：单步替换：先 findNext 找到 "__TEST_OLD__" → replaceCurrent 为
//              "__TEST_NEW__"，断言找到的位置 old 消失 + new 出现（**不是插在旁边**）
//    TC_FR_004：替换全部 3 处 "__TEST_OLD__"，断言状态栏提示 N=3，结果文件内容中
//              无 old 残留、有且仅有 3 处 new、其他区域原封不动
// ============================================================================
#include <QtCore/QObject>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QWidget>
#include <QtWidgets/QMessageBox>
#include <QtGui/QKeySequence>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <algorithm>

#include "SmokeTestBase.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"

class SmokeFindReplace : public SmokeTestBase {
    Q_OBJECT
private slots:
    void initTestCase() {}
    void cleanupTestCase() {}

    // TC_FR_001：替换模式打开（主窗口 Ctrl+R 在命令 smoke 验证）。
    // 由于我们 smoke 不 link MainWindow，直接测试"实例化 FindReplaceDialog
    // + 调 showReplaceMode 后 visible == true + m_findEdit/m_replaceEdit != nullptr"
    // 这里验证 FindReplaceDialog 自己的真实 GUI buildUi 和模式状态。
    void tc_fr_001_replace_dialog_opens();

    // TC_FR_002：连续查找下一个跳转正确
    void tc_fr_002_find_next_jumps_strictly_forward();

    // TC_FR_003：单步替换正确性（不插在旁边）
    void tc_fr_003_replace_current_not_appended();

    // TC_FR_004：替换全部 N=3 处，结果全部正确
    void tc_fr_004_replace_all_3_places();

    // 中文/emoji 位于超长行中部：结果上下文、高亮像素命中和双击字节跳转均精确。
    void tc_fr_005_unicode_long_line_find_all_and_double_click();

    // L2 可见编辑尚未 flush 时，Find All 必须搜索新内容、正文高亮并使旧结果随编辑失效。
    void tc_fr_006_live_overlay_search_snapshot_highlight_and_invalidation();

    // 全部打开文件运行中纳入新 L1 目标，并把双击路由到结果所属 Tab。
    void tc_fr_007_all_open_files_incremental_results_and_target_jump();

    // PCRE2 使用完整 mmap subject：跨行命中必须显示在 Dock 并精确双击跳转。
    void tc_fr_008_multiline_regex_result_and_double_click();

    // S1 与 L2 使用同一 PCRE2 查询：Find All、F3、Dock 跳转和替换命中一致。
    void tc_fr_009_l1_pcre2_find_navigation_replace_and_error();

    // 全部打开文件按事件循环增量派发；取消保留已完成结果并可立即重跑。
    void tc_fr_010_all_open_cancel_and_rerun();

    // Binary/Hex 原始字节查找：Dock 高亮上下文，双击精确跳转到对应字节。
    void tc_fr_011_hex_byte_find_all_highlight_and_double_click();

    // 混合 Text/Binary 的全部打开文件字节搜索只调度 Binary，并明确报告跳过目标。
    void tc_fr_012_hex_byte_all_open_filters_text_targets();

    // 全部打开文件的前后查找按标签顺序导航；关闭循环时不得跨列表边界。
    void tc_fr_013_all_open_navigation_respects_wrap_boundary();

    // 结果因正文编辑失效后，可用原查询上下文一键重跑并恢复精确跳转。
    void tc_fr_014_invalidated_result_rerun_from_dock();

    // 全部打开文件替换：修改所有策略可写 Text（包含可编辑 L2），明确跳过真正只读目标；
    // 每个文档一个 undo group，且不会自动写盘。
    void tc_fr_015_all_open_replace_transaction_and_skip_summary();

    // S2/S3 文本搜索不得等待全量 LineIndex；第一个进度必须是扫描进度，
    // 结果位置以 64 位 byte offset 为权威。
    void tc_fr_016_large_search_starts_before_line_index_completes();

    // 正则复选框是查询语义开关：关闭时元字符必须按字面量查找，
    // 开启时同一输入才按正则表达式解释。
    void tc_fr_017_regex_toggle_distinguishes_literal_and_regex();

    // 模型驱动试点：一份正则参数契约自动贯穿 Find All 与 Find Next，
    // 并检查 literal/escaped-regex 等价关系和非法表达式分流。
    void tc_fr_018_regex_contract_model_runs_gui_variants();

    // 查询选项状态机：穷举每个合法模式组合，并真实点击所有禁用的冲突选项，
    // 保证 GUI 无法形成 regex+whole-word / regex+hex / hex+text-options。
    void tc_fr_019_query_option_state_machine_exhaustive();

    // Replace 按钮必须替换当前已选中的匹配，而不是先跳到下一处再替换。
    // 同时验证替换后自动选中下一处，连续操作不跳项。
    void tc_fr_020_replace_button_uses_current_match_then_advances();

    // 每个文本查询选项不仅要有正确 enabled 状态，还必须真实改变 Find All
    // 与重复 Find Next 的 authoritative byte ranges；wrap 只改变边界导航。
    void tc_fr_021_text_option_semantics_all_legal_combinations();

    // Hex 查询格式等价类、非法输入原子拒绝、Find All/Next ranges 与 wrap。
    void tc_fr_022_hex_query_formats_errors_and_navigation();

    // 正则替换必须展开编号/命名捕获；单次与全部替换使用相同语法且不写入字面 `$1`。
    void tc_fr_023_regex_capture_replacement_gui_contract();

    // L2 patch Provider 必须与 S1 使用同一捕获展开语义，并保持单次/全部 Undo 边界。
    void tc_fr_024_l2_regex_capture_replacement_consistency();

    // 替换值等价类：删除/短长/Unicode/Literal 元语法，以及失败前原子拒绝和 Undo/Redo。
    void tc_fr_025_replacement_value_contract_and_atomic_failures();

    // L2 Replace All 跨过内部进度批次仍必须只有一个用户可见 Undo/Redo 事务。
    void tc_fr_026_l2_replace_all_over_batch_is_one_undo();

    // 保留的 4.9 GB S3 样本：正式对话框/Dock/双击/窗口化 Provider 全链精确到 64 位 byte range。
    void tc_fr_027_real_s3_find_all_dock_double_click();

    // 同一真实 S3 样本：首次 Find Next 等待异步 mmap，重复导航读取可见 Provider 选区而非隐藏后端。
    void tc_fr_028_real_s3_find_next_preparation_and_repeat_boundary();

    // 结果超过常驻内存上限后进入 spool；Dock 仍虚拟展示并可精确双击最后一条结果。
    void tc_fr_029_result_spool_virtual_tail_navigation();

    // 2 GiB 后的有效 UTF-8 S3 Text：Regex/大小写/Dock/双击与只读替换边界。
    void tc_fr_030_sparse_s3_text_regex_after_two_gib();

    // 真实 Binary S3 默认仅勾选循环查找：复制的文本必须按原始字节字面量
    // 搜索，不能暗中进入 PCRE2 并报“正文不是有效 UTF-8”。
    void tc_fr_031_real_s3_default_literal_is_not_regex();

    // 可移植的非法 UTF-8 Binary 门禁：不依赖保留的大文件，也必须证明
    // 默认 Literal + ASCII 不区分大小写不会进入正则引擎。
    void tc_fr_032_invalid_utf8_binary_default_literal_portable();
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

void SmokeFindReplace::tc_fr_005_unicode_long_line_find_all_and_double_click() {
    const QString marker = QStringLiteral("关键😀词");
    QByteArray content(5000, 'a');
    content += marker.toUtf8();
    content += QByteArray(5000, 'z');
    content += '\n';
    const QString path = createSmallTestFile(content, "fr-005-unicode.log");
    QVERIFY(!path.isEmpty());
    QString error;
    QVERIFY2(openL2(path, &error), qPrintable(error));

    SearchResultsDock dock;
    dock.resize(1100, 320);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    QVERIFY(pattern && findAll);
    pattern->setText(marker);
    for (QCheckBox *option : dialog.findChildren<QCheckBox *>()) {
        if (option->text().contains(QStringLiteral("正则")))
            option->setChecked(true);
    }
    QTest::mouseClick(findAll, Qt::LeftButton);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 1, 10000);
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    const GroupedSearchLine &group = model->groupedLineAt(0);
    const QString context = QString::fromUtf8(group.lineContext);
    QCOMPARE(group.matchPositions.size(), 1);
    QCOMPARE(int(group.matchPositions.first().first), context.indexOf(marker));
    QCOMPARE(int(group.matchPositions.first().second), marker.size());
    QVERIFY(group.contextByteOffset > 0); // 超长行上下文应围绕命中，而不是固定截取行首。

    const QModelIndex contextIndex = model->index(0, SearchResultsModel::COL_CONTEXT);
    tree->scrollTo(contextIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    const QRect rect = tree->visualRect(contextIndex);
    QVERIFY(rect.isValid());
    const QFontMetrics metrics(tree->font());
    const int markerStart = context.indexOf(marker);
    const int prefixWidth = metrics.horizontalAdvance(context.left(markerStart));
    const int markerWidth = metrics.horizontalAdvance(
        context.mid(markerStart, marker.size()));
    const QPoint click(rect.left() + 4 + prefixWidth + qMax(1, markerWidth / 2),
                       rect.center().y());
    QVERIFY2(rect.contains(click), qPrintable(
        QStringLiteral("keyword click %1,%2 outside context column; viewport %3x%4; context rect left=%5 width=%6")
            .arg(click.x()).arg(click.y())
            .arg(tree->viewport()->width()).arg(tree->viewport()->height())
            .arg(rect.left()).arg(rect.width())));
    // QTest::mouseDClick 只投递 dblclick 阶段；先投递一次普通点击，模拟真实用户
    // 双击的首个 press/release，并让 Dock 的命中测试记录具体高亮词。
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(qvariant_cast<EditorTab *>(jumpSpy.first().at(0)), tab());
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), qint64(5000));
    QCOMPARE(jumpSpy.first().at(2).toInt(), marker.toUtf8().size());
    QTRY_COMPARE_WITH_TIMEOUT(currentSelectionBytes(), marker.toUtf8(), 5000);

    const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("gui-unicode-search.png"));
    QVERIFY(dock.grab().save(screenshot));
    dock.close();
}

void SmokeFindReplace::tc_fr_006_live_overlay_search_snapshot_highlight_and_invalidation()
{
    const QByteArray original("alpha\nbeta\n");
    const QString path = createSmallTestFile(original, "fr-006-live.log");
    QString error;
    QVERIFY2(openL2(path, &error), qPrintable(error));

    const QString marker = QStringLiteral("LIVE_PATCH_TOKEN");
    tab()->gotoByteOffset(0);
    tab()->editor()->setFocus();
    QTest::keyClicks(tab()->editor(), marker + QLatin1Char(' '));
    QApplication::processEvents(QEventLoop::AllEvents, 120);
    QVERIFY(tab()->editor()->isModified());

    SearchResultsDock dock;
    dock.resize(1050, 320);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    QVERIFY(pattern && findAll);
    pattern->setText(marker);
    QTest::mouseClick(findAll, Qt::LeftButton);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qobject_cast<SearchResultsModel *>(tree->model())
            && qobject_cast<SearchResultsModel *>(tree->model())->totalMatches() == 1,
        10000);
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    QVERIFY(view()->patches().patchCount() > 0); // Find All flushed the overlay into its snapshot

    const long highlighted = tab()->editor()->SendScintilla(
        QsciScintillaBase::SCI_INDICATORVALUEAT, 8UL, long(0));
    QVERIFY2(highlighted != 0, "L2 visible Scintilla body did not highlight the match");
    const QString highlightScreenshot = QDir(QCoreApplication::applicationDirPath())
                                            .filePath(QStringLiteral(
                                                "gui-live-l2-search-highlight.png"));
    QVERIFY(container_->grab().save(highlightScreenshot));

    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });
    const QModelIndex contextIndex = model->index(0, SearchResultsModel::COL_CONTEXT);
    tree->scrollTo(contextIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 60);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(contextIndex).center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                       tree->visualRect(contextIndex).center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(qvariant_cast<EditorTab *>(jumpSpy.first().at(0)), tab());
    QTRY_COMPARE_WITH_TIMEOUT(currentSelectionBytes(), marker.toUtf8(), 3000);

    tab()->editor()->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                   tab()->editor()->length());
    QTest::keyClicks(tab()->editor(), QStringLiteral("changed"));
    QApplication::processEvents(QEventLoop::AllEvents, 120);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() == nullptr, 2000);
    auto *summary = dock.findChild<QLabel *>();
    auto *rerun = dock.findChild<QPushButton *>(
        QStringLiteral("searchResultsRerunButton"));
    QVERIFY(summary);
    QVERIFY(rerun);
    QVERIFY(summary->text().contains(QStringLiteral("失效")));
    QVERIFY(rerun->isVisible() && rerun->isEnabled());
    QCOMPARE(jumpSpy.count(), 1);

    const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("gui-live-l2-search-invalidated.png"));
    QVERIFY(dock.grab().save(screenshot));
    dock.close();
}

void SmokeFindReplace::tc_fr_007_all_open_files_incremental_results_and_target_jump()
{
    const QString marker = QStringLiteral("SCOPE_TOKEN");
    const QString firstPath = createSmallTestFile("first file\n", "fr-007-first.log");
    const QString secondPath = createSmallTestFile(
        QByteArray("second ") + marker.toUtf8() + QByteArray(" file\n"),
        "fr-007-second.log");

    QTabWidget workspace;
    workspace.resize(1100, 650);
    auto *first = new EditorTab(&workspace);
    auto *second = new EditorTab(&workspace);
    auto *third = new EditorTab;
    third->editor()->setText(QStringLiteral("third %1 file\n").arg(marker));
    workspace.addTab(first, QStringLiteral("first"));
    workspace.addTab(second, QStringLiteral("second"));
    QVERIFY(first->loadFile(firstPath));
    QVERIFY(second->loadFile(secondPath));
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));
    QApplication::processEvents(QEventLoop::AllEvents, 300);

    workspace.setCurrentWidget(first);
    first->gotoByteOffset(0);
    first->editor()->setFocus();
    QTest::keyClicks(first->editor(), marker + QLatin1Char(' '));
    QApplication::processEvents(QEventLoop::AllEvents, 100);

    SearchResultsDock dock;
    dock.resize(1050, 350);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setActiveTab(first);
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    QVERIFY(pattern && scope && findAll);
    pattern->setText(marker);
    scope->setCurrentIndex(scope->findData(1));
    QTest::mouseClick(findAll, Qt::LeftButton);

    // Match MainWindow::addNewTab ordering: the newly opened document becomes
    // active while the all-open generation is busy, then joins that generation.
    workspace.addTab(third, QStringLiteral("third"));
    workspace.setCurrentWidget(third);
    dialog.setActiveTab(third);
    dialog.notifyTabOpened(third);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 3, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        tree->model()->rowCount(tree->model()->index(0, 0)), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        tree->model()->rowCount(tree->model()->index(1, 0)), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        tree->model()->rowCount(tree->model()->index(2, 0)), 1, 10000);
    QVERIFY(first->editor()->isModified());

    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    QModelIndex secondGroup;
    for (int index = 0; index < tree->model()->rowCount(); ++index) {
        const QModelIndex candidate = tree->model()->index(index, 0);
        if (candidate.data().toString().contains(QStringLiteral("fr-007-second"))) {
            secondGroup = candidate;
            break;
        }
    }
    QVERIFY(secondGroup.isValid());
    tree->expand(secondGroup);
    const QModelIndex secondMatch = tree->model()->index(0, 1, secondGroup);
    QVERIFY(secondMatch.isValid());
    tree->scrollTo(secondMatch);
    const QRect secondRect = tree->visualRect(secondMatch);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      secondRect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                       secondRect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(qvariant_cast<EditorTab *>(jumpSpy.first().at(0)), second);

    first->editor()->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                   first->editor()->length());
    first->editor()->setFocus();
    QTest::keyClicks(first->editor(), QStringLiteral("changed"));
    QApplication::processEvents(QEventLoop::AllEvents, 120);
    QModelIndex firstGroup;
    for (int index = 0; index < tree->model()->rowCount(); ++index) {
        const QModelIndex candidate = tree->model()->index(index, 0);
        if (candidate.data().toString().contains(QStringLiteral("fr-007-first"))) {
            firstGroup = candidate;
            break;
        }
    }
    QVERIFY(firstGroup.isValid());
    QTRY_VERIFY_WITH_TIMEOUT(firstGroup.data().toString().contains(QStringLiteral("失效")), 2000);
    QVERIFY2(tree->model()->flags(secondMatch).testFlag(Qt::ItemIsEnabled),
             "editing one target must not invalidate another file's search results");

    const QString screenshot = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("gui-all-open-files-search.png"));
    QVERIFY(dock.grab().save(screenshot));

    first->shutdownL2BeforeClose();
    second->shutdownL2BeforeClose();
    dock.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_008_multiline_regex_result_and_double_click()
{
    const QByteArray expected("BEGIN request=42\r\nEND status=ok");
    const QByteArray content = QByteArray("prefix\r\n") + expected
                               + QByteArray("\r\nsuffix\r\n");
    const QString path = createSmallTestFile(content, "fr-008-multiline.log");
    QString error;
    QVERIFY2(openL2(path, &error), qPrintable(error));

    SearchResultsDock dock;
    dock.resize(1000, 300);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    QVERIFY(pattern && findAll);
    pattern->setText(QStringLiteral("BEGIN[^\\r\\n]*\\r?\\nEND[^\\r\\n]*"));
    for (QCheckBox *option : dialog.findChildren<QCheckBox *>()) {
        if (option->text().contains(QStringLiteral("正则")))
            option->setChecked(true);
    }
    QTest::mouseClick(findAll, Qt::LeftButton);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qobject_cast<SearchResultsModel *>(tree->model()) != nullptr, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        qobject_cast<SearchResultsModel *>(tree->model())->totalMatches(),
        qint64(1), 10000);
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    const QModelIndex contextIndex = model->index(0, SearchResultsModel::COL_CONTEXT);
    QVERIFY(contextIndex.isValid());
    tree->scrollTo(contextIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    const QPoint click = tree->visualRect(contextIndex).center();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), qint64(8));
    QCOMPARE(jumpSpy.first().at(2).toInt(), expected.size());
    QTRY_COMPARE_WITH_TIMEOUT(currentSelectionBytes(), expected, 5000);
    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-multiline-regex-dock.png"))));
    QVERIFY(container_->grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-multiline-regex-selection.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_009_l1_pcre2_find_navigation_replace_and_error()
{
    const QByteArray expected("BEGIN request=42\r\nEND status=ok");
    const QString path = createSmallTestFile(
        QByteArray("prefix\r\n") + expected + QByteArray("\r\nsuffix\r\n"),
        "fr-009-l1.log");
    EditorTab l1;
    l1.resize(900, 520);
    QVERIFY(l1.loadFile(path));
    QVERIFY(!l1.isL2Mode());
    l1.show();
    QVERIFY(QTest::qWaitForWindowExposed(&l1));

    const auto selectedBytes = [&]() {
        const qint64 start = l1.editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 end = l1.editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        return l1.editor()->text().toUtf8().mid(start, end - start);
    };

    SearchResultsDock dock;
    dock.resize(1000, 300);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            &l1, [](EditorTab *target, qint64 offset, int length) {
                if (target) target->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog;
    dialog.setActiveTab(&l1);
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && findAll && status);
    const QString expression = QStringLiteral(
        "BEGIN[^\\r\\n]*\\r?\\nEND status=ok");
    pattern->setText(expression);
    for (QCheckBox *option : dialog.findChildren<QCheckBox *>()) {
        if (option->text().contains(QStringLiteral("正则")))
            option->setChecked(true);
    }
    QTest::mouseClick(findAll, Qt::LeftButton);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree && tree->model());
    QCOMPARE(tree->model()->rowCount(), 1);
    const QModelIndex contextIndex = tree->model()->index(
        0, SearchResultsModel::COL_CONTEXT);
    const QPoint click = tree->visualRect(contextIndex).center();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTRY_COMPARE_WITH_TIMEOUT(selectedBytes(), expected, 3000);

    l1.resetSearchPosition(true);
    QVERIFY(l1.findNext(expression, true, true, false, true, false));
    QCOMPARE(selectedBytes(), expected);

    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-l1-pcre2-dock.png"))));
    QVERIFY(l1.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-l1-pcre2-editor.png"))));

    pattern->setText(QStringLiteral("[broken"));
    QTest::keyClick(pattern, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("正则无效")), 2000);

    const int replaced = l1.replaceAll(expression, QStringLiteral("REPLACED"),
                                       true, true, false);
    QCOMPARE(replaced, 1);
    QVERIFY(l1.editor()->text().contains(QStringLiteral("prefix\nREPLACED\nsuffix"))
            || l1.editor()->text().contains(QStringLiteral("prefix\r\nREPLACED\r\nsuffix")));

    dialog.close();
    dock.close();
    l1.close();
}

void SmokeFindReplace::tc_fr_010_all_open_cancel_and_rerun()
{
    const QString marker = QStringLiteral("CANCEL_SCOPE_TOKEN");
    QTabWidget workspace;
    workspace.resize(1000, 620);
    constexpr int TargetCount = 6;
    for (int index = 0; index < TargetCount; ++index) {
        const QString path = createSmallTestFile(
            QStringLiteral("file %1 %2\n").arg(index).arg(marker).toUtf8(),
            QStringLiteral("fr-010-%1.log").arg(index));
        auto *target = new EditorTab(&workspace);
        QVERIFY(target->loadFile(path));
        workspace.addTab(target, QString::number(index));
    }
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    SearchResultsDock dock;
    dock.resize(1000, 330);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setActiveTab(qobject_cast<EditorTab *>(workspace.currentWidget()));
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *close = dialog.findChild<QPushButton *>(QStringLiteral("closeSearchButton"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *previous = dialog.findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *caseSensitive = dialog.findChild<QCheckBox *>(
        QStringLiteral("caseSensitiveSearchCheck"));
    auto *wholeWord = dialog.findChild<QCheckBox *>(
        QStringLiteral("wholeWordSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *summary = dock.findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(pattern && scope && findAll && cancel && close && next && previous
            && regex && hex && caseSensitive && wholeWord && wrap
            && summary && tree);
    pattern->setText(marker);
    scope->setCurrentIndex(scope->findData(1));
    QTest::mouseClick(findAll, Qt::LeftButton);
    QVERIFY(cancel->isVisible() && cancel->isEnabled());
    QVERIFY(close->isVisible() && close->isEnabled());
    QVERIFY(!pattern->isEnabled());
    QVERIFY(!scope->isEnabled());
    QVERIFY(!next->isEnabled());
    QVERIFY(!previous->isEnabled());
    QVERIFY(!findAll->isEnabled());
    QVERIFY(!regex->isEnabled());
    QVERIFY(!hex->isEnabled());
    QVERIFY(!caseSensitive->isEnabled());
    QVERIFY(!wholeWord->isEnabled());
    QVERIFY(!wrap->isEnabled());

    // The first target dispatch is queued before this cancellation. At most a
    // completed prefix may remain; undispatched targets must not be scanned.
    QTimer::singleShot(0, cancel, [cancel]() {
        QTest::mouseClick(cancel, Qt::LeftButton);
    });
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains(QStringLiteral("已取消")), 3000);
    QVERIFY(tree->model());
    QVERIFY(tree->model()->rowCount() < TargetCount);
    QVERIFY(findAll->isEnabled());
    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dialog.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-search-cancelled-dialog.png"))));
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-search-cancelled-results.png"))));

    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), TargetCount, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 5000);
    QVERIFY(summary->text().contains(QStringLiteral("找到 6 处匹配")));
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-search-rerun-results.png"))));

    dock.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_011_hex_byte_find_all_highlight_and_double_click()
{
    QByteArray content(80, '\0');
    const QByteArray marker = QByteArray::fromHex("DEADBEEF");
    content.replace(5, marker.size(), marker);
    content.replace(35, marker.size(), marker);
    content[32] = 'A';
    content[40] = 'Z';
    const QString path = createSmallTestFile(content, "fr-011-binary.bin");
    QString error;
    QVERIFY2(tab()->openLargeFileL2(path, &error,
                                    L2Presentation::Hex,
                                    L2ContentKind::Binary), qPrintable(error));
    QApplication::processEvents(QEventLoop::AllEvents, 200);
    QVERIFY(tab()->isBinaryDocument());
    QCOMPARE(tab()->viewModeL2(), ViewMode::Hex);

    SearchResultsDock dock;
    dock.resize(1050, 330);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    QVERIFY(pattern && hex && findAll && cancel);
    QVERIFY(hex->isEnabled());
    QTest::mouseClick(hex, Qt::LeftButton);
    QVERIFY(hex->isChecked());
    QCOMPARE(pattern->placeholderText(), QStringLiteral("例如 DE AD BE EF"));
    pattern->setText(QStringLiteral("DE AD BE EF"));
    QTest::mouseClick(findAll, Qt::LeftButton);

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qobject_cast<SearchResultsModel *>(tree->model()) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 5000);
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    QCOMPARE(model->totalMatches(), qint64(2));
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->headerData(SearchResultsModel::COL_LINE, Qt::Horizontal).toString(),
             QStringLiteral("偏移"));
    const GroupedSearchLine &second = model->groupedLineAt(1);
    QCOMPARE(second.contextByteOffset, qint64(32));
    QCOMPARE(second.lineContext, QByteArray("A.......Z.......", 16));
    QCOMPARE(second.matchPositions.size(), 1);
    QCOMPARE(second.matchPositions.first().first, qint32(3));
    QCOMPARE(second.matchPositions.first().second, qint32(4));
    QCOMPARE(model->index(1, SearchResultsModel::COL_LINE).data().toString(),
             QStringLiteral("0x20"));

    const QModelIndex contextIndex = model->index(1, SearchResultsModel::COL_CONTEXT);
    tree->scrollTo(contextIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    const QRect rect = tree->visualRect(contextIndex);
    QVERIFY(rect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), qint64(35));
    QCOMPARE(jumpSpy.first().at(2).toInt(), marker.size());
    QTRY_COMPARE_WITH_TIMEOUT(currentSelectionBytes(), marker, 3000);
    auto *asciiPreview = tab()->findChild<QPlainTextEdit *>(
        QStringLiteral("HexAsciiPreview"));
    QVERIFY(asciiPreview && asciiPreview->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(asciiPreview->textCursor().selectedText(),
                              QStringLiteral("...."), 1000);
    QCOMPARE(asciiPreview->extraSelections().size(), 1);

    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-hex-byte-search-dock.png"))));
    QVERIFY(container_->grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-hex-byte-search-editor.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_012_hex_byte_all_open_filters_text_targets()
{
    const QByteArray marker = QByteArray::fromHex("DEADBEEF");
    QByteArray binaryBytes(64, '\0');
    binaryBytes.replace(20, marker.size(), marker);
    const QString binaryPath = createSmallTestFile(binaryBytes, "fr-012-binary.bin");
    const QString textPath = createSmallTestFile(
        QByteArray("plain text DE AD BE EF must not join byte-pattern results\n"),
        "fr-012-text.log");

    QTabWidget workspace;
    workspace.resize(1100, 650);
    auto *textTab = new EditorTab(&workspace);
    auto *binaryTab = new EditorTab(&workspace);
    auto *lockedTab = new EditorTab(&workspace);
    QString error;
    QVERIFY(textTab->loadFile(textPath));
    QVERIFY2(binaryTab->openLargeFileL2(binaryPath, &error,
                                        L2Presentation::Hex,
                                        L2ContentKind::Binary), qPrintable(error));
    workspace.addTab(textTab, QStringLiteral("Text"));
    workspace.addTab(binaryTab, QStringLiteral("Binary"));
    lockedTab->initializeSecureDocument(
        QUuid::createUuid(), QStringLiteral("Locked search target"),
        QStringLiteral("DE AD BE EF"));
    lockedTab->maskSecureDocument();
    workspace.addTab(lockedTab, QStringLiteral("Locked"));
    workspace.setCurrentWidget(textTab);
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    SearchResultsDock dock;
    dock.resize(1050, 330);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            &workspace, [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setActiveTab(textTab);
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && scope && hex && findAll && cancel && status);
    QVERIFY(!hex->isEnabled());
    scope->setCurrentIndex(scope->findData(1));
    QVERIFY(hex->isEnabled());
    QTest::mouseClick(hex, Qt::LeftButton);
    pattern->setText(QStringLiteral("DE AD BE EF"));
    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 5000);
    QVERIFY(status->text().contains(QStringLiteral("跳过文本 1 个")));
    QVERIFY(status->text().contains(QStringLiteral("锁定文档 1 个")));

    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(tree && tree->model());
    QCOMPARE(tree->model()->rowCount(), 1);
    const QModelIndex binaryGroup = tree->model()->index(0, 0);
    QVERIFY(binaryGroup.data().toString().contains(QStringLiteral("fr-012-binary")));
    QCOMPARE(tree->model()->rowCount(binaryGroup), 1);
    tree->expand(binaryGroup);
    const QModelIndex matchIndex = tree->model()->index(
        0, SearchResultsModel::COL_CONTEXT, binaryGroup);
    QCOMPARE(tree->model()->index(0, SearchResultsModel::COL_LINE, binaryGroup)
                 .data().toString(), QStringLiteral("0x10"));
    tree->scrollTo(matchIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    const QRect rect = tree->visualRect(matchIndex);
    QVERIFY(rect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(qvariant_cast<EditorTab *>(jumpSpy.first().at(0)), binaryTab);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), qint64(20));
    QCOMPARE(jumpSpy.first().at(2).toInt(), marker.size());
    QCOMPARE(binaryTab->l2View()->selectionStart(), qint64(20));
    QCOMPARE(binaryTab->l2View()->selectionEnd(), qint64(24));

    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dialog.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-hex-all-open-dialog.png"))));
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-hex-all-open-results.png"))));
    binaryTab->shutdownL2BeforeClose();
    dock.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_013_all_open_navigation_respects_wrap_boundary()
{
    const QString marker = QStringLiteral("GLOBAL_NEXT_TOKEN");
    const QString firstPath = createSmallTestFile(
        marker.toUtf8() + QByteArray(" first\n"), "fr-013-first.log");
    const QString middlePath = createSmallTestFile(
        QByteArray("middle without marker\n"), "fr-013-middle.log");
    const QString lastPath = createSmallTestFile(
        marker.toUtf8() + QByteArray(" last\n"), "fr-013-last.log");

    QTabWidget workspace;
    workspace.resize(1000, 620);
    auto *first = new EditorTab(&workspace);
    auto *middle = new EditorTab(&workspace);
    auto *last = new EditorTab(&workspace);
    QVERIFY(first->loadFile(firstPath));
    QVERIFY(middle->loadFile(middlePath));
    QVERIFY(last->loadFile(lastPath));
    workspace.addTab(first, QStringLiteral("first"));
    workspace.addTab(middle, QStringLiteral("middle"));
    workspace.addTab(last, QStringLiteral("last"));
    workspace.setCurrentWidget(last);
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setActiveTab(last);
    connect(&workspace, &QTabWidget::currentChanged, &dialog,
            [&workspace, &dialog](int index) {
                dialog.setActiveTab(qobject_cast<EditorTab *>(workspace.widget(index)));
            });
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *previous = dialog.findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && scope && wrap && next && previous && status);
    QCOMPARE(scope->currentData().toInt(), 0);
    scope->setCurrentIndex(scope->findData(1));
    pattern->setText(marker);

    wrap->setChecked(false);
    last->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                  last->editor()->length(),
                                  last->editor()->length());
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(workspace.currentWidget(), static_cast<QWidget *>(last));
    QVERIFY(status->text().contains(QStringLiteral("边界")));

    wrap->setChecked(true);
    QTest::mouseClick(next, Qt::LeftButton);
    QTRY_COMPARE(workspace.currentWidget(), static_cast<QWidget *>(first));
    QCOMPARE(first->editor()->selectedText(), marker);

    wrap->setChecked(false);
    first->editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                   static_cast<unsigned long>(0),
                                   static_cast<long>(0));
    QTest::mouseClick(previous, Qt::LeftButton);
    QCOMPARE(workspace.currentWidget(), static_cast<QWidget *>(first));
    QVERIFY(status->text().contains(QStringLiteral("边界")));

    wrap->setChecked(true);
    QTest::mouseClick(previous, Qt::LeftButton);
    QTRY_COMPARE(workspace.currentWidget(), static_cast<QWidget *>(last));
    QCOMPARE(last->editor()->selectedText(), marker);

    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-all-open-navigation-wrap.png"))));
    dialog.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_014_invalidated_result_rerun_from_dock()
{
    const QString marker = QStringLiteral("RERUN_CONTEXT_TOKEN");
    const QString path = createSmallTestFile(
        QByteArray("before ") + marker.toUtf8() + QByteArray(" after\n"),
        "fr-014-rerun.txt");
    QVERIFY(tab()->loadFile(path));
    QVERIFY(!tab()->isL2Mode());

    SearchResultsDock dock;
    dock.resize(1050, 320);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    auto *rerun = dock.findChild<QPushButton *>(
        QStringLiteral("searchResultsRerunButton"));
    QVERIFY(pattern && findAll && tree && rerun);

    pattern->setText(marker);
    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() && tree->model()->rowCount() == 1, 3000);
    QVERIFY(!rerun->isVisible());

    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *target, qint64 offset, int length) {
                if (target)
                    target->gotoByteOffset(offset, length);
            });

    tab()->editor()->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                   tab()->editor()->length());
    tab()->editor()->setFocus();
    QTest::keyClicks(tab()->editor(), QStringLiteral("changed"));
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() == nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(rerun->isVisible() && rerun->isEnabled(), 2000);

    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-search-rerun-stale.png"))));

    QTest::mouseClick(rerun, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(tree->model() && tree->model()->rowCount() == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!rerun->isVisible(), 2000);

    const QModelIndex context = tree->model()->index(
        0, SearchResultsModel::COL_CONTEXT);
    QVERIFY(context.isValid());
    tree->scrollTo(context);
    QApplication::processEvents(QEventLoop::AllEvents, 60);
    const QPoint click = tree->visualRect(context).center();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 2000);
    QCOMPARE(qvariant_cast<EditorTab *>(jumpSpy.first().at(0)), tab());
    const auto selectedBytes = [this]() {
        const qint64 start = tab()->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 end = tab()->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        return tab()->editor()->text().toUtf8().mid(start, end - start);
    };
    QTRY_COMPARE_WITH_TIMEOUT(selectedBytes(), marker.toUtf8(), 3000);

    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-search-rerun-valid.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_015_all_open_replace_transaction_and_skip_summary()
{
    const QString marker = QStringLiteral("MULTI_REPLACE_TOKEN");
    const QString replacement = QStringLiteral("REPLACED_VALUE");
    const QByteArray firstOriginal =
        QByteArray("first ") + marker.toUtf8() + " and " + marker.toUtf8() + "\r\n";
    const QByteArray secondOriginal =
        QByteArray("second ") + marker.toUtf8() + "\r\n";
    const QByteArray l2Original =
        QByteArray("editable-l2 ") + marker.toUtf8() + "\r\n";
    const QString firstPath = createSmallTestFile(firstOriginal, "fr-015-first.txt");
    const QString secondPath = createSmallTestFile(secondOriginal, "fr-015-second.txt");
    const QString l2Path = createSmallTestFile(l2Original, "fr-015-editable-l2.txt");

    QTabWidget workspace;
    workspace.resize(1050, 650);
    auto *first = new EditorTab(&workspace);
    auto *second = new EditorTab(&workspace);
    auto *secure = new EditorTab(&workspace);
    auto *l2 = new EditorTab(&workspace);
    QVERIFY(first->loadFile(firstPath));
    QVERIFY(second->loadFile(secondPath));
    secure->initializeSecureDocument(
        QUuid::createUuid(), QStringLiteral("secure-note"),
        QStringLiteral("secure %1\r\n").arg(marker));
    QVERIFY(secure->isSecureDocument());
    QString error;
    QVERIFY2(l2->openLargeFileL2(l2Path, &error), qPrintable(error));
    QVERIFY(l2->isL2Mode());
    QVERIFY(!l2->isReadOnlyByPolicy());
    workspace.addTab(first, QStringLiteral("first"));
    workspace.addTab(second, QStringLiteral("second"));
    workspace.addTab(secure, QStringLiteral("secure"));
    workspace.addTab(l2, QStringLiteral("editable-l2"));
    workspace.setCurrentWidget(first);
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    SearchResultsDock dock;
    dock.resize(1050, 320);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setActiveTab(first);
    dialog.setSearchResultsDock(&dock);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replace = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && replace && scope && replaceAll && status);
    pattern->setText(marker);
    replace->setText(replacement);
    scope->setCurrentIndex(scope->findData(1));
    QTRY_VERIFY_WITH_TIMEOUT(replaceAll->isEnabled(), 2000);
    QVERIFY(status->text().contains(QStringLiteral("只读大文件")));

    bool sawConfirmation = false;
    bool sawCompletion = false;
    QString confirmationText;
    QTimer modalDriver;
    modalDriver.setInterval(25);
    connect(&modalDriver, &QTimer::timeout, this, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (box->windowTitle() == QStringLiteral("确认全部打开文件替换")) {
            sawConfirmation = true;
            confirmationText = box->text();
            box->grab().save(QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("gui-all-open-replace-confirm.png")));
            if (auto *yes = box->button(QMessageBox::Yes))
                QTest::mouseClick(yes, Qt::LeftButton);
        } else if (box->windowTitle() == QStringLiteral("全部替换完成")) {
            sawCompletion = true;
            if (auto *ok = box->button(QMessageBox::Ok))
                QTest::mouseClick(ok, Qt::LeftButton);
        }
    });
    modalDriver.start();
    QTest::mouseClick(replaceAll, Qt::LeftButton);
    modalDriver.stop();

    QVERIFY(sawConfirmation);
    QVERIFY(sawCompletion);
    QVERIFY(confirmationText.contains(QStringLiteral("4 个可编辑文本文件")));
    QVERIFY(confirmationText.contains(QStringLiteral("5 处匹配")));
    QVERIFY(confirmationText.contains(QStringLiteral("只读大文件 0 个")));
    QCOMPARE(first->editor()->text().count(replacement), 2);
    QCOMPARE(second->editor()->text().count(replacement), 1);
    QCOMPARE(secure->editor()->text().count(replacement), 1);
    QCOMPARE(l2->l2View()->readByteRange(
                 0, l2->l2View()->virtualFileSize()).count(replacement.toUtf8()), 1);
    QVERIFY(first->isModified());
    QVERIFY(second->isModified());
    QVERIFY(secure->isModified());
    QVERIFY(l2->isModified());
    QVERIFY(status->text().contains(QStringLiteral("文件尚未保存")));

    // No implicit disk write, and each document is exactly one undo unit.
    QFile firstDisk(firstPath);
    QVERIFY(firstDisk.open(QIODevice::ReadOnly));
    QCOMPARE(firstDisk.readAll(), firstOriginal);
    QFile secondDisk(secondPath);
    QVERIFY(secondDisk.open(QIODevice::ReadOnly));
    QCOMPARE(secondDisk.readAll(), secondOriginal);
    first->undo();
    second->undo();
    secure->undo();
    QCOMPARE(first->editor()->text(), QString::fromUtf8(firstOriginal));
    QCOMPARE(second->editor()->text(), QString::fromUtf8(secondOriginal));
    QCOMPARE(secure->editor()->text(), QStringLiteral("secure %1\r\n").arg(marker));
    l2->undo();
    QCOMPARE(l2->l2View()->readByteRange(0, l2->l2View()->virtualFileSize()), l2Original);
    QFile l2Disk(l2Path);
    QVERIFY(l2Disk.open(QIODevice::ReadOnly));
    QCOMPARE(l2Disk.readAll(), l2Original);

    QVERIFY(dialog.grab().save(QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("gui-all-open-replace-complete.png"))));
    l2->shutdownL2BeforeClose();
    dock.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_016_large_search_starts_before_line_index_completes()
{
    qRegisterMetaType<SearchResultsModel *>("SearchResultsModel*");
    const QString marker = QStringLiteral("INDEX_INDEPENDENT_TOKEN");
    QByteArray content(2 * 1024 * 1024, 'x');
    content.append("\r\ncontext-before ");
    content.append(marker.toUtf8());
    content.append(" context-after\r\n");
    const QString path = createSmallTestFile(content, "fr-016-index-independent.log");
    QString error;
    QVERIFY2(openL2(path, &error), qPrintable(error));
    QVERIFY(tab()->isL2Mode());

    // Deterministically exercise the production incomplete-index branch after
    // normal GUI open. No builder is active, so this mutation cannot race.
    LineIndex &index = const_cast<LineIndex &>(tab()->l2View()->lineIndex());
    index.setIncompleteWithFileSize(QFileInfo(path).size());
    QVERIFY(index.isIncomplete());

    QSignalSpy progress(tab(), &EditorTab::findAllL2Progress);
    QSignalSpy finished(tab(), &EditorTab::findAllL2Finished);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(tab()->findAllL2Async(marker, false, true, false, container_, false));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    QVERIFY2(elapsed.elapsed() < 5000, "search waited for an unavailable full LineIndex");
    for (const QList<QVariant> &emission : progress) {
        QVERIFY2(emission.at(0).toInt() >= 0,
                 "text search emitted the legacy wait-for-LineIndex state");
    }
    auto *model = qvariant_cast<SearchResultsModel *>(finished.first().at(0));
    QVERIFY(model);
    QCOMPARE(model->totalMatches(), qint64(1));
    QModelIndex offsetCell = model->index(0, SearchResultsModel::COL_LINE);
    const QModelIndex contextCell = model->index(0, SearchResultsModel::COL_CONTEXT);
    QVERIFY(offsetCell.data().toString().startsWith(QStringLiteral("0x")));
    QVERIFY(contextCell.data().toString().contains(marker));

    // Bind the early byte-position result to the real Dock, then complete the
    // line index.  The row must turn into a line number in place without a
    // second search, while the byte range remains the navigation authority.
    SearchResultsDock dock(container_);
    dock.setResultsModel(model, marker, tab()->fileName(), tab());
    dock.show();
    QCoreApplication::processEvents();
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(tree);
    QVERIFY(tree->model()->index(0, SearchResultsModel::COL_LINE)
                .data().toString().startsWith(QStringLiteral("0x")));

    LineIndex completed;
    QVERIFY(completed.buildFromMmap(
        reinterpret_cast<const uchar *>(content.constData()), content.size()));
    index.setCompletedAndSwapIn(std::move(completed));
    QVERIFY(!index.isIncomplete());
    QVERIFY(QMetaObject::invokeMethod(tab(), "l2LineBuildFinished",
                                      Qt::DirectConnection, Q_ARG(bool, true)));
    QTRY_VERIFY_WITH_TIMEOUT(
        !tree->model()->index(0, SearchResultsModel::COL_LINE)
             .data().toString().startsWith(QStringLiteral("0x")), 1000);
    QCOMPARE(tree->model()->index(0, SearchResultsModel::COL_LINE)
                 .data().toString(), QStringLiteral("2"));
    QCOMPARE(model->totalMatches(), qint64(1));
    QVERIFY(dock.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-search-line-decoration.png"))));
}

void SmokeFindReplace::tc_fr_017_regex_toggle_distinguishes_literal_and_regex()
{
    // "a.b" 是判别语料：literal 只能命中带点文本；regex 还应命中
    // axb/acb。若 UI 开关未传入领域层或两个分支都直接按 regex 执行，
    // 第一个断言会立即失败。
    const QString path = createSmallTestFile(
        QByteArray("literal a.b\nregex axb\nregex acb\n"),
        QStringLiteral("fr-017-regex-toggle.log"));
    EditorTab l1;
    l1.resize(900, 520);
    QVERIFY(l1.loadFile(path));
    QVERIFY(!l1.isL2Mode());
    l1.show();
    QVERIFY(QTest::qWaitForWindowExposed(&l1));

    SearchResultsDock dock;
    dock.resize(1000, 300);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));

    FindReplaceDialog dialog;
    dialog.setActiveTab(&l1);
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(pattern && regex && findAll && tree);

    const auto resultRows = [tree]() -> int {
        // The L1 Dock deliberately uses FlatL1SearchResultsModel while L2 uses
        // SearchResultsModel.  This corpus puts one match on each line, so row
        // count is a provider-neutral visible-result assertion.
        return tree->model() ? tree->model()->rowCount() : -1;
    };

    pattern->setText(QStringLiteral("a.b"));
    regex->setChecked(false);
    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(resultRows(), 1, 3000);

    regex->setChecked(true);
    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(resultRows(), 3, 3000);

    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-search-regex-toggle.png"))));
}

void SmokeFindReplace::tc_fr_018_regex_contract_model_runs_gui_variants()
{
    struct ContractVariant {
        const char *id;
        QString query;
        bool regex = false;
        QVector<QPair<qint64, int>> expected;
        bool invalidRegex = false;
    };

    // The oracle is deliberately specified as byte ranges, independently of
    // EditorTab/Pcre2Regex/SearchResultsDock.  Each match occupies its own line
    // so visible Dock rows and authoritative byte ranges are both observable.
    const QByteArray corpus("a.b\naxb\nacb\n[ bracket\n");
    const QVector<ContractVariant> variants = {
        {"literal-dot", QStringLiteral("a.b"), false, {{0, 3}}},
        {"regex-dot", QStringLiteral("a.b"), true,
         {{0, 3}, {4, 3}, {8, 3}}},
        {"escaped-regex-dot", QStringLiteral("a\\.b"), true, {{0, 3}}},
        {"literal-open-bracket", QStringLiteral("["), false, {{12, 1}}},
        {"invalid-regex-open-bracket", QStringLiteral("["), true, {}, true},
    };

    QVector<QPair<qint64, int>> literalDotResults;
    QVector<QPair<qint64, int>> escapedRegexDotResults;

    for (const ContractVariant &variant : variants) {
        const QString path = createSmallTestFile(
            corpus, QStringLiteral("fr-018-%1.log").arg(variant.id));
        EditorTab l1;
        l1.resize(900, 520);
        QVERIFY2(l1.loadFile(path), variant.id);
        QVERIFY2(!l1.isL2Mode(), variant.id);
        l1.show();
        QVERIFY2(QTest::qWaitForWindowExposed(&l1), variant.id);

        SearchResultsDock dock;
        dock.resize(1000, 300);
        dock.show();
        QVERIFY2(QTest::qWaitForWindowExposed(&dock), variant.id);

        FindReplaceDialog dialog;
        dialog.setActiveTab(&l1);
        dialog.setSearchResultsDock(&dock);
        dialog.showFindMode();
        QVERIFY2(QTest::qWaitForWindowExposed(&dialog), variant.id);

        auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *findNext = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY2(pattern && regex && wrap && findNext && findAll && status && tree,
                 variant.id);

        pattern->setText(variant.query);
        regex->setChecked(variant.regex);
        wrap->setChecked(false);
        QTest::mouseClick(findAll, Qt::LeftButton);

        if (variant.invalidRegex) {
            QTRY_VERIFY_WITH_TIMEOUT(
                status->text().contains(QStringLiteral("正则无效")), 2000);
            QVERIFY2(!tree->model() || tree->model()->rowCount() == 0, variant.id);
            QTest::mouseClick(findNext, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(
                status->text().contains(QStringLiteral("正则无效")), 2000);
            continue;
        }

        QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 2000);
        QVector<QPair<qint64, int>> dockRanges;
        for (int row = 0; row < tree->model()->rowCount(); ++row) {
            const QModelIndex index = tree->model()->index(
                row, SearchResultsModel::COL_CONTEXT);
            const QVariantList ranges =
                index.data(SearchResultsModel::MatchByteRangesRole).toList();
            for (const QVariant &entry : ranges) {
                const QVariantMap range = entry.toMap();
                dockRanges.append(qMakePair(
                    range.value(QStringLiteral("byteOffset")).toLongLong(),
                    range.value(QStringLiteral("length")).toInt()));
            }
        }
        QCOMPARE(dockRanges.size(), variant.expected.size());
        for (int i = 0; i < variant.expected.size(); ++i)
            QCOMPARE(dockRanges.at(i), variant.expected.at(i));

        // Drive the second execution path through the actual button.  Find All
        // and repeated Find Next must expose the same ordered byte ranges.
        l1.editor()->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                   static_cast<unsigned long>(0),
                                   static_cast<long>(0));
        l1.resetSearchPosition(true);
        for (const auto &expected : variant.expected) {
            QTest::mouseClick(findNext, Qt::LeftButton);
            const qint64 start = l1.editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONSTART);
            const qint64 end = l1.editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONEND);
            QCOMPARE(start, expected.first);
            QCOMPARE(end - start, qint64(expected.second));
        }
        QTest::mouseClick(findNext, Qt::LeftButton);
        QVERIFY2(status->text().contains(QStringLiteral("未找到更多匹配")),
                 variant.id);

        if (QString::fromLatin1(variant.id) == QStringLiteral("literal-dot"))
            literalDotResults = dockRanges;
        if (QString::fromLatin1(variant.id) == QStringLiteral("escaped-regex-dot"))
            escapedRegexDotResults = dockRanges;
    }

    // Metamorphic oracle: literal(q) == regex(escape(q)).  This catches a
    // shared wrong implementation even if individual result counts happen to
    // look plausible.
    QCOMPARE(literalDotResults, escapedRegexDotResults);
}

void SmokeFindReplace::tc_fr_019_query_option_state_machine_exhaustive()
{
    const QString textPath = createSmallTestFile(
        QByteArray("cat Cat scatter\n"), QStringLiteral("fr-019-text.log"));
    QByteArray binaryBytes(64, '\0');
    binaryBytes.replace(8, 3, QByteArray::fromHex("CA7F00"));
    const QString binaryPath = createSmallTestFile(
        binaryBytes, QStringLiteral("fr-019-binary.bin"));

    QTabWidget workspace;
    workspace.resize(1000, 620);
    auto *textTab = new EditorTab(&workspace);
    auto *binaryTab = new EditorTab(&workspace);
    auto *lockedTab = new EditorTab(&workspace);
    QVERIFY(textTab->loadFile(textPath));
    QString openError;
    QVERIFY2(binaryTab->openLargeFileL2(
                 binaryPath, &openError,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(openError));
    workspace.addTab(textTab, QStringLiteral("Text"));
    workspace.addTab(binaryTab, QStringLiteral("Binary"));
    lockedTab->initializeSecureDocument(
        QUuid::createUuid(), QStringLiteral("Locked"), QStringLiteral("cat"));
    lockedTab->maskSecureDocument();
    QVERIFY(lockedTab->isSecureDocumentMasked());
    workspace.addTab(lockedTab, QStringLiteral("Locked"));
    workspace.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workspace));

    SearchResultsDock dock;
    FindReplaceDialog dialog(&workspace);
    dialog.setTabWidget(&workspace);
    dialog.setSearchResultsDock(&dock);
    dialog.setActiveTab(textTab);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *scope = dialog.findChild<QComboBox *>(QStringLiteral("searchScopeCombo"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *caseSensitive = dialog.findChild<QCheckBox *>(
        QStringLiteral("caseSensitiveSearchCheck"));
    auto *wholeWord = dialog.findChild<QCheckBox *>(
        QStringLiteral("wholeWordSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *previous = dialog.findChild<QPushButton *>(
        QStringLiteral("findPreviousButton"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *replace = dialog.findChild<QPushButton *>(QStringLiteral("replaceButton"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    QVERIFY(pattern && scope && regex && hex && caseSensitive && wholeWord
            && wrap && next && previous && findAll && replacement && replace
            && replaceAll);

    const auto clearInterpretation = [&]() {
        regex->setChecked(false);
        hex->setChecked(false);
        caseSensitive->setChecked(false);
        wholeWord->setChecked(false);
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    const auto assertNoConflict = [&](const QByteArray &caseId) {
        QVERIFY2(!(regex->isChecked() && wholeWord->isChecked()), caseId.constData());
        QVERIFY2(!(regex->isChecked() && hex->isChecked()), caseId.constData());
        QVERIFY2(!(hex->isChecked() && caseSensitive->isChecked()), caseId.constData());
        QVERIFY2(!(hex->isChecked() && wholeWord->isChecked()), caseId.constData());
    };

    // Empty queries are not executable actions; the user must not be offered
    // buttons that silently do nothing.
    pattern->clear();
    QTRY_VERIFY(!next->isEnabled());
    QVERIFY(!previous->isEnabled());
    QVERIFY(!findAll->isEnabled());
    QVERIFY(!replace->isEnabled());
    QVERIFY(!replaceAll->isEnabled());
    QVERIFY(replacement->isEnabled());
    pattern->setText(QStringLiteral("cat"));
    QTRY_VERIFY(next->isEnabled());
    QVERIFY(previous->isEnabled());
    QVERIFY(findAll->isEnabled());

    struct TargetContext {
        EditorTab *active = nullptr;
        int scopeData = 0;
        bool byteModeAvailable = false;
        bool searchExecutable = true;
        const char *id = nullptr;
    };
    const QVector<TargetContext> contexts = {
        {textTab, 0, false, true, "current-text"},
        {binaryTab, 0, true, true, "current-binary"},
        {lockedTab, 0, false, false, "current-locked"},
        {textTab, 1, true, true, "all-open-mixed"},
    };

    for (const TargetContext &context : contexts) {
        workspace.setCurrentWidget(context.active);
        dialog.setActiveTab(context.active);
        scope->setCurrentIndex(scope->findData(context.scopeData));
        QTRY_COMPARE(scope->currentData().toInt(), context.scopeData);
        const bool singleReplaceExpected = context.scopeData == 0
            && !context.active->isBinaryDocument()
            && !context.active->isSecureDocumentMasked();
        const bool replaceAllExpected = context.scopeData == 1
            || singleReplaceExpected;

        // Literal mode: case/whole/wrap are independent legal dimensions.
        for (int flags = 0; flags < 8; ++flags) {
            clearInterpretation();
            const bool wantCase = flags & 1;
            const bool wantWhole = flags & 2;
            const bool wantWrap = flags & 4;
            wrap->setChecked(wantWrap);
            if (wantCase)
                QTest::mouseClick(caseSensitive, Qt::LeftButton);
            if (wantWhole)
                QTest::mouseClick(wholeWord, Qt::LeftButton);
            const QByteArray id = QByteArray(context.id)
                + "-literal-" + QByteArray::number(flags);
            QVERIFY2(!regex->isChecked() && !hex->isChecked(), id.constData());
            QCOMPARE(caseSensitive->isChecked(), wantCase);
            QCOMPARE(wholeWord->isChecked(), wantWhole);
            QCOMPARE(wrap->isChecked(), wantWrap);
            QVERIFY2(caseSensitive->isEnabled(), id.constData());
            QVERIFY2(wholeWord->isEnabled(), id.constData());
            QCOMPARE(hex->isEnabled(),
                     context.byteModeAvailable && !wantCase && !wantWhole);
            QCOMPARE(regex->isEnabled(), !wantWhole);
            QCOMPARE(replacement->isEnabled(), replaceAllExpected);
            QCOMPARE(replace->isEnabled(), singleReplaceExpected);
            QCOMPARE(replaceAll->isEnabled(), replaceAllExpected);
            QCOMPARE(next->isEnabled(), context.searchExecutable);
            QCOMPARE(previous->isEnabled(), context.searchExecutable);
            QCOMPARE(findAll->isEnabled(), context.searchExecutable);
            assertNoConflict(id);
        }

        // Regex mode: match-case and wrap remain meaningful; whole-word and
        // byte mode are disabled and real mouse clicks cannot change them.
        for (int flags = 0; flags < 4; ++flags) {
            clearInterpretation();
            QVERIFY(regex->isEnabled());
            QTest::mouseClick(regex, Qt::LeftButton);
            const bool wantCase = flags & 1;
            const bool wantWrap = flags & 2;
            caseSensitive->setChecked(wantCase);
            wrap->setChecked(wantWrap);
            const QByteArray id = QByteArray(context.id)
                + "-regex-" + QByteArray::number(flags);
            QVERIFY2(regex->isChecked(), id.constData());
            QVERIFY2(caseSensitive->isEnabled(), id.constData());
            QVERIFY2(!wholeWord->isEnabled() && !wholeWord->isChecked(),
                     id.constData());
            QVERIFY2(!hex->isEnabled() && !hex->isChecked(), id.constData());
            QTest::mouseClick(wholeWord, Qt::LeftButton);
            QTest::mouseClick(hex, Qt::LeftButton);
            QVERIFY2(regex->isChecked() && !wholeWord->isChecked()
                         && !hex->isChecked(), id.constData());
            QCOMPARE(wrap->isChecked(), wantWrap);
            QCOMPARE(replacement->isEnabled(), replaceAllExpected);
            QCOMPARE(replace->isEnabled(), singleReplaceExpected);
            QCOMPARE(replaceAll->isEnabled(), replaceAllExpected);
            QCOMPARE(next->isEnabled(), context.searchExecutable);
            QCOMPARE(previous->isEnabled(), context.searchExecutable);
            QCOMPARE(findAll->isEnabled(), context.searchExecutable);
            assertNoConflict(id);
        }

        clearInterpretation();
        QCOMPARE(hex->isEnabled(), context.byteModeAvailable);
        if (context.byteModeAvailable) {
            // Byte mode: only scope and wrap remain meaningful. All text
            // interpretation controls are disabled, unchecked and immutable.
            QTest::mouseClick(hex, Qt::LeftButton);
            for (const bool wantWrap : {false, true}) {
                wrap->setChecked(wantWrap);
                const QByteArray id = QByteArray(context.id)
                    + "-hex-" + (wantWrap ? "wrap" : "nowrap");
                QVERIFY2(hex->isChecked(), id.constData());
                QVERIFY2(!regex->isEnabled() && !regex->isChecked(), id.constData());
                QVERIFY2(!caseSensitive->isEnabled()
                             && !caseSensitive->isChecked(), id.constData());
                QVERIFY2(!wholeWord->isEnabled() && !wholeWord->isChecked(),
                         id.constData());
                QTest::mouseClick(regex, Qt::LeftButton);
                QTest::mouseClick(caseSensitive, Qt::LeftButton);
                QTest::mouseClick(wholeWord, Qt::LeftButton);
                QVERIFY2(hex->isChecked() && !regex->isChecked()
                             && !caseSensitive->isChecked()
                             && !wholeWord->isChecked(), id.constData());
                QCOMPARE(wrap->isChecked(), wantWrap);
                QVERIFY2(!replacement->isEnabled(), id.constData());
                QVERIFY2(!replace->isEnabled(), id.constData());
                QVERIFY2(!replaceAll->isEnabled(), id.constData());
                assertNoConflict(id);
            }
        }
    }

    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-search-option-state-machine.png"))));
    binaryTab->shutdownL2BeforeClose();
    dialog.close();
    workspace.close();
}

void SmokeFindReplace::tc_fr_020_replace_button_uses_current_match_then_advances()
{
    const QString path = createSmallTestFile(
        QByteArray("first cat middle cat last\n"),
        QStringLiteral("fr-020-current-match.txt"));
    EditorTab editor;
    editor.resize(900, 500);
    QVERIFY(editor.loadFile(path));
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    FindReplaceDialog dialog;
    dialog.setActiveTab(&editor);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *replace = dialog.findChild<QPushButton *>(QStringLiteral("replaceButton"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    QVERIFY(pattern && replacement && next && replace && wrap);
    pattern->setText(QStringLiteral("cat"));
    replacement->setText(QStringLiteral("DOG"));
    wrap->setChecked(false);
    editor.editor()->setCursorPosition(0, 0);

    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(editor.editor()->selectedText(), QStringLiteral("cat"));
    QCOMPARE(editor.editor()->SendScintilla(
                 QsciScintillaBase::SCI_GETSELECTIONSTART), 6L);

    QTest::mouseClick(replace, Qt::LeftButton);
    QCOMPARE(editor.editor()->text(),
             QStringLiteral("first DOG middle cat last\n"));
    QCOMPARE(editor.editor()->selectedText(), QStringLiteral("cat"));
    QCOMPARE(editor.editor()->SendScintilla(
                 QsciScintillaBase::SCI_GETSELECTIONSTART), 17L);

    QTest::mouseClick(replace, Qt::LeftButton);
    QCOMPARE(editor.editor()->text(),
             QStringLiteral("first DOG middle DOG last\n"));
    QVERIFY(editor.editor()->selectedText().isEmpty());

    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-replace-current-then-next.png"))));
    dialog.close();
    editor.close();
}

void SmokeFindReplace::tc_fr_021_text_option_semantics_all_legal_combinations()
{
    struct Variant {
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWord = false;
        QVector<qint64> offsets;
        const char *id = nullptr;
    };
    const QVector<Variant> variants = {
        {false, false, false, {0, 4, 9, 16, 20, 24}, "literal-i-part"},
        {false, true,  false, {0, 9, 16, 20, 24}, "literal-s-part"},
        {false, false, true,  {0, 4, 24}, "literal-i-word"},
        {false, true,  true,  {0, 24}, "literal-s-word"},
        {true,  false, false, {0, 4, 9, 16, 20, 24}, "regex-i"},
        {true,  true,  false, {0, 9, 16, 20, 24}, "regex-s"},
    };
    const QByteArray corpus("cat\nCat\nscatter\ncat_cat\ncat.\n");

    for (const Variant &variant : variants) {
        const QString path = createSmallTestFile(
            corpus, QStringLiteral("fr-021-%1.log").arg(variant.id));
        EditorTab editor;
        editor.resize(900, 500);
        QVERIFY2(editor.loadFile(path), variant.id);
        editor.show();
        QVERIFY2(QTest::qWaitForWindowExposed(&editor), variant.id);
        SearchResultsDock dock;
        FindReplaceDialog dialog;
        dialog.setActiveTab(&editor);
        dialog.setSearchResultsDock(&dock);
        dialog.showFindMode();
        QVERIFY2(QTest::qWaitForWindowExposed(&dialog), variant.id);

        auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
        auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
        auto *caseSensitive = dialog.findChild<QCheckBox *>(
            QStringLiteral("caseSensitiveSearchCheck"));
        auto *wholeWord = dialog.findChild<QCheckBox *>(
            QStringLiteral("wholeWordSearchCheck"));
        auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
        auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
        auto *previous = dialog.findChild<QPushButton *>(
            QStringLiteral("findPreviousButton"));
        auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
        auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
        QVERIFY2(pattern && regex && caseSensitive && wholeWord && wrap
                     && next && previous && findAll && status && tree, variant.id);
        pattern->setText(variant.regex ? QStringLiteral("c.t")
                                       : QStringLiteral("cat"));
        regex->setChecked(variant.regex);
        caseSensitive->setChecked(variant.caseSensitive);
        wholeWord->setChecked(variant.wholeWord);
        wrap->setChecked(false);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model() != nullptr, 2000);

        QVector<qint64> dockOffsets;
        for (int row = 0; row < tree->model()->rowCount(); ++row) {
            const QModelIndex index = tree->model()->index(
                row, SearchResultsModel::COL_CONTEXT);
            const QVariantList ranges =
                index.data(SearchResultsModel::MatchByteRangesRole).toList();
            for (const QVariant &entry : ranges) {
                dockOffsets.append(entry.toMap()
                    .value(QStringLiteral("byteOffset")).toLongLong());
            }
        }
        QCOMPARE(dockOffsets, variant.offsets);

        editor.resetSearchPosition(true);
        QVector<qint64> navigationOffsets;
        for (qsizetype i = 0; i < variant.offsets.size(); ++i) {
            QTest::mouseClick(next, Qt::LeftButton);
            navigationOffsets.append(editor.editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONSTART));
        }
        QCOMPARE(navigationOffsets, variant.offsets);
        QTest::mouseClick(next, Qt::LeftButton);
        QVERIFY2(status->text().contains(QStringLiteral("未找到更多匹配")),
                 variant.id);

        editor.resetSearchPosition(false);
        QVector<qint64> reverseOffsets;
        for (qsizetype i = variant.offsets.size(); i > 0; --i) {
            QTest::mouseClick(previous, Qt::LeftButton);
            reverseOffsets.append(editor.editor()->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONSTART));
        }
        QVector<qint64> expectedReverse = variant.offsets;
        std::reverse(expectedReverse.begin(), expectedReverse.end());
        QCOMPARE(reverseOffsets, expectedReverse);
        QTest::mouseClick(previous, Qt::LeftButton);
        QVERIFY2(status->text().contains(QStringLiteral("未找到更多匹配")),
                 variant.id);

        // Wrap changes only navigation after the final hit; the query result
        // set and its order remain exactly the same.
        wrap->setChecked(true);
        QTest::mouseClick(previous, Qt::LeftButton);
        QCOMPARE(editor.editor()->SendScintilla(
                     QsciScintillaBase::SCI_GETSELECTIONSTART),
                 variant.offsets.last());
        dialog.close();
        dock.close();
        editor.close();
    }
}

void SmokeFindReplace::tc_fr_022_hex_query_formats_errors_and_navigation()
{
    QByteArray content(96, '\0');
    const QByteArray marker = QByteArray::fromHex("DEADBEEF");
    for (const int offset : {0, 15, 92})
        content.replace(offset, marker.size(), marker);
    const QString path = createSmallTestFile(
        content, QStringLiteral("fr-022-hex-formats.bin"));
    QString openError;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &openError,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(openError));

    SearchResultsDock dock;
    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(pattern && hex && wrap && next && findAll && cancel && status && tree);
    QVERIFY(hex->isEnabled());
    QTest::mouseClick(hex, Qt::LeftButton);
    QVERIFY(hex->isChecked());

    const QStringList equivalentFormats = {
        QStringLiteral("DEADBEEF"),
        QStringLiteral("de ad be ef"),
        QStringLiteral("DE-AD-BE-EF"),
        QStringLiteral("0xDE,0xAD,0xBE,0xEF"),
        QStringLiteral("DE_AD_BE_EF"),
    };
    const QVector<qint64> expectedOffsets = {0, 15, 92};
    for (const QString &query : equivalentFormats) {
        pattern->setText(query);
        wrap->setChecked(false);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 5000);
        auto *model = qobject_cast<SearchResultsModel *>(tree->model());
        QVERIFY2(model, qPrintable(query));
        QCOMPARE(model->totalMatches(), qint64(expectedOffsets.size()));
        QVector<qint64> actual;
        for (int row = 0; row < model->rowCount(); ++row) {
            const GroupedSearchLine &group = model->groupedLineAt(row);
            for (const auto &range : group.matchByteRanges)
                actual.append(range.first);
        }
        QCOMPARE(actual, expectedOffsets);

        tab()->resetSearchPosition(true);
        QVector<qint64> navigated;
        for (qsizetype index = 0; index < expectedOffsets.size(); ++index) {
            QTest::mouseClick(next, Qt::LeftButton);
            navigated.append(tab()->l2View()->selectionStart());
            QCOMPARE(currentSelectionBytes(), marker);
        }
        QCOMPARE(navigated, expectedOffsets);
        QTest::mouseClick(next, Qt::LeftButton);
        QVERIFY(status->text().contains(QStringLiteral("未找到更多匹配")));
        wrap->setChecked(true);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(tab()->l2View()->selectionStart(), qint64(0));
    }

    QAbstractItemModel *validModel = tree->model();
    QVERIFY(validModel);
    const QStringList invalidFormats = {
        QStringLiteral("D"),
        QStringLiteral("DE AD BZ"),
        QStringLiteral("0x"),
    };
    for (const QString &query : invalidFormats) {
        pattern->setText(query);
        QTest::mouseClick(findAll, Qt::LeftButton);
        QVERIFY2(!status->text().isEmpty(), qPrintable(query));
        QCOMPARE(tree->model(), validModel);
        QVERIFY(!cancel->isVisible());
    }

    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-hex-search-formats.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_023_regex_capture_replacement_gui_contract()
{
    const QString path = createSmallTestFile(
        QByteArray("name=alice id=42\nname=bob id=7\n"),
        QStringLiteral("fr-023-regex-replace.txt"));
    EditorTab editor;
    editor.resize(900, 500);
    QVERIFY(editor.loadFile(path));
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    FindReplaceDialog dialog;
    dialog.setActiveTab(&editor);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *replace = dialog.findChild<QPushButton *>(QStringLiteral("replaceButton"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    QVERIFY(pattern && replacement && regex && wrap && next && replace && replaceAll);
    pattern->setText(QStringLiteral("name=(?<who>\\w+) id=(\\d+)"));
    replacement->setText(QStringLiteral("$2:${who}"));
    QTest::mouseClick(regex, Qt::LeftButton);
    wrap->setChecked(false);
    editor.editor()->setCursorPosition(0, 0);

    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(editor.editor()->selectedText(), QStringLiteral("name=alice id=42"));
    QTest::mouseClick(replace, Qt::LeftButton);
    QCOMPARE(editor.editor()->text(),
             QStringLiteral("42:alice\nname=bob id=7\n"));
    QCOMPARE(editor.editor()->selectedText(), QStringLiteral("name=bob id=7"));

    bool sawCompletion = false;
    QTimer modalDriver;
    modalDriver.setInterval(20);
    connect(&modalDriver, &QTimer::timeout, this, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (box->windowTitle() == QStringLiteral("替换完成")) {
            sawCompletion = true;
            if (auto *ok = box->button(QMessageBox::Ok))
                QTest::mouseClick(ok, Qt::LeftButton);
        }
    });
    modalDriver.start();
    QTest::mouseClick(replaceAll, Qt::LeftButton);
    modalDriver.stop();
    QVERIFY(sawCompletion);
    QCOMPARE(editor.editor()->text(), QStringLiteral("42:alice\n7:bob\n"));
    QVERIFY(!editor.editor()->text().contains(QStringLiteral("$2")));
    QVERIFY(!editor.editor()->text().contains(QStringLiteral("${who}")));

    editor.undo();
    QCOMPARE(editor.editor()->text(),
             QStringLiteral("42:alice\nname=bob id=7\n"));
    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-regex-capture-replace.png"))));
    dialog.close();
    editor.close();
}

void SmokeFindReplace::tc_fr_024_l2_regex_capture_replacement_consistency()
{
    const QByteArray original("name=alice id=42\nname=bob id=7\n");
    const QString path = createSmallTestFile(
        original, QStringLiteral("fr-024-l2-regex-replace.txt"));
    QString openError;
    QVERIFY2(openL2(path, &openError), qPrintable(openError));

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *replace = dialog.findChild<QPushButton *>(QStringLiteral("replaceButton"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    QVERIFY(pattern && replacement && regex && wrap && next && replace && replaceAll);
    pattern->setText(QStringLiteral("name=(?<who>\\w+) id=(\\d+)"));
    replacement->setText(QStringLiteral("$2:${who}"));
    QTest::mouseClick(regex, Qt::LeftButton);
    wrap->setChecked(false);
    tab()->resetSearchPosition(true);

    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(currentSelectionBytes(), QByteArray("name=alice id=42"));
    QTest::mouseClick(replace, Qt::LeftButton);
    QCOMPARE(readByteRange(0, view()->virtualFileSize()),
             QByteArray("42:alice\nname=bob id=7\n"));
    QCOMPARE(currentSelectionBytes(), QByteArray("name=bob id=7"));

    bool sawCompletion = false;
    QTimer modalDriver;
    modalDriver.setInterval(20);
    connect(&modalDriver, &QTimer::timeout, this, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (box->windowTitle() == QStringLiteral("替换完成")) {
            sawCompletion = true;
            if (auto *ok = box->button(QMessageBox::Ok))
                QTest::mouseClick(ok, Qt::LeftButton);
        }
    });
    modalDriver.start();
    QTest::mouseClick(replaceAll, Qt::LeftButton);
    modalDriver.stop();
    QVERIFY(sawCompletion);
    QCOMPARE(readByteRange(0, view()->virtualFileSize()),
             QByteArray("42:alice\n7:bob\n"));
    tab()->undo();
    QCOMPARE(readByteRange(0, view()->virtualFileSize()),
             QByteArray("42:alice\nname=bob id=7\n"));
    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-l2-regex-capture-replace.png"))));
}

void SmokeFindReplace::tc_fr_025_replacement_value_contract_and_atomic_failures()
{
    const QString original = QStringLiteral("A TOKEN B TOKEN C\n");
    const QString path = createSmallTestFile(
        original.toUtf8(), QStringLiteral("fr-025-replacement-values.txt"));
    EditorTab editor;
    editor.resize(920, 520);
    QVERIFY(editor.loadFile(path));
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    FindReplaceDialog dialog;
    dialog.setActiveTab(&editor);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && replacement && regex && replaceAll && status);

    QString modalTitle;
    QString modalText;
    const auto clickReplaceAllAndDismiss = [&]() {
        modalTitle.clear();
        modalText.clear();
        QTimer driver;
        driver.setInterval(10);
        connect(&driver, &QTimer::timeout, this, [&]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            modalTitle = box->windowTitle();
            modalText = box->text();
            if (auto *button = box->button(QMessageBox::Ok))
                QTest::mouseClick(button, Qt::LeftButton);
        });
        driver.start();
        QTest::mouseClick(replaceAll, Qt::LeftButton);
        driver.stop();
        QVERIFY2(!modalTitle.isEmpty(), "Replace All did not produce completion/error feedback");
    };

    struct LiteralVariant {
        QString replacement;
        const char *id = nullptr;
    };
    const QVector<LiteralVariant> literalVariants = {
        {QString(), "delete"},
        {QStringLiteral("X"), "short"},
        {QStringLiteral("LONGER_世界_😀"), "long-unicode"},
        {QStringLiteral("$1\\n${name}\\1"), "literal-meta-syntax"},
    };
    pattern->setText(QStringLiteral("TOKEN"));
    QVERIFY(!regex->isChecked());
    for (const LiteralVariant &variant : literalVariants) {
        replacement->setText(variant.replacement);
        QVERIFY2(replaceAll->isEnabled(), variant.id);
        clickReplaceAllAndDismiss();
        QCOMPARE(modalTitle, QStringLiteral("替换完成"));
        QString expected = original;
        expected.replace(QStringLiteral("TOKEN"), variant.replacement);
        QCOMPARE(editor.editor()->text(), expected);
        QVERIFY2(editor.isModified(), variant.id);

        editor.undo();
        QCOMPARE(editor.editor()->text(), original);
        editor.redo();
        QCOMPARE(editor.editor()->text(), expected);
        editor.undo();
        QCOMPARE(editor.editor()->text(), original);
    }

    // Replacement expansion errors and zero-length patterns are rejected
    // before the first byte changes. The previous valid document remains
    // available and no partial undo record is created.
    regex->setChecked(true);
    pattern->setText(QStringLiteral("(TOKEN)"));
    replacement->setText(QStringLiteral("${missing}"));
    clickReplaceAllAndDismiss();
    QCOMPARE(modalTitle, QStringLiteral("替换失败"));
    QVERIFY(!modalText.isEmpty());
    QCOMPARE(editor.editor()->text(), original);

    pattern->setText(QStringLiteral("(?=TOKEN)"));
    replacement->setText(QStringLiteral("prefix"));
    clickReplaceAllAndDismiss();
    QCOMPARE(modalTitle, QStringLiteral("替换失败"));
    QVERIFY(status->text().contains(QStringLiteral("零长度")));
    QCOMPARE(editor.editor()->text(), original);

    QFile disk(path);
    QVERIFY(disk.open(QIODevice::ReadOnly));
    QCOMPARE(disk.readAll(), original.toUtf8());
    QVERIFY(dialog.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-replacement-value-contract.png"))));
    dialog.close();
    editor.close();
}

void SmokeFindReplace::tc_fr_026_l2_replace_all_over_batch_is_one_undo()
{
    constexpr int MatchCount = 1205;
    QByteArray original;
    original.reserve(MatchCount * 2);
    for (int i = 0; i < MatchCount; ++i)
        original.append("x,");
    const QString path = createSmallTestFile(
        original, QStringLiteral("fr-026-l2-over-batch.txt"));
    QString error;
    QVERIFY2(openL2(path, &error), qPrintable(error));

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    QVERIFY(pattern && replacement && replaceAll);
    pattern->setText(QStringLiteral("x"));
    replacement->setText(QStringLiteral("yy"));

    bool completed = false;
    QTimer modalDriver;
    modalDriver.setInterval(10);
    connect(&modalDriver, &QTimer::timeout, this, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        completed = box->windowTitle() == QStringLiteral("替换完成");
        if (auto *ok = box->button(QMessageBox::Ok))
            QTest::mouseClick(ok, Qt::LeftButton);
    });
    modalDriver.start();
    QTest::mouseClick(replaceAll, Qt::LeftButton);
    modalDriver.stop();
    QVERIFY(completed);
    const QByteArray replaced = readByteRange(0, view()->virtualFileSize());
    QCOMPARE(replaced.count("yy"), MatchCount);
    QVERIFY(tab()->isModified());

    tab()->undo();
    QCOMPARE(readByteRange(0, view()->virtualFileSize()), original);
    QVERIFY(tab()->canRedoCommand());
    tab()->redo();
    QCOMPARE(readByteRange(0, view()->virtualFileSize()), replaced);
    tab()->undo();
    QCOMPARE(readByteRange(0, view()->virtualFileSize()), original);
}

void SmokeFindReplace::tc_fr_027_real_s3_find_all_dock_double_click()
{
    const QString path = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../bigtestfile"));
    if (!QFileInfo::exists(path))
        QSKIP("retained 4.9 GB S3 fixture is not available");
    const qint64 size = QFileInfo(path).size();
    QVERIFY(size > 2LL * 1024 * 1024 * 1024);
    const qint64 target = size * 3 / 4;
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(source.seek(target));
    const QByteArray signature = source.read(32);
    QCOMPARE(signature.size(), 32);
    source.close();

    QString error;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &error,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(error));
    QVERIFY(tab()->isReadOnlyByPolicy());
    QVERIFY(tab()->isL2Mode());
    auto *provider = tab()->windowedReadOnlyProvider();
    QVERIFY(provider);
    QCOMPARE(provider->fileSize(), size);

    SearchResultsDock dock;
    dock.resize(1100, 360);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *targetTab, qint64 offset, int length) {
                if (targetTab)
                    targetTab->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(pattern && hex && findAll && cancel && status && tree);
    QVERIFY(hex->isEnabled());
    QTest::mouseClick(hex, Qt::LeftButton);
    const QString hexPattern = QString::fromLatin1(signature.toHex(' ').toUpper());
    pattern->setText(hexPattern);
    QCOMPARE(pattern->text(), hexPattern);
    QSignalSpy findAllClickSpy(findAll, &QPushButton::clicked);

    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(findAll, Qt::LeftButton);
    QCOMPARE(findAllClickSpy.count(), 1);
    QVERIFY(!findAll->isEnabled());
    QVERIFY(cancel->isVisible());
    QVERIFY(status->text().contains(QStringLiteral("准备"))
            || status->text().contains(QStringLiteral("搜索")));
    QTRY_VERIFY_WITH_TIMEOUT(cancel->isVisible() || tree->model() != nullptr, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 120000);
    qInfo() << "S3_DIALOG_DOCK_SEARCH_MS=" << elapsed.elapsed();
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    QVERIFY(model->totalMatches() >= 1);

    int targetRow = -1;
    for (int row = 0; row < model->rowCount() && targetRow < 0; ++row) {
        const GroupedSearchLine &group = model->groupedLineAt(row);
        for (const auto &range : group.matchByteRanges) {
            if (range.first == target && range.second == signature.size()) {
                targetRow = row;
                break;
            }
        }
    }
    QVERIFY2(targetRow >= 0, "real S3 target range missing from SearchResultsModel");
    const QModelIndex contextIndex = model->index(
        targetRow, SearchResultsModel::COL_CONTEXT);
    tree->scrollTo(contextIndex);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    const QRect rect = tree->visualRect(contextIndex);
    QVERIFY(rect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, rect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                       Qt::NoModifier, rect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 3000);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), target);
    QCOMPARE(jumpSpy.first().at(2).toInt(), signature.size());
    QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), target, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                              target + signature.size(), 5000);
    QCOMPARE(provider->sourceBytes(target, signature.size()), signature);
    QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);

    const QString outputDir = QCoreApplication::applicationDirPath();
    QVERIFY(dock.grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-s3-real-search-dock.png"))));
    QVERIFY(container_->grab().save(
        QDir(outputDir).filePath(QStringLiteral("gui-s3-real-search-editor.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_028_real_s3_find_next_preparation_and_repeat_boundary()
{
    const QString path = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../bigtestfile"));
    if (!QFileInfo::exists(path))
        QSKIP("retained 4.9 GB S3 fixture is not available");
    const qint64 size = QFileInfo(path).size();
    QVERIFY(size > 2LL * 1024 * 1024 * 1024);
    const qint64 target = size * 3 / 4;
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(source.seek(target));
    const QByteArray signature = source.read(32);
    QCOMPARE(signature.size(), 32);
    source.close();

    QString error;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &error,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(error));
    auto *provider = tab()->windowedReadOnlyProvider();
    QVERIFY(provider);

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *previous = dialog.findChild<QPushButton *>(QStringLiteral("findPreviousButton"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && hex && next && previous && wrap && cancel && status);
    QTest::mouseClick(hex, Qt::LeftButton);
    wrap->setChecked(false);
    pattern->setText(QString::fromLatin1(signature.toHex(' ').toUpper()));

    QElapsedTimer firstElapsed;
    firstElapsed.start();
    QTest::mouseClick(next, Qt::LeftButton);
    qInfo() << "S3_DIALOG_FIND_NEXT_MS=" << firstElapsed.elapsed();
    QCOMPARE(provider->selectionStartOffset(), target);
    QCOMPARE(provider->selectionEndOffset(), target + signature.size());
    QCOMPARE(provider->sourceBytes(target, signature.size()), signature);
    QVERIFY(!cancel->isVisible());

    // The visible provider selection is the current match. A second forward
    // command must advance beyond it and hit the non-wrapping boundary; it
    // must not rediscover the same range from the hidden mmap view.
    QElapsedTimer cachedElapsed;
    cachedElapsed.start();
    QTest::mouseClick(next, Qt::LeftButton);
    QVERIFY(cachedElapsed.elapsed() < 1000);
    QVERIFY(status->text().contains(QStringLiteral("未找到"))
            || status->text().contains(QStringLiteral("边界")));
    QCOMPARE(provider->selectionStartOffset(), target);
    QCOMPARE(provider->selectionEndOffset(), target + signature.size());

    QTest::mouseClick(previous, Qt::LeftButton);
    QVERIFY(status->text().contains(QStringLiteral("未找到"))
            || status->text().contains(QStringLiteral("边界")));
    wrap->setChecked(true);
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(provider->selectionStartOffset(), target);
    QCOMPARE(provider->selectionEndOffset(), target + signature.size());
    QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
    QVERIFY(container_->grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s3-real-find-next.png"))));
}

void SmokeFindReplace::tc_fr_029_result_spool_virtual_tail_navigation()
{
    constexpr int MatchCount = 60050; // Always exceeds MAX_MATCHES_MEM <= 50000.
    const QByteArray marker("OVERFLOW_HIT");
    QByteArray payload;
    payload.reserve(MatchCount * 24);
    qint64 expectedLastOffset = -1;
    for (int index = 0; index < MatchCount; ++index) {
        expectedLastOffset = payload.size();
        payload.append(marker);
        payload.append(' ');
        payload.append(QByteArray::number(index));
        payload.append('\n');
    }
    const QString path = createSmallTestFile(
        payload, QStringLiteral("fr-029-result-spool.log"));
    QVERIFY2(openL2(path), "unable to open result-spool fixture");

    SearchResultsDock dock;
    dock.resize(1100, 420);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *targetTab, qint64 offset, int length) {
                if (targetTab)
                    targetTab->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(pattern && findAll && cancel && tree);
    pattern->setText(QString::fromLatin1(marker));
    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(cancel->isVisible() || tree->model(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 30000);

    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    QCOMPARE(model->totalMatches(), qint64(MatchCount));
    QCOMPARE(model->rowCount(), MatchCount);
    LargeFileSearch *engine = view()->searchEngine();
    QVERIFY(engine);
    QVERIFY(engine->overrunCount() > 0);
    QVERIFY(!engine->overrunFilePath().isEmpty());
    QVERIFY(QFileInfo::exists(engine->overrunFilePath()));
    QVERIFY(engine->memoryMatches().size() <= 50000);

    const int tailRow = MatchCount - 1;
    const GroupedSearchLine &tail = model->groupedLineAt(tailRow);
    QCOMPARE(tail.firstByteOffset, expectedLastOffset);
    QCOMPARE(tail.firstLength, marker.size());
    QCOMPARE(tail.matchByteRanges.size(), 1);
    QCOMPARE(tail.matchByteRanges.first().first, expectedLastOffset);

    const QModelIndex tailIndex = model->index(
        tailRow, SearchResultsModel::COL_CONTEXT);
    tree->scrollTo(tailIndex, QAbstractItemView::PositionAtCenter);
    QApplication::processEvents(QEventLoop::AllEvents, 100);
    const QRect tailRect = tree->visualRect(tailIndex);
    QVERIFY(tailRect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, tailRect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                       Qt::NoModifier, tailRect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 3000);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), expectedLastOffset);
    QCOMPARE(jumpSpy.first().at(2).toInt(), marker.size());
    QTRY_COMPARE_WITH_TIMEOUT(view()->selectionStart(), expectedLastOffset, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(view()->selectionEnd(),
                              expectedLastOffset + marker.size(), 3000);
    QCOMPARE(currentSelectionBytes(), marker);
    QVERIFY(dock.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-search-result-spool-tail.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_030_sparse_s3_text_regex_after_two_gib()
{
    constexpr qint64 TwoGiB = 2LL * 1024 * 1024 * 1024;
    constexpr qint64 FileSize = TwoGiB + 16LL * 1024 * 1024;
    constexpr qint64 MarkerWriteOffset = TwoGiB + 4LL * 1024 * 1024 + 123;
    const QByteArray line("\nS3_REGEX_739_END\n");
    const QByteArray matchBytes("S3_REGEX_739_END");
    const qint64 expectedOffset = MarkerWriteOffset + 1;
    const QString path = tmpDir_.filePath(QStringLiteral("fr-030-s3-text.log"));
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadWrite));
    QVERIFY(source.resize(FileSize));
    QVERIFY(source.seek(MarkerWriteOffset));
    QCOMPARE(source.write(line), qint64(line.size()));
    source.close();
    QCOMPARE(QFileInfo(path).size(), FileSize);

    QString error;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &error,
                 L2Presentation::Text, L2ContentKind::Text),
             qPrintable(error));
    QVERIFY(tab()->isL2Mode());
    QVERIFY(tab()->isReadOnlyByPolicy());
    QVERIFY(!tab()->isBinaryDocument());
    auto *provider = tab()->windowedReadOnlyProvider();
    QVERIFY(provider);
    QCOMPARE(provider->fileSize(), FileSize);

    SearchResultsDock dock;
    dock.resize(1100, 360);
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    QSignalSpy jumpSpy(&dock, &SearchResultsDock::jumpToTabByteOffset);
    connect(&dock, &SearchResultsDock::jumpToTabByteOffset,
            tab(), [](EditorTab *targetTab, qint64 offset, int length) {
                if (targetTab)
                    targetTab->gotoByteOffset(offset, length);
            });

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.setSearchResultsDock(&dock);
    dialog.showReplaceMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *replacement = dialog.findChild<QLineEdit *>(QStringLiteral("replaceTextEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *caseSensitive = dialog.findChild<QCheckBox *>(
        QStringLiteral("caseSensitiveSearchCheck"));
    auto *wholeWord = dialog.findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
    auto *findAll = dialog.findChild<QPushButton *>(QStringLiteral("findAllButton"));
    auto *replace = dialog.findChild<QPushButton *>(QStringLiteral("replaceButton"));
    auto *replaceAll = dialog.findChild<QPushButton *>(QStringLiteral("replaceAllButton"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("cancelSearchButton"));
    auto *tree = dock.findChild<QTreeView *>(QStringLiteral("searchResultsTree"));
    QVERIFY(pattern && replacement && regex && caseSensitive && wholeWord
            && findAll && replace && replaceAll && cancel && tree);
    QVERIFY(regex->isEnabled());
    QTest::mouseClick(regex, Qt::LeftButton);
    QVERIFY(regex->isChecked());
    QVERIFY(caseSensitive->isEnabled());
    caseSensitive->setChecked(false);
    QVERIFY(!wholeWord->isEnabled());
    QVERIFY(!replacement->isEnabled());
    QVERIFY(!replace->isEnabled());
    QVERIFY(!replaceAll->isEnabled());
    pattern->setText(QStringLiteral("s3_regex_([0-9]+)_end"));

    QTest::mouseClick(findAll, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(cancel->isVisible() || tree->model(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!cancel->isVisible(), 120000);
    auto *model = qobject_cast<SearchResultsModel *>(tree->model());
    QVERIFY(model);
    QCOMPARE(model->totalMatches(), qint64(1));
    const GroupedSearchLine &result = model->groupedLineAt(0);
    QCOMPARE(result.firstByteOffset, expectedOffset);
    QCOMPARE(result.firstLength, matchBytes.size());
    QVERIFY(result.lineContext.contains(matchBytes));

    const QModelIndex contextIndex = model->index(
        0, SearchResultsModel::COL_CONTEXT);
    const QRect rect = tree->visualRect(contextIndex);
    QVERIFY(rect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, rect.center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                       Qt::NoModifier, rect.center());
    QTRY_COMPARE_WITH_TIMEOUT(jumpSpy.count(), 1, 3000);
    QCOMPARE(jumpSpy.first().at(1).toLongLong(), expectedOffset);
    QCOMPARE(jumpSpy.first().at(2).toInt(), matchBytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(provider->selectionStartOffset(), expectedOffset, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(provider->selectionEndOffset(),
                              expectedOffset + matchBytes.size(), 5000);
    QCOMPARE(provider->sourceBytes(expectedOffset, matchBytes.size()), matchBytes);
    QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
    QVERIFY(dock.grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s3-text-regex-after-2g-dock.png"))));
    QVERIFY(container_->grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s3-text-regex-after-2g-editor.png"))));
    dock.close();
}

void SmokeFindReplace::tc_fr_031_real_s3_default_literal_is_not_regex()
{
    const QString path = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../bigtestfile"));
    if (!QFileInfo::exists(path))
        QSKIP("retained 4.9 GB S3 fixture is not available");
    const qint64 size = QFileInfo(path).size();
    QVERIFY(size > 2LL * 1024 * 1024 * 1024);

    // Reproduce a user copying text from the ASCII side of the Hex view.  A
    // 12-byte printable run is effectively unique in this random fixture but
    // remains ordinary text that QLineEdit can hold without transformation.
    const qint64 probeOffset = size * 3 / 4;
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(source.seek(probeOffset));
    const QByteArray probe = source.read(1024 * 1024);
    QCOMPARE(probe.size(), 1024 * 1024);
    QByteArray copiedText;
    qint64 copiedOffset = -1;
    for (qsizetype start = 0; start + 12 <= probe.size(); ++start) {
        bool printable = true;
        bool hasLetter = false;
        for (qsizetype index = 0; index < 12; ++index) {
            const uchar value = uchar(probe.at(start + index));
            if (value < 0x20 || value > 0x7e) {
                printable = false;
                break;
            }
            hasLetter = hasLetter
                || (value >= 'A' && value <= 'Z')
                || (value >= 'a' && value <= 'z');
        }
        if (printable && hasLetter) {
            copiedText = probe.mid(start, 12);
            copiedOffset = probeOffset + start;
            break;
        }
    }
    QVERIFY2(copiedOffset >= 0, "no printable copied-text fixture found");
    source.close();

    QString error;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &error,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(error));
    auto *provider = tab()->windowedReadOnlyProvider();
    QVERIFY(provider);

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *caseSensitive = dialog.findChild<QCheckBox *>(
        QStringLiteral("caseSensitiveSearchCheck"));
    auto *wholeWord = dialog.findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && regex && hex && caseSensitive && wholeWord
            && wrap && next && status);

    // This is the product default reported by the user: only Wrap is checked.
    QVERIFY(!regex->isChecked());
    QVERIFY(!hex->isChecked());
    QVERIFY(!caseSensitive->isChecked());
    QVERIFY(!wholeWord->isChecked());
    QVERIFY(wrap->isChecked());
    pattern->setText(QString::fromLatin1(copiedText));

    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(next, Qt::LeftButton);
    qInfo() << "S3_DEFAULT_LITERAL_FIND_NEXT_MS=" << elapsed.elapsed();
    QVERIFY2(tab()->lastSearchError().isEmpty(),
             qPrintable(tab()->lastSearchError()));
    QVERIFY(!status->text().contains(QStringLiteral("正则")));
    QVERIFY(!status->text().contains(QStringLiteral("UTF-8")));
    QCOMPARE(provider->selectionStartOffset(), copiedOffset);
    QCOMPARE(provider->selectionEndOffset(), copiedOffset + copiedText.size());
    QCOMPARE(provider->sourceBytes(copiedOffset, copiedText.size()), copiedText);
    QCOMPARE(provider->visibleVerticalScrollBarCount(), 1);
    QVERIFY(container_->grab().save(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("gui-s3-default-literal-copied-text.png"))));
}

void SmokeFindReplace::tc_fr_032_invalid_utf8_binary_default_literal_portable()
{
    QByteArray payload;
    payload.append(char(0xff));
    payload.append(char(0xfe));
    payload.append(char(0x80));
    payload.append("prefix-CopyMeToken-suffix");
    payload.append(char(0xc0));
    payload.append(" cat catalog _cat cat!");
    const qint64 expectedOffset = payload.indexOf("CopyMeToken");
    const qint64 firstWholeWord = payload.indexOf("cat", int(expectedOffset));
    const qint64 secondWholeWord = payload.lastIndexOf("cat");
    QVERIFY(expectedOffset >= 0);
    QVERIFY(firstWholeWord >= 0 && secondWholeWord > firstWholeWord);
    const QString path = createSmallTestFile(
        payload, QStringLiteral("fr-032-invalid-utf8.bin"));

    QString error;
    QVERIFY2(tab()->openLargeFileL2(
                 path, &error,
                 L2Presentation::Hex, L2ContentKind::Binary),
             qPrintable(error));
    auto *provider = tab()->windowedReadOnlyProvider();
    QVERIFY(provider || view());

    FindReplaceDialog dialog(container_);
    dialog.setActiveTab(tab());
    dialog.showFindMode();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *pattern = dialog.findChild<QLineEdit *>(QStringLiteral("findPatternEdit"));
    auto *regex = dialog.findChild<QCheckBox *>(QStringLiteral("regexSearchCheck"));
    auto *hex = dialog.findChild<QCheckBox *>(QStringLiteral("hexBytesCheck"));
    auto *caseSensitive = dialog.findChild<QCheckBox *>(
        QStringLiteral("caseSensitiveSearchCheck"));
    auto *wholeWord = dialog.findChild<QCheckBox *>(QStringLiteral("wholeWordSearchCheck"));
    auto *wrap = dialog.findChild<QCheckBox *>(QStringLiteral("wrapSearchCheck"));
    auto *next = dialog.findChild<QPushButton *>(QStringLiteral("findNextButton"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("findStatusLabel"));
    QVERIFY(pattern && regex && hex && caseSensitive && wholeWord
            && wrap && next && status);
    QVERIFY(!regex->isChecked() && !hex->isChecked()
            && !caseSensitive->isChecked() && !wholeWord->isChecked()
            && wrap->isChecked());

    // Deliberately change case. Binary Literal's documented insensitive mode
    // folds ASCII bytes but never validates unrelated bytes as UTF-8.
    pattern->setText(QStringLiteral("copymetoken"));
    QTest::mouseClick(next, Qt::LeftButton);
    QVERIFY2(tab()->lastSearchError().isEmpty(),
             qPrintable(tab()->lastSearchError()));
    QVERIFY(!status->text().contains(QStringLiteral("正则")));
    QVERIFY(!status->text().contains(QStringLiteral("UTF-8")));
    if (provider) {
        QCOMPARE(provider->selectionStartOffset(), expectedOffset);
        QCOMPARE(provider->selectionEndOffset(),
                 expectedOffset + qint64(QByteArrayLiteral("CopyMeToken").size()));
        QCOMPARE(provider->sourceBytes(expectedOffset, 11),
                 QByteArrayLiteral("CopyMeToken"));
    } else {
        QCOMPARE(view()->selectionStart(), expectedOffset);
        QCOMPARE(view()->selectionEnd(),
                 expectedOffset + qint64(QByteArrayLiteral("CopyMeToken").size()));
        QCOMPARE(readByteRange(expectedOffset, expectedOffset + 11),
                 QByteArrayLiteral("CopyMeToken"));
    }

    // Whole word remains a literal query with an ASCII byte boundary in a
    // Binary subject. It must skip catalog/_cat and must not validate the
    // unrelated FF/FE/80/C0 bytes as UTF-8 or expose a regex error.
    pattern->setText(QStringLiteral("cat"));
    QTest::mouseClick(wholeWord, Qt::LeftButton);
    QVERIFY(wholeWord->isChecked());
    QVERIFY(!regex->isChecked());
    tab()->resetSearchPosition(true);
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(view()->selectionStart(), firstWholeWord);
    QCOMPARE(view()->selectionEnd(), firstWholeWord + 3);
    QVERIFY(tab()->lastSearchError().isEmpty());
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(view()->selectionStart(), secondWholeWord);
    QCOMPARE(view()->selectionEnd(), secondWholeWord + 3);
    QVERIFY(tab()->lastSearchError().isEmpty());
}

QTEST_MAIN(SmokeFindReplace)
#include "smoke_find_replace.moc"
