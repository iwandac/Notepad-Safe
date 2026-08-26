# Notepad Safe 架构收敛与防回退合同

> 状态：当前重构执行合同
> 日期：2026-08-26
> 权威边界：产品与安全事实以 `DESIGN.md` 为准，用户体验以 `TARGET_EXPERIENCE.md` 为准；本文只规定本轮迁移、验证和退出条件，不建立第二套产品设计。

## 1. 本轮必须解决的问题

本轮不是整理目录或缩短文件，而是同时消除下列共同根因：

1. 测试程序重复编译产品源码，测试组合与正式程序可能漂移。
2. GUI 测试按大型可执行文件注册，反馈慢且统一超时无法区分慢用例与挂死。
3. MainWindow、EditorTab、WindowedReadOnlyProvider 和 SearchResultsDock 同时拥有业务状态、视图和协调职责。
4. 文档中的模块、Document、Provider、CommandRegistry 和 typed position 尚未成为编译边界。
5. Plain/Secure、Text/Hex、S1/S2/S3 和 Accounts 通过条件分支组合，新增能力容易只接入部分模式。
6. 测试覆盖“函数能工作”，但没有系统覆盖第二次操作、状态所有权切换、revision 失效和跨模式一致性。
7. 当前架构没有稳定的文档快照与工具合同，未来 AI、文本处理和脚本可视化若直接接入 EditorTab/MainWindow，会继续扩大耦合。
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

`AppComposition` 是唯一创建具体服务、adapter、controller 和 registry 的位置。采用普通构造函数参数：

- QObject 视图由 Qt parent 管理；
- 非 QObject 服务由组合根使用 `std::unique_ptr` 管理；
- 只有文件系统、剪贴板、时钟、模型 Provider、KDBX/DPAPI 和编辑内核等外部边界定义接口；
- 禁止 Service Locator、全局可变单例、隐式查找 MainWindow 和为每个类机械创建接口。

### 3.3 文档与工具扩展合同

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
- `DocumentToolRegistry` 静态注册格式化、摘要、AI 辅助和脚本可视化工具。
- 工具只接收不可变快照，不能直接持有 EditorTab 或修改 Scintilla。
- 修改型工具返回带基线 revision 的 edit proposal，由统一 DocumentCommand 预检后形成一个 undo transaction。
- 只读分析结果绑定 revision；正文变化、标签关闭或锁定后标为 stale，不得套用旧位置。
- Plain 与已解锁 Secure 使用同一合同；Accounts workspace 不实现通用 DocumentTool target。
- 本轮只需实现测试用 `EchoDocumentTool` 和 `StaticGraphTool` 证明扩展点，不引入真实模型依赖或脚本执行器。

### 3.4 后续 AI 与脚本可视化的适配边界

- `IInferenceProvider` 只负责模型调用、流式输出、取消和错误归一化；它不读取标签、路径、账号数据库或剪贴板。
- 本地进程、独立本地服务和未来远程服务都实现同一接口。选择哪一种由用户设置和本次数据授权决定，不在文档模式中写条件分支。
- AI 提示词由 `DocumentTool` 从用户批准的 DocumentSnapshot 构造；Provider 不自行扩大到全文、其他标签或最近文件。
- 加密文档的 AI 中间结果默认只在内存中；用户明确保存的结果走普通 DocumentCommand/SaveRouter。该限制保护离线静态文件，不把正文误归类为账号秘密。
- 脚本可视化工具默认只解析文本并产生 graph/chart/annotation artifact，不执行被分析脚本。未来如增加执行能力，必须作为独立的进程与权限边界立项。
- 模型权重、分词器和本地运行时使用独立的版本化 manifest，记录来源、许可证、hash、格式和硬件要求；核心安装和离线编辑不得依赖启动时下载模型。
- AI 任务复用统一任务中心，但任务类型使用可注册的稳定 kind id，不能继续依赖每增加一种任务就修改多个 `AsyncTaskType` switch 的封闭枚举。

## 4. 现有大类的收敛目标

| 当前对象 | 保留职责 | 移出职责 |
|---|---|---|
| MainWindow | 布局、菜单和视图状态呈现 | 打开/保存路由、账号/KDBX 调用、搜索协调、命令能力计算 |
| EditorTab | 文档表面的 Qt 容器 | 文档身份/持久化、搜索执行、Secure 特判、工具执行、跨 Provider 命令语义 |
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

当前实现：`DocumentState` 已建立 DocumentId/location/origin/revision/content/persistence/security/
capabilities/dirty 单一值对象，source byte 与 editor position 使用不可互换类型；EditorTab 的旧 API
只转发到该状态，不保留第二份 identity、location、revision、content kind 或 security class。
`WindowedReadOnlyProvider` 的生产选择路径使用唯一 `ReadOnlySelectionModel`，反向选择、连续用户选择和
重复复制由纯合同与正式 GUI 双哨兵保护。presentation window、mmap 和控件状态仍正确属于 Provider；
EditorTab 仍是较大的 UI facade，因此不能把“状态真值单一化”上推为全部 UI 职责迁移完成。

### R3 搜索、命令和视图收敛

- SearchSession/ResultStore/NavigationController 不再识别具体 Widget。
- SearchResultsDock 变成纯 view；所有模式使用同一 model/delegate/geometry contract。
- CommandRegistry 根据当前 DocumentCapabilities 统一启停和分派菜单、快捷键、工具栏和上下文菜单。

当前实现：`CommandRegistry` 已将菜单/工具栏/快捷键共享的 command objectName 绑定到稳定
FeatureId，并提供统一 DocumentCapabilities 基线；启动合同拒绝任何未登记 QAction。
SearchResultsDock 头文件已不再声明 target/revision/stale/rerun 状态，这些状态由非视觉
`SearchResultsController` 持有，架构脚本阻止其回流；42 个单/多文件、Text/Hex、S1/S2/S3 搜索 GUI
旅程通过。FindReplaceDialog 到 EditorTab 的执行协调仍是迁移 facade；本轮完成的是导致模式不一致的
结果会话状态与视图分离，不虚构全局 SearchController/NavigationController 已全部改写。

### R4 安全与未来扩展边界

- 加密文档持久化通过 Document port 接入，不在 EditorTab 建立功能分叉。
- Accounts 单独实现结构化服务和 SecretLease，通用工具 registry 默认拒绝账号秘密。
- 用测试工具证明 Plain/Secure 快照等价、Accounts denied、取消、revision stale 和 edit proposal 单次 undo。
- KDBX ON/OFF 均编译并运行适用合同。

当前实现：`DocumentToolRegistry`、`DocumentProviderRegistry`、`InferenceProviderRegistry`、
`DocumentSnapshot`、`AnalysisArtifact`、`EditProposal`、取消/进度、逐次远程暴露策略和版本化模型
manifest 已成为无 Widgets/KDBX 依赖的编译合同。测试用 Echo/StaticGraph 证明 Plain/已解锁 Secure
等价、Accounts denied、取消、artifact/proposal revision stale；没有接入真实模型或脚本执行器，
因此只能声称扩展合同可接入，不能声称 AI 功能已经实现。

### R5 删除与发布复核

- 只有替代路径的行为矩阵全部通过后才能删除 facade 和旧字段。
- 删除重复 model、重复位置换算、模式专用 UI 分支和过时文档。
- 重跑正式程序核心 GUI、数据完整性、失败注入、性能、供应链和安装包验证。

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

## 8. 2026-08-26 最终执行证据与未完成边界

已验证：

- 最终 revision `895fe94` 的 KDBX ON 正式 Release 构建成功；CTest 全矩阵 **153/153** 通过，总实耗 **315.62 秒**。其中 adjacent 133、GUI 136、搜索 42、大文件 24、Shell 51、Accounts 6、Secure 6、E2E 2；单项 timeout 为 30–300 秒，没有统一 3600 秒等待。
- KDBX OFF 正式 EXE、架构/启动合同、smallfixes 与窗口化只读 GUI harness 编译成功；架构、启动、生命周期动作、Hex 保存重开和连续复制代表旅程 **5/5** 通过（8.86 秒）。OFF 只证明编译和 capability 解耦，不表示安全发布验证。
- quick **5/5**，累计测试时间 1.47 秒；两个历史回归哨兵均在其中。架构、供应链和真实 `testfiles` 合同也包含在 153 项最终矩阵中。
- 真实 `testfiles` 八文件矩阵通过：11–108 MiB，Text/Binary、S1/S2、GBK、UTF-8、UTF-8 BOM、UTF-16LE/BE；每个样本同时核对冻结字节数、大小分层、内容路由、编码和 BOM。
- 正式 Release 外部进程启动 E2E：100 MiB 真实文件 session **223 ms** 可见，100 MiB autosave session **213 ms** 可见，均 exit=0 且未走 L1 阻塞路径；L2 40 MiB 正文连续三帧可见 E2E 通过。数据是普通新进程启动，不宣称受控冷缓存 P95。
- 内部启动合同通过（整个测试进程 0.45 秒，合同内 first paint <1500 ms）：first paint 前 Security 重型 factory=0；100 个 Tool、24 个 DocumentProvider（4 内建+20 测试）、22 个 PersistenceAdapter（2 内建+20 测试）和 20 个 InferenceProvider 均实例化为 0。
- 已确认两类历史回归均有正式产品哨兵：搜索 Dock 单行紧凑几何；窗口化只读文本在搜索选择后连续用户选择并重复复制。
- GUI 测试驱动原先在模态对话框尚未出现时执行一次性 timer，断言返回后会把对话框永久留住直至 300 秒超时；现改为有界轮询状态机和 8 秒 watchdog，失败会关闭模态框并给出明确原因，不再用长超时掩盖测试机制错误。

不得上推为完成的边界：

- MainWindow 已清除 KDBX 编译宏、KdbxFormatProbe 和 SecurityIdentity/SecureDocument 静态后端调用；安全能力通过 port/factory 和运行时 capability 表达。它仍协调凭据对话框、打开/保存用户旅程，是较大的 UI application controller，不能称为整体低复杂度完成态。
- 搜索 target/revision/watch/stale 生命周期已从 SearchResultsDock 移入 `SearchResultsController`；Dock 仅保留模型呈现、点击命中计算和导航信号适配。
- EditorTab 的 durable identity/location/revision/content/security 真值已单一化到 DocumentState，窗口化只读选择已单一化到 ReadOnlySelectionModel；presentation/provider 控件状态和搜索执行 facade 尚未全部移出。
- Document/Inference/Tool registry 已落地；PersistenceAdapterRegistry 已接管格式探测优先级与延迟实例化，但 adapter 尚未统一 load/save/conflict/recovery，新增完全不同的持久化格式仍需修改应用协调层，不能声称完整持久化插件化。
- 当前 153 项正式矩阵与 KDBX OFF 代表旅程证明已登记功能未发现本轮回退，并建立了更强门禁；它不构成“未来永不回退”的数学保证，也不能把尚未迁移的职责称为低复杂度完成态。
