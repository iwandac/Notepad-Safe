# 搜索与替换细粒度契约矩阵

本文件是搜索/替换专项的可执行边界，不是抽样用户旅程。每次修复必须选择对应 `SR-*`
契约，以判别语料、明确 byte range 和真实 GUI 操作验证；不能只证明函数返回值。

## 1. 查询状态机

查询解释只有三种稳定状态。灰显控件必须用真实鼠标验证不可改变；领域层仍需防御程序化非法状态。

| 模式 | 合法可选项 | 必须禁用且清空 | 目标约束 |
|---|---|---|---|
| Literal 普通文本 | 区分大小写 × 整词 × 循环 × 当前/全部 | 无 | 所有可搜索 Text/Secure Text；Binary 的文本呈现按当前编码解释 |
| Regex 正则 | 区分大小写 × 循环 × 当前/全部 | 整词、十六进制字节 | 仅有效 UTF-8 文本解释；PCRE2 资源上限生效 |
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
| SR-Q01 | Literal | 元字符 `.[]()^$*+?\\`、ASCII/中文/emoji、CRLF/LF | 元字符按字面量；UTF-8 byte range 与可见字符一致 |
| SR-Q02 | Regex | 点号、字符类、量词、alternation、lookaround、命名/编号捕获、跨行 | PCRE2 语义；Find Next 与 Find All 有序 ranges 相同 |
| SR-Q03 | 非法 Regex | 未闭合组/类、错误转义、超限回溯 | 可见错误；旧结果不冒充新结果；不启动后台扫描 |
| SR-Q04 | 大小写 | ASCII、Unicode case fold、大小写长度变化 | off 是 on 的超集；S1/S2/S3 结果一致 |
| SR-Q05 | 整词 | `cat`/`catalog`/`_cat`、数字、下划线、中文边界、标点；Text/Binary | 查询仍按字面量解释，不开放正则语法；Text 使用 Unicode letter/number/underscore 边界，Binary 原始字节使用 ASCII letter/number/underscore 边界；无效 UTF-8 不得产生“正则错误” |
| SR-Q06 | Hex bytes | 连续、空格、逗号、连字符、`0x`、大小写 | 解析成同一 bytes；结果 PositionSpace=SourceBytes |
| SR-Q07 | 非法 Hex | 奇数 nibble、非 Hex、空分隔、超长查询 | 原子拒绝；不清空有效旧内容为“0 结果” |
| SR-Q08 | 零长度 | `^`、`$`、lookaround、空 alternative | 查找可终止且按 UTF-8 code point 前进；Replace All 明确拒绝 |
| SR-Q09 | 重叠 | `ana` in `banana`、alternation 同起点 | 默认 PCRE2 非重叠全局语义，所有 Provider 一致 |
| SR-Q10 | 边界 | BOF/EOF、空文件、末行无 EOL、跨 mmap/window/chunk | 不漏首尾，不重复跨块命中 |
| SR-Q11 | 长文本 | 超长行、跨行、多字节落在上下文裁剪边界 | context 围绕命中；高亮像素和 byte range 精确 |
| SR-Q12 | 资源安全 | 灾难性回溯、大结果、取消、临时 spool 失败 | 有界失败/取消；部分结果标为 incomplete；UI 不假死 |

## 3. 命令与导航

| ID | 操作 | 细粒度场景 | 断言 |
|---|---|---|---|
| SR-N01 | 打开查找 | 菜单、工具栏、Ctrl+F、已有选区、重复打开 | 单一非模态窗口；查询焦点/选中策略一致 |
| SR-N02 | 打开替换 | 菜单、快捷键、模式往返 | 替换控件显隐正确；查询和合法选项保留 |
| SR-N03 | Find Next | 无选择、当前命中已选、任意选择、BOF/middle/EOF | 严格向后；当前命中已选时前进一处 |
| SR-N04 | Find Previous | 同 N03 | 严格向前；不重复当前命中 |
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
| SR-A05 | Dock 高亮 | Unicode、同一 context 多命中、活动命中 | 一眼可见每个关键词；非命中文本不染色 |
| SR-A06 | 双击跳转 | 点击命中/同行空白、折叠组、失效结果 | 激活正确 tab，正文选中精确 query range；失效行不可跳 |
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
- 后台任务验证 running/progress/cancel/finished 的 GUI 状态及 generation；完成提示不能早于结果提交。
- 每次视觉改动保存对话框、正文、Dock 或确认框截图并人工检查；截图不能代替动态断言。
- 临时 fixture 使用 `QTemporaryDir` 或 `build/` 证据目录；测试结束清除日志和临时数据。

完成不是“现有测试全绿”，而是本矩阵 P0 数据正确性全部有可执行证据、真实 S2/S3/Hex
代表组合通过、主程序构建并完成 GUI 操作验收。

## 8. 当前自动化证据（2026-08-18）

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
| `tinyBinaryHexRowsReverseSelectionRawSearchAndTextLineNumbers` | 从保留的 `tinytestfile` ASCII 区真实反向拖选 Hex 地址 `0x416–0x418`、source `[0x416,0x419)`=`01 E9 24`；剪贴板显示安全 Hex 文本并保留 raw bytes；粘贴到查找框自动启用 Hex bytes；Find Next 跳过当前匹配并命中下一处同 bytes，重置到 BOF 后精确选中原三字节 |
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
字面查询的边界约束，并未变成用户正则。不可见 byte 的剪贴板往返由 `tinyBinaryHexRowsReverseSelectionRawSearchAndTextLineNumbers`
通过真实 fixture 补充。视觉证据为
`build/gui-s3-default-literal-copied-text.png`；同时复跑 `tc_fr_017/019/021/022`，确认
Literal/Regex 状态、合法组合、文本语义和 Hex 语义没有退化。
百万级压力、原编码 PositionMapper、正式 MainWindow 的 Regex/Hex/S2/S3 路径和跨全部锁定入口清理仍按
第 2–6 节及 `DESIGN.md` 14.5 作为后续发布门禁，不能因该数字被标记为完成。

2026-08-18 已补正式 MainWindow 的菜单/F3 S1 Literal 代表和 UTF-8/UTF-16LE Find All/Dock
代表；后者同时修复了结果 delegate 按 `QChar` 拆坏 emoji/组合字形的问题。这里证明的是
S1 解码后编辑空间的导航，原编码 source-byte PositionMapper、S2/S3 和动态成员竞态仍是门禁。
