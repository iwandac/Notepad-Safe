# 第 4 轮：Windows 分发收敛验收契约

> 状态：历史设计，按 2026-08-30 用户决定排除出本次路线；未执行、未验收，不是 AI 的前置条件
> 原设计日期：2026-08-29；范围更新：2026-08-30

以下内容仅保留历史设计与调研记录，不构成本次待执行任务或待用户回答的安装/签名问题。第 3 轮关闭后直接进入 AI-D0。只有用户将来另行提出 Windows 分发，才恢复本契约并重新核验平台信息。原生视觉、真实 UIA、数据 schema 兼容与故障保全要求已经转入[第 9 轮组合产品验收](ROUND9_AI_RELEASE_ACCEPTANCE.md)，不会随分发范围一起删除。

## 1. 本轮用户结果

- 用户下载到的每个文件都能回答“是谁构建、来自哪个 commit、包含哪些依赖、是否被篡改”。
- 便携版解压即可运行；安装版若获批，则安装、升级、回滚和卸载不会损坏用户选择保存的文档，也不会把不兼容 session/恢复稿交给旧版本盲读。
- Windows 显示可验证的发布者身份；签名和时间戳由发布门禁验证，不以资源管理器看见名称或本机自签成功代替。
- 干净 Windows 机器无需开发工具、Qt SDK、MSVC runtime 下载或网络即可完成核心离线编辑、保存和恢复旅程。
- 发布失败、签名缺失、证据过期或 dirty workspace 都产生明确的“不可发布”，不会降级生成一个看似正式的包。

## 2. 当前公开平台约束

微软当前官方合同要求 SignTool 显式指定文件摘要和时间戳摘要，推荐 SHA-256；发布验收使用默认 Authenticode policy 的 `signtool verify /pa`。签名只能证明发布者和签名后未被修改，不能直接保证 SmartScreen 声誉；微软已明确 EV 证书不再自动获得 SmartScreen 正面声誉。

MSIX/App Installer 能提供包身份、差分更新和可配置更新检查，但升级/降级会保留 app data，而卸载 packaged desktop app 可能删除被重定向的 AppData/HKCU 写入。Notepad Safe 的 session、加密恢复稿和设置目前位于这些区域，因此在确定迁移、卸载保留和回滚合同前，不能把 MSIX 视为无风险封装。

参考：

- [Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)
- [使用 SignTool 验证文件签名](https://learn.microsoft.com/en-us/windows/win32/seccrypto/using-signtool-to-verify-a-file-signature)
- [SmartScreen reputation for Windows app developers](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
- [MSIX 部署与降级规划](https://learn.microsoft.com/en-us/windows/msix/desktop/managing-your-msix-deployment-targetdevices)
- [Packaged desktop app 的数据与卸载行为](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)
- [App Installer 自动更新与修复](https://learn.microsoft.com/en-us/windows/msix/app-installer/auto-update-and-repair--overview)

## 3. 产品决定门

### 前轮保留的原生视觉与辅助功能风险（现由第 9 轮承接）

- 第 2 轮 Tail 原生多标签暗色截图中，非选中标签标题仍可能呈黑字/暗背景；当前选中编辑内容的配色已单独修复。第 9 轮完整视觉出口须复验并收敛该壳层问题，不能把 Tail 当前行对比度通过当作所有控件暗色通过。
- 实际 Windows UIA 探针显示，Qt Label 的 `accessibleDescription` 在本机 .NET UIA `HelpText` 中可能为空。Tail 已将实际状态与原因放入可访问 Name；最终读屏验收必须检查平台实际暴露的动态名称/值，不能仅检查 QObject 属性非空。既有状态栏计数等应列入该复核，不以局部 Tail 修复宣称全应用读屏完备。

开发 4C/4D 前必须由产品负责人明确：

1. 发布者法定/显示名称及证书来源（CA 代码签名证书、Microsoft Artifact Signing 或其他获批身份）；私钥是否位于硬件/托管签名服务。
2. 首发渠道：仅签名便携 ZIP，还是同时提供 MSIX/App Installer；是否进入 Microsoft Store。
3. 更新策略：完全手动、启动时提示或后台；是否允许强制更新；是否允许回滚。
4. 卸载时 session、设置和加密恢复稿的保留/删除策略。用户选择的源文档永远不属于卸载器。

缺少这些决定时可以完成可重复的未签名开发包与迁移验证，但第 4 轮和发布门 RG-09 必须保持失败。

## 4. 切片 4A：唯一版本与可重复便携包

### 设计

版本只来自一个机器可读源，并进入 PE version resource、包目录、SBOM、provenance 和 UI About。包生成采用 allow-list，不打包 build cache、测试、私钥、日志、恢复稿或本机绝对路径。

### 验收

- 两次从相同 source identity 与锁定工具链构建，解释任何非确定字节；不得无说明声称 bit-for-bit reproducible。
- `verify_release_package.ps1` 验证唯一顶层目录、精确文件清单、完整 GPLv3、THIRD_PARTY_NOTICES、依赖 lock、SPDX 2.3 SBOM、产品版本和 EXE hash。
- provenance 至少绑定 commit、dirty=false、source archive hash、dependency lock hash、SBOM hash、工具链、KDBX、测试 manifest、视觉/性能/签名 evidence hash 和最终包 hash。
- ZIP 解压到含空格、非 ASCII、长路径但受支持的位置，非管理员启动并完成核心旅程。

## 5. 切片 4B：N-1/N-2 数据兼容与回滚

### 设计

session、settings、draft、Secure Note、Accounts 和未来 AI/tool 元数据各自有显式 schema/version。新版本只执行可回滚或可重新生成的迁移；迁移前保留受控备份，失败保持旧数据且不启动半迁移工作区。旧版本遇到新 schema 必须 fail closed 并解释，不把未知字段当正文。

### 验收

- N-2 → N-1 → N 和 N → N-1（仅在获批允许时）矩阵：干净状态、混合标签、dirty plain/B1、Secure、Accounts、损坏/部分迁移。
- 每步记录用户文档 hash、session/draft 引用、KDBX 可重新打开结果、设置差异和备份清单；失败恢复后只存在一个权威 active generation。
- 更新中强杀、磁盘满、权限拒绝和安全软件占用不会同时损坏旧安装与新安装；重启后能确定继续、回滚或清理。
- 旧版本无法安全读取新数据时明确阻止回滚；不以“MSIX 支持 downgrade”上推为产品数据兼容。

## 6. 切片 4C：签名与证据链

### 设计

签名发生在全部测试通过的最终 EXE/package 上，签名后重新计算 hash 并只进行只读验证；发布流程不得接收 PFX 密码命令行参数、把私钥复制进仓库/构建目录或记录证书秘密。时间戳使用获批 RFC 3161 服务与 SHA-256。

### 验收

- `signtool verify /pa /all /v` 对 EXE 和所选安装包通过；证书 subject、thumbprint、chain、EKU、有效期、timestamp subject/time 进入结构化 evidence。
- 修改签名后任一字节、使用错误证书、无时间戳、过期 evidence、包内 EXE 与已验证 EXE 不同，RG-09/RG-10 必须失败。
- 离线验证区分“签名结构/已签名 hash 正确”和“在线吊销检查可达”；网络失败不能伪装成完全验证通过。
- SmartScreen/Smart App Control 的实际提示只作为目标 Windows 版本的观察证据，不承诺可由代码或 EV 证书直接消除。

## 7. 切片 4D：干净机安装、升级与卸载

### 设计

便携 ZIP 是已确定的最小发布形态。只有产品决定批准后才实现 MSIX/App Installer 或其他安装器；选型必须先证明现有 LocalAppData/QSettings/文件关联/单实例语义不会因包虚拟化改变。更新检查不得进入首帧前，也不得让离线核心能力失效。

### 验收

- Windows 11 当前受支持版本的干净 VM：标准用户安装/解压、首次启动、离线编辑、重启恢复、文件关联、多个打开请求、修复/升级/回滚/卸载。
- Windows Security/SmartScreen 实际画面、安装日志、事件日志、进程/文件/注册表差异和最终残留进入脱敏 evidence。
- 卸载绝不删除用户选择目录中的 Plain/Secure/Accounts 文件；session/draft/settings 按获批策略执行并在 UI 中提前说明。
- 无 Qt/MSVC/SDK/开发 `.dll` 依赖；不联网时除明确的更新/签名吊销检查外功能可用。

## 8. 第 4 轮退出门

- 第 0 轮 RG-01…RG-10 全部 PASS，工作树 clean；不得使用 `-AllowDirty` 形成发布候选。
- KDBX ON/OFF、ASAN、性能、视觉、包、签名和干净机证据全部绑定同一最终 source/package identity。
- 零 P0/P1；已知 P2/P3 有用户影响、规避、负责人和版本计划。
- 产品负责人已批准发布者身份、渠道、更新和卸载策略。

任一缺失都保持“开发预览”，不生成 release tag 或公开下载声明。

## 9. 退出记录

尚未执行，当前明确排除，不标记 PASS。历史 4A → 4B → 4C → 4D 顺序不再是本次计划；安装格式、证书、渠道和卸载策略均不阻塞性能或 AI 路线。
