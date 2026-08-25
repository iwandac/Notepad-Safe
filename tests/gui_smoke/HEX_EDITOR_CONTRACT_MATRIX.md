# Hex 编辑器专项契约矩阵

本文件定义 Hex 模块的有限测试边界。执行器根据维度自动生成 pairwise、三元或穷举案例；
它不是用户旅程。每个失败必须输出稳定 case ID，并同时检查 byte model、Hex/ASCII 投影、
caret/selection、状态栏、dirty/Undo 和磁盘结果中适用的部分。

## 1. 状态维度

| 维度 | 值 |
|---|---|
| Provider | B1 editable、B2 read-only、B3 read-only、Secure Binary memory-backed |
| 大小 | 0/1/15/16/17 B、row−1/row/row+1、window−1/window/window+1、32 MiB±1、2 GiB±1 |
| 表面 | Hex nibble、ASCII、地址 gutter、全局滚动条、状态栏、搜索 Dock |
| 模式 | OVR、INS、只读 |
| 位置 | BOF、byte high/low nibble、行尾、行首、窗口边界、EOF |
| 选择 | none、单字节、正向/反向、多行、跨窗口、全选 |
| 布局 | 900/1400/1800px、resize 前后、100/125/200% DPI、Light/Dark、0/1 Dock |
| 生命周期 | clean、dirty、saved、external-change、recovered、locked、error |

## 2. 操作原语与判定

| ID | 操作原语 | 自动展开 | 必须证明 |
|---|---|---|---|
| HX-O01 | 统一打开/路由 | 扩展名伪装、NUL/控制字节、Text/Binary/Secure、B1–B3 | 内容身份决定 Text/Hex；无错误 readAll 或错误 Provider |
| HX-O02 | Text↔Hex 呈现 | clean/dirty、位置/选择、重开/会话恢复 | payload 不变；菜单/状态/位置一致；转换不保存 |
| HX-L01 | 三列布局 | Provider×宽度×DPI×主题、长距离行高累计 | 地址/Hex/ASCII 使用同一实际像素行高，首行至第 1000 行水平基线不漂移；利用宽度且无大片空白、裁 nibble、重叠 |
| HX-L02 | 自适应 bytes/row | 15/16/17B、resize 往返、窄/宽 | 只按完整字节和 4B 粒度重排；数据、caret、selection 不变 |
| HX-N01 | 鼠标定位 | high/low、空格、ASCII、地址、各边界 | caret 吸附有效 byte/nibble；两表面映射同一 global offset |
| HX-N02 | 左右键 | high↔low、byte±1、BOF/EOF、OVR/INS | 永不停在分隔空格；状态和 ASCII caret 同步 |
| HX-N03 | 上下/Home/End | 行首尾、短末行、保持 nibble | global byte 正确钳制；不落入不存在字节 |
| HX-N04 | Page/滚轮 | Hex/ASCII 表面、angle/pixel、窗口边界 | 单一全局滚动；地址、thumb、两投影同步；无 gap/重复/回跳 |
| HX-N05 | 滚动条拖动 | 0/25/50/75/100%、B1–B3、resize/wrap 不适用 | 可达物理 EOF；首行地址 = global row；无第二滚动条 |
| HX-S01 | Hex 选择 | high/low 起点、正反向、跨行/窗口、Shift+导航 | 选择按完整 source byte 定义；ASCII 镜像相同 half-open range |
| HX-S02 | ASCII 选择 | 正反向、跨行/窗口、末字节 | Hex 镜像精确；方向不改变 payload range |
| HX-S03 | Select All/清除 | Hex/ASCII、空/单/多窗口 | 全文件或产品明确的有界范围；状态栏 64 位长度准确 |
| HX-E01 | Hex nibble 输入 | 0–9/A–F/a–f、high/low、OVR/INS、BOF/middle/EOF、完整 byte selection | OVR 长度不变；INS 仅在 high nibble 新增 X0、low nibble 完成同一字节；选区先整体替换为 X0；两 nibble 是一个 byte-entry Undo 单元；caret 跳转正确 |
| HX-E02 | 非法 Hex 输入 | G/空格/换行/Unicode、组合键、IME preedit/commit | payload 不变；候选整段原子拒绝；预编辑不进入固定网格；导航/快捷键不被误拦截 |
| HX-E03 | ASCII 输入 | 0x20–0x7E、0x00/0xFF via paste、Unicode、OVR/INS | Latin-1 规则明确；Hex 即时同步；非法输入无修改 |
| HX-E04 | Insert 模式切换 | Insert 键、编辑菜单、状态栏按钮、两表面、切标签、重开、只读 | 默认 OVR；单文档共享 OVR/INS；不泄漏；持续可见、可点击且 QAction checked/enabled 与实际能力一致 |
| HX-D01 | Delete/Backspace | 两表面×模式×选择方向×BOF/middle/EOF | 删除准确；BOF Backspace/EOF Delete no-op；不被字符校验吞掉 |
| HX-C01 | Copy | 两表面×选择/none×跨行、可打印/控制/NUL bytes | Hex 输出规范 bytes；ASCII 可打印内容输出文本；不可打印内容同时携带私有 raw-byte MIME 与可见 Hex 文本，不能被剪贴板 NUL 截断；payload 不变 |
| HX-C02 | Cut | 两表面×选择方向×边界×只读 | 剪贴板与删除范围一致；无选择 no-op；只读禁用 |
| HX-C03 | Paste | 两表面×OVR/INS×选择×合法/非法/空/大输入 | 替换/插入语义正确；错误原子无修改；长度限制生效 |
| HX-U01 | Undo/Redo | nibble、ASCII、delete、cut、paste、跨表面序列 | 每个用户操作一个合理历史单元；字节/投影/caret/dirty 恢复 |
| HX-F01 | Find Next/Prev | bytes、BOF/middle/EOF、wrap on/off、B1–B3 | 选择真实字节；方向/边界一致；64 位 offset |
| HX-F02 | Find All/Dock | 0/1/多/同一行/窗口边界、current/all-open | 高亮精确；增量/取消；双击激活正确表面和 byte range |
| HX-P01 | clean Save | B1、9MiB、只读 | 严格 no-op；identity/mtime/hash 不变；无固定备份 |
| HX-P02 | dirty Save/Save As | insert/delete/replace、0/16/window 边界 | 原子保存；重开逐字节一致；backing handoff 正确 |
| HX-P03 | 外部冲突/失败 | changed/missing/read-only/write/commit 失败 | 不静默覆盖；原文件和 dirty 保留；错误可见 |
| HX-R01 | 崩溃恢复 | 各编辑原语、跨窗口、位置/模式 | 加密 patch journal；恢复字节、dirty、offset；无明文临时文件 |
| HX-A01 | QAction/状态栏 | B1/B2/B3、Hex/ASCII focus、OVR/INS、selection | enabled/checked 与实际可执行能力相同；offset/size/tier 不重叠 |
| HX-Q01 | 只读 Provider | B2/B3 的所有编辑/剪贴/替换入口 | 浏览/搜索可用；任何写操作禁用且 payload 不变 |

### 2.1 细粒度动作清单（执行时不得只测代表项）

| 族 | 必须逐项进入生成器/专项用例的动作 |
|---|---|
| 指针 | Hex high/low/分隔空格单击；ASCII 可打印/`.` 单击；地址区单击；双击；Shift+单击；正反向拖选；跨行拖选；拖到 viewport 上下缘自动滚动；选区内右键；空白尾部单击 |
| 水平键盘 | Left/Right；Shift+Left/Right；Ctrl+Left/Right 产品语义；BOF/EOF；从 high/low/separator 起步；有选区时折叠方向 |
| 垂直键盘 | Up/Down；Shift+Up/Down；Home/End；Ctrl+Home/End；PageUp/PageDown；Ctrl/Shift 组合；完整行/短尾行/窗口首尾 |
| 选择 | 单 byte；多 byte；正/反向；跨行；跨窗口；Ctrl+A；Esc/单击清除；切换 Hex↔ASCII 后保持；resize/reflow 后保持；状态栏长度 |
| Hex 输入 | 每个 0–9/A–F/a–f；high→low→next；OVR/INS；选区替换；行尾/窗口尾/EOF；连续输入；非法 G、空格、Tab、Enter、Unicode、IME |
| ASCII 输入 | 可打印 Latin-1 边界 0x20/0x7E/0xA0/0xFF；控制字符拒绝；OVR/INS；选区替换；EOF；连续输入；IME/Unicode 原子拒绝 |
| 删除 | Delete/Backspace；无选区/正向/反向；BOF/middle/EOF；两表面；OVR/INS；连续删除；Undo/Redo |
| 剪贴板 | Ctrl+C/X/V 与菜单/工具栏/右键；Hex/ASCII 焦点；空/单/多/跨行选区；空/合法/带分隔符/奇数 nibble/非法/Unicode/超限 payload；只读 |
| 历史 | 单 nibble、整 byte、连续 byte、ASCII、Delete、Cut、Paste、选区替换、跨表面交替；Undo 到 clean；Redo；新编辑截断 Redo；保存点 dirty 状态 |
| 搜索 | Find Next/Prev/All；纯文本/正则开关；大小写/整词/回绕；0/1/N；重叠/同一行/跨窗口；当前/全部打开；取消/增量；Dock 双击 |
| 滚动布局 | wheel angle/pixel；Hex/ASCII 上滚轮；bar 箭头/page/拖柄 0–100%；resize/reflow；DPI/theme/Dock；唯一滚动条；EOF 可达；横向不截断 |
| 文件生命周期 | 新建/打开/另存为/转换/保存/外部冲突/失败/恢复/会话重开；空/边界大小；B1/B2/B3/加密内存源；hash/mtime/backing identity |
| 只读 | 每个写入键、Cut/Paste/Delete、菜单/工具栏/右键、Replace/Replace All、拖放输入均不修改；Copy/选择/搜索/跳转仍可用 |
| 状态与可访问性 | offset/selection/size/tier/OVR-INS；焦点环；Tab 顺序；菜单 enabled/checked；tooltip/accessibleName；窄窗文字不重叠 |

## 3. 自动化强度

- P0 数据修改：相关维度 pairwise 全覆盖；BOF/EOF、跨窗口和历史至少三元组合。
- 导航与布局：固定百分位 + 随机可重放 offset；每次比较地址、thumb、caret 和 byte model。
- 保存与恢复：操作前后计算长度和 hash；重开后逐字节或分块 hash 比较。
- B2/B3：使用保留的 `smalltestfile`/`bigtestfile`，不复制、不删除、不全量读入。
- 新增 Hex QAction、按键处理或可见控件时，必须加入本表；无契约 ID 的功能不得视为完成。

## 4. 当前自动证据

| 契约 | 用例 | 状态 |
|---|---|---|
| HX-D01/HX-U01 | `tc_hex_delete_backspace_pairwise_contract` | 9 个 pairwise 案例通过；已发现并修复 BOF Backspace 错删首字节 |
| HX-N01/N02/N03 | `tc_hex_keyboard_navigation_contract` | 通过；真实点击分隔符及 Left/Right/Up/Down/Home/End/Page/Ctrl+Home/End；已修复 separator caret、短尾行和垂直 source-byte 导航 |
| HX-S01/S02 | `tc_hex_byte_selection_contract` | 通过；正反向 Shift、跨行、Ctrl+Shift+End、真实鼠标拖选；已修复半 nibble/分隔符选区和拖选事件递归 |
| HX-E01/E04/HX-U01/P02/HX-A01 | `tc_hex_nibble_input_pairwise_contract` | 9 个 pairwise 案例通过；已修复 Undo/Redo 后 ASCII 残留和 OVR/INS 状态栏失配 |
| HX-E01/E04/HX-U01/P02/HX-A01 | `tc_hex_insert_mode_discoverable_and_eof_append` | 通过；真实点击常驻 OVR/INS、编辑菜单双向切换、Ctrl+End 到真实 EOF、`A` 中间态 `A0`、`B` 完成 `AB`、字节模型/投影/状态栏/保存重开逐字节一致；视觉证据 `build/gui-hex-insert-mode.png` |
| HX-E01/HX-S01/HX-U01/P02 | `tc_hex_selected_direct_typing_replaces_complete_range` | OVR/INS 均通过；正向多字节选区 `[2,6)` 直接输入 `AB` 后严格缩为单字节，修复 Scintilla 投影已改而 PatchList 仍保留旧长度及重叠 `AB A0` 的数据损坏路径 |
| HX-C01/C02/HX-U01 | `tc_hex_copy_cut_pairwise_contract` | 9 个 pairwise 案例通过 |
| HX-C03/HX-U01 | `tc_hex_paste_pairwise_contract` | 9 个 pairwise 案例通过；已修复 OVR 在 EOF/剩余空间不足时增长文件 |
| HX-E02/E03 | `tc_hex_invalid_direct_input_contract` | 8 个约束 pairwise 案例通过；控制字符/Unicode 原子拒绝，已修复 ASCII Enter 写入 0x0D |
| HX-E01/E02/E03/E04/P02 | `tc_hex_ime_commit_preserves_byte_grid_contract` | 通过；真实 `QInputMethodEvent` 覆盖 Hex/ASCII preedit 与 commit，混合非法候选整段拒绝；合法 `D8DB` 在 OVR/INS 中分别原子覆盖/插入，Text↔Hex 往返后输入法策略和落盘字节正确；视觉证据 `build/gui-hex-ime-safe.png` |
| HX-E03/E04/HX-U01 | `tc_hex_ascii_direct_input_pairwise_contract` | 9 个 pairwise 案例通过；已修复 OVR EOF 意外增长 |
| HX-C01/C02/C03/HX-A01 | `tc_hex_mainwindow_clipboard_actions_follow_focused_surface` | 通过；已修复主窗口 QAction 忽略 ASCII 焦点 |
| HX-E01/E03/E04/L02/S01/S02/P01/P02/U01 | `tc_hex_binary_open_edit_save_as_reopen_roundtrip` | 通过；统一 UTF-8 字符投影、双栏编辑、Undo/Redo、原始 byte 保存、Save As/backing handoff 与重开一致；投影 `�` 不进入普通文本重编码确认 |
| HX-N04/N05/L01/P01 | `tc_nine_mib_unknown_binary_unchanged_save_is_noop` | 部分覆盖 |
| HX-N05/HX-C01/HX-Q01 | `s3ProductionProviderDragMatchesRenderedHex`、`b2BinaryUsesStableSingleScrollFallback` | 保留的 4.9GB/228MB 样本通过；拖动地址一致、精确 byte Copy、写键/命令不改源、Ctrl+Home/End 到物理首尾 |
| HX-L01/L02/HX-N02/HX-S01/S02/S03/HX-C01/HX-Q01 | `windowedHexUsesUnifiedThreeColumnByteContract` | 通过；B2/B3 已改为与 B1 相同的“地址 margin + 纯 Hex + 独立 ASCII + 单一全局滚动条”契约，并证明双向选区、逐 nibble 导航、焦点敏感 Copy、Ctrl+A 全源边界、窄宽重排与源文件不变 |
| HX-N04/HX-N05 | `windowedHexBothPanesWheelInLockstep`、`tc_nine_mib_unknown_binary_unchanged_save_is_noop` | 通过；Hex/ASCII 两个表面的 angle/pixel wheel 均驱动同一全局位置，屏幕首行逐行一致；B1/B2/B3 跨 256KiB 投影边界即时换窗，无停顿后跳跃、回跳或两栏漂移 |
| HX-O02 | `windowedBinaryTextPresentationUsesByteOffsetGutterWithoutGlobalScans`、`tc_binary_text_presentation_survives_session_restore` | 通过；Binary 切到 Text 后 margin 改为全局十进制 byte offset，字符 pane 隐藏；B1 与 Windowed Provider 均不启动全文 LineIndex/字符统计，切回 Hex 恢复十六进制地址 |
| HX-L01/HX-S02/HX-C01/HX-O02/HX-F02 | `tinyBinaryHexRowsReverseSelectionRawSearchAndTextByteOffsets` | 通过；直接打开保留的 32 MiB 以下 `tinytestfile`，逐 block 核对 Hex/字符区固定行高；在字符表面真实反向拖选 source byte range，复制保留 raw bytes 并自动进入 Hex bytes 查找；Find Next 精确回到源范围；切换 Text 显示十进制 byte-offset gutter 且不产生全文扫描 |
| HX-E03/HX-E04/HX-U01/HX-O02/HX-N05 | `tinyBinaryAsciiEditingTextScrollAndWrapAreByteSafe` | 保留 Hex 字节编辑、拖柄、IME、INS/OVR 与源 SHA-256 回归；旧“Binary→Text 补建 LineIndex/绝对行号”断言已由当前 byte-offset/零全文扫描契约替代，不得重新启用该高成本行为 |
| HX-R01 | `tc_hex_binary_crash_recovery_restores_bytes_and_position` | 基线通过，编辑原语待展开 |
| HX-O02 | `tc_binary_text_presentation_survives_session_restore` | 基线通过 |
| HX-F02 | `tc_fr_011_hex_byte_find_all_highlight_and_double_click` | 当前文件基线通过 |
| HX-F02 | `tc_fr_012_hex_byte_all_open_filters_text_targets` | 全部打开文件基线通过 |

## 5. 当前明确未收敛项

- **2026-08-18 已关闭“只能覆盖、不能插入”主故障**：OVR 保持安全默认；INS 通过状态栏常驻按钮、编辑菜单和 Insert 键可发现。直接 Hex 键入不再修改 Scintilla 字符投影，而是先修改 authoritative byte model 再重绘两栏。EOF 是可达插入边界；输入首个 high nibble 生成 X0，第二个 nibble 原位完成该字节；完整 byte selection 在 OVR/INS 中都按一次字节替换。PatchList 会合并对既有 patch payload 的同长覆盖，禁止产生重叠 payload 或每个低 nibble 意外增长文件。该交互采用 [010 Editor Hex Editing](https://www.sweetscape.com/010editor/manual/UsingHexEditor.htm)、[010 Editor Edit Menu](https://www.sweetscape.com/010editor/manual/MenuEdit.htm) 的 INS/OVR 与选区替换边界，并保留 ImHex 明确区分 size-changing 操作的思路。

- **2026-08-17 已关闭可见界面分裂**：B2/B3 不再把地址、Hex、ASCII 拼成一行；B1/B2/B3 均为地址 margin、纯 Hex 网格、独立 ASCII pane 和唯一全局纵向滚动条。`windowedHexUsesUnifiedThreeColumnByteContract` 与真实 228MB/4.9GB 样本共同作为禁止回退到旧拼接投影/双滚动的永久哨兵。
- B1 的鼠标拖到 viewport 边缘自动滚动、跨活动窗口选区尚未自动证明。
- B2/B3 已证明焦点敏感 Copy、逐 nibble 键盘导航、正反向选区、Ctrl+A 全源边界、两表面 angle/pixel wheel、跨投影滚动、常用写键/命令 no-op 和 Ctrl+Home/End；跨投影窗口鼠标选区、右键/Replace/拖放等其余入口尚未完成专项生成。
- Hex Find Next/Prev、正则开关差异、Replace/Replace All 禁用边界，以及搜索结果跨窗口高亮仍只有局部证据。
- DPI/theme/辅助功能、右键菜单、超限剪贴板、保存冲突/失败、每种编辑原语的恢复日志仍待验证。
