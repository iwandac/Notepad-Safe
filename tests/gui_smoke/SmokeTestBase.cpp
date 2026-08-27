#include "SmokeTestBase.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtTest/QTest>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSaveFile>

#include "EditorTab.h"
#include "VirtualFileView.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "PlainFilePersistence.h"

SmokeTestBase::SmokeTestBase(QObject* parent) : QObject(parent) {}
SmokeTestBase::~SmokeTestBase() = default;

void SmokeTestBase::init() {
    container_ = new QWidget(nullptr, Qt::Window);
    container_->setWindowTitle(QStringLiteral("L2 GUI Smoke — %1").arg(metaObject()->className()));
    container_->resize(1200, 800);
    auto* lay = new QVBoxLayout(container_);
    lay->setContentsMargins(0, 0, 0, 0);
    tab_ = new EditorTab(container_);
    lay->addWidget(tab_);
    container_->show();
    QVERIFY(QTest::qWaitForWindowExposed(container_));
    QApplication::processEvents(QEventLoop::AllEvents, 100);
}

void SmokeTestBase::cleanup() {
    if (tab_) tab_->shutdownL2BeforeClose();
    if (container_) {
        container_->close();
        delete container_;
        container_ = nullptr;
        tab_ = nullptr;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 50);
}

bool SmokeTestBase::openL2(const QString& filePath, QString* errMsg) {
    if (!QFile::exists(filePath)) {
        if (errMsg) *errMsg = QStringLiteral("文件不存在: %1").arg(filePath);
        return false;
    }
    QString err;
    const bool ok = tab_->openLargeFileL2(filePath, &err);
    if (!ok) {
        if (errMsg) *errMsg = err;
        return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 200);
    if (!tab_->isL2Mode()) {
        if (errMsg) *errMsg = QStringLiteral("openLargeFileL2 成功但 isL2Mode 返回 false");
        return false;
    }
    // 测试夹具要等待真实的完成标志。旧逻辑以 totalLines > 100 判断就绪，
    // 对只有几行的小文件永远不成立，并且会在索引仍 incomplete 时继续执行。
    QElapsedTimer waitTimer;
    waitTimer.start();
    while (tab_->l2View()->lineIndex().isIncomplete()
           && waitTimer.elapsed() < 3000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(40);
    }
    return true;
}

QString SmokeTestBase::cloneToTemp(const QString& srcPath) {
    const QString name = QFileInfo(srcPath).fileName();
    const QString dst = tmpDir_.filePath(QStringLiteral("clone-%1").arg(name));
    if (QFile::exists(dst)) QFile::remove(dst);
    if (!QFile::copy(srcPath, dst)) return {};
    QFile f(dst);
    f.setPermissions(f.permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser | QFileDevice::WriteGroup | QFileDevice::WriteOther);
    return QFileInfo(dst).absoluteFilePath().replace(R"(\)", "/");
}

QString SmokeTestBase::saveAsTempAndReopen(QString* errMsg) {
    QString err;
    const QString tmp = tmpDir_.filePath(QStringLiteral("saved-%1.bin").arg(quint64(this) & 0xFFFF, 4, 16, QLatin1Char('0')));
    PlainFilePersistence persistence;
    const PersistenceResult save = persistence.save(
        tab_, {tmp, {}, tab_->persistenceBaseline(), false});
    if (!save.succeeded()) {
        err = save.error;
        if (errMsg) *errMsg = err;
        return {};
    }
    tab_->shutdownL2BeforeClose();
    if (!tab_->openLargeFileL2(tmp, &err)) {
        if (errMsg) *errMsg = QStringLiteral("重打开失败: %1").arg(err);
        return {};
    }
    return tmp;
}

// --------- 坐标 helper：lineIndex(orig) ↔ Virtual 坐标（VirtualFileView / PatchList 的接口期望 virt） ---------
static qint64 _toVirt(const VirtualFileView* v, qint64 origByte) {
    return v ? v->patches().origToVirt(origByte) : origByte;
}
static qint64 _toVirtForLineStart(const VirtualFileView* v, qint64 origLineStart) {
    return v ? v->patches().origToVirtForLineStart(origLineStart) : origLineStart;
}
static qint64 _toOrig(const VirtualFileView* v, qint64 virtByte) {
    return v ? v->patches().virtToOrig(virtByte) : virtByte;
}

qint64 SmokeTestBase::lineStartByte(int line) const {
    // 返回 virtual 字节偏移：orig(lineToByteOffset) → origToVirtForLineStart
    // （必须用 ForLineStart 版本，否则在行首 insert 会把插入内容归属到上一行末尾）
    const qint64 origStart = view()->lineIndex().lineToByteOffset(line);
    return _toVirtForLineStart(view(), origStart);
}

qint64 SmokeTestBase::lineBytesLength(int line) const {
    // 用 virtual 的相邻两行 start 相减（即 display 长度）；这样当插入/替换修改了行内容，length 也准确。
    const qint64 start = lineStartByte(line);
    const qint64 nextStart = lineStartByte(line + 1);
    return qMax(qint64(0), nextStart - start);
}

QByteArray SmokeTestBase::readLineColBytes(int line, qint64 colBegin, qint64 colEnd) const {
    const qint64 s = lineStartByte(line);
    const qint64 len = lineBytesLength(line);
    const qint64 cb = qBound(qint64(0), colBegin, len);
    const qint64 ce = qBound(cb, colEnd, len);
    return view()->readByteRange(s + cb, s + ce);
}

QByteArray SmokeTestBase::readLineBytes(int line) const {
    const qint64 s = lineStartByte(line);
    const qint64 next = lineStartByte(line + 1);
    qint64 n = next - s;
    if (n <= 0) return {};
    QByteArray all = view()->readByteRange(s, s + n);
    while (!all.isEmpty() && (all.endsWith('\n') || all.endsWith('\r'))) all.chop(1);
    return all;
}

void SmokeTestBase::moveCursorToLineCol(int line, qint64 col, bool selectTo) {
    const qint64 s = lineStartByte(line);
    const qint64 len = lineBytesLength(line);
    const qint64 off = s + qBound(qint64(0), col, len);
    // gotoByteOffset 接收 virtual 坐标（与 cursorByte_ 一致）
    tab_->gotoByteOffset(off, 0);
    if (selectTo) view()->selectBytes(view()->selectionStart(), off);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
}

void SmokeTestBase::typeTextAtCursor(const QString& utf8Text) {
    auto* v = view();
    QVERIFY(v != nullptr);
    // QAbstractScrollArea 的焦点代理是 viewport()；如果只 setFocus(v)，
    // viewport 虽然拿到 focus 但 Qt 仍通过 focusProxy 转发 keyPressEvent。
    // 保险起见：set focus 到 viewport 并直接在 viewport 上做 keyClicks
    // （VirtualFileView::keyPressEvent 会被 Qt 通过 viewport → parent 转发到。）
    auto* vp = v->viewport();
    vp->setFocus(Qt::ActiveWindowFocusReason);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    if (!vp->hasFocus()) {
        v->setFocus(Qt::ActiveWindowFocusReason);
        QApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    QWidget* keyTarget = (vp->hasFocus() ? (QWidget*)vp : (QWidget*)v);
    QTest::keyClicks(keyTarget, utf8Text);
    // 每轮输入 2~3 字符后可能会触发 L2 内部的异步 patch 重算 / viewport update，
    // 给 120ms 让后续 update 走完。
    QApplication::processEvents(QEventLoop::AllEvents, 150);
    QTest::qWait(60);
}

bool SmokeTestBase::insertTextAtCursorDirect(const QByteArray& utf8Bytes, QString* errMsg) {
    auto* v = view();
    if (!v) {
        if (errMsg) *errMsg = QStringLiteral("L2 view 未初始化");
        return false;
    }
    QString err;
    const bool ok = v->insertAtCursor(utf8Bytes, &err);
    if (!ok) {
        if (errMsg) *errMsg = err;
        return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 120);
    v->viewport()->update();
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    return true;
}

void SmokeTestBase::clickAtLineCol(int line, qint64 col, int* outActualLine, qint64* outActualCol) {
    auto* v = view();
    QVERIFY(v != nullptr);
    v->setFocus(Qt::ActiveWindowFocusReason);
    v->gotoLine(line);
    QApplication::processEvents(QEventLoop::AllEvents, 50);

    const int approxVisible = qMax(20, v->viewport()->height() / 20);
    const int approxLineHeight = qMax(8, v->viewport()->height() / approxVisible);
    const int viewY = qBound(approxLineHeight,
                             3 * approxLineHeight + approxLineHeight / 2,
                             v->viewport()->height() - approxLineHeight);
    const int viewX = qBound(40, int(col) * 9 + 120, v->viewport()->width() - 40);
    const QPoint pt(viewX, viewY);
    QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, pt);
    QApplication::processEvents(QEventLoop::AllEvents, 60);
    if (outActualLine) *outActualLine = -1;
    if (outActualCol)  *outActualCol  = -1;
}

void SmokeTestBase::dragSelect(int lineBegin, qint64 colBegin, int lineEnd, qint64 colEnd) {
    auto* v = view();
    QVERIFY(v != nullptr);
    v->setFocus(Qt::ActiveWindowFocusReason);
    const qint64 offBegin = lineStartByte(lineBegin) + qBound(qint64(0), colBegin, lineBytesLength(lineBegin));
    const qint64 offEnd   = lineStartByte(lineEnd)   + qBound(qint64(0), colEnd,   lineBytesLength(lineEnd));
    // 先把光标放到 offBegin（让视觉与锚点一致）
    tab_->gotoByteOffset(offBegin, 0);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    // 直接设置选区（和 Smoke 拖选语义一致：start=offBegin，end=offEnd）
    v->selectBytes(offBegin, offEnd);
    v->viewport()->update();
    QApplication::processEvents(QEventLoop::AllEvents, 30);
}

QByteArray SmokeTestBase::currentSelectionBytes() const {
    return view() ? view()->selectedBytes() : QByteArray{};
}

QByteArray SmokeTestBase::readByteRange(qint64 start, qint64 end) const {
    return view() ? view()->readByteRange(start, end) : QByteArray{};
}

VirtualFileView* SmokeTestBase::view() const {
    return tab_ ? tab_->l2View() : nullptr;
}

bool SmokeTestBase::isL2() const {
    return tab_ && tab_->isL2Mode();
}

qint64 SmokeTestBase::totalLines() const {
    return view() ? view()->lineIndex().totalLines() : 0;
}

QString SmokeTestBase::createSmallTestFile(const QByteArray& data, const QString& suffixName) {
    const QString p = tmpDir_.filePath(suffixName);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    const qint64 w = f.write(data);
    if (w != data.size()) { f.close(); return {}; }
    f.close();
    return QFileInfo(p).absoluteFilePath().replace(R"(\)", "/");
}

QString SmokeTestBase::defaultLargeFilePath() {
    const QString fromEnv = qEnvironmentVariable("NOTEPADSAFE_GUI_FIXTURE");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return QFileInfo(fromEnv).absoluteFilePath();
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.absoluteFilePath(QStringLiteral("../../testfiles/smalltestfile")),
        appDir.absoluteFilePath(QStringLiteral("../../smalltestfile")),
        QStringLiteral("F:/projects/notepadsafe/testfiles/smalltestfile"),
        QStringLiteral("F:/projects/notepadsafe/smalltestfile")
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    // CI/新 clone 不应依赖开发者机器上的大文件 fixture。生成一个包含足够
    // 行数的确定性文件；openL2() 会显式走 mmap 路径，因此无需伪造巨大体积。
    const QString generated = QDir::temp().absoluteFilePath(
        QStringLiteral("notepadsafe-gui-smoke-default.txt"));
    QSaveFile out(generated);
    if (!out.open(QIODevice::WriteOnly)) return {};
    for (int i = 0; i < 4096; ++i) {
        QByteArray line = QByteArrayLiteral("line-") + QByteArray::number(i) + QByteArrayLiteral(":");
        while (line.size() < 112) line.append(static_cast<char>('A' + (i % 26)));
        line.append('\n');
        if (out.write(line) != line.size()) return {};
    }
    if (!out.commit()) return {};
    return QFileInfo(generated).absoluteFilePath();
}
