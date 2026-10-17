# Theme Tokens and New Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the native client the “封印” look from the design canvas: one token set driving all colors, a frameless window with a custom title bar, painted document tabs, a clickable status bar, a Ctrl+K command palette and one notice component — without changing document, save or security rules.

**Architecture:** New code lives in a new `src/shell/` folder compiled into `notepadsafe_app`; `MainWindow` only wires it (its line budget is not raised). Pure logic (tokens, hit-testing, tab paint spec, palette filtering) is separated from widgets so it gets core tests. Existing `QAction`s and `CommandRegistry` stay the single source of commands; the shell re-hosts them.

**Tech Stack:** C++17, Qt 6.11.1 Widgets static, QScintilla, Win32/DWM, QtTest.

**Spec:** Design canvas https://claude.ai/artifact/7HvSVHq5npFLxc2xNhdjgU (boards 主窗口、设计令牌与组件、状态栏弹出层), `docs/UI_FEATURE_MAP.md`, `docs/FEATURE_DECISIONS.md` §1, `docs/PRODUCT_GOALS.md` (界面技术).

**Prerequisite:** `docs/superpowers/plans/2026-10-17-remove-confirmed-features.md` completed (no split view, no presentation menu, no toolbar).

## Global Constraints

- Native Qt Widgets only; no WebView, Qt Quick or new third-party libraries.
- System fonts only: UI `Segoe UI Variable Text`, fallback `Segoe UI`, Chinese `Microsoft YaHei UI`; editor keeps Consolas 10 (font choice is “不做”).
- Light/dark tokens (exact):
  - Light: ground `#E6EAEE`, chrome `#F1F3F6`, paper `#FCFDFE`, raised `#FFFFFF`, ink `#18212C`, muted `#556171`, faint `#7F8A98`, line `#D5DBE2`, lineSoft `#E3E7EC`, hover `#E7EBF0`, selection `#D6E4F5`, focus `#2F6FEB`, seal `#0B6F6A`, sealSoft `#DCEFED`, exposed `#9A5205`, exposedSoft `#F8E9D6`, hit `#FCEBA4`, hitCurrent `#F2B33D`, danger `#A3271B`, dangerSoft `#FBE4E1`.
  - Dark: ground `#0F1318`, chrome `#151A21`, paper `#1A2029`, raised `#212833`, ink `#E3E8EF`, muted `#9AA5B4`, faint `#6F7A89`, line `#303945`, lineSoft `#252C36`, hover `#29313C`, selection `#263E5C`, focus `#4D7CE0`, seal `#4FC3B9`, sealSoft `#14312F`, exposed `#F2A54A`, exposedSoft `#3A2A14`, hit `#5A4A16`, hitCurrent `#B9861F`, danger `#F0776A`, dangerSoft `#3B1C1A`.
- Metrics (px at 100% DPI): title bar 40, tab 34, status bar 26, control 28, primary button 36; radius control 6, tab top 8, overlay 10, palette 12, pill 14.
- Windows high-contrast mode: no stylesheet, platform palette, editor uses palette colors (current behavior kept).
- Seal = encrypted/locked, exposed = unlocked session; these two colors appear only in the security pill, secure tab spine, secure gutter tint and the status security segment.
- The only new shortcut is Ctrl+K; every existing shortcut keeps working with the menu bar hidden.
- Copy in Simplified Chinese; reuse existing command texts (strip `&` mnemonics for display).
- Tabs stay in their own 34 px row under the title bar (VS Code-style). Moving tabs into the title bar waits for the accounts-as-tab plan, because the accounts and security-center workspaces currently replace the tab widget.
- Build/test rules as in the removal plan (isolated profile, `build-final`, offscreen GUI suites, architecture scripts green). Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- **Window maximized, then dragged from the title bar** → restores and follows the cursor; double-click title toggles maximize; Win+Arrow and Snap Layouts flyout on the maximize button still work. Test in Task 3 (hit-test incl. `HTMAXBUTTON`).
- **Per-monitor DPI change (150% → 100%) while running** → title bar, tabs and status bar heights scale; no clipped text. Test in Task 3 (metrics scaled by `devicePixelRatio`-independent logical px via layout check at two scale factors offscreen).
- **Switching appearance light ↔ dark with documents open, including a read-only large file and a hex view** → every editor surface repaints with the new paper/ink, search highlights use hit colors. Test in Task 2.
- **Command palette opened while a modal-free busy search runs or while locked** → disabled commands are listed but not triggerable; secure document titles are not shown while locked. Test in Task 7.
- **Alt pressed alone vs Alt+F / Alt+S** → Alt alone opens the main menu; Alt+letter opens the matching submenu as before (menu mnemonics). Test in Task 4.

## File Structure

| File | Responsibility |
|---|---|
| `src/shell/ThemeTokens.h/.cpp` | Token struct, `light()`, `dark()`, contrast helper |
| `src/shell/ThemeStyle.h/.cpp` | Tokens → application QSS, `QPalette`, QScintilla editor colors |
| `src/shell/WindowChrome.h/.cpp` | Frameless window: Win32 messages, DWM corners/Mica, pure `hitTest` |
| `src/shell/TitleBar.h/.cpp` | Menu button, command-center button, security pill, window buttons |
| `src/shell/DocumentTabBar.h/.cpp` | Painted `QTabBar` and `DocumentTabWidget` that installs it; pure `tabPaintSpec` |
| `src/shell/StatusSegments.h/.cpp` | Flat clickable status segments and their popups |
| `src/shell/CommandPaletteModel.h/.cpp` | Pure entry list + filtering |
| `src/shell/CommandPalette.h/.cpp` | Popup widget over the model |
| `src/shell/NoticeBar.h/.cpp` | Non-blocking notice with up to two actions |
| `src/window/MainWindow*.cpp` | Wiring only |
| `CMakeLists.txt` | Add `src/shell/*` to `notepadsafe_app`; add core tests `test_theme_tokens`, `test_shell_logic`, `test_command_palette_model` |
| `tools/check_architecture.ps1` | Ownership rule: `src/shell` may depend on Qt and `src/app` command types, not on `src/editor`/`src/vault` internals |

---

### Task 1: Theme tokens and style generation

**Files:** Create `src/shell/ThemeTokens.h/.cpp`, `src/shell/ThemeStyle.h/.cpp`, `tests/test_theme_tokens.cpp`; modify `CMakeLists.txt`.

**Interfaces:**
- Produces:
  - `struct ThemeTokens { QColor ground, chrome, paper, raised, ink, muted, faint, line, lineSoft, hover, selection, focus, seal, sealSoft, exposed, exposedSoft, hit, hitCurrent, danger, dangerSoft; int titleBar=40, tab=34, statusBar=26, control=28, primaryButton=36, radiusControl=6, radiusTab=8, radiusOverlay=10, radiusPalette=12, radiusPill=14; static ThemeTokens light(); static ThemeTokens dark(); };`
  - `double contrastRatio(const QColor &a, const QColor &b);` (WCAG relative luminance)
  - `QString applicationStyleSheet(const ThemeTokens &t);`, `QPalette applicationPalette(const ThemeTokens &t);`, `void applyEditorTheme(QsciScintilla *editor, const ThemeTokens &t, bool secureGutter);`

- [ ] **Step 1: Write failing tests** `tokenValuesMatchSpec` (every hex in Global Constraints, both themes), `textContrastMeetsAA` (`ink/paper`, `muted/paper`, `muted/chrome`, `seal/sealSoft`, `exposed/exposedSoft`, `danger/paper` ≥ 4.5 in both themes), `styleSheetUsesTokens` (light sheet contains `#FCFDFE` and `#2F6FEB`, contains no `#2d2d30` legacy color).
- [ ] **Step 2: Run** `build_incr.bat test_theme_tokens` + exe → FAIL (missing).
- [ ] **Step 3: Implement.** `applyEditorTheme` sets paper/ink, margins chrome/faint, caret line hover, selection, fold margin, and when `secureGutter` the line-number margin background `sealSoft`; lexer default paper follows `paper`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** — `feat(shell): add theme tokens`

### Task 2: Apply the theme across the app

**Files:** Modify `src/window/MainWindowCommands.cpp:735-830` (`applyDarkMode` → `applyTheme(const ThemeTokens&)`, delete legacy QSS), `src/editor/EditorSurfaceStyle.h` (`applyEditorSurfacePalette` delegates to `applyEditorTheme` when tokens are set), `src/editor/EditorTabCommands.cpp:540-565` (search indicator uses `hit` instead of `0x00B7FF`; current hit selection uses `hitCurrent`), `src/editor/WindowedReadOnlyProvider.cpp` and hex char pane (same paper/ink); test `tests/gui_smoke/smoke_smallfixes_menu_ui.cpp`.

**Interfaces:** Consumes Task 1. Produces `void MainWindow::applyTheme(const ThemeTokens &t)`; appearance mode resolution (system/light/dark) unchanged.

- [ ] **Step 1: Write failing test** `appearanceRepaintsEverySurface`: open a text tab, a 40 MiB read-only text tab and a small binary; switch to dark → each visible `QsciScintilla` has `paper() == ThemeTokens::dark().paper`; after Find All the indicator 8 fore color equals `hit`; switch to light → `ThemeTokens::light().paper`; with high contrast forced (existing test hook) → `qApp->styleSheet().isEmpty()`.
- [ ] **Step 2: Run offscreen** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; run `gui_smoke_large_readonly` and `gui_smoke_hex_accessibility` for regressions.
- [ ] **Step 5: Commit** — `feat(shell): drive all colors from theme tokens`

### Task 3: Frameless window and title bar

**Files:** Create `src/shell/WindowChrome.h/.cpp`, `src/shell/TitleBar.h/.cpp`, `tests/test_shell_logic.cpp`; modify `src/window/MainWindow.cpp` (`nativeEvent` delegates `WM_NCCALCSIZE`, `WM_NCHITTEST` to `WindowChrome`; title bar added above `m_workspaceStack` via a container central widget).

**Interfaces:**
- Produces: `enum class ChromeHit { Client, Caption, MaxButton, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };` and `ChromeHit WindowChrome::hitTest(QPoint local, QSize window, int resizeBorder, QRect captionArea, QRect maxButton, bool maximized);` (pure). `TitleBar` signals: `mainMenuRequested()`, `commandPaletteRequested()`, `securityPillClicked()`; slot `setSecurityState(SecurityPillState)` with `enum class SecurityPillState { Hidden, Unlocked, ExpiringToday, Locked };` text `已解锁` / `今天 HH:mm 到期` / `已锁定`, tooltip `本设备会话有效至 MM-dd HH:mm，点击立即锁定`.
- Window buttons: minimize, maximize/restore, close, 46 px wide each; close hover uses `danger` background with white glyph.

- [ ] **Step 1: Write failing tests** in `test_shell_logic.cpp`: `hitTestRegions` table — corners within 6 px → corner hits; top edge → `Top` unless `maximized` (then `Caption`); point in `maxButton` → `MaxButton`; point in captionArea but over an interactive child rect → `Client` (pass child rects as empty caption holes via `captionArea` minus children); body → `Client`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** chrome (DWM: `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND`; `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW` when the OS supports it, otherwise solid `ground`) and `TitleBar`. Security pill state comes from the existing security session (`hasActiveSession`, `expiresAtUtc`), refreshed on lock/unlock/expiry timer.
- [ ] **Step 4: Run** core test → PASS; GUI check `gui_smoke_startup_shell` offscreen; manual check on desktop: drag, double-click, snap flyout, resize edges, Win+Arrow (record result; 受阻 if no desktop).
- [ ] **Step 5: Commit** — `feat(shell): frameless window with title bar`

### Task 4: Main menu button replaces the menu bar

**Files:** Modify `src/window/MainWindow.cpp` (menu construction keeps the `QMenu` objects; `menuBar()->hide()`; every action from `CommandRegistry` also added to the window with `addAction` so shortcuts keep working), `src/shell/TitleBar.cpp`; test `smoke_smallfixes_menu_ui.cpp`.

**Interfaces:** Produces `QMenu *MainWindow::mainMenu()` — submenus in order 文件、编辑、搜索、视图、编码、语言、工具、帮助, then separator, 立即锁定, 设置 (disabled placeholder until the settings plan).

- [ ] **Step 1: Write failing test** `shortcutsWorkWithHiddenMenuBar`: menu bar not visible; `QTest::keyClick` Ctrl+S on a modified file saves; F3 finds next; Ctrl+R opens replace; Alt pressed and released alone shows `mainMenu()`; Alt+F shows the 文件 submenu; Ctrl+K emits `commandPaletteRequested`.
- [ ] **Step 2: Run offscreen** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; run `gui_smoke_smallfixes` fully.
- [ ] **Step 5: Commit** — `feat(shell): move menus behind the main menu button`

### Task 5: Painted document tabs

**Files:** Create `src/shell/DocumentTabBar.h/.cpp`; modify `src/window/MainWindow.cpp:110-125` (`m_tabs` becomes `DocumentTabWidget`), `MainWindow.h`; test in `test_shell_logic.cpp` and `gui_smoke_smallfixes`.

**Interfaces:**
- Produces: `struct TabVisualState { enum class Kind { Plain, Secure, ReadOnly, Recovery } kind; bool dirty; bool secureLocked; };`, `struct TabPaintSpec { QColor background, text, spine, icon; bool showDirtyDot; bool showClose; int fontWeight; };`, `TabPaintSpec tabPaintSpec(const TabVisualState &s, bool active, bool hovered, const ThemeTokens &t);` and `class DocumentTabWidget : public QTabWidget` (same API as `QTabWidget`, installs `DocumentTabBar`). `MainWindow` provides `TabVisualState visualStateFor(int index) const` from the `EditorTab`.
- Rules: active → background `paper`, text `ink`, weight 600; inactive → transparent, `muted`, 400; secure active → spine 2 px `exposed` when unlocked, `seal` when locked; dirty → dot shown and close hidden unless hovered; read-only icon = eye, secure icon = lock (icon color follows spine color).

- [ ] **Step 1: Write failing test** `tabPaintSpecRules` covering the rules above (8 rows).
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement**; context menu, drag-to-reorder, middle-click close and `WorkspaceRecoveryCoordinator`'s `setTabButton` calls keep working on the new bar.
- [ ] **Step 4: Run** core + `gui_smoke_recovery_issue` and `gui_smoke_smallfixes` offscreen → PASS.
- [ ] **Step 5: Commit** — `feat(shell): paint document tabs from tokens`

### Task 6: Clickable status bar

**Files:** Create `src/shell/StatusSegments.h/.cpp`; modify `src/window/MainWindowCommands.cpp:186-230` (status labels → segments) and `updateStatusBar`; test `gui_smoke_smallfixes`.

**Interfaces:**
- Produces: `class StatusSegment : public QToolButton` (flat, 26 px, padding 0 8, hover `hover`); segments: position, characters, security (only for secure docs and accounts), encoding, eol, language, hex INS/OVR (only editable hex), zoom.
- Popups reuse existing actions: encoding → `encoding.display.*` actions + submenu “转换编码另存” with `encoding.convert.*`; eol → `encoding.eol.*`; language → `language.*`; security → “立即锁定” or login; INS/OVR → `edit.hexInsertMode`.

- [ ] **Step 1: Write failing test** `statusSegmentsReuseCommandActions`: the encoding segment's popup contains exactly the `QAction` objects named `encoding.display.*` (pointer equality); for a plain text tab the INS segment is hidden; for a small binary in hex it is visible and toggling it flips `hexOverwriteMode()`.
- [ ] **Step 2–4:** run → FAIL, implement, run → PASS.
- [ ] **Step 5: Commit** — `feat(shell): clickable status segments`

### Task 7: Command palette (documents, commands, recent files)

**Files:** Create `src/shell/CommandPaletteModel.h/.cpp`, `src/shell/CommandPalette.h/.cpp`, `tests/test_command_palette_model.cpp`; modify `MainWindow` wiring (Ctrl+K, title bar button).

**Interfaces:**
- Produces: `struct PaletteEntry { enum class Group { OpenDocument, Command, Recent } group; QString title; QString subtitle; QString shortcut; bool enabled; std::function<void()> activate; };`, `class CommandPaletteModel : public QAbstractListModel { void setEntries(QVector<PaletteEntry>); void setQuery(const QString&); }`.
- Filtering: case-insensitive; `&` stripped; entries whose title starts with the query rank above substring matches; within a rank keep group order 已打开的文档 → 命令 → 最近打开; disabled commands listed but `activate` not called; empty query shows at most 5 per group.
- Locked secure documents appear as `已锁定文档` with no title text.
- Accounts in the palette and copying passwords belong to the accounts plan; this task leaves an `Account` group out.

- [ ] **Step 1: Write failing tests** `prefixBeatsSubstring`, `groupsKeepOrder`, `disabledEntryDoesNotActivate`, `mnemonicIsIgnored` (`保存(&S)` matches `保存`), `emptyQueryCapsGroups`.
- [ ] **Step 2–4:** run → FAIL, implement model then popup (width 620, top offset 64, radius 12, rows 44, Enter activates, Esc closes, ↑↓ moves), run → PASS; GUI check opening with Ctrl+K and running “查找” opens the find dialog.
- [ ] **Step 5: Commit** — `feat(shell): add command palette`

### Task 8: Notice bar for save failure and restore summary

**Files:** Create `src/shell/NoticeBar.h/.cpp`; modify `src/window/MainWindowDocuments.cpp:1135-1141` (plain save failure), `src/window/WorkspaceRecoveryCoordinator.cpp:455-477` (restore summary); test `gui_smoke_recovery_issue` and a new case in `gui_smoke_smallfixes`.

**Interfaces:**
- Produces: `void NoticeBar::show(NoticeKind kind, const QString &text, QVector<NoticeAction> actions, int autoHideMs);` with `enum class NoticeKind { Info, Error }`, `struct NoticeAction { QString label; std::function<void()> run; }`; one notice at a time, newest replaces older.
- Save failure text: `没有保存：<原因>。修改仍在，原文件未更改。` actions `重试`, `另存为…`; the modal `无法保存文件` box is removed for plain documents only (secure flows unchanged).
- Restore summary uses Info with the existing strings and `autoHideMs` 5000 (3000 when nothing unsaved); unresolved-items message stays until dismissed.

- [ ] **Step 1: Write failing test** `saveFailureShowsNoticeAndRetries`: make the target read-only, Ctrl+S → no `QMessageBox` appears, notice visible with two buttons, document still modified; clear read-only, click 重试 → saved, notice hidden.
- [ ] **Step 2–4:** run → FAIL, implement, run → PASS; rerun `gui_smoke_recovery_issue`.
- [ ] **Step 5: Commit** — `feat(shell): notice bar for save failures and restore`

### Task 9: Verification and documentation

- [ ] **Step 1:** Full build; all core tests; all GUI suites touched in Tasks 2–8 offscreen; both architecture scripts.
- [ ] **Step 2:** On a desktop (if available): J1 first-screen time with the new shell (30 cold starts, P95), window chrome checks from Task 3, light/dark screenshots of 主窗口 compared with the canvas.
- [ ] **Step 3:** Update `docs/UI_FEATURE_MAP.md` (mark implemented rows), `docs/EDIT_CURRENT_STATE.md` §5 and §7, `docs/ARCHITECTURE_ASSESSMENT.md` §5 step 1 status, `docs/DAILY_USE_CLOSEOUT.md` (build identity, results, 受阻 items).
- [ ] **Step 4: Commit** — `docs: record new shell`

## Not in this plan

Find overlay (search module plan), settings page, accounts workspace and palette account items, locked-tab unlock, go-to-line, Ctrl+F prefill, saved zoom — each follows in its own plan per ARCHITECTURE_ASSESSMENT §5.
