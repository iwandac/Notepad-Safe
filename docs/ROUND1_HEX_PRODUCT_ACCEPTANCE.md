# 第 1 轮：Hex 产品完整性设计与验收契约

状态：完成；1A、1B、1C、1D 自动门与最终工作树视觉门均已接受
适用版本：成熟度路线图 v2.0
发布含义：本轮完成只关闭 Hex 相关 P0/P1；不代表产品可发布

## 1. 用户结果

非技术用户不需要理解“B1/B2/B3”或窗口化 Provider。产品必须让用户只看到一种可预测的 Hex 体验：

- 鼠标或键盘选择的始终是源文件中的完整字节；滚动、换窗和改变宽度不会悄悄改变范围。
- 可编辑文件的修改能保存、能撤销、能在异常退出后恢复；失败时原文件与未保存修改都不会被静默丢弃。
- 只读大文件仍可选择、复制、查找和跳转；任何会写入的入口都明确禁用。
- 查找结果回到准确字节。Hex 模式不假装支持文本正则替换，也不显示“看似可点、实际无效”的命令。
- 在 100%/125%/200% DPI、浅色/深色主题和键盘操作下，地址、Hex、ASCII、状态与焦点仍清楚可见。

## 2. 不可破坏的边界

1. 源字节是唯一权威；Hex/ASCII 都只是投影。
2. B2/B3 永久只读，不为“体验一致”复制整个大文件或建立全文索引。
3. 所有范围使用 `[byteStart, byteEnd)` 与 64 位 offset；不得从渲染字符位置反推文件身份。
4. Save、恢复和搜索命中必须绑定打开时的文件身份与 revision；过期结果不得应用。
5. 自动验收使用生成样本或仓库保留样本；不得复制、改写或删除用户的大文件。
6. 本轮不引入 AI、不执行脚本、不改变安全文档的明文边界。

## 3. 开发切片

### 1A：选择、滚动与窗口边界

完成 `HX-N04/N05`、`HX-S01/S02/S03` 的剩余缺口：

- B1 在 Hex 与 ASCII 两表面从 viewport 上下缘继续拖动时自动滚动；正向、反向都能跨活动窗口。
- B2/B3 真实鼠标拖选跨窗口，Hex 与 ASCII 镜像同一源范围；释放鼠标后范围稳定。
- resize/reflow、Hex↔ASCII 聚焦和跨窗口重投影后，anchor、caret 与 payload range 不变。
- 地址、唯一全局滚动条、状态栏 offset/selection length 与源范围一致。

通过条件：测试必须真实发送鼠标事件，至少覆盖窗口边界前后各一个字节、正反向和两表面；不能只调用 `setSelection()`。

### 1B：查找、命令与只读入口

完成 `HX-F01/F02`、`HX-Q01`、`HX-A01`：

- Find Next/Previous 在 BOF/EOF、回绕开关、B1/B2/B3 中选择准确源字节。
- Hex bytes 查找与文本/正则语义明确分离；不支持的选项在进入 Hex 模式时禁用并说明原因。
- Replace、Replace All、Cut、Paste、Delete、拖放输入在 B2/B3 的菜单、工具栏、快捷键、右键入口全部 fail-closed；Copy/Find/Go to 保持可用。
- 搜索 Dock 双击命中即使跨当前投影窗口，也只激活命中创建时的文件身份和 byte range；过期命中给出可理解提示。

通过条件：每个可见命令同时检查 enabled/checked、真实触发结果和源文件 hash；只测 QAction 状态不算完成。

### 1C：保存冲突、失败与崩溃恢复

完成 `HX-P03`、`HX-R01`：

- 外部修改、源文件消失、只读、候选写失败和提交失败都不静默覆盖；磁盘上至少保留一份可验证的旧或新完整文件，编辑器保持 dirty 修改。
- nibble、ASCII、Insert、Delete、Cut、Paste、选区替换分别产生可恢复的加密 patch journal。
- 正式进程到达已确认恢复点后由外部 harness 强制终止；重启后逐字节验证内容、dirty、offset、selection 与 OVR/INS。
- 恢复失败可重试或放弃；不得产生明文正文、明文 patch 或未登记临时副本。

通过条件：同进程销毁 `EditorTab` 只能作为快速测试，不能替代至少一条正式 EXE 强杀旅程。每个失败用例记录原文件 hash、journal 是否加密、恢复后 hash 与用户可见诊断码。

### 1D：DPI、主题、右键与辅助功能

完成 `HX-L01/L02`、`HX-A01` 的产品证据：

- 100%/125%/200% DPI，900/1400/1800 px，浅色/深色，0/1 Dock 的最小正交矩阵。
- 地址、Hex、ASCII 到第 1000 行仍对齐；无裁 nibble、重叠、不可达 EOF 或多余滚动条。
- 两编辑表面、滚动条、OVR/INS、状态字段与关键菜单有稳定 accessibleName/description；键盘焦点可见，Tab 顺序可预测。
- 右键菜单只显示当前表面真实可执行的动作；只读原因采用用户语言，不暴露内部 Provider 名称。
- 超过剪贴板预算的 Copy/Paste 原子拒绝并说明建议操作，不截断、不部分应用。

通过条件：自动几何/辅助属性断言 + 人工视觉矩阵共同通过；截图必须绑定 commit、DPI、主题、窗口大小和测试 case ID。

## 4. 第 1 轮发布判定

只有以下条件同时满足，路线图第 1 轮才可标记完成：

1. `HEX_EDITOR_CONTRACT_MATRIX.md` 的“当前明确未收敛项”全部关闭或被明确移到用户裁决门，不能用“局部证据”代替。
2. 新增用例进入 CTest/GUI 注册表，单用例有超时和稳定 case ID。
3. KDBX=ON 与 KDBX=OFF 的相关 Release 矩阵通过；架构边界与供应链门通过。
4. B2/B3 样本运行后 hash 不变，测试目录没有大文件副本和明文恢复残留。
5. 人工视觉证据经独立复核，且第 0 轮门禁能验证其 commit 与 artifact hash。
6. 根代理逐项复核实现 diff、自动证据与可见界面；任何一项不满足即打回原开发切片。

## 5. 结果记录模板

每个切片完成时在本文件追加：

- 设计版本与 case ID
- 实现文件与行为变化
- 自动测试命令、通过数和构建配置
- 人工视觉环境与 artifact hash
- 源文件/样本前后 hash
- 已知限制与是否阻断下一切片
- 根代理判定：接受 / 打回，以及理由

## 6. 执行记录

### 1A：B1 选择、边缘滚动与活动窗口（已接受，2026-08-29）

- 旧实现红灯：真实 Hex 鼠标拖到 viewport 下缘后，活动 256 KiB 投影窗口没有前进，选择被困在旧窗口；不是 `setSelection()` 伪测。
- 实现：`HexMouseSelectionController` 实例拥有 timer、gesture generation、anchor/caret、表面、方向和 source-authority；`EditorTab` 只保留聚焦边界。Hex/ASCII 投影仅镜像全局 source range 的可见交集。
- 返工：先修正 stale release 回调覆盖下一手势、BOF 边缘误触发、拖出后回入释放的旧 caret；随后因静态 helper 只是搬行数被根代理打回，改为真正状态所有者；最后补齐跨窗 Copy 与方向键 authority 交接。
- 根代理独立 Release GUI：保存重开、键盘导航、byte selection、新真实鼠标契约、INS 发现性共 5/5 通过。
- 自动契约覆盖 B1 的 Hex 正向、ASCII 反向、上下缘、跨活动窗口、回入释放、resize/focus、Copy、Shift+方向、普通方向键、状态、源字节和磁盘 SHA-256；`dirty=false`。
- 当时限制：B2/B3 真实鼠标跨窗口专项尚未执行；该缺口已由下一子切片关闭。B1 视觉截图仍由 1D 单独验收。

### 1A-B2/B3：窗口化只读真实鼠标（已接受，2026-08-29）

- 可信旧实现红灯：真实 Hex 鼠标拖到 viewport 下缘后，B2 在 19.294 秒、B3 在 43.727 秒均因 `windowStart` 没有前进而失败；旧 provider 只有键盘能进入 source-authoritative 跨窗路径，鼠标没有 gesture owner、timer 或 reload 路径。
- 实现新增 `WindowedHexMouseSelectionController`，独立持有一手势的 timer、anchor/caret、方向和 stale-release generation；`WindowedReadOnlyProvider` 继续持有 256 KiB 活动投影、源字节与 `ReadOnlySelectionModel` 权威。provider 收敛为 1925 行，controller 214 行，未抬架构预算。
- B2 40 MiB：logical/allocated 为 `41,943,040 / 1,048,576` bytes，SHA-256 `71a096d9825fd39d241b6efbf9ab3f4a5932773348566885834ad6ff51c8af34`；正向 Hex `[31631224,31649044)`、反向 ASCII `[31548612,31560988)`，两向各实际换窗一次。
- B3 4 GiB+64 MiB sparse：logical/allocated 为 `4,362,076,160 / 1,048,576` bytes，SHA-256 `e9f418ac4d6d2d56468fc4f214aa826ea2e2909f2cf18a7f4b39397441a2dbed`；正向 Hex `[3271731052,3271749268)`、反向 ASCII `[3271648552,3271661208)`，两向各实际换窗一次。
- 两条旅程同时验证另一 pane 镜像、方向、全局滚动、状态栏、释放后 resize/focus、完整 raw-byte Copy，以及 Cut/Paste/Delete/Backspace、Replace/Replace All、右键菜单和 drag/drop 全部只读；动作后完整文件 hash 不变。
- 子代相邻 5/5 与架构门（72 个产品源码条目）通过；根代理独立 Release B2 2.13 秒、B3 52.28 秒，2/2 通过，独立架构门通过。
- 证据边界：B3 是稀疏文件，只证明 64 位 offset、跨 256 KiB 投影选择、bounded resident projection 与只读交互；不证明密集 4 GiB I/O、磁盘占用或完整扫描性能。1A 至此关闭。

### 1B：B2 只读命令与 Hex 搜索（已接受，2026-08-29）

- 旧实现红灯：真实 B2 Hex 文档中顶层 `search.replace` 仍 enabled；根因是命令层把所有搜索命令只按文档/搜索能力放行。
- 实现：Replace 同时要求 `Text` 内容与 `Edit/canModify`；Find 保持可用，对话框的 Hex Replace/Replace All 继续 fail-closed。
- 根代理独立 Release GUI：新 B2 旅程与相邻 `tc_fr_011/012/019/022` 共 5/5 通过。
- 新旅程覆盖 QAction、真实 trigger、Ctrl+X/V/Delete、右键、拖放、Copy、Hex bytes 选项、Next/Previous、BOF/EOF、wrap、Find All，以及切到另一 B1 标签后双击冻结的 B2 Dock 结果并按 documentId/revision/byte range 返回原文档。
- 保留 `smalltestfile` 测试后 SHA-256 为 `829497727AD332841FEAA25850C0F534375D0C684C4587C7B94F218288A98F01`，与测试前一致。
- 限制：仓库没有受控 B3 `bigtestfile`；没有合成、复制或修改 >2 GiB 文件。因此 B2 已关闭，B3 实物证据仍阻断第 1 轮完成。

### 1C-1：B1 编辑原语恢复与外部冲突（已接受，2026-08-29）

- 可信旧实现红灯：INS 模式的 Insert 与选区替换恢复后错误回到 OVR；根因是 `SessionTab/session.json` 没有保存 Hex INS 状态。Cut 曾出现夹具红灯，确认是聚焦 ASCII 前未建立真实 ASCII 选区，已更正且不计产品缺陷。
- 实现：增加默认 false 的 `hexInsertMode`，只贯通 session capture、JSON、load 与 `DocumentSessionView` restore；不改变 patch journal 格式。
- 新恢复矩阵覆盖 nibble、ASCII、Insert、Delete、Cut、Paste、选区替换，组合 Hex/ASCII、OVR/INS、无/正向/反向选区；每条逐字节验证恢复、dirty、offset/selection、模式、源 hash、`BinaryPatchJournal` 和密文不含 journal magic/长 payload。
- 新 HX-P03 旅程证明二进制外部修改后的 Save 显示冲突；取消后外部磁盘版本、local dirty 字节和既有加密恢复点同时保留，不在变化后的源上静默应用 journal。
- 根代理独立 Release：KDBX=ON `session + 8 新 GUI + 2 相邻` 为 11/11；KDBX=OFF `session + 8 新 GUI` 为 9/9；架构门 68 个产品源码条目通过。
- 限制：仍是同进程窗口销毁；正式 EXE 外部强杀、候选写/提交失败注入和 stale journal 的重试/放弃 UI 尚未完成，1C 不能整体关闭。

### 1C-2：正式进程强杀恢复（已接受，2026-08-29）

- 新 `e2e_hex_process_recovery` 不链接产品 runtime、没有测试后门；它用 Win32 可见输入驱动正式 `notepad-safe.exe`，等待 `session.json` 与加密 `.ndraft` 原子发布，再强杀精确 PID 并启动同一个 EXE。
- 可信红灯先后揭示两类问题：harness 未按 Windows 扩展键发送 Home；产品的 Ctrl+Home/End 又只在无选区时接管，导致旧 source-authoritative 选区继续替换字节。harness 修正键标志，产品移除 selection 门并在跳转前 relinquish authority；新增 GUI case 在旧实现 0.66 秒失败、修复后通过。
- 子代正式 E2E 连续两次通过（44.12 秒、43.81 秒）；直跑强杀 PID `3536`，源 SHA-256 为 `af3d1a15e30819857dc2e7304760b07cc3d9c6a698ace6a45224f29a4939df4a`。
- 根代理独立正式 E2E 43.95 秒通过：强杀 PID `29840`；重启后逐字节内容、`[0,4)` 选区、INS 状态正确，源 SHA-256 不变，恢复树不含 binary canary 或明文 journal magic。
- 根代理独立 `session + gui_smoke_hex_recovery` 为 10/10，通过新 Ctrl+Home/End、七个编辑原语、外部冲突；当前架构运行契约在沙箱外通过，源码边界为 MainWindow 5190/616/112、EditorTab 7089/860/176、71 个产品源码条目。
- 限制：只覆盖 B1；stale journal 的重试/放弃 UI 尚未完成，因此 1C 仍不能整体关闭。

### 1C-3：原子保存候选创建/提交失败（已接受，2026-08-29）

- 两条旅程均由真实 Windows 文件系统语义触发，没有生产测试后门：目录 DACL 只拒绝同目录候选创建；不允许 delete sharing 的源文件句柄允许候选完整写入、但阻止原子替换。
- 旧实现红灯揭示 P0：`QSaveFile::commit()` 失败前为释放 Windows mmap 而关闭 B1 backing，同时清空 `PatchList`；界面仍显示 dirty，却已经回到旧源字节。修复后失败提交重新映射旧源并保留 patch、caret 与 selection；成功提交仍按新源重置 patch，未启用 direct-write fallback。
- 用户可见诊断分为 `[plain.save.candidate.open]`、`[plain.save.candidate.write]` 与 `[plain.save.commit]`，说明原文件未更改，不再把候选创建、写入和替换混成一个错误。
- 根代理独立 Release 复跑：KDBX=ON 2/2、KDBX=OFF 2/2。两条路径均逐字节验证界面编辑、dirty、选区与重启恢复；源 SHA-256 分别为 `63b855fdd5a4db1eb8c775818dcc97a1dab45fa30a285adabcecb98efdf234aa`、`a0917a5404ed15606e90e28348070e2a0fe1ceea873053e339e5faf8dc263498`，故障前后相同；journal 为加密 `.ndraft` 且不含 canary，同目录无候选残留。
- 限制：尚未覆盖真实磁盘耗尽造成的候选部分写、secure memory-backed binary 的 commit 失败，以及 stale journal 的用户处理。本切片关闭候选创建/commit 两个 P0，不据此关闭整个 1C。

### 1C-4：过期/损坏恢复稿的重试与明确放弃（已接受，2026-08-29）

- 旧链路的可信静态证据：恢复失败会移除失败标签；队列结束会无条件删除 `session.json`；损坏 `.ndraft` 还可能落入 legacy plaintext 路径。由于本机 Ninja 锁故障，未取得旧提交的动态红灯，不把静态证据表述成已执行旧版测试。
- 实现：`WorkspaceRecoveryCoordinator` 逐标签恢复并保留失败状态；`RecoveryIssuePage` 以用户语言显示稳定诊断码，提供真实 Retry 与两步 Discard。问题页自身实现 `ISessionDocumentPort`，因此失败记录和加密 journal 能继续进入会话真值；单标签关闭不能隐式丢弃，应用退出可在保存问题记录后完成。
- Retry 每次重新调用正式 `SessionManager`：源身份仍变化时保持问题页、源 hash、journal hash 与 session；源字节、文件身份和原修改时间恢复后，成功替换为真实 `EditorTab`，逐字节恢复 dirty 内容、`[64,68)` 选择和模式。
- Discard 只接受受管恢复目录中的路径，首次点击只展开不可逆说明，第二次确认才删除该 `.ndraft`；删除失败会保留问题页。确定性损坏样本保留合法加密头但截断终止记录，稳定得到 `[recovery.draft.invalid]`，不再回退到明文打开或宽泛诊断。
- 根代理在首次真实编译中打回并修复两项问题：非 QObject 协调器误解析 WinSock `connect`，以及测试调用不存在的选择 API；运行时又打回过宽的 `[recovery.restore.failed]`，收敛为 source changed / unlock / invalid 三类稳定代码。测试夹具同时修正为恢复完整 `DiskFileVersion`，没有削弱产品的保守文件身份判定。
- 独立 NMake Release 验收：KDBX=ON 新问题页 2/2、KDBX=OFF 2/2；既有 `session` 在 ON/OFF 各 1/1；`architecture_contracts + architecture_boundary` 在 ON/OFF 各 2/2。静态架构边界为 MainWindow 5035 行/600 branches/112 methods、EditorTab 7104 行/861 branches/176 methods、74 个唯一产品源码条目；`git diff --check` 通过。
- 证据边界：未覆盖真实磁盘耗尽、受管恢复稿删除权限失败的动态 GUI，以及 Secure Document 解锁失败页；这些属于第 3 轮故障矩阵。本切片满足 Hex `HX-R01` 的 stale/corrupt Retry/Discard 合同，1C 至此关闭。

### 1D：DPI、主题、右键、剪贴板与辅助功能（已接受，2026-08-29）

- 旧实现红灯：Hex 主表面缺 accessible name；ASCII 右键绕过产品菜单；64 MiB+1 raw-byte 粘贴真实修改字节；ASCII 固定浅色，在暗色主题下错误。
- 实现统一 64 MiB 剪贴板预算并在 mutation 前原子拒绝，补齐 Hex/ASCII/全局滚动/INS-OVR 可访问名称和描述、Tab/Backtab 顺序、palette-aware ASCII 主题以及 ASCII 表面对产品右键命令的归属。
- 首版返工两次：CMake 曾把 125%/200% 错算为 12.5/20.0；又曾把实现内联进公共 `EditorTab.h` 以绕过 cpp 预算。最终显式使用 1.0/1.25/2.0，公共头只留声明，clipboard、focus/A11y 与 UI helper 分别进入职责明确的 `.cpp`。
- 右键验收从真实菜单 trigger `edit.copy`、`edit.cut`、`edit.undo`，验证 raw-byte MIME、精确删除和完整 Undo；根代理独立复跑通过。
- KDBX=ON 新 1D 自动门 5/5；KDBX=OFF 曾出现一次 200% 进程无输出失败，单独复跑通过，随后五条 `repeat until-fail:3` 共 15/15。该瞬态尚不能归因，因此不表述为完全稳定。
- 根代理独立 ON 新 5 条 + 相邻键盘/选择/鼠标 3 条为 8/8；独立 OFF 首轮 4/5 后单例与连续三轮均通过。三个独立进程分别断言 actual DPR 为 1.0、1.25、2.0。
- 最终视觉矩阵使用当前产品的 1 MiB B1 fixture，覆盖 DPR 1.0/1.25/2.0 × 900/1400/1800 逻辑宽度 × Light/Dark × Dock 关闭/打开，共 36 张原图与 3 张联系表。捕获脚本在前后两次计算中得到同一工作树身份，36 个 case 的逻辑/物理尺寸、DPR、12-byte 选择、row 1000、主题、Dock 与 OVR/INS 均匹配。
- 根代理把联系表只用于导航，并逐张查看 36 张原图；未发现地址/Hex/ASCII 错位、选择不一致、关键状态不可见、主题残留、Dock 遮挡或 100%/125%/200% 下的裁切。人工审阅清单 manifest SHA-256 为 `41bf9fd053c50583c308a6b82647272089befe43c04fe8355e806e8e9db9c7cc`，39 个视觉 artifact 的排序哈希集合 SHA-256 为 `c240068322ecb6e4b1c76ac83494faca41fef61d5f2ff20de5d9eb85e033b3f0`。
- 为使“本轮已完成”的文档状态也进入身份绑定，审批前会在 `build-round1-hex-visual/hex-visual-evidence-final-02` 对最终工作树重新捕获；审批产物保留在忽略的构建证据目录，不把机器生成截图提交进源码。
- 证据边界：视觉矩阵是 B1，不证明 B3 密集文件性能；B3 的 64 位 offset、bounded resident projection 与只读鼠标旅程由 1A-B2/B3 的稀疏 4 GiB+64 MiB 证据承担。第 1 轮关闭不等于产品发布门通过。
