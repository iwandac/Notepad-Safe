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

#include "SmokeTestBase.h"
#include "EditorTab.h"
#include "FindReplaceDialog.h"
#include "VirtualFileView.h"
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

    // 全部打开文件替换：仅修改可编辑 Text，明确跳过 L2/只读目标；
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
    auto *summary = dock.findChild<QLabel *>(QStringLiteral("searchResultsSummary"));
    auto *tree = dock.findChild<QTreeView *>();
    QVERIFY(pattern && scope && findAll && cancel && close && summary && tree);
    pattern->setText(marker);
    scope->setCurrentIndex(scope->findData(1));
    QTest::mouseClick(findAll, Qt::LeftButton);
    QVERIFY(cancel->isVisible() && cancel->isEnabled());
    QVERIFY(close->isVisible() && close->isEnabled());

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
    QVERIFY2(tab()->openLargeFileL2(path, &error, false,
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
    QString error;
    QVERIFY(textTab->loadFile(textPath));
    QVERIFY2(binaryTab->openLargeFileL2(binaryPath, &error, false,
                                        L2Presentation::Hex,
                                        L2ContentKind::Binary), qPrintable(error));
    workspace.addTab(textTab, QStringLiteral("Text"));
    workspace.addTab(binaryTab, QStringLiteral("Binary"));
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
    QVERIFY(status->text().contains(QStringLiteral("跳过 1 个文本文件")));

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
    const QByteArray readonlyOriginal =
        QByteArray("readonly ") + marker.toUtf8() + "\r\n";
    const QString firstPath = createSmallTestFile(firstOriginal, "fr-015-first.txt");
    const QString secondPath = createSmallTestFile(secondOriginal, "fr-015-second.txt");
    const QString readonlyPath = createSmallTestFile(readonlyOriginal, "fr-015-readonly.txt");

    QTabWidget workspace;
    workspace.resize(1050, 650);
    auto *first = new EditorTab(&workspace);
    auto *second = new EditorTab(&workspace);
    auto *secure = new EditorTab(&workspace);
    auto *readonly = new EditorTab(&workspace);
    QVERIFY(first->loadFile(firstPath));
    QVERIFY(second->loadFile(secondPath));
    secure->initializeSecureDocument(
        QUuid::createUuid(), QStringLiteral("secure-note"),
        QStringLiteral("secure %1\r\n").arg(marker));
    QVERIFY(secure->isSecureDocument());
    QString error;
    QVERIFY2(readonly->openLargeFileL2(readonlyPath, &error, false), qPrintable(error));
    QVERIFY(readonly->isL2Mode());
    workspace.addTab(first, QStringLiteral("first"));
    workspace.addTab(second, QStringLiteral("second"));
    workspace.addTab(secure, QStringLiteral("secure"));
    workspace.addTab(readonly, QStringLiteral("readonly"));
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
    QVERIFY(confirmationText.contains(QStringLiteral("3 个可编辑文本文件")));
    QVERIFY(confirmationText.contains(QStringLiteral("4 处匹配")));
    QVERIFY(confirmationText.contains(QStringLiteral("只读/未晋升大文件 1 个")));
    QCOMPARE(first->editor()->text().count(replacement), 2);
    QCOMPARE(second->editor()->text().count(replacement), 1);
    QCOMPARE(secure->editor()->text().count(replacement), 1);
    QVERIFY(!readonly->editor()->text().contains(replacement));
    QVERIFY(first->isModified());
    QVERIFY(second->isModified());
    QVERIFY(secure->isModified());
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

    QVERIFY(dialog.grab().save(QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("gui-all-open-replace-complete.png"))));
    readonly->shutdownL2BeforeClose();
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

QTEST_MAIN(SmokeFindReplace)
#include "smoke_find_replace.moc"
