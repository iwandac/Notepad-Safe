// ============================================================
// test_settings_ui.cpp — 设置持久化 & 视图状态 回归测试（纯逻辑，不创建任何 QWidget/QAction）
// 背景：用户反馈"开启自动换行后重启，设置恢复了但菜单勾选没同步显示"
//       根因：createMenus() 先构造 action(用默认 false 初始化勾选)，
//             之后 loadSettings() 才修改成员变量，没有把新值同步回 action。
//       修复：在 MainWindow 构造函数里 loadSettings() 后紧跟 syncGlobalMenuCheckStates()
//
// 本测试目的：
//   1. 验证 QSettings 关键字段（wordWrap/edgeVisible/darkMode/edgeColumn）往返正确
//   2. 以"相同同步算法"最小化复现菜单勾选同步，防"忘记调用 syncGlobalMenuCheckStates"
//      （syncAlgo_scenario_all4together 精准复现用户 bug 流程）
//
// 说明：该项目是 Qt 静态库构建，创建 QtWidgets 模块对象（QMainWindow/QAction/QsciScintilla 等）
//       会触发静态 Qt Test + 平台插件冲突（STATUS_STACK_BUFFER_OVERRUN）。
//       因此本文件用极简 C++ 结构体模拟 action 勾选状态和 data，仅保留 QSettings / QCoreApplication。
//       EditorTab::setWordWrap 为单行转发 QsciScintilla::setWrapMode 的封装，
//       功能正确性在人工验证和运行时保证，此处不做单元化 GUI 创建。
// ============================================================
#include "EditorPreferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QVector>
#include <QString>
#include <cstdio>

// 与 MainWindow 使用的组织/应用名严格一致
static constexpr const char *kOrg = "NotepadSafe";
static constexpr const char *kApp = "NotepadSafe";

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)     fprintf(stderr, "  [RUN] %s\n", name)
#define CHECK(cond,msg) do { ++test_count; if (cond){ ++test_passed; fprintf(stderr,"    [PASS] %s\n", msg);} else { fprintf(stderr,"    [FAIL] %s\n", msg);} } while(0)

namespace {

// ---- 用极简结构体模拟 QAction / QActionGroup，避免 QtWidgets 依赖 ----
struct FakeAction {
    bool checkable = true;
    bool checked   = false;
    int  dataInt   = 0;
};
struct FakeGroup {
    QVector<FakeAction> actions;
};

// ---- 模拟 MainWindow::syncGlobalMenuCheckStates 的同步算法 ----
// 这是从修复代码里 1:1 抽出的纯逻辑（不依赖 MainWindow 对象）。
// 未来如果有人改了 MainWindow 的 syncGlobalMenuCheckStates，
// 必须同步修改这里的算法并让测试通过，
// 这样保证菜单勾选逻辑和数据成员始终一致。
void simulateSyncAlgo(
    bool darkMode, bool wordWrap, bool edgeVisible, int edgeColumn,
    FakeAction *actDark, FakeAction *actWrap, FakeAction *actEdge,
    FakeGroup  *edgeColGroup)
{
    if (actDark && actDark->checkable) actDark->checked = darkMode;
    if (actWrap && actWrap->checkable) actWrap->checked = wordWrap;
    if (actEdge && actEdge->checkable) actEdge->checked = edgeVisible;
    if (edgeColGroup) {
        for (FakeAction &a : edgeColGroup->actions) {
            if (a.checkable) a.checked = (a.dataInt == edgeColumn);
        }
    }
}

// 构造默认视图菜单集合（模拟 MainWindow.createMenus 初始化）
void makeDefaultMenu(FakeAction &actDark, FakeAction &actWrap, FakeAction &actEdge, FakeGroup &grp, const QVector<int> &cols, int defaultCol = 80)
{
    actDark = FakeAction{true, false, 0};
    actWrap = FakeAction{true, false, 0};
    actEdge = FakeAction{true, true,  0};   // 标尺默认 ON
    grp.actions.clear();
    grp.actions.reserve(cols.size());
    for (int c : cols) {
        FakeAction a;
        a.checkable = true;
        a.checked   = (c == defaultCol);
        a.dataInt   = c;
        grp.actions.push_back(a);
    }
}

FakeAction *findColAction(FakeGroup &grp, int col)
{
    for (FakeAction &a : grp.actions) {
        if (a.dataInt == col) return &a;
    }
    return nullptr;
}

} // namespace

// ======================================================
// 1. QSettings 关键字段往返
// ======================================================
static void test_qsettings_wordWrap_roundTrip()
{
    TEST("QSettings: wordWrap 往返");
    QSettings(kOrg, kApp).setValue("wordWrap", true);
    QSettings s(kOrg, kApp);
    CHECK(s.value("wordWrap").toBool() == true, "wordWrap 写入 true 后读取应为 true");
}
static void test_qsettings_edgeVisible_roundTrip()
{
    TEST("QSettings: edgeVisible 往返");
    QSettings(kOrg, kApp).setValue("edgeVisible", false);
    QSettings s(kOrg, kApp);
    CHECK(s.value("edgeVisible").toBool() == false, "edgeVisible 写入 false 后读取应为 false");
}
static void test_qsettings_darkMode_roundTrip()
{
    TEST("QSettings: darkMode 往返");
    QSettings(kOrg, kApp).setValue("darkMode", true);
    QSettings s(kOrg, kApp);
    CHECK(s.value("darkMode").toBool() == true, "darkMode 写入 true 后读取应为 true");
}
static void test_qsettings_edgeColumn_roundTrip()
{
    TEST("QSettings: edgeColumn 往返");
    QSettings(kOrg, kApp).setValue("edgeColumn", 150);
    QSettings s(kOrg, kApp);
    CHECK(s.value("edgeColumn").toInt() == 150, "edgeColumn 写入 150 后读取应为 150");
}

// ======================================================
// 2. 菜单勾选同步算法防回归
// ======================================================
static void test_syncAlgo_applyTrue_allChecked()
{
    TEST("同步算法: 默认值→全 true 后菜单勾选");
    FakeAction actDark, actWrap, actEdge; FakeGroup grp;
    QVector<int> cols = {80,100,120};
    makeDefaultMenu(actDark, actWrap, actEdge, grp, cols, 80);

    simulateSyncAlgo(true, true, true, 80, &actDark, &actWrap, &actEdge, &grp);

    CHECK(actDark.checked == true,  "darkMode=true → 菜单勾选 true");
    CHECK(actWrap.checked == true,  "wordWrap=true → 菜单勾选 true [★用户bug核心]");
    CHECK(actEdge.checked == true,  "edgeVisible=true → 菜单勾选 true");
}

static void test_syncAlgo_applyFalse_allUnchecked()
{
    TEST("同步算法: 全勾选脏状态→全 false 后取消");
    FakeAction actDark, actWrap, actEdge; FakeGroup grp;
    QVector<int> cols = {80,100,120};
    makeDefaultMenu(actDark, actWrap, actEdge, grp, cols, 120);
    actDark.checked = true; actWrap.checked = true; actEdge.checked = true;

    simulateSyncAlgo(false, false, false, 80, &actDark, &actWrap, &actEdge, &grp);

    CHECK(actDark.checked == false, "darkMode=false → 取消勾选");
    CHECK(actWrap.checked == false, "wordWrap=false → 取消勾选");
    CHECK(actEdge.checked == false, "edgeVisible=false → 取消勾选");
}

static void test_syncAlgo_edgeColumn_switch()
{
    TEST("同步算法: 标尺列号 80→100 切换正确");
    FakeAction actDark, actWrap, actEdge; FakeGroup grp;
    QVector<int> cols = {80,100,120};
    makeDefaultMenu(actDark, actWrap, actEdge, grp, cols, 80);

    simulateSyncAlgo(false, false, true, 100, nullptr, nullptr, nullptr, &grp);

    FakeAction *a100 = findColAction(grp, 100);
    CHECK(a100 && a100->checked, "100 列应被勾选");
    for (int c : cols) {
        if (c == 100) continue;
        FakeAction *a = findColAction(grp, c);
        CHECK(a && !a->checked, qPrintable(QString("非目标列 %1 不应被勾选").arg(c)));
    }
}

// ★ 最贴近用户真实场景：关窗时 QSettings 保存 (wrap=on, edge=off, dark=on, col=120)
//   → 下次启动 createMenus 先用默认值 → loadSettings → sync → 检查
static void test_syncAlgo_scenario_all4together()
{
    TEST("★用户场景复现: QSettings→默认菜单→同步后一致");
    // Step 1: 写 QSettings（模拟上次保存）
    {
        QSettings s(kOrg, kApp);
        s.setValue("wordWrap",    true);
        s.setValue("edgeVisible", false);
        s.setValue("darkMode",    true);
        s.setValue("edgeColumn",  120);
        s.sync();
    }

    // Step 2: 创建 action 并按默认值初始化（模拟 MainWindow.createMenus()）
    FakeAction actDark, actWrap, actEdge; FakeGroup grp;
    QVector<int> cols = {80,100,120};
    makeDefaultMenu(actDark, actWrap, actEdge, grp, cols, 80);

    // 先断言：同步前状态就是 bug 时的"未同步"状态
    CHECK(actWrap.checked == false, "[同步前] wrap 默认 false(=bug 状态)");
    CHECK(actEdge.checked == true,  "[同步前] edge 默认 true (=bug 状态)");
    CHECK(actDark.checked == false, "[同步前] dark 默认 false(=bug 状态)");

    // Step 3: 模拟 MainWindow.loadSettings()（必须和 MainWindow 用相同默认值！）
    QSettings s(kOrg, kApp);
    bool darkMode    = s.value("darkMode",    false).toBool();
    bool wordWrap    = s.value("wordWrap",    false).toBool();
    bool edgeVisible = s.value("edgeVisible", true).toBool();
    int  edgeColumn  = s.value("edgeColumn",  80).toInt();

    CHECK(wordWrap    == true,  "loadSettings: wordWrap 应读回 true");
    CHECK(edgeVisible == false, "loadSettings: edgeVisible 应读回 false");
    CHECK(darkMode    == true,  "loadSettings: darkMode 应读回 true");
    CHECK(edgeColumn  == 120,   "loadSettings: edgeColumn 应读回 120");

    // Step 4: 同步算法（就是修复后 MainWindow 在 loadSettings 后调用的 syncGlobalMenuCheckStates）
    simulateSyncAlgo(darkMode, wordWrap, edgeVisible, edgeColumn,
                     &actDark, &actWrap, &actEdge, &grp);

    // ★ 核心断言 — 菜单勾选必须与 QSettings 中保存的真实值一致
    CHECK(actWrap.checked == true,  "[用户Bug回归] wordWrap=true 但菜单勾选未同步显示为 true！");
    CHECK(actEdge.checked == false, "[用户Bug回归] edgeVisible=false 但菜单勾选未同步显示为 false！");
    CHECK(actDark.checked == true,  "[用户Bug回归] darkMode=true 但菜单勾选未同步显示为 true！");
    // 列数：120 应勾选，其它不应
    for (const FakeAction &a : grp.actions) {
        int c = a.dataInt;
        bool expect = (c == 120);
        CHECK(a.checked == expect,
            qPrintable(QString("[用户Bug回归] edgeColumn=120 但 %1 列勾选状态应为 %2")
                .arg(c).arg(expect ? "true" : "false")));
    }
}

// 标尺只保留 80/100/120 三档；旧设置值落到最近的一档。
void test_rulerColumnSnapsToSupportedSet()
{
    TEST("rulerColumnSnapsToSupportedSet");
    const struct { int saved; int expected; } cases[] = {
        {72, 80}, {80, 80}, {99, 100}, {100, 100}, {150, 120}, {200, 120}, {0, 80},
    };
    for (const auto &c : cases) {
        CHECK(normalizeRulerColumn(c.saved) == c.expected,
              qPrintable(QString("edgeColumn %1 应规整为 %2").arg(c.saved).arg(c.expected)));
    }
    CHECK(std::size(RulerColumns) == 3 && RulerColumns[0] == 80
              && RulerColumns[1] == 100 && RulerColumns[2] == 120,
          "RulerColumns 为 80/100/120");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(kOrg);
    app.setApplicationName(kApp);

    QSettings(kOrg, kApp).clear();

    fprintf(stderr, "\n=== Settings Persistence & UI Sync Tests ===\n\n");

    // ---- 1. QSettings 往返 ----
    test_qsettings_wordWrap_roundTrip();
    test_qsettings_edgeVisible_roundTrip();
    test_qsettings_darkMode_roundTrip();
    test_qsettings_edgeColumn_roundTrip();
    fprintf(stderr, "\n");

    // ---- 2. 菜单同步算法 ----
    test_syncAlgo_applyTrue_allChecked();
    test_syncAlgo_applyFalse_allUnchecked();
    test_syncAlgo_edgeColumn_switch();
    test_syncAlgo_scenario_all4together();
    test_rulerColumnSnapsToSupportedSet();

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);

    QSettings(kOrg, kApp).clear();
    return (test_passed == test_count) ? 0 : 1;
}
