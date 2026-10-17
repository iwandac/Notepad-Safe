# Remove Confirmed Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the existing features that FEATURE_DECISIONS marked for removal, so each document has exactly one view and search/replace has no cross-file mutation path.

**Architecture:** Pure subtraction inside the current structure. No new abstractions: remove commands, menu entries, `EditorTab` modes, session fields and test cases; keep data compatibility (old sessions still load, existing secure binaries still open). This shrinks `EditorTab` before the later view split (ARCHITECTURE_ASSESSMENT step 3).

**Tech Stack:** C++17, Qt 6.11.1 static, QScintilla, QtTest, CMake/Ninja (`build-final`, Release, KDBX=ON, TESTS=ON).

**Spec:** `docs/FEATURE_DECISIONS.md` §2 “删除” and “简化”; behavior references in `docs/*_CURRENT_STATE.md` (each has a “已确认的变更（尚未实现）” section).

## Global Constraints

- Build only through `set NOTEPADSAFE_BUILD_DIR=build-final` + `build_incr.bat <target>`; single job if the machine is shared (`set NOTEPADSAFE_BUILD_JOBS=1`).
- Run tests with an isolated profile: `NOTEPADSAFE_DATADIR` and `TEMP/TMP` pointing at a fresh scratch folder; never the user's profile.
- GUI smoke suites use `QApplication`; run them with `QT_QPA_PLATFORM=offscreen`. A failure that only reproduces offscreen is recorded as “受阻（需桌面）”, not as pass or product bug.
- Do not raise any budget in `tools/check_architecture.ps1`; removals must keep it green.
- Old `session.json` files written before this change must still load: unknown or removed keys are ignored, never an error.
- Existing encrypted binary documents (`.nsafe` with binary payload) must still open, edit, save (Ctrl+S) and Save As plain.
- User-visible copy is Simplified Chinese; reuse existing wording where one exists.
- Each task ends with `git diff --check` clean and one commit; commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- **Old session with `"splitView": true`** → app restores the tab as a single view, rewrites the manifest without split keys. Test in Task 1.
- **Old session with a binary tab saved as `"presentation": "text"`** → tab restores in Hex, bytes unchanged, no error page. Test in Task 2.
- **Large read-only text file viewed as Hex via the tab menu, then app restarted** → restores in Hex at the same byte offset (windowed text docs keep both presentations). Test in Task 2.
- **Find dialog left in “全部打开文件” scope with replace visible when the user switches tabs** → Replace and Replace All stay disabled, no stale enablement. Test in Task 3.
- **Saved `edgeColumn=72` or `150` from an older settings file** → snaps to 80 / 120 and the matching menu item is checked. Test in Task 5.

## File Structure

No files are created except tests. Files modified, by responsibility:

| Area | Files |
|---|---|
| Editor tab | `src/editor/EditorTab.h`, `src/editor/EditorTab.cpp`, `src/editor/EditorTabCommands.cpp`, `src/editor/EditorTabLargeDocument.cpp`, `src/editor/EditorTabLargeDocumentPersistence.cpp`, `src/editor/WindowedReadOnlyProvider.cpp/.h` |
| Session | `src/domain/SessionDocumentPort.h`, `src/session/SessionManager.cpp`, `src/app/DocumentSessionView.cpp`, `src/app/DocumentOpenWorkflow.cpp` |
| Commands & menus | `src/app/CommandRegistry.h/.cpp`, `src/domain/FeatureRegistry.cpp`, `src/window/MainWindow.h/.cpp`, `src/window/MainWindowCommands.cpp`, `src/window/MainWindowSession.cpp`, `src/window/DocumentTailActions.cpp` |
| Search | `src/search/FindReplaceDialog.cpp/.h`, `src/search/SearchNavigationRequest.cpp` |
| Secure save | `src/window/MainWindowDocuments.cpp`, `src/app/SaveAsDialog.cpp/.h` |
| Tests | `tests/test_session_preservation.cpp`, `tests/test_settings_ui.cpp`, `tests/test_architecture_contracts.cpp`, `tests/gui_smoke/smoke_smallfixes_menu_ui.cpp`, `smoke_secure_documents.cpp`, `smoke_find_replace.cpp`, `smoke_hex_readonly_search.cpp`, `smoke_large_readonly.cpp`, `smoke_document_product_language.cpp`, `smoke_readonly_search_source.cpp`, `smoke_recovery_issue.cpp`, `smoke_search_malformed_encoding.cpp`, `smoke_tail_follow.cpp` |
| Docs | the seven `docs/*_CURRENT_STATE.md`, `docs/UI_FEATURE_MAP.md`, `docs/FEATURE_LEDGER.md`, `tests/gui_smoke/*_MATRIX.md`, `docs/DAILY_USE_CLOSEOUT.md` |

---

### Task 1: Remove split view

**Files:**
- Modify: `src/editor/EditorTab.h:342-347,495-496` (split API and members), `src/editor/EditorTab.cpp:198,771,1002-1006,1155-1159,1177-1246,1250-1390` (split branches, `activeTextEditor`, `l1Page`, `canSplitView`, `splitOrientation`, `setSplitView`), `src/editor/EditorTabCommands.cpp` (1 reference), `src/editor/EditorTabLargeDocumentPersistence.cpp` (session capture/restore of split)
- Modify: `src/domain/SessionDocumentPort.h` (delete `splitView`, `splitOrientation`), `src/session/SessionManager.cpp` (stop writing/reading those keys), `src/app/DocumentSessionView.cpp` (stop restoring split)
- Modify: `src/window/MainWindow.cpp:1361-1396` (View › 分屏 menu), `src/window/MainWindow.h` (`m_actSplitHorizontal`, `m_actSplitVertical`), `src/window/MainWindowCommands.cpp` (split references), `src/app/CommandRegistry.h:splitAvailable/splitEnabled/splitVertical` and `.cpp:164-170`, `src/domain/FeatureRegistry.cpp` (all `view.split*` ids), `src/window/DocumentTailActions.cpp` (anchor `view.split` → insert before `view.symbols`)
- Test: `tests/test_session_preservation.cpp`, `tests/test_architecture_contracts.cpp`, `tests/gui_smoke/smoke_smallfixes_menu_ui.cpp`, `tests/gui_smoke/smoke_secure_documents.cpp`

**Interfaces:**
- Produces: `QsciScintilla *EditorTab::activeTextEditor() const` keeps its name and now always returns `m_editor` (later tasks and the view split rely on it). `QWidget *EditorTab::l1Page() const` returns `m_editor`.

- [ ] **Step 1: Write the failing tests**

In `tests/test_session_preservation.cpp` add `legacySplitFieldsAreIgnoredAndNotRewritten`: write a manifest containing one plain tab with `"splitView": true, "splitOrientation": "vertical"`, load it through `SessionManager`, save it back, and assert the saved JSON object for that tab has neither key while `filePath` and `cursorLine` survive.

In `tests/test_architecture_contracts.cpp` add a check that `FeatureRegistry::featureIdForCommand(id).isEmpty()` for `view.split`, `view.split.horizontal`, `view.split.vertical`.

- [ ] **Step 2: Run them to see them fail**

Run: `build_incr.bat test_session_preservation test_architecture_contracts`, then `build-final\test_session_preservation.exe` and `build-final\test_architecture_contracts.exe` with the isolated profile.
Expected: both new checks FAIL (keys rewritten; ids still known).

- [ ] **Step 3: Remove split view**

Delete the API, members and every `m_splitEditor`/`m_l1Splitter` branch; delete the menu, actions, command-context fields and feature ids; move the tail actions' insertion anchor to `view.symbols`. In `smoke_smallfixes_menu_ui.cpp` and `smoke_secure_documents.cpp` delete the cases whose subject is split view (find with `grep -nE "setSplitView|splitView|view\.split"`); do not weaken other assertions in the same cases—split such cases instead of deleting them.

- [ ] **Step 4: Verify**

Run: `build_incr.bat notepad-safe test_session_preservation test_architecture_contracts gui_smoke_smallfixes gui_smoke_secure_documents`, the two core tests, both GUI suites offscreen, `powershell -NoProfile -File tools/check_architecture.ps1`.
Expected: all pass; `grep -rnE "splitView|setSplitView|m_splitEditor|view\.split" src tests` returns nothing.

- [ ] **Step 5: Commit** — `refactor: remove split view`

---

### Task 2: Remove in-tab text/hex switching and the binary text presentation

**Files:**
- Modify: `src/window/MainWindow.cpp:1447-1485` (View › 呈现方式, Ctrl+Shift+H), `src/window/MainWindow.h:102,192-193`, `src/window/MainWindowCommands.cpp:448-461` (`toggleHexViewMode`), `:465-511` (presentation part of `syncGlobalMenuCheckStates`), `:1027-1060` (tab context “以文本/十六进制重新打开”), `src/app/CommandRegistry.h:37` and `.cpp:161-176` (`dualPresentation`, `view.presentation*`), `src/domain/FeatureRegistry.cpp:109-112,190-192`
- Modify: `src/editor/EditorTab.h:356` (`setViewModeL2` becomes private), `src/editor/EditorTab.cpp:223`, `src/editor/EditorTabLargeDocument.cpp` (delete the Text branch for `m_l2View`/binary and the decimal byte-offset gutter), `src/editor/WindowedReadOnlyProvider.cpp/.h` (binary content only allows `Presentation::Hex`)
- Modify: `src/editor/EditorTabLargeDocumentPersistence.cpp:108-117` (capture), `src/app/DocumentOpenWorkflow.cpp:57-72` and `src/app/DocumentSessionView.cpp:14-16` (restore)
- Test: `tests/gui_smoke/smoke_large_readonly.cpp` plus the suites listed in File Structure that call `setViewModeL2`, `view.presentation`, `presentText`/`presentHex`

**Interfaces:**
- Consumes: Task 1's single-view `EditorTab`.
- Produces: `bool EditorTab::canSwitchWindowedPresentation() const` — true only for windowed read-only, non-binary documents; `bool EditorTab::reopenWindowedPresentation(WindowedReadOnlyProvider::Presentation target, QString *error)` — re-opens the same path through the provider in `target` presentation, keeping the current top byte offset, clearing selection, turning tail-follow off. Rule for every other document: binary ⇒ Hex, text ⇒ Text, decided at open, never changed in place.

- [ ] **Step 1: Write the failing GUI tests** in `smoke_large_readonly.cpp`:
  - `binarySessionTextPresentationRestoresAsHex`: session with a 40 MiB binary fixture tab `"presentation": "text"` → after restore `tab->viewModeL2() == ViewMode::Hex`, fixture SHA-256 unchanged.
  - `windowedTextReopensAsHexAndRestoresAcrossSession`: open a 40 MiB text fixture, scroll to offset 20 MiB, `reopenWindowedPresentation(Hex)` → presentation Hex, visible top offset within one row of 20 MiB; capture and restore the session → still Hex at the same offset.
  - `noInTabPresentationCommandsRemain`: no `QAction` with object name starting `view.presentation`; tab context menu for a small binary has no enabled presentation action.

- [ ] **Step 2: Run to see them fail** — `build_incr.bat gui_smoke_large_readonly` then run offscreen. Expected: FAIL (restores Text; method missing; actions present).

- [ ] **Step 3: Implement** the removals and the two `EditorTab` methods. Context menu keeps one action, text “以十六进制查看” or “以文本查看” by current presentation, enabled only when `canSwitchWindowedPresentation()`. Session capture writes `"presentation": "hex"` only for windowed text docs shown as Hex; restore ignores `"text"` for binary content. Update the other listed GUI suites: delete cases about binary Text presentation; port cases about large-text Hex to `reopenWindowedPresentation`.

- [ ] **Step 4: Verify** — build `notepad-safe` and every touched GUI suite, run them offscreen, run both architecture scripts. Expected: pass; `grep -rnE "toggleHexViewMode|view\.presentation|dualPresentation" src` empty.

- [ ] **Step 5: Commit** — `refactor: let content decide the document view`

---

### Task 3: Remove cross-file Replace All and all-open byte search

**Files:**
- Modify: `src/search/FindReplaceDialog.cpp:552-708` (`updateQueryUi`), `:815-947` (all-open branch of `onReplaceAll`), `:1105-1148` and `:415-459` (hex skip logic in all-open dispatch and `notifyTabOpened`), `src/search/SearchNavigationRequest.cpp:84-90` (hex filter stays as a defensive check only)
- Test: `tests/gui_smoke/smoke_find_replace.cpp`, `tests/gui_smoke/smoke_hex_readonly_search.cpp`

**Interfaces:**
- Produces: in “全部打开文件” scope the dialog only finds. Exact rules: `replaceButton` and `replaceAllButton` disabled; `replaceTextEdit` disabled; `hexBytesCheck` disabled and unchecked; when replace mode is shown the status line reads `全部打开文件范围只查找；替换只作用于当前文件。`

- [ ] **Step 1: Write the failing test** `tc_fr_all_open_scope_is_find_only` in `smoke_find_replace.cpp`: two editable text tabs plus one small binary in Hex; open Replace (Ctrl+R), pick scope index 1; assert the three controls are disabled, `hexBytesCheck` unchecked, status text equals the string above; switch active tab and assert the same; switch scope back to current file and assert Replace/Replace All enabled for the text tab.

- [ ] **Step 2: Run to see it fail** — build and run `gui_smoke_findreplace` offscreen with `-run tc_fr_all_open_scope_is_find_only`. Expected: FAIL (Replace All enabled).

- [ ] **Step 3: Implement** the rules; delete the all-open `onReplaceAll` branch and its message boxes (`确认全部打开文件替换`, `全部替换已回滚`, `无法执行全部替换` all-open variants). Delete `tc_fr_015_all_open_replace_transaction_and_skip_summary` and `tc_fr_012_hex_byte_all_open_filters_text_targets`; in `smoke_hex_readonly_search.cpp` delete the all-open byte cases only. Keep `tc_fr_010` and `tc_fr_013` (all-open find/navigation).

- [ ] **Step 4: Verify** — rebuild `notepad-safe gui_smoke_findreplace gui_smoke_hex_readonly_search`, run both suites offscreen. Expected: pass.

- [ ] **Step 5: Commit** — `refactor: make all-open search find-only`

---

### Task 4: Stop creating encrypted binary documents

**Files:**
- Modify: `src/window/MainWindowDocuments.cpp:1341-1420` (`saveAsFile`), `src/app/SaveAsDialog.h/.cpp` (`choose` gains an `allowSecure` flag)
- Test: `tests/gui_smoke/smoke_secure_documents.cpp`

**Interfaces:**
- Produces: `std::optional<SaveAsSelection> SaveAsDialog::choose(QWidget *parent, const QString &suggestion, DocumentPersistence defaultKind, bool allowSecure)`; `saveAsFile` passes `allowSecure = !tab->isBinaryDocument() || tab->isSecureDocument()`. If a binary plain document still reaches the secure branch, show `不能转换为安全文档` / `安全文档只保存文本。二进制文件请另存为普通文件。` and return false.

- [ ] **Step 1: Write the failing tests** in `smoke_secure_documents.cpp`:
  - `plainBinarySaveAsOffersNoSecureType`: open a small binary fixture, invoke Save As, assert the type combo contains only `未加密文件 (*)`.
  - `existingSecureBinaryStillSavesAndExports`: create a secure-binary `.nsafe` in the test through `SecureDocumentService::saveAsSecureBinary` (the API this task does not remove), open it, edit one byte, Ctrl+S, reopen → byte changed; Save As plain → file bytes equal payload.

- [ ] **Step 2: Run to see the first fail** — build `gui_smoke_secure_documents`, run offscreen. Expected: first FAIL, second PASS.

- [ ] **Step 3: Implement** the flag and guard.

- [ ] **Step 4: Verify** — rerun the suite and `test_security_identity`. Expected: pass.

- [ ] **Step 5: Commit** — `refactor: keep encrypted documents text-only for new files`

---

### Task 5: Remove transpose, split-by-width, toolbar; simplify ruler

**Files:**
- Modify: `src/window/MainWindow.cpp:1113-1116,1141-1144` (two edit actions), `:1487-1506` (ruler columns), `:1546-1553` (toolbar visibility action), `src/editor/EditorTab.h/.cpp` and `EditorTabCommands.cpp:124-129,163-178` (`transposeLine`, `splitSelectedLines`), `src/window/MainWindowCommands.cpp:145-186` (toolbar creation) and `:790-794` (toolbar stylesheet), `src/window/MainWindowSession.cpp:219,251-252` (toolbar visibility load/save), `src/domain/FeatureRegistry.cpp` (`edit.line.transpose`, `edit.lines.split`, `view.toolbar`)
- Create: `src/window/EditorPreferences.h`
- Test: `tests/test_settings_ui.cpp`, `tests/gui_smoke/smoke_smallfixes_menu_ui.cpp`

**Interfaces:**
- Produces: `inline constexpr int RulerColumns[] = {80, 100, 120};` and `int normalizeRulerColumn(int saved)` in `src/window/EditorPreferences.h`: returns 80 for `saved < 90`, 100 for `saved < 110`, else 120. `loadSettings` applies it to `edgeColumn`.

- [ ] **Step 1: Write the failing tests**
  - `test_settings_ui.cpp`: `rulerColumnSnapsToSupportedSet` with table `{72→80, 80→80, 99→100, 100→100, 150→120, 200→120, 0→80}`.
  - `smoke_smallfixes_menu_ui.cpp`: `removedCommandsAreAbsent` — no `QAction` named `edit.line.transpose`, `edit.lines.split`, `view.toolbar`; no `QToolBar` named `MainToolBar`; ruler menu has exactly three column actions.

- [ ] **Step 2: Run to see them fail.**

- [ ] **Step 3: Implement** the removals and `normalizeRulerColumn`. Saved `toolbarVisible` is ignored on load and no longer written. Delete toolbar cases in `smoke_smallfixes_menu_ui.cpp`.

- [ ] **Step 4: Verify** — build `notepad-safe test_settings_ui gui_smoke_smallfixes`, run; architecture scripts green.

- [ ] **Step 5: Commit** — `refactor: drop rarely used edit commands and the toolbar`

---

### Task 6: Full verification and documentation

**Files:**
- Modify: the seven `docs/*_CURRENT_STATE.md` (move each completed “已确认的变更” item into the body as current behavior and delete it from that section), `docs/UI_FEATURE_MAP.md`, `docs/FEATURE_LEDGER.md`, `tests/gui_smoke/GUI_TEST_COVERAGE_MATRIX.md`, `HEX_EDITOR_CONTRACT_MATRIX.md`, `SEARCH_REPLACE_CONTRACT_MATRIX.md`, `docs/DAILY_USE_CLOSEOUT.md`, `docs/FEATURE_DECISIONS.md` (status line: which removals are done)

- [ ] **Step 1: Build everything** — `build_incr.bat` with no target (all, TESTS=ON). Expected: success.
- [ ] **Step 2: Run all core tests** (the 10 existing plus `test_session_preservation`, `test_settings_ui`, `test_architecture_contracts`) with the isolated profile. Expected: all pass.
- [ ] **Step 3: Run every GUI suite touched in Tasks 1–5 offscreen.** Record pass / fail / 受阻 per suite.
- [ ] **Step 4: Update docs** as listed; record build identity (exe SHA-256), commands and results in `DAILY_USE_CLOSEOUT.md`.
- [ ] **Step 5: Commit** — `docs: record removal of split view, in-tab view switching and cross-file replace`
