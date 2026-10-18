# 搜索与替换细粒度契约矩阵

> 2026-10-02 范围校准：这是既有测试 ID 与语义索引，历史通过记录只适用于当时版本；当前行为见 [搜索现状](../../docs/SEARCH_CURRENT_STATE.md) 和 [正则语义](../../docs/REGEX_SEMANTICS.md)。S2/S3 同为只读，2 GiB 不是功能门槛；下文 ADR-022 是历史方案编号，现行语义以正则语义文档和源码为准。

> 2026-10-17 功能删除（见 [功能取舍](../../docs/FEATURE_DECISIONS.md)）：全部打开文件范围只查找，不再有跨文件全部替换和字节搜索；二进制没有文本呈现。已删除的用例：`tc_fr_015_all_open_replace_transaction_and_skip_summary`、`tc_fr_012_hex_byte_all_open_filters_text_targets`、`tc_fr_034_real_smalltestfile_text_directional_search_progress`、`tc_fr_038_real_tinytestfile_replacement_literal_user_flow`（依赖已删除的二进制文本呈现）、整个 `gui_smoke_search_malformed_encoding` 套件。新增 `tc_fr_all_open_scope_is_find_only`；`tc_fr_019` 的 all-open-mixed 上下文改为无字节模式、无全部替换。

本文件是搜索/替换专项的可执行边界，不是抽样用户旅程。每次修复必须选择对应 `SR-*`
契约，以判别语料、明确 byte range 和真实 GUI 操作验证；不能只证明函数返回值。

本文下方的 SR 契约号按文件名区分。必须记录真实打开路由；强制windowed的小Text测试不能替代原生T或大文件RT验收。

## 1. 查询状态机

查询解释只有三种稳定状态。灰显控件必须用真实鼠标验证不可改变；领域层仍需防御程序化非法状态。

| 模式 | 合法可选项 | 必须禁用且清空 | 目标约束 |
|---|---|---|---|
| Literal 普通文本 | 区分大小写 × 整词 × 循环 × 当前/全部 | 无 | 所有可搜索 Text/Secure Text |
| Regex 正则 | 区分大小写 × 循环 × 当前/全部 | 整词、十六进制字节 | PCRE2 产品方言；非 UTF-8 原始 subject 仅启用明确日志子集 |
| Hex bytes 原始字节 | 循环 × 当前/全部 | 正则、区分大小写、整词 | 当前范围至少有一个 Binary/Hex；全部文件只调度 Binary/Hex |

附加状态：

- 查询为空：Find Next、Find Previous、Find All、Replace、Replace All 均禁用。
- 后台 Find All 运行中：查询、模式、范围、导航、重复启动和替换入口禁用；取消和关闭保持可用。
- 当前范围没有 Binary/Hex：字节模式禁用；从 Binary 切到 Text 后不得保留已勾选的字节模式。
- Regex 与 whole-word、Regex 与 Hex、Hex 与 case/whole 的冲突不能依赖“点击后互相取消”；GUI 必须阻止点击。
- Wrap 只影响 Next/Previous 的边界导航，不改变 Find All 结果集合。

`tc_fr_019_query_option_state_machine_exhaustive` 当前覆盖 Literal 8、Regex 4、Hex 2 个合法
选项组合，并在 current-text、current-binary、current-locked、all-open-mixed 四种上下文中
真实点击所有冲突控件。

## 2. 查询语义契约

| ID | 能力 | 必须展开的维度 | 正确性 oracle |
|---|---|---|---|
| SR-Q01 | Literal | 元字符 `.[]()^$*+?\\`、ASCII/中文/emoji、CRLF/LF、全部支持编码 | 元字符按字面量；typed byte range 与可见字符一致 |
| SR-Q02 | Regex | 点号、量词、alternation、捕获/backreference、lookahead、ASCII 定长 lookbehind、跨行、全部支持编码；UTF 路径另测 Unicode 属性/命名捕获 | PCRE2 语义；非 UTF 子集保持源字符边界；Find Next 与 Find All 有序 ranges 相同 |
| SR-Q03 | 非法 Regex | 未闭合组/类、错误转义、超限回溯 | 可见错误；旧结果不冒充新结果；不启动后台扫描 |
| SR-Q04 | 大小写 | ASCII、可由单 scalar 等长编码展开的字符、`ß/ss` 等长度变化反例 | ASCII 与可安全展开字符一致；多 scalar fold 明确不承诺、不误报为已支持 |
| SR-Q05 | 整词 | `cat`/`catalog`/`_cat`、数字、下划线、中文短语、标点；Text/Binary Text projection | UTF 使用 Unicode 边界；非 UTF 使用 ASCII word 边界，中文按精确短语；无效单元不得产生“正则错误” |
| SR-Q06 | Hex bytes | 连续、空格、逗号、连字符、`0x`、大小写 | 解析成同一 bytes；结果 PositionSpace=SourceBytes |
| SR-Q07 | 非法 Hex | 奇数 nibble、非 Hex、空分隔、超长查询 | 原子拒绝；不清空有效旧内容为“0 结果” |
| SR-Q08 | 零长度 | `^`、`$`、lookaround、空 alternative | 查找可终止且按当前 Source 的一个 Unicode scalar 前进；Replace All 明确拒绝 |
| SR-Q09 | 重叠 | `ana` in `banana`、alternation 同起点 | 产品非重叠全局语义，所有 Provider/编码一致 |
| SR-Q10 | 边界 | BOF/EOF、空文件、末行无 EOL、跨 mmap/window/chunk | 不漏首尾，不重复跨块命中 |
| SR-Q11 | 长文本 | 超长行、跨行、多字节落在上下文裁剪边界 | context 围绕命中；高亮像素和 byte range 精确 |
| SR-Q12 | 资源安全 | 灾难性回溯、大结果、取消、临时 spool 失败 | 有界失败/取消；部分结果标为 incomplete；UI 不假死 |

## 3. 命令与导航

| ID | 操作 | 细粒度场景 | 断言 |
|---|---|---|---|
| SR-N01 | 打开查找 | 菜单、工具栏、Ctrl+F、已有选区、重复打开 | 单一非模态窗口；查询焦点/选中策略一致 |
| SR-N02 | 打开替换 | 菜单、快捷键、模式往返 | 替换控件显隐正确；查询和合法选项保留 |
| SR-N03 | Find Next | 无选择、当前命中已选、任意选择、BOF/middle/EOF、Find All 未完成 | 严格向后；当前命中已选时前进一处；第一处到达即响应，不等待全量结果 |
| SR-N04 | Find Previous | 同 N03；已有/没有前序索引；有界/无界 Regex | 严格向前且不重复当前命中；无索引时保持正确扫描/状态，不使用可能漏报的固定回看窗口 |
| SR-N05 | Wrap | 当前/全部 × 正/反向 × on/off | off 到边界停止；on 只回绕一次且状态可见 |
| SR-N06 | F3/Shift+F3 | 对话框开/关、切标签、Literal/Regex/Hex | 使用最后一次完整查询状态，不暗改范围或模式 |
| SR-N07 | 全部文件导航 | 0/1/N 标签、无命中标签、关闭/新增标签 | 按标签顺序；激活所属标签并选择精确 range |
| SR-N08 | 内容变化 | 命中前/内/后编辑、Undo/Redo、保存 | 缓存按 revision 失效；不得跳旧 offset |

## 4. Find All、正文高亮与结果 Dock

| ID | 能力 | 细粒度场景 | 断言 |
|---|---|---|---|
| SR-A01 | 当前文件 Find All | 0/1/N、同一行多处、多行、跨行 Regex | Dock 计数、分组、ranges 与正文 indicator 一致 |
| SR-A02 | 全部打开文件 | S1/S2/S3/Secure/Text/Binary 混合 | 增量分文件发布；目标过滤明确；不串文件 |
| SR-A03 | 运行中新增/关闭 | 新 L1/L2、目标销毁、切活动标签 | generation/settled 正确；无悬空跳转或重复完成 |
| SR-A04 | 取消/重跑 | 派发前/中/完成边界、连续取消 | 保留已完成结果并标记取消；可立即重跑 |
| SR-A05 | Dock 高亮 | Unicode/Hex、窄宽、超长上下文、同一 context 多命中、hover/selected、行号/字节、单/多文件切换 | 关键词优先完整可见；文件/内容正文起点一致；整行状态和 inline highlight 层级/基线正确；真实结果子行始终保持统一单行紧凑高度 |
| SR-A06 | 双击跳转 | 点击命中/同行空白、折叠组、失效结果 | 激活正确 tab，正文选中精确 query range；导航不改变 modified/revision；失效行不可跳 |
| SR-A07 | 结果失效 | 编辑、Replace、关闭、重新解释编码/模式 | 仅相关文件失效；提供重跑；其他文件仍可跳 |
| SR-A08 | 大结果 | 内存上限、磁盘 spool、64 位 offset | 计数准确；随机页/尾页可达；不把截断当完整 |
| SR-A09 | 早期结果 | LineIndex 未完成、S3 随机位置 | byte offset 先可用；行号完成后原位装饰，不重搜 |
| SR-A10 | 视觉/可访问性 | Dock 打开/隐藏、窄宽、DPI、键盘、主题 | 无截断重叠；列标题、焦点、accessible name 正确 |

## 5. Replace 与 Replace All

| ID | 能力 | 细粒度场景 | 断言 |
|---|---|---|---|
| SR-R01 | Replace 当前 | 当前命中已选/未选、首/中/末、wrap | 已选则替换当前，不跳项；未选先定位；完成后选下一处 |
| SR-R02 | 替换长度 | 0、短、等长、长、Unicode、多行 | 仅目标 range 改变；caret/下一命中 offset 正确 |
| SR-R03 | Literal replacement | `$1`、`\\1`、`\\n`、emoji | 全部按字面量写入，不做捕获或转义扩展 |
| SR-R04 | Regex replacement | `$1`、`${name}`、`$$`、换行/反斜杠 | S1/S2 使用同一明确替换语法；单次与全部一致 |
| SR-R05 | Replace All 当前 | 0/1/N、相邻、长度变化、case/whole/regex | 精确计数；一次合理 Undo；不自动保存 |
| SR-R06 | Replace All 全部 | 多可编辑、Secure、只读、Binary、锁定混合 | 预检清单+确认；明确 skip；每文件一个 Undo |
| SR-R07 | 全部文件事务 | 确认后 revision 变化、标签关闭、第 N 个失败 | 未修改即退出，或回滚此前文件；不产生半批状态 |
| SR-R08 | 只读边界 | S2/S3、B2/B3、权限只读、锁定安全文档 | 所有替换入口禁用；搜索/复制/跳转仍可用 |
| SR-R09 | 空查询/空替换 | 空 find、空 replacement | 空 find 禁止；空 replacement 是合法删除 |
| SR-R10 | 零长度/超量 | 零长度 Regex、超过安全上限、Patch 压力 | 修改前拒绝或明确分批策略；错误不改变文档 |
| SR-R11 | 历史与保存 | Replace/All 后 Undo/Redo、Save/Save As、外部冲突 | 正文、dirty、结果失效、磁盘 hash 一致 |
| SR-R12 | Hex replacement | 当前版本 | UI 明确只查找并禁用 replacement；不得误走文本替换 |

## 6. Provider 与文件矩阵

每个适用的 SR 契约至少在下列 Provider 中选择代表组合；查询语义不得由 Provider 重解释。

| Provider | Find Next/Prev | Find All | Replace | Replace All |
|---|---|---|---|---|
| S1 普通/未命名/安全文本 | 必须 | 必须 | 必须 | 必须 |
| S2 只读分析 | 必须 | 必须异步/可取消 | 禁止 | 禁止 |
| S3 只读日志 | 必须 | 必须异步/可取消 | 禁止 | 禁止 |
| B1 Hex | bytes 必须 | bytes 必须 | 当前禁用 | 当前禁用 |
| B2/B3 Hex | bytes 必须 | bytes 必须异步 | 禁止 | 禁止 |
| 锁定/过渡安全文档 | 禁止正文查询 | 禁止 | 禁止 | 禁止并在多文件摘要列出 |

边界尺寸固定包含：0 B、1 B、31/32/33 MiB、2 GiB−1/等于/加 1、保留的 228 MB 与
4.9 GB 样本。大样本只读，不复制、不修改、不删除。

## 7. 自动化方法与完成条件

- 状态机使用合法组合穷举；内容/Provider/方向等高维交叉使用约束 pairwise，并固定 BOF/EOF、
  跨窗口、零长度和失败事务为强制三元案例。
- 每个查询同时比较 Find Next 顺序、Find All authoritative ranges、正文 selection/highlight、
  Dock 计数与双击目标；只比较“找到几处”不算通过。
- 替换前后比较完整内存 payload、非目标哨兵、dirty、Undo/Redo、磁盘内容和结果失效状态。
- 后台任务验证 priority/queued/running/progress/ETA/cooperative-yield/cancel/finished 的 GUI 状态及
  generation；完成提示不能早于结果提交；预计或实际超过 30 秒只弹一次可点击任务气泡。
- 每次视觉改动保存对话框、正文、Dock 或确认框截图并人工检查；截图不能代替动态断言。
- Dock 结构测试必须断言真实结果索引：单文件为顶层结果行，多文件为文档组的子行；只检查
  文档组、控件属性或 model 数据不能作为结果行布局通过的依据。
- 临时 fixture 使用 `QTemporaryDir` 或 `build/` 证据目录；测试结束清除日志和临时数据。

完成不是“现有测试全绿”，而是本矩阵 P0 数据正确性全部有可执行证据、真实 S2/S3/Hex
代表组合通过、主程序构建并完成 GUI 操作验收。

## 8. 旧实现自动化证据（2026-08-18）

下列记录只证明当时旧搜索路径覆盖的历史切片，不证明 ADR-022 的编码子集编译、
协作式调度或新 Dock 合同。它们不得被简单重跑后用作本次改造的完成证据。

| 用例 | 已证明的契约 |
|---|---|
| `tc_fr_019` | current Text/current Binary/current locked/all-open mixed；Literal 8、Regex 4、Hex 2 合法组合；禁用控件真实点击；空 query、搜索/替换 capability |
| `tc_fr_020` | Replace 先修改当前已选命中，再选择下一处；L1 不依赖 Scintilla 私有查找状态 |
| `tc_fr_021` | 每个合法 Text 组合的 Dock ranges、Find Next、Find Previous、wrap 边界一致 |
| `tc_fr_022` | Hex 五种等价输入、三种非法输入、BOF/EOF、Next、Find All 与 wrap |
| `tc_fr_023` | S1 Regex 编号/命名捕获，单次/全部一致，一次 Undo |
| `tc_fr_024` | 兼容 L2 patch 与 S1 捕获语义一致，dirty/Undo/正文更新 |
| `tc_fr_025` | 空/短/长/Unicode/Literal 元语法 replacement；Undo/Redo；缺失捕获与零长度原子失败；磁盘未保存 |
| `tc_fr_026` | 1205 个 L2 命中跨内部进度批次仍只有一次用户可见 Undo/Redo，dirty 与全文 payload 一致 |
| `tc_fr_027` | 保留的 4.9 GB S3 样本经正式对话框进入 mmap 准备态并完成 Hex Find All；Dock 含精确 64 位 range，真实双击选中 75% 位置的 32 bytes，正文只有一条纵向滚动条 |
| `tc_fr_028` | 全新 4.9 GB S3 标签首次 Find Next 等待异步 mmap 并进入 busy；随后 forward/backward 不重复当前可见选区，非循环边界与 wrap 正确且缓存导航小于 1 s |
| `tc_fr_029` | 60,050 个分行命中越过 `MAX_MATCHES_MEM <= 50,000`；其余结果进入临时 spool，Dock 虚拟滚到最后一行仍正确高亮并真实双击到正文精确 byte range |
| `tc_fr_030` | 2 GiB 后的稀疏有效 UTF-8 marker；S3 Text 大小写不敏感 Regex 命中精确 64 位 range，Dock/正文双击正确，替换控件全程禁用且正文单滚动 |
| `tc_fr_031` | 保留的 4.9 GB Binary S3 中模拟从 ASCII 面板复制文本；默认仅勾选循环查找时按原始字节 Literal 搜索，不进入 PCRE2/UTF-8 错误路径，正文精确选中 64 位 byte range |
| `tc_fr_032` | 含 `FF FE 80/C0` 的小型 Binary；默认 Literal 以不同字母大小写命中 ASCII token；勾选整词后只命中独立 `cat`/`cat!`，跳过 `catalog`/`_cat`，且无效 UTF-8 不产生正则错误 |
| `tinyBinaryHexRowsReverseSelectionRawSearchAndTextByteOffsets` | 从保留的 `tinytestfile` 字符区真实反向拖选并保留 raw bytes；粘贴到查找框自动启用 Hex bytes；Find Next 精确选中源 byte range；切换 Binary Text 后显示 byte-offset gutter 且不启动全文行索引 |
| `tc_mainwindow_find_shortcuts_preserve_query_scope_and_tab_order` | 正式 MainWindow 中关闭查找框后以真实 F3/Shift+F3 保留查询；当前文件正反向、全部打开文件按标签跨越/返回；范围项显示可搜索目标数 |
| `tc_mainwindow_all_open_find_all_utf8_utf16_dock_jump` | 正式 MainWindow 的 UTF-8/UTF-16LE + 中文/emoji/CRLF；全部打开 Find All 两组结果、Dock Unicode 高亮、真实双击、标签激活、正文精确选区与 indicator |

本轮 30 个功能用例全部通过：`tc_fr_001`–`tc_fr_029` 为 **31 passed / 0 failed**（含
init/cleanup），`tc_fr_030` 为 **3 passed / 0 failed**。两轮完整运行中真实 S3 Find All
与 Find Next 链路约耗时 10.9–14.3 s，视觉证据为 `build/gui-s3-real-search-dock.png`、
`build/gui-s3-real-search-editor.png` 与 `build/gui-s3-real-find-next.png`。它关闭的是上述 P0
切片及真实 S3/B3 查找全部/重复导航代表组合；60,050 行尾部虚拟化证据为
`build/gui-search-result-spool-tail.png`。2 GiB 后 S3 Text Regex 优化后耗时 59.2 s，证据为
`build/gui-s3-text-regex-after-2g-dock.png` 与 `build/gui-s3-text-regex-after-2g-editor.png`。
针对默认 Binary Literal 分流新增的 `tc_fr_031/032` 已单独通过；`tc_fr_032` 同时证明整词只是
字面查询的边界约束，并未变成用户正则。不可见 byte 的剪贴板往返由 `tinyBinaryHexRowsReverseSelectionRawSearchAndTextByteOffsets`
通过真实 fixture 补充。视觉证据为
`build/gui-s3-default-literal-copied-text.png`；同时复跑 `tc_fr_017/019/021/022`，确认
Literal/Regex 状态、合法组合、文本语义和 Hex 语义没有退化。
当时尚未关闭的原编码、正式 MainWindow Regex/Hex/S2/S3 与跨全部锁定入口门禁，已由下方
2026-08-22 证据取代；本段数字仍只保留为历史记录。

2026-08-18 已补正式 MainWindow 的菜单/F3 S1 Literal 代表和 UTF-8/UTF-16LE Find All/Dock
代表；后者同时修复了结果 delegate 按 `QChar` 拆坏 emoji/组合字形的问题。这里证明的是
S1 解码后编辑空间的导航；当时未覆盖的原编码 source-byte、S2/S3 和动态成员竞态由下节收口。

## 9. ADR-022 最终验收证据（2026-08-22）

本节是当前实现的完成证据，取代第 8 节的历史完成状态：

- `gui_smoke_findreplace`：35 passed / 0 failed / 0 skipped。覆盖 S1 首命中独立异步导航、
  UTF-8/UTF-16 全部文件分组与双击、GB18030 原始编码 Regex 的 Find Next + Find All +
  Dock 精确跳转、Binary Text/Hex、中文整词、Dock 窄宽/hover/selected/关键词完整显示，且
  导航不改变正文 revision。
- `gui_smoke_large_readonly`：24 passed / 0 failed / 0 skipped。覆盖 40 MiB 与 4 GiB+ 便携
  S2/S3 fixture、64 位 head/middle/tail、首结果不等待全量索引、行号和字符统计并行、动态
  中央线程预算，以及近 2 GiB 异常超长行的有界首屏打开（最终复跑 167 ms）。
- `gui_smoke_smallfixes`：50 passed / 0 failed / 0 skipped。覆盖编码菜单/另存转换、Binary Text
  会话、状态栏任务 ETA 与超过 30 秒气泡、任务失败状态不可被 Finished 覆盖、搜索助手与正式
  MainWindow 快捷键/范围。
- `gui_smoke_coord`、`gui_smoke_vault`、`gui_smoke_secure_documents`：分别 7/8/8 passed，
  0 failed / 0 skipped；覆盖正式启动协调、安全 lane、Vault 与安全文档生命周期。
- 六套 GUI 统一运行合计 132 passed / 0 failed / 0 skipped；视觉截图由用例运行时生成，并同时
  以控件状态、像素 hover 差异、model range、正文 selection/revision 进行动态断言。
- 非 GUI CTest 11/11 通过，包括 PCRE2、原编码 LargeFileSearch、编码、会话、KDBX、安全身份和
  供应链；E2E 2/2 通过；全目标构建与 `check_async_threading.ps1` 通过。

当前产品支持边界以 `docs/REGEX_SEMANTICS.md` 为准：UTF 路径使用完整 PCRE2 Unicode
模式；非 UTF 原始 subject 只启用登记的日志正则子集，超出子集在扫描前短错误拒绝，不以第二
matcher、全文转码或隐式语义降级冒充支持。

## 10. 真实 smalltestfile 定向搜索补充验收（2026-08-23）

第 9 节的通过记录没有覆盖“保留的 `smalltestfile` 以 Binary→Text 显示后，从 BOF 搜索
`field`”这一真实路径，不能用于证明该场景。现场复现显示，旧候选器只按 `f/F` 过滤且命中
密集时不报告进度，Previous 也没有把当前 byte boundary 下传，造成任务长期显示 0%。

新增 `tc_fr_034_real_smalltestfile_text_directional_search_progress`，只读使用真实 108,385,159
字节 fixture，并通过正式 `FindReplaceDialog` 鼠标点击验证 Literal/Regex/Whole Word 的
Next/Previous、任务进度与正文选区。专项运行结果为 3 passed / 0 failed / 0 skipped；六条路径
耗时分别为 24/212/192/197/49/39 ms，总用时 2.325 s。视觉证据为
`build/gui-smalltestfile-text-search-regression.png`，文本选区精确覆盖 source byte range 中的
`field`，Whole Word 正确排除 `field_code`。该专项证据只关闭本次真实路径，不替代整套发布门禁。

## 11. Regex 校验阶段进度补充验收（2026-08-23）

大文件有效 UTF-8 Regex 在进入 PCRE2 匹配前必须先验证完整 subject；旧实现只在匹配阶段回调，
因此校验期间后台任务长期停在 0%。当前实现把校验与匹配映射为同一条单调进度：校验占前 20%，
匹配占后 80%，既不会长时间无反馈，也不会在阶段切换时从 100% 回退。

新增 `tc_fr_035_valid_utf8_regex_validation_progress_is_monotonic`，使用 40 MiB 有效 UTF-8 日志和
无命中 Regex，分别断言 `EditorTab::findAllL2Progress` 与后台任务栏数据源
`AsyncTaskManager::taskProgressChanged` 均出现非零中间值且全程单调。专项运行结果为
3 passed / 0 failed / 0 skipped；只运行这一新增用例，未用既有测试套件代替本次验证。

## 12. Binary Text 有限搜索与 Dock 快照补充验收（2026-08-24）

- `tc_fr_037_replacement_character_projection_literal_reverse`：查询 `�` 可命中真实 U+FFFD 与
  非法编码投影，并在 Previous/Next 中保持准确源 byte range；
- `tc_fr_038_real_tinytestfile_replacement_literal_user_flow`：按用户路径打开真实
  `testfiles/tinytestfile`、切换 Text、粘贴 `H�T$`、只勾选循环，依次执行 Previous、Next、
  Find All；52,893 个结果有限完成，Literal 不进入 PCRE2，Search 任务有中间线性进度并到
  `100.00%`，不产生 LineIndex/字符统计任务；Dock resize 与之后切换编码不改变结果；
- `tc_fr_039_multi_encoding_dock_models_are_independent`：同一 Dock 中 UTF-8、UTF-16LE、
  GB18030 和 Binary 四个模型分别按自己的搜索快照解码；窄宽 resize 后 Context、高亮和位置不变；
- `tc_fr_007_all_open_files_incremental_results_and_target_jump` 邻接复跑通过，证明全部打开文件的
  增量结果与目标跳转没有因模型收敛而回退。

专项运行均为 3 passed / 0 failed；真实 `tinytestfile` 用例 7.306 s，多编码 Dock 428 ms。
最终完整 `gui_smoke_findreplace` 为 41 passed / 0 failed；大文件/Binary GUI 为 24/24，
主窗口/进度/Hex 编辑与会话 GUI 为 53/53。上述证据覆盖本轮实现，但不替代安装包、签名和
人工视觉发布矩阵。
