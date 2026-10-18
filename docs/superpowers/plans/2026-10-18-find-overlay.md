# 查找浮层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用右上角查找浮层替换旧的查找 / 替换窗口，加入普通文件的边输入查找与命中序号、按标签顺序的全部打开文件结果分组和最近搜索。

**Architecture:** `FindReplaceDialog` 改名并改基类为 `FindOverlay : QFrame`（文档标签控件的子控件），保留查询、导航、查找全部、替换与会话逻辑和控件 objectName；边输入查找独立为 `FindLiveSearch`；结果面板的多文件模型增加预建分组与状态。

**Tech Stack:** C++17，Qt 6.11.1 静态 /MT，QScintilla，QtTest GUI smoke。

**Spec:** `docs/superpowers/specs/2026-10-18-find-overlay-design.md`

## Global Constraints

- 保持 Qt 静态链接；不新增第三方依赖。
- 不新增设置项。
- MainWindow 聚合方法数 112 不得增加；EditorTab 方法预算最多 +1（新增 `searchCursor` 与 `selectSearchHit` 共计 2 个定义，若超出预算则在 `tools/check_architecture.ps1` 中按实际数提高并注明原因）。
- 用户可见文案用中文，沿用现有错误与状态文案。
- 控件 objectName 保持：`findPatternEdit`、`replaceTextEdit`、`regexSearchCheck`、`hexBytesCheck`、`caseSensitiveSearchCheck`、`wholeWordSearchCheck`、`wrapSearchCheck`、`regexAssistantToggle`、`regexAssistant`、`regexQueryHint`、`findNextButton`、`findPreviousButton`、`replaceButton`、`replaceAllButton`、`findAllButton`、`cancelSearchButton`、`closeSearchButton`、`findStatusLabel`、`searchScopeCombo`。新增：`findOverlay`、`replaceToggleButton`、`findOptionsToggle`、`searchHistoryButton`。

## Review Focus

1. 边输入的后台任务与显式“查找全部”在同一文档上冲突（`findAllL1Async` 在途时拒绝）——显式查找全部前必须先取消边输入任务，且不得出现“搜索任务未能启动”。
2. 边输入结果在文档编辑、切换标签、锁定后到达——必须按代次丢弃，不得高亮或选中错误文档。
3. 浮层在文档区缩放、切到账号页、主题切换后仍定位正确、可见性正确。
4. 全部打开文件搜索过程中新打开、关闭文档——预建分组不得错位，关闭的文档分组显示为已失效。
5. 窗口化只读大文件与十六进制视图上输入时不得启动任何扫描。

---

### Task 1: FindOverlay 外壳

**Files:** 改名 `src/search/FindReplaceDialog.{h,cpp}` → `src/search/FindOverlay.{h,cpp}`；改 `CMakeLists.txt`、`src/window/MainWindow*.{h,cpp}`（`m_findDialog` → `m_findOverlay`，父对象改为 `m_tabs`）、`src/shell/ThemeStyle.cpp`（`#findOverlay` 样式）、`tools/check_architecture.ps1`（文件名与预算条目）、所有引用的测试。Test: `tests/gui_smoke/smoke_find_replace.cpp`、`smoke_smallfixes_menu_ui.cpp`。

- [ ] 新测试 `overlayFloatsTopRightOfDocumentArea`：主窗口 Ctrl+F 后 `findOverlay` 可见、父控件是文档标签控件、右边距 ≤ 24 px、顶端位于标签栏之下；缩放窗口后仍满足。
- [ ] 新测试 `overlayKeysNavigateAndClose`：Enter 选中下一处、Shift+Enter 上一处、Alt+Enter 等同点击 `findAllButton`、Esc 隐藏浮层且焦点回到编辑器。
- [ ] 新测试 `overlayRowsExpandOnDemand`：默认替换框与选项行隐藏；点 `replaceToggleButton` 显示替换框；`showReplaceMode()` 直接显示；点 `findOptionsToggle` 显示 `wrapSearchCheck`；`hexBytesCheck` 仅十六进制视图时可见。
- [ ] 运行确认失败，实现：`class FindOverlay : public QFrame`，公开接口与旧类相同；`setAnchor` 不需要——构造时对 `parentWidget()` 安装事件过滤器，Resize 时重新定位；`keyPressEvent`/查找框事件过滤器处理四个按键；忙碌时 `findAllButton` 隐藏、`cancelSearchButton` 显示在同一位置。
- [ ] 旧用例全部改用 `FindOverlay`，`smoke_find_replace` 41/41 通过；架构检查通过；提交。

### Task 2: 搜索光标接口与 Ctrl+F 预填

**Files:** `src/editor/DocumentSearchSurface.h`、`src/editor/EditorTab.h`、`src/editor/EditorTabSearch.cpp`、`src/search/FindOverlay.cpp`。

**Interfaces — Produces:**
```cpp
struct DocumentSearchCursor { qint64 selectionStart = -1; qint64 selectionEnd = -1; QString prefill; };
virtual DocumentSearchCursor searchCursor() const = 0;          // 可编辑文本：UTF-8 字节；其余返回 -1 与选中文字（若有）
virtual void selectSearchHit(qint64 byteOffset, qint32 byteLength) = 0; // 选中并滚动可见，不移动焦点
```
- [ ] 测试 `ctrlFPrefillsSelectionOrWord`：选中单行文字 → 查找框为该文字；无选区时为光标处的词；多行选区保留上次查询。
- [ ] 测试 `selectSearchHitKeepsFocus`：查找框有焦点时调用后选区为该范围，焦点仍在查找框。
- [ ] 实现并通过；提交。

### Task 3: 边输入查找

**Files:** Create `src/search/FindLiveSearch.{h,cpp}`；改 `FindOverlay.cpp`、`CMakeLists.txt`。

**Interfaces — Produces:**
```cpp
class FindLiveSearch : public QObject {
public:
    struct Query { QString pattern; bool regex, caseSensitive, wholeWord; };
    explicit FindLiveSearch(QObject *parent);
    static bool eligible(DocumentSearchSurface *tab);   // !isL2Mode && !isHexPresentation && !isBinaryDocument && 可搜索
    void schedule(DocumentSearchSurface *tab, const Query &q, qint64 anchorByte); // 150 ms 去抖
    void cancel();                                       // 取消在途任务、清代次
    void clearHighlights();
    QString positionText(DocumentSearchSurface *tab) const; // “第 i / n 处”“第 i / 200000+ 处”“未找到匹配”，无结果时空
signals:
    void updated();
};
```
- [ ] 测试 `liveSearchHighlightsAndCountsInNormalFile`：输入 `timeout`（文档 4 处）→ 状态“第 1 / 4 处”，选区为光标后第一处，查找框保留焦点；Enter 后“第 2 / 4 处”。
- [ ] 测试 `liveSearchSkipsLargeAndHexDocuments`：窗口化只读大文件与十六进制视图上输入后无后台任务、无选区变化。
- [ ] 测试 `findAllAfterTypingStartsCleanly`：输入后立即 Alt+Enter → 结果面板有结果，状态不含“未能启动”。
- [ ] 测试 `staleLiveResultIsDropped`：输入后立刻切换标签，原文档无高亮、新文档选区不变。
- [ ] 实现：代次计数；在途时新请求只记待办，回调到达后若有待办再跑；`FindOverlay::onFindAll` 与 `setActiveTab` 先 `cancel()`；关闭浮层 `clearHighlights()`；导航完成后用 `searchCursor().selectionStart` 更新序号；文档 `contentRevision` 变化后下次导航前重新 `schedule`。通过；提交。

### Task 4: 安静提示

**Files:** `src/search/FindOverlay.cpp`。
- [ ] 测试 `encodedRegexShowsSubsetHint`：GBK 大文件、正则开 → `regexQueryHint` 文本含“子集”，工具提示说明原因，颜色与普通提示相同（灰）。
- [ ] 测试 `invalidRegexIsQuietAndBlocksFindAll`：无效正则 → 提示为错误原因，`findAllButton` 禁用，无弹窗。
- [ ] 实现：正则有效且为编码子集时显示“按 %1 子集匹配 ⓘ”；有效 UTF 正则不再显示“正则有效”而是空；错误灰字；去掉绿/红样式。通过；提交。

### Task 5: 全部打开文件按标签顺序分组

**Files:** `src/search/SearchResultsAggregateModel.h`、`src/search/SearchResultsDock.{h,cpp}`、`src/search/FindOverlay.cpp`。

**Interfaces — Produces:**
```cpp
enum class MultiTargetState { Waiting, Searching, Done, SkippedLocked };
void SearchResultsDock::beginMultiFileResults(const QString &pattern, const QVector<SearchResultTarget> &searchable, const QVector<SearchResultTarget> &locked);
void SearchResultsDock::setMultiFileTargetProgress(const SearchResultTarget &target, int percent);
```
- [ ] 测试 `allOpenGroupsFollowTabOrder`：三个标签（第一个为大文件），查找全部开始后立即三组且顺序与标签一致，第一组“等待”或“搜索中”；完成后顺序不变，每组显示“N 处”。
- [ ] 测试 `lockedTabShowsSkippedGroup`：锁定加密文档出现“已跳过：已锁定”分组，无子行。
- [ ] 测试 `allOpenSummaryCountsFinishedFiles`：进行中摘要含“已完成 i/n 个文件”。
- [ ] 实现：模型 `reserve(target, state)` 预建分组，`addL1`/`addModel`/`addPreview` 改为先按 key 填入已有分组；分组标题“内容”列显示状态文字；FindOverlay 开始时传两组目标，大文件进度转发到 `setMultiFileTargetProgress`。通过；提交。

### Task 6: 最近搜索

**Files:** `src/search/SearchResultsDock.{h,cpp}`、`src/search/FindOverlay.{h,cpp}`。
- [ ] 测试 `recentSearchRerunsQuery`：先后查找全部 `alpha`、`beta`，`searchHistoryButton` 菜单两项（新的在上，最多 5 项，重复查询去重）；选 `alpha` → 查找框为 `alpha`、结果为 alpha 的结果。
- [ ] 测试 `lockClearsRecentSearches`：锁定后菜单为空。
- [ ] 实现：FindOverlay 保存最近 5 个 `SearchQuery`；Dock 提供 `setRecentSearches(QStringList)` 与信号 `recentSearchChosen(int)`。通过；提交。

### Task 7: 文档与全量验证

- [ ] 更新 `FEATURE_DECISIONS.md`、`SEARCH_CURRENT_STATE.md`、`UI_FEATURE_MAP.md`、`DAILY_USE_CLOSEOUT.md`、`README.md`、`tests/gui_smoke/SEARCH_REPLACE_CONTRACT_MATRIX.md` 中的类名与现状。
- [ ] 运行核心 17 组与 GUI 套件（findreplace、smallfixes、large_readonly、secure_documents、tail_follow、search_* 等全部引用查找的套件），全部通过；提交。
