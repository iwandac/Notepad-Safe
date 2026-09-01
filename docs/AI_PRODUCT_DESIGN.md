# Notepad Safe AI 与代码可视化产品设计

> 状态：设计基线；真实 AI/代码解析尚未实现
> 更新日期：2026-08-30；沿用既有设计，按用户范围调整执行顺序

本设计以[成熟度路线图](ITERATION_ROADMAP.md)为执行索引。Windows 安装、签名、打包分发与发布渠道不在本次范围，也不是 AI 开发的前置条件；Windows 上的运行质量与 KDBX ON/OFF 验收仍保留。下列公开产品参考是此前调研基线，本次恢复设计未重新联网核验版本或接口。

## 1. 用户价值

AI 不是聊天窗口的堆叠，而是四个明确的文档任务：

1. **看懂这段内容**：解释选中的文字、代码或脚本，先给结论，再说明关键步骤。
2. **看见它怎么流动**：把代码表达成“输入 → 处理 → 判断/重复 → 输出/副作用”的可点击流程图。
3. **把文字写得更合适**：总结、改写、续写、按要求生成；不覆盖原文，先显示提案。
4. **安全地采用结果**：清楚显示读取范围、处理位置和将要修改的内容；用户确认后一次应用，一次 Undo 可撤回。

成熟产品参考采用以下能力，而不照搬其 IDE/云平台范围：

- VS Code 的 Outline、Breadcrumbs 和 Call Hierarchy：结构与调用关系可导航，而不是只有语法着色。
- Visual Studio Code Map：用结构、依赖、调用和容器层级帮助用户理解代码。
- GitHub Copilot 的选区/文件上下文、解释、改进、生成测试和建议变更：上下文明确，建议由用户决定是否应用。
- Tree-sitter 的增量、容错语法树：解析即使面对未完成代码也能提供有界结构事实。
- llama.cpp server 的本地 OpenAI-compatible chat/response/embedding 接口：模型在独立进程中运行，主程序不嵌入模型运行时。

参考链接：

- https://code.visualstudio.com/docs/editing/userinterface
- https://code.visualstudio.com/docs/cpp/cpp-ide
- https://learn.microsoft.com/visualstudio/modeling/map-dependencies-across-your-solutions
- https://docs.github.com/copilot/how-tos/chat-with-copilot
- https://tree-sitter.github.io/tree-sitter/
- https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md

## 2. 产品界面

工具菜单增加“文档助手”，打开右侧非模态面板。面板只使用用户语言：

- **要做什么**：解释、画出流程、总结、改写、续写、按要求生成。
- **读取范围**：默认“当前选中的内容”；没有选区时禁止静默读取全文，只提供“明确使用当前文档”。
- **在哪里处理**：本机模型 / 远程服务。远程只有在产品批准并配置后出现。
- **将发生什么**：只生成说明 / 生成修改建议。
- **结果**：说明、流程图或差异预览；显示“内容已变化，请重新运行”而不是套用旧结果。

对非技术用户，代码说明固定回答：

1. 这段内容要完成什么。
2. 输入从哪里来。
3. 依次做了哪些事。
4. 在哪里判断、分叉或重复。
5. 读取或修改了什么。
6. 最终输出和可见副作用是什么。
7. 哪些结果无法仅凭静态代码确认。

界面不以 AST、CFG、token、Provider、revision、KDBX 等词作为主文案。

## 3. 数据与权限合同

```text
Editor selection / explicit whole-document action
                    |
                    v
ApprovedDocumentSlice
  documentId + revision + securityClass + contentKind
  exact source range + encoding + byte/time budget + one-shot approval
                    |
                    v
DocumentTool / CodeParser
  source is untrusted data, never system instructions
                    |
          +---------+----------+
          |                    |
          v                    v
 deterministic CodeModel   InferenceService
          |                 policy + provider + timeout/cancel/limits
          +---------+----------+
                    v
AnalysisArtifact / TypedEditProposal
  bound to document + revision + source ranges
                    |
                    v
Preview / graph / diff -> user confirms -> one undo transaction
```

`ApprovedDocumentSlice` 必须包含：

- 文档身份、revision、安全级别、内容类型和编码；
- 明确的半开源范围 `[byteStart, byteEnd)`；
- 不可变、有界读取接口，避免同步复制整篇文档；
- 输入字节、输出 token、运行时间和内存预算；
- 绑定 Provider、目的、有效期和单次使用的一次性授权。

账号密码、TOTP seed、恢复材料和密钥在类型与 capability 层硬拒绝。UI 隐藏不是安全边界。

## 4. 代码与脚本可视化

确定性 parser 是事实来源，AI 只允许润色这些事实，不能发明边或副作用。

```text
approved slice
  -> language resolver
  -> pinned static parser
  -> CodeModel(symbol/call/branch/read/write/sourceRange/confidence)
  -> product-language explanation
  -> bounded native graph
  -> click node -> exact source range
```

结论分三级：

- **静态确认**：parser 可直接证明的定义、调用、赋值、返回和分支。
- **保守推断**：动态语言中的可能调用、别名或外部状态影响。
- **无法确认**：运行时输入、网络/文件内容、反射、动态导入或外部命令结果。

安全限制：不执行脚本、不导入模块、不读取未获批文件、不启动 Shell/调试器；限制节点数、嵌套深度、字符串长度、解析时间和内存；SVG/HTML 不得含脚本、外链或可执行 URL。

首批语言推荐 Python、PowerShell 和 JavaScript，但 parser 与供应链需要产品裁决后才能锁定。

## 5. AI 推理边界

`InferenceService` 是唯一策略入口，负责：

- Provider 选择与 capability discovery；
- 本地/远程暴露策略和一次性授权；
- 不允许本地失败自动回退远程；
- 输入/输出上限、超时、取消、流式更新和结构化错误；
- 日志脱敏，不记录 prompt、正文、模型输出或文件路径；
- 结果只进入内存 Artifact/Proposal，不进入 session、Draft、崩溃报告或自动保存。

本地模型优先采用独立 loopback 进程 seam。Loopback 不等于天然安全：必须固定 endpoint、模型/模板身份，禁止自动下载和 prompt 日志，限制缓存目录与生命周期；模型进程不得获得文件系统或 Shell 工具权限。

远程 Provider 默认不存在。只有完成数据等级、供应商保留/训练政策、HTTPS、API key 安全存储、费用与逐次确认设计后才可实现。

## 6. 文本生成与应用

第一版只允许两类结果：

- `AnalysisArtifact`：只读解释、摘要、流程和提示。
- `TypedEditProposal`：对获批选区的一组有序替换，包含原范围、原 hash、替换文本和理由。

应用前同时复核 documentId、revision、range、原 hash 和 Edit capability。成功应用形成一个 Undo transaction；失败保持文档、磁盘和恢复状态不变。整篇重写、自动保存、多文件修改和脚本执行不属于第一版。

## 7. 性能与验收预算

- 首帧前 Tool/Parser/Provider/模型实例化为 0。
- 选择快照主线程单次工作目标 `<16 ms`，硬上限 `<50 ms`；大范围读取分块并可取消。
- 本地 AI 记录首 token、总耗时、输入/输出量和取消延迟；取消目标 `<100 ms`，超时必须释放请求资源。
- CodeModel 和图默认最多 500 节点/1000 边；超过时按入口/关键路径折叠并解释截断。
- 锁定、关闭、revision 变化或 Provider 切换后，旧结果立即 stale，不可应用。

每轮至少覆盖 Ordinary Plain、已解锁 Secure、Accounts denied、锁定、关闭、stale、取消、超时、Provider 失败、日志无正文、KDBX=ON/OFF 和首帧延迟加载。Secure 文档是否可进入外部本地模型是独立产品决策；在决定前只允许不跨进程的确定性工具。

## 8. 明确不做

- 通用 IDE、终端、调试器、插件市场或自动代理。
- 自动运行生成代码、安装依赖或访问未授权项目文件。
- 自动上传、远程 fallback、后台全文索引或默认持久化聊天。
- 用 AI 生成的图替代确定性 parser 事实。
- 把账号秘密、恢复材料或密钥作为“用户选中的文本”绕过隔离。

## 9. AI-D0：性能轮之后的设计收敛与开发交接

### 顺序与完成定义

第 3 轮性能/恢复验收完成后，直接进入 AI-D0，不插入历史第 4 轮分发任务。复用本文及第 5–8 轮契约，不重新创建一套并行架构。AI-D0 的结果是可开发、可验收的具体合同，不代表真实 AI 已经实现。

1. 根代理核对当前 DocumentSession、Capabilities、命令注册、异步任务和恢复边界，明确新增职责归属，禁止让 MainWindow/EditorTab 再承担推理或解析实现。
2. 固定“选区 → 本次授权 → 内存结果 → 来源/Diff → 确认 → 一次 Undo”的界面状态与数据合同，包括无选区、编码、超预算、取消、锁定和内容变化。
3. 给第 5 轮各切片写出输入、输出、禁止行为、正负例、真实 GUI 路径和失败判据；确定性工具必须有真实用户价值。整理空白不能笼统保证代码语义不变，尤其是多行字符串、Markdown 与依赖空白的内容；应展示精确 Diff 并由用户确认。
4. 集中登记下面的产品决定；缺项只阻塞对应 Provider/parser 分支，不阻塞安全基座和默认拒绝策略。
5. 按切片委派子代理开发；根代理审查差异、执行相关 KDBX ON/OFF 验收与可见旅程。失败退回同一切片修复，不通过降低阈值或用测试替身宣告完成。

### 需要产品决定的最小集合

| 决定 | 尚未决定时的安全默认 | 影响范围 |
| --- | --- | --- |
| 首个语言、后续语言及固定 parser/grammar 供应链 | 不增加真实 parser；可开发语言无关合同 | 第 6 轮真实语言分支 |
| 本地模型/runtime 的来源、许可证、身份与生命周期 | 不下载、不启动未登记服务；只做合同与禁用状态 | 第 7 轮真实推理 |
| Secure 明文是否允许跨进程进入本地模型 | 拒绝；进程内确定性工具仍可用 | 第 7 轮 Secure 推理 |
| 最低硬件、模型资源预算及无加速时体验 | 不承诺未经测量的速度 | 第 7 轮支持范围与性能门 |
| 是否引入远程服务 | 不注册远程 Provider、不外传、不 fallback | 第 8 轮；不阻塞本地路线 |

模型缓存按获批模型文件管理；prompt、正文、结果与聊天历史默认仅在内存，不借“缓存”之名持久化。任何扩大数据范围、启动第三方模型进程或联网下载都必须符合对应决定，不能从“继续目标”推定授权。

### 开发优先级

- **先第 5 轮**：做通共用安全基座和两个确定性工具，关闭 5A–5D。
- **优先第 7 轮文本助手**：真实模型身份/生命周期 → 流式与取消 → 总结、改写端到端 → 解释、续写、按要求生成。前两个旅程通过是中间里程碑，五任务和完整退出门通过后才关闭第 7 轮。
- **第 6 轮独立推进**：基座通过后，先做一个获批语言的解析 → 产品说明 → 图 → 来源导航闭环，再扩展其他获批语言。无需等所有语言完成才开发纯文本 AI；代码事实增强只有相应 CodeModel 验收后才启用。
- **第 8 轮默认拒绝门**：AI-D0 明确默认策略，在第 5/7 轮累积零远程外连证据；没有远程采购或联网验证等待项。
- **最后第 9 轮组合验收**：核心编辑、确定性工具、获批代码可视化与本地 AI 在同一候选上复验质量、性能和隐私，不进行 Windows 安装发布。

详细验收沿用：[第 5 轮基座](ROUND5_DOCUMENT_TOOL_FOUNDATION_ACCEPTANCE.md)、[第 6 轮代码可视化](ROUND6_CODE_VISUALIZATION_ACCEPTANCE.md)、[第 7 轮文本助手](ROUND7_LOCAL_AI_TEXT_ASSISTANT_ACCEPTANCE.md)、[第 8 轮远程策略](ROUND8_REMOTE_AI_DECISION_ACCEPTANCE.md)、[第 9 轮组合验收](ROUND9_AI_RELEASE_ACCEPTANCE.md)。当前 AI-D0 尚未关闭，以上待决定项不是已经批准的产品选择。
