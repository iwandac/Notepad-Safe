# 第 5 轮：安全文档工具基座验收契约

> 状态：设计完成，尚未进入开发
> 更新日期：2026-08-30
> 前置条件：第 3 轮性能/恢复验收关闭，完成 AI-D0 基座设计交接；不要求 Windows 分发或真实远程 Provider 决策

## 1. 本轮用户结果

用户可以在不交出整篇文档、不改变源文件的前提下，对明确选中的内容运行文档工具；结果先作为可检查的说明或修改建议出现。只有用户确认后才修改当前文档，而且一次 Undo 能完整撤回。

本轮交付两个小而真实的确定性工具：

- **文本概览**：说明选区的字符、词/段落、行、空白与编码异常等可确定事实，并可点击回到相应范围。
- **整理空白**：仅对明确选区提出“删除行尾空格 / 收敛连续空行”建议；默认不应用，不改变换行类型或文档外内容。

它们既提供实际价值，也证明未来代码 parser 与 AI 只能通过同一安全合同进入产品；不创建空壳聊天 UI。

## 2. 固定数据边界

`ApprovedDocumentSlice` 是唯一工具输入，包含 documentId、revision、securityClass、contentKind、encoding、精确半开字节范围、预算、目的、工具身份、到期时间和单次授权 nonce。正文通过不可变有界 reader 提供，不包含路径；工具不能持有 EditorTab、MainWindow、persistence adapter 或文件句柄。

- 默认范围始终是当前选区。无选区时工具按钮不静默读取全文，只提供明确的“使用当前文档”动作，并显示预计字节数。
- Plain 与已解锁 Secure Note 可进入进程内确定性工具；锁定文档、Accounts、密码、TOTP seed、恢复材料和密钥在 capability/type 层硬拒绝。
- 结果默认只在内存存在，不写 session、Draft、剪贴板、日志、崩溃报告或临时文件。
- 关闭、锁定、revision 变化、授权到期、取消或工具卸载立即使 slice/artifact/proposal stale。

## 3. 切片 5A：范围授权与任务生命周期

### 开发合同

文档助手面板显示“要做什么、读取范围、处理位置、输出类型”四项。本轮处理位置固定为“本应用内”；不存在模型、网络或 Provider 选择。授权只允许一次运行，不可转用于另一个工具或重试。

### 验收

- Plain Text、B1 Text interpretation、已解锁 Secure Text：正向/反向选区、Unicode、CRLF、非法编码单元和预算边界的范围与原 hash 精确一致。
- 无选区时真实按钮路径要求全文确认；取消后 factory 不实例化、reader 不读取。
- Accounts、锁定、安全秘密 synthetic target、文档关闭和 revision 改变均在读取前拒绝；错误使用“发生了什么 / 还能做什么 / 下一步”产品语言。
- 首帧前 100 个 Tool descriptor 可见但重型 factory=0；首次运行才实例化目标 Tool。

## 4. 切片 5B：typed artifact、annotation 与来源导航

### 开发合同

`AnalysisArtifact` 只包含有界产品文本、typed annotation、source range、confidence、截断/警告和 document/revision identity。渲染器使用原生控件；不接受任意 HTML、脚本、外链、data URL 或工具提供的可执行命令。

### 验收

- 点击每条结果回到原 documentId/revision/range；目标变化时不近似跳转，显示“内容已变化，请重新运行”。
- 500 条 annotation、超长 label、RTL/emoji、窄窗口、深浅主题、100%/200% DPI 可滚动、键盘可达、读屏名称完整。
- 工具伪造别的 documentId、越界 range、重复/交叉非法 annotation、超预算文本或 URL 时 registry fail closed；UI 不渲染部分不可信 artifact。
- 关闭面板或锁定安全文档后，artifact 正文从 model/widget/clipboard 清除；脱敏日志只保留 tool id、状态、时长和计数。

## 5. 切片 5C：typed edit、Diff 与一次 Undo

### 开发合同

`TypedEditProposal` 是按源位置排序、互不重叠的替换集合，每项绑定原 range、原 bytes hash、replacement、理由和编码。应用前重新检查 documentId、revision、原 hash、Edit capability 与编码可表示性；全部检查成功后才形成一个编辑事务。

### 验收

- Diff 同时显示原文、建议和将修改的精确范围；Apply、Cancel、重新运行与关闭结果都不自动保存。
- 多段替换要么全部应用、要么零应用；一次 Undo 恢复 byte-for-byte 原内容、caret/selection 与 dirty 语义，一次 Redo 恢复建议。
- stale revision、重叠 range、越界、hash 不符、只读 B2/B3、锁定/关闭、不可表示字符和预算溢出全部在 mutation 前拒绝。
- Secure Note 的应用只改变内存文档并走原有加密保存/恢复；不得因工具产生明文临时文件。KDBX=OFF 仍通过 Plain 合同。

## 6. 切片 5D：两个生产确定性工具

### 文本概览

统计定义必须在 Unicode/换行/编码语料上固定；结果中的“异常”只报告可直接证明的事实，不判断内容质量。每条有位置的发现绑定范围，超出预算时明确“仅分析了选定范围/已截断”。

### 整理空白

默认建议只删除选区内每行末尾的空格/Tab，并把 3 个及以上连续空白行收敛到 2 个；不改变 CRLF/LF、文件尾换行、选区外内容或 Unicode 空格。用户可分别取消两类建议。不能保证空白修改不影响代码多行字符串、Markdown 等内容的语义；必须显示精确 Diff 和这一限制，不能自动采用。

### 验收

- golden 覆盖 UTF-8/16、GBK 显示投影、CRLF/LF、无尾换行、emoji、组合字符、空选区、超长行、31 MiB 边界。
- Plain 与已解锁 Secure 对相同内容给出相同 artifact/proposal；Accounts denied；B2/B3 只允许有界只读概览，整理空白禁用。
- 真实 GUI 完成选择 → 授权 → 结果 → 来源导航 / Diff → Apply → Undo → 保存 → 重开，文件结果逐字节验证。

## 7. 打回与退出

以下任一项立即打回：工具直接读取文件/EditorTab；无选区静默全文；artifact 可执行内容；stale 仍能应用；多段部分修改；自动保存；Secure 明文落盘；Accounts 只靠隐藏按钮而非类型拒绝；用测试 Echo/StaticGraph 冒充生产能力。

5A–5D 全部通过 KDBX=ON/OFF 相关 Release GUI、架构/供应链、首帧和视觉门后，第 5 轮才能关闭。
