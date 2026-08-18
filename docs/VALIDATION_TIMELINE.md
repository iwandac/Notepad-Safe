# 开发与验证时间线

本文记录已经完成的纵向批次、真实 GUI 证据和最小重测边界。它不是提交日志复述，也不替代
`GUI_TEST_COVERAGE_MATRIX.md` 的完整功能契约。开始一个新批次时先查本文，只有命中“重跑触发”
时才重复邻近冒烟；发现缺陷后把它加入对应永久哨兵，而不是不断扩写用户旅程。

> 2026-08-18 边界更正：S2 编辑和后台 ILoader 装载经真实用户验证判定为未实现且不可用，
> 已从生产路径移除。下文早期提交中涉及“晋升/Editable/资源准入”的记录只说明当时测试过什么，
> 不再是有效能力证据；当前 S2/S3 均为窗口化 mmap 只读分析。

## 1. 状态口径

- **切片通过**：代表输入和正式 GUI 路径通过，只能关闭该切片。
- **契约门禁**：有判别语料、动态断言和永久回归入口；相关实现变化时必须重跑。
- **产品关闭**：所有 P0 边界和失败路径都有证据，才允许使用此状态。
- 每条证据绑定提交。工作树有未提交实现时，上一提交的通过结论不能自动覆盖新代码。
- 大文件原件 `../tinytestfile`、`../smalltestfile`、`../bigtestfile` 只读使用，不修改、不复制、
  不删除。临时 fixture 只进入 `QTemporaryDir` 或项目 `build/`。

## 2. 时间线

| 日期/提交 | 批次与已完成内容 | 保留证据/最小冒烟 | 重新执行触发 | 明确未由本批证明 |
|---|---|---|---|---|
| 2026-08-12 `bede313`–`5fc7355` | 独立产品仓库、可重复构建脚本、应用名称与静态 Qt 构建基线 | 构建正式 exe；供应链/版本测试 | CMake、依赖、资源、应用入口或构建路径变化 | 编辑器、加密和大文件成熟度 |
| 2026-08-13 `c52b34c` | 源码按领域聚类，建立 S1/S2 门限与 ScintillaBridge | L1 打开编辑；L2 基础映射/桥接单测 | `EditorTab`、Bridge、门限或装载路径变化 | 真实 S2 连续浏览、资源回退、2 GiB 编辑 |
| 2026-08-14 `1aaa5fe` | Text/Binary 分级路由和 KDBX/会话骨架进入同一 Qt 产品 | Secure 文档/账户代表 GUI；S2/S3 垂直切片 | `OpenDispatcher`、会话、KDBX 保存或 Provider 路由变化 | 无感安全完整失败路径；成熟 Hex/Search |
| 2026-08-17 `8b5b2d7` | 0.3.1 收敛：统一打开、默认安全草稿、会话、账户工作区、窗口化大文件 Provider、正式发布门禁 | Shell 代表矩阵；安全草稿重启；外来凭据恢复；S2/S3 真实文件 | MainWindow、SaveRouter、Session、SecurityIdentity、Windowed Provider 变化 | 统一搜索动态竞态；接近 2 GiB 编辑；全部 Hex 细节 |
| 2026-08-17 `55fcad6` | B1/B2/B3 可见 Hex 三栏、单滚动、ASCII 同步和交互契约 | `windowedHexUsesUnifiedThreeColumnByteContract`、`windowedHexBothPanesWheelInLockstep`、真实 B2/B3 | Hex 布局、选择、滚轮、bytes-per-row 或 Provider 变化 | 跨窗口鼠标拖选、保存失败、全部搜索分支 |
| 2026-08-18 `4e9159e` | Hex 输入/选择/IME/剪贴板成熟度；搜索三态、Literal/Regex/Hex、整词、替换模板、S3 真实搜索；不可见 byte 往返；B1 Text 行号 | H0/H1/S0/S1（见第 3 节）；`tinyBinaryHexRowsReverseSelectionRawSearchAndTextLineNumbers` | `EditorTab`、Hex/ASCII 投影、FindReplaceDialog、LargeFileSearch、PCRE2 或搜索结果模型变化 | 全部打开文件的完整动态竞态；编码 PositionMapper；S2 真实资源门禁 |
| 2026-08-18 工作树 | 加密专项：胶囊丢失立即失效、Secure Text 复制条件清理、锁定态标题脱敏、编辑区快捷键/右键统一入口 | C0；`secureClipboardIsConditionalAndClearedOnLock`；完整 security unit/GUI slice | SecuritySessionService、MainWindow 剪贴板/锁定、EditorTab secure title、VaultDock 变化 | 抗同用户恶意进程；操作系统剪贴板历史/云同步中已经复制出去的历史副本 |
| 2026-08-18 工作树 | 禁用未实现的 S2 编辑：S2/S3 capability 统一只读，删除 LargeFileLoad、ILoader 晋升、资源准入、编辑保存/恢复分支和独立 promotion target | `test_filetierpolicy`；40 MiB 动态浏览/输入/直调保存拒绝；`mmapS2RemainsReadOnlyAndStartsNoBackgroundLoad`、`s2DoesNotQueueBackgroundEditLoad`、`invalidEncodingDoesNotStartBackgroundLoader`、`nearTwoGiBReadOnlyReachesHeadMiddleAndTailWithoutEditLoad`；真实 `smalltestfile` 的 `realB2BinaryRemainsReadOnlyAndStartsNoBackgroundLoad`；恢复导出与旧日志拒绝；KDBX=ON 生产目标构建 | FileTierPolicy、EditorTab L2、Windowed Provider、任务类型或 capability 变化 | 本批不宣称 S2/S3 已具备成熟的全量搜索、wrap 和持续增长日志体验 |

## 3. 可复用最小冒烟集合

### G0：Shell/路由/状态

- `tc_unified_open_focuses_existing_document`
- `tc_recent_files_deduplicate_identity_and_remove_missing_nonmodal`
- `tc_tab_context_menu_complete_user_flow`
- `tc_external_change_save_conflict_reload_roundtrip`
- `tc_background_task_center_lifecycle`

触发：MainWindow、统一打开、标签、保存、状态栏、任务中心或 QAction capability 变化。

### H0：B1 Hex 数据正确性

- `tc_hex_delete_backspace_pairwise_contract`
- `tc_hex_keyboard_navigation_contract`
- `tc_hex_byte_selection_contract`
- `tc_hex_nibble_input_pairwise_contract`
- `tc_hex_copy_cut_pairwise_contract`
- `tc_hex_paste_pairwise_contract`
- `tc_hex_ime_commit_preserves_byte_grid_contract`
- `tc_hex_binary_open_edit_save_as_reopen_roundtrip`

触发：Byte model、输入事件、选择、Undo、剪贴板、保存和 Text/Hex 切换变化。

### H1：窗口化 Hex/真实文件

- `windowedHexUsesUnifiedThreeColumnByteContract`
- `windowedHexBothPanesWheelInLockstep`
- `realB2BinaryRemainsReadOnlyAndStartsNoBackgroundLoad`
- `s3ProductionProviderDragMatchesRenderedHex`
- `tinyBinaryHexRowsReverseSelectionRawSearchAndTextLineNumbers`

触发：Windowed Provider、滚动、行高、ASCII pane、全局 offset 或真实文件路由变化。

### S0：查询语义和替换

- `tc_fr_019_query_option_state_machine_exhaustive`
- `tc_fr_021_text_option_semantics_all_legal_combinations`
- `tc_fr_022_hex_query_formats_errors_and_navigation`
- `tc_fr_023_regex_capture_replacement_gui_contract`
- `tc_fr_025_replacement_value_contract_and_atomic_failures`
- `tc_fr_032_invalid_utf8_binary_default_literal_portable`
- `test_pcre2_regex`

触发：FindReplaceDialog 控件、query options、PCRE2、Literal/Hex parser、Replace/Undo 变化。

### S1：全量搜索、Dock 与大结果

- `tc_fr_007_all_open_files_incremental_results_and_target_jump`
- `tc_fr_010_all_open_cancel_and_rerun`
- `tc_fr_014_invalidated_result_rerun_from_dock`
- `tc_fr_015_all_open_replace_transaction_and_skip_summary`
- `tc_fr_027_real_s3_find_all_dock_double_click`
- `tc_fr_028_real_s3_find_next_preparation_and_repeat_boundary`
- `tc_fr_029_result_spool_virtual_tail_navigation`
- `tc_fr_030_sparse_s3_text_regex_after_two_gib`

触发：SearchSession、目标枚举、generation/revision、Dock、双击导航、spool 或 Provider 搜索变化。

### L0：S2/S3 只读能力边界

- `editorSurfaceBaselineMatchesFullAndWindowedProviders`
- `mmapS2RemainsReadOnlyAndStartsNoBackgroundLoad`
- `s2DoesNotQueueBackgroundPromotion`
- `nearTwoGiBPreviewReachesHeadMiddleAndTailWithoutPromotion`
- `windowedTextWrapReachesPhysicalEofAndKeepsGutter`
- `windowedProviderRemapsAppendAndTruncate`

触发：门限、只读 capability、L2 打开、任务类型、wrap、EOF 或文件变化处理。

### C0：安全文档与账户代表链路

- `saveAsUsesTwoSecurityChoicesAndRoundTrips`
- `unifiedOpenForeignDocumentWithRecovery`
- `unifiedOpenForeignAccountWithRecovery`
- `encryptedUnnamedDraftRestoresSilentlyAcrossRestart`
- `operationLeaseWorkspaceLifecycle`
- `secureClipboardIsConditionalAndClearedOnLock`
- `accountNoteSaveFilterAndLock`
- `recoverableDeleteRestoreAndPermanentDelete`

触发：KDBX、会话、恢复、锁定、Accounts workspace、秘密剪贴板或安全保存变化。

## 4. 重测选择规则

1. 先运行直接命中的集合中的新用例或失败用例。
2. 同一提交内只选择一个最邻近的稳定集合做回归，不默认执行全部集合。
3. 同一集合在最近一个基线提交已经通过、依赖文件未变化时，不因文档或测试注释变化重跑。
4. 阶段门、发布候选、供应链/编译器升级才执行完整 CTest 和正式 exe GUI。
5. 任何数据损坏、错误 offset、旧异步请求覆盖、锁定后明文残留，都立即提升为永久 P0 哨兵。

## 5. 当前批次：统一搜索与跨 Provider 位置映射

起点：`4e9159e`。状态：进行中。

目标：在正式 MainWindow 中证明 Text/Hex × S1/S2 ReadOnly/S3 ReadOnly/Secure × 当前/全部
打开文件共享同一 query、generation、PositionSpace 和导航契约；运行中新增/关闭/编辑目标、
快速 A/B/C 跳转、取消/重跑和锁定清理不会产生旧结果覆盖或错误 selection。

本批优先新增的证据：

- 正式 MainWindow 的 Ctrl+F、F3、Shift+F3、范围切换和 Dock 生命周期。
- UTF-8、UTF-16LE/BE、GB18030、emoji、CRLF 在窗口/chunk 边界的 source-byte ↔ editor-position。
- Find All 后跨多个窗口滚出/滚回，indicator 与 MatchId 不丢失。
- 全部打开文件运行中新增、关闭、编辑、锁定；快速双击 A/B/C 最后请求胜出。
- Replace All 混合 eligible/skipped/failed、revision 变化和每文档一次 Undo。

本批不做：目录搜索、Hex Replace、byte wildcard、B2 写入、Tail、2 GiB+ 编辑。

### 2026-08-18 首个切片

- 基线：`4e9159e`；工作树批次尚未提交。
- `tc_mainwindow_find_shortcuts_preserve_query_scope_and_tab_order` 已从正式 `MainWindow`
  打开两个真实临时文件，通过真实 F3/Shift+F3 键盘事件证明查询保留、当前文件顺序、
  全部打开文件按标签跨越及反向返回；同时断言范围项显示当前可搜索目标数。
- 动态结果：3 passed / 0 failed；视觉证据
  `build/gui-mainwindow-search-shortcuts-scope.png`，正文选区、标签、行号和状态栏已人工复核。
- 本证据关闭 SR-N06/N07 的 S1 Literal 代表切片，不关闭 Regex/Hex、运行中成员变化、
  编码 PositionMapper、Dock A/B/C 或 S2/S3 相同路径。
- `tc_mainwindow_all_open_find_all_utf8_utf16_dock_jump` 随后从正式主窗口打开 UTF-8 与
  UTF-16LE 两个 CRLF 文件（均含中文、emoji），发起全部打开文件 Find All；结果 Dock
  分组/计数/上下文正确，真实双击激活 UTF-16 标签并精确选中 `TOKEN`，正文 indicator
  保持高亮。动态结果 3 passed / 0 failed，视觉证据
  `build/gui-mainwindow-search-utf16-dock-jump.png`。
- 本次视觉复核发现 Dock 旧 delegate 按单个 `QChar` 绘制，会把 emoji 代理对显示为两个
  替换字符，也破坏组合字符/字体回退。已改为整段塑形、按高亮矩形裁剪重绘；截图确认
  UTF-8/UTF-16 两组 emoji 都正常。邻接哨兵
  `tc_fr_005_unicode_long_line_find_all_and_double_click` 3 passed / 0 failed。
- 该切片证明 S1 已解码编辑空间内的用户可见定位正确；原编码 source-byte、S2/S3
  跨窗口映射仍未关闭，后续不能把这里的 UTF-16 通过记录误写成完整 PositionMapper 已完成。

## 6. 2026-08-18 加密专项

起点：`4e9159e` 之后的搜索批次工作树；本节安全改动尚未提交。

- 静态审计与失败优先回归确认：加密笔记从 QScintilla 执行真实 `Ctrl+C` 后，Windows
  锁定虽然遮罩正文并清理搜索结果，但系统剪贴板仍保留明文。原因是 QScintilla 直接处理
  快捷键，绕过了原有账户秘密剪贴板通道。
- 修复后编辑区 `Ctrl+C`、`Ctrl+X`、菜单和右键菜单统一经过 MainWindow 命令；只在存在
  真实选区且源标签未锁定时登记 SHA-256 内容指纹。立即锁定、Windows 锁定及正常退出只在
  剪贴板当前内容仍匹配时清空；用户后来复制的外部内容不会被误删；空选区复制保持 no-op。
- GUI 截图复核发现锁定后正文虽已消失，正文首行派生的标签标题仍泄漏。现在锁定时覆写并
  释放内存标题，标签统一显示“已锁定文档”，解锁后从加密 backing 重新取得真实标题。
- 会话单测确认 `security-session-v1.bin` 被外部删除时，旧 `active_` 会让 `matches()` 继续
  误报身份匹配。现在每次匹配先验证绝对有效期、时间回拨和胶囊存在性；胶囊缺失立即清除
  活跃状态，操作租约返回 Locked，用户可用主密码建立新会话。
- 最终证据：正式 `notepad-safe.exe` 链接成功；`test_security_identity` 默认/主密码轮换/
  恢复验证、`test_kdbx_backend`、`test_vault_contract` 全部成功；`test_session` 59/59；
  `gui_smoke_secure_documents` 8/8；`gui_smoke_vault` 7/7。锁定态视觉证据为
  `build/gui-secure-clipboard-locked.png`，人工复核正文仅有锁定占位、标签无秘密标题、状态栏
  无重叠。邻接普通文本命令用例
  `tc_edit_commands_follow_real_selection_history_and_readonly_state` 3/3，通过真实
  Ctrl+C、Ctrl+X、Undo 和只读 capability，确认统一拦截未造成重复剪切或命令状态回归。
- 明确边界：应用无法撤回 Windows 剪贴板历史、云同步或其他进程在锁定前已读取的内容；
  这与“不以抵抗同用户恶意进程为主要目标”的产品边界一致。应用自身仍必须避免新增长寿命
  明文副本，本用例作为 C0 永久 P0 哨兵保留。

## 7. 2026-08-18 Hex 插入与字节事务专项

- 失败优先证据证明旧测试虽显示“支持 INS”，真实 UI 没有常驻入口；Ctrl+End 又把 EOF
  映射回末字节 low nibble，用户实际上无法从文件尾插入。现在状态栏提供可点击 OVR/INS，
  编辑菜单提供 checked action，Insert 键仍可切换；三者共享同一文档状态。
- 旧 direct nibble 路径只改 Scintilla 的 `00 01 ...` 投影，底层 `VirtualFileView` 要到保存或
  换窗才尝试 diff；多字节选区输入后可见内容、virtual size、Undo 和落盘内容不一致。
  现在所有 Hex 键入先执行 byte-model replace/insert，再从模型重建 Hex/ASCII 两栏。
- PatchList 原先不能组合“先插入 A0，再把同一虚拟字节写成 AB”，会渲染为 `AB A0`；
  选区替换后继续输入也会发生相同重叠。现在同长写入若落在已有 patch payload 内会原位
  更新 payload，并保留最初磁盘快照，使整字节输入成为一个可撤销单元。
- 新增 `tc_hex_insert_mode_discoverable_and_eof_append` 和
  `tc_hex_selected_direct_typing_replaces_complete_range`。失败前分别稳定复现“缺少入口”和
  “选 4 字节输入后 virtual size 仍为 8”；修复后两者 4/4 通过，逐项核对首 nibble A0、
  完整 AB、EOF 长度、即时 raw bytes、Undo/Redo、保存和重开。
- 完整 Hex 专项矩阵随后执行打开/保存、Delete/Backspace pairwise、键盘导航、正反向选择、
  nibble/ASCII 输入 pairwise、Copy/Cut/Paste pairwise、非法输入、真实 IME commit、主窗口
  QAction 和加密恢复。首次 13 passed / 3 failed，额外发现 byte-entry Undo 旧断言、Hex
  光标未同步 ASCII caret、跨用例 Windows 顶层窗口焦点残留；修复后失败集合 5/5 通过。
  最终同进程完整矩阵为 16 passed / 0 failed / 0 skipped（4.687s），记录在
  `build/hex-specialty-final.txt`；最终截图 `build/gui-hex-insert-mode.png` 已人工复核：正文
  为 `00 11 22 33 AB`，ASCII 为 `.."3.`，状态栏显示 `5 B · B1` 与常驻 `INS`，无文字重叠。
- 同一源码和已锁定依赖已成功链接正式 `build/notepad-safe.exe`；没有仅把实验逻辑留在
  测试程序中。专项构建脚本与失败诊断日志在验证完成后删除，只保留最终日志和截图。

## 8. 2026-08-18 tinytestfile ASCII 编辑、Text 滚动与自动换行专项

- 保留样本 `../tinytestfile` 只读打开，专项前后以 SHA-256
  `5B80CF3A6A64836B5A8CF1B44B7E53E4991A51BA673CFF78148DFCE81D15B07D`
  守护；不复制、不保存、不修改该 fixture。
- 失败优先用例首先复现 ASCII pane 连续输入后位置被后台预读重置。根因不是输入字符转换，
  而是缓存预读完成后在 active window 未变化时仍重复发出 `activeWindowChanged`，无条件
  `setText` 把 Scintilla caret 推到 256 KiB 投影末尾。现在缓存填充与投影变化分离，旧投影
  debounce 带 generation 隔离；Hex Undo/Redo 只使用 authoritative PatchList，原生投影撤销栈
  每次重建后清空。
- Binary→Text 的 B1 投影从有损 `QString::fromUtf8` 改为 8-bit 长度写入；NUL、非法 UTF-8
  和任意 byte 保持 1:1 position 与内容。Text/Hex 均只显示 provider 的全局滚动条，内部
  Scintilla 纵横滚动条关闭；Hex 首次打开未构建完整文本索引时，Text 拖动暂按文件字节比例
  寻址，避免被 quick index 的头部覆盖范围钳制。
- 自动换行只调用 Scintilla wrap API，不做 `SCI_LINESSPLIT` 或正文替换；wrap 布局完成后按
  source anchor 重定位。滑块到最大值时使用最后逻辑行的 `SCI_WRAPCOUNT` 到达真正最后显示
  子行，并阻止延迟 UI 通知把 EOF 窗口拉回。Windowed B2/B3 provider 同样在 `SCN_PAINTED`
  后重算尾屏。
- 最终 GUI 证据：`tinyBinaryAsciiEditingTextScrollAndWrapAreByteSafe`、原 tiny 反向选择/字节
  搜索/绝对行号用例、Windowed wrap EOF、Hex 双栏 wheel、B2 单滚动，以及四个 Hex 编辑
  合同全部通过。专项主用例逐项断言 INS/OVR、三步 Undo、75%/100% 物理拖动、单滚动条、
  wrap 前后 raw projection/逻辑行数/virtual size/PatchList 不变及关闭后磁盘 hash 不变；视觉
  证据 `build/gui-tiny-binary-text-wrap-eof.png`。

## 9. 2026-08-18 tinytestfile 真实拖动与立即输入复验

- 用户复验推翻了第 8 节的“可用”结论。原用例只做一次 slider move、等待 500ms，且编辑
  通过程序化 `gotoByteOffset` 放置光标；没有覆盖拖柄连续移动、松手后数秒、远处可见行鼠标
  点击后立即按键/IME commit，因此漏掉实际竞争。第 8 节保留为当时证据，不再外推成熟度。
- 根因一：所有 `SCN_UPDATEUI`（包括投影 setText、paint 和 wrap lazy layout）都启动
  Scintilla→provider 回写；外层全局滚动条与内部投影因此形成双主反馈。现在外层 scrollbar
  是唯一全局坐标源，只有 editor viewport 收到真实 Wheel/导航键时才开放一次反向同步；
  provider 也不再把程序化 `valueChanged` 误作新加载请求，只由 drag 和真实
  `actionTriggered` 换 mmap 窗口，普通 value change 最多在当前 Hex 投影内轻量对齐行。
- 根因二：Binary 以 Hex 打开时按设计跳过 LineIndex，但切换 Text 后也没有补建，导致永久
  混用“byte progress”和“logical line”坐标。现在切换即异步建索引；完成 swap 时先保存当前
  byte anchor，再换算为真实行，禁止 range 改变把内容跳到另一位置。索引未完成时 thumb 目标
  作为 viewport top byte，而不是误作 active-window center。
- 根因三：ASCII pane 每个键都重建 256 KiB raw、约 768 KiB Hex 文本及完整右栏文档；IME
  字符串还逐字符重复。现在同长 OVR 在 byte model 成功后原位更新 Hex/ASCII 两栏，IME commit
  合并为一个 byte transaction；INS/DEL 的结构性重排窗口降至 64 KiB，仍覆盖千行以上。
- 根因四：ASCII 鼠标 release 的 model/status 同步被延后一轮事件；立即输入可能看到旧 offset，
  实际写入相邻字节。现在 ASCII click/drag release 在后续键/IME 之前同步 authoritative selection，
  Hex 表面仍保留 release 后归一化以保护原生反向拖选 anchor。
- 新真实轨迹在保留的 `tinytestfile` 上执行：默认 Hex 连续 24 段拖至 61%，松手稳定 3 秒；
  点击远处屏幕中部右栏并立即 OVR；同位置立即提交 `SAFE` IME 字符串；校验模型 offset、caret、
  byte content、单次 Undo；再执行 INS/OVR、Hex→Text、连续中段拖动、Wrap ON 中段/EOF 拖动及
  磁盘 hash 守护。最终 tiny 邻接集合 4/4、完整 MainWindow Hex 邻接集合 6/6 通过。
