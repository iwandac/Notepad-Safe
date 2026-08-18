# Notepad Safe 参考基线与探索边界

> 状态：目标模式的外部参考基线
> 版本：1.0
> 资料核对日期：2026-08-17
> 配套文档：`DESIGN.md`、`TARGET_EXPERIENCE.md`、`ITERATION_ROADMAP.md`

## 0. 为什么需要这份文档

本项目不再用“功能看起来像能工作”作为探索方向，也不以复制某一个现有产品为目标。本文件把成熟产品和上游组件的行为转成有限、可验收的产品契约：

- **采用**：与“离线默认安全、使用时像普通记事本”直接相关的成熟行为。
- **适配**：保留用户预期，但按安全文档、Hex 和大文件分级改造。
- **拒绝**：会把项目扩大为 IDE、云服务、自研编辑器内核或第二套密码学的能力。
- **验证**：每个采用项最终必须映射到正式 KDBX=ON Release 的动态 GUI、文件结果或安全边界测试。

资料更新不能静默扩大范围。供应商升级只允许修复缺陷、兼容新平台或替换已采用能力；新增上游功能必须先修改本文件和架构决策。

## 1. 产品参考：采用什么，不采用什么

### 1.1 Notepad++：文本编辑和搜索结果的基准

来源：[Notepad++ User Interface](https://npp-user-manual.org/docs/user-interface/)

采用：

- 文档标签、编辑区、状态栏、菜单和可停靠结果面板组成清楚的主工作区。
- 状态栏在不打扰编辑的前提下显示文档类型、字节/行、行列位置、换行符、编码和插入/覆盖状态。
- 文档的垂直滚动只有一个位置真值；滚轮、滚动条、Page Up/Down 和跳转必须到达同一内容位置。
- “查找全部”以文件分组展示上下文；结果双击激活正确标签并把匹配滚入视口、选中并高亮。
- 当前文件是默认搜索范围；全部打开文件和目录是显式扩展范围。
- 自动换行、横向滚动、尾部监视等能力必须由文档状态决定，不能由大文件外壳制造第二套互相冲突的滚动行为。

适配：

- S3 结果可以来自未装入 Scintilla 的 mmap 数据，但结果导航仍必须表现为同一个编辑器。
- 全部替换只作用于可编辑目标；S3、只读、锁定或保存冲突目标必须逐项报告跳过原因。

不采用：插件生态、宏兼容、每一个语言菜单、会扩大维护面的历史选项。

### 1.2 Windows Notepad：低认知负担的基准

采用：

- 打开、保存、另存为、新建、查找等基础命令始终位于用户预期位置。
- 打开文件不先询问“文本/二进制/加密”；应用根据内容路由，只在误判或显式重新解释时让用户选择。
- 常规编辑过程不显示后台实现术语，不因加密增加重复确认。

不采用：只支持小型文本、单一窗口或缺少高级搜索的能力边界。

### 1.3 VS Code：恢复和搜索信息架构的参考，而非产品范围

来源：[Basic Editing](https://code.visualstudio.com/docs/editing/codebasics)、[User Interface](https://code.visualstudio.com/docs/editing/userinterface)、[Accessibility](https://code.visualstudio.com/docs/configure/accessibility/accessibility)

采用：

- 标签脏状态、会话恢复、最近位置和工作区布局恢复必须可预测。
- 搜索结果按文件分组、显示上下文和数量；查询历史与取消状态明确。
- 面板可隐藏、恢复和键盘聚焦；状态栏只显示可操作或可诊断的信息。
- 缩放、高对比度、完整键盘路径和屏幕阅读器语义进入发布门槛。

拒绝：项目系统、终端、调试器、语言服务器、Git、远程开发和插件市场。Notepad Safe 不是 IDE。

## 2. 编辑内核和大文件边界

### 2.1 Scintilla/QScintilla

来源：[Scintilla Documentation](https://www.scintilla.org/ScintillaDoc.html)、[QScintilla Downloads](https://riverbankcomputing.com/software/qscintilla/download)

采用：

- 只有 S1 使用 Scintilla 的完整可编辑文档、选择、撤销和编辑行为。
- S2/S3 统一使用窗口化 mmap 只读 Provider；不创建 `SCI_CREATELOADER`/`ILoader` 后台编辑文档，不显示资源准入、准备编辑或晋升状态。
- 横向滚动使用 Scintilla 的滚动宽度与跟踪能力。`SCI_SETSCROLLWIDTHTRACKING` 只保证已显示行可完整横向浏览，因此窗口化 Provider 还必须测试窗口边界和超长逻辑行。
- 标记搜索结果使用 Scintilla indicator/selection 语义；导航后必须验证实际可见文本和选区。
- QScintilla 是 Qt 适配层，Scintilla 才是编辑内核；上游更新按 `SUPPLY_CHAIN.md` 验证，禁止为了一个局部问题长期分叉存储内核。

拒绝：

- 修改 Scintilla 的底层存储，让它直接在 2 GiB+ mmap 上任意编辑。
- 恢复通用 PatchList 编辑器路线。
- 让窗口化外层滚动条与 Scintilla 垂直滚动条同时可见或各自维护位置。

### 2.2 三档文件能力的最终边界

| 档位 | 目标 | 可承诺能力 | 明确不承诺 |
|---|---|---|---|
| S1 `<32 MiB` | 完整编辑 | Scintilla 全功能、Text/Hex、搜索替换、恢复 | 无 |
| S2 `32 MiB–2 GiB` | 快速只读分析 | mmap 首屏；可连续浏览全文；搜索、结果跳转和有界复制；能力与 S3 相同 | 编辑、替换、完整装入 Scintilla、后台编辑晋升 |
| S3 `>2 GiB` | 日志/二进制分析 | 单滚动位置、连续浏览、跳转、字面/正则查找、Find All、复制选择 | 编辑、替换、格式化、整文件装入 Scintilla |

“像访问内存一样浏览”在产品上定义为：打开时间不随全文解码线性阻塞；拖动到任意比例能看到对应源字节；滚轮/按键跨窗口连续；跳转不回弹；后台搜索不阻塞 UI。它不意味着把完整文件伪装成一个可编辑 Scintilla 文档。

S2/S3 不执行编辑资源准入。大小档位只用于性能策略、状态说明和将来重新评估，不能产生不同的修改能力。

## 3. Hex 体验基线

来源：[Microsoft Hex Editor](https://marketplace.visualstudio.com/items?itemName=ms-vscode.hexeditor)、[KDE Okteta Basics](https://docs.kde.org/trunk_kf5/en/okteta/okteta/usage-basics.html)、[Okteta Handbook](https://docs.kde.org/stable5/en/okteta/okteta/okteta.pdf)

采用：

- 一行由 offset、十六进制字节和字符解释三栏组成，Hex/ASCII 光标指向同一源字节。
- 常用默认值是 16 bytes/row，但布局可以按可用宽度在完整分组上调整，避免右侧大片无意义空白。
- B1 支持 nibble 输入、ASCII 输入、选择、撤销/重做、复制/粘贴、查找/替换、插入与严格覆盖模式。
- B2/B3 在写入事务和恢复达到门槛前保持只读，但导航、选择、复制和搜索不能退化。
- 字节查询支持 Hex 和文本表达；显示与解析必须清楚区分，不允许悄悄丢弃非法 nibble。

不采用：通用二进制结构模板语言、反汇编器或磁盘扇区编辑器。

## 4. 安全文档和账户边界

### 4.1 KDBX/KeePassXC

来源：[KeePass KDBX format](https://keepass.info/help/kb/kdbx.html)、[KDBX 4](https://keepass.info/help/kb/kdbx_4.html)、[KeePassXC User Guide](https://keepassxc.org/docs/KeePassXC_UserGuide)

采用：

- 文件头签名决定 KDBX 路由；认证加密失败视为损坏或密钥错误，不能降级成文本。
- KDBX4、Argon2id 和可扩展 KDF 参数由供应商实现；产品不自创正文加密算法。
- 新建安全 Note、账户数据库、历史密码文件和其他设备文件共享统一 KeyResolver，但保持各自密码域。
- 账户工作区采用列表/分组/详情、搜索、生成器、TOTP、历史和回收站等成熟核心行为，不以“保险箱插件”拼在编辑器旁边。
- 敏感字段默认遮挡；复制、显示、自动清除和锁定是短时动作。

产品限制：安全 Note 的解密 payload 严格小于 32 MiB，解锁后复用 S1 Text/Hex Provider。普通大文件能力不得迫使 KDBX 解密明文长期驻留或引入另一种可移植加密格式。

### 4.2 Windows 无感会话与恢复

来源：[CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)、[BitLocker recovery overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/data-protection/bitlocker/recovery-overview)

采用：

- 七天设备会话用 DPAPI CurrentUser 密封 KeyRing；它提供同一 Windows 用户/通常同一设备上的无感解封，不是用户文件的唯一密钥来源。
- 每个安全文件携带能用主密码或独立恢复密钥重新得到内容密钥的版本化槽。AppData、会话或整台设备损坏不能成为永久丢失文件的原因。
- 当前会话先尝试解锁；失败才进入外来/历史密码流程。成功后只在适当会话范围内复用，不静默重写文件密码域。
- 恢复密钥必须在首次建立身份时要求用户导出到设备外，并提供可重复验证恢复包的只读检查。

拒绝：厂商云账户、在线恢复、只存 AppData 的根密钥、把 DPAPI 当可移植文件密码。

### 4.3 运行时秘密的合理防护

来源：[SecureZeroMemory2](https://learn.microsoft.com/en-us/windows/win32/memory/winbase/nf-winbase-securezeromemory2)、[VirtualLock](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock)、[Windows Clipboard](https://learn.microsoft.com/en-us/windows/win32/dataxchg/about-the-clipboard)、[Clipboard formats](https://learn.microsoft.com/zh-cn/windows/win32/dataxchg/clipboard-formats)

采用：

- 解封的根密钥和条目密码使用短租约、确定性清零、最少复制；只对小而明确的关键缓冲区考虑 `VirtualLock`，不锁定正文或大块内存。
- 锁定、会话过期和退出清除租约、正文和剪贴板；剪贴板仅在仍是本应用写入值时清除。
- 写入 Windows 剪贴板时标记不进入历史和云同步；仍明确承认同一用户上下文的其他程序可读取当前剪贴板。

不承诺：抵御同一用户/管理员权限下读取进程内存、注入或调用 DPAPI 的恶意程序。

## 5. 搜索、正则和后台任务边界

来源：[PCRE2 API](https://pcre2project.github.io/pcre2/doc/pcre2api/)、[pcre2_match](https://www.pcre.org/current/doc/html/pcre2_match.html)、[partial matching](https://www.pcre.org/current/doc/html/pcre2partial.html)、[Qt Concurrent](https://doc.qt.io/qt-6/qtconcurrent-index.html)

采用：

- Text/Secure/Hex/S1/S2/S3 的正则语义统一到 PCRE2；偏移在内部以源字节和解码映射明确转换，禁止混用 Qt/Scintilla 正则产生不同答案。
- 分块搜索使用重叠或 PCRE2 partial 语义处理跨块匹配；超出产品可证明边界的可变长回溯/环视必须受限并给出明确结果，而不是漏报。
- 每个 SearchSession 有不可变查询、范围、目标快照/版本、取消令牌和 MatchId。新请求使旧结果失效，旧 worker 不能覆盖新 UI。
- PCRE2 match/depth/heap/offset 等资源限制必须显式设置；错误、超限、取消和完成是不同状态。
- 当前文件是默认；全部打开文件和目录可选。worker 数量有上限，结果按批次增量提交并稳定排序。
- Find All Dock 每项携带文档身份、版本、源字节位置、行列和上下文；双击经统一 OpenDispatcher/Provider 导航，不直接操纵第二套编辑器。

拒绝：每个标签自行启动无上限线程、一次把所有结果对象推到 UI、正则卡死被误报为“无结果”。

## 6. GUI、可访问性和真实验收

来源：[Qt Test Overview](https://doc.qt.io/qt-6/qtest-overview.html)、[QTest](https://doc.qt.io/qt-6/qtest.html)、[Qt Accessibility](https://doc.qt.io/qt-6/accessible.html)

动态 GUI 测试必须使用正式 `notepad-safe.exe` 对应的 KDBX=ON Release 代码路径，并至少验证：

1. 真实鼠标拖动、带 `angleDelta`/`pixelDelta` 的滚轮、键盘和焦点行为。
2. 操作前后可见首行/末行、源字节、选区和 caret，而不只检查 slider 数值或 worker 状态。
3. 搜索 Dock 显示分组、上下文和高亮；双击后正确标签激活、匹配可见且选择范围正确。
4. 保存后重新读文件；安全文件还要重新启动、锁定并解锁验证，不以进程内对象为真值。
5. 100%、125%、150%、200% DPI，窗口缩放、窄窗口、深色/高对比度和完整键盘路径。
6. 控件有名称、角色、状态和焦点顺序；错误不能只靠颜色表达。

单元测试继续用于纯算法和失败注入，但不能替代上述用户旅程。

## 7. 探索停止规则

自由探索只能在以下边界内进行：

- 能直接改善 `TARGET_EXPERIENCE.md` 的核心旅程，或消除发布阻断缺陷。
- 优先复用 Scintilla、Qt、KeePassXC/Botan、PCRE2 和 Windows 平台能力。
- 一个方案连续两轮不能提升既定 GUI 指标，必须降级、替换或明确砍掉，禁止继续局部打补丁。
- 新架构若引入第二个编辑位置真值、第二套文件探测、第二套搜索语义、第二种可移植加密格式或明文恢复缓存，立即否决。
- 未达到数据完整性、安全恢复、连续浏览和精确搜索跳转门槛时，不投入动画、装饰性视觉或低频偏好项。
- 每轮只保留能通过真实 GUI/文件/安全证据的改动；无法证明的完成状态视为未完成。

参考资料是边界，不是功能愿望清单。最终产品是否收敛，以 `ITERATION_ROADMAP.md` 的发布门槛为准。
