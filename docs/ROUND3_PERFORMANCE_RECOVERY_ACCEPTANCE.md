# 第 3 轮：性能与故障恢复验收契约

> 状态：性能代码热点与关键取消/关闭故障已收敛；正式空profile首屏 P95 仍超 500 ms，故性能门保持红灯并按 2026-08-31 停止点暂停；完整故障矩阵尚未关闭
> 更新日期：2026-08-31
> 前置条件：第 2 轮已按冻结身份通过；3A 保全曾因真实发布阻断获优先修复例外

### 当前交接与范围（2026-08-30）

本轮完成后直接进入 [AI-D0 设计收敛](AI_PRODUCT_DESIGN.md#9-ai-d0性能轮之后的设计收敛与开发交接)，不安排 Windows 安装、签名或打包分发。本轮启动、资源、取消、保存/恢复与 Secure/Accounts 故障门不因排除分发而降低。

Accounts 分屏恢复焦点 candidate2 已通过 KDBX ON/OFF 定向与相邻动态验收并冻结，详见 3A-2b 记录。接下来收敛本轮剩余性能与故障矩阵；不开始新一轮安装工作。历史 500 ms 首 paint 红灯及 100 ms 输入呈现证据缺口继续有效。

## 1. 本轮用户结果

本轮不靠“打开函数很快返回”或单次最佳数字证明性能。用户应得到以下可感知结果：

- 启动后先出现可输入的最小窗口；恢复标签、账号、安全文档和未来工具不会抢占首帧。
- 小文件可立即编辑；大文本和大二进制先显示正确首屏，再持续浏览、搜索、取消，不生成与文件大小同量级的完整副本。
- 进度、取消和完成与真实工作一致；界面显示 100% 时，Provider、文件或提交已经完成。
- 保存、恢复和安全库在磁盘满、权限拒绝、文件占用、原子替换失败、取消或进程终止时，至少保留一个可验证的旧版本或新版本；未保存内容不被静默丢弃。
- 空闲 Tail、后台搜索和关闭后的任务不持续消耗 CPU、句柄或线程。

## 2. 证据协议

所有性能结论必须绑定 commit/dirty identity、Release 构建、KDBX 开关、Windows 版本、CPU、RAM、存储、Qt/MSVC、fixture hash 和运行命令。每个场景至少 30 轮，分别报告首轮与后续轮次的 P50/P95、最差值和失败数；未执行受控重启或缓存协议时不得称为“冷启动/热启动”。

采样同时记录：

- 外部进程创建、窗口首次可见、首次 paint、探针输入被呈现、首屏数据可见、任务完成的单调时间戳；
- working set、private bytes、commit size、句柄数、线程数和读取字节峰值；
- UI heartbeat 的 P95 与最大间隔、取消请求到用户可见停止的延迟；
- 源文件、目标文件、恢复稿、实际 EXE、诊断制品和归档证据的 SHA-256；不依赖分发包。

测试 harness 只观察正式产品进程和公开 UI/文件结果。生产代码不得加入“睡眠、模拟成功、绕过 KDF、跳过 flush、固定进度”测试后门。性能采集可以有只读诊断事件，但不得记录路径、正文、query、密码、账号字段或密钥材料。

## 3. Fixture 分层

| 语料 | 用途 | 可证明 | 不能证明 |
|---|---|---|---|
| 1 MiB/31 MiB - 1 byte | S1/B1 编辑边界 | 编辑、保存、恢复、Undo、近上限内存 | 大文件窗口化能力 |
| 32 MiB/256 MiB 密集文件 | S2/B2 首屏、滚动、搜索 | 常规大文件 I/O、RSS、取消 | 2 GiB 后 64 位位置 |
| 近 2 GiB 密集或受控生成文件 | 边界与长时搜索 | 大文件真实读取、峰值资源、边界前后定位 | 4 GiB 后坐标 |
| 4 GiB + 64 MiB 稀疏文件 | S3/B3 64 位坐标与有界驻留 | offset、滚动、选择、搜索命中身份不截断 | 密集 4 GiB 吞吐、磁盘占用和完整扫描耗时 |
| 可获得的 4 GiB+ 密集文件 | S3/B3 发布性能 | 真实 I/O、RSS、长时取消、全量扫描 | 不代表其他硬件宣传数字 |

任何稀疏 fixture 都必须记录逻辑大小与 allocated size；二者不得混写。测试目录不得复制 2 GiB/4 GiB 语料，生成器只在受控输出目录创建并在验收后按明确路径清理。

## 4. 切片 3A：启动、首帧与恢复调度

### 设计

沿用 `DESIGN.md` 3.7/14.2：首帧前只构造最小 Shell 与轻量 descriptor；session、KDBX、Accounts、Provider、Tool 和 Inference 重型实例在首次 paint 后或首次使用时创建。恢复当前标签优先，其余按 UI 批次推进，用户输入优先级高于恢复。

### 验收

- 正式 EXE 30 轮外部进程启动：可交互 Shell P95 < 1500 ms；没有受控缓存协议时只称“首轮/重复轮次”。
- 内部 `QApplication` 完成到 first paint P95 < 500 ms。首帧后的真实输入分三段记录：输入进入应用并触发对应命令/正文revision P95 < 100 ms；输入导致的对应控件paint P95 < 100 ms；从发出输入到外部捕获首个清晰像素的未扣减上界 P95 < 200 ms。输入前UIA发现/前台/命中点核验，以及输入后截图编码/落盘均保留原始时刻，但不得混入前两个产品指标。
- 注册 100 个未使用 DocumentTool 与各 20 个 DocumentProvider、PersistenceAdapter、InferenceProvider：内部首帧 P95 增量不超过 5% 或 25 ms 中较大者，首帧前重型 factory 调用为零。
- 混合 20 标签、当前脏 B1、B2/B3、Secure 与 Accounts 会话恢复时，单个UI事件轮目标 < 16 ms、硬上限 50 ms；批次之间必须真实让出事件循环，输入被接受/旧自动结果失效 < 100 ms，连续无响应不得超过100 ms。Secure解锁/KDF等待单列，不冒充UI计算；新建、打开和菜单持续可用。

### 3A 测量设计预审（第 2 轮验收期间，只读准备）

现有 `StartupCoordinator` 已等待 `MainWindow::firstPaintCompleted` 并排队让出事件循环，但 queued turn 不等于输入已呈现。`test_startup_composition` 的注册压力发生在其计时器之前，首次 paint 数值在等待返回后读取；`e2e_startup_verify` 的外部窗口计时又排除了启动等待，且旧明文 autosave 夹具不代表当前加密恢复。保留这些原有功能门，不将它们冒充本轮 30 次正式进程性能证据。

开发前先按下列口径登记场景和采集协议：

- 外部正式 EXE 从创建请求前记录单调时钟；内部另记 QApplication 完成、真实首 paint 完成、恢复开始/完成。计时事件只含固定阶段、PID、计数与单调数值，不含路径、标题、正文、query 或凭据；诊断采集不能改变调度、构造额外 Provider 或执行替代路径。
- Shell 首个输入探针是实际菜单输入及该菜单的呈现，另测 Ctrl+N 后真实文本输入和呈现；不能用 WM_NULL、窗口存在、菜单对象已创建或输入函数返回代替。无受控呈现事件时，将观测精度和未测部分列为缺口，不自行扣除 harness 延迟。
- 将空 profile、外部打开、真实混合会话、注册压力分开。正式 EXE 不添加“注册测试 Provider”的产品开关；100/20 未用注册项的差分测量在真实组合根测试宿主内独立报告，注册过程必须包含在两组可比计时中，实际首 paint 前重型 factory 均为零。
- 每个场景至少 30 个独立样本，保留失败。先固定样本、缓存说明、背景负载和阈值，再测基线，禁止看结果后选择性删除慢轮次。无人为等待的可计算批次与用户输入密码的等待分别标记；解锁等待不能计作 CPU 工作，也不能把 KDF、真实读取或提交耗时隐去。
- 恢复应优先上次当前项，但保持原标签顺序、身份和恢复稿责任；用户在恢复途中新建、切换或关闭的显式意图优先于自动恢复，后续批次不得抢回焦点。现有实现按 manifest 顺序同步处理，`preferredActiveTab_` 只在开始捕获；先用真实旅程建立红灯，再设计最小调度改动。
- Secure 的同步调用目前可能通过既有异步安全通道等待完成，不能仅依据外围函数耗时判断 UI 阻塞。须同时观察真实消息处理/呈现与恢复批次边界；Accounts 与安全工厂继续延迟创建，不能为测量预解锁或降低 KDF。

本节仅细化测量与用户意图合同，尚无新增诊断实现或第 3 轮通过结果。

2026-08-30 执行顺序补充：2D 最后空闲长测期间先将已审阅的恢复保全反例注册为独立测试并运行旧实现基线。2D 继续绑定已冻结并复制的正式 EXE 和原源码/测试快照，不能用后续红灯或新增测试数量改写其既有出口；并行编译/测试作为真实背景负载披露。两条反例动态确认未保存数据责任丢失后，批准仅提前修复此发布阻断缺陷，不等待剩余的被动采样；该例外不是跳过 2D 或扩大功能。第 3 轮性能结论、完整故障覆盖和阶段关闭仍须等第 2 轮通过；修复后的源树需要自己的构建/相邻回归，不能冒用冻结旧 EXE 的结果。

### 3A 切片顺序与已核实的阻断结构

#### 3A-1a 测量有效性先行（2026-08-30）

保全子片之后，先建立可复核测量，不把旧启动门直接循环三十次称为性能验收。源码复核发现：`e2e_startup_verify` 在 `waitForStarted()` 之后才开始可见计时，且可见顶层窗不等于正确主窗口、客户区首帧或 DWM 呈现；它的旧明文 `.tmp` 夹具不代表当前加密恢复。`test_startup_composition` 的注册发生在计时前、首 paint 时刻在等待返回后读取，因此当前只保留功能门含义。

最小执行合同：

- 只读诊断默认关闭，仅显式本地测量启用。记录固定阶段枚举、PID、Windows QPC ticks/frequency、项目序号/总数、结果枚举和五项工厂/实例计数；不得记录路径、正文、query、账户或凭据。不得发新业务信号、pump 事件、改变调用次序、创建第二调度器或把 UI 移到 worker。
- 父进程在启动调用之前记录 QPC；产品在 QApplication 完成、真实主窗口首 paint 信号当刻、空闲回合、外部打开前后、恢复元数据就绪、每项恢复前后及总结束记录。两个独立 elapsed 起点不能直接相减；恢复总结束还须区分未解决项，不能把队列走完称为成功恢复全部内容。逐项墙钟跨度不是连续 UI 阻塞时长，可能含嵌套循环和密码等待。
- baseline/stress 两组注册测量仅放在真实组合根测试宿主，均从 QApplication 完成后、创建组合根之前计时，包含 100 Tool、20 Document Provider、20 Inference Provider、20 Persistence Adapter 的注册成本；首 paint 直接捕获时刻及全部实例数。正式 EXE 不加测试注册开关。
- 原生外部 runner 绑定自己启动的 PID、启动身份与正确 MainWindow；始终排空诊断，记录异常/超时/非零退出/强杀。实际菜单输入前后按自有 HWND、前台和命中归属保护；菜单身份由 owned UIA 确认，呈现由菜单区域实际截图及根代理视觉复核支持。截图采集前后时刻只是呈现观察上界，不扣除采集延迟，也不拿 `WM_NULL`、单一 hash 变化或 `IsVisible` 替代像素结果。
- 第一片仅空 profile、外部普通 33 MiB 各 1–2 次有效性 pilot，加组合根 baseline/stress 测量对照。空 profile 的 ON 身份初设窗口如实记录，不预解锁或改为普通明文新建。pilot 不记性能通过；确认采集正确后冻结 30 轮协议、序列和 P50/P95 算法，保留全部失败及第一轮，不能事后删慢轮次。
- 正式 EXE 的空 profile、外部打开、真实 20 项混合恢复各 30 轮；混合种子由当前真实 SessionManager/DraftStore/安全身份流程生成，每轮恢复同一完整输入。Accounts 指恢复中真实进入账户工作区，不发明持久化格式。注册 baseline/stress 则各 30 个独立测试宿主，交错运行；结果不得混入正式 EXE 分位数。

本段是下一片设计，尚无新诊断实现、pilot 或 30 轮性能结果。存储读取失败/坏清单与合法空清单在当前 `loadSession` 返回值仍折叠；该 fail-closed 故障门另由 SessionManager 负责验证，不在 Coordinator 复制解析器。

组合根测量开始（2026-08-30）：仅测试宿主增加 baseline/stress 参数，复用原注册循环，起点在 QApplication 返回后，首 paint Direct 回调直接保存 QPC/频率与五计数；不启动恢复。ON/OFF 原默认启动功能门均复验通过。新本地 runner 独立进程串行、每样本保存原 stdout/stderr/退出、10 秒外部期限、同源 QPC 整数校验，正负验证16/16。ON 1轮 pilot 两样本有效（111.908/446.753 ms），不计入正式组；协议先冻结于 `3a-composition-series-protocol.md`。

ON 首轮正式组 `3a-composition-series-30` **采样有效60/60，但性能红灯**：baseline P50 **123.039 ms**、P95 **749.904 ms**、最大 **994.966 ms**；stress P50 **119.318 ms**、P95 **708.442 ms**、最大 **757.077 ms**。两组五类实例计数均零、正常退出，P95 增量未超门，但两组都不满足本来登记的内部首 paint P95 <500 ms。不删除慢轮次、不把 runner 的采样成功称为性能通过；具体原因尚未定位，也不能据此直接归因于注册或安全后端。OFF 同协议采集中。既有 Qt timer 退出警告继续保留。

OFF 同协议亦完成 **60/60 有效、性能红灯**：baseline P50 **119.002 ms**、P95 **734.229 ms**、最大 **745.073 ms**；stress P50 **136.714 ms**、P95 **727.577 ms**、最大 **737.656 ms**。原测量 v1 源码与两配置 EXE 已另行冻结存档，不覆盖这些慢样本。下一最小诊断只在测试宿主记录组合根返回、注册返回、窗口构造返回、show 调用前/返回五个 QPC 时刻，原首 paint 信号取值和阈值不动；首 paint 可能发生在 show 内，不能造出负“等待时间”或假设必须晚于 show 返回。先确认慢在哪一段，再考虑产品修复。

独立审阅直接重读 v1 的 120 个 sample/stdout/stderr/run.log，以整数重新计算 QPC 纳秒，全部误差为 0，复算分位数与上述一致；少量 PID 被 Windows 在先前进程退出后复用，时间区间分离，不是并行样本。每个进程的两条既有 timer 警告均保留。v2 只新增五阶段计时，源码 SHA `a2f9b9b0dcb1f2fca2df5ea6004d12618a13ba1e00620f69bf9f81716e1977d1`、runner SHA `e801d1e0a0950f8fae21857ec731138f57dd2b3d16a392ec0b097da6c688d71c`；双配置离线编译与原无参数功能门通过。v2 ON pilot 530.519/121.559 ms 均为有效样本，慢样本主要位于 show 调用内，仍不足以归因。新的30轮协议另存 `3a-composition-v2-series-protocol.md`，不与 v1 合并、不改预算。

v2 WinPS5.1 正负校验 **49/49** 后，双配置各 **60/60 有效**：ON baseline/stress 首 paint P95 **712.104/686.699 ms**，OFF **713.154/721.811 ms**，四组仍红。根按 ON 每个原始样本分段复算：窗口构造 P95 69.514/70.751 ms，注册 P95 0.0003/0.2012 ms，show 调用 P95 639.303/616.515 ms，show 返回后到 paint P95 6.188/6.588 ms；阶段分位不能相加。全部慢样本保留。v2 源 zip SHA `6d3f90348fb710a7cf56a848945ea52eaf71f9a5b10652c8b3554731880aba1e`，两配置 EXE 已拷贝为各自 `3a-composition-v2.exe`。

下一定位仅在测试测量分支派生窗口，覆盖 showEvent 并在始终调用真实基类的前后记录 QPC；不额外调用 winId、不 pump、不跳过 WTS 安全注册。该区间包含基类事件、winId 和 WTS 注册，若慢也不能直接指认 WTS。若将来证实安全通知注册阻塞，不能简单延迟注册后立即开放安全正文：注册失败、注册中关闭、注册期间锁屏、HWND 生命周期和有效锁定状态均须另行设计与验收。现有锁屏测试注入 WM_WTSSESSION_CHANGE 只能证明消息处理，不证明 Windows 真注册成功。

v3 74项纯校验及原默认功能门通过；双配置各60有效样本的首 paint P95：ON baseline/stress **260.189/119.617 ms**、OFF **123.842/122.110 ms**，本批数值满足原门。前后产品实现没有变化，不能称为性能修复或抹去 v1/v2 红灯。另按预先登记的5对交错协议运行归档 v2/v3（20进程），全部有效，v2同样回到110–129 ms，v3为108–228 ms。这排除了“只因v3测试子类才变快”的简单解释，说明目前未定位的时变因素仍须在正式EXE测量中追踪；没有关闭安全通知、无障碍、系统防护或改变网络策略。v3慢至335ms的样本主要在构造段；本批showEvent均很短，未得到WTS造成旧慢段的证据。

#### 3A-1b 空会话可操作 Shell（已独立接受，仅此子片）

继续推进同轮中可独立复现的输入阻断，不能因为间歇计时尚待定位而停止检查用户旅程。涉及 `NSP-SHELL`、`NSP-FILE`、`NSP-SECURE`，沿用上文的空Shell方案：无会话、无外部文件时，启动不等于用户要求创建安全文档；不得自动进入身份初始化/解锁，也不得作为替代悄悄新建明文。保留空的可操作窗口与明确开始提示，恢复完成信号仍准确发出；用户显式“新建”继续走原ON安全认证/OFF普通文档流程。

- 真实 StartupCoordinator + MainWindow + 全新临时 profile，未预置身份、会话或首选项；等待真实恢复完成并观察期间所有模态窗口。ON/OFF启动都不得自行弹出身份框、实例化安全后端或制造文档。
- 正常显示后以公开菜单输入确认 File 菜单可展开，记录真实菜单paint/截图；不是只检查 QAction 存在。此定向测试证明旅程，不代替100ms/30轮正式进程性能门。
- 用户显式点击新建：ON须进入原身份初始化流程，正常取消后无明文替代文档；OFF按原行为创建普通文档。不得降低KDF、写测试凭据或跳过安全边界。随后打开真实小文件仍走原入口，正文与磁盘hash一致。
- 补验恢复已有标签、关闭前保全、外部CLI以及恢复中用户主动打开/新建不回退；只允许移除恢复路径中的两处隐式newFile及相应空Shell提示，不改锁屏注册或整套恢复调度。
- 测试先记录旧实现真实模态阻断，取消只能用于结束失败测试，不能取消后才宣称启动没有阻断。30秒外部期限保持；红灯落盘和旧EXE归档后才放行最小修复。

正式 ON EXE 原样观察 `3a-empty-formal-before2` 已复现全新profile启动自动显示“建立本地安全身份”；根查看真实屏幕上的密码/确认密码/建立身份/取消，未输入凭据，实际点击自有取消后对绑定MainWindow发正常关闭，exit0、无强杀。首版观察器 `3a-empty-formal-before` 已记录同一模态，但 `Process.CloseMainWindow()` 在对话框消失后未关闭正确主窗口，10秒清理失败后只终止自有测试进程；旧日志保留，不将该观察器问题称为产品关闭缺陷。

新增真实GUI基线首轮：ON **0/1，外部30秒超时**，只有initTestCase输出，未到模态观察断言，需先返工测试的顶层QDialog归属判断；不能用超时冒称该业务断言已执行。OFF **0/1，0.97秒**，实际paint/idle/restore均1、modal0，但documents1/heavy1，命中 `IMPLICIT_STARTUP_DOCUMENT`。两份 `3a-empty-shell-baseline-*` 保留。正式EXE与Coordinator旧源码另存 `3a-empty-shell-before*`，产品尚未放行修改。

测试改用 QObject 实际父链辨认顶层对话框后，ON `3a-empty-shell-baseline2-*` **0/1，0.42秒**：modal=1/owned=1/windowReady=1，真实身份框与真实Cancel各1，paint/idle/restore各1，documents0/heavy1，准确命中 `IMPLICIT_STARTUP_MODAL`；不再超时。旧实现测试EXE另存 `3a-empty-shell-old-implementation.exe`。现在仅放行Coordinator两处隐式新建与空提示最小修改；显式New/Open、加密、安全通知、持久化和恢复队列均不改。

候选1的三条真实GUI旅程 ON **3/3，1.80秒**、OFF **3/3，3.06秒**，显式New真实身份/Cancel和普通文件完整正文/hash断言均实际通过。正式EXE两配置另以新profile观察约3秒，不再出现身份框，均正常exit0、无强杀；该观察时长不是启动耗时。独立固定身份为ON `0b1884723a229daf9f819f9f2d25dcd6ca952ffc81dcc4a072b50987b3b324bb`、OFF `93742b25276016c373008c6b0513aba19975c20d8742c977c27910224e16bc43`。

**根视觉验收打回候选1，不能仅凭3/3接受。** 正式空Shell截图仍显示默认“行1列1/UTF-8/0B”等无文档元数据；普通文件打开后，原“当前没有打开的文档”提示仍常驻。已保存截图和候选源码/EXE，先给空视图元数据隐藏、打开后旧提示撤销增加独立红断言，不让共享前置守卫使第二个断言不可达。最小返工允许MainWindow构造阶段调用已有空状态展示函数，以及Coordinator空提示仅清理自身消息的短生命周期；新tab或离开文档工作区撤销提示，但不得清除已替换它的错误、保存或CLI反馈，不新增假文档或平行状态机。

`3a-empty-shell-visual-baseline-*` **0/2，0.69秒** 将两项截图问题分别转为真红：第一例已达菜单实际Paint和空文档/零工厂守卫，再确认五个命名状态标签均错误可见；第二例已通过完整源字节/hash/普通文档能力/clean等守卫，才命中陈旧空提示断言。测试EXE另存 `3a-empty-shell-visual-old.exe` 后放行返工。Qt timer退出警告和旧失败继续保留。

候选2将既有空状态初始化放回 `createStatusBar()`（未增加MainWindow构造函数预算），空提示只在新tab/工作区切换时清理仍属于自己的消息。新增“恢复唯一标签后、finish前实际关闭”和“新反馈不被旧空提示误清除”两条真实旅程；ON **5/5，1.95秒**、OFF **5/5，3.65秒**，见 `3a-empty-shell-candidate2-*`。根另查看双正式EXE `3a-empty-formal-candidate2/empty-shell.png`：约3秒观察后没有身份框、没有假文档元数据，底部真实开始提示可见，两个进程均normal exit0、无强杀。菜单操作的status tip可正常替换该提示；本片不是持久欢迎页，不宣称提示在所有菜单交互后仍常驻。

相邻组 `3a-empty-shell-adjacent1-*`：ON **42/43，127.39秒**，OFF **42/42，123.74秒**。ON唯一失败为旧 `encryptedUnnamedDraftRestoresSilentlyAcrossRestart` 仍要求manifest删除，原正文、密文、无提示断言先通过；与保全子片已接受的保留清单语义冲突。只修该测试为真实完成信号及fresh reader核验唯一安全记录、原DocumentId/恢复路径连续性，不以删除清单迎合旧断言。独立离线编译和真实GUI复验 `3a-secure-manifest-contract{,-lasttest}.log` **1/1，7.62秒**（业务7.07秒）通过，旧失败保留；不伪称原整组43/43。

**根与第二代理接受空Shell子片，整个3A/第3轮未关闭。** 生产身份：ON `2a733b5ed92b17caa31f76373a8a35ca5163949cc146500a0d3a5dae13a46635`，OFF `16e8ce04819c7f0b8129460be99644636b8f51be8404bc95dc39d1769a0d5522`。架构MainWindow5163行/613分支/112方法（预算未变），Editor6915/858/176；91个唯一产品cpp。ON/OFF相邻组74/72条既有timer退出警告未隐藏，不上推为零生命周期问题。

#### 3A-1c 正式进程首帧与菜单观察（最小诊断设计）

在空Shell子片验收后推进正式EXE证据。第一版只覆盖空profile和外部普通文件的启动里程碑/菜单呈现有效性，不冒领逐项恢复批次、密码等待或混合20项性能结果：

- 增加独立轻量 `StartupTrace`，仅 `NOTEPADSAFE_STARTUP_TRACE=1` 精确启用。默认关闭时不连额外业务信号、不读时钟、不创建Provider、不输出；不增加命令行业务分支、延时、事件pump或替代窗口。
- 在实际main路径记录 QApplication返回、composition返回、MainWindow构造返回、show前/返回、真实firstPaint信号当刻、既有postPaintIdleTurn信号、既有sessionRestoreCompleted信号及app事件循环退出。paint时读取既有五项factory计数；不实例化观察目标。时间来自同一Windows QPC，ticks/frequency以十进制字符串输出。
- 固定容量数组记录固定枚举和数字；溢出/时钟失效必须显式报告，不偷偷丢样本。采样时不格式化JSON、不写磁盘/管道；正常事件循环退出后才一次写有界JSON至既有stdout句柄。导出失败不得改变用户业务退出语义，但外部harness必须判样本无效。父进程仍须观察真正exit0，不能用诊断中的exit_code替代析构后进程结果。
- restore完成仅表示既有完成信号，不能解释为所有项目成功。外部打开的独立前后、恢复元数据/逐项耗时将在下一诊断版本补齐，第一版不通过缺失数据推算这些阶段。
- 父harness用现有本地PowerShell/.NET/Win32/UIA观察自建profile、自有PID+启动身份+MainWindow；启动调用前QPC计时，真实鼠标点击文件菜单，并截图菜单区域供根视觉复核。菜单visible/Invoke返回不算呈现；截图采集完成只作为观察上界，保留输入至采集结束的全部开销。前台/命中不是自有窗口时停止输入并保留失败，不操作其他窗口。
- 先1轮有效性pilot，不提前跑30轮或改变原100/500/1500ms预算。默认关闭/精确启用/坏时钟或溢出/JSON字段隐私/父子时钟和PID一致性均先验；正式观察所用EXE另绑定新hash，不能借用候选2或旧composition性能数据。

v1诊断独立预审及双构建通过；`3a-formal-trace-v1-contract-*` ON **9/9，25.98秒**、OFF **9/9，28.04秒**，包含14条诊断业务数据行（QtTest计初始化/清理共16）、原启动、5条空Shell、架构及正式单实例矩阵。生产EXE分别为ON `5a7fd6c26305bc54960b5339f03abd891d36f84fb168e41daf414ee60ee49887`、OFF `0718330a6754f91cc18eb737c00c2ea9fb24a1b09566f22fc95921cca1fe2d6f`；源码zip `3a-formal-trace-v1-sources.zip` SHA `cda84102c654d76d0609c1f4173e4df7833d21b332ad76f6319b8c7a63e5d0df`。双正式输出pilot正常exit0、9阶段/五计数零/单行JSON/无stderr，内部首paint分别128.011/193.247ms；这只是输出有效性。默认关闭的双正式控制也均正常0、stdout/stderr各0字节。控制外层命令曾因对未设置env重复Remove-Item导致exit1，纯命令复现为宿主清理状态；已独立核对两产品进程本身退出及输出，不把宿主失败改写成产品失败或隐去。

外部reader独立纯校验首57项通过后，追加“同QPC值但事件顺序倒置”反例真红；修为同时检查数组位置与时间偏序，**58项通过**，原日志保留。不要求paint一定晚于show返回。独立审查另复现转义重复键被PowerShell合并而绕过raw键数检查，要求固定ASCII协议明确拒绝反斜线并补负例，尚不算最终reader放行。

ON真实菜单 `3a-formal-menu-pilot1` 首次结构采集有效、normal exit0，原runner已按hash冻结。内部首paint240.287ms，启动前至菜单截图上界516.798ms，实际输入至截图96.818ms；但输入本身距首paint124.761ms，首paint至截图221.580ms，不能宣称100ms门通过。**根视觉验收发现截图仍半透明叠影，拒绝以QMenu已存在或首张图证明清晰呈现。** 追加最小观察合同：同一菜单保留首张图及原时间，再按50ms间隔捕获另外3帧，每帧独立前后归属/遮挡检查及QPC；记录实际间隔，不扣除等待/检查/PNG开销，不靠hash变化自动判清晰，不改产品动画或系统偏好。根逐帧判断是否仍叠影；此固定4帧只用于呈现原因诊断，不取最后一帧替换首样本或冒领稳定呈现/P95通过。

reader已拒绝任何反斜线/非printable ASCII、独立单行单对象解析，root **66项纯校验通过**，旧转义重复键反例也已从归档源精确重放保留RED。根另补每帧UIA NativeWindowHandle必须等于已绑定QMenu HWND；冻结runner `3a-formal-menu-pilot2-runner.ps1` SHA `66277d4c41e4d997443d2b89d51d41ad691e7ba76492d199c1e8436b3afb6181`。

双正式 `3a-formal-menu-pilot2` 均结构有效、normal0、4帧完整且根逐张查看：两者frame0明显叠影，frame1仍有淡的工具栏透出，frame2/3清晰。ON实际像素复制相对输入分别72.196–89.327、164.585–178.642、415.085–424.844、485.917–495.144ms；OFF分别60.249–76.215、154.359–164.517、280.392–289.922、346.265–355.440ms。未采到中间的精确消退时刻，不能把首个清晰采样点称为真实完成时刻。ON frame1复制后检查/保存另耗152.856ms，观察器自身开销不隐去。正式trace及窗口观察口径可以继续使用；100ms与30轮性能仍未通过。

#### 3A-1d 菜单立即可读反馈（最小产品候选）

根与只读审查确认产品菜单没有透明背景/opacity样式，当前实测更接近呈现过渡，但尚不凭Qt effects头文件认定具体实现或默认时长。为改进真实首个操作反馈，单独设计产品候选：仅在本应用Qt初始化后、实际firstPaint前明确禁用 `UI_AnimateMenu` 与 `UI_FadeMenu`，其他动画、主题、系统设置、WTS、安全流程、恢复顺序和所有时间门保持不变。不是观察器临时切换或测试专用env；正式产品与测试对照使用同一候选。

- 保留v1正式EXE、旧脚本、单帧/四帧叠影证据，新候选绑定新hash。先在同一不改的四帧runner上运行ON/OFF各1次；根要求第一张实际像素图已清晰，无需等后续帧才去除叠影。
- 若首张仍叠影或新建/打开/退出/单实例回退，打回该候选，不扩大关闭其他效果或放宽时间门。真实GUI默认流程、显式安全New/Cancel与正式单实例相邻门继续通过。
- 只证明最小呈现修复时不能关闭整体3A。观察器尚有首paint到发现菜单/发出输入的延迟；即使首图清晰，也仍须单独证明100ms探针与30轮首帧/正式启动P95，不能把上述4帧末图或单轮数字拿来代替。

候选1已由根和独立代理接受，仅限菜单反馈：`src/main.cpp` 在实际Qt初始化后、窗口前无条件设置上述两项，没有trace专用路径。新ON/OFF正式EXE SHA分别 `6c4f6d98e280769e376e6b68d8a41de0ebe1cef5aced6d4fbf486139f5397695` / `8e76b549d3d2d46ccd0861af4a36ed1e67e88aed491c6af90a64ef1266c938b6`。同一四帧runner、`3a-menu-feedback-candidate1` 两次正常exit0，9阶段/五计数零；根与第二代理各查看全部8图，首张和后续都没有旧工具栏叠影。相邻 `3a-menu-feedback-candidate1-contract{,-lasttest}.log` ON **9/9，22.09秒**、OFF **9/9，21.72秒**；每配置12条既有timer退出警告仍保留。

本次内部首paint ON164.6878ms、OFF638.3319ms；OFF的show调用584.767ms，再次出现慢段，不能凭菜单修复称启动性能通过。首paint到第一张截图上界174.6042/183.9473ms，仍未通过100ms门。接着按事先冻结的 `build-round2c-on/3a-formal-empty-series-v1-protocol.md` 运行ON→OFF交错30轮独立空profile样本，nearest-rank分位，保留首轮、后29、全部30及所有失败；无受控重启/缓存、不称冷/热启动。恢复意图行为测试可并行准备，但不先改产品恢复调度。

正式空profile `3a-formal-empty-series-v1` 已完成 **60/60结构有效，0失败、0缺样**，全部真实normal0，9阶段/五计数零，240张诊断帧完整。根直接由原QPC重算，第二代理独立脚本再复核raw stdout/ownership/host-status/父子时钟/全部8项报告指标与帧时间；首轮、后29与全部30均保留，未补跑。ON01–30与OFF01–15由两代理分别逐张查看首图，根逐张查看OFF16–30；**60张首图均清晰**，没有用后帧或hash猜测替代。

| 全30轮指标（ms） | ON P50 / P95 / 最大 | OFF P50 / P95 / 最大 |
|---|---:|---:|
| QApplication完成→实际first paint | 139.8723 / **624.2156** / 729.2830 | 154.8227 / **657.1518** / 711.9620 |
| 父创建请求→第一张菜单截图上界 | 501.9712 / 902.6750 / 1076.3587 | 528.6381 / 947.3861 / 967.2469 |
| first paint→第一张菜单截图上界 | 186.4064 / 224.0781 / 228.1544 | 187.2321 / 220.8352 / 225.0070 |

首轮内部137.0910/128.1319ms；后29内部P50为172.4192/165.2995ms、P95同全30。内部≥500ms样本ON11、OFF5，**500ms P95门真红**。本空profile的正式创建至已实际呈现菜单上界P95满足1500ms；不代表外部文件/混合恢复或整体启动门通过。60个first-paint→截图上界都超过100ms，当前观察器未证明原100ms门；其中发出输入前P95已耗90.6885/95.2010ms，不能把全部观测延迟直接归因产品。show调用P95为571.0846/597.2788ms；组合根约2–4ms、窗口构造约46–54ms，慢段再次主要在show内，但根因仍未证实，不停安全注册或系统防护试过门。

原始组位于两构建目录，根计算 `3a-formal-empty-series-v1/root-summary.json`，独立 `3a-formal-empty-series-v1-independent-review.json` SHA `8fb4696beea6e42a70729dc65099be8531867d64eb1cf082f158ec2517638165`。现场RAM为68,138,061,824 bytes，CPU Ultra9 275HX/24逻辑线程、Windows26200.9168/F:NTFS；介质型号未测，不称NVMe。首次元数据Registry枚举触发类型转换警告后单独仅读取CPU字符串核对，原元数据与修正说明都保留，未进入产品计时。测试构建在60进程结束后才开始；同机其他负载未受控。

#### 3A-1e 慢show的自有进程资源诊断（设计，非性能修复）

本机有调试器及WPR工具，但尚无已核准的“只采自有PID”栈/等待脚本，也没有匹配本静态Qt构建的源码/PDB。不启动机器范围ETW，不联网找符号，不改窗口动画或安全注册。先仅观察单个全新空profile正式进程：沿现有owned PID/启动身份/Handle与readonly StartupTrace，每约20ms读取这个进程的累计CPU和既有native I/O计数，保存每次读取前后同源QPC、实际间隔，不采其他进程。

- 一次2秒采集，立即开始；采样期间不做UIA、截图、WM_NULL或输入，避免把这些行为混入被诊断的show片段。之后才按自有PID+MainWindow身份正常关闭，保存实际exit、原trace/stdout/stderr/hash、所有失败；不强杀后称成功。若show未被采样区间包围，只记区间覆盖不足，不补造边界。
- CPU是进程累计100ns刻度，I/O是操作系统该进程累计计数，不解释成此函数独占CPU/物理磁盘吞吐。每个读数有采集前后QPC不确定区间；用完全包围show的外侧样本给累计增量上界，用完全位于show内的样本给下界，二者不能误称精确show成本。实际20ms间隔有抖动，全部保留。
- 不对诊断组宣称原30轮性能改善，也不把低CPU自动归因于WTS、DWM或防护软件。只为决定下一步需要CPU热点还是等待栈证据；任何更大范围的系统trace、外部符号或安全策略变更须另行确认。正式程序和原四帧runner不增加新开关/线程/延时。

诊断runner已独立预审：逐行JSON序列化/Flush、Process.Refresh及调度会消耗观察器资源，并可能间接影响被观察进程；这些不直接计入目标CPU/I/O，也不能从目标耗时中扣除。Flush不等于强制物理落盘。接受其用于一次粗粒度诊断，不用于性能验收；2秒从ownership发布后开始，可能错过show开头，缺少边界仍须如实标注。

根实际执行两个pilot后，预先冻结 `build-round2c-on/3a-resource-series-v1-protocol.md` 的五组ON→OFF固定序列，十个新profile进程全部保留，不按快慢追加/替换样本。**独立资源观测验收通过：pilot2/2单列、固定组10/10有效**，全部actual exit0、九阶段/五zero、原始counter单调、时钟及内外端点复算通过；每次采样前后exe/hash匹配已接受3A-2a双正式身份。采样时无项目构建/其他项目GUI，其他机器负载未控制。

| 固定组慢样本 | show墙钟ms | 进程CPU内侧增量ms | 进程CPU外侧增量ms |
|---|---:|---:|---:|
| OFF01 | 658.5948 | 78.125 | 109.375 |
| ON03 | 552.8951 | 15.625 | 93.750 |
| OFF04 | 587.4079 | 31.250 | 78.125 |

三慢样本的外侧CPU增量约为show墙钟13.30%–16.96%，不支持“主要耗时是目标进程持续高CPU计算”；但不能区分I/O等待、桌面同步、调度或其他等待，更不能归因具体组件。累计CPU呈15.625ms量化；请求20ms睡眠，实际各轮query间隔P95为40.74–45.76ms、最大49.9615ms，全部保留。I/O外侧读取约94KB/1902–1905次是进程计数，不是物理磁盘吞吐或已排除磁盘等待。**这不是性能修复，原500ms/100ms门仍未通过。**

证据在双配置 `3a-resource-pilot1` 和 `3a-resource-series-v1-round-01..05`；frozen runner SHA `6d31943b4e924eee8a44c4d231377dffabec2d8ebfea06f44156cd3c17e5a354`，原runner未改。独立脚本 `build-round2c-on/3a-review-resource-series-v1.ps1` SHA `f134b0d3e16414f6d1a7adea6e8c2846344c8ab8e9ec6620d2d5b253cd8b2323`，完整结果 `3a-resource-series-v1-independent-review.json` SHA `1d6792b26a1c60fe3fed43d8b5c4313bb201ec265156b114be8bfeaa5822de8c`；WinPS5.1纯复核exit0，根另从原rows重算同样内外界。

后续只读能力核查排除了看似按PID过滤的PS5.1 `Process.Threads/WaitReason`：本机System.dll方法体实际先NtQuerySystemInformation再筛选，不符合本次只查自有进程的窄边界。SDK wct.h仅证实等待链API/16节点上限与跨进程、COM、网络标志，不能证明最低权限、内部RPC边界或同步查询有界；尚未调用WCT、附加调试器、启用系统ETW或改变权限。深入等待归因需要另立明确采集范围，不能偷偷扩大到其他应用信息。

#### 3A-2a 当前项优先与后续用户选择（行为红灯合同）

先在独立 `smoke_recovery_intent` 建立两条最小真旅程，不修改生产调度来制造基线：

- 用真实打开、编辑与SessionManager保存生成三条记录，两条普通文本及最后一条dirty B1。B1必须有实际字节编辑、加密journal和活动项=2；重启经过真实StartupCoordinator。第一次公开“正在恢复会话… 1/3 个标签页”的同步切面，首先完整交付的应是上次当前B1；最后三标签仍为原manifest顺序，每个身份唯一，正文/字节、dirty、选择及源hash完整。不能手写session或用未完整加载的占位项冒充首项完成。
- 另以三条真实dirty S1恢复稿，加一条启动时公开外部打开的普通文件建立旧preferred。同一1/3切面用真实鼠标点击已交付恢复标签，先确认选中确实从外部项改变，再让余下恢复完成；最终仍是该用户选择的widget/DocumentId，不能跳回旧preferred或后来自动加入的标签。若它恰好也是manifest活动项，不要求与自己不等；断言针对实际用户选择，不耦合具体队列算法。
- 每个用例区分fixture守卫、切面与最终业务判据；异步保存清理必须等原任务结束，清理不能补采或修复已冻结失败切面。根运行双Release配置；子代仅开发测试，旧实现真实红灯和测试EXE归档后才设计最小产品修复。
- 这两项不是30轮恢复性能、8/16ms批次、实际OS输入延迟或Accounts安全工作区证据。Accounts须追加ON真实安全会话与工具入口旅程；OFF不能用普通标签替代。已有在途关闭、取消、Retry、顺序保全和正式单实例门都须在产品候选后复跑。

双配置真实基线 `3a-intent-baseline-contract{,-lasttest}.log` 均 **0/2**：ON2.45秒、OFF4.02秒。两例均先到达切面与最终完整数据守卫；首例实际先交付record0而非active record2；次例真实点击已改选且身份/正文保持，终态却被旧preferred覆盖。每配置4条既有timer警告保留。双测试EXE另存 `3a-intent-old-implementation.exe`、原Coordinator/测试/CMake存 `3a-intent-baseline-sources.zip`，不覆盖红灯。

仅放行这两条普通文档行为的最小候选：Coordinator将恢复处理次序与原记录索引分离，活动项先处理，其余保持原相对顺序；原items、SessionTab、责任阶段与capture格式不变。已交付标签按原相邻项插入，而非恢复完成后全表排序；用户已完成的重排不得在finish里被强制回滚。普通恢复创建沿既有PreserveCurrentWorkspace入口保留当前选择，失败页也不得无条件抢选；移除固定旧preferred的每批/最终覆写。没有用户/外部选择时第一项自然选中，关闭后不复活；进度仍为已处理数，游标/完整责任交接早于可重入视图信号。安全正文采用既有认证/加载路径，不能借此关闭WTS、预解锁或更改账户凭据清理。Secure/Accounts的完整不抢工作区合同另立真实ON旅程，不由普通两例冒领。

根预审在编译前拦下一个相邻回退：Secure的既有adopt会主动选中自身，若直接删除finish旧选择纠正，后续Secure将覆盖本来应保留的普通active。当前只批准Coordinator内兼容：每次Secure前保存当刻current，不是启动旧preferred；完整交付后只有current仍为自动Secure目标、先前页仍存在且未关闭时才恢复这个当刻选择。首项Secure且没有先前页时自然选中。认证、masked恢复与Accounts清理路径不改；**这不解决原有安全加载内部被adopt覆写选择或切走Accounts的问题**，不能宣称完整安全意图已过。另补ON真实混合会话守卫，验证此普通候选不新增原最终选择回退。

追加恢复过程中重排验收：三份真实dirty S1在2/3同步公开切面，只通过公开QTabBar移动已交付两页并真实点击选中，记录两个页的相对顺序和所选身份；剩下一页允许合法相邻位置，不能强制特定实现排列。后续每次current变化及finish都不得改选，原两页相对顺序不被回滚；正常关闭后的fresh manifest顺序等最终UI，允许合法密文重封。此项是行为/数据合同，不是原生鼠标拖动时延证明。

候选1普通三例已双 **3/3**（ON3.51秒、OFF4.02秒），但**未接受并打回**。独立复审发现Secure临时页仍主动选中：A普通active、B普通、C安全文件缺失时，C临时页删除后自动落到B，局部兼容中的C指针已失效，不会恢复A。真实ON新数据化旅程 `laterSecureRestoreKeepsOrdinaryCurrentDocument` 经原身份/KDBX保存生成A/B/C，再只把测试C密文移动到同临时目录备份；`3a-intent-secure-missing-baseline{,-lasttest}.log` **0/1 CTest、1成功/1失败业务行，13.82秒**。两行完整正文/密文备份/源hash/manifest三项责任/唯一身份/无意外认证全部先通过，missing行最后current断言失败，success行通过；2条既有timer警告保留。

拒绝候选源码与测试EXE已另存 `3a-intent-candidate1-rejected-coordinator.cpp` / `3a-intent-secure-missing-old-candidate.exe`。返工只将Secure临时标签创建也改为既有PreserveCurrentWorkspace：成功adopt保持原流程并由局部兼容保留之前当前，失败后台临时页从未抢选、删除就不触发邻选。不得改测试阈值、跳过缺失安全项或恢复全局旧preferred；Secure内部工作区问题仍单列未解决。

**根与独立代理接受3A-2a，仅普通恢复意图及Secure相邻不回退。** `3a-intent-candidate2-adjacent{,-lasttest}.log` ON **48/48，147.50秒**、OFF **46/46，132.91秒**，0失败/0跳过，两矩阵只差预期的2个Secure入口。普通active-first实际recordIndex2；恢复中点击确在completed0，重排currentSignals2/unexpected0，真实关闭后published完整守卫通过。ON安全成功/缺失两行均完整守卫后PASS；原加密草稿重启门保持。正式single-instance为17.42/17.82秒，Hex强杀恢复43.72/43.62秒；ON恢复中外部请求观察17/200→56/200→201唯一标签且外部文件选中。

ON82、OFF78条既有timer退出警告继续保留，另有断管及坏稿/拒绝路径诊断，不宣称生命周期问题已解决。架构门92个产品cpp，MainWindow5163行/613分支/112方法、Editor6915/858/176，预算不变。正式EXE与各自 `3a-intent-candidate2.exe` SHA：ON `e444608ad5222c2bce8cf88e39d62b50a190f33bca43d9ecca0701ba7f0287ad`、OFF `7be2911eead89b9182fa3b857495b07f1399f33b648a71afa0cc21df962012fb`；源码/测试/CMake归档 `3a-intent-candidate2-sources.zip` SHA `7acd3a4bb8fc98a4b92df7c8b623eeb6568038eb4505166839256866aede76c8`。普通行为验收不覆盖安全加载内部用户输入、Accounts工作区或8/16ms恢复性能；第3轮未关闭。

#### 3A-2b 恢复中显式进入Accounts（下一行为红灯）

复用上一片已由真实身份/KDBX/正常关闭生成的A普通active、B普通、C Secure三项会话；保留原成功/缺失两行不变，单列进入Accounts的第三行。重启后第一次1/3完整交付公开切面，触发可用的真实 `tools.accounts` 命令，确认账户工作区当前且可见、VaultDock真实打开及 `accounts.kdbx` 存在，再让后续恢复继续。使用正常记住身份场景，不手写capsule、不降低KDF、不预先切换工作区。

终态先验证A/B及C完整正文、唯一身份、源/密文hash和原三项manifest责任；随后要求用户选定的Accounts仍是当前工作区且账户库未因自动恢复而被关闭。中间记录工作区变化，不能只在最后切回Accounts掩盖抢占。该行只验恢复间隙的一次显式工作区选择，不冒称安全加载内部可随时输入、账户CRUD耐久性或性能通过。先在当前正式产品库跑红灯，根保存旧测试EXE后才设计最小presentation贯穿修复；不允许通过取消恢复C或取消离开Accounts时的凭据清理契约过门。

真实ON基线 `3a-accounts-baseline-contract{,-lasttest}.log` **0/1 CTest，19.87秒**：原success/missing两行通过；第三行在completed0/已交付1进入真实Accounts，全部A/B/C数据与三项责任守卫后，workspaceDepartures=1、accountsOpen=false，最终工作区是文档页而非Accounts。2条timer退出警告保留。旧测试EXE `3a-accounts-old-implementation.exe` SHA `fece1f02c4dec3cc735108b612de0a97e644ef5ac4f75e76e683107b2fb8852f`，测试源码SHA `dadf330cd7583ee6cd3c3d93a28986adfc7dfb0e1770df83ec538a80be47bb20`。

放行最小候选：复用现有OpenPresentationPolicy，通过openSecurePath、adoptSecureSnapshot及必要的ensureSecuritySessionInteractive/其masked恢复传递，默认仍FocusDocumentWorkspace以保持显式用户打开/新建等语义。只有自动恢复传PreserveCurrentWorkspace；此模式不切工作区、不改已有当前项、不抢编辑器焦点，但仍完整初始化、认证、解密及恢复视图。初始无当前项时沿QTabWidget自然选中首项。masked后台恢复也不得用旧索引强制改选；前台默认行为保持。移除Coordinator每次Secure后再恢复旧current的临时兼容，避免先抢后还。重复打开分支亦遵从presentation，不扩展为身份去重重构。账户离开时释放凭据、锁屏、会话租约、持久化与同步输入屏蔽均不改；若遇到超出presentation的需要先报根。原三行、普通意图、恢复保全、真实Accounts返回/重新进入/锁屏及显式安全打开相邻门须重新验收。

独立源码预审追加Text焦点验收：账户行在1/3处真实mouseClick `vaultFilterEdit`，确认QApplication当前焦点确为该输入框后输入前缀；后续记录focusChanged进入后台恢复文档子树的事件（临时null/正常安全进度窗不算后台文档），完整数据守卫后不重新点击/聚焦，对当刻focusWidget输入后缀并确认只追加到filter，A/B/C内容/revision/dirty/源hash仍正确。另加真实Secure Text分屏行，seed通过公开分屏并正常保存，恢复终态先验证分屏、共享正文/真实视图再验焦点。不因旧setSplitView的隐藏setFocus调用先改产品；Secure Binary旧底层焦点入口另列未覆盖，不由本Text子片冒领。

根在构建前视觉审实际Accounts截图，发现空库显示“安全保存你的第一个账号”介绍页，搜索框尚不可见。因此焦点夹具在1/3真实进入Accounts后，先点击可见 `accountsEmptyCreateButton`、填写纯fixture账号字段并点击真实Save；确认仍只有1条文档交付/尚未完成恢复，账户保存成功并出现正常列表/搜索框后才冻结账户hash和焦点观察。不能直接show隐藏搜索框或写造数据库；也不能把该测试准备差异定性产品回退。旧三行首次通过结果不由未运行的焦点扩展替代。

候选1扩展相邻矩阵 `3a-accounts-candidate1-adjacent{,-lasttest}.log` ON **52/54，179.14秒**、OFF **46/46，133.47秒**。原三行已经PASS（Accounts departure0/open=true），实际账户返回/重开/锁屏、外部拒绝、剪贴板及恢复保全/进程门通过；新增焦点/分屏证据尚待运行，不先接受本片。两个foreign安全打开旧测试仍从openFilesFromExternal期待解密框，违背第2轮已接受的外部请求DenyInteractiveUnlock合同。根在归档旧产品 `3a-accounts-old-implementation.exe` 各自独立进程复核，两个均相同守卫失败（1296/1908ms，actual exit1），日志 `3a-accounts-prechange-unifiedOpenForeign{Account,Document}WithRecovery.log`，不是本候选新增回退。

这两个旧测试仅修正交互入口：以真实file.open QAction及测试宿主内真实Qt QFileDialog选择fixture并点击Open，不直接调用私有路由或把外部请求改为Allow。若选择非native文件对话框以供Qt宿主驱动，只在该helper作用域设置/恢复本测试进程属性，不修改产品或全机设置；限时必须清理自有对话框。身份/恢复密钥、错误密码重试、记住身份后第二文件、账户CRUD/回收/字段/剪贴板及源数据守卫全部保留。凭据观察按真实对象到达而非固定80ms猜测，仍有有界看门狗。状态断言沿第2轮既有产品中文（不恢复旧Text工程词）；任何新失败保留原日志逐点检查。原外部拒绝解锁门继续全量运行。

`3a-accounts-candidate1-test2-focused{,-lasttest}.log` **2/3，35.64秒**：两个foreign明确Open例分别4.25/3.33秒通过全部原守卫；混合恢复中success/missing/普通Accounts均通过，**Secure分屏行真红灯，候选1打回**。它已经在completed0/单文档处真实创建账号并获得filter焦点；恢复终态完整正文、分屏共享文档、密文/源hash和三项责任守卫全部通过，workspaceDepartures=0/accountsOpen=true，但documentFocusTransfers=1，actual focus为隐藏 `secondaryScintillaEditor` 而非filter。没有向错误目标继续输入、没有重聚焦掩盖；此结果不能称为数据已经误写。旧测试EXE `3a-accounts-split-focus-old-candidate.exe` SHA `5244a637a035c063b56d2a33292b0e276e9bcccfe6309c658aef4d5cac5ddd80`，红灯源码归档 `3a-accounts-split-focus-red-sources.zip` SHA `8fa2f36e13e0d28912e1500d40267e833ca0cc1bad79117771fcaa514adfb010`。

最小返工只拆开“恢复分屏视图”和“主动把焦点交给新分屏”：EditorTab::setSplitView增加显式、默认仍聚焦的新分屏focus policy；DocumentSessionView的恢复调用传PreserveCurrentFocus，保留双editor创建、共享正文、布局与命令能力，不能在外层抢后还或取消恢复分屏。用户菜单的默认分屏仍聚焦新视图，关闭/方向调整路径不重写。Binary底层焦点仍单列未覆盖，不借本反例扩展全部editor焦点架构。另独立预审发现测试顶层QDialog归属不能用QWidget::isAncestorOf，helper及watcher改用既有QObject父链判定以确保8秒失败退场/意外认证不会漏报；仅测试修正，不放宽时间或正文守卫。

新增焦点判定曾被既有EditorTab.cpp分支208上限拒绝（实际209），没有调高预算或隐藏条件。根审同一分屏责任后允许去除析构中重复的m_l1Splitter外层守卫：原局部QSignalBlocker作用域不变，仍调用setSplitView(false)，其入口在无分屏时已立即返回，无事件泵/副作用。有分屏时关闭路径不变；只收敛重复判断以保持原架构预算。

candidate2 于2026-08-31完成动态验收。ON定向 **4/4，34.30秒**：主动分屏仍聚焦新视图；两个真实File Open入口通过；四行Secure恢复（成功、缺失、Accounts、Accounts+分屏）全部通过。分屏行在恢复1/3时经可见控件真实新建并保存fixture账号、点击搜索框、输入前缀；恢复后workspaceDepartures=0、documentFocusTransfers=0，实际焦点仍在搜索框，后缀进入搜索框而非隐藏编辑器，三个文档identity/revision/dirty/正文、两个普通源hash、Secure密文hash、Accounts KDBX hash和三项manifest责任均不变。根查看 `3a-accounts-focus-split.png`，搜索框有实际caret；长账号标题左侧裁切仍作为第9轮视觉风险保留，不冒充本片通过。

相邻矩阵绑定同一构建：KDBX ON **55/55**、OFF **47/47**，各0失败、0跳过；包含恢复保全、普通恢复意图、Secure相邻、正式单实例及Hex强杀恢复。OFF另行主动分屏1/1通过。ON/OFF正式EXE SHA分别为 `bf007e90c3eddcd414d606d97ec114ee8ab54146c43ed069d46916c58240cc98`、`c11067f3e44c9c80b9610de17fe8f529f47e3d3bc99733b1eb6a4df30a028f39`；源码/测试/CMake归档 `3a-accounts-candidate2-sources.zip` SHA `4c8bd48de2301f2aa06fe4e75551e38106ab7ee93847c9e1a145059c3b0e4030`。首轮误用不存在构建目标的失败日志保留。**根接受Accounts/分屏焦点子片；这仍不关闭3A的500ms首帧、输入呈现、混合恢复批次或Binary焦点门。**

2026-08-30 只读预审确认，现有“后台工作 + 同步等待”仍可能阻断真实输入：B1 patch 恢复在 `EditorTabLargeDocumentPersistence.cpp` 的映射等待使用 `ExcludeUserInputEvents`；`AsyncUiOperation.h` 的安全串行操作同时采用该等待模式和 ApplicationModal 进度框；文本恢复经过 `SessionManager::restoreAutosave` 的读取/解码及 `EditorTab::loadUtf8Chunked`，后者的分块事件处理也排除用户输入。这是源码确认的结构风险，不是已完成的 30 轮性能红灯，也不能仅靠再加一个零延时 timer 解决。

按以下顺序逐片验收，不在拿到基线前整体改写恢复系统：

1. **3A-1 可核查基线**：固定空 profile、外部文件、真实混合会话与注册压力四类场景。诊断事件仅记录固定阶段、单调数值、PID、恢复序号/数量、工厂数量和结果枚举；外部 harness 另测真实菜单输入至呈现。记录失败、原始样本与观测误差，不把发出输入/对象可见/paint 事件偷换为最终像素已呈现。诊断关闭时不得改变正常调度；打开时不得引入正文、路径或凭据日志。
2. **3A-2 恢复顺序与用户意图**：先用已存在的真实会话格式建立当前项位于队尾、用户在恢复中切换/新建/关闭、外部打开重复项的行为红灯。只调整既有恢复队列及标签占位/顺序责任；Manifest 顺序、DocumentId、未保存稿责任和单实例唯一写入者不变。自动动作不得覆盖更新的用户意图，关闭的项不得在迟到回调中重生。此片通过不等于 16 ms 批次门已过。
3. **3A-3 有界准备与提交**：依据基线定位超时阶段，将真正阻塞的读取/解码/映射准备与 UI 提交分开，复用既有任务/安全串行通道。未完成的文档不能暴露可编辑的半成品；其他标签和菜单仍应接收输入。关闭/锁定/新版本使旧完成结果失效，临时明文按原安全责任清理。不得把 QWidget、Scintilla 或用户密码对话框移入 worker，也不得简单去掉输入屏蔽而让原同步栈承受未设计的重入。

KDBX=ON 的 `MainWindow::newFile()` 会先建立或解锁安全会话，再创建加密草稿。因此空 profile 的 Ctrl+N 不是无需凭据的普通文本探针；菜单响应仍是 Shell 首个输入主门。新建及输入场景必须单列真实安全会话前提、密码等待和创建时间，不能为了计时预先解锁却称为空 profile，或临时改为明文新建。密码等待可以单列，但计算、真实 I/O、提交及等待期间的输入屏蔽不能从证据中消失。

上述三片仍受本节原定 P95、零重型首帧和 8/16 ms 批次合同约束；若需要改变安全交互或数据边界，先停止该片取得产品决定，不把范围扩张混入性能修复。

**恢复保全优先于增加并发交互。** 同次只读复审发现：当前 `saveSessionNow` 只捕获已经进入 `m_tabs` 的项；`SessionManager::writeAsyncSave` 在新 manifest 提交后删除 `toKeep` 之外的旧恢复稿。尚在恢复队列、未创建标签的项目未被该集合覆盖。`WorkspaceRecoveryCoordinator::finish` 又直接移除旧 manifest，而关闭流程未冻结恢复的 queued 回调。这些是已定位的静态风险，尚未完成动态反例；3A-2 必须先建立“恢复到一半自动保存/关闭/强杀/重启”和“恢复刚完成即强杀”的红灯。

最小合同是：尚未恢复、在途、已恢复但仍有未保存修改的每个项目，在任一成功发布的新 manifest 中都必须仍有明确责任；旧稿只能在替代版本可恢复且不再被待恢复项引用，或用户明确放弃该项后删除。不能只跳过自动保存来掩盖正常关闭和进程中断窗口。用户开始关闭时先冻结新的恢复调度，迟到完成不得新建标签或覆写关闭快照；关闭被取消后的继续恢复也必须保留队列身份。SessionManager 仍是 manifest 发布与恢复稿删除的唯一权威，不另建并行 journal。

本轮“含 Accounts”指恢复期间用户主动进入既有账户工作区，认证和安全通道不变，恢复回调不得抢回文档工作区；不新增账户工作区持久化格式。空 profile 自动 fallback 的两处 `newFile()` 会触发 ON 的身份初设对话框；基线必须如实记录此行为。若据此修复首帧输入门，最小方案是保留可操作空 Shell、正常通知会话恢复完成，仅在用户显式新建时沿原认证流程创建加密草稿，而不是悄悄生成明文标签。

2D 长测期间仅在构建输出目录准备两条未注册的测试草稿：公开状态栏“已恢复 1/3”的同步观察点执行正常 `close()`，以及 `sessionRestoreCompleted` 当刻用新 SessionManager 重读 manifest。夹具由真实编辑器和 SessionManager 创建 DPAPI `.ndraft`，不手写恢复格式。根与独立代理已复核切点，去除关闭后控件存活和禁止合法重新发布的实现假设；业务磁盘切面先冻结，清理不得事后重采而掩盖丢失。当前最小保存方案将未交付 `SessionTab` 原值并入计划，不预先解密/重写其正文，因此这两份未交付稿仍校验旧 hash；已交付稿允许合法重新封装并改用新引用。这不是永久禁止将来的显式格式迁移。草稿没有运行结果，也不代表自动保存、在途半成品、取消关闭或正式 EXE 强杀重启已覆盖。

## 5. 切片 3B：大文件时间、驻留与响应

### 设计

窗口化 Provider 只持有当前投影、overlap、受控预取和索引片段；不得创建完整 Scintilla 文档、全文解码副本或与逻辑文件大小线性增长的结果对象。搜索批量发布按帧合并，UI 不按匹配创建 QWidget。

### 验收

- 256 MiB、近 2 GiB、4 GiB+ Text/Hex：首屏 P95 < 500 ms；地址/正文/状态来自相同源 identity。
- 已缓存窗口跳转 P95 < 200 ms，NVMe 未缓存窗口目标 P95 < 500 ms；滚轮、键盘、拖动和搜索跳转的 UI 最大事件 < 50 ms。
- 30 轮分别记录 RSS/private bytes/commit；256 MiB 到 4 GiB+ 的峰值不得按文件大小近线性增长。阈值用基线 + 明确常数预算表达，不以机器剩余内存掩盖复制。
- Find All 面板 < 100 ms 出现；首批结果目标 < 500 ms；百万匹配滚动无 QWidget-per-row，UI 最大停顿 < 50 ms。
- 取消后的用户可见反馈与旧 generation 失效 P95 < 100 ms；读取计数/内存不再增长及worker资源静止目标 < 500 ms。不可中断的单次OS读取须单列原始跨度和有界上限，不能用它延迟界面反馈或接受迟到结果；关闭标签后watcher、timer、句柄和worker回到基线容差。

## 6. 切片 3C：Plain 与恢复存储故障矩阵

### 设计

Plain 保存保持 `snapshot → 同目录完整候选 → flush/commit → 原子替换 → 重新绑定`；direct-write fallback 关闭。Draft/session 使用加密、原子发布、可识别版本和显式用户处置。每个故障阶段具有稳定诊断码和用户下一步。

### 验收

- 候选创建拒绝、候选部分写/磁盘耗尽、flush 失败、commit/replace 失败、源被外部改变、源消失、只读和进程强杀。
- 对每点记录旧/新源 hash、候选 hash、dirty 内存、selection/caret、journal 密文、重启结果；至少一个旧或新完整版本可验证。
- 失败后“重试”重新执行真实阶段；“另存为”不覆盖旧源；“放弃恢复”只删除用户明确选择的恢复稿。
- 恢复树、日志、session 和临时目录不含正文 canary、原始 patch journal magic 或未登记明文副本。

## 7. 切片 3D：Secure、Accounts 与进程边界故障

### 设计

Secure Note 与 Accounts 继续通过各自 persistence adapter；故障注入位于 I/O、KDF/解锁、序列化和提交 adapter 边界，不进入编辑器或账号 UI。KDBX=OFF 只证明普通编辑器不依赖后端，不能替代 ON 的加密结果。

### 验收

- Secure：错误密码、取消、KDF/解密失败、密文损坏、候选写/commit 失败、锁屏/休眠、保存中关闭、进程强杀；旧库或新库至少一份可由产品重新打开并逐字段/逐字节验证。
- Accounts：新增/编辑/删除/回收站/TOTP 状态在每个提交故障点保持一致；列表、日志、恢复稿和剪贴板不含 password、seed 或恢复材料。
- DPAPI blob 损坏和密钥租约过期不会阻塞最小 Shell；只影响对应安全能力并提供可操作恢复路径。
- KDBX=ON 完整故障矩阵、KDBX=OFF 普通 Plain/B1/B2/B3 代表矩阵、ASAN/静态供应链与架构门共同通过。

## 8. 打回条件

出现任一项即退回当前切片：

- 只报告平均值、最佳一次、内部函数耗时或“任务已安排”，没有可见首屏/输入/完成真值。
- 用稀疏文件宣称密集 4 GiB I/O 性能，或不记录 allocated size。
- 为通过测试提高统一超时、放宽既有预算、预热未使用 Provider，或隐藏失败样本。
- 峰值内存、句柄、线程或后台 CPU 随文件/标签/扩展数量无界增长。
- 故障后源文件与未保存内容都不可验证，或恢复数据出现明文。
- 测试 double 替代正式 EXE、真实 Windows 文件语义和重新打开后的数据结果。

## 9. 退出记录

### 3A 保全红灯与最小修复合同（2026-08-30）

新注册 `gui_smoke_recovery_preservation` 的两项在真实 ON Release/Windows Qt 宿主运行，`3a-preservation-baseline-{ctest,lasttest}.log` **0/2**（每进程初始化/清理通过，业务各失败）。夹具由真实普通文件编辑、SessionManager 和 DPAPI 生成三份加密恢复稿；源 hash、已恢复完整正文、未保存属性、无明文残留及真实关闭提交 TaskId/终态全部先通过。1/3 公开状态切面正常关闭后，manifest 只剩一项、两份未交付稿丢失；全部恢复完成的公开信号当刻，新 reader 读到零项。这是动态反例，不是正式 EXE 强杀/重启或自动保存的完整覆盖。

第三条原生反例 `3a-inflight-baseline-{ctest,lasttest}.log` **0/1，3.81 秒**：8 MiB 真实加密稿仅投影 2,097,152 字节、完整 EOF 标记尚未出现时，公开加载进度发出的 queued 正常关闭被接受；`shutdownCommitted=1`，实际 SessionSnapshot TaskId=1 到达 Finished，但新清单只含一项，三份原完整稿责任均不可由 fresh reader 恢复。源 hash 和关闭前完整 DPAPI 稿守卫先通过。测试允许未来暂禁编辑或安全延期关闭，不强迫修复保留当前不安全的立即接受语义；仍不是 30 秒自动保存或正式进程强杀证据。旧实现的三例测试 EXE 已另存 `3a-preservation-old-implementation.exe`，之后才放行生产修复。

提前开发只允许下列保全修复，不重写性能调度、不引入第二套 journal 或持久化格式：

- Coordinator 保有本次恢复的原记录与 Pending/InFlight/Live/明确移除责任；在暂存标签进入公开 tab 集合前认领 InFlight，保存时排除半成品，用原记录保留。完全恢复及视图提交之后才转 Live，发出 1/3 进度之前完成该转移。
- SessionManager 的同一保存计划接收有序的真实 port 或原值 SessionTab，原值项不解密/重写、不回写实例 revision；manifest/删除仍由它统一负责，activeIndex 按最终集合映射。正常未恢复时保持原捕获路径。
- 成功发布前不删除不同路径的旧稿，发布失败不 apply 新引用；保留同路径原子重封和原节流，不扩大为全新快照格式。恢复完成不直接删除有效 manifest。
- 窗口关闭冻结新恢复回调；取消后准确续跑，接受后不再新建标签/改写关闭清单。若关闭发生在同步恢复栈内，先保留原责任、延迟到安全退栈再关闭，不能删除仍被原生/解码栈使用的对象。明确移除与保存顺序不得使旧计划事后复活已移除项。
- 最小通过需两条现有红灯、真实部分正文恢复反例、有序原值保留单测与旧恢复相邻门。用户取消、失败页重试/放弃、真实 manifest 发布失败、ON/OFF 等按逐片补证。`manifestPublished` 不等于所有最新正文写入成功；强制保存失败仍关闭的更完整耐久性问题保留 3C 硬门，不由本片冒领。

主代理实现预审另要求保留既有重排语义：初始恢复全部交付后，下一次保存必须遵循当前 UI 标签顺序和活动项，不能因保存原队列元数据而回退到启动顺序；完成后的 Retry 在原失败页槽位保留原值、排除暂存编辑器。交付游标必须在后续焦点/状态信号之前推进，避免该信号中关闭再取消后重复恢复同一项。已据此补独立顺序回归，尚待运行结果。

独立复审追加两条保全边界，先注册真实红灯再改实现：

- **首次恢复开始前关闭**：真实 StartupCoordinator 的首帧后空闲信号处正常关闭，尚无正文交付；三份旧加密稿、源 hash 与可恢复引用必须保留。`captureEntries` 首次需要时可从现有 SessionManager 惰性接管原记录元数据，`start` 复用同一份记录，不提前读取正文、实例化 Provider 或创建认证流程，也不把外部新打开标签重复并入待恢复队列。这仍不代替 CLI 长时读取中的自动保存/输入延迟完整矩阵。
- **重试复用已有文档时关闭再取消**：原文件待处理页重试复用实际已打开的同一 Editor；移除当前待处理页发出公开 currentChanged 时正常关闭，另一个真实 dirty 文档触发 Save/Discard/Cancel 并点击 Cancel。成功项必须仍可达、没有丢失责任或隐藏重复；最终正常关闭后每个文档只持久化一次。完整成功责任交接必须早于可重入的旧页移除，不能回滚到已移除且 deleteLater 的页。不得为此引入新的 journal 或屏蔽真实窗口信号。

首轮候选的五条窗口旅程和真实 SessionManager 故障单测：ON **6/6，10.68 秒**，OFF **6/6，11.70 秒**，见各配置 `3a-preservation-candidate1-{ctest,lasttest}.log`；单测内部六数据行均通过、无跳过。首次编译因测试 mixed-const 指针 initializer_list 无法推导失败，改为显式 `std::array<const DiskCut *, 2>` 后双构建通过，失败日志保留，没有改业务断言。

新增边界首轮 `3a-boundary-baseline-{ctest,lasttest}.log` **0/2，2.47 秒**：首帧后关闭已确认真红灯，paint=1、正文标签=0、恢复完成=0、正常关闭被接受、实际 Snapshot #1 Finished，而 fresh manifest=0，三份完整旧稿责任丢失。Retry 例完成真 Cancel 和文档/正文/hash 守卫，但随后在测试寻找 Undo 动作处失败，尚未达到最终 manifest 检查；不能将该测试准备错误称为重复责任已动态证实，先修测试再重跑。上述结果仅对应本轮已链接候选，不代表阶段完成。

Retry 测试准备已按现有 vendor 实现校准：`QsciScintilla::append` 会明确清空 Undo 栈，原 action 名称并无错误；改用公开 `insertAt` 并立即要求原生 Undo 与产品命令能力可用，仍执行实际 `edit.undo`，不放宽断言。`3a-retry-reuse-baseline2-{ctest,lasttest}.log` **0/1，0.65 秒** 确认业务红灯：真 Cancel、同文档身份/revision/正文/hash 守卫及 Undo 后正常关闭均通过，fresh manifest 却 total=3、原文件=2、另一文件=1。原待处理页消失后残留的隐藏原记录造成重复责任。两条边界的旧候选测试 EXE 分别另存 `3a-prestart-old-candidate.exe`、`3a-retry-reuse-old-candidate.exe`，之后才放行 Coordinator 两点最小返工。

最小返工候选 2：ON **34/34，40.81 秒**，OFF **34/34，44.52 秒**（各 29 GUI + 5 其他），见 `3a-preservation-candidate2-{ctest,lasttest}.log`。首帧后关闭现保留 3 条完整稿引用；Retry/Cancel 最终清单恰为 2 个文档各 1 条。独立审阅接受当前保全子片的代码/业务结果，不代表全部 3A；两份完整日志各仍有 62 条既有 Qt timer 退出警告。架构 MainWindow 5162 行/613 分支/112 方法、EditorTab 6915/858/176，异步边界通过，未放宽预算。

根补验正式进程边界：ON `3a-preservation-process-{ctest,lasttest}.log` **2/3，70.56 秒**；单实例单测 32 业务通过，正式 Hex 强杀恢复通过（43.93 秒），单实例进程组却在 CleanRestoreSame 的旧等待条件失败。该脚本把 `session.json` 被删除当成恢复完成，本片已明确禁止以删除恢复入口表示成功，因此需改为真实公开完成状态观察，保持 10 秒预算、source hash、单标签和物理别名守卫，不恢复错误产品语义；修正并重跑前不计正式进程组通过。

正式进程观察返工完成：Clean Same/Alias 从自有 PID/启动身份/HWND 读取精确公开恢复完成状态、唯一实际 TabItem 与选中项；200 项恢复场景先观察精确 n/200 公开状态，B 正常 ACK 退出后再核对仍在恢复，最后要求完整 201 个唯一标签及外部文件选中，再核对正常关闭后的全部持久化路径。没有通过 ACK 单独推断交付，也没有恢复 manifest 删除假设。途中 .NET SelectionPattern API 名误用、WinPS5 四参数 `String.Concat` 动态绑定异常均保留独立日志；前者经本机反射校准，后者由完整异常栈和纯表达式对照确认，以 char[] 拼接修复并通过 12 个精确匹配正负例，不改产品或超时。

最终 `3a-single-process-ui2-{ctest,lasttest}.log`：ON **1/1，18.84 秒**，OFF **1/1，18.80 秒**，覆盖 CleanSame/Alias、ABC、协议拒绝、启动竞争、持锁进程强杀接替、恢复中 ACK/最终交付、无响应拒绝、缺失路径和强杀重开。ON 实际公开恢复状态 8/200 → B 退出后 25/200 → 201 标签及外部选中；无响应仍按预算 exit 5。OFF 独立 `3a-preservation-process-*` **2/2，60.29 秒**，含单实例单测及正式 Hex 强杀恢复（44.87 秒）。两种配置所有本片相邻门现已通过。

**保全子片接受，整个 3A/第 3 轮仍未关闭。** 当前生产源码及正式 EXE 已独立冻结：ON `5e1addfc2af4cda5e0b9aba3be3fc6487d83ad853729f9d4080df87bda3c8f8a`，OFF `0b59afa79432a5f484fb818329e7b166bd044ded716dd6089b148ccf5f6d2446`。旧失败、编译、GUI、正式进程证据均保留；后续先测量有效性与 30 轮启动基线，再按已登记顺序推进完整性能/故障门。正式 Hex 强杀覆盖不能上推为本次每个恢复切面均做过正式进程强杀。

### 从 2C 转入的明确性能回归

真实 33 MiB 普通只读文本、60,050 个大写密集命中、默认不区分大小写时，`2c-spool-real-failure-first.txt` 和 `2c-spool-short-lines.txt` 均超过 12 秒；短行 `X\n` 版本 QtTest 诊断约 17.95 秒才达到领域终态。显式勾选区分大小写、不改产品或容量/超时后，同一短行 spool 失败测试在 2.773 秒到达任务状态断言。该对照用于定位，不是 30 轮性能验收。

源码原因候选已具体定位：`makeByteCandidateFinder` 对每个候选分别 `memchr` 两种大小写，其中一种不存在时反复扫描同一剩余 16 MiB 范围，密集单一大小写输入产生巨量重复读取。3B 须先保存可复现的大小写/密度对照，再使候选枚举对已扫描区间单调推进，保留取消粒度、编码边界和精确结果；不能通过改默认选项或增加超时隐去。UI 预览已有 5,000 项上限，此处不能未经证据归因于每匹配创建控件或无限预览。

### 从 2C 复审转入的既有排队统计风险

`WindowedReadOnlyProvider` 行号统计的 pending 状态原先仅在业务 worker 内清理；普通池满时从任务面板取消 Queued 任务，worker 可能被跳过。该结构已存在于 HEAD，不是本次纯分析函数提取新引入；尚无动态红灯。3B 须用真实线程池占用、精确统计 TaskId、实际取消和重新请求统计验证 pending 是否结算，不把该静态风险写成已复现故障或扩大当前 2C 范围。

第 3 轮尚未关闭。3A 恢复保全按上面的明确例外提前执行；其余在第 2 轮关闭后按 3A → 3B → 3C → 3D 完成红灯、最小实现、主代理独立验收和返工，全部通过才能关闭第 3 轮。

### 从 2C 最终出口保留的 Windows 并发与退出诊断

- 使用 `MoveFileExW` 的外部程序在本产品短期统计 reader 存活期间可能收到 Windows 拒绝访问；`FILE_SHARE_DELETE` 本身不保证该原语成功。2C 的空闲无永久 mmap 验收先等待真实统计 reader 退出，不能上推为“任意时刻所有外部保存均成功”。3B/3C 的长时资源和外部编辑兼容性必须包含真实统计/搜索并发，区分临时共享冲突、长期句柄残留和可用的不同原子替换原语，不能以固定重试掩盖长期占用。
- 2C OFF 首轮完整日志包含 168 条 Qt `Timers cannot be stopped from another thread` 退出警告。业务 CTest 的成功不证明退出无告警；3B 关闭/进程退出资源验收须定位实际 timer owner/析构线程并记录，不把当前日志写成无 Qt 生命周期诊断。尚未确认其根因或用户影响。

2026-08-30 正常交互宿主的 2D ON 120/120（385.73 秒）另保留 210 条同类警告：105 个测试进程各两条，多在 QtTest Finished/PASS 后出现，不能解释成 210 个独立 timer 泄漏。源码与本机 QtTest 头的只读对照将 `AsyncTaskManager::statsTimer_` 列为优先候选：Manager 为函数静态对象，测试中的栈 QApplication 先析构；`QTEST_MAIN` 返回 `QTest::qExec` 而不经过正式 `app.exec`，既有 `aboutToQuit` 停表可能未执行。尚未取得 timer 地址/调用栈对应，不能正式归因，更不能未经证据称为业务 worker 跨线程删除。3B 先用最小 Core 搜索宿主核对实际 timer、Qt 线程身份、原生 TID 和 Application 析构顺序，再与正式 EXE 正常退出对照；不得通过屏蔽警告或测试私下停表宣称修复。

### 2026-08-31 加速收口结果与停止点

本次按“先加速收口性能，然后停止”执行，没有开始 AI 或 Windows 分发工作。根代理接受以下最小产品修复：

- 大文件不区分大小写搜索改为每个 16 MiB chunk 单调扫描并同时接受 ASCII 两种大小写，不再为不存在的另一大小写反复扫描剩余区间。真实 33 MiB、60,050 密集命中及真实 spool 创建失败旅程当前 **3.74 秒**通过，继续使用原 12 秒门，没有改默认选项或提高超时。
- `WindowedReadOnlyProvider` 的行号统计以精确 TaskId+generation 结算；Queued 状态取消即清 pending/连接，同一 generation 可重新请求。最终真实 GUI 例 **0.67 秒**通过。
- 强制 session 提交失败不再接受窗口关闭或发出 `shutdownCommitted`；恢复写入成功后可重试关闭。最终真实 GUI 例 **0.56 秒**通过。
- 三个修复的相邻真实 GUI 组此前 **32/32，106.09 秒**通过；最终重建后的定向组 **3/3，4.98 秒**通过。架构门为 MainWindow **5183/616/112**、EditorTab **6917/858/176**，93 个唯一产品源码条目；异步线程边界通过。证据为 `build-round3-close-nmake/round3-three-fixes-adjacent.log`、`round3-performance-final-focused.log`。

正式 KDBX=ON Release 的低干扰空profile组绑定同一候选 hash `ad2596ba7c1d897d04e63095ec326110051e7b640a29c4015bbc3b4507f979c1`，30/30 有效、正常退出、单一 hash。应用就绪到首 paint 为 P50 **122.631 ms**、P95 **693.458 ms**、max **718.221 ms**，其中 7/30 ≥500 ms；`show()` 自身 P95 **639.520 ms**，而组合到窗口 P95 **48.402 ms**、show 返回到 paint P95 **5.491 ms**。因此 500 ms 门继续为真红灯，不能用构建、P50 或旧测试宿主数据替代。

两项产品候选均被打回并已从源码撤销：窗口 PNG 改多尺寸 ICO 的 10 轮仍有 3 个 615–691 ms 慢样本；移除 EXE 资源图标的 10 轮仍有 645 ms 慢样本。观察器延后 1000 ms 的对照仍有 616 ms 慢样本，排除持续窗口枚举是主因。临时正式细分测点版本关闭时访问冲突，判为观察器无效并完整撤销，不用于产品结论。最终恢复后的正式 EXE hash `db47be37afd691f7da5b2af0883891b9f20ca4e3cd66752a5e3e8d5ccc17e4cd` 已完成一次有效首屏轨迹、正常 exit0；该单次 234.756 ms 只证明恢复后的程序正常，不覆盖 30 轮红灯。

现有双配置 v3 共 120 个样本中，包住真实 `MainWindow::showEvent`、基类事件、`winId()` 和 WTS 注册的区间 P95 均低于 0.9 ms、最大 1.096 ms；资源诊断的三个慢 show 仅消耗约 13.30%–16.96% 同期墙钟的目标进程 CPU。当前证据将剩余首显波动限定在 Qt/Windows 顶层窗口同步路径，但不足以指认具体系统调用。为了达到数字而延迟/关闭 WTS 会产生锁屏通知安全空窗，降低 500 ms 门、关闭系统防护或改 DWM/网络策略同样不被接受。

**性能工作在此停止，状态为“代码热点已修复、首屏门仍阻塞”，不是第 3 轮通过。** 尚缺的 256 MiB/近 2 GiB/4 GiB+ 完整资源矩阵、混合 20 标签事件轮、100 ms 应用侧输入呈现，以及 3C/3D Plain/Secure/Accounts 完整故障矩阵保持开放；不得据本节声称可发布，也不得自动进入 AI。
