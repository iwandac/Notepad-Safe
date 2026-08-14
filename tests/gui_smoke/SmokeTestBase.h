#pragma once
// ============================================================================
//  SmokeTestBase —— GUI 冒烟测试公共基类（Qt6 Test，QObject 子类）
//
//  职责：
//    · 实例化 EditorTab 并显式走 L2 openLargeFileL2（真实 VirtualFileView + mmap）
//    · 封装 openL2 / readLineBytes / insertAtLine / gotoLineCol /
//      clickAtLineCol / dragSelectLineColRange / getSelectionBytes /
//      saveToTempAndReopen 等 helper，简化 smoke 用例编写
//    · 所有操作走真实 GUI 事件链路（QTest::mouseClick / keyClicks / keyPress）
//      不直接调用 private 方法伪造状态（符合全局约束文档 §2.3）
//
//  依赖：
//    · QApplication（由 QTEST_MAIN 创建）
//    · Qt6::Test
// ============================================================================
#include <QtCore/QObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QPair>

class QWidget;
class EditorTab;
class VirtualFileView;

class SmokeTestBase : public QObject {
    Q_OBJECT
public:
    explicit SmokeTestBase(QObject* parent = nullptr);
    ~SmokeTestBase() override;

protected slots:
    // QTest 每个用例前后钩子
    void init();      // 每个 case 前：创建 EditorTab + 容器窗口
    void cleanup();   // 每个 case 后：销毁容器 + 清理临时文件

protected:
    // ---- 基础操作：打开 / 保存 ----
    // 打开 smalltestfile（或自定义文件）到 L2 模式。
    // 返回 true = 成功，isL2Mode() == true；否则填 errMsg。
    bool openL2(const QString& filePath, QString* errMsg = nullptr);

    // 把当前 L2 内容另存到临时路径，再重新打开（模拟保存 round-trip）。
    // 返回新的 tempPath，失败返回空字符串。
    QString saveAsTempAndReopen(QString* errMsg = nullptr);

    // 创建一个临时文件（srcPath 的精确拷贝），返回拷贝后的绝对路径。
    // 避免直接修改用户的 smalltestfile 原文件。
    QString cloneToTemp(const QString& srcPath);

    // ---- 坐标 & 导航 ----
    // 返回 Line N (0-based) 的起始虚拟字节偏移。
    qint64 lineStartByte(int line) const;

    // 返回 Line N 的字节长度（不含结尾换行符）。
    qint64 lineBytesLength(int line) const;

    // 读取 Line N 的 [colBegin, colEnd) 字节区间（已应用 patch，0-based 字节 col）。
    QByteArray readLineColBytes(int line, qint64 colBegin, qint64 colEnd) const;

    // 读取整行字节（不含结尾换行）。
    QByteArray readLineBytes(int line) const;

    // 用真实键盘事件跳转：先按 Ctrl+G（如果有 goto 对话框）或直接
    // 调 API gotoByteOffset（我们允许 goto API 作为显式跳转，然后再用
    // 键盘输入，因为 goto 行号对话框本身属于另一个 GUI 交互路径，
    // 这里对"跳转后位置正确"的验证用 API 跳转 + 验证选区内容）。
    void moveCursorToLineCol(int line, qint64 col, bool selectTo = false);

    // ---- 真实 GUI 事件输入 ----
    // 在当前光标位置用 QTest::keyClicks 真实输入文本（逐 keyPressEvent，
    // 会走 VirtualFileView 的 keyPressEvent → insertAtCursor 链路）。
    // 注意：CI / 后台窗口环境下，keyClicks 可能因焦点/平台输入法被丢弃，
    //       因此同时提供 insertTextAtCursorDirect() 作为「绕过 GUI 事件的
    //       程序化 insert」fallback——我们用它验证坐标变换，再用 GUI 事件
    //       验证按键路径。
    void typeTextAtCursor(const QString& utf8Text);

    // 程序化直接插入（直接调 VirtualFileView::insertAtCursor）。
    // 用途：当 GUI 事件路径（keyClicks）因焦点问题丢事件时，仍然可以稳定
    //       驱动 patch 链路验证坐标 bug（如 tc_coord_002）。
    // 返回 true = 成功。
    bool insertTextAtCursorDirect(const QByteArray& utf8Bytes, QString* errMsg = nullptr);

    // 模拟点击 Line N 的 Col M 字节位置：用 viewport() 的像素坐标计算
    // 后走 QTest::mouseClick(view, Qt::LeftButton, {}, pt)。
    // 断言：点击后 cursor 的 line/col = (expectedLine, expectedCol)，误差为 0。
    void clickAtLineCol(int line, qint64 col,
                        int* outActualLine = nullptr, qint64* outActualCol = nullptr);

    // 模拟拖选：按下 LineBegin/ColBegin → 鼠标移动到 LineEnd/ColEnd → 释放。
    // 断言：selectionStart/End 对应字节区间与期望一致。
    void dragSelect(int lineBegin, qint64 colBegin, int lineEnd, qint64 colEnd);

    // ---- 选区 & 断言 ----
    // 取得当前选区字节 [start,end)（= PatchList 应用后真实字节内容）。
    QByteArray currentSelectionBytes() const;

    // 取得虚拟字节区间 [start,end) 的内容（等价于 readByteRange）。
    QByteArray readByteRange(qint64 start, qint64 end) const;

    // ---- 访问器 ----
    EditorTab*        tab()  const { return tab_; }
    VirtualFileView*  view() const;   // 内部 if L2 返回 l2View()，否则 nullptr
    bool              isL2() const;
    qint64            totalLines() const;

    // 小文件专用：在 TEMP 里构造一个测试文件（内容 = data，UTF-8 字节），
    // 不使用 smalltestfile（228MB）以加速用例。小文件也用 L2 打开，
    // 保证 L2 的 patch/坐标链路被真实驱动。
    QString createSmallTestFile(const QByteArray& data, const QString& suffixName = "smoke-l2.txt");

    // 内置的 smalltestfile 路径（F:/projects/credentials/smalltestfile）。
    static QString defaultLargeFilePath();

protected:
    QWidget*    container_ = nullptr;   // 顶层可见窗口（show() 后真实 paintEvent）
    EditorTab*  tab_       = nullptr;
    QTemporaryDir tmpDir_;              // 每个 case 独立临时目录，析构自动清
};
