# 第 8 轮：可选远程 AI 决策与验收契约

> 状态：设计完成；默认不实现远程 Provider
> 更新日期：2026-08-30
> 前置条件：默认策略可在 AI-D0 确定，拒绝门随第 5/7 轮验证；只有另行批准的真实远程分支才安排在本地 AI 成熟后，不阻塞本地路线

## 1. 两种合法完成方式

### A. 明确拒绝远程 AI（默认推荐）

产品保持本地优先、离线核心和最小数据边界。产品构建不注册远程 Provider、不出现 API key/UI/网络 fallback；negative gate 扫描并动态证明文档助手零远程外连。获批本地模型的固定 loopback 通信不等于远程外传，应单独记录并限制。该选择不降低第 7 轮本地 AI 的功能验收资格，也不要求等待供应商、采购或联网验证。

### B. 批准一个具体远程 Provider

只有产品负责人批准供应商、数据政策、地区、费用和密钥方案后，才能实现单一 HTTPS adapter。不得先造“通用 OpenAI-compatible URL”让用户把正文发往任意服务。

## 2. 批准远程所需产品事实

- 供应商与具体 API/model/version；服务条款、隐私政策、数据保留、训练使用、人工审阅、删除、subprocessor、region 和企业/个人账号差异的存档日期与链接。
- 允许的数据等级：默认只允许 Ordinary Plain 的明确选区；Secure Note、Accounts、锁定内容和恢复材料默认永久拒绝。
- 费用：输入/输出计价、上限、预算耗尽、失败/重试是否计费，以及用户可见估算的计算方法。
- API key 的归属与存储：用户自带/产品账户；使用 Windows 受保护凭据 adapter，不进入 QSettings、session、日志、命令行或环境变量快照。
- 网络边界：固定 HTTPS origin、证书/代理策略、重定向、超时、重试、region endpoint 与撤销方案。

上述事实任何一项缺失，选择 A，而不是以模糊提示让用户承担未知风险。

## 3. 逐次授权界面

每次远程请求都显示并绑定：

- 文件显示名（不发送完整路径）、精确范围和预计字符/token；
- 任务、Provider、model、远程目的地与处理地区；
- 保留/训练/人工审阅摘要及政策链接；
- 费用估算/上限；
- “仅本次发送”与 Cancel。

授权 nonce 绑定 documentId、revision、range、原 hash、task、provider/model、目的地、有效期和一次使用。勾选“记住”不属于首版；重试、续写下一轮和模型切换都需要新授权。

## 4. Provider 与网络合同

- 仅固定 HTTPS origin；拒绝明文 HTTP、任意 base URL、IP/host 改写、跨 origin redirect 和响应内 URL fetch。
- 本地失败绝不自动转远程；远程失败也不换另一个供应商。重试只针对明确幂等、未收到响应且产品批准的阶段，并重新展示可能费用。
- 请求只含固定 system/task template、获批正文与最小参数；不发送路径、用户名、设备名、session、其他标签、剪贴板或诊断日志。
- 响应视为不可信文本，经大小/编码/schema 验证后才进入 Artifact/Proposal；tool/function call、HTML、URL 和可执行指令不被执行。
- proxy/TLS/certificate/revocation/timeout/DNS/429/5xx/partial stream/中途断网都返回结构化产品错误，原文和既有结果不变。

## 5. 验收矩阵

### 若选择 A

- 产品 EXE、CMake link graph、SBOM、registry 和运行时 factory 中无远程 adapter 初始化；固定 loopback client 与远程权限分开审查。
- 文档助手只显示“本应用内 / 本机模型”等实际获批处理位置；本地服务失败时，以获准且限定到测试进程的观测证明零远程外连。静态扫描不能冒充动态网络证据，缺证据时记录未验收。
- 配置伪造 remote URL、模型输出要求上传、文档中 prompt injection 都不能产生远程请求。

### 若选择 B

- Ordinary Plain 的解释/改写各完成一次真实供应商 E2E，记录脱敏 request identity、区域、token、费用、首 token、取消和 Artifact/Proposal；不保存正文/输出。
- 无选区、revision stale、授权过期/复用、Provider/model/region 改变、重试、redirect、代理、TLS 错误、429、5xx、partial stream 与取消全部 fail closed。
- Secure、Accounts、锁定、B2/B3 全文和 secret synthetic target 在网络 adapter 创建前硬拒绝；抓包证明零请求。
- key 创建、读取、轮换、删除、错误账户、Windows 用户切换和日志扫描；任何输出不含 key/token/header。
- 30 轮资源/UI/取消与预算；真实费用与估算偏差有阈值和失败处理。

## 6. 打回与退出

以下任一项立即打回：任意 URL；一次同意永久复用；本地失败自动上传；Secure/Accounts 只靠文案而非策略拒绝；API key 存入设置/日志；隐藏费用/保留；跟随 redirect；执行 tool call；网络测试替身冒充真实服务。

选择 A 且拒绝门通过，或选择 B 且产品事实、逐次授权、真实 E2E、安全/费用/故障矩阵全部通过后，第 8 轮关闭。
