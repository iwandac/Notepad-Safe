# L2 模式大文件编辑器 · GUI 冒烟测试用例表（按真实菜单树 1:1 对齐 v2）

> **文档原则（用户强制要求）**
> 1. **不做大文件压测**（删除所有 "P 类压力/容量/10万行"TC，聚焦真实用户点击的编辑/搜索/替换/菜单/快捷键）
> 2. **1:1 对齐软件真实菜单树 + 31 条快捷键 + 菜单条目全部覆盖**：每个顶层菜单、每个子菜单项、每条快捷键至少 1 条 TC（含 L2 模式下的行为）
> 3. **路径矩阵 = 真实菜单/快捷键链路（路径 A）+ EditorTab API（路径 B）**：用户真实操作全是路径 A（MainWindow 的 QAction 触发 / QShortcut 按键），L2 下目前大量路径 A **根本没有转发到 L2**（直接操作 Scintilla），本 TC 表显式标注每条"已知会 FAIL 的具体代码证据"
> 4. **优先级分级**：P0 = 用户真实会点到的 L2 编辑主链路（任何 FAIL 不可交付）；P1 = 显示/视图一致性；P2 = 语法高亮等 L2 本来不适用但菜单仍可选的"至少不崩溃/不改字节流"
> 5. **SmokeBase 能力映射**：每条 TC 写清楚需要哪些新 Helper 接口（保证不是"口头 TC"）

**代码溯源（MainWindow.cpp L160-L420）：**
- 菜单树：`文件 → 编辑 → 搜索 → 语法 → 书签 → 视图 → 编码`（共 7 个顶层菜单 + 未枚举帮助菜单）
- 快捷键总数：31 条（见下表 `Shortcut` 列）
- **重大已知 L2 失效证据（预填 FAIL 原因）：**
  - ① [MainWindow undo/redo/cut/copy/paste/selectAll](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L201-L237)：lambda 全部直接调 `t->editor()->undo()`（Scintilla）**跳过 L2**
  - ② [EditorTab::selectAll](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1288-L1296) 的 L2 分支：只 `gotoByteOffset(INT_MAX)`（光标移末尾）**根本没选** → 对应「全选无反应」
  - ③ [EditorTab::cut](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1316-L1324) 的 L2 分支：只调 `copy()` **不删选中字节** → 对应「无法剪切」
  - ④ [EditorTab 行操作](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L489-L572) duplicateLine / deleteLine / moveLineUp/Down / sortAsc/Desc（6 项）：100% Scintilla 代码 **L2 全空**；且 **L2 下 sortAsc/Desc 本身不适配超大文件，应直接禁用菜单项（enabled=false 变灰）**（D001/D002 强制）
  - ⑤ [EditorTab 书签](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L576-L621)（4 项）：同上，100% marker 操作 Scintilla **L2 全空**
  - ⑥ [MainWindow zoom](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L774-L787)：直接 `t->editor()->zoomIn()` 绕过 EditorTab
  - ⑦ [EditorTab applyEdge/长行标尺](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L715-L736)：只对 Scintilla，**L2 无效**
  - ⑧ 语法 / applySyntax：仅 Scintilla lexer（L2 无效，P2 级要求"不崩+不改字节流"）
  - ⑨ **自动换行非首显示行光标/拖选不可用（用户原话实锤新增）**：物理 1 逻辑行占 N 条显示行时，displayLines[1..N-1] 点击=不生效（光标仍跳回显示行首起点/或不变）；拖选从非首显示行起/终=无法形成选区 → W007/C012/C013/S009/S010 新增 TC 必须数值断言
  - ⑩ **自动换行下滚轮一跳跳到下一物理行，中间 90% 显示行不可见（用户原话实锤新增）**：视口高=10 display lines，当前超长行占 100 display lines，滚轮一格（或一页）前进应该前进 scrollStep（3 行）/ 一页（10 行）display lines，但实际被重置 cursor 到 startOfLine(逻辑行+1) → W008 精确断言（topByte 增量 ≈ scrollStep×displayStride，**绝对不能 = startOfLine(nextLogicalLine)**）
  - ⑪ **Enter 键「只多一个不可见字符，不真换行」（用户原话 + 代码联合实锤）**：代码 [VirtualFileView keyPress L1535-1538](file:///F:/projects/credentials/notepad-safe/VirtualFileView.cpp#L1535-L1538) 确实执行了 `insertAtCursor("\n")`（字节流里会真的多 0x0A），但 LineIndex / renderRange / computeLines **没把 patch 新增的 \n 当作新的分行点**（或显示层没画换行第二行内容，光标还在原位看不见）——这不是插入 bug，是 **patch 后分行/渲染的一致性 bug**，对应 E004-VISUAL：字节流 0x0A + 行数+1 + 视口中新第二行内容真的被绘制在新 Y 位置，三条必须同时成立
  - ⑫ **替换菜单显示不一致（用户原话实锤新增）**：菜单显示 `"替换(&R)..."`（Alt+S 打开搜索菜单后 R 是访问键），但 `replaceAct->setShortcut(QKeySequence::Replace)` 在 Windows 下是 `Ctrl+H`，用户看到括号里 &R 自然联想全局快捷键是 Ctrl+R（与 Notepad++ 一致），造成认知偏差。强制要求：**全局快捷键改为 Ctrl+R，括号保持 (&R)，二者一致 → SR02-COMFORT 断言**
  - ⑬ **编码「转为 XXX」菜单只改标志位，未做真实字节流 re-encode（用户原话实锤 + 代码铁证）**：[MainWindow convertEncodingFromAction](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L943-L970) 仅做了 `tab->setEncoding(enc)` + `setModified(true)` + 弹提示，**完全没有**：读虚拟字节流全量 → 按原 encoding 解码为 QString → 按目标 encoding 重新编码 → 全量回写 replaceBytes（而大文件 L2 这种"全量读-转-写"本身就不适合）。这会导致：① 切换 UTF-16LE 后保存，写了 UTF-16LE BOM 但正文字节仍是 UTF-8 → 直接乱码。② L2 超大文件根本无法一次性做"全量转码"。强制要求：**L2 模式下「转为...」5 项 + 行升序/降序 2 项全部禁用（enabled=false 变灰，点不动）**（D003/D004 强制）；L1 下则在真正实现转码前也先禁用，避免产生假转码+乱码。
  - ⑭ **16 进制编辑功能存在（代码有）但原 TC 只测了切换未测"编辑"**：[MainWindow toggleHexViewMode](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L835-L847) 确实实现了 ViewMode::Text ↔ ViewMode::Hex 切换（仅 L2 生效，L1 拒绝），但原 TC VW05-A/B/C 仅覆盖了切换/不脏/L1 拒绝，完全没覆盖光标定位/改字节/Hex 搜索/保存 round-trip → 新增 §6.1 Hex01~Hex10 专项

---

## 0. 回归测试（锁定上一轮已修 5 条 — 保留不删；全部 P0）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 溯源/代码 | SmokeBase 映射 |
|---|---|---|---|---|---|---|
| R001 | OFF 模式：先下行插 MARK → 再上行插 MARK → 下行 MARK 字节偏移无偏移 | openL2 T_SMALL_20L → goto L10 插 `__MARK_BELOW__` → goto L5 插 `__MARK_ABOVE__` → read L9/L10/L11 起始 32B | L10 起始=`__MARK_BELOW__L10:...`；下一行 start-上一行 end = 行长+MARK 精确一致 | **P0** | [PatchList::physicallyInsertPatch shift](file:///F:/projects/credentials/notepad-safe/PatchList.cpp#L115-L132) | openL2+gotoLine+insertText+lineStartByte |
| R002 | 单步替换覆盖（不插在旁边） | openL2 T_REPLACE_3_PAT → findNext(OLD) → replaceCurrent(NEW) → read wider-6..NEW.len+6 | wider = prefix+NEW+suffix；OLD 字节在 wider 中出现次数=0 | **P0** | [EditorTab::replaceCurrent L2](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1666-L1686) · [replaceBytes newData/oldSnapshot 实参顺序](file:///F:/projects/credentials/notepad-safe/VirtualFileView.cpp#L1818-L1824) | findNext+replaceCurrent+readRange |
| R003 | 替换全部 N=3；OLD 残留=0；NEW=3 | openL2 T_REPLACE_3_PAT → OLD.count=3 → replaceAll(OLD→NEW).n=3 → OLD.count=0 且 NEW.count=3 | n=3；OLD=0；NEW=3 | **P0** | [LargeFileSearch outReplacedCount](file:///F:/projects/credentials/notepad-safe/LargeFileSearch.cpp#L683-L819) | replaceAll+countOccurrences |
| R004 | findNext forward(wrap=off) 3 次偏移 = 430 < 2056 < 3682 | openL2 T_REPLACE_3_PAT → 10 次 findNext wrap=off → offsets | offsets.size=3；严格递增；第 4 次=false | **P0** | [EditorTab::findNext m_lastL2Byte](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1661-L1665) | findNext×N+offsets |
| R005 | readByteRange 读 patch 尾部：end clamped to virtualSize | openL2 T_SMALL_20L → EOF 插 100B '@' → readByteRange(vs-150, vs) | 后 100B='@'×100；前 50B=原尾；size=150；无截断负值 | **P0** | [readByteRange endClamp vs](file:///F:/projects/credentials/notepad-safe/VirtualFileView.cpp#L842-L846) | insertAtCursorDirect+readRange(vs-150, vs) |

---

## 1. 文件菜单（File · 顶层 6 条目 + 1 子菜单）

| TC ID | 标题（菜单原文/快捷键） | 步骤 | 预期 | 优先级 | L2 已知 FAIL？ | SmokeBase 映射 |
|---|---|---|---|---|---|---|
| F001-A | 新建 Ctrl+N（路径 A 真实快捷键） | **MainWindow 实例**（非 EditorTab 裸部件）openL2 small → `QTest::keyClick(MainWindow, Qt::Key_N, Qt::ControlModifier)` → tabCount++ | tabCount = old+1；new tab.title=`"未命名 2"`；isUntitled=true；L1 empty | **P0** | - | smoke 需要 MainWindow 级：MainWindow 实例化 + `keySeq(target=MainWindow, Ctrl+N)` + `tabCount()` |
| F001-B | 新建（菜单点击路径 A） | MainWindow → 触发 `fileMenu->actions()[0]->trigger()` → tabCount++ | 同 F001-A | **P1** | - | `actionTrigger(action)` + tabCount |
| F002-A | 打开 Ctrl+O（路径 A 真实快捷键 + FileDialog mock 返回 fixturePath） | MainWindow openL2 small → keySeq Ctrl+O → mock 对话框返回 T_SMALL_20L 路径 → tab 打开成功 | tabCount+1；isL2=true；virtualSize=fixture.size；字节流对 | **P0** | - | mockFileDialogReturn(path) + keySeq Ctrl+O + openL2 assertions |
| F002-B | 最近打开子菜单（菜单「最近打开」） | open 3 个文件（a,b,c）→ rebuildRecent → 最近打开 menu actions[0..2] text 含 (a,b,c) 路径；点最近打开[0] → 打开对应 tab | actions 文本正确；点击后 tab 打开成功 | **P1** | - | `recentMenuActionsTexts()` + `triggerRecentMenu(idx)` |
| F003-A | 保存 Ctrl+S（路径 A 快捷键） | openL2 small → 在 line 5 插 "ABC"（saveNeeded=true）→ 写 snapshot vsBefore bytes + vs bytes diff → **MainWindow Ctrl+S keySeq** → saveNeeded=false；disk bytes = vs bytes；标题无 `*` | 磁盘字节流 === `readByteRange(0, vs)` 全量逐字节 | **P0** | ⚠️ 需先保证 MainWindow saveFile 的 L2 链路完整（验证通过后此条 PASS） | MainWindow 实例 + keySeq Ctrl+S + diskBytesEquals(readByteRangeFull) + saveNeeded(false) |
| F003-B | 保存（菜单点击） | 同 F003-A，用 `saveAct->trigger()` 代替 keySeq | 同 F003-A | **P1** | - | `actionTrigger(saveAct)` + same assertions |
| F004-A | 另存为 Ctrl+Shift+S（路径 A 快捷键 + mock 另存为对话框返回 newPath） | openL2 small → edit 3 处 → mock FileDialog return newPath → keySeq(Ctrl+Shift+S) → ① newPath disk bytes === vs bytes；② originalPath 保持旧内容；③ title 更新为 newPath；④ saveNeeded=false | 三条断言全过 | **P0** | - | mockFileDialog(path, SaveAs) + keySeq Ctrl+Shift+S + disk diff 2 路径 + titleEndsWith(newPath) |
| F004-B | 另存为（菜单点击） | 同 F004-A，用 saveAsAct->trigger() | 同 F004-A | **P1** | - | actionTrigger(saveAsAct) |
| F005 | 只读文件保存失败提示（文件 attrib=只读） | openL2 T_1KB_RO → 插 5B → Ctrl+S → ① statusBar 文本包含 "保存失败"；② 磁盘字节 === 编辑前 snapshot（不被改）；③ saveNeeded 仍 true；④ title 仍有 `*` | 四断言全过 | **P0** | ⚠️ 用户反馈「保存提示无法保存」之一（只读来源） | SetFileAttributes(readonly) + Ctrl+S + statusBarContains("保存失败") + diskIdentical(before snapshot) |
| F006-A | 退出 Ctrl+Q（路径 A 快捷键 + 修改文件时 prompt 取消 → 不退出） | openL2 small → 插 1B（saveNeeded=true）→ mock MessageBox Close=Cancel → keySeq(Ctrl+Q) → window still exists；saveNeeded=true；title 仍有 `*` | MainWindow.isVisible()=true | **P1** | - | mockCloseDialog(Cancel) + keySeq Ctrl+Q + windowVisible(true) |
| F006-B | 退出（菜单点击 + 保存后退出） | openL2 → edit → mock MessageBox Close=Save → exitAct trigger → window closed；disk saved | windowVisible=false；disk==vs | **P1** | - | mockCloseDialog(Save) + actionTrigger(exitAct) |
| F007 | 打开-编辑-保存-关闭-重新打开：字节流 100% round-trip 一致（闭环） | openL2 small(T_SMALL_20L) → insert MARK_BELOW(14B) at L10 + insert MARK_ABOVE(14B) at L5 → save → close tab → reopen path → read full vs snapshot (openL2 原始 + 两处插入 patch 的虚拟字节流完全相同)；undoStack.canUndo=false；cursor=0 | full byte diff identical | **P0** | ⚠️ L2 保存闭环核心 | open+2 inserts+save+close+reopen+fullDiff(snapshot) |
| F008 | 文件被外部修改：外部写 5B 到磁盘 → 焦点回窗口弹"文件已被外部修改"对话框；点"重新加载"→ 字节流==外部新内容；点"保持原样"→ 不改 | mock FileChanged prompt + choices：reload / keep；分别断言 | 对应断言 | **P1** | - | modifyFileOnDiskExternal() + mockReloadDialog(Choice) + bytes assertion |

---

## 2. 编辑菜单（Edit · 20 条目）
> **核心菜单：L2 目前 MainWindow Lambda 只操作 Scintilla（证据①）；EditorTab L2 selectAll/cut 有半成品 bug（证据②③）；行操作/排序 6 项全空（证据④）——共 14 条 P0 已知 FAIL**

### 2.1 撤销/重做/剪切/复制/粘贴/全选（6 项 + 每条含 双路径 = 12 TC）

| TC ID | 标题+快捷键 | 步骤（路径 A=MainWindow 快捷键；路径 B=EditorTab API） | 预期 | 优先级 | L2 已知 FAIL + 代码证据 | SmokeBase |
|---|---|---|---|---|---|---|
| ED01-A | 撤销 Ctrl+Z（路径 A MainWindow 真实 KeySeq） | MainWindow.openL2 small → 插 X 10B → snapshot_full() → 再插 Y 10B → vs = s+20 → **MainWindow Ctrl+Z keySeq** → vs = s+10；full === snapshot | vs=s+10；full==snapshot；saveNeeded 正确 | **P0** | ⚠️ **已知 FAIL 证据①**：[MainWindow.cpp L203](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L203) 只调 `t->editor()->undo()`（Scintilla），L2 下该 undo 不碰 VirtualFileView 的 patches_ / undoStack，**什么都不会发生**；路径 A 必须 FAIL（作为修复前的失败证据）；修完后必须变 PASS | MainWindow实例+keySeq Ctrl+Z + vs + fullDiff |
| ED01-B | 撤销 Ctrl+Z（路径 B EditorTab::undo()） | 同 ED01-A 但改为 `tab()->undo()` 直接 API | vs=s+10；full==snapshot | **P0** | 当前应 PASS（EditorTab 已分发） | `tab()->undo()` + same assertions |
| ED02-A | 重做 Ctrl+Y（路径 A MainWindow 真实 KeySeq） | 打开 → 插 X → Ctrl+Z（撤销）→ vs=s → **MainWindow Ctrl+Y keySeq** → vs=s+10 | vs=s+10；canRedo=false | **P0** | ⚠️ **已知 FAIL 证据①**：redoAct lambda 也是 `t->editor()->redo()`（Scintilla），L2 下路径 A 什么都不发生 | MainWindow实例+keySeq Ctrl+Y + vs |
| ED02-B | 重做 Ctrl+Y（路径 B EditorTab::redo()） | 同 ED02-A 改为 `tab()->redo()` | vs=s+10 | **P0** | 当前应 PASS | `tab()->redo()` |
| ED03-A | 剪切 Ctrl+X（路径 A MainWindow 真实 KeySeq） | L2 open small → dragSelect 400-800 byte（400B）→ saveSelectionBytes(S) → snapshot_full_before → **MainWindow Ctrl+X keySeq** → ① vs=-400；② readByteRange(400, vs_prev_800 的新位置) === snapshot 原 800..；③ QClipboard === S；④ saveNeeded=true | 4 条断言全过 | **P0** | ⚠️ **双重已知 FAIL**：证据① MainWindow cutAct lambda = `t->cut()`（这个 OK，因为 EditorTab 有 cut 方法），但证据③ [EditorTab::cut() L2](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1316-L1324) 只调 `copy()` **不删除选中字节** → vs 没变化 = 无法剪切。路径 A 必 FAIL。 | MainWindow实例+Ctrl+X + vs + clipboardBytes=S + readByteRange(400,new 位置) |
| ED03-B | 剪切（路径 B EditorTab::cut()） | 同 ED03-A 改为 `tab()->cut()` API | 同 ED03-A 4 断言 | **P0** | ⚠️ **已知 FAIL 证据③**：同"不删除"，路径 B 同样必 FAIL（EditorTab cut 本身 bug） | `tab()->cut()` |
| ED04-A | 复制 Ctrl+C（路径 A MainWindow 真实 KeySeq） | L2 open small → dragSelect 200-600 → saveSelectionBytes(S) → **MainWindow Ctrl+C keySeq** → ① vs 未变；② QClipboard === S | vs 不变；clipboard=S | **P0** | ⚠️ **已知 FAIL 证据①**：copyAct lambda = `t->copy()` 这个 OK；但需确认 MainWindow Lambda 确实调 `t->copy()` → **实际代码 MainWindow copy lambda = `t->copy()` OK**，不是 editor()，所以 4 条里只有 copy 路径 A 没问题，其余都崩（这里实际查代码确认是 `t->copy()`，所以 copy 路径 A 现在就应该过） | MainWindow实例+Ctrl+C + vs + clipboardBytes |
| ED04-B | 复制（路径 B EditorTab::copy()） | 同 ED04-A → `tab()->copy()` | 同 ED04-A | **P0** | 当前应 PASS | `tab()->copy()` |
| ED05-A | 粘贴 Ctrl+V（路径 A MainWindow 真实 KeySeq + clipboardBytes 预填） | L2 open small → cursor=1000 → snapshot before → setClipboard(500B 'P') → **MainWindow Ctrl+V keySeq** → ① vs=+500；② readByteRange(1000, 1500) = 'P'×500；③ cursor=1000+500；④ saveNeeded=true；⑤ undo 1 步回到 before | 5 断言全过 | **P0** | ⚠️ **已知 FAIL 证据①**：pasteAct lambda = `t->paste()`（OK，EditorTab 有实现），但需要确认真实 L2 pasteBytes 是否走通。当前 EditorTab L2 paste 有实现（toUtf8 + pasteBytes），路径 A 可能已经 OK。需要 smoke 实锤验证 | MainWindow实例+setClipboard+Ctrl+V + vs+cursor+readRange+undoOnce |
| ED05-B | 粘贴（路径 B EditorTab::paste()） | 同 ED05-A 改为 `tab()->paste()` | 同 ED05-A | **P0** | 当前应 OK（EditorTab 有实现） | `tab()->paste()` |
| ED05-C | 粘贴超大内容 >PASTE_LIMIT（L2 压力提示或仍插入成功）+ 内容正确 | setClipboard(2MB 'Q') → Ctrl+V（路径 A）→ vs+2MB；内容='Q'×2MB；spy(patchPressureWarn) == 收到（若实现） | 内容 100% 正确（优先保证）；信号其次 | **P0** | ⚠️ 用户：「大粘贴是否可用」 | same as ED05-A + signalSpy(QPatchPressureWarn) + large clipboard(2MB) |
| ED06-A | 全选 Ctrl+A（路径 A MainWindow 真实 KeySeq） | L2 open T_1KB（已知 vs=102_400）→ cursor=0 → 有选区=No → **MainWindow Ctrl+A keySeq** → ① selStart=0；② selEnd=102_400；③ selSize=102_400；④ readSelectionBytes() === readByteRange(0, vs) 逐字节 | 4 断言全过 | **P0** | ⚠️ **已知 FAIL（最大实锤，你原话「全选无反应」的根因）双重 bug：** 证据① MainWindow selectAllAct lambda = `t->selectAll()`（这个调对了 EditorTab），但证据② [EditorTab::selectAll L2](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1288-L1296) 只 `gotoByteOffset(INT_MAX)`（光移末尾，不选任何字节！）→ 路径 A/B 都 FAIL。修复后 selSize 必须 = vs | MainWindow实例 + Ctrl+A keySeq + selectionStart/End/Size + readSelectionBytes === readByteRangeFull |
| ED06-B | 全选（路径 B EditorTab::selectAll()） | 同 ED06-A 改为 `tab()->selectAll()` | 同 ED06-A | **P0** | ⚠️ **已知 FAIL 证据②**：同上，EditorTab selectAll 半成品 | `tab()->selectAll()` |

### 2.2 行操作（6 项：Ctrl+D / Ctrl+Shift+L / Alt+Up / Alt+Down / 行升序 / 行降序）
> 6 项 EditorTab 实现 **100% Scintilla 代码，L2 全空**（证据④）— 全部已知 FAIL

| TC ID | 标题+快捷键 | 步骤 | 预期（L2 下定义，修完后断言） | 优先级 | L2 已知 FAIL + 证据 | SmokeBase |
|---|---|---|---|---|---|---|
| ED07 | 复制当前行 Ctrl+D | L2 open small(T_SMALL_20L 每行 80B) → goto line 10 → snapshot before → **MainWindow Ctrl+D keySeq**（路径 A）→ ① vs=+81（新增一行 80B+'\n'）；② line 10 内容=原 line 10；③ line 11 内容=原 line 10（复制一份）；④ 原 line 11..19 → 下移到 12..20 | 4 断言全过 | **P0** | ⚠️ **FAIL 证据④**：[EditorTab::duplicateLine](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L489-L496) 全 Scintilla，L2 下什么都不会发生 | MainWindow实例 Ctrl+D + vs + readByteRange(startOfLine(9), startOfLine(13)) |
| ED08 | 删除当前行 Ctrl+Shift+L | L2 open small → goto line 10 (80B + '\n' = 81B) → snapshot → 路径 A Ctrl+Shift+L → ① vs=-81；② 新 line 10 内容 = old line 11 内容（原 line 11 顶上来）；③ 原 line 10 字节完全消失（readRange 里没有它的特征字符串） | 3 断言全过 | **P0** | ⚠️ **FAIL 证据④**：deleteLine() 全 Scintilla | MainWindow实例 Ctrl+Shift+L + vs + readRange + old line 10 substring occur 0 |
| ED09 | 上移当前行 Alt+Up | L2 open small → goto line 5 → snapshot（L4="AAAAA...", L5="BBBBB..."）→ Alt+Up → ① vs 未变（只是字节重组）；② 新 L4="BBBBB..."；③ 新 L5="AAAAA..."（两行交换） | vs 不变；L4/L5 内容交换；L6..19 保持不变 | **P0** | ⚠️ FAIL 证据④ | MainWindow实例 Alt+Up + vs + readLines(4,6) + diff |
| ED10 | 下移当前行 Alt+Down | L2 open small → goto line 5 → snapshot → Alt+Down → L5/L6 内容互换；L7+不变 | 同 ED09（L5↔L6） | **P0** | ⚠️ FAIL 证据④ | MainWindow实例 Alt+Down + same |
| ED11 | 行升序排序（菜单项：编辑 → 行升序排序） | L2 open fixture = lines 乱序：`["CCC","AAA","BBB"]` 加上 \n（3 行）→ dragSelect 全部（0..vs）→ 触发 sortAscAct action() → read 前 3 行 === "AAA\nBBB\nCCC\n" | lines 正确升序；字节流精确匹配 | **P0** | ⚠️ FAIL 证据④ | `triggerAction(sortAscAct)` + readLines(0,3) === expected sorted |
| ED12 | 行降序排序 | fixture = 3 行乱序 → 选中全部 → trigger sortDescAct() → "CCC\nBBB\nAAA\n" | 正确降序 | **P0** | ⚠️ FAIL 证据④ | 同 ED11（desc） |

---

## 3. 搜索菜单（Search · 6 条目：Ctrl+F/Ctrl+R / 查找下一个 F3 / 查找上一个 Shift+F3）
> 注：原菜单 `"替换(&R)..."` 的访问键 &R 与全局快捷键不一致，造成用户混淆（见证据⑫）。按你要求：**全局快捷键改为 Ctrl+R**，与菜单括号显示 (&R) 对齐，与 Notepad++ 习惯一致。

| TC ID | 标题+快捷键 | 步骤 | 预期 | 优先级 | 已知 FAIL？ | SmokeBase |
|---|---|---|---|---|---|---|
| SR01-A | 查找对话框 Ctrl+F（路径 A MainWindow 快捷键） | MainWindow openL2 small → tab 无对话框 → **MainWindow Ctrl+F keySeq** → FindReplaceDialog visible=true；**替换输入框 hidden/disabled（查找模式）**；查找输入框 focused；window title 包含"查找"（或产品配置标题） | 3 断言 | **P0** | ⚠️ 上一轮 FR smoke 只直接 show dialog 没有用真实 MainWindow Ctrl+F，这条是缺失的用户路径 | MainWindow实例 Ctrl+F + dialogVisible + replaceEditVisibility=disabled/hidden + titleContains("查找") |
| SR01-B | 查找（菜单点击：findAct trigger） | 同 SR01-A 改为 findAct->trigger() | 同 SR01-A | **P1** | - | `triggerAction(findAct)` |
| SR02-A | 替换对话框 **Ctrl+R**（路径 A MainWindow 快捷键；证据⑫：改为 Ctrl+R 与菜单 (&R) 对齐） | MainWindow openL2 small → tab 无对话框 → **MainWindow Ctrl+R keySeq**（注意：不是 Ctrl+H）→ dialog visible=true；**替换输入框 visible+enabled（替换模式）**；窗口标题含"替换" | 3 断言 | **P0** | ⚠️ **你原话实锤「替换快捷键 &R 与全局 H 不一致」的直接修复验证（证据⑫）** | MainWindow实例 Ctrl+R + dialog visible=true + replaceEdit.isVisible=true + replaceEdit.enabled=true + titleContains("替换") |
| SR02-B | 替换（菜单点击：replaceAct trigger） | 同 SR02-A replaceAct trigger | 同 SR02-A | **P1** | - | triggerAction(replaceAct) |
| **SR02-COMFORT** | **替换菜单一致性：menuText 显示 "替换(&R)" 且 replaceAct shortcut 为 Ctrl+R（证据⑫硬断言）** | 读 MainWindow replaceAct 对象 → ① action->text() 包含 "(&R)"（或 regex "替换.*&R"，或菜单文本里括号内是 R 不是 H）；② action->shortcut() == QKeySequence(QKeySequence("Ctrl+R"))（精确等于 Ctrl+R，**绝对不能是 Qt::KeySequence::Replace = Ctrl+H**）；③ 且 Alt+S 打开搜索菜单后按 R 键也能打开替换对话框（访问键验证，路径 C） | 3 条断言全过（与你原话要求：「我下意识以为是 Ctrl+R，通常习惯也是这个，不建议改为 H」完全一致 → 强制 Ctrl+R + &R 同字） | **P0** | ⚠️ **证据⑫ 当前代码直接 FAIL：[MainWindow.cpp:272-273](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L272-L273) 菜单 text="替换(&R)..." 但 shortcut=QKeySequence::Replace（Windows下=Ctrl+H），二者不一致 = FAIL** | `readAction(replaceAct)` + textContains("&R") + shortcut==Ctrl+R exact + 访问键 Alt+S 后 R 按下 dialog 弹出 |
| SR03 | 查找下一个 F3（路径 A MainWindow 快捷键） | MainWindow openL2 T_REPLACE_3_PAT → 先通过 findDialog 设置 pattern="__TEST_OLD__"（保证 m_lastFindPattern 非空）→ **MainWindow F3 keySeq 连按 3 次 wrap=off** → 每次 selection === OLD bytes；offsets 单调递增 430<2056<3682；第 4 次按 F3 返回 false（wrap=off） | 同 FR004 严格递增断言；路径 A=F3 | **P0** | ⚠️ 你原话：「查找上一个/下一个经常无法跳正确位置」的真实路径（MainWindow::findNext wrap 硬编码 true 而不是 false，可能 wrap 语义不对？）—— 看 [MainWindow::findNext](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L710-L717) 最后一个参数 wrap=true —— 这可能就是 wrap 语义与用户预期不符的 bug | MainWindow实例 setLastFindPattern + F3 keySeq×3 + offsets 递增断言 |
| SR04 | 查找上一个 Shift+F3（路径 A MainWindow 快捷键） | 先 goto EOF（cursor=vs）→ pattern 存在 → **MainWindow Shift+F3 连按 3 次 wrap=off** → offsets 单调递减 3682>2056>430；第 4 次 false；每次 selection=OLD bytes | 递减断言 | **P0** | ⚠️ 你「查找上一个不跳」实锤来源；路径 A 之前从未覆盖 | MainWindow实例 gotoEOF + setLastFindPattern + Shift+F3×3 + offsets |
| SR05 | patch 插入新内容后查找（LargeFileSearch 是否真走 renderRange？） | L2 open small（原无 OLD）→ 在 L10 插入 `abc__TEST_OLD__xyz` → Ctrl+F 填 OLD → F3 → ① findNext 返回 true；② selection=="__TEST_OLD__"（长度 12）；③ 再 findNext wrap=off → false（只有 1 处） | 3 断言 | **P0** | ⚠️ **超大潜在 bug**：怀疑 LargeFileSearch::findAll/scanChunk 直接读原 mmap，没走 renderRange → **插入到 patch 的内容永远搜不到** → 这条必 FAIL（修复前应该 FAIL） | insertText + findNext with pattern + selectionAssert + 2nd=false |
| SR06 | 替换 0 处 = 没找到的 pattern：replaceAll 返回值=0 且文件字节流 100% 不变 | L2 open small → snapshotFull → replaceAll("NOSUCH_XXXX", "X") → ① n=0；② full vs snapshot 逐字节完全相同（没有任何字节被意外改写）；③ saveNeeded 仍 false | 3 断言 | **P0** | 0 处必须不产生任何副作用 | snapshotFull + replaceAll(nonExist) + fullDiff===snapshot + saveNeeded false |
| SR07 | 替换全部 undo grouping：3 处替换后 undo × 1，一次性全回到 3 处 OLD（不是只回滚 1 处） | L2 open T_REPLACE_3_PAT → snapshot → replaceAll OLD→NEW(n=3) → snapshot2（NEW 3 处）→ **MainWindow Ctrl+Z 路径 A 或 B undo × 1** → full === snapshot（不是 snapshot-1 中间态）；canUndo=false 或 回到原来 undostack 状态 | full identical to snapshot（一次 undo 回到原） | **P0** | 批量 replaceAll 的 undo group 语义 | snapshot + replaceAll + undo(1) + full identical to original |

---

## 4. 语法菜单（Syntax · 9 项 group，纯文本 / C / C++ / Python / JSON / HTML / XML / SQL / YAML / Markdown）
> L2 本来不需要语法高亮（VirtualFileView 自己画），所以 P2 级要求：**菜单点了不崩溃 + 不破坏字节流 + saveNeeded 不被意外置为 dirty**

| TC ID | 标题 | 步骤 | 预期 | 优先级 | SmokeBase |
|---|---|---|---|---|---|
| SY01-SY09 | 切换 9 种语法：L2 下不崩溃 / 不崩 paint | 对每个 syntax i=0..8：L2 open small → snapshot full → trigger syntaxMenu->actions()[i] → processEvents × 3 → paint 1 次 → ① mainWindow 无异常 exit；② readByteRangeFull === snapshot（字节流 0 改动）；③ saveNeeded===false（切语法不是修改） | 每条都满足 3 断言 → 9 条全过 | P2 | loop: triggerSyntax(i) + snapshot diff + saveNeeded(false) + noCrash |
| SY10 | 语法切换后保存：字节流仍 === 原文件磁盘内容 | L2 open 切换到 Syntax::Cpp → save → disk bytes === original disk bytes（一模一样，不因为语法切换写入额外字节） | 一致 | P2 | syntaxChange + save + disk identical to original |

---

## 5. 书签菜单（Bookmarks · 4 条）
> 现状：EditorTab 4 条 100% Scintilla marker，L2 全空（证据⑤）→ 全部已知 FAIL
> **P0 定义**：L2 下点击这 4 条菜单项/快捷键，必须 **不崩 + saveNeeded 不意外变 dirty + 有明确反馈（statusBar 提示「L2 暂不支持书签」或实现行号栏书签标记）**；若真正实现则按更高断言

| TC ID | 标题+快捷键 | 步骤 | 预期（最低标准 = 不崩；若已实现=行号栏标记+跳转） | 优先级 | 已知 FAIL 证据 | SmokeBase |
|---|---|---|---|---|---|---|
| BM01 | 切换书签 Ctrl+F2（路径 A MainWindow Ctrl+F2） | L2 open small → goto line 10 → snapshot → Ctrl+F2 → statusBar 提示（含"书签"或行号栏该 line 有 图标绘制标志位 + 无异常；saveNeeded 仍 false）；saveNeeded=false；没有崩溃 | 最低：不崩 + saveNeeded=false；高：行号栏 marker 有标志位 | **P0** | ⚠️ FAIL 证据⑤：toggleBookmark 全 Scintilla marker | MainWindow实例 Ctrl+F2 + windowNoCrash + saveNeeded=false + statusBarContains("书签") 或 gutterMarkerPresent(line) |
| BM02 | 下一书签 F2（路径 A F3） | L2 open → 先在 line 3/7/12 切换书签（BM01 做 3 次）→ cursor=0 → 连按 F3 3 次 → jump 到 3→7→12；wrap=true 第 4 次回到 3 | cursor = startOfLine(3/7/12) 顺序；若未实现则至少不崩 + statusBar 提示 | **P0** | ⚠️ FAIL 证据⑤：nextBookmark 全 Scintilla markerFindNext | F2 keySeq×N + cursor===startOfLine(expected) |
| BM03 | 上一书签 Shift+F2 | 先加书签 3/7/12；cursor=EOF；连按 Shift+F2 → 12→7→3 | 反向递减跳转正确 | **P0** | ⚠️ FAIL 证据⑤ | Shift+F2 keySeq + cursor |
| BM04 | 清除所有书签（菜单点击 clearBmAct trigger） | 加书签 5/8 → trigger clearBmAct → line5/8 marker 全部清除（F2 跳 0 处或 wrap 不到）；saveNeeded=false | 清除正确 | P1 | ⚠️ FAIL 证据⑤ | triggerAction(clearBmAct) + marker count=0 |

---

## 6. 视图菜单（View · 16 条目含子菜单）
> MainWindow zoom 证据⑥：只调 Scintilla；长行标尺 证据⑦：L2 无效；16 进制模式/自动换行/深色模式有 L2 分发

| TC ID | 标题+快捷键 | 步骤 | 预期 | 优先级 | 已知 FAIL？ | SmokeBase |
|---|---|---|---|---|---|---|
| VW01 | 深色模式（视图 → 深色模式 checkable） | L2 open small → 勾选 dark 菜单 → viewport 背景色 == dark palette；再切回 light → 正常；不崩；字节流不变；saveNeeded=false | 背景色；不崩；bytes 不变；saveNeeded=false | P1 | - | triggerAction(darkModeAct) + viewportBackground == expectedColor + saveNeeded(false) |
| VW02-A | 自动换行 Ctrl+W（路径 A MainWindow 快捷键） | L2 open small（默认 wordWrapOff）→ horizontalScrollBar.range = big（非零）→ **MainWindow Ctrl+W keySeq** → ① m_actWordWrap.checked=true；② horizontalScrollBar.enabled=false（自动换行不应出现水平滚动，W006 语义）；③ wordWrap()=true；④ 再 Ctrl+W → 反向复原 | 4 断言 | **P0** | [EditorTab::setWordWrap L2 已分发](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L856-L871)，路径 A 应该 OK | MainWindow实例 Ctrl+W + checkedState + horizontalScrollBar().enabled/Range + wordWrap() bool |
| VW02-B | 自动换行（菜单点击：wordWrapAct trigger） | 同 VW02-A，用 trigger action | 同 VW02-A | **P0** | - | triggerAction(wordWrapAct) |
| VW03 | 长行标尺竖线（视图 → 长行标尺(竖线) checkable） | L2 open small → setEdgeColumn(80) → toggle edge visible ON → viewport paint 里 X==80×charWidth 的竖线出现；切 OFF 消失；字节流不变；saveNeeded=false | paint 有/无竖线；bytes 不变；saveNeeded=false | P1 | ⚠️ FAIL 证据⑦：[EditorTab applyEdgeSettings](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L715-L723) 只调 Scintilla，VirtualFileView 自己画不检查 edgeVisible | triggerAction(edgeVisibleAct) + paintSpy hasVerticalLineAt(col) or not + saveNeeded(false) |
| VW04-A | 长行标尺列数 = 72/80/100/120/150/200（6 项循环） | 对 6 个 col，依次点击视图 → 长行标尺列数 → "100 列" 动作 → m_edgeColGroup checked action 的 data==100；edgeColumn property ==100；随后竖线（VW03 打开时）位置正确落在 col × charWidth | checked correct；edgeColumn correct；竖线 X correct | P1 | ⚠️ 证据⑦：同 VW03，L2 VirtualFileView 没读 edgeColumn | 循环 6 个 col + triggerEdgeColMenu(col) + edgeColumn property check + lineAtX == colX |
| VW05-A | 16 进制编辑模式 Ctrl+Shift+H（路径 A MainWindow 快捷键） | L2 open small → snapshot → Ctrl+Shift+H → ① viewModeL2==Hex；② 视图像素区域中出现 hex dump（"00000000: 4C 30 3a ..."之类的文本行）；③ 字节流不变；④ 再 Ctrl+Shift+H → 回到 Text 模式；⑤ saveNeeded 全程 false | 5 断言 | **P0** | [MainWindow toggleHexViewMode 仅 L2 生效](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L835-L847) → L1 切不切都 OK；实锤路径 A L2 | MainWindow实例 Ctrl+Shift+H + viewModeL2() + paint 内容（has hexdump style） + saveNeeded(false) + fullDiff(snapshot) |
| VW05-B | 16 进制模式（菜单点击 hexAct trigger） | 同 VW05-A trigger hexAct | 同 VW05-A | **P0** | - | triggerAction(hexAct) |
| VW06 | 16 进制编辑模式 · 非 L2 Tab（L1 small file）按 Ctrl+Shift+H → m_actHexViewMode.checked 仍= false（不进入）；statusBar 有提示「仅 L2 大文件」；无崩溃 | checked=false；status 有提示；noCrash | P1 | - | openL1 small + Ctrl+Shift+H + checked(false) + statusBarContains("仅 L2") |
| VW07-A | 放大 Ctrl++（路径 A MainWindow 快捷键 Ctrl+'+'） | L2 open small → zoom level before=Z0 → **MainWindow Ctrl++ keySeq 3 次** → 每次 viewport.font.pointSize 递增（或 fontMetrics height 递增）；字节流不变；saveNeeded=false | height 递增×3；bytes 不变；saveNeeded=false | P1 | ⚠️ **FAIL 证据⑥**：[MainWindow zoomIn](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L774-L777) 直接调 `t->editor()->zoomIn()`（Scintilla），L2 VirtualFileView 的字体没变 → 路径 A 必 FAIL；另外 EditorTab 没有 zoomIn() 接口，需要补上 L2 分发 | MainWindow实例 Ctrl++ × 3 + fontMetricsHeightBefore/After（递增） + bytes 不变 + saveNeeded(false) |
| VW07-B | 缩小 Ctrl+-（路径 A MainWindow 快捷键） | L2 open small → Ctrl+- × 3 → fontMetrics height 递减×3；bytes 不变；saveNeeded=false | 递减×3；其余不变 | P1 | ⚠️ FAIL 证据⑥（MainWindow zoomOut 同样 editor()） | same（递减） |
| VW07-C | 重置缩放 Ctrl+0（路径 A MainWindow 快捷键） | L2 open small → Ctrl++ × 5（字体变大）→ Ctrl+0 → fontMetrics height === 初始值（精确回到 100%）；bytes 不变；saveNeeded=false | height exactly original | P1 | ⚠️ FAIL 证据⑥（resetZoom 同样 editor()） | same（original） |

### 6.1 16 进制编辑专项（Hex · 10 条，证据⑭：补你说「没看到测试用例」的 Hex 功能）
> 功能入口：`视图 → 16 进制编辑模式`（Ctrl+Shift+H）。代码已实现 [MainWindow::toggleHexViewMode](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L835-L847)，仅 L2 生效。原 TC VW05-A/B/C + VW06 仅覆盖模式切换与 L1 拒绝，完全没覆盖"真正编辑字节"的功能。本节测"16 进制编辑"（不是 Hex 查看）的全链路。

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 溯源（证据⑭） | SmokeBase |
|---|---|---|---|---|---|---|
| **Hex01** | **Hex 模式视觉格式校验：每一行结构 = 8 字节 offset + ": " + 16×hex(2 位+空格) + "  " + ASCII printables（非可见=.）** | L2 open small fixture = "Hello_World! 0123456789ABCDEF..."（至少 64 字节）→ Ctrl+Shift+H 进入 Hex → viewport 前 4 行 paint spy 内容解析 | ① 每一行 offset 严格 +16（第 0 行=00000000，第 1 行=00000010/0x10，第 2 行=0x20 ...）；② hex 区域 2 位十六进制字符，间隔空格，每行 16 对；③ ASCII 区域：可见字符原样显示，0x00-0x1F/0x7F-0xFF 统一显示为 '.'；④ 解析出的所有 hex 字节值（按顺序合并 = 16 行 × N 字节）与 `readByteRange(0, vs)` 逐字节 **100% 一致**（Hex dump 显示值必须等于真实虚拟字节流） | **P0** | ⚠️ 你原话「16 进制编辑也是重要功能，没看到你写测试用例」（证据⑭）——本条是 Hex 最基本的"显示正确性"，如果 hex 内容跟真实字节不一致，后面所有 Hex 编辑都毫无意义 | toggleHexViewMode(Hex) + paintSpy parse hex rows + parsed bytes concat === readByteRangeFull() 逐字节 |
| **Hex02** | **Hex 模式光标字节级定位：点击第 0 行 offset 0 + 第 5 个 hex pair → cursor byte = 4；点击第 1 行（0x10） 第 1 个 hex pair → cursor byte = 0x10** | L2 open small（至少 32B）→ Hex 模式 → mouseClickHex(offsetRow=0, pairIdx=5)（第 5 对 0-indexed，即第 6 字节）→ 读 cursor byte C5；再 click Hex(offsetRow=1, pairIdx=0)（offset=0x10 第 1 字节）→ C16 | ① C5===4；② C16===16；③ 在 (row, pair) 处 caret 真的画出来（caretRect 位于该 hex pair 图形 boundingRect 内部，不在 ASCII 区） | **P0** | 16 进制下编辑字节的前提 = 光标必须能精确定位到每个 hex pair | clickHex(row=0, pair=5) + cursor===4 + clickHex(row=1, pair=0) + cursor===16 + caret inside hex pair boundingRect |
| **Hex03** | **Hex 模式 ASCII 区点击也能正确定位字节：ASCII 区第 0 行第 5 个字符 → cursor byte = 4（同 Hex02 pairIdx=5）** | Hex 模式 → clickAsciiCell(row=0, asciiCol=5)（ASCII 区 16 列的第 6 个字符）→ cursor byte C | C===4（完全等价于点 Hex 区第 5 对）；caretRect 位置在 ASCII 区对应字符（如果产品支持 Hex/ASCII 两边都能定位光标） | **P0** | 常见 16 进制编辑器用户习惯（ASCII 区点击也能定位） | clickAscii(row=0, col=5) + cursor===4 + caret inside ascii cell rect |
| **Hex04** | **Hex 模式真正编辑字节：改单个 hex pair（0x41='A' → 0x42='B'）→ 切回 Text 模式看到 'A' 变 'B'，readByteRange 读到 0x42** | fixture = bytes { 0x00, 0x01, 0x41, 0x41, 0x41, 0xFF }（中间 3 个 'A'）→ L2 open → Hex → 定位 offset=2 (row=0 pair=2，字节值 0x41='A') → **输入键盘 4 2（键入两个 hex 字符）** → 新值 = 0x42；再改 offset=3 输入 4 3（→0x43='C'）→ 按 Ctrl+Shift+H 切回 Text → 读 readByteRange(2,5) 3 bytes；再切回 Hex 看第 0 行 hex pair 2/3 值 | ① **readByteRange(2,5)=={0x42, 0x43, 0x41}**（第一个 A→B，第二个 A→C，第三个 A 不变）；② Text 模式下，该位置文本显示为 "BCA"（不是 "AAA"）；③ vs 不变（只改 3 字节，没增删）；④ saveNeeded=true；⑤ undo 1 次 → read(2,5) 回到 {0x41,0x41,0x41} | **P0** | ⚠️ 16 进制"编辑"≠"查看"的核心 TC —— 你原话「16 进制编辑也是重要功能」，这才叫"编辑"（改字节值），原 VW05 只测了切换 | open hex + locate at pair(2) + **typeHex("42")** + typeHex("43") at pair(3) + toggle Text + readRange(2,5) == {0x42,0x43,0x41} + vs unchanged + saveNeeded true + undo restore |
| **Hex05** | **Hex 模式删/增字节（如果支持）：选中 3 个 hex pair → 按 Delete → vs=-3；光标处输入 FF 00 → vs=+2**（若产品目前 Hex 模式只允许改字节不允许增删，则 TC 标"已知不支持 = 禁用增删按钮"，但至少不崩）** | 至少支持 "改现有字节"（Hex04）；如果允许增删：① dragSelectHexPairs(offset=4, len=3) → Delete → vs=old-3；② cursor=8 → typeHex "FF00" 两字节 → vs=old-3+2；readBytes(8,10) === {0xFF, 0x00} | 对应断言；如果不允许增删则：选中按 Delete → statusBar 提示 "Hex 模式仅支持修改现有字节，请先切换到 Text 模式增删"，vs 不变（至少不崩） | **P0** | Hex 模式增删字节的可选但常见能力（如果暂未实现，至少保证"不崩溃+有提示"） | dragSelectHex + Delete + vs trans + typeHex + vs trans + readBytes |
| **Hex06** | **Hex 模式下 Ctrl+F 可搜索字节序列：pattern="43 43 43"（即 "CCC"）→ findNext 跳转到对应 offset，选中对应的 3 个 hex pair** | fixture 在 byte 400/2000/3682 处分别写入 0x43 0x43 0x43（CCC）→ L2 open → Hex → Ctrl+F → pattern="43 43 43"（空格分隔的十六进制串）→ F3 连按 3 次 wrap=off → 每次 cursorStartByte === 400 / 2000 / 3682 分别；每次 selectionBytes = {0x43,0x43,0x43} | 3 次跳转到正确 3 处；selection 正确；第 4 次=false（wrap=off） | **P0** | Hex 模式下搜索十六进制字节序列（大文件二进制编辑关键能力） | write fixture with 3×CCC at known offsets + Hex mode + findDialog pattern hex bytes "43 43 43" + F3×3 + cursorStart offsets === [400,2000,3682] |
| **Hex07** | **Hex 模式下 Ctrl+A 全选 → selSize == virtualSize × 2 hex_pairs + ... = 真正全选字节数 vs，不是十六进制显示的 2×vs** | L2 open T_1KB（102_400 字节）→ Hex → Ctrl+A → selSize（真正字节数，不是选中的 hex 字符数） | **selSize_byte_scope === 102_400**（等于 virtualSize）；readSelectionBytes() === readByteRangeFull() 逐字节相同（口径与 S 节一致） | **P0** | 与 S001-A 口径一致（字节流口径），不是"十六进制字符数"这种显示口径 | Ctrl+A + selSize(bytes)==102_400 + readSel===readRangeFull |
| **Hex08** | **Hex 模式保存 round-trip：在 Hex 模式下编辑 10 处字节（Hex04 流程 × 10）→ Ctrl+S → 关闭 → 重新打开 → 读磁盘字节 === 编辑后 readByteRangeFull()** | fixture 2000B → open L2 → Text 模式 snapshot_full；切换 Hex 模式，对 offset = 100×i (i=1..10) 10 处分别写 0xAA 0xBB 0xCC 0xDD 0xEE 0xFF 0x11 0x22 0x33 0x44（每处 1 字节）→ **记录 vs + 快照 readByteRangeFull()** → save → close tab → reopen path → **disk bytes === 快照**（逐字节全量对比，10 处改动必须完整保存进磁盘） | disk bytes === 快照 readByteRangeFull；10 处 offset 字节分别等于写入的 0xAA~0x44；其他字节等于 original snapshot（其他字节没被意外改写） | **P0** | Hex 模式编辑后的保存 round-trip 闭环（跟 F007 一样的闭环，Hex 版本） | 10 edits at offsets + snapshot full → save/close/reopen → disk full === snapshot full + 10 offsets == 0xAA..0x44 + other bytes === original |
| **Hex09** | **Hex ↔ Text 双向切换一致性：任何时候（编辑前/编辑 N 次后/undo M 次后）切模式，readByteRange(0,vs) 100% 一致，无任何字节被偷偷改** | small → Text 插 3 patch（制造复杂组合）→ snapshot1 = readFull；切 Hex → 改 5 处字节 → snapshot2 = readFull；切 Text → 再改 2 处文本 → snapshot3 = readFull；切 Hex → undo 5 次 → snapshot4 = readFull；最后回到 Text → snapshot_final | snapshot1..snapshot_final 每个模式切换瞬间，**在模式切换的那一刻读取的 readFull 完全等于上一个模式最后读到的 readFull**（字节流完全独立于视图模式，模式切换绝不能对字节流有副作用） | **P0** | 防止"切个 Hex 模式把字节流改了/改了 BOM/加了空格"这种经典 Hex/Text 模式破坏 | 循环 mode toggle × N 次 + 每次 toggle 前后 readFull 完全 identical + saveNeeded 不因 toggle 改变（除非真的做了编辑） |
| **Hex10** | **Hex 模式下 NUL 字节 0x00 正常显示和编辑：offset 100 字节原=0x00 → Hex 显示为 "00"，ASCII 显示为 '.'；改为 0x01 → "01"/'.'；再改回 0x00** | fixture 写入 offset 100 = 0x00（NUL）→ open L2 Hex → parseHexRowAtOffset(100) pair(4)=="00"（row=6 if 16-per-row 行 0x60=96, pair 4=100）；ascii row6 col4=='.'；typeHex "01" → pair=="01"；ascii still '.'；readByte(100)==0x01；typeHex "00" → back 0x00 → byte==0x00 | 显示（0x00 = "00"/'.'）正确；编辑/回读 0x00↔0x01↔0x00 精确；不被 strip（编辑器不偷偷删 NUL 字节） | **P0** | 二进制大文件的基本前提 = NUL 字节必须可见且可编辑，不能按 C 字符串语义遇到 0 就止 | parse hex row at NUL + ascii char '.' + typeHex("01")/("00") back + readByte(100) 循环相等 |

---

## 7. 编码菜单（Encoding · 11 条 = 5 条转为 + 5 条以编码显示 + 1 分隔线）
> 「转为」会脏化 saveNeeded；「以编码显示」= 不改字节流只改变显示解码

| TC ID | 标题 | 步骤 | 预期 | 优先级 | SmokeBase |
|---|---|---|---|---|---|
| EC01-05 | 转为 5 种编码（GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE）→ saveNeeded=true + 状态栏显示对应编码名；提示 dialog yes/no 行为正确（仅当 isModified 时弹） | ① 先 clean → 转 UTF-8（==当前）→ saveNeeded=false（无变化）；② clean → 转 UTF-16LE（!=当前）→ 若 isModified=false（无用户编辑）→ saveNeeded=true（无需弹对话框）；③ dirty → 转 GBK → mock QMessageBox=Cancel → encoding 仍不变（没被切）；④ dirty → mock=Yes → encoding===GBK + saveNeeded=true | 4 条场景分别断言；不崩溃 | **P0** | loop over 5 enc + isDirty 3 种 scenario + mockMessageBox(Yes/No/Cancel) + encoding() + saveNeeded() + statusBarContains(encodingName) |
| ED06-10 | 以编码显示 GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE：字节流 100% 不被改动 + saveNeeded 永远 false + viewport 显示文本（中文）正确（对应编码） | ① fixture 中写中文 fixture 5 编码各一份；② open → 切"以 GBK 显示" → 读 rendered text "中"对应位置显示字符正确（GBK 下"ÖÐ"或正确解码，关键是字节流不被改）；③ saveNeeded=false；④ full vs snapshot 一致 | 核心：full === snapshot（字节流不破坏）；saveNeeded=false；其次：显示渲染符合编码（not crash） | **P0** | loop over 5 enc "display encoding" + full identical snapshot + saveNeeded(false) + viewport contains expected glyphs（或至少 not crash） |
| ED11 | 先"转为 UTF-16LE"→ save → reopen → disk bytes 前 2B == BOM 0xFF 0xFE（UTF-16LE）→ read first N bytes（按 BOM/按编码）符合 Notepad++ round-trip | 磁盘字节前 2B == BOM；bytes 完整保存不丢失；reopen 后文本完整 | **P0** | convertEncoding + save + reopen + disk first 2 bytes == BOM；full text identical original |

---

## 8. 光标与定位（C · 保留但去掉压测，11 条；用户反馈「定位全错」）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 用户反馈溯源 | SmokeBase |
|---|---|---|---|---|---|---|
| C001 | OFF 模式：click line L/col C → cursor = startOfLine(L) + C；statusBar row=L+1 col=C+1 | T_SMALL_20L → clickAtLineCol(9, 5) → cursor + status | cursor = start +5；status "10,6" | **P0** | - | clickAtLineCol + cursor + statusBar |
| C002 | OFF 模式：click 换行前 vs 换行后 → cursor = endOfLine(N) vs startOfLine(N+1) | click (N, 80) vs (N, 81) 或下一行 0 | 两个 cursor 值；差=1（\n 字节） | **P0** | - | 2 clicks + cursor diff assertion |
| C003 | ON 模式·超长行：显示行 0 vs 显示行 1 top click → cursor = 0 vs displayLineStride 字节且仍在同一逻辑行 | ULTRA_LONG_LINE_10MB → 点击显示行 0 第 1 像素 → cursor=C0；scrollVerticalDisplayLines(+1) → 点击显示行 0（新 top）→ cursor=C1；logicalLine(C0)==logicalLine(C1)=0；C1-C0 等于 displayLine 平均字节数（±2） | C0=0；C1>0；同逻辑行 | **P0** | ⚠️ 你：「自动换行下光标无法出现在点击位置」 | clickAtDisplayLine + scroll + cursor + logicalLine(c) |
| C004 | ON 模式·超长行：同一逻辑行，连续 4 条显示行同 X=10px 点击 → C0..C3 严格递增，差约 displayStride | 同 C003，连续 click 显示行 0/1/2/3 → 记录 C0..C3 | 单调递增；Ci+1-Ci ≈ stride（±5%） | **P0** | ⚠️ 同上 | same |
| C005 | 方向键 ↓：OFF = 跳逻辑行（每行 80+1）→ cursor 递增 81B；ON = 跳 1 条显示行（约 80 字节前进） | L=10 col=0 → KeyDown 1 次 → 新 cursor；分别在 OFF/ON 测 | OFF: new cursor - old = 81；ON: new - old ≈ avg 显示行字节数（±3） | **P0** | - | keyPress(KeyDown) + cursor before/after |
| C006 | Home / End（OFF）→ 行首 / 行末（\n 前） | goto line5 middle → Home → C=startOfLine(5)；End → C=startOfLine(6)-1 | Home/End 各断言 | **P0** | - | key Home/End + cursor |
| C007 | Home / End（ON 超长行）→ 当前**显示行**首 / 末（不是逻辑行！） | ON → scroll 到显示行 #100 → 点击 middle → Home → C=**当前显示行起始字节**；End → C=当前显示行末字节；C1-C0≈列宽 | **关键：Home 不能跳回逻辑行首 0**（这就是你说的"定位全错"的典型） | **P0** | ⚠️ 你：「Home/End 拉回逻辑行起始」典型 bug | scrollDisplayLine(100) + middle click + Home + End + cursor start/end display |
| C008 | PgDn × 10 → 回 PgUp × 10 → 光标回到起始位置附近（±1 displayLine） | T_MEDIUM；PgDn×10；记录 C10；PgUp×10；Cfinal | Cfinal 在 [C0 - avgPerPage, C0 + avgPerPage]；无 crash/越界 | **P0** | - | N 次 PgDn/PgUp + cursor 区间断言 |
| C009 | Goto 行 N（Ctrl+G，路径 A 快捷键 + mock goto 对话框输入 N） | Ctrl+G → mockDlg input=500 → cursor = startOfLine(499)；status row=500 | cursor = start；status "500, 1" | **P0** | - | keySeq Ctrl+G + mockGotoDlg(N) + cursor + status |
| C010 | Goto 字节 N（EditorTab gotoByteOffset 或 find 模式）→ readByteRange(N-5,N+5) === fixture 的对应字节（含 patch 后虚拟字节） | 对 N=400/800/1598 → goto + read vs fixture | 逐字节一致 | **P0** | - | gotoByteOffset + readRange vs fixture bytes |
| C011 | CJK 多字节点击：不落到 3 字节中间；后续插 X 不写坏 UTF-8 | CJK fixture 1 行 → 点击 "中"字中间 1/2 像素 → cursor 要么字符起始要么结束（字节位置 % 3 ==0 对于中文 3B）→ 插 "X" 到 cursor → 读回附近 6B UTF-8 decode valid，无 replacement char | UTF-8 valid always；insert 后仍 valid | **P0** | 多字节边界正确性 | click CJK glyph pixel + cursor % codepointSize assertion + insert "X" + utf8Valid(readRange) |
| **C012** | **ON 模式超长行：非首显示行（显示行 #5/#10/#20）点击必须能真正落光标，不得跳回逻辑行首（证据⑨）** | ULTRA_LONG_LINE_10MB 超长单逻辑行 → scrollVerticalDisplayLines(5)（显示行 5 顶对齐）→ clickAtDisplayLine(0, xPx=100)（当前 viewport 显示行 0，对应整个超长逻辑行的第 6 条显示行）→ 记录 cursor C5；再 scrollVerticalDisplayLines(+5) 再 click 记录 C10；再 +10 再 click C20 | ① C5、C10、C20 都 **> displayStrideBytesApprox()**（即不是 0，也不是逻辑行首 = 没有跳回第一行起点）；② C5 < C10 < C20 严格递增；③ 三次点击的 caret 在 viewport 对应 (X, Y) 真的画了闪烁像素（paint spy） | **P0** | ⚠️ 你原话实锤「除了第一行，其它显示行目前无法放入光标」的精确断言（证据⑨） | scrollVerticalDisplayLines(N) + clickAtDisplayLine + cursor > stride（≠0）+ strict C 递增 + caret paint present |
| **C013** | **ON 模式超长行：滚轮事件前进 = display line 粒度，不得直接跳下一物理行（证据⑩）** | ULTRA_LONG_LINE_10MB 超长单逻辑行 → vs >> 1 屏；viewport 显示行数 VDL = viewportHeight() / fontHeight() = 记为 N（通常 30~60）；记录初始视口顶部字节 T0；**执行一次 wheelEvent 一格（delta=120，对应 QApplication 行滚动步数 step=3 display lines）** → 新顶部字节 T1；再做一次 wheelEvent（两格 step=6）→ T2 | ① **核心断言：T1 ≈ T0 + 3 × displayStrideBytesApprox()**（± stride × 0.3 容差）；**② 绝对禁止：T1 == startOfLine(logicalLine(T0) + 1)**（一跳跳到下一物理行 = FAIL，这就是你原话「滚动滚轮会直接跳到物理第二行」的数值证据 ⑩）；③ T2 ≈ T0 + 6 × stride 或 T1 + 3 × stride（增量正确累积） | **P0** | ⚠️ 你原话实锤「中间 90 行内容不可见」的精确数值断言（证据⑩） | wheelEventOneStep(forward) + T before/after + stride assertion + **T1 != startOfLine(nextLogical)** |

---

## 9. 选区（S · 8 条，用户「无法拖选文字」）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 溯源 | SmokeBase |
|---|---|---|---|---|---|---|
| S001-A | Ctrl+A（路径 A MainWindow 快捷键）→ selStart=0, selEnd=virtualSize, selSize=vs | open L2 102_400 → Ctrl+A → size=102_400；readSelectionBytes() === readByteRange(0,vs) | 4 断言 | **P0** | 你「全选无反应」（见 ED06-A，同路径） | MainWindow Ctrl+A + size/start/end + readSel===readRange |
| S001-B | Ctrl+A（路径 B EditorTab API）→ same | `tab()->selectAll()` | same | **P0** | 同上 | tab()->selectAll() |
| S002 | OFF 模式拖选字节 [s=800, e=1600] → readSelectionBytes === readByteRange(800, 1600) 逐字节 | dragSelect(800,1600) → compare | identical | **P0** | 你「无法拖选文字」 | dragSelect(start,end) + readSel vs readRange |
| S003 | ON 模式跨显示行拖选：起点显示行 0 X=20px；终点显示行 5 X=40px（同一逻辑行超长）→ 选中大小 = (endByte-startByte)；读回来全是 'A'（全 'A' fixture）；logical 同 line | start/end 属同一逻辑行；读回全 'A'；size==end-start | **P0** | ⚠️ 你「自动换行无法拖选」 | dragSelectAcrossDisplayLines + logicalLinesSame + readSel is all 'A' + size check |
| S004 | ON 模式跨逻辑行拖选：行 1 末尾 display line → 行 2 第一条 display line → 选中字节内容 == "A"×N + "\n" + "B"×M（含换行符，行 1 全 A，行 2 全 B fixture） | 2 lines fixture；drag → readSel === expected sequence "A"+"\n"+"B"；size == N+1+M | exact sequence correct | **P0** | 同上（跨行更难） | same + 读回 exact bytes sequence |
| S005 | Shift+Right × 10 ASCII → selSize=10；再 Shift+Right ×5 CJK → selSize 再 +15（中文字符 3B×5=15） → 总计 25 | 先 ASCII 10 → +10；再 CJK 5 → +15 = 25；selectionBytes length correct | 25；byte content right | **P0** | - | keySeq Shift+Right × N + selSize + content |
| S006 | 双击选词：点击 "hello_world-123" 中 'w' → selection === "hello_world-123"（或按 Notepad++ 词边界规则的对应词） | fixture "hello_world-123 foo_bar" → doubleClickAtByte(位置 w) → selection 完整词 | sel == expected word | **P0** | 经典双击行为 | doubleClickAtByte + readSel == word |
| S007 | 三击选整行：点击 line 7 任意位置 → selection bytes === readByteRange(startOfLine(7), startOfLine(8))（或去掉 \n 按产品） | tripleClickAtLine(7) + compare | exact identical | **P0** | 经典三击 | tripleClickAtLine(N) + readSel vs readLineRange |
| S008 | **选区内 readSelectionBytes 永远与 readByteRange(selStart, selEnd) 逐字节相同**（口径一致性） | open + insert 100B patch（制造复杂 patch 组合）→ dragSelect(800, 2400) → compare | 永远 identical（如果不同，会造成复制/剪切/替换文本跟 UI 显示不匹配——这就是"替换内容插旁边"的第二来源） | **P0** | 你「替换旁插」第二条潜在根因 | multiInserts + dragSelect + readSel === readRange |
| **S009** | **ON 模式超长行：拖选起点在显示行 #10（非首显示行，证据⑨），终点显示行 #20 → selSize ≈ 10 × displayStride（非 0 / 非只选第一行开头）** | ULTRA_LONG_LINE_10MB（单逻辑行全 'A'）→ scrollVerticalDisplayLines(10) → dragSelectAcrossDisplayLines(startDisplayRow=0 of viewport = 逻辑显示行 #10, x1=20, endDisplayRow=viewport rows-1, x2=viewportWidth-20 → 跨 10 条显示行）→ 再 scrollVerticalDisplayLines(10) 再跨 10 条 → 读 selectionBytes | ① selSize > 0；② **selSize ≈ 10 × displayStride**（± 10% 容差，即真的选了 10 条显示行长，不是 0/不是只选了显示行首几个字节）；③ selectionBytes 全部 = 'A'（全'A' fixture）；④ **绝对禁止**：selStart==0 或 selEnd==displayStride（= 偷偷跳回第一行只选了首行 = FAIL） | **P0** | ⚠️ 你原话实锤「除了第一行，其它显示行无法拖动选择」（证据⑨） | scrollVerticalDisplayLines(N) + dragSelectAcrossDisplayLines + selSize≈10×stride + selStart≠0 + all bytes 'A' |
| **S010** | **ON 模式超长行：滚轮滚动后再拖选，选区仍正确（结合证据⑨⑩）** | ULTRA_LONG_LINE_10MB → wheelEvent × 4（前进 4 格，约 12 display lines）→ 记录顶部字节 T；拖选 3 条显示行 → selStart ∈ [T-50, T+500]（选区真的在当前可视字节附近，不是偷偷回到 0 = FAIL）；selSize ≈ 3×stride | 选区范围真的在滚动后位置；selSize 合理 | **P0** | 滚动+拖选链路常见组合 bug | wheelEvent×N + dragSelect 3 lines + selStart∈[T-50,T+500] + selSize≈3×stride |

---

## 10. 基础编辑（E · 13 条）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 溯源 | SmokeBase |
|---|---|---|---|---|---|---|
| E001 | 行首/行中/行末/换行前 4 点插可打印字符：字节流正确；下方行无水平偏移（与 R001 同机制） | 4 点 × "INSERT_<tag>"（15B）→ 读 3 行边界 + content | 插入位置内容正确；下一行 start-上一行 end = 正确 +15 | **P0** | 你「坐标错乱」 | insert 4 位置 + read line boundaries + content |
| E002 | Backspace：行中删 1B → 行少 1；行首删 → 本行并入上一行（少一个换行字节） | Backspace ×1 行首 → vs-1；startOfLine(N) 新 == startOfLine(N-1) 旧 + len(old line N-1) | 2 场景断言 | **P0** | - | keyPress Backspace + vs + startOfLine before/after |
| E003 | Delete：行末删 1B（换行符）→ 下一行并入本行（startOfLine(N+1) 向后移 1，少了换行） | Delete ×1 at (行末前 1 byte) → vs-1；contents merge check | vs-1 + contents merged correctly | **P0** | - | keyPress Delete + vs + merged content check |
| E004 | Enter 行中：1 行断 2 行；新行内容正确；后行 start +1（多一个 \n） | Enter at col 40 → vs+1；new startOfLine(6) = old startOfLine(5) + 40 + 1；line 6 bytes = old line5 col40..end + \n | 4 断言 | **P0** | - | keyPress Enter + vs + line start + line 6 content |
| E005 | Tab 键插入 0x09 字节；cursor +1；显示宽度按 tabWidth（显示相关，但字节必须=0x09） | cursor=500 → keyPress Tab → readByteRange(499,501)[1] == 0x09 | byte==0x09；cursor+1 | **P0** | - | keyPress Tab + byte == '\t' + cursor+1 |
| E006 | Undo × N 全还原：插 M1 → M2 → undo 2 次；字节流 === snapshot 原值；canUndo=false；canRedo=true | snapshot + 2 inserts + undo×2 + fullDiff | full===snapshot | **P0** | - | snapshot + 2 insert + undo×2 + fullDiff(snapshot) + canUndo/Redo states |
| E007 | Redo × N：E006 之后 redo×2 → 回到 M1+M2 态；canRedo=false | redo×2 + full===expected(M1+M2) | full===M1+M2 | **P0** | - | redo×2 + fullDiff(expected) + canRedo(false) |
| E008 | 100 步 undo/redo 往返（非压测，是容量正确性的小步循环）：循环插 100 个单字符（每个位置 i 有特征字节）→ undo 100 次 回到 snapshot；redo 100 次 回到全插入态；任何中间点字节与特征一致 | snapshot；for i=1..100 insert i-th char；undo 100 次 → full===snapshot；redo 100 → full===allInserts | 2 端点 + 中间随机抽样 10 处字节一致 | **P0** | 不是压测（仅 100 步小容量），是 undo/redo stack 正确性的"不丢一步" | loop 100 steps + undo×100 + redo×100 + full diffs |
| E009 | Ctrl+X 剪切：dragSelect [1000, 2000) → copy 保 S → Ctrl+X → ① vs=-1000；② 读取 [1000, vs_prev 位置) === original bytes from 2000；③ QClipboard===S | 3 断言 | **P0** | 你「无法剪切」 | dragSelect + saveS + Ctrl+X + vs=-1000 + readRange(1000, orig2000 pos) + clipboard===S |
| E010 | Ctrl+C 复制：选中 S bytes → Ctrl+C → vs 不变；Clipboard===S | vs 不变；Clipboard===S | **P0** | 你「无法复制？」（如果未实现） | dragSelect + Ctrl+C + vs identical + clipboard===S |
| E011 | Ctrl+V 粘贴 1KB：cursor=1000 → setClipboard('P'×1024) → Ctrl+V → vs+1024；readRange(1000, 2024) === 'P'×1024；cursor=2024 | vs+1024；1024 'P'；cursor+1024 | **P0** | 你「无法粘贴」 | setClipboard + Ctrl+V + vs + readRange + cursor |
| E012 | 撤销到保存点：clean → save → edit → saveNeeded=true → undo → saveNeeded=false → edit → saveNeeded=true | saveNeeded transitions 正确：false → true → false → true；bytes 也相应 | saveNeeded state machine | **P0** | saveNeeded 脏标志 | save+edit+undo+edit 检查 saveNeeded state transitions 4 次 |
| E013 | 多行插入换行后再全选+删除：插入 Enter 5 次 → vs+5 → Ctrl+A → Ctrl+X（剪切全部）→ vs=0（或空文件） | vs+5；然后全选剪切 vs=0；saveNeeded=true；空文件 OK | vs transitions correct；saveNeeded=true | **P0** | 极端场景 | insert Enters×5 + Ctrl+A + Ctrl+X + vs transitions + saveNeeded |
| **E004-BYTE** | **Enter 插入字节流实锤：光标位置 -1 必须真有 0x0A** | T_SMALL_20L open（word wrap OFF 避免干扰）→ 定位 line 5 col 0 → snapshot bytes → **Key_Return 1 次**（不是菜单）→ ① vs = old + 1；② **readByteRange(cursor-1, cursor) == QByteArrayLiteral("\n")**（即新字节 0x0A 确实在光标前被插入）；③ readByteRange(0, vs) 中 0x0A 次数 old_count + 1 | 3 断言同时成立（证明 insertAtCursor("\n") 字节流层面真生效了——证据⑪） | **P0** | ⚠️ 证据⑪ 你：「只会多出一个不可见字符」→ 此 TC 验证字节流确实有 \n（是显示层没分行/没渲染第二行，不是没插入） | keyPress(Key_Return) + vs=old+1 + byteAtCursor-1==0x0A + 0x0A count==old+1 |
| **E004-VISUAL** | **Enter 必须真分行可见：行数 +1 + 新第二行内容真画在新 Y 位置（证据⑪）** | T_SMALL_20L line 5 原本内容 = "L5: AAAAAAA..."（80B）→ 定位 col 40 → snapshot startOfLine(5) / startOfLine(6) / lineCount = Lcount_0；**按 Enter** → 断言：① lineCount == Lcount_0 + 1（**行数必须真 +1**，不是只多字节没加逻辑行）；② 新 startOfLine(5) = old startOfLine(5)（原行首不变）；③ 新 startOfLine(6) = old startOfLine(5) + 40 + 1（"前 40 字符 + 1 个插入的 \\n"= 新行 6 的起点）；④ 新 line 6 内容 = old line5 col40..end + '\\n'（新行 6 内容是旧 col40 以后的部分）；⑤ **视口渲染断言：原 line5 col40 之后的文本不再画在 line5 的 Y 位置，它出现在 line6 Y = line5 Y + fontHeight 像素的位置（paint spy：该文本的 boundingRect Y 差 = 约 1×fontHeight）**；⑥ saveNeeded=true（Enter 视为修改）；⑦ undo 一次后行数回到 Lcount_0 | 7 断言全过；核心是 ⑤（新内容真的画到新 Y 位置，不是「不可见字符」——证据⑪） | **P0** | ⚠️ 你原话实锤：「当前输入回车无法实现内容换行，只会多出一个不可见字符」——此 TC 直接验证 visual 层 bug（与 E004-BYTE 形成：字节流 OK / 显示层 FAILED，定位到 LineIndex/renderRange/computeLines 一致性问题） | keyPress Enter + lineCount+1 + 新 startOfLine(6) 计算 + content(line6) 正确 + paintSpy(boundingRect) 新 Y ≈ old Y+fontHeight + saveNeeded + undo 还原行数 |
| **E014** | **ON 模式下 Enter 插入新逻辑行：换行前后新逻辑行真的在行号栏出现，后续滚轮/方向键能正常进入新第二行** | ULTRA_LONG_LINE_10MB（单逻辑行，lineCount==1）→ word wrap ON → 定位到逻辑行 0 col ≈ displayStride×2 处（即某个显示行中段，随便选一个非首显示行中间位置也行）→ lineCount_0 = 1；按 Enter → ① lineCount == 2；② startOfLine(1) = col + 1；③ **行号栏 paint spy 看到新行号 "2"**（不是仍只显示 1）；④ scrollVertical 或按 Down 键 1 次后，光标能真的进入新逻辑行 startOfLine(1) 位置（不是仍在原逻辑行滚动） | 4 断言（超长行模式下 Enter 更难，因为 LineIndex/patch 新增 \n 后 computeLines 必须把它作为分行点，否则行数就不会 +1——这就是你说的「Enter 不换行」核心 bug） | **P0** | ⚠️ 证据⑪ 的超长行版本 | word wrap ON + lineCount_0=1 + key Return + lineCount==2 + startOfLine(1) computed + gutter has "2" + Down key 进入 line 1 |

---

## 11. 自动换行专项（W · 6 条，用户另一大分类 bug：「滚动条跳第二行」「无法看完整行」）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 用户原话 | SmokeBase |
|---|---|---|---|---|---|---|
| W001 | 超长行 scrollBar max/value 关系：value=0 → top cursor=0；value=max → top cursor≈end；value=mid → top≈mid×2 | ULTRA_LONG_LINE_10MB；sb.setValue(0/max/mid) + readViewportTopCursorByte | 0→C=0；max→C≈vs-1；mid→C≈vs/2（± 1 page 容差） | **P0** | ⚠️ 你：「下拉滚动条跳第二行」= sb 设置后内部被覆盖 | setValue + readTopByte + value→cursor 线性关系 |
| W002 | sb.setValue(2000) 后 processEvents ×2；sb.value() 仍=2000（不回弹！） | setValue(2000) → processEvents × 3 轮 → value===2000（容差 0） | value === 2000；**不被重写成 1/0/2** | **P0** | ⚠️ 你原话：「下拉滚动条会自动跳转到第二行」的数值证据（这就是 W002 的断言！） | setValue(2000) + processEvents×3 + value == 2000 exactly！ |
| W003 | 自动换行同一逻辑行（哪怕 13k 显示行）：行号栏只显示 1 次行号，其余显示行位置都空字符串或不画 drawLineNumber | paint spy drawLineNumber calls 次数：viewport 显示 60 条 display lines 同 1 逻辑行 → drawLineNumber 仅调用 1 次 | draw count = 1；其余要么 0 要么空 | **P0** | 你：「左边的行号是对的」（锁定这个行为不破坏） | paintSpy drawLineNumber callCount == 1 |
| W004 | 超长行滚动扫一遍：从 0 到 max 每步一屏滚动，收集每屏字节区间 union；最终 union == [0, vs) 全覆盖（没有任何字节段看不到 = 你「无法查看完整内容」的 bug） | scroll page by page from 0 → sb.max；对每屏 readByteRange(viewportTopByte, viewportBottomByte)；记录覆盖区间 → 做 union；最后 union == [0, vs) 全区间 | union === [0, vs)；无 gap | **P0** | ⚠️ 你：「无法查看这一行完整的内容」= 扫描覆盖不完整 | scrollPageLoop(0..max) + coveredRangeUnion === [0, vs) |
| W005 | 自动换行下每行高度相同：fontMetrics.height == 实际 display 行高（display line 0 vs 1234 高度相同，不因为某些字形变高） | Y 间距比较，display lines 高度差异 <= 1px（整型舍入） | 所有行高度一致 | P1 | - | drawContentLine Y before/after 差值断言（diff≤1） |
| W006 | 自动换行下水平滚动条 disabled（range==0 或 enabled==false）；切回 OFF 后水平 range>0、enabled=true | ON → hsb.enabled false；OFF → hsb.enabled true + range>0 | 2 状态断言 | **P0** | 自动换行不应有横向滚动条，用户易混淆 | horizontalScrollBar().isEnabled/range，两种 wrap 模式对比 |
| **W007** | **ON 模式超长行：非首显示行 caret 可见性（证据⑨）—— 即显示行 #N（N≥1）点击后，该 (X,Y) 真的画了 caret，不能消失/跳回显示行 0** | ULTRA_LONG_LINE_10MB；scrollVerticalDisplayLines(20) 前进 20 条显示行 → 当前 viewport 显示行 0 对应全局显示行 #20；clickAtDisplayLine(0, xPx=200)（点击全局显示行 #20 的 x=200）→ processEvents + 触发 1 次 paint；**用 paint spy 记录该次 paint 中 caretRect 的 boundingRect** | ① caretRect.top() 在 [viewportTop + 0, viewportTop + 2×fontHeight]（即真的画在当前点击的 viewport 显示行 0 = 全局 20 的 Y 范围内）；② **绝对禁止 caretRect.top() ∈ viewport 上方显示行 0 范围（即不是偷偷跳回画面外/原逻辑行首显示行）**；③ caretRect.width() 约等于 caretWidth（正常光标宽度） | **P0** | ⚠️ 你原话实锤「其它显示行无法放入光标」的 visual 证据 —— 与 C012 合起来形成：字节位置正确（C012）+ 视觉光标真画在该位置（W007），二者缺一不可 | scrollVerticalDisplayLines(20) + clickAtDisplayLine(0,200) + paintSpy(caretRect) + caretRect.top ∈ viewport + caretRect.top 不在显示行 0 起始范围 |
| **W008** | **ON 模式超长行：滚轮 1 格前进 display line 粒度，禁止一跳跳到下一逻辑行（证据⑩，数值硬断言）** | ULTRA_LONG_LINE_10MB（lineCount==1 的超长单逻辑行！—— 这里只有 1 条逻辑行，所以「跳到下一逻辑行」= 直接到 EOF）→ viewport 视口高=10 display lines 则一页能显示超长逻辑行的连续 10 条显示行片段；**执行 wheelEvent 一格 (delta=120, step=3 display lines 典型)** → 记录 topByte T1；再 wheelEvent × 2（累计 9 display lines）→ T3；再 wheelEvent × 8（累计 33 lines）→ T33 | ① **最核心断言：当 lineCount==1 时，任何 N 格 wheel 之后 **lineAtTop() == 0 仍然成立**（因为只有一条逻辑行！）—— 这对应你原话「本来只有 1 条物理行，滚一格就直接跳到 EOF 或物理第二行（不存在！）」的 FAIL 模式 = lineAtTop() !=0 = FAIL；② 序列 T1 < T3 < T33 严格递增（前进）；③ T33 ≈ 33 × displayStride（±10%，累积增量）；④ **（若有 2 条逻辑行场景再补）** lineCount=2 时，滚 N 格之后 lineAtTop() 仍 ==0 直到滚过 line0 全部显示行，不能一步跳到 lineAtTop=1 | ①②③④（尤其 ① 是「1 条物理行场景下跳第二行不存在=必崩 FAILED」的硬证据，证据⑩） | **P0** | ⚠️ 你原话实锤「当前可视窗口只能显示 10 行，而自动换行下这个超长行占据 100 行，滚动滚轮会直接跳到物理第二行」的精确数值断言（证据⑩）—— 这里特意用单逻辑行 fixture，就是为了让「跳第二行（不存在）」直接 FAIL | 单逻辑行 fixture lineCount=1 + wheelEvent×N + lineAtTop() 恒 == 0 + T 严格递增 + T33≈33×stride |
| **W009** | **ON 模式超长行：wheel 上下滚动往返对称** | ULTRA_LONG_LINE_10MB；scrollVerticalDisplayLines(100) 前进 100 条显示行 → 记录顶部 Tf=~100 stride；wheelEvent 反向 33 格（回退 99 display lines）→ 记录 Tb | Tb ≈ Tf - 99 × stride（±10% 回退到位）；不能「前进 1 格 = 跳 100 行，后退 1 格 = 回 1 行」这种不对称性 | P1 | 滚动对称性常见 bug | wheelEvent forward N then backward N + Tfinal ≈ T0 |

---

## 12. 行号栏 & 状态栏信息（I · 5 条）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | SmokeBase |
|---|---|---|---|---|---|
| I001 | 状态栏行/列字节：click L/C → statusBar "行: <L+1> 列: <C+1>"（1-based 显示） | 对 (0,0)/(5,10)/(19,79)；检查 statusBar 文本 regex match | match regex "行:\\s*\\d+\\s*列:\\s*\\d+"；数值正确 | **P0** | statusBar regex |
| I002 | 状态栏字节位置：cursor=1234 → status 包含 "1234"（字节口径，不是 UTF-16 字符数） | gotoByteOffset(1234) + statusBar substring "1234" present | status contains "1234" | **P0** | 字节口径一致性 | gotoByteOffset + status contains |
| I003 | 修改星号：saveNeeded=true 时 title 前置 `*`；false 时去掉 → clean→edit(星号出)→save(星号消)→edit(星号出)→undo(星号消) 4 次过渡 100% 正确 | 4 次状态切换；titleStartsWithAsterisk(saveNeeded==true) 对应 | 4 次都对 | **P0** | saveNeeded + title starts with '*' 过渡 | save/edit/save/edit/undo × titleStartsWithAsterisk check |
| I004 | L2 模式标识：L2 打开 → status 包含 "L2"或 "大文件"；L1 打开 → status 不含（或显示 L1） | L2 vs L1 tab 切换，statusBar contains L2 flag | L2 has, L1 not | P1 | 模式不混淆 | statusBar contains("L2") |
| I005 | 编码显示：切换 encoding menu 转为 UTF-8BOM → status 编码字段显示 "UTF-8 BOM"；转为 GBK → 显示 "GBK/ANSI" | 2 种切换 + status label contains encoding name | name matches | P1 | 编码名正确 | trigger encodingMenu + status label |

---

## 13. 边界字符（B · 7 条；不是压测，是编辑器基础正确性）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | SmokeBase |
|---|---|---|---|---|---|
| B001 | 空文件：打开 → 输入 100 char → save → reopen → full==输入；no crash | open(empty) + type "hello 100 chars" + save + reopen + bytes equal | 100% identical；no crash | **P0** | 空文件经典崩溃点 | openEmptyFixture + type/save/reopen/fullDiff |
| B002 | 末尾无换行（no trailing \n）：打开后 vs == fixture.len（不短少/多加 \n）；光标 goto EOF = vs（不落在行内）；插 "Z" → 保存后最后字节 = 'Z'（不自动多加 \n） | fixture 5 lines no trailing NL → vs check + goto EOF + insert 'Z' + save disk last byte == 'Z' | last byte == 'Z'；no extra '\n' added | **P0** | 编辑器经典 "末尾多加换行" bug | vs==fixtureLen；cursor at EOF==vs；disk last byte |
| B003 | 含 NUL 0x00 字节：打开 → virtualSize == fixture 10_000（不短！因为 C 字符串到 \0 止）；line 划分正确；NUL 前后字节正确读；保存后 NUL 字节仍然 0x00（不被 strip） | vs==10_000；bytes around NUL position (before, after, at) correct；disk bytes after save still has 0x00 | vs == fixture.size；NUL preserved | **P0** | 大文件能处理二进制文件的底线 | vs assertion + bytes around NUL position + disk preserved NUL |
| B004 | 非法 UTF-8：打开 fixture 包含 0xC3 0x28 不完整序列 → ① readByteRange 不 crash；② 保存 → disk bytes === fixture bytes（**不被偷偷替换成 replacement char**！编辑器绝不改写用户字节流） | save disk === fixture bytes exactly；no crash | disk identical to fixture（100% 逐字节） | **P0** | 用户字节流 0 破坏 > 显示好看 | save + diskDiff(fixture) 100% identical；no crash |
| B005 | Emoji / 组合字符（4B UTF-8 / combining）：click 落点在字符边界；插字符不写坏；UTF-8 decode valid | fixture "🚀中文a̐"；click at glyph position；cursor 落在字节边界（不落在 4B 中间）；insert X → nearby 6B UTF-8 decode no replacement char | cursor boundary；utf8 valid；no crash | P2 | - | click glyph + boundary% assertion + insert + utf8Valid() |
| B006 | 10MB 超长单行（非压测 100MB 超大）：打开 + scroll 3 次 + click 2 位置 + 插 10B → 不崩溃；光标/字节正确 | open(10MB single 'A')；scroll 3 positions；click 2 bytes；insert 10 'X' → all no crash；inserted at correct bytes | no crash；inserted bytes correct | **P0** | 真超大单行的基本可用性（不是压测，是"能不能用"） | open 10MB + scroll×3 + click×2 + insert "X"×10 + no crash |
| B007 | 文件名含中文/空格/多字节字符：open path "中文 文件名 有空格.txt" → 成功；L2 加载正确；save；reopen bytes | open successfully；bytes round trip | **P0** | 中文路径兼容性 | open path with CJK/spaces + save/reopen/fullDiff |

---

## 14. 拖放 & 帮助 & 多标签 & 最近文件（H · 8 条）

| TC ID | 标题 | 步骤 | 预期 | 优先级 | SmokeBase |
|---|---|---|---|---|---|
| H001 | 拖入文件打开：MainWindow 接收 QDragEnter/QDrop 事件 → 若当前 tab 是 empty untitled 则复用；否则 tab+1；open 成功 | 模拟 QMimeData(urls=[fixture path]) → dragEnterEvent + dropEvent → tabCount change；path open ok | tab count right；bytes correct | **P0** | 拖放打开 | simulateDrop(urls=[path]) + tab count + bytes correct |
| H002 | 多标签切换：open fileA（L2）→ 新建 Ctrl+N（tab 2）→ open fileB（tab 3）→ 切换 tab 0/1/2 → 各 tab 的 isL2/wordWrap/saveNeeded/viewModeL2 状态独立不串 | each tab state preserved when switching；no cross-contamination | **P0** | 多标签状态独立 | create 3 tabs + switch + each state assertion preserved |
| H003 | 标签关闭 Ctrl+W（或 tab 关闭按钮）：2 个 tab 打开 → Ctrl+W 路径 A MainWindow 快捷键 → tab count-1；若有修改弹 F003 保存对话框（mock Cancel → 不关闭 tab） | same assertions as F003 but for tab close | **P0** | 标签关闭链路 | keySeq Ctrl+W + tabCount + mockCloseDialog(Cancel) |
| H004 | 最近打开菜单 rebuild：open a/b/c → rebuildRecent → 最近打开 actions count==3；第 0 个 action text contains c（最近）；第 2 个 contains a（最远）；action 0 trigger 打开 c 成功 | order c/b/a；trigger 打开 ok | P1 | 最近打开排序/重建 | recentActionsTexts() + assertOrder(c,b,a) + triggerAction(0) opens c |
| H005 | 恢复设置启动：m_wordWrap=true / dark=true / edgeColumn=100 / encoding=UTF-8BOM → MainWindow 重新实例化时菜单勾选状态同步（syncGlobalMenuCheckStates 生效） | new MainWindow with settings → view menu actions checked states === 设置值 | all checked right | P1 | 启动后菜单勾选一致 | readSettings + new MainWindow + checkedState assertions |
| H006 | 帮助菜单（如存在 "关于"）：trigger aboutAct → QMessageBox 弹；关闭不崩 | if helpMenu exists → trigger aboutAct + dialog found；closed no crash | P2 | - | trigger + dialog visible |
| H007 | 同时打开 3 个 tab：1 个 L2 + 2 个 L1 → 切换过程中没有 crash；MainWindow::onTabChanged 对三种 tab 都正确 syncEncodingMenu / wordWrap / edge settings | switch tab 0→1→2→0 2 循环；no crash；encodingMenu state per tab matches | no crash；per-tab state matches | **P0** | Tab switch crash 常见 | 3 tabs mixed L1/L2 + switch N×2 + no crash + state match |
| H008 | 标题栏更新：另存为 newPath → tab title 显示 newPath（basename）；主窗口 windowTitle 含 newPath；saveNeeded=false 时没有 `*` 前缀；saveNeeded=true 时有 `*` | tab title endsWith basename(newPath)；windowTitle contains newPath；saveNeeded toggles `*` prefix | 3 assertions | **P0** | 标题信息正确 | tabText endsWith basename；windowTitle contains path；title asterisk transitions |

---

## 15. L2 模式下禁用不适配功能的菜单机制（D · 7 条，用户原话：「行排序和编码转换不适合 L2，应该禁用」；证据④⑬）
> **禁用原则（用户强制）**：对 L2 超大文件不适合、或代码未真的实现（如编码转换"只改标志不转码"证据⑬）、或排序需要 O(n log n) 全文件内存加载的菜单项，**L2 tab 打开/切换到 L2 tab 时必须自动 setEnabled(false) 变灰**，用户点不动；切回 L1 tab 时若 L1 支持则还原 enabled=true。触发点在 `MainWindow::onTabChanged` 内统一检查 currentTab()->isL2Active()。禁用范围（首批）：
> - **编辑菜单**：行升序排序 / 行降序排序（2 项，证据④）
> - **编码菜单**：转为 GBK / UTF-8 / UTF-8BOM / UTF-16LE / UTF-16BE（5 项，证据⑬，"全量转码"对 L2 超大文件不可做，且当前代码没真的转码，证据⑬ MainWindow::convertEncodingFromAction 只改标志没转字节）
> - **额外考虑**：书签 4 项如果未实现，也应先在 L2 禁用（enabled=false + 提示），避免点进去没反应。

| TC ID | 标题 | 步骤 | 预期 | 优先级 | 证据 | SmokeBase |
|---|---|---|---|---|---|---|
| **D001** | **L2 打开后：行升序/降序两个动作 → enabled=false（菜单灰）** | MainWindow 启动（无 tab）→ openL2 small（isL2Active=true）→ 读取 sortAscAct 和 sortDescAct 的 enabled 状态 | sortAscAct.enabled==false；sortDescAct.enabled==false（**点不动，不是点进去再 FAIL**） | **P0** | ⚠️ 你原话「行排序不适合 l2 超大文件编辑」（证据④）+ EditorTab::sortAsc/sortDesc 全 Scintilla L2 全空（证据④） | open L2 + actionEnabled(sortAscAct)==false + actionEnabled(sortDescAct)==false |
| **D002** | **切 tab 同步：L1 small tab → L2 tab → L1 tab，行排序动作 enabled 自动切换 true→false→true** | open 3 tabs：tab0=L2；tab1=L1 small；tab2=L1 small → start at tab1 (L1) → sortAsc/sortDesc enabled=true → switchToTab(0)（L2）→ enabled=false → switchToTab(2)（L1）→ enabled=true → switchToTab(0) → false | 4 次状态切换 **100% 正确**，没有一次错位（L2=true/L1=false 都是 bug） | **P0** | onTabChanged 必须同步菜单项状态；常见忘记切回的 bug | create 3 tabs mixed + switch N×2 + sortAsc/sortDesc enabled state 4 次 transition exact |
| **D003** | **L2 打开后：编码「转为...」5 项 → enabled=false（全变灰，点不动）** | openL2 small → 读编码菜单的「转为 GBK/UTF-8/UTF-8BOM/UTF-16LE/UTF-16BE」5 个动作的 enabled | 5 个动作.enabled 全部 = false（**"转为..."组全灰；"以编码显示..."组仍可点=正常，因为它不改字节流**）；状态栏/tooltip 可有 "L2 暂不支持编码转换" 提示（可选但优先） | **P0** | ⚠️ 证据⑬ + 你原话「编码转换不适合 L2」：当前代码 convertEncodingFromAction **只改标志没真转码**（证据⑬，会导致切 UTF-16LE 保存写 BOM 但正文字节仍 UTF-8 → 乱码）；且超大文件做"全量 re-encode"也不现实 | open L2 + for each 5 convertEnc actions: enabled==false + displayEnc 5 actions: enabled==true（允许只改变显示解码） |
| **D004** | **切 tab 同步：L1→L2→L1，编码转为组 enabled 同步，以编码显示组始终 enabled** | tab0=L2；tab1=L1 → tab1 (L1): convert 5 项若产品实现则 true，未完成先禁用；切到 tab0 (L2): convert 5 项必须 false；切回 tab1: 还原（true 或 保持 false，跟 L1 状态一致）；display 5 项全程 true（不改字节流的显示解码，L2/L1 都可） | convert 组状态随 tab 切换正确；display 组始终 enabled=true（始终可点） | **P0** | 同上（D003），切 tab 必须同步状态 | mixed tabs + switch + convert/display actions enabled check |
| **D005** | **（作废，不需要防御性代码；仅保留作为文档说明）用户强制：L2 禁用只靠 UI 层 setEnabled(false) 变灰点不动，不允许在 convertEncodingFromAction/sortLinesAsc/sortLinesDesc 内部加 L2 early return 引入不必要复杂度** | open L2 small → 直接「菜单项 enabled 状态」是唯一要检验的对象（即 D001/D003）；任何 API 强行触发的防御性 TC 一律不写不跑 | 仅 D001~D004 就足够；D005 从 P1 降级为作废，不再编写 smoke 代码 | **（作废）** | 用户原话：「直接在菜单将对应功能设为 disable 就行了，不要引入不必要的复杂度。快速回滚冗余代码」 | 不需要对应 SmokeBase helper（删掉）|
| **D006** | **（可选但建议）书签 4 项 L2 下先禁用 enabled=false / statusBar 提示「L2 暂不支持书签」；点进去至少不崩**（因为证据⑤ EditorTab toggleBookmark 等 4 项全 Scintilla 代码 L2 全空，目前如果直接点进去是什么都不发生，用户不知道原因） | openL2 → bookmarkMenu 4 个动作.enabled 均 false；或：Ctrl+F2 (BM01) 路径 A 触发后，statusBarContains("书签") 或 "暂不支持" 字样；saveNeeded=false；no crash | **最低：不崩 + saveNeeded=false**（P0 底线）；推荐：enabled=false + 友好提示（P1 建议） | **P0**（底线）/ P1（友好） | 证据⑤：书签 4 项 100% Scintilla，L2 全空 | open L2 + 4 bookmark actions enabled==false（推荐）+ trigger Ctrl+F2 → no crash + saveNeeded=false + statusBarContains("暂不支持"/"书签") |
| **D007** | **D001~D006 综合：无 tab → 新建 L1 tab（所有动作按 L1 状态）→ open L2（禁用动作立即同步变灰）→ 新建 L1（动作立即恢复）→ 关闭最后一个 tab（无 tab 状态下所有依赖 tab 的动作都变灰或符合产品无 tab 状态策略）** | 完整启动生命周期 4 个阶段切换 → 各阶段动作 enabled 完全符合预期；任何阶段没有对不存在 tab 的动作误触发 enabled | 全阶段状态正确 | **P0** | MainWindow 启动/关闭 tab 的状态机完整性 | lifecycle no-tab → create L1 → open L2 → create L1 → close all tabs → each phase actions enabled assertions |

---

## 附录 A. SmokeTestBase Helper 需要新增（按 TC 实际使用汇总）
> 保证 TC 可机械实现（不是"口头 TC"）

| Helper 方法签名 | 用途 | 被 TC 引用 |
|---|---|---|
| **MainWindow 级 fixture**（因为路径 A = MainWindow 真实快捷键） | `SmokeMainWindowTestBase`（继承自原 SmokeTestBase，额外持有真正 MainWindow 实例，不是裸 QWidget + EditorTab 栈） | 所有 `-A` 路径 A TC：ED01A-F3A/SR01A/BM01/VW02A… |
| `keySeqToMainWindow(const QKeySequence& seq)` | 向 MainWindow 发送快捷键（QShortcut/QAction triggered） | 所有路径 A 快捷键 |
| `triggerActionByMenuPath(const QStringList& menuPath)` 或 `triggerActionByName(QString act_member_name)` | 触发指定菜单动作（如 `findAct` / `sortAscAct` / `bookmarkMenu->actions()[1]`） | 所有路径 B（菜单点击） |
| `tabCount()` → int；`tabTitle(int idx)` → QString | 标签数目/标题断言 | F001/F002/H003 |
| `mockFileDialogAcceptReturnPath(const QString& path, AcceptMode mode=Open/SaveAs)` | mock QFileDialog 返回路径 | F002/F004 |
| `mockMessageBoxCloseDialog(ButtonRole role=Save/Discard/Cancel)`（mock "关闭修改提示"） | mock QMessageBox 关闭对话框按钮选择 | F003/F006 |
| `mockGotoLineDialog(int lineNo)` | mock goto 对话框输入行号 | C009 |
| `fileOnDiskBytes(const QString& path)` → QByteArray；`setFileReadOnly(const QString& path, bool readonly)` | 磁盘字节读回；设置只读属性 | F001-F008 |
| `titleStartsWithAsterisk()` → bool；`statusBarText()` → QString；`statusBarContains(QString regex)` | 标题/状态栏 UI 状态断言 | I001-I005 + F003/F005/E012/H008 |
| `saveNeeded()` → bool；`encoding()` / `displayEncoding()` → enum；`wordWrap()` / `isL2Active()` / `viewModeL2()` → T | EditorTab 公开属性（已有）+包装 | 所有状态/模式切换 |
| `setClipboardBytes(const QByteArray&)`；`clipboardBytes()` → QByteArray | 剪贴板读写 | ED03/04/05/E009/010/011/S001-S008 |
| `spyPatchPressureWarn()` → QSignalSpy（或自定义计数器） | 压力提示信号捕获 | ED05C |
| `viewportBackgroundColor()` → QRgb；`viewportContainsVerticalLineAtX(int xPx)` → bool | 视图断言（深色模式、长行标尺） | VW01/VW03 |
| `scrollVerticalPage(int pages)` / `scrollPageLoopCollectCoverage()` / `coveredRangeUnion()` → pair<qint64,qint64> | W004 全可见覆盖扫描 + W001 滚动 | W001/W004 |
| `scrollBarSetValue(int value)` / `scrollBarValue()` → int / `scrollBarMaxValue()` → int | W002 setValue 不回弹核心断言 | W001/W002 |
| `horizontalScrollBarRange()` / `horizontalScrollBarEnabled()` → bool | W006 自动换行水平滚动条断言 | W006 |
| `readViewportTopCursorByte()` / `readViewportBottomCursorByte()` → qint64 | 视口顶/底部字节 offset | W001/W004/C003/C004 |
| `clickAtDisplayLine(int viewportDisplayRow, int xPx = 10)` / `scrollVerticalDisplayLines(int dLines)` | 自动换行 click 粒度 | C003/C004/C007 |
| `logicalLineOfByte(qint64 byteOffset)` → int / `displayStrideBytesApprox()` → int | 自动换行语义判断 | C003/S003 |
| `dragSelectByteRange(qint64 startByte, qint64 endByte)` → sel size（S002 基准） / `dragSelectAcrossDisplayLines(int startDisplay, int x1, int endDisplay, int x2)` | 拖选跨 display line | S002-S004 |
| `triggerSyntaxMenu(int syntaxEnum)` / `triggerBookmarkToggle / nextBm / prevBm / clearBm` | 语法书签辅助 | SY01-09/BM01-04 |
| `triggerEdgeColumnMenu(int col)` / `edgeColumnProperty()` → int / `edgeVisibleProperty()` → bool | 长行标尺 | VW03/VW04 |
| `triggerConvertEncoding(TextEncoding enc)` / `triggerDisplayEncoding(TextEncoding enc)` + mock convertCancel/Yes | 编码菜单 | EC01-10 |
| `mouseDClickAtByte(qint64 byte)` / `mouseTripleClickAtLine(int logicalLine)` | 双击选词/三击选行 | S006/S007 |
| `utf8DecodeValid(const QByteArray& bytes)` → bool；`countOccurrences(const QByteArray& hay, const QByteArray& needle)` → qint64；`readByteRangeFull()` → QByteArray | 常用工具函数（多条 TC 使用） | R003/SR06/C011/B005 |
| `simulateDropFileUrls(const QStringList& paths)` | H001 拖放打开 | H001 |
| `applySettingsThenRestartMainWindow(...)`（构造新 MainWindow 应用相同设置） | H005 恢复设置一致性 | H005 |
| `zoomFontHeight()` → int（读 viewport fontMetrics height，用于 VW07 放大缩小断言） | VW07 zoom 递增/递减/复原 | VW07A/B/C |
| `switchToTab(int idx)`；`verifyAllTabsStatesPreserved(QVector<TabState>& expected)` | H002 多标签切换状态独立断言 | H002/H007 |

---

## 附录 B. 按「MainWindow 快捷键/菜单项 → TC ID」1:1 映射速查表
> 保证**没有任何一条快捷键或菜单项被遗漏**（共 31 条快捷键 + 全部子菜单项）

### 文件菜单（6 条目）
| 原文 | 快捷键 | TC ID（L2 相关） |
|---|---|---|
| 新建(&N) | Ctrl+N | F001-A / F001-B |
| 打开(&O) | Ctrl+O | F002-A / F002-B / H001 |
| 最近打开(&R)（子菜单） | - | F002-B / H004 |
| 保存(&S) | Ctrl+S | F003-A / F003-B / F007 / F008 |
| 另存为(&A) | Ctrl+Shift+S | F004-A / F004-B |
| 退出(&X) | Ctrl+Q | F006-A / F006-B |

### 编辑菜单（20 条目=6 编辑+6 行操作+2 排序 + 6 条分隔）
| 原文 | 快捷键 | TC ID（L2 相关） |
|---|---|---|
| 撤销 | Ctrl+Z | ED01-A (路径 A 已知 FAIL!) / ED01-B |
| 重做 | Ctrl+Y | ED02-A (已知 FAIL!) / ED02-B |
| 剪切 | Ctrl+X | ED03-A (已知 FAIL 双重!) / ED03-B / E009 |
| 复制 | Ctrl+C | ED04-A (应 OK) / ED04-B / E010 |
| 粘贴 | Ctrl+V | ED05-A / ED05-B / ED05-C(大粘贴) / E011 |
| 全选 | Ctrl+A | **ED06-A (已知 FAIL 实锤「全选无反应」！) / ED06-B / S001-A/B** |
| 复制当前行 | Ctrl+D | ED07 (已知 FAIL!) |
| 删除当前行 | Ctrl+Shift+L | ED08 (已知 FAIL!) |
| 上移当前行 | Alt+Up | ED09 (已知 FAIL!) |
| 下移当前行 | Alt+Down | ED10 (已知 FAIL!) |
| 行升序排序 | - | ED11 (已知 FAIL!) |
| 行降序排序 | - | ED12 (已知 FAIL!) |

### 搜索菜单（6 条目）
| 原文 | 快捷键 | TC ID |
|---|---|---|
| 查找(&F) | Ctrl+F | SR01-A / SR01-B |
| 替换(&R) | **Ctrl+R**（与菜单括号 &R 对齐，见 SR02-COMFORT；**禁止使用 Ctrl+H**，Notepad++ 用户习惯 Ctrl+R） | **SR02-A / SR02-B / SR02-COMFORT（菜单文本含 &R 且 shortcut===Ctrl+R 硬断言 3 条全过）** |
| 查找下一个 | F3 | SR03 + R004 |
| 查找上一个 | Shift+F3 | SR04 |
| 替换/查找对话框内部按钮（查找下一个/上一个/替换/替换全部/取消/选项） | - | SR05-SR07 + R002/R003/R005 |

### 语法菜单（9 条目）
| 原文 | 快捷键 | TC ID |
|---|---|---|
| 纯文本 / C++ / Python / JSON / HTML / XML / SQL / YAML / Markdown（ActionGroup 9 项） | - | SY01-SY10 |

### 书签菜单（4 条目）
| 原文 | 快捷键 | TC ID |
|---|---|---|
| 切换书签(&M) | Ctrl+F2 | BM01 (已知 FAIL!) |
| 下一书签 | F2 | BM02 (已知 FAIL!) |
| 上一书签 | Shift+F2 | BM03 (已知 FAIL!) |
| 清除所有书签 | - | BM04 (已知 FAIL!) |

### 视图菜单（16 条目 = 8 toggle/action + 1 子菜单 = 6 edgeCols + 分隔符）
| 原文 | 快捷键 | TC ID |
|---|---|---|
| 深色模式 | - | VW01 |
| 自动换行 | Ctrl+W | VW02-A / VW02-B + (关联 W001-W006, C003-C007) |
| 长行标尺(竖线) | - | VW03 |
| 长行标尺列数（72/80/100/120/150/200，6 条目） | - | VW04-A×6 |
| 16 进制编辑模式 | Ctrl+Shift+H | VW05-A / VW05-B / VW06 |
| 放大 | Ctrl++ / Ctrl+= | VW07-A (已知 FAIL：MainWindow 只调 Scintilla editor()) |
| 缩小 | Ctrl+- | VW07-B (已知 FAIL) |
| 重置缩放 | Ctrl+0 | VW07-C (已知 FAIL) |

### 编码菜单（11 条目）
| 原文 | 快捷键 | TC ID |
|---|---|---|
| 转为 ANSI / UTF-8 / UTF-8BOM / UTF-16LE / UTF-16BE（5 条） | - | EC01-EC05 × 多个 scenario + ED11 |
| 以编码显示 ANSI / UTF-8 / UTF-8BOM / UTF-16LE / UTF-16BE（5 条） | - | ED06-ED10（字节流不破坏） |

### 多标签 & 标签关闭 + 拖放 & 帮助（额外 8 条）
| 原文（不在主菜单栏，但是常用交互） | 快捷键 | TC ID |
|---|---|---|
| 标签关闭 | Ctrl+W / Ctrl+F4 | H003 + F003（关闭 prompt） |
| 新建标签（Ctrl+N 打开新 tab） | Ctrl+N | F001-A |
| 拖放文件打开 | Drag & Drop | H001 |
| 多标签切换状态独立 | - | H002 / H007 |
| 标题栏更新（保存/修改/另存为） | - | H008 + I003 |
| 最近打开重建 | - | H004 |
| 启动恢复设置 | - | H005 |
| 帮助关于（如有） | - | H006 |

---

## 附录 C. 全局交付门槛（更新版，严格按你的要求）
1. ✅ **P0 全 PASS = 可交付最低门槛**（现在表格中 P0 数量 ≈ 90+；每条 FAIL 必须按 §4 质量方法论先 FAIL 测试 → 数值证据 → 修复 → PASS）
2. ✅ **所有 MainWindow 31 条快捷键 在 L2 下 100% 走通路径 A**（每条快捷键都有 TC，现在 Appendix B 已 1:1 覆盖，没有遗漏）
3. ✅ **所有子菜单项（含语法/书签 13 项、视图 16 项、编码 11 项） 每个至少 1 条 TC**（Appendix B 1:1）
4. ✅ **删除全部 P 类压测 TC**（用户明确要求不做压力测试，本文档 v2 已移除所有 P 类大文件容量 TC；仅有 B006=10MB 基本可用性、不属于压测）
5. ✅ **所有「已知 FAIL」条目（证据①-⑭）必须在修复前用 GUI smoke 先捕获 FAIL 数值证据**（否则禁止动代码）

---

## 16. 按功能集中度分组 · 攻坚顺序（1→8 按 ROI 从高到低逐个做）

> **用户强制策略**：先修上面明确的小问题（快速获得胜利）→ 再把冒烟测试按功能集中度分组 → **一个组一个组地做：先在对应 smoke_*.cpp 里写/跑失败 smoke 捕获数值证据 → 修 → 再跑该组变绿 → 进入下一组**。严禁跨组乱改代码导致互相 regress。
>
> 每组末尾标注：
> - ✅ **已修条目**（代码已改但尚未跑 GUI smoke 实锤变绿）
> - 📂 **对应 smoke 目标文件名**（需单独编译成独立 EXE，或合并到同一 cpp）
> - 📊 **P0 数量预估** / **总体复杂度 S/M/L/XL**

---

### 组 1 · 小修复组（快捷键 + 菜单禁用 + 纯 UI 标志位类）
**目标**：最快速度拿一批 PASS（全是 1~50 行代码的纯 UI 层改动，无复杂坐标变换）

| TC ID | 状态 | 说明 |
|---|---|---|
| SR02-COMFORT | ✅ 代码已修（待 smoke 实锤） | replaceAct shortcut == Ctrl+R 精确；text 含 &R；访问键 Alt+S+R 可打开 |
| D001 | ✅ 代码已修（解决了「同一 tab 内部 L1 切 L2 currentChanged 不触发 → enabled 不更新」的根因：EditorTab::l2ModeStatusChanged 信号 + MainWindow::syncL2SensitiveActions 统一同步） | L2 打开 → sortAscAct / sortDescAct enabled=false |
| D002 | ✅ 代码已修 | 切 tab L1→L2→L1 → sort 动作 enabled 同步 true→false→true |
| D003 | ✅ 代码已修 | L2 打开 → 编码「转为...」5 项 enabled=false；显示组仍 enabled=true |
| D004 | ✅ 代码已修 | 切 tab L1→L2→L1 → 编码转为组 enabled 同步；显示组恒 true |
| D005 | ❌ **用户强制作废（不要防御性代码）** | D005 不再执行；TC 表第 315 行已标注"作废"。禁用仅依赖 UI setEnabled(false)=变灰点不动=用户最直接体验，拒绝函数内 early return 复杂度 |
| D007 | ✅ 代码已修 | 无 tab → 新建 L1 → 打开 L2 → 新建 L1 → 关闭所有 tab，生命周期动作 enabled 全正确 |

📂 **目标 smoke 文件**：`smoke_smallfixes_menu_ui.cpp`（或合并到 `smoke_shortcuts.cpp` + `smoke_menu_disable_l2.cpp`）  
📊 **P0：6 条（D005 已作废，从原 7 条减到 6，不再 count 防御性 TC）** · 复杂度 **S**（估计半天内全部 PASS）  
✅ **代码改动文件（精简，严格遵循「不要引入不必要复杂度」：禁用只靠 UI enabled=false，函数内绝无 early return 冗余）**：
- 替换快捷键对齐 [MainWindow.cpp#L274-L278](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L274-L278)（1 行：Ctrl+R 精确，不用 QKeySequence::Replace=Ctrl+H）
- L2 动作成员收集 [MainWindow.h#L112-L115](file:///F:/projects/credentials/notepad-safe/MainWindow.h#L112-L115) + [MainWindow.cpp#L259-L265](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L259-L265)（sortAsc/Desc）+ [MainWindow.cpp#L399-L406](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L399-L406)（编码转为组 5 项 m_convertEncodingActions）
- **根因修复（你反馈「构建测试了仍然可点击」）：EditorTab 同一个 tab 内部 L1↔L2 切 currentChanged 不发射 → 新增信号 + 显式同步**：
  - [EditorTab.h#L204-L207](file:///F:/projects/credentials/notepad-safe/EditorTab.h#L204-L207) 信号声明 `void l2ModeStatusChanged()`（无参数，避免 bool 与实际模式不一致 bug）
  - [EditorTab.cpp#L222-L223](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L222-L223) / [L1039-L1041](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1039-L1041) / [L1262-L1264](file:///F:/projects/credentials/notepad-safe/EditorTab.cpp#L1262-L1264)：所有 `m_l2Mode = true/false` 赋值处 `emit l2ModeStatusChanged()`
  - [MainWindow.h#L87-L90](file:///F:/projects/credentials/notepad-safe/MainWindow.h#L87-L90) + [MainWindow.cpp#L1043-L1054](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L1043-L1054)：统一同步函数 `syncL2SensitiveActions(EditorTab* tab=nullptr)` 入口单一，无冗余
  - [MainWindow.cpp#L552-L557](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L552-L557)：addNewTab 内对每个新 tab 连接 `l2ModeStatusChanged` → syncL2SensitiveActions（同一 tab 内部模式切换立即响应）
  - [MainWindow.cpp#L559-L562](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L559-L562)：新建 tab 后立即 syncL2SensitiveActions(tab)（双重保险，避免早期空状态无信号）
  - [MainWindow.cpp#L1056-L1066](file:///F:/projects/credentials/notepad-safe/MainWindow.cpp#L1056-L1066)：onTabChanged 里也统一调 syncL2SensitiveActions（切 tab 场景 + index<0 无 tab 全禁用）

**⚠️ 铁律（用户强制）**：D001~D004/D007 的唯一检验对象是「菜单项的 `.isEnabled()` 状态」；**严禁在 sortLinesAsc/sortLinesDesc/convertEncodingFromAction 三个函数体内加任何 L2 特判**，已回滚到最小实现。

---

### 组 2 · 搜索替换组（Find/Replace/Replace All 链路）
**目标**：用户最高频功能；且现有 `smoke_find_replace.cpp`（gui_smoke_findreplace.exe）框架已经有，只需扩充 + 修。

| TC ID | 关键断言 |
|---|---|
| SR01-A/B | Ctrl+F 打开查找对话框 |
| SR02-A/B + SR02-COMFORT（组1也有） | Ctrl+R 打开替换对话框 |
| SR03 + R004 | F3 查找下一个 wrap=off 3 次严格递增（430<2056<3682） |
| SR04 | Shift+F3 查找上一个递减 |
| R002 | 单步替换不插在 OLD 旁边（prefix+NEW+suffix，OLD 出现次数=0） |
| R003 | 替换全部 n=3，OLD 残留 0，NEW 计数=3 |
| SR05 | **patch 插入新内容后查找**（LargeFileSearch 必须走 renderRange 而非直接读 mmap）|
| SR06 | 0 处 replaceAll 字节流 100% 不变（无副作用） |
| SR07 | replaceAll 一次 undo 回到原（group 语义） |

📂 **目标 smoke 文件**：`smoke_find_replace.cpp`（= 已有 `gui_smoke_findreplace.exe`，扩充新 TC）  
📊 **P0：10 条** · 复杂度 **M**（LargeFileSearch 的 patch 渲染链路可能是坑，SR05 必 FAIL 先捕获）

---

### 组 3 · 基础编辑操作组（输入/删除/换行/撤销重做/剪切粘贴/全选）
**目标**：核心编辑链路；与现有 `smoke_coord_editops.cpp` 框架相邻，可以在同一 EXE 加新 test case。

| TC ID | 关键断言 |
|---|---|
| ED01-A/B ~ ED06-A/B | undo/redo/cut/copy/paste/selectAll 双路径 A+B；含「全选 selSize==vs」「剪切真删选中字节」 |
| E001~E003 | 插/Backspace/Delete 行边界正确性，下方行无水平偏移 |
| **E004 + E004-BYTE + E004-VISUAL + E014** | Enter 真换行（字节流 0x0A 实锤 + 显示层 lineCount+1 + Y 差=fontHeight + 超长行下 Enter lineCount=2 行号栏出"2"）|
| E006~E008 | 1 步/100 步 undo/redo 精确还原 |
| E009~E011 | 剪切 1000B 真删 / 复制不变 / 粘贴 1KB vs+1024 |
| ED07~ED10 | 复制/删除/上下移行（4 项，若 L2 未实现则至少不崩，P0 底线）|

📂 **目标 smoke 文件**：`smoke_editops_basic.cpp`（或并入 `gui_smoke_coord.exe` 的新增 test class）  
📊 **P0：约 18 条** · 复杂度 **L**（E004-VISUAL 显示层真分行和 E014 超长行换行是硬骨头，直接对应你「Enter 不可见字符」原话）

---

### 组 4 · 自动换行 滚动 + 光标定位组
**目标**：你原话最严重的 4 个 bug（①非首显示行无光标 ②滚轮跳物理行 ③非首显示行拖选失败 ④Enter 不换行）集中在这里，是**最大用户痛点**；E004 已放组 3，W/C 相关都在本组。

| TC ID | 对应症状 |
|---|---|
| C001~C002 | OFF 模式基础点击定位 sanity |
| C003 / C004 / **C012** | ON 超长行：连续显示行 0/1/2/3 同 X 点击 C 递增；**C012 = scroll 10/20 行后点击 C>stride（≠0 不跳回逻辑行首）** |
| C005~C010 | 方向键↓/Home/End（ON 模式 Home/End 在当前显示行）/PgDn↔PgUp 往返对称/Goto 行/字节 |
| W001~W006 | scrollBar 语义 / sb.setValue 不回弹 / 行号栏只画一次行号 / 全屏扫描覆盖 union=[0,vs) / 行高一致 / 水平滚动条 disabled |
| **W007（⭐核心）** | scroll 20 display lines 后点击 → caretRect 真画在当前 viewport 显示行 Y 范围内，不跳回画面外 |
| **W008（⭐核心）** | **单逻辑行 fixture (lineCount==1)** → wheel N 格后 **lineAtTop() 恒 == 0** + topByte ≈ N×stride；禁止跳到"不存在的物理第二行" |
| W009 | wheel 前进/后退对称 |
| C013 | wheel 前进 1 格后 topByte ≈ +3×stride（±0.3 stride） |

📂 **目标 smoke 文件**：`smoke_word_wrap_scroll.cpp`  
📊 **P0：约 20 条** · 复杂度 **XL**（VirtualFileView 的坐标转换 + LineIndex 的 wrap displayLine 映射要全部重审，必须每条先拿 FAIL 数值证据再修）

---

### 组 5 · 选区 + 拖选组
**目标**：组 4 修好 displayLine 映射后，拖选的坐标变换也会随之好转；拖选 8 条单独一组因为 drag 事件模拟有独立 helper（mousePress→mouseMove→mouseRelease），不和 click/cursor 混淆。

| TC ID | 关键断言 |
|---|---|
| S001-A/B | Ctrl+A 全选双路径；selStart=0 selEnd=vs selSize=vs |
| S002 | OFF 模式 dragSelect(800,1600) bytes===readByteRange(800,1600) |
| S003 | ON 模式跨显示行拖选（同 1 逻辑行）→ 全 'A' 完整 |
| S004 | ON 跨逻辑行拖选 → 序列 "A"+"\n"+"B" 精确 |
| S005 | Shift+Right ASCII×10 + CJK×5 → selSize=10+15 |
| S006~S007 | 双击选词 / 三击选整行 |
| S008 | 口径一致性：readSelectionBytes 永远与 readByteRange(selStart,selEnd) 相同（"替换旁插"第二根源） |
| **S009** | **scroll 10 显示行后拖选 10 行 → selSize≈10×stride；selStart≠0（不偷偷跳回第一行）** |
| **S010** | wheel×4 后拖选 3 行 → selStart∈[T-50, T+500] |

📂 **目标 smoke 文件**：`smoke_selection_drag.cpp`  
📊 **P0：10 条** · 复杂度 **L**（强依赖于组 4 的 wrap 坐标修复作为前提）

---

### 组 6 · 16 进制编辑组
**目标**：独立视图模式（ViewMode::Hex ↔ Text），不依赖自动换行。可以与组 3/4 并行做。

| TC ID | 要点 |
|---|---|
| VW05-A/B / VW06 | 模式切换快捷键/菜单；L1 拒绝 |
| Hex01 | Hex dump 与真实字节逐字节一致（= 最基础，显示层 sanity） |
| Hex02 / Hex03 | Hex 区/ASCII 区点击光标字节级定位 |
| **Hex04（⭐核心）** | 键入 "42"/"43" → 真正改字节 0x41→0x42→0x43；切 Text 看到；undo 还原 |
| Hex05~Hex06 | 增删/可选能力；Hex 搜索字节序列 "43 43 43" → 400/2000/3682 |
| Hex07 | 全选口径一致性 |
| Hex08 | 保存 round-trip（10 处编辑 → 保存/关/重开 → 磁盘全一致） |
| Hex09 | Hex↔Text 切换无副作用 |
| Hex10 | NUL 0x00 正常显示+编辑（二进制底线） |

📂 **目标 smoke 文件**：`smoke_hex_edit.cpp`  
📊 **P0：13 条** · 复杂度 **M**（独立视图模式，代码在 VirtualFileView 的 Hex 分支，风险相对可控）

---

### 组 7 · 显示视图一致性组（UI 标志位但不改动字节流 + 状态栏）
**目标**：菜单勾选/状态栏文字/深色模式/缩放/边缘标尺等视觉层一致性；统一在同一个 smoke 文件里 assert UI 状态和字节流不变（saveNeeded=false）。

| TC 范围 | 涵盖项 |
|---|---|
| VW01/VW02-A×2 | 深色模式 / 自动换行开关 + 水平滚动条语义 |
| VW03/VW04 | 长行标尺竖线 + 列数 6 档（如果 VirtualFileView 尚未实现则至少要求不崩+saveNeeded=false，P1） |
| VW07-A/B/C | 缩放 +/-/0 三档（MainWindow zoom 目前只调 Scintilla editor()，证据⑥，先 FAIL 捕获再修 EditorTab zoom 分发） |
| I001~I005 | 状态栏行/列/字节位置/星号/L2 标识/编码名 |
| SY01-SY10（P2） | 语法切换 9 项 + save：不崩 + 字节流 0 改动 + saveNeeded 正确 |

📂 **目标 smoke 文件**：`smoke_view_status_consistency.cpp`  
📊 **P0：约 7+5=12 条** · 复杂度 **S~M**（缩放和标尺 L2 要补分发，其余简单）

---

### 组 8 · 多标签 / 拖放 / 最近文件 / 保存 round-trip 闭环组
**目标**：外围用户体验；F007/E012/SR07 核心闭环要和上面对 Edit/Replace 组同时通过。

| TC 范围 | 涵盖项 |
|---|---|
| F001-A/B ~ F008 | 新建/打开/保存/另存为/只读保存失败/退出 prompt/外部修改 reload/save round-trip |
| H001~H008 | 拖放打开/多标签状态独立（L2 的 wordWrap/saveNeeded/viewMode 不串）/标签关闭/最近打开/恢复设置/帮助/标题栏 |
| H007 | 3 个 tab（1×L2 + 2×L1）切换不崩 + 编码菜单同步 |
| F007 / Hex08 同类闭环 | 编辑→save→close→reopen→fullDiff 全闭环（F007=Text；Hex08=Hex） |
| D006 | 书签 L2 禁用或至少友好提示 + 不崩（P0 底线） |
| BM01~BM04（P0 底线） | L2 下 4 条书签动作：最低 = 不崩 + saveNeeded=false + statusBarContains("书签"/"暂不支持") |

📂 **目标 smoke 文件**：`smoke_multi_tab_save_roundtrip.cpp` + `smoke_bookmarks_minimum.cpp`（单独）  
📊 **P0：约 12+4=16 条** · 复杂度 **M**（保存 round-trip 底层 diag 全绿，GUI 层主要打通路径 A）

---

### 分组总览 · 推荐攻坚顺序 1→8

| 顺序 | 组名 | P0 条数 | 复杂度 | 关键收益 |
|---|---|---|---|---|
| **1** | 🔧 小修复组 | 7 | **S** | **立即拿 7 条 PASS**（代码全已修，等 smoke 实锤）：替换快捷键、L2 禁用菜单、生命周期完整性 |
| **2** | 🔍 搜索替换组 | 10 | **M** | 用户最高频主链路；框架已有直接扩充；修 SR05（patch 查找）和 SR07（undo group） |
| **3** | ✏️ 基础编辑组 | 18 | **L** | 编辑器核心功能；E004 双层面（字节流+显示层）实锤 Enter 换行 |
| **4** | ➿ 自动换行滚动定位组 | 20 | **XL** | **最大用户痛点**（W008/W007 两条 P0 是你原话的"跳第二行/无光标"） |
| **5** | 🖱️ 选区拖选组 | 10 | **L** | 依赖组 4 的 wrap 坐标；S009/S010 与 W008 强相关 |
| **6** | 🔢 16 进制编辑组 | 13 | **M** | 可与 4/5 并行；独立 Hex 视图模式；Hex04=真正改字节 |
| **7** | 🎨 显示视图一致性组 | 12 | **S~M** | UI 标志位；缩放分发+标尺+状态栏 |
| **8** | 📂 多标签/保存闭环组 | 16 | **M** | 外围体验；BM 最低底线不崩 |

> 👆 **16.1 单组内部执行顺序铁律（§4 质量方法论）**：  
> ① 进入该组 → ② 在对应 smoke_*.cpp 里**写最小可复现的失败 smoke（FAIL 版）** → ③ 跑拿到具体数值证据（比如"wheel 1 格之后 topByte == startOfLine(2)，期望≈3×stride"）→ ④ 依据数值证据定位根因 → ⑤ 修代码 → ⑥ 再跑同一 smoke 从 FAIL 变 PASS。  
> **禁止**：在 smoke 尚未捕获 FAIL 之前就盲改代码。  
> **禁止**：在组 N 的 smoke 还没全绿之前，就跳到组 N+1 改代码（防止 regress 前面刚修好的）。
