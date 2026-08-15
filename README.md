# Notepad Safe

> Qt/QScintilla/Scintilla Windows 编辑器，目标产品为加密笔记本与账号保险库。
> 本仓库为独立产品仓库，clone 后可直接构建运行。

当前代码仍处于架构迁移期；生产保险库已经接入锁定的 KeePassXC 2.7.12、Botan 3.12、libargon2 与 zlib，支持创建 KDBX 4、解锁 KDBX 3/4、账号/加密笔记 CRUD 和原子保存。后续开发以 [docs/DESIGN.md](docs/DESIGN.md) 为唯一架构基线。

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

:: 4. 运行所有测试（可选）
run_tests.bat
```

`build.bat` 默认构建生产 KDBX 后端。只验证编辑器、且不希望准备密码学依赖时，可先执行 `set NOTEPADSAFE_WITH_KDBX=OFF`。依赖身份及摘要来自 `third_party/dependencies.lock.json`；独立准备命令为：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap_crypto_deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify_supply_chain.ps1
```

首次 bootstrap 需要在 x64 Visual Studio Developer PowerShell/Prompt 中运行；依赖缓存完整后的校验可在普通 PowerShell 中重复执行。

启动后可通过“保险库”菜单新建 KDBX，或用 `Ctrl+Shift+O` 打开 KeePassXC 兼容的 KDBX 文件；`Ctrl+Shift+L` 会立即锁定。解锁后的账号与加密笔记仅保留在进程内存中，保存走同目录临时文件和原子替换。账号密码可一键复制；30 秒后或保险库锁定时，仅在剪贴板仍包含该密码的情况下自动清除。
