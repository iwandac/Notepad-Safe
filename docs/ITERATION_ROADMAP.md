# Notepad Safe 目标模式收敛路线

> 状态：10 轮主迭代完成；0.3.1 发布候选已生成并通过门禁
> 版本：1.0
> 更新日期：2026-08-17
> 预期：10 轮主迭代，允许在证据驱动下收敛于 9–12 轮

## 1. 最终目标

将 Notepad Safe 收敛为可发布的 Windows 离线安全编辑器：

> 普通文本和二进制具备接近 Notepad++ 的完整编辑、Hex 和大文件分级体验；安全 Note 解锁后无缝复用同一编辑能力；账户管理具备现代 KeePassXC 级核心体验；统一打开、保存、另存为、搜索、会话、任务和状态体系全部通过正式 KDBX=ON Release 的真实 GUI 动态验收。

“成熟”不是菜单数量，而是以下核心旅程均无阻断、无数据损坏、无双重概念和无虚假完成：

1. 新建默认安全 Note，七天会话内编辑/保存/恢复无加密提示。
2. 用同一打开入口打开普通文本、安全 Note、未知二进制和账户 KDBX，路由正确。
3. S1 完整编辑；S2 立即浏览并在资源允许时晋升；资源不足自动保持可用只读；S3 能连续浏览和分析全文。
4. 当前文件和全部打开文件搜索语义一致，结果 Dock 一眼可读，双击精确跳转；目录树批处理留给发布后独立立项。
5. Plain/Secure 只通过另存为转换；保存永远保持当前类型；失败不破坏原文件。
6. 账户 CRUD、搜索、生成器、TOTP、历史/回收站和敏感信息短时访问形成完整工作流。
7. 崩溃、断电式保存失败、AppData 丢失、会话过期和历史/外来密码均有可验证恢复路径。

## 2. 优先级和资源约束

固定优先级：

1. 数据完整性、加密恢复和不会误写。
2. 打开—编辑/浏览—搜索—保存的核心旅程。
3. S2/S3 连续浏览和搜索可用性。
4. 安全 Note 与账户的无感融合。
5. 状态、可访问性和视觉成熟度。
6. 低频便利功能。

每轮聚焦一个可端到端验收的垂直切片。只有受影响的单元/组件测试、该旅程的动态 GUI 和少量邻接回归在开发期运行；完整矩阵只在阶段门和发布轮运行，避免重复消耗。

任何轮次不得顺带扩展 IDE、云同步、插件平台、2 GiB+ 编辑、自研密码学或通用二进制分析语言。

## 3. 十轮路线

| 轮次 | 主题 | 主要交付 | 退出证据 |
|---:|---|---|---|
| 0 | 参考与收敛基线 | 参考采用/拒绝清单、最终目标、风险和动态验收协议 | `REFERENCE_BASELINE.md`、本路线、现有正式构建基线 |
| 1 | S2/S3 浏览器成熟切片 | 单一纵向位置；滚轮跨窗口连续；长行横向可达；Hex 自适应有效宽度 | 40 MiB 文本、`smalltestfile`、`bigtestfile` 的拖动/滚轮/横向/缩放 GUI 证据 |
| 2 | S2 晋升与资源准入 | UTF-8/16/GBK 估算、ILoader 阶段、取消、失败/内存不足回退、位置保持 | 32 MiB 边界、数百 MiB、近 2 GiB 稀疏/真实样本；UI 可操作且回退可浏览 |
| 3 | Hex Provider 核心成熟 | B1 nibble/ASCII、选择、撤销、插入/覆盖、保存；B2/B3 只读导航/复制/搜索一致 | 结构化夹具逐字节回读；窄/宽/DPI GUI；数据无错位 |
| 4 | 统一文档生命周期 | 全入口 OpenDispatcher、标签/脏状态、编码/EOL、外部修改、会话/最近文件、原子保存 | Shell/拖放/CLI/最近/恢复同路由；重启及失败注入验证 |
| 5 | 统一搜索与替换 | PCRE2 查询，当前/全部打开，增量 Dock、取消、精确跳转、可编辑目标事务替换 | S1/S2/S3/Text/Hex/Secure 混合范围；跨块正则；双击可见选区；撤销和磁盘不隐式保存复核 |
| 6 | 安全 Note 无感同构 | 默认加密新建/临时、七天会话、锁定、外来密码、恢复密钥、Plain↔Secure 另存为 | 同一 GUI 脚本对 Plain/Secure；零日常提示；AppData 删除后恢复 |
| 7 | 现代账户工作区 | 列表/分组/详情、CRUD、搜索、生成器、TOTP、历史/回收站、剪贴板和键盘路径 | 新建到恢复的账户旅程；锁屏/超时/剪贴板验证；窄窗口 GUI |
| 8 | Shell、命令和状态统一 | CommandRegistry/capability、菜单聚合、状态栏、后台任务中心、设置、主题、可访问性 | 所有现有菜单进入功能总账；禁用原因正确；DPI/键盘/高对比度 GUI |
| 9 | 发布收敛 | 端到端旅程、性能/失败/安全矩阵、供应链验证、清除已替代路径、打包与文档一致性 | KDBX=ON Release 全矩阵；零 P0/P1；已知 P2 有边界和降级；可复现构建 |

轮次是结果单位，不是日历承诺。一轮可能跨多个自主工作回合；如果第 1、2 或 5 轮揭示底层假设错误，允许增加一轮替代实现，但必须遵守“同一方案两轮无指标改善即停止”的规则。

## 4. 每轮固定循环

1. **选切片**：只选一个核心旅程和它的邻接风险。
2. **建立事实**：读取当前代码、已有测试和上轮真实 GUI 结果，不重复证明已确认事实。
3. **写动态验收**：先定义鼠标/键盘动作、可见内容和持久化结果；必要的纯算法再加单元测试。
4. **最小实现**：优先删除冲突路径、复用供应商能力和建立单一真值。
5. **正式验证**：KDBX=ON Release，真实窗口，使用代表性文件和明确超时。
6. **人工可复核**：生成截图、结构化报告和可由用户复现的构建；截图不能替代动态断言。
7. **更新总账**：记录通过、失败、降级和下一轮风险；不把“代码存在”标成“功能完成”。

## 5. 发布门槛

只有同时满足以下条件，目标才可标记完成：

- `DESIGN.md`、`TARGET_EXPERIENCE.md`、参考基线、菜单/状态功能总账与正式软件一致。
- 统一打开和 Save/Save As 类型保持规则不存在旁路。
- S2/S3 没有双滚动条、回跳、不可达区域或搜索结果无法精确定位。
- S2 晋升失败不会失去已打开内容；S3 永不伪装成可编辑。
- Find/Find All/Replace 的范围、正则、取消、增量和结果失效语义一致。
- 安全 Note 解锁后与 Plain Text/Hex 共享功能；会话内日常加解密零提示。
- AppData 丢失仍可凭安全文件加主密码或恢复密钥恢复；保存失败保留原文件。
- 运行时秘密按已声明威胁模型短驻留、清零并避免剪贴板历史/云同步。
- 现有菜单、工具栏、状态栏、后台任务、Dock、快捷键和设置均有归属、GUI 测试或明确移除决策。
- 正式 KDBX=ON Release 在支持的 Windows/DPI 下通过核心动态 GUI、文件结果、失败恢复和供应链校验。

缺少可见行为证据的能力一律视为未完成。低频 P2/P3 问题可以在不破坏数据、安全和核心旅程时进入已知限制；P0/P1 不允许带入发布。

## 6. 当前轮状态

- 第 0 轮：**完成**。参考产品采用/拒绝边界、供应链锁、用户旅程、动态 GUI 协议和十轮停止条件已建立，并在后续各轮按实现证据校正。
- 第 1 轮：**完成**。窗口内滚动改为 Scintilla 逐行滚动，只在边界用重叠窗口换页；文本长行启用横向宽度跟踪并把完整逻辑行扩展到有界投影；Hex 按窗口宽度在完整 4-byte 分组上选择 bytes/row；补齐触控板 `pixelDelta`、Page Up/Down 跨窗、64 位源选区/复制。外部变化路径最终收敛为无持久文件句柄的按需随机读，避免 Windows 长期 mmap/文件 watcher 阻塞原子替换；追加、替换/截断、位置钳制和新头尾显示已通过真实 GUI。正式 Qt 6.11.1 Release GUI 报告 `build/gui-s2-round2.txt` 为 10/10，通过 40 MiB S2 晋升、真实滚轮/触控板、跨窗口选择复制、长行横向、宽窄 Hex、文件变化、UTF-16、加密日志与检查点恢复。关键截图包括 `build/gui-production-s2-preview-dynamic.png`、`build/gui-s2-long-line-horizontal.png`、`build/gui-windowed-remap-truncated.png`、`build/gui-production-s3-dynamic.png`、`build/gui-production-b2-dynamic.png`。
- 第 2 轮：**完成**。正式路由不再在窗口 Provider 背后启动隐藏 `VirtualFileView`/全文件 mmap；33 MiB 打开期间外部原子替换成功。资源预算改按有界投影计算，运行态明确区分 Preview/Preparing/Editable/ReadOnlyAnalysis/ReadOnlyFallback；1.5 GiB UTF-16 扩张越界在分配前回退，任务中心取消和读取途中无效 UTF-8 均稳定回退且保留预览，`2 GiB−4 KiB` 稀疏文本在 1.5 秒预算内打开并可直接到达头/中/尾。`build/gui-s2-round2-final.txt` 为 15/15 通过；正式 KDBX=ON Release 已重链，E2E 文件路由 11 ms、连续三帧正文可见。旧 mmap 搜索源仅在显式搜索时按需打开，列为第 5 轮必须消除的过渡债务。
- 第 3 轮：**完成**。B1 的 64 位 offset、Hex 网格和 ASCII 栏已共享同一 source-byte 坐标与双向 selection；两栏均可输入，Insert 在每标签切换 OVR/INS，nibble、ASCII、粘贴、删除、Undo/Redo 和 PatchList 保存保持逐字节一致。bytes-per-row 依据真实 viewport 以 4-byte 对齐自适应，宽窄重排会先安全 flush，首/末 nibble 不裁切，复用 Scintilla 时不继承错误水平偏移；状态栏按 B1/B2/B3 显示并在可编辑 B1 标注 OVR/INS。Hex byte Find All 的 Dock 分组、高亮、双击 64 位跳转及 ASCII 镜像选区已通过；混合“全部打开文件”只调度 Binary 并报告跳过 Text。B2/B3 继续使用单滚动只读 Provider，不暴露未证明的写穿。正式 GUI 证据：`build/gui-hex-round3-slice.txt`、`build/gui-hex-search-final.txt`、`build/gui-hex-b2-b3-round3.txt`；截图 `build/gui-hex-roundtrip.png`、`build/gui-hex-roundtrip-narrow.png`、`build/gui-hex-byte-search-dock.png`、`build/gui-hex-byte-search-editor.png`。正式 KDBX=ON Release 已重链，E2E 文件路由 13 ms、连续三帧正文可见。
- 第 4 轮：**完成**。文件菜单、拖放/CLI 调用、最近文件和会话恢复共享统一内容探测与文件身份；同一文件的规范路径/硬链接只激活既有标签，二进制的 Text/Hex 呈现选择跨重启保持。最近文件在设置加载后立即重建，按磁盘身份去重，缺失项无阻塞提示并同步持久化删除。普通文本显式检测主导 CRLF/LF/CR，新键入换行继承并在状态栏与编码共同显示；UTF-8 CRLF 经真实键盘编辑、菜单保存、转 UTF-16LE、BOM/正文/行尾磁盘回读通过。编码元数据变化拥有独立 dirty 状态，不会因 Scintilla save-point 限制静默丢失，“以编码重新打开”则正确清除放弃内容后的 dirty。外部改写保存冲突、重新加载、原文件不被覆盖，以及最近/会话/统一打开邻接回归均通过。正式 GUI 证据 `build/gui-round4-lifecycle-final.txt` 为 8/8；正式 KDBX=ON Release 已重链，E2E 首帧约 196 ms、40 MiB 路由 11 ms、连续三帧正文可见。
- 第 5 轮：**完成**。当前文件/全部打开文件共用 Text、PCRE2 Regex 与 Hex byte 查询入口；F3/Shift+F3、Find All、Dock 高亮和双击跳转服从同一显式范围。全部打开文件按事件循环和后台任务增量调度，支持运行中加入/关闭目标、取消后保留完成结果、内容 revision 失效与重跑。S2/S3 搜索不再等待完整 LineIndex：先发布 64 位 byte range 和有界上下文，索引随后到达时 Dock 原位补齐行号且不重搜。全部打开文件 Replace All 先冻结 revision 和完整预检，经一次确认后对所有兼容的 S1 Plain/Secure Text 各形成一个 undo step；任一目标失败会逆序回滚，L2/只读、Hex 和锁定文档显式跳过，成功不自动保存。正式搜索 GUI `build/gui-search-round5-full.txt` 为 18/18；搜索逻辑 32/32、PCRE2 0 failure；正式 KDBX=ON Release 已重链，E2E 40 MiB 路由 12 ms、连续三帧正文可见。目录树搜索/替换不属于本次发布边界，未来必须作为带枚举、权限、备份和原子提交的独立工作流立项，不得塞进“全部打开文件”。
- 第 6 轮：**完成**。新建与无可恢复会话的启动兜底默认建立安全 Note；未命名正文自动保存到 `.nsafe-draft`，普通 Save 保持该加密 backing，只有 Save As 显式选择安全/未加密并完成格式转换。七天 DPAPI capsule 可跨 MainWindow/进程重启静默恢复安全草稿和异地凭据；Windows 锁屏会清空正文、Undo、查找框/结果与账户解密对象，解锁后凭 capsule 无提示恢复，显式锁定则删除 capsule。当前会话不匹配的安全文档和账户 KDBX 共用“文件密码/恢复密钥”对话框，成功后按 identityId+keyVersion 记入密封会话，后续文件及重启不再询问；身份/恢复单元门禁覆盖 AppData 丢失后的文件内恢复槽、密码轮换和历史版本。Text payload 新增 KDBX 加密的精确 UTF-8 附件，同时保留 Notes 可读镜像，修复 CRLF 经 XML 往返变 LF 的同构缺口；旧 Notes-only 文件兼容读取并在下次保存迁移。正式 GUI `build/gui-secure-round6-full.txt` 为 7/7，身份、KDBX、会话门禁通过（会话 59/59）；正式 KDBX=ON Release 已重链，E2E 40 MiB 路由 12 ms、连续三帧正文可见。
- 第 7 轮：**完成**。账户已是与文档并列的中央工作区，覆盖首次空状态、列表/详情、账号 CRUD、结构化脱敏搜索、KeePassXC 密码生成/强度、TOTP、按需秘密访问、30 秒受控剪贴板、回收站恢复/永久删除和外来账户文件的七天无提示重开。保存现有账号会由 KeePassXC Entry History 保留旧版本；历史列表只显示时间、标题和用户名，恢复旧密码后被替代的当前版本仍可反向恢复。适配层避免把 KeePassXC `setTotp()` 的内部历史事务嵌套到账号外层事务；新建账号和安全文档静默保存不产生伪历史。`Ctrl+F` 在账户页聚焦账号搜索，`Ctrl+S` 只在当前账号有未保存编辑时启用并保存；940px 窄窗口动作完整可达。正式账户 GUI `build/gui-account-round7-full.txt` 为 7/7，统一安全文档/账户会话 GUI `build/gui-secure-round7-full.txt` 为 7/7，真实 KDBX 后端更新—历史—密码恢复—再次原子保存门禁通过；视觉证据为 `gui-account-history.png` 与 `gui-foreign-account-recovery-narrow.png`。
- 第 8 轮：**完成**。八个顶级菜单和核心 QAction 已有稳定 Command ID，工具栏/菜单复用同一动作；Undo/Redo、Cut/Copy/Paste、全选、行操作和排序按真实 selection、history、clipboard 与 Provider 可写能力即时启停。窗口化 mmap 的状态栏大小改取 64 位 provider 真值，S2 持续显示预览/准备编辑/可编辑/只读回退，S3/B3 显示只读分析，可编辑 B1 保留 INS/OVR。后台任务 model 修复 Qt begin/end 结构协议并加入致命 model tester，任务中心覆盖运行、失败、取消、清理和安全任务脱敏。视图聚合、System/Light/Dark、工具栏/状态栏/面板持久化和中央工作区切换已收敛；Windows 高对比度优先使用平台 palette，不被应用 stylesheet 覆盖。状态栏 accessible name、About 供应链版本/上游/GPLv3 已完成；本版明确不增加重复“首选项”、空命令面板或不可用目录搜索入口。正式 GUI `build/gui-shell-round8-final.txt` 为 23/23；150% DPI 高对比度截图为 `build/gui-system-high-contrast.png`，About 截图为 `build/gui-about-supply-chain.png`。
- 第 9 轮：**完成**。正式 KDBX=ON Release 的 11 个单元/契约/供应链测试目标全部通过（`build/unit-release-round9.txt`，11/11），供应链锁校验 7 个组件通过。发布轮额外在实际 200% DPI 下复核 S2/S3 单滚动、4.59 GiB 全局位置/搜索和 Hex 编辑/崩溃恢复，分别为 `build/gui-largefile-200dpi-round9.txt` 4/4、`build/gui-hex-200dpi-round9.txt` 4/4；该轮捕获并修复了 future 已结束但 GUI 结果尚未提交时过早报告“搜索完成”的竞态。最终正式 exe 首个可见 Shell 为 205 ms，40 MiB 路由 13 ms，连续三帧正文可见 E2E 通过。版本 0.3.1 由 CMake 单一来源同步到 About、Windows File/Product Version 与包名；`portable_package` 生成后自动验证精确清单、完整 GPLv3、供应链锁、SPDX 2.3 SBOM 和 exe 哈希。发布候选为 `build/NotepadSafe-0.3.1-windows-x64.zip`（14,820,950 bytes，SHA-256 `1590C789D97239F2D49E838E9F05CB5A60C718D67414FFC178BE012709B5072F`）。当前发布边界内未留 P0/P1；目录搜索、Hex 通配符/Data Inspector、内部类型重命名等增强不进入 0.3.1。
- 保留的大文件验收资产：
  - `F:\projects\notepadsafe\smalltestfile`：228,451,550 bytes，用于 B2 中等二进制。
  - `F:\projects\notepadsafe\bigtestfile`：4,928,168,473 bytes，用于 B3/S3 超大文件。

这两个文件不是构建垃圾，清理脚本和人工清理均不得删除。
