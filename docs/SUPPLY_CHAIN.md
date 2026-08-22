# 供应链锁定、源码构建与更新规程

## 1. 构建边界

`third_party/dependencies.lock.json` 是唯一机器可读依赖清单。

| 类型 | 组件 | 仓库/构建边界 |
|---|---|---|
| 外部静态工具链 | Qt 6.11.1、MSVC、CMake、Ninja、Python | 不提交生成后的 Qt/MSVC；CMake 精确拒绝非 6.11.1 Qt，Qt 官方源码包 SHA-256 写入锁文件 |
| 仓库内运行时源码 | QScintilla 2.14.1、KeePassXC 2.7.12、Botan 3.12.0、Argon2 20190702、zlib 1.3.2、PCRE2 10.47、Boost.Regex 1.90.0 | 完整纯源码提交到 `third_party/src`；正式构建不联网 |
| 可再生产物 | QScintilla/Botan 静态库及所有 CMake/qmake 中间文件 | 只写 `.deps/build`、`.deps/install` 或 `build`，均被 Git 忽略 |

Qt 被视为编译器旁的外部工具链，而不是应用运行时下载项。源码仓库加符合锁定版本的 Qt/MSVC 前置环境即可构建；应用依赖不再依赖 `.deps/src`、预编译 QScintilla 或嵌套 Git checkout。

## 2. 双重完整性

版本号本身不能建立可信来源。每个 vendored 组件包含两类独立证据：

- `source_integrity`：官方归档 SHA-256、完整 Git commit，或签名 tag/签名者指纹，证明上游身份。
- `integrity.kind=source-tree-sha256`：对仓库内规范化源码的真实磁盘内容逐文件计算 SHA-256，再对按相对路径排序的清单计算树摘要。

实树摘要不读取 Git index，因此未暂存修改、额外文件和工作树污染都无法逃过校验。`verify_supply_chain.ps1` 还会拒绝 `.lib/.obj/.dll/.exe/.pdb`、qmake/CMake 生成物和嵌套 `.git/.svn`。`.gitattributes` 禁止 Git 按本机 `core.autocrlf` 改写 vendored 字节。

当前校验命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify_supply_chain.ps1
```

## 3. 克隆后的离线构建

正常开发和 CI 只使用统一入口：

```bat
build.bat
```

`build.bat` 激活 x64 MSVC 后调用 `prepare_dependencies.bat`。后者按顺序：

1. 校验完整锁文件与七棵源码树。
2. 用锁定 Qt 的 qmake/nmake 将 QScintilla 以 release、staticlib、C++17、`/MT` 构建到 `.deps`。
3. 确认 PCRE2 与 standalone Boost.Regex vendored 输入；PCRE2 8-bit static、Argon2、zlib 和 KeePassXC Core 随正式 CMake 编译，Boost.Regex 作为受限 header-only target 编译。
4. 以最小模块集合和 `/MT` 从 Botan 源码生成 CMake package 到 `.deps/install`。

QScintilla/Botan 缓存标记包含源码树摘要、工具链版本、运行库和构建选项；任一输入变化会自动删掉对应的精确缓存目录并重建。需要人工排除缓存问题时可分别执行：

```powershell
tools/bootstrap_qscintilla.ps1 -ForceRefresh
tools/bootstrap_crypto_deps.ps1 -ForceRefresh
```

这两个命令仍然不下载源码。`build_incr.bat`、`run_tests.bat` 和 GUI smoke 同样调用统一准备入口，避免产品构建和测试构建产生不同供应链。

## 4. 依赖更新流程

一次只更新一个供应商族；Qt/QScintilla、KeePassXC/Botan/Argon2/zlib、PCRE2/Boost.Regex 不在同一个常规升级中混改。

1. 只从官网或官方仓库读取 release、签名、安全公告和许可变化。常规版本至少观察 14 天；明确安全修复可越过等待期。
2. 在项目目录之外的临时隔离目录取得候选源码。归档先验证官方 SHA-256；Git 来源验证完整 commit 或签名 tag 与固定指纹。
3. 比较 API/ABI、构建选项、许可证、CVE 和上游测试变化。不得从 `develop/main/master/latest` 直接进入产品。
4. 用候选纯源码替换 `third_party/src/<component>-<version>`。删除嵌套 VCS、编译产物和仅由旧机器生成的 Makefile；上游 `.gitignore` 改名为 `.gitignore.upstream`，避免签名提交/发布包中的输入在产品仓库被隐藏；其他 Windows 规范化也必须在 lock 中说明。
5. 本地兼容修改必须成为 `third_party/patches/<component>/<version>/` 中可审查补丁；禁止把产品业务逻辑写进上游树。
6. 使用 `tools/SourceTreeIntegrity.ps1` 计算文件数和新树摘要，更新 lock 的版本、revision、来源、上游证据、`local_path` 与实树摘要。
7. 运行供应链校验、删除对应 `.deps` 缓存并做正式干净构建；再运行非 GUI、真实进程 GUI/E2E 和专项兼容/性能门禁。
8. 生成 SPDX SBOM、更新第三方声明和升级记录；保留上一版本锁文件/发布包以便回滚。

摘要计算示例：

```powershell
. tools/SourceTreeIntegrity.ps1
Get-SourceTreeIntegrity third_party/src/qscintilla-2.14.1
```

专项升级门禁：

- Qt/QScintilla/Scintilla：编辑命令、IME、编码、撤销、lexer、32 MiB 边界、启动和 GUI 基线。
- KeePassXC/KDBX：旧/新版本双向 golden corpus、KDBX3/4、AES/ChaCha20、Argon2d/id、附件、历史、未知字段和第三方 KeePass/KeePassXC 往返。
- Botan/Argon2/zlib：KDF 参数不静默变化、错误口令/损坏认证、内存与耗时门禁。
- PCRE2/Boost.Regex：S1/S2/S3 与 UTF-8、UTF-16、GBK、GB18030、Big5、Shift-JIS、Windows-1252 同查询语料、跨块匹配、原始字节范围、零长度推进、捕获替换、资源上限、取消和 ReDoS 门禁。

## 5. SBOM 与发布

SPDX 2.3 SBOM 由 lock 自动生成，不手工维护第二张版本表。上游归档 SHA-256 写入 package checksum，vendored 树摘要写入 package comment：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/generate_sbom.ps1 `
  -OutputFile build/notepad-safe.spdx.json -ProductVersion 0.3.1
```

`portable_package` 将正式 EXE、GPLv3、第三方声明、锁文件、SBOM 和用户文档写入版本化 ZIP。供应链校验失败、上游身份不明、许可不清或安全状态无法确认时必须停止发布，不能临时替换为另一份二进制。

## 6. 选型与退出边界

- QScintilla/Scintilla 只通过编辑器适配边界使用；可迁移到直接 Scintilla Qt 端口。
- KeePassXC 是 KDBX 行为供应商，不复制其整套桌面架构；产品服务层不暴露上游对象。
- Botan 3、参考 Argon2 和 zlib 按 KeePassXC 闭包锁定；不自研 AES、KDF、Argon2 或认证加密。
- PCRE2 兼容查询是产品方言；PCRE2 与 Boost.Regex 都隐藏在 RegexEngine 后端，存储 Provider 或物理编码不能改变已承诺的用户语义。
- 用户退出路径始终是标准 KDBX/恢复导出和显式明文另存为，依赖升级不得制造不可逆数据迁移。
