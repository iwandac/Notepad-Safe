# Notepad Safe 当前功能总账

> 基线：`9a80eec`（功能修复基线），架构迁移检查点从该 revision 向前比较。
> 规则：运行时所有有 objectName 的 QAction 必须由 `FeatureRegistry` 绑定本表中的稳定 FeatureId；未知命令使启动合同测试失败。删除、降级或改变下列能力必须先获得产品裁决。

| FeatureId | 当前用户能力（必须保留） | 主要命令/入口 | 回归证据族 |
|---|---|---|---|
| NSP-SHELL | 最小 Shell、工作区切换、会话渐进恢复、设置恢复、单实例外部打开；辅助工作区中的普通外部打开保持当前工作区 | `menu.*`、WorkspaceStack、StartupCoordinator | startup composition、startup E2E、session、settings、single_instance unit/process、external-open GUI |
| NSP-FILE | 新建默认安全草稿；统一打开 Text/Binary/Secure/Accounts KDBX；保存保持格式；另存为显式转换；关闭/退出提示 | `file.*`、`tabContext.*`、拖放、CLI、最近文件 | smallfixes、secure documents、vault、testfiles、session |
| NSP-EDIT | Undo/Redo、Cut/Copy/Paste、Select All、行复制/删除/移动/大小写/排序、缩进、补全、Hex 插入/覆盖 | `edit.*`、工具栏、快捷键、上下文 | coord editops、smallfixes、large readonly |
| NSP-SEARCH | 当前/全部标签查找与替换、上一处/下一处、单/多文件结果、byte 精确导航、书签 | `search.*`、`bookmark.*`、SearchResultsDock | find/replace、Dock compact quick sentinel、large-file encoded search |
| NSP-LANGUAGE | Plain 及当前语言语法呈现、扩展名猜测；保留 clean/恢复稿重试的手选语言 | `language.*` | smallfixes/menu UI、session、recovery issue GUI |
| NSP-VIEW | 系统/亮/暗/高对比、换行、分屏、符号、折叠、Text/Hex、标尺、缩放、面板/工具栏/状态栏 | `view.*`、状态栏 | smallfixes、coord、large readonly |
| NSP-LARGEFILE | 大文件窗口化只读浏览、Text/Hex 源字节坐标、外部变化检测；Tail 候选的验收边界见下 | Provider、全局滚动条、`view.tail.toggle`、`view.tail.resume` | ordinary read source、large readonly、tail contracts/follow/presentation、正式进程资源采样 |
| NSP-ENCODING | 编码检测/强制显示、UTF/代码页转换、BOM、EOL 转换并经另存为保护；会话恢复保留受支持显示编码且不改变 Binary 身份 | `encoding.*` | textencoding、encoded search、testfiles、smallfixes、recovery issue GUI |
| NSP-ACCOUNTS | Accounts 一级工作区、KDBX 账号 CRUD/TOTP/历史/回收站、Security Center、锁定、主密码与恢复密钥 | `tools.accounts`、`tools.securityCenter`、`tools.lockNow` | vault contract、KDBX backend、security identity、vault/secure GUI |
| NSP-TASKS | 状态栏任务、任务面板、准确进度、按实际能力取消和安全串行队列；选中任务实时更新，诊断默认折叠且更新时保持阅读位置 | `tools.tasks`、状态栏任务控件 | async architecture gate、task product language/real errors、搜索/安全 GUI journeys |
| NSP-HELP | About、版本和依赖信息 | `help.*` | smallfixes/menu UI、release package |

## 不回退判定

本次执行顺序见[成熟度路线图](ITERATION_ROADMAP.md)：Windows 安装发布排除，性能轮之后直接 AI-D0。下列运行质量要求继续有效；发布包工具的历史存在不代表本次需要执行或已经通过。

- “入口存在”不等于功能保留：文件内容/hash、KDBX 可重开、session 字段、undo、可见状态和耗时均需对应证据。
- GUI harness 必须链接与正式 EXE 相同产品 target；测试不能重复编译产品 `.cpp`。
- 历史回归哨兵固定进入 quick：`quick.search_dock_compact` 与 `quick.readonly_repeated_copy`。
- `testfiles` 八文件矩阵是本机验收 fixture，不清理、不提交；覆盖不同大小、Text/Binary 和 GBK/UTF-8/BOM/UTF-16LE/BE。
- KDBX OFF 只验证 editor/search/tools 不依赖具体后端；安全行为必须由 KDBX ON 验收。只在 ON 编译的 case 必须由同一个 CMake capability 条件排除，禁止注册成“未知测试函数”制造伪失败或诱导跳过整个 OFF 门禁。
- 外部 CLI/拖放的安全文件请求不自动解锁，只提示在窗口内显式打开；窗口内 File Open/最近文件保留安全打开能力。该入口授权差异不属于格式路由不一致。
- 拖放保留缺失/目录等本地打开意图；有效文件不被失败项阻断，最近一批失败可从状态栏用键盘复查。外部拒绝解锁授权贯穿最终路由，不由当前工作区推导；已打开的 dirty 文档即使磁盘被独占也可复用原标签。2B2 独立 ON 22/22、OFF 21/21 证据见第 2 轮验收契约。
- clean 会话恢复按物理文件身份复用显式打开的标签；未保存恢复稿独立保留，不能用去重丢弃。原文件重新打开失败与未保存稿恢复失败分别提供重试/移除及重试/确认放弃；移除待打开项不删除源文件。旧普通记录不得通过 clean 或 dirty 恢复绕过路径当前的安全类型。2B1 定向独立 ON 24/24、OFF 22/22 证据见第 2 轮验收契约。
- 任务、恢复与查找的默认提示使用用户语言，已知原因由生产分支提供，未知错误不猜原因；原始诊断可显式展开和复制。文件大小保持各自既有权威，字符数未知且未在统计时不假报“统计中”；恢复长说明可在窄页滚动，操作语义不变。2B3 干净 ON 78/78、OFF 76/76、原生亮暗与窄页视觉证据见第 2 轮验收契约；不包含尚未闭合的搜索生命周期/Tail。
- runner 的任何 label/regex 选择必须带 `--no-tests=error`；零测试不是通过。GUI case 的 `smoke/tier/FeatureId` labels 由注册 helper 一次生成，不能在批处理、CMake 和文档中维护三份漂移清单。

## 第 2 轮候选与已验收能力的区分

- 搜索生命周期、结果容量与部分结果、外部身份变化、同句柄普通读取和安全类型拒绝已由 2C 的双配置 97 项出口闭合；以上 2B 历史证据仍只代表各自切片，不覆盖这些后续行为。
- Tail 默认关闭，仅用于普通只读 Text/Hex；浏览/选择/导航暂停，显式回到末尾才恢复，源截断/替换/删除/不可读/安全类型变化停止，Accounts 和安全文档禁用。冻结版本完整定向矩阵 ON 120/120、OFF 119/119、最终图像 2/2，以及 30 分钟追加/空闲各自完整回收阶段均通过独立复核，第 2 轮按本轮合同关闭。各次失败与第 3 轮性能门仍保留；后续源树不冒用冻结版本证据。
- 3A 恢复保全子片已修复并接受：恢复启动前、1/3 交付时、部分正文投影中、完成当刻均保有完整稿责任，关闭取消准确续跑，Retry 复用不会残留隐藏重复记录，完成后保留用户标签顺序。当前候选 ON/OFF 各34项相邻门通过，正式单实例矩阵及 Hex 强杀恢复也已独立补验；证据见第3轮契约，不冒用冻结 Tail 版本结果。完整故障存储、启动输入与性能矩阵仍未关闭，用户实际文件未参与测试。
- 3A 空Shell子片已接受：全新配置启动不自动建立安全身份、不隐式新建明文；显示真实开始提示且无假文档状态，显式新建继续原安全流程。双配置各5条真实旅程、正式EXE窗口视觉及相邻恢复门已验收，旧secure恢复清单删除测试修正后单独1/1通过。该结果不代表100ms菜单响应或30轮性能门已过；组合根旧慢样本保留，正在建立正式进程采样。
- 3A 菜单可读反馈已接受：仅关闭本应用Qt菜单渐变/展开过渡，系统设置和其他效果不变。双配置各9条相邻门与双正式四帧对照通过；随后正式各30轮、全部60首张菜单图均清晰。内部首paint P95为624.216/657.152ms，500ms门未达；100ms输入呈现仍无通过证据。该组绑定菜单候选身份，不冒用为后续恢复候选的性能结果。
- 3A 普通恢复意图已接受：上次当前项优先，原标签次序保留；恢复途中用户点击、重排及关闭持久化不被finish回滚。Secure临时页缺失引起的邻页抢选在真实红灯后返工闭合。当前冻结候选ON48/48、OFF46/46及正式单实例/Hex强杀通过独立验收；不冒用为完整Secure内部意图、Accounts或性能证据。先前两条红灯与首个被拒候选均保留于第3轮记录。

## 持续运行质量风险（不随 Windows 分发范围取消）

以下事项由根代理在[第 9 轮组合验收](ROUND9_AI_RELEASE_ACCEPTANCE.md)跟踪与复验，不表示已修复：暗色非选中标签对比度；Windows UIA 实际动态 Name/Value 与键盘可达性；Accounts 长标题的裁切/横向滚动与窄窗口可读性。全应用结论不能由单个 Tail 或恢复截图代替。数据 schema 识别、未知/损坏版本拒绝和原件保全继续由第 3/9 轮验证；取消安装升级不取消数据安全。

真实 AI/代码解析仍是设计基线，尚未实现。第 5–8 轮的公开能力必须有真实确定性工具、获批 parser 或真实获批模型证据，不能把接口、fake Provider 或完成设计当作功能已交付。
