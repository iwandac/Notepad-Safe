# Notepad Safe

> Qt/QScintilla/Scintilla Windows 编辑器：默认安全地记录，必要时显式使用明文文件。
> 本仓库为独立产品仓库，clone 后可直接构建运行。

生产加密存储已经接入锁定的 KeePassXC 2.7.12、Botan 3.12、libargon2 与 zlib。新建和未命名草稿默认使用自包含 KDBX4 安全文档；普通文本通过统一“打开文件”进入，只有“另存为”用于安全文档/文本的显式转换。应用支持本地安全身份、独立恢复密钥、Windows DPAPI 密封的七天会话、按操作短期解封的密钥租约、历史主密码文件回退、中央账户工作区和原子保存。文本按 `<32 MiB`、`32 MiB–2 GiB`、`>2 GiB` 分级；未知二进制自动进入十六进制视图。查找和查找全部可选择当前文件或全部打开文件，结果按文件分组并支持精确双击跳转。后续开发以 [架构设计](docs/DESIGN.md)、[目标体验与动态验收参考](docs/TARGET_EXPERIENCE.md)、[参考基线](docs/REFERENCE_BASELINE.md) 和 [目标模式收敛路线](docs/ITERATION_ROADMAP.md) 为共同基线。

详细说明见：[docs/README.md](docs/README.md)
设计文档见：[docs/DESIGN.md](docs/DESIGN.md)

## 快速开始

```bat
:: 1. 设置 Qt 静态库路径（或跳过使用默认路径 E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64）
set NOTEPADSAFE_QT6_DIR=你的Qt静态库路径\msvc2026_64

:: 2. 一键构建（首次会下载并校验锁定的 KDBX/密码学源码）
build.bat

:: 3. 运行
build\notepad-safe.exe

:: 4. 运行非 GUI 回归（可选；测试目标不进入默认产品构建）
run_tests.bat

:: 5. 构建并运行 GUI smoke（可选）
run_all_gui_smoke.bat
```

`build.bat` 默认构建生产 KDBX 后端。只验证编辑器、且不希望准备密码学依赖时，可先执行 `set NOTEPADSAFE_WITH_KDBX=OFF`。依赖身份及摘要来自 `third_party/dependencies.lock.json`；独立准备命令为：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_crypto_deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_pcre2.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify_supply_chain.ps1
```

生成便携 Windows 发布包（静态 Qt/CRT，包内含 GPLv3、第三方声明、依赖锁和 SPDX 2.3 SBOM）：

```bat
build_incr.bat portable_package
```

产物名为 `build\NotepadSafe-0.3.1-windows-x64.zip`。`portable_package` 只依赖正式程序和
SBOM，不会为了打包重复构建诊断工具或 GUI 测试；发布前仍须先执行完整门禁。

首次 bootstrap 需要在 x64 Visual Studio Developer PowerShell/Prompt 中运行；依赖缓存完整后的校验可在普通 PowerShell 中重复执行。

`Ctrl+N` 新建默认安全文档；`Ctrl+O` 是文本、二进制、安全文档和账户 KDBX 的统一入口。首次使用需要建立主密码并把恢复密钥保存到当前电脑之外；文件携带密码派生参数、密码槽和恢复槽，因此 AppData 或设备会话损坏后仍可直接凭文件加主密码/恢复密钥解锁。`Ctrl+Shift+L` 会先保存安全文档，再清除编辑器正文和进程内密钥并删除设备会话。工具栏“账户”进入中央账户工作区；账号密码复制后 30 秒自动清除（仅在剪贴板仍是该密码时执行）。
