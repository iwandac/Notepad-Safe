# 供应链锁定与更新规程

## 目标

`third_party/dependencies.lock.json` 是依赖身份的唯一机器可读来源。`active` 表示已经进入当前构建或源码树，`planned` 表示架构已经选定但尚未接入；二者不能混写。

版本号本身不足以建立可信来源。归档必须锁定官方 SHA-256；Git 来源必须锁定上游签名 tag 和完整签名者指纹；仓库内 vendoring 的源码还要锁定 Git index 清单摘要。`release/`、`debug/` 等构建产物不属于源码清单。

## 常规更新流程

1. 只从项目官网或官方仓库读取 release、签名、校验和、安全公告及许可变更。
2. 常规版本发布至少观察 14 天；存在修复已知漏洞的安全版本可以立即进入验证。
3. 在隔离目录下载源码归档，先验证 SHA-256 或 GPG，再查看变更日志和 ABI/API 破坏项。
4. 更新 lock 中的精确版本、revision、来源和完整性字段，不允许 `latest`、分支名、`TBD`。
5. 更新 vendored 源码后重新生成清单摘要，并检查没有把 `.obj`、`.lib`、构建目录混入源码清单。
6. 执行供应链校验、干净构建、完整 CTest、真实进程 GUI/E2E；保存兼容性和性能基线结果。
7. QScintilla、KeePassXC/KDBX 或密码学依赖升级必须额外执行上游互操作 fixture。密码学参数和 KDBX 数据迁移必须显式版本化，不能随库升级静默改变。

当前 Windows 校验命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify_supply_chain.ps1
```

生产 KDBX 依赖准备命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_crypto_deps.ps1
```

该脚本只接受锁文件中的来源和精确身份：KeePassXC/Botan/zlib 归档先校验 SHA-256，Argon2 checkout 校验完整 commit，随后回放 `third_party/patches/keepassxc-2.7.12` 中的兼容补丁并构建最小静态 Botan。产物缓存于被 Git 忽略的 `.deps/`，重复执行会做幂等校验；只有明确传入 `-ForceRefresh` 才重建缓存。

生产构建使用 `-DNOTEPADSAFE_ENABLE_KDBX_BACKEND=ON`。CMake 的默认值仍为 `OFF`，用于不下载密码学供应链的编辑器/领域层验证；仓库的 `build.bat` 会先执行上述 bootstrap 并显式开启生产后端，除非调用者设置 `NOTEPADSAFE_WITH_KDBX=OFF`。

## 选型边界

- Qt 和 QScintilla 是 UI/编辑器供应链。QScintilla 保持小补丁、可回放的 vendor patch，不把产品领域逻辑直接写进上游目录。
- KeePassXC 是 KDBX 行为与互操作的参考上游，不直接复制整个应用架构。第一阶段抽取最小必要的 KDBX 数据模型、序列化和测试 fixture。
- Botan 3 是计划中的通用密码学执行层；KeePassXC 2.7.12 的 Argon2Kdf 仍使用上游参考 libargon2，两者都必须锁定。禁止自研 AES、Argon2、密钥派生或认证加密实现。
- KDBX gzip 压缩路径使用锁定的 zlib；不为减少一个依赖而改变标准 KDBX 压缩语义。
- PCRE2 是统一正则语义的计划供应商；S1、S2、S3 共用语法/选项契约，执行器按存储层分别实现。

## 失败策略

供应链校验失败时停止发布构建。若上游签名身份、许可、归档摘要或安全状态无法确认，不用“临时可用”版本顶替；保持上一已验证版本并记录阻塞原因。
