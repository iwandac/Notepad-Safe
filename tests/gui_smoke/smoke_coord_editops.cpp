// ============================================================================
//  smoke_coord_editops：坐标错乱 / 输入偏移 / 自动换行 / 光标 / 拖选
//  按 QUALITY_ASSURANCE_AND_GUI_TESTING.md §3.1 强制清单编写。
//  所有用例都必须 FAIL 先捕获数值证据 → 修 → 再跑变绿。
// ============================================================================
#include <QtCore/QObject>
#include <QtTest/QTest>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QDebug>

#include "SmokeTestBase.h"
#include "EditorTab.h"
#include "VirtualFileView.h"
#include "LineIndex.h"

class SmokeCoordEditops : public SmokeTestBase {
    Q_OBJECT
private slots:
    // ===== 非自动换行（默认模式）=====

    // TC_COORD_001：打开 L2 → 跳转 Line 100 → 读取 Line 100 内容 → 与 LineIndex lineStart(100)
    //              + 直接 readByteRange 对比；再 gotoLineCol(100,10) 后读取下一字节与
    //              readByteRange(lineStart+10, lineStart+11) 完全一致。
    void tc_coord_001_goto_and_read_consistency();

    // TC_COORD_002：**用户反馈的核心 bug**：
    //              先在 Line 10 输入标记 "__MARK_BELOW__"（在"靠下"的行输入）
    //              → 然后在 Line 5 输入标记 "__MARK_ABOVE__"（在"靠上"的行输入）
    //              → 断言 Line 11（原 Line 10 下移 1 行）的内容首字节仍为原 Line 10
    //              的字节序列，**仅 vertical 偏移 1 行，horizontal 不偏移任何字符**。
    void tc_coord_002_below_then_above_insert_no_horizontal_offset();

    // TC_COORD_003：拖选 Line 200, Col 5..Line 210, Col 15 → 选区字节 ==
    //              renderRange(virtOffset(200,5) .. virtOffset(210,15)) byte-for-byte
    void tc_coord_003_drag_select_byte_match();

    // ===== 自动换行（Word Wrap）=====

    // TC_WRAP_001：构造一个 3000 字符的超长行（小文件加速）→ 开自动换行
    //              → 垂直滚动到该行 → 任何虚拟 col 点击后，字节位置与点击前
    //              col 一致（不会"跳到下一个 visualLine"）
    void tc_wrap_001_long_line_click_position_consistency();

    // TC_WRAP_002：超长行内，拖选跨 2 个 visual wrap line 的 100 字节
    //              → selectedBytes == mid(超长行, dragStartCol, 100)
    void tc_wrap_002_long_line_drag_across_wrap();
};

// -------- 小工具：把 QByteArray 截断前 64 字节，带 hex 信息用于 FAIL 输出 --------
static QByteArray trunc64(const QByteArray& b) {
    if (b.size() <= 64) return b;
    return b.left(64) + QByteArrayLiteral("...[total=") + QByteArray::number(b.size()) + ']';
}

void SmokeCoordEditops::tc_coord_001_goto_and_read_consistency() {
    QString err;
    const QString clone = cloneToTemp(defaultLargeFilePath());
    QVERIFY2(!clone.isEmpty(), "clone smalltestfile failed");
    QVERIFY2(openL2(clone, &err), qPrintable(QStringLiteral("openL2 failed: %1").arg(err)));
    // smalltestfile 打开后 LineIndex 可能是 incomplete 状态，等 300ms 让后台构建完成（totalLines 用真实值）
    for (int i = 0; i < 8 && totalLines() < 50; ++i) {
        QTest::qWait(80);
    }
    QVERIFY(totalLines() >= 10);

    const int line = 100;
    const qint64 s = lineStartByte(line);
    const qint64 len = lineBytesLength(line);
    QVERIFY(len > 0);
    const QByteArray viaLineBytes = readLineBytes(line);
    const QByteArray viaReadRange = readByteRange(s, s + len);
    // viaReadRange 可能带 \r\n 分隔符；viaLineBytes 已 chop；只比较 viaLineBytes 长度的字节
    QCOMPARE(viaReadRange.left(viaLineBytes.size()), viaLineBytes);

    // 断言 2：gotoLineCol(100, 10) 后，选 [start+10,start+11) 这 1 字节
    //         与 readByteRange 完全一致
    moveCursorToLineCol(line, 10, false);
    auto* v = view();
    v->selectBytes(s + 10, s + 11);
    const QByteArray sel = currentSelectionBytes();
    const QByteArray exp = readByteRange(s + 10, s + 11);
    QCOMPARE(sel.size(), 1);
    QCOMPARE(sel, exp);
}

void SmokeCoordEditops::tc_coord_002_below_then_above_insert_no_horizontal_offset() {
    // 构造可预测纯文本（避免 smalltestfile 是二进制导致内容断言无意义）：
    //   Line 0..19，每行 = "L<行号>: padding<空格>" + 行号 0 填充到 80 字节宽 + \n
    //   这样任意 Line N 的前 6 字节就是固定的 "L<N>: p"，插入 MARK 后断言位置很清晰。
    QByteArray data;
    for (int i = 0; i < 20; ++i) {
        QByteArray line("L" + QByteArray::number(i) + ": padding-");
        while (line.size() < 79) line.append(static_cast<char>('A' + (i % 26)));
        line.append('\n');
        data.append(line);
    }
    const QString path = createSmallTestFile(data, "coord002-text.txt");
    QVERIFY2(!path.isEmpty(), "create temp test file failed");

    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(QStringLiteral("openL2 failed: %1").arg(err)));
    QCOMPARE(totalLines(), 20LL);

    // Step 1：在 Line 10（0-based）输入标记（下方的行）
    const int BELOW_LINE = 10;
    const int ABOVE_LINE = 5;
    const QByteArray MARK_BELOW = QByteArrayLiteral("__MARK_BELOW__");
    const QByteArray MARK_ABOVE = QByteArrayLiteral("__MARK_ABOVE__");

    moveCursorToLineCol(BELOW_LINE, 0, false);
    // 记录 Line 10 原前 32 字节（验证 horizontal 偏移用）
    const QByteArray origBelowFirst32 = readLineBytes(BELOW_LINE).left(32);
    // 插入 MARK_BELOW：程序化 direct 插入（驱动真实 patch 链路）。
    QString errInsert;
    QVERIFY2(insertTextAtCursorDirect(MARK_BELOW, &errInsert),
             qPrintable(QStringLiteral("Insert MARK_BELOW 失败: %1").arg(errInsert)));
    // 调试信息：确认 MARK 到底落到了哪里（可能是上一行末尾 bug）
    {
        const QByteArray tail9 = readLineBytes(BELOW_LINE - 1).right(MARK_BELOW.size() + 4);
        const QByteArray head10 = readLineBytes(BELOW_LINE).left(MARK_BELOW.size() + 4);
        const qint64 l9s = lineStartByte(BELOW_LINE - 1);
        const qint64 l9e = lineStartByte(BELOW_LINE);
        const QByteArray raw9 = readByteRange(l9s, l9e);
        qInfo() << "[tc002 MARK landed] Line9 tail:" << trunc64(tail9)
                << " | Line10 head:" << trunc64(head10)
                << " | Line9.raw bytes(" << l9s << ".." << l9e << ") len=" << raw9.size()
                << " last20=" << trunc64(raw9.right(24));
    }

    // 验证此刻 Line 10 的前 MARK_BELOW.size() 字节是 MARK_BELOW
    const QByteArray belowAfterInsert = readLineBytes(BELOW_LINE).left(MARK_BELOW.size());
    QCOMPARE(belowAfterInsert, MARK_BELOW);

    // Step 2：在 Line 5 行首插入 MARK_ABOVE（上方的行），这是用户说的"在下面输入过的行**上面**
    //         再输入"的典型场景，一旦坐标变换 bug 存在，Line 10 插入的 MARK_BELOW 会水平偏移
    moveCursorToLineCol(ABOVE_LINE, 0, false);
    const QByteArray origAboveFirst32 = readLineBytes(ABOVE_LINE).left(32);
    QVERIFY2(insertTextAtCursorDirect(MARK_ABOVE, &errInsert),
             qPrintable(QStringLiteral("Insert MARK_ABOVE 失败: %1").arg(errInsert)));
    const QByteArray aboveAfterInsert = readLineBytes(ABOVE_LINE).left(MARK_ABOVE.size());
    QCOMPARE(aboveAfterInsert, MARK_ABOVE);
    // 调试：插入 Line 5 后，Line 9/10/11 的开头 + 所有 patch dump
    {
        const QByteArray h9 = readLineBytes(9).left(24);
        const QByteArray h10 = readLineBytes(10).left(24);
        const QByteArray h11 = readLineBytes(11).left(24);
        const qint64 s9 = lineStartByte(9), s10 = lineStartByte(10), s11 = lineStartByte(11);
        // 直接按虚拟 offset 读 raw：[s10..s10+14) 应该 = MARK_BELOW，[s10+14..s10+14+8) = 原 L10 前 8
        const QByteArray rawMark = readByteRange(s10, s10 + MARK_BELOW.size());
        const QByteArray rawAfter = readByteRange(s10 + MARK_BELOW.size(), s10 + MARK_BELOW.size() + 8);
        const QByteArray raw9 = readByteRange(s9, s9 + 24);
        qInfo() << "[tc002 after MARK_ABOVE insert]"
                << "start9=" << s9 << "start10=" << s10 << "start11=" << s11
                << "\n   head9:" << trunc64(h9)
                << "\n   head10:" << trunc64(h10)
                << "\n   head11:" << trunc64(h11)
                << "\n   raw [s10..+14):" << trunc64(rawMark)
                << "\n   raw [s10+14..+22):" << trunc64(rawAfter)
                << "\n   raw [s9..+24):" << trunc64(raw9);
        auto& pl = view()->patches();
        qInfo() << "  patchCount=" << pl.patchCount() << "netDelta=" << pl.netDelta();
        for (int i = 0; i < pl.patchCount(); ++i) {
            const auto* p = pl.patchAt(i);
            qInfo() << "    patch[" << i << "] off=" << p->byteOffset << "old=" << p->oldLength
                    << "newLen=" << p->newText.size() << "origInsertOff=" << p->origInsertOffset
                    << "newText=" << trunc64(p->newText);
        }
        qint64 origL10 = view()->lineIndex().lineToByteOffset(10);
        qint64 vL10_ov = pl.origToVirt(origL10);
        qint64 vL10_fls = pl.origToVirtForLineStart(origL10);
        qInfo() << "  origL10=" << origL10 << "origToVirt=" << vL10_ov << "origToVirtForLineStart=" << vL10_fls;
    }

    // ===== 关键断言：
    //   (a) Line 5 插入的 MARK_ABOVE（无换行）不会把 Line 10 的内容挤到 Line 11，
    //       因此 MARK_BELOW 仍在 Line 10 开头。
    //   (b) MARK_BELOW 之后紧跟的 32 字节应等于原 Line 10 开头 32 字节（即水平未偏移）。
    const int belowLineAfterBothInserts = BELOW_LINE;
    const QByteArray shiftedMark = readLineBytes(belowLineAfterBothInserts).left(MARK_BELOW.size());
    const QByteArray shiftedRest32 = readLineBytes(belowLineAfterBothInserts)
                                        .mid(MARK_BELOW.size(), 32);
    if (shiftedMark != MARK_BELOW) {
        qDebug() << "--- FAIL 数值证据 START ---";
        qDebug() << "期望 Line" << belowLineAfterBothInserts << "前 13 字节 =" << MARK_BELOW;
        qDebug() << "实际 Line" << belowLineAfterBothInserts << "前 64 字节 =" << trunc64(readLineBytes(belowLineAfterBothInserts));
        qDebug() << "原 Line 10（插入前）First32 =" << trunc64(origBelowFirst32);
        qDebug() << "这 13 字节偏移 = " << (shiftedMark.isEmpty() ? -1 : int(shiftedMark[0]) - int(MARK_BELOW[0]));
        qDebug() << "--- FAIL 数值证据 END ---";
    }
    QCOMPARE(shiftedMark, MARK_BELOW);

    // ===== 更严格：MARK_BELOW 后面紧跟的 32 字节应该等于原 Line BELOW_LINE
    //               开头 32 字节（即 origBelowFirst32）—— 若下方内容水平偏移 N 字节，
    //               这里 rest32 就会对不齐 N 个位置。
    if (shiftedRest32 != origBelowFirst32) {
        qDebug() << "--- FAIL 水平偏移数值证据 START ---";
        qDebug() << "期望 (Mark 后 32) = " << trunc64(origBelowFirst32);
        qDebug() << "实际 (Mark 后 32) = " << trunc64(shiftedRest32);
        // 找出第一个不同字节的偏移 = horizontal offset bug 量
        int diffAt = -1;
        const int minLen = qMin(origBelowFirst32.size(), shiftedRest32.size());
        for (int i = 0; i < minLen; ++i) {
            if (origBelowFirst32[i] != shiftedRest32[i]) { diffAt = i; break; }
        }
        qDebug() << "水平偏移检测：第 1 个不同字节在 rest 的 offset = " << diffAt
                 << "(期望 0，若 = 8 说明正好把上面插入的 MARK_ABOVE 的长度叠加到了下面)";
        qDebug() << "--- FAIL 水平偏移数值证据 END ---";
    }
    QCOMPARE(shiftedRest32, origBelowFirst32);
    Q_UNUSED(origAboveFirst32);
}

void SmokeCoordEditops::tc_coord_003_drag_select_byte_match() {
    QString err;
    const QString clone = cloneToTemp(defaultLargeFilePath());
    QVERIFY2(!clone.isEmpty(), "clone smalltestfile failed");
    QVERIFY2(openL2(clone, &err), qPrintable(QStringLiteral("openL2 failed: %1").arg(err)));
    QVERIFY(totalLines() > 220);

    const int L1 = 200, L2 = 210;
    const qint64 C1 = 5, C2 = 15;
    const qint64 offBeg = lineStartByte(L1) + C1;
    const qint64 offEnd = lineStartByte(L2) + qMin(lineBytesLength(L2), C2);
    // 拖选
    dragSelect(L1, C1, L2, C2);
    const QByteArray viaSelection = currentSelectionBytes();
    const QByteArray viaRender    = readByteRange(offBeg, offEnd);
    if (viaSelection != viaRender) {
        qDebug() << "--- FAIL 拖选数值证据 ---";
        qDebug() << "选区字节数 =" << viaSelection.size() << "，renderRange 字节数 =" << viaRender.size();
        qDebug() << "选区前 64 =" << trunc64(viaSelection);
        qDebug() << "期望前 64 =" << trunc64(viaRender);
        qDebug() << "字节差（第 1 个不同位置）:";
        const int minLen = qMin(viaSelection.size(), viaRender.size());
        int d = -1;
        for (int i = 0; i < minLen; ++i)
            if (viaSelection[i] != viaRender[i]) { d = i; break; }
        qDebug() << "  diffIdx = " << d;
        qDebug() << "--- END ---";
    }
    QCOMPARE(viaSelection.size(), viaRender.size());
    QCOMPARE(viaSelection, viaRender);
}

// ===== Word Wrap：用小文件（自带超长行）加速，不依赖 smalltestfile =====
static QByteArray makeFileWithLongLine() {
    QByteArray out;
    // 短行 x 20
    for (int i = 0; i < 20; ++i) {
        out.append("short line ").append(QByteArray::number(i)).append("\r\n");
    }
    // 超长行 3000 字节：AAAABBBBCCCC... 循环 （4 字节 pat * 750 = 3000）
    QByteArray pat;
    for (char c = 'A'; c <= 'Z'; ++c) pat.append(QByteArray(4, c));
    // pat 长度 = 26*4 = 104；重复 29 次 = 3016，再截 3000 正好
    QByteArray longLine;
    for (int i = 0; i < 29; ++i) longLine.append(pat);
    longLine.truncate(3000);
    out.append(longLine).append("\r\n");
    // 再短行 x 20
    for (int i = 0; i < 20; ++i) {
        out.append("short tail ").append(QByteArray::number(i)).append("\r\n");
    }
    return out;
}

void SmokeCoordEditops::tc_wrap_001_long_line_click_position_consistency() {
    const QByteArray fdata = makeFileWithLongLine();
    const QString path = createSmallTestFile(fdata, "wrap-longline.bin");
    QVERIFY2(!path.isEmpty(), "create small test file failed");
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(QStringLiteral("open small L2 failed: %1").arg(err)));
    auto* v = view();
    QVERIFY(v != nullptr);
    // 开自动换行
    tab()->setWordWrap(true);
    v->setWordWrap(true);
    QApplication::processEvents(QEventLoop::AllEvents, 80);
    QVERIFY(v->wordWrap());

    const int LONG_LINE_NO = 20;  // 0-based（正好是超长行，前面 20 条短行）
    const qint64 origStart = lineStartByte(LONG_LINE_NO);
    const qint64 origLen   = lineBytesLength(LONG_LINE_NO);
    // 允许 3000±2：不同平台/构造行尾分隔符可能 \r\n 2 字节
    QVERIFY2(origLen >= 2998 && origLen <= 3002,
             qPrintable(QStringLiteral("origLen=%1 不在 2998..3002").arg(origLen)));

    // 对 100 个均匀分布的 col（0..2999，步长 30）做：moveCursorToLineCol(LONG_LINE_NO, col)
    // → 选 [origStart+col, origStart+col+1) 1 字节 → 等于 readByteRange(origStart+col, +1)
    for (int col = 0; col < 3000; col += 30) {
        moveCursorToLineCol(LONG_LINE_NO, col, false);
        v->selectBytes(origStart + col, origStart + col + 1);
        const QByteArray got = currentSelectionBytes();
        const QByteArray exp = readByteRange(origStart + col, origStart + col + 1);
        if (got != exp) {
            qDebug() << "--- FAIL wrap 点击 col=" << col << " 数值证据 ---";
            qDebug() << "期望字节 (char) = " << exp.toHex() << " = '" << (exp.isEmpty() ? ' ' : (char)exp[0]) << "'";
            qDebug() << "实际字节 (char) = " << got.toHex() << " = '" << (got.isEmpty() ? ' ' : (char)got[0]) << "'";
            // 这个 col 期望的字符是：longLine[col]，即我们构造的 AAAABBBBCCCC... 模式
            const int mod = col % 104;
            const int letterIdx = mod / 4;
            const char expectedChar = 'A' + letterIdx;
            qDebug() << "按构造公式，col = " << col << " 应该对应字符 '" << expectedChar << "' (ASCII" << (int)expectedChar << ")";
            qDebug() << "--- END ---";
            QFAIL("word wrap long line click position mismatch");
        }
    }
}

void SmokeCoordEditops::tc_wrap_002_long_line_drag_across_wrap() {
    const QByteArray fdata = makeFileWithLongLine();
    const QString path = createSmallTestFile(fdata, "wrap-longline-drag.bin");
    QVERIFY2(!path.isEmpty(), "create small test file failed");
    QString err;
    QVERIFY2(openL2(path, &err), qPrintable(QStringLiteral("open small L2 failed: %1").arg(err)));
    auto* v = view();
    tab()->setWordWrap(true);
    v->setWordWrap(true);
    QApplication::processEvents(QEventLoop::AllEvents, 80);

    const int LONG_LINE_NO = 20;
    const qint64 start = lineStartByte(LONG_LINE_NO) + 1400;  // 中间位置 1400 → 跨 visual wrap line
    const qint64 end   = start + 100;
    dragSelect(LONG_LINE_NO, 1400, LONG_LINE_NO, 1500);
    const QByteArray sel = currentSelectionBytes();
    const QByteArray exp = readByteRange(start, end);
    QCOMPARE(sel.size(), 100);
    QCOMPARE(sel, exp);
}

QTEST_MAIN(SmokeCoordEditops)
#include "smoke_coord_editops.moc"
