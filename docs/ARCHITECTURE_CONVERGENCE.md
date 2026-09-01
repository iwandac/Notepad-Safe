# Notepad Safe 架构收敛与防回退合同

> 状态：当前重构执行合同
> 日期：2026-08-27
> 权威边界：产品与安全事实以 `DESIGN.md` 为准，用户体验以 `TARGET_EXPERIENCE.md` 为准；本文只规定本轮迁移、验证和退出条件，不建立第二套产品设计。

## 1. 本轮必须解决的问题

本轮不是整理目录或缩短文件，而是同时消除下列共同根因：

1. 测试程序重复编译产品源码，测试组合与正式程序可能漂移。
2. GUI 测试按大型可执行文件注册，反馈慢且统一超时无法区分慢用例与挂死。
3. MainWindow、EditorTab、WindowedReadOnlyProvider 和 SearchResultsDock 同时拥有业务状态、视图和协调职责。
4. 文档中的模块、Document、Provider、CommandRegistry 和 typed position 尚未成为编译边界。
5. Plain/Secure、Text/Hex、S1/S2/S3 和 Accounts 通过条件分支组合，新增能力容易只接入部分模式。
6. 测试覆盖“函数能工作”，但没有系统覆盖第二次操作、状态所有权切换、revision 失效和跨模式一致性。
7. 当前架构没有稳定的文档快照与工具合同，后续任何文档处理能力若直接接入 EditorTab/MainWindow，都会继续扩大耦合。
8. 组合根和启动流程尚未把轻量 descriptor 与重型实现生命周期分开，未来增加 Provider、Tool、账号和模型可能把初始化成本线性加入冷启动。

**当前功能不回退是本轮独立的 P0 约束。** 除非用户对具体 FeatureId 明确批准删除、降级或改变行为，重构不得减少当前已存在的功能、支持模式、快捷键、文件兼容性、数据恢复能力、可见交互质量或性能等级。“新架构更整洁”“测试总数更多”以及“主要路径可用”均不能抵消任何未批准的功能回退。

## 2. 不可混淆的产品事实

- 加密文档主要保护磁盘上的静态文件及恢复数据；解锁后是正常文档工作数据，必须与普通文档共用编辑、搜索和未来文档工具。
- 账号密码、TOTP seed、恢复材料和密钥才是独立运行时秘密；它们不进入通用 DocumentSnapshot、AI、脚本分析或动态插件入口。
- 离线时核心编辑、打开、保存、搜索和恢复必须完整可用。未来本地 AI 可以直接满足该边界；远程 AI 只能在用户明确触发并确认本次数据范围后使用。
- 脚本可视化是对文档或选区的静态解读产物，不等同于脚本执行、IDE、终端或调试器。

## 3. 目标方案

采用模块化单体、Ports/Adapters 和手工构造注入，不引入运行时插件系统或 DI 框架。

### 3.1 编译边界

正式程序和所有测试必须链接同一组产品库：

```text
notepad-safe.exe / GUI harness
          |
          v
notepadsafe_app ---------------> notepadsafe_tools
      |        |                         |
      v        v                         v
notepadsafe_editor -----> notepadsafe_search
      |                         |
      +------------+------------+
                   v
          notepadsafe_domain

notepadsafe_secure / accounts / session / platform
          |        实现 domain/application ports
          +--------------------------------------^
```

约束：

- `notepadsafe_domain` 不链接 Qt Widgets、QScintilla、KeePassXC 或 Windows API。
- `notepadsafe_search` 不包含 EditorTab、MainWindow、Dock 或具体文件持久化类型。
- `notepadsafe_tools` 不包含具体 AI SDK；本地或远程实现是 adapter。
- `notepadsafe_app` 不读取 mmap、KDBX Database、DPAPI blob 或 Scintilla document pointer。
- 测试不得再次列举产品 `.cpp`；只能链接产品 target。
- KDBX=OFF 通过不可用 adapter 表达 capability，不允许宏泄漏到 MainWindow/EditorTab。

### 3.2 唯一组合根与依赖注入

`AppComposition` 是唯一选择具体外部服务/adapter 并建立扩展 registry 的位置。采用普通构造函数参数；纯 UI controller 可由其唯一 Widget owner 创建：

- QObject 视图由 Qt parent 管理；
- 非 QObject 服务由组合根使用 `std::unique_ptr` 管理；
- 只有文件系统、剪贴板、时钟、模型 Provider、KDBX/DPAPI 和编辑内核等外部边界定义接口；
- 禁止 Service Locator、全局可变单例、隐式查找 MainWindow 和为每个类机械创建接口。

### 3.3 文档与工具扩展合同（架构边界，不是本轮产品功能）

```text
DocumentState
  identity + revision + contentKind + persistenceKind + capabilities
        |
        v
DocumentSnapshot
  immutable content/range + encoding + typed source positions
        |
        v
DocumentTool
  descriptor + accepted content + exposure policy + run/cancel/progress
        |
        v
AnalysisArtifact
  source document/revision + typed annotations + text/graph/chart payload
```

- `DocumentProviderRegistry` 静态注册 Text、Hex、Windowed 和后续格式 Provider。
- `DocumentToolRegistry` 静态注册文档处理工具；本轮只验证合同，不交付新的用户工具。
- 工具只接收不可变快照，不能直接持有 EditorTab 或修改 Scintilla。
- 修改型工具返回带基线 revision 的 edit proposal，由统一 DocumentCommand 预检后形成一个 undo transaction。
- 只读分析结果绑定 revision；正文变化、标签关闭或锁定后标为 stale，不得套用旧位置。
- Plain 与已解锁 Secure 使用同一合同；Accounts workspace 不实现通用 DocumentTool target。
- 测试用 `EchoDocumentTool` 和 `StaticGraphTool` 只证明扩展点无需修改核心 Widget；它们不是产品功能，也不引入模型或脚本执行器。

## 4. 现有大类的收敛目标

| 当前对象 | 保留职责 | 移出职责 |
|---|---|---|
| MainWindow | 布局、菜单、对话框、页面/标签解析和跨页面用户旅程 | 文档生命周期真值、文件写入、KDBX/DPAPI/供应商调用、搜索会话真值、后端选择 |
| EditorTab | Qt 文档 surface、具体 Provider 适配和编辑事件投影 | 文档生命周期真值、文件写入、会话序列化策略、搜索查询会话、KDBX/账号业务、工具编排 |
| WindowedReadOnlyProvider | 窗口化只读 Qt 适配 | WindowedByteSource、TextProjection、ReadOnlySelectionModel、搜索快照 |
| SearchResultsDock | 一行结果视图和用户事件 | model 存储、target watch、revision 失效、打开与导航 |
| VaultDock/AccountsWorkspace | 账号页面 | 文档编辑、通用搜索/工具 target、共享 EditorTab 状态 |

文件行数下降不是验收指标；职责和依赖能否由编译器阻断才是指标。

## 5. 一轮重构、多个可回退检查点

本轮禁止并行增加用户功能。每个检查点都必须能构建、运行快速门禁并独立回退。

### R0 行为冻结

- 先把当前工作树中已完成的功能修复、文档修正和无关用户改动分清归属，形成用户确认的不可变基线 revision；重构前后不得共享一个持续漂移的工作树作为比较对象，也不得覆盖或顺带提交无关改动。
- 从正式程序运行时枚举 QAction/CommandId、菜单/工具栏/上下文入口、Dock/Workspace、快捷键、设置键、会话字段、文件格式/版本和 Provider capability，与源码入口及目标体验对账；未登记行为先补 FeatureId 和保留/删除裁决，并由用户确认基线。
- 从确认后的功能总账生成当前旅程清单，记录 FeatureId、输入、状态变化、可见结果和持久化结果。未完成清点的区域只能标记“回退尚未证明”，不得标记“无回退”。
- 为两次已知回归保留强制哨兵：跨模式 Dock 单行几何；搜索选择后连续用户选择和复制。
- 每个新哨兵必须保存“修复前失败、修复后通过”的证据；不能只提交一个始终为绿的测试。
- 保存 KDBX ON/OFF 构建结果、快速门禁时长和核心 GUI 旅程基线。

### R1 构建图收敛

- 先机械迁移源码到 `DESIGN.md` 3.1 的产品 targets，不改变行为。
- 正式程序和测试链接相同 targets；删除 `GUI_SMOKE_SHARED_SOURCES`。
- 通过 target include/link 可见性形成单向依赖；补充轻量 forbidden-include 检查作为辅助。
- 建立最小启动 composition：首帧前只创建 Shell、核心 command descriptor 和 lazy factory；counting factory 证明零重型实例。

当前实现（2026-08-26）：基线 revision 为 `9a80eec`。正式 EXE 与 GUI harness 链接同一组
`notepadsafe_*_runtime`/`notepadsafe_app` 产品组件，测试 CMake 不再列举产品 `.cpp`；架构脚本
检查单一源码归属和禁止依赖。CTest 已按 QtTest 函数拆分，并为 quick/adjacent/release 设置逐项
30–300 秒 timeout，不再使用统一 3600 秒。`AppComposition` 与 `StartupCoordinator` 已进入正式
启动路径；Find/Accounts/Security Center/KDBX service 首次使用创建。启动合同以实际 first paint
信号证明首帧前重型 factory 为 0，100 个未使用 Tool、20 个未使用推理 Provider、20 个测试
PersistenceAdapter 和 20 个附加文档 Provider 均只注册 descriptor、实例化为 0。8 个本地 `testfiles` 样本覆盖 11–108 MiB、
Text/Binary、S1/S2、GBK、UTF-8/BOM、UTF-16LE/BE。

### R2 状态所有权收敛

- 先落地 DocumentState、Capabilities、typed positions 和单一 selection owner。
- 旧 EditorTab API 暂时保留 facade，内部转发到新状态和 Provider；禁止同时写旧、新两份状态。
- 纯逻辑路径在删除旧实现前运行 differential tests，比较旧/新输出。

当前实现：`DocumentSession` 是 identity/location/origin/revision/content/persistence/security/dirty、
恢复路径、安全 backing path 和安全遮罩/迁移状态的唯一非视觉 owner；EditorTab 不再保留平行字段，
且 `DocumentSession` 不暴露可变引用。EditorTab 只通过 `IDocumentContentPort`、
`ISessionDocumentPort`、`IDocumentSurface` 和 `DocumentSearchSurface` 向应用用例投影视图能力。
`SessionManager` 不包含 EditorTab/QScintilla/VirtualFileView，普通文件写入由
`PlainFilePersistence` 执行，EditorTab 源码中不存在 QSaveFile/ReplaceFileW/MoveFileExW。
`WindowedReadOnlyProvider` 使用唯一 `ReadOnlySelectionModel`；反向选择、连续用户选择和重复复制由
纯合同与正式 GUI 双哨兵保护。

### R3 搜索、命令和视图收敛

- SearchSession/ResultStore/NavigationController 不再识别具体 Widget。
- SearchResultsDock 变成纯 view；所有模式使用同一 model/delegate/geometry contract。
- CommandRegistry 根据当前 DocumentCapabilities 统一启停和分派菜单、快捷键、工具栏和上下文菜单。

当前实现：`CommandRegistry` 已将菜单/工具栏/快捷键共享的 command objectName 绑定到稳定
FeatureId，并由一个 `CommandContext` 计算 enable/check 状态；命令只接受显式固定项或受控参数集合，
拼写错误和未登记后缀 fail closed。菜单的 `QMenu` 与对应 `QAction` 使用同一个稳定 ID，避免运行时清单
和 GUI 查询各认一套身份。SearchResultsDock 不再持有 EditorTab，所有 model 绑定、header resize、
uniform row height、禁换行和 elide 规则由 `SearchResultsViewPolicy` 唯一设置；架构门禁禁止 Dock 绕过
该策略。非视觉 `SearchResultsController` 是纯值状态，持有 target/revision/stale/incomplete，MainWindow
负责按 DocumentId 解析标签并处理正文变化、关闭、索引完成和导航。`FindReplaceDialog` 只依赖
`DocumentSearchSurface`，不再识别 EditorTab；查询、scope、generation、cancel、pending 与 skip 计数
由唯一 `SearchSession` 持有。已收敛的 All Open 路径在新一轮查询时失效上一 generation，
晚到 completion 不能让计数下溢。2026-08-29 成熟度 2C 复审发现单文件路径尚未完整服从
该查询代次；2026-08-30 候选以 `SingleFindAllRequest` 持有冻结目标与连接寿命，复用同一
SearchSession 作为代次权威，小/大文件切换和晚到结果已有定向首绿，整片 ON/OFF 尚未关闭。

成熟度 2C 候选将 L1 不可变快照的扫描/一次终态派发放入无 Widgets 的
`EditableTextFindAllTask`（既有 `notepadsafe_search_runtime`），EditorTab 保留 revision、
缓存和正文提交权。L2 的真实工作入口/停止保留量由 `LargeFileSearchTasks.cpp` 内的
一次原子 claim 和原 mutex/CV 结算；通用调度仍唯一归 AsyncTaskManager。
这些候选不新增查询管理器；关闭重入及外部请求通知的验收状态以
`ROUND2_CORE_JOURNEYS_PRODUCT_ACCEPTANCE.md` 为准，尚不代表 2C 通过。

2026-08-30 新发现的边界仍待收敛：普通 S2 打开只有 location，persistence 元数据仍为 None；
工具基座前必须使文档身份元数据与实际持久化路由一致，不能让工具自行猜测。
只读 Provider 的搜索仍借用旧 VirtualFileView mmap，真实 Find All 完成后阻止外部截断/替换；
释放必须同时尊重全部真实 reader 和可见结果/spool 的寿命，不能直接 closeFile 清掉结果。

### R4 安全与未来扩展边界

- 加密文档持久化通过 Document port 接入，不在 EditorTab 建立功能分叉。
- Accounts 单独实现结构化服务和 SecretLease，通用工具 registry 默认拒绝账号秘密。
- 用测试工具证明 Plain/Secure 快照等价、Accounts denied、取消、revision stale 和 edit proposal 单次 undo。
- KDBX ON/OFF 均编译并运行适用合同。

当前实现：`DocumentToolRegistry`、`DocumentProviderRegistry`、`InferenceProviderRegistry`、
`DocumentSnapshot`、`AnalysisArtifact`、`EditProposal`、取消/进度、逐次远程暴露策略和版本化模型
manifest 已成为无 Widgets/KDBX 依赖的编译合同。测试用 Echo/StaticGraph 证明 Plain/已解锁 Secure
等价、Accounts denied、取消、artifact/proposal revision stale；没有接入真实模型或脚本执行器，
因此只能声称扩展合同可接入，不能声称 AI 功能已经实现。文档打开的生产边界进一步收敛为
`IDocumentSurface + DocumentSurfaceOpenRequest`：registry 按 capability/priority 选择 provider，
`DocumentOpenWorkflow` 不再硬编码 provider 字符串，也不依赖 EditorTab/QScintilla；具体 EditorTab
适配只存在于组合根。安全会话和安全文档快照分别通过 `SecuritySessionPort` 与
`SecureDocumentContract` 暴露；MainWindow 的实现文件禁止包含 `SecuritySessionService`、
`SecureDocumentService` 或 `SecurityServiceFactory`。该变化属于当前职责解耦，不把未来 AI 算作
本轮交付功能。

### R5 删除与发布复核

- 只有替代路径的行为矩阵全部通过后才能删除 facade 和旧字段。
- 删除重复 model、重复位置换算、模式专用 UI 分支和过时文档。
- 重跑正式程序核心 GUI、数据完整性、失败注入、性能、供应链和安装包验证。

当前实现：旧的 EditorTab 文件写入 facade、平行生命周期字段、FindReplaceDialog 的 EditorTab 依赖、
SearchResultsDock 的 target/revision owner、SessionManager 的 Widget 依赖均已删除。Hex 投影、大文档
搜索任务、账号条目逻辑和大类实现分别拆入有明确职责的编译单元；全局产品源码硬上限为 2,000 行，
MainWindow/EditorTab 还受跨文件 line/branch/method 聚合预算，不能通过继续拆文件重置复杂度计数。

## 6. 防止测试再次“看起来很多但没有保护”

### 6.1 测试必须链接正式组件

测试 harness 与 `notepad-safe.exe` 只允许在入口和平台窗口包装上不同。任何测试专用源码组合都不能作为产品通过证据。

### 6.2 按合同和状态迁移组织

- Provider contract：打开、能力、快照、搜索、位置、选择、连续复制、保存拒绝和 revision。
- Tool contract：输入范围、取消、进度、stale、只读结果、修改提案和数据暴露策略。
- Command contract：菜单、快捷键、工具栏和右键对同一 capability 得到相同结果。
- View contract：Text/Hex、Plain/Secure、single/multi 使用同一几何和焦点规则。
- Persistence contract：保存保持类型、另存为转换、失败不破坏原文件、恢复可重开。

测试必须覆盖至少两次连续操作和一次状态所有权切换，不能只验证首次成功。

### 6.3 测试门禁分级

CTest 按具体 QtTest 函数注册：

| 门禁 | 用途 | 超时原则 |
|---|---|---|
| quick | 每次受影响修改；纯合同和关键回归 | 单项 30–60 秒，目标总时长不超过 5 分钟 |
| adjacent | 合并前；受影响模块及邻接 GUI | 单项按实测上限，通常不超过 300 秒 |
| release | 阶段门；正式 exe、真实 KDBX、大文件、失败和性能 | 可长时间运行，但仍是单用例超时；禁止每个大型可执行文件统一 3600 秒 |

注册函数同时声明 FeatureId、labels 和 timeout，由同一个 CMake helper 生成 CTest 与覆盖清单，避免脚本、文档和测试列表三份真值。

### 6.4 门禁有效性证明

每个历史回归和新回归测试至少满足一项：

1. 在修复前 revision 上实际失败并保存结构化报告；或
2. 使用违反合同的测试 double，证明断言能够拒绝错误实现；或
3. 对可隔离纯逻辑运行小范围 mutation，证明门禁会变红。

只记录“新代码通过”不能证明测试有效。

## 7. 本轮退出条件

以下条件全部满足前，不得声称重构完成：

**零未批准功能回退门槛：** 功能总账中的每个现有 FeatureId 都必须在重构后的正式程序中保持相同或更好的可见行为、数据结果和性能边界；任何缺失、降级或语义变化都必须由用户明确批准并同步修改产品基线，否则整轮重构失败。

1. 正式程序和 GUI 测试链接同一产品 targets，测试 CMake 不再重复列产品 `.cpp`。
2. KDBX ON/OFF 两套编译门禁通过；关闭 KDBX 不需要修改 MainWindow/EditorTab。
3. 两个历史回归哨兵进入 quick gate，且具备失败有效性证据。
4. 全部已登记核心旅程在重构前后得到相同或经用户批准的更好结果；文件 hash、session、undo 和显示状态均纳入比较。
5. Plain 与已解锁 Secure 对编辑、搜索和测试文档工具具有相同 capability；Accounts secrets 被工具合同拒绝。
6. 新增测试 Provider 和两个测试 DocumentTool 时，不修改 MainWindow、EditorTab、SearchResultsDock 或具体持久化服务。
7. SearchResultsDock 不拥有 target、revision 或导航；EditorTab 不拥有 KDBX/账号业务；MainWindow 不直接调用供应商或文件后端。
8. 默认 quick gate 在增量构建后可持续执行，没有统一 3600 秒等待；release gate 输出每个用例的耗时和失败现场。
9. `DESIGN.md`、目标体验、CMake targets、实现职责和测试覆盖清单一致，不保留“仅文档存在”的目标对象。
10. 用户数据格式保持兼容；任何迁移使用复制、验证、可回退流程，不原地破坏旧文件。
11. 正式 Release 外部进程/内部首帧启动、首次输入和分批恢复满足 `DESIGN.md` 3.7/14.2；增加 100 个未使用 Tool、20 个未使用 Provider 和已安装未启用模型后仍在扩展规模预算内，首帧前重型 factory 调用为零；无受控缓存协议时不声称冷/热启动结论。

这组退出条件不能数学上保证未来永远没有缺陷，但能证明本轮资金没有只购买目录移动：架构边界由编译器执行，扩展点由测试 Provider/Tool 证明，旧功能由重构前后同旅程和持久化结果证明，历史回归由已验证会失败的快速哨兵保护。

## 8. 2026-08-27 最终执行证据与边界

已验证：

- KDBX ON 正式 Release 与全部测试目标构建成功；最终串行 CTest 全矩阵 **153/153** 通过，实耗 **255.39 秒**。其中包含 136 个真实窗口 GUI case、搜索、大文件、Shell、Accounts、Secure、供应链与 E2E；单项 timeout 为 30–300 秒，没有统一 3600 秒等待。此前一轮 Hex 打开/编辑/另存/重开用例出现一次无诊断红灯，随后该用例连续 **20/20** 通过，并在最终 153 项同轮矩阵中通过；这次波动保留为测试稳定性事实，不冒充可复现产品缺陷。
- KDBX OFF 在独立构建目录中构建正式 EXE 和全部适用测试成功；最终适用矩阵 **140/140** 通过，实耗 **259.29 秒**。OFF 证明基础编辑产品不需要在 MainWindow/EditorTab 中编译 KDBX 后端分支；它不替代 ON 配置的加密与账号验证。
- KDBX 保存测试暴露出 KeePassXC 2.7.12 流加密在 payload 恰好填满 1024-byte buffer 时会对空尾块再次调用 cipher，因随机 XML 长度呈概率失败。vendored patch 现将空 stream tail 作为 no-op，并增加精确 1024-byte 确定性断言；修复后 KDBX 后端连续 **50/50** 通过。patch、source-tree SHA-256 和供应链锁同步更新。
- 搜索 Dock 单行紧凑和窗口化只读连续复制两个历史回归哨兵均固定进入 quick；`run_tests.bat`、`run_all_gui_smoke.bat` 使用 `--no-tests=error`，GUI label 由同一注册 helper 生成，空选择和条件编译清单漂移不能再伪绿。
- 真实 `testfiles` 八文件矩阵通过：11–108 MiB，Text/Binary、S1/S2、GBK、UTF-8、UTF-8 BOM、UTF-16LE/BE；每个样本同时核对冻结字节数、大小分层、内容路由、编码和 BOM。
- 正式 Release 外部进程启动 E2E：100 MiB 真实文件 session **521 ms** 可见，100 MiB autosave session **284 ms** 可见，均 exit=0 且未走 L1 阻塞路径；L2 40 MiB `openDocumentByPolicy` **65 ms**，正文连续三帧可见。数据是普通新进程启动，不宣称受控冷缓存 P95。
- 内部启动合同通过（CTest 进程 **0.76 秒**，合同内 first paint <1500 ms、first-paint 后首个干净 idle turn <250 ms）：first paint 前 Security 重型 factory=0；100 个 Tool、24 个 DocumentProvider（4 内建+20 测试）、22 个 PersistenceAdapter（2 内建+20 测试）和 20 个 InferenceProvider 均实例化为 0。`StartupCoordinator` 等待真实 paint，再让出一个事件循环回合，之后才排队外部打开和 session 恢复；测试还通过生产 `EditorTab` 实际打开 fixture，防止把只登记 descriptor 冒充可执行 Provider。
- 已确认两类历史回归均有正式产品哨兵：搜索 Dock 单行紧凑几何；窗口化只读文本在搜索选择后连续用户选择并重复复制。
- 文档打开不再只由 registry 返回描述信息：`DocumentOpenWorkflow` 对普通打开和 session 恢复都调用同一个 capability-based `openBest(IDocumentSurface, request)`。Text S1、Text windowed、Binary Hex windowed、Binary Text windowed 四种路由分别注册，Binary Text 的内容类型与呈现类型不再混为一项；对应恢复回归在全矩阵中通过。
- `notepadsafe_domain` 不再编译 `DraftStore`、`DocumentProviderRegistry` 或链接 `crypt32`/`EditorTab`；恢复存储和文档运行时分别由 `notepadsafe_recovery_runtime`、`notepadsafe_document_runtime` 承担。KDBX 宏只在 vault 实现和需要显示 capability 的 UI adapter 内私有定义，架构门禁拒绝经公共链接接口泄漏。
- 当前最大产品源码为 `VirtualFileView.cpp` **1,983 个物理行**，所有产品 `.cpp/.h` 均低于 **2,000 行**硬上限。`MainWindow.cpp` 为 1,694 行，`EditorTab.cpp` 为 1,704 行，`SearchResultsDock.cpp` 为 523 行；原 2,618 行大文档实现、LargeFileSearch、VaultDock 与 Hex 逻辑已按职责拆分。
- 复杂度门禁使用跨 PowerShell 版本一致的“非空行 + branch + method”计数。MainWindow 四个片段当前/上限为 **5,190/616/112**，EditorTab 七个片段为 **7,115/865/176**；20 个重点文件也各自锁定当前 line/branch 上限。未来修改若增加旧协调器复杂度必须先提取职责，不能靠新文件、空行差异或提高单文件阈值绕过。

完成声明的精确边界：

- 本轮完成的是**当前产品路径的状态与持久化职责下沉、依赖方向、可执行打开 Provider、搜索状态所有权、启动延迟加载和防回退门禁**。未来 AI、模型、远程服务、脚本执行及其产品功能明确不属于本轮重构，也不作为完成度加分项。
- MainWindow 仍是 UI application controller，负责对话框、页面和跨文档用户旅程，但只通过应用/安全端口操作持久化与安全状态，不包含具体安全服务、工厂、KDBX/DPAPI 或普通文件写入实现。EditorTab 仍是具体 Qt 文档 surface/facade，但生命周期真值在 `DocumentSession`，文件写入在 Persistence adapter，会话序列化在 `SessionManager + ISessionDocumentPort`，搜索查询真值在 `SearchSession`。这就是本轮“职责下沉”的完成边界，不虚构两个类已经变成无逻辑的被动 Widget。
- 搜索 target/revision/stale 状态由 `SearchResultsController` 独占，`SearchResultsDock` 不拥有目标生命周期；Dock 保留 Qt model/delegate、选中行解释和导航意图信号，这些属于呈现职责。
- Plain/Binary 打开与恢复走可执行 Provider；普通保存、原子替换和冲突检测走 `PlainFilePersistence`，Secure 文档走 `ISecureDocumentPersistence`，两者都由 `PersistenceAdapterRegistry` 提供强类型路由。当前合同完整覆盖 save/Save As/conflict/commit；没有实现任意第三方动态插件，也不宣称未知格式已经自动具备 load/recovery 能力。
- 153 项 ON 矩阵、140 项 OFF 适用矩阵、八个真实测试文件、两个历史哨兵、KDBX 50 次压力验证和启动合同共同证明已登记当前功能在本轮最终版本未发现回退。门禁能显著降低未来回退漏检概率，但不构成“任何未来修改永远不会产生缺陷”的不可验证承诺。
