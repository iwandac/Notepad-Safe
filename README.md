# Notepad Safe

Windows 本地的轻量记事本，带离线加密文档和 KDBX 账号库。用 C++17、Qt 6.11.1 静态库（/MT）和 QScintilla 构建，单个 exe 运行，不联网。版本 0.3.1，开发中。

先读 **[产品目标与核心旅程](docs/PRODUCT_GOALS.md)**：不变的目标、边界和 12 条必须跑通的核心旅程。其余文档见 [文档索引](docs/README.md)。

## 能做什么

- **编辑**：Notepad++ 式的文本编辑——行操作、多选、语法高亮、折叠、书签、标尺；查找替换支持普通、PCRE2 正则（捕获组替换）和十六进制字节三种模式，可在当前文件或全部打开文件中查找（替换只作用于当前文件）。
- **编码与换行**：自动识别 UTF-8/UTF-16/GBK/GB18030/Big5/Shift-JIS/Windows-1252，可按其他编码显示或转换另存；CRLF/LF/CR 可切换。
- **二进制**：小于 32 MiB 的二进制文件以可编辑的十六进制视图打开（插入/覆盖、剪贴板、撤销）。
- **大文件**：32 MiB 及以上的文件以窗口化只读视图打开，可浏览、复制、查找、跟随文件末尾；大文本可在标签右键切换为十六进制查看。
- **加密文档**：新建默认是离线加密文档（`.nsafe`，KDBX 格式），自动写回、锁定后遮蔽内容；本机会话 7 天，可立即锁定。
- **账号库**：标题、用户名、密码、网址、标签、备注和动态验证码（TOTP）；复制的用户名、密码和验证码 30 秒后从剪贴板清除。
- **保存与恢复**：原子保存；未保存的修改以加密草稿保存，崩溃或重启后恢复。

界面：无边框窗口与标题栏，≡ 主菜单（Alt / F10 打开），Ctrl+K 命令面板，可点击的状态栏（编码、换行符、语言、缩放、安全状态），浅色/深色外观，Windows 高对比度下跟随系统配色。

各功能的详细现状见 `docs/*_CURRENT_STATE.md`。

## 构建

需要 MSVC x64 工具链（Visual Studio 2026）、CMake ≥ 3.22、Python 3，以及**精确版本的 Qt 6.11.1 静态 /MT 构建**（必须保持静态链接）。依赖源码已锁定在 `third_party/src`，离线构建。

```bat
set NOTEPADSAFE_QT6_DIR=你的Qt静态库目录\msvc2026_64
build.bat
build\notepad-safe.exe
```

- `build.bat`：先运行 `prepare_dependencies.bat`（离线校验并构建 QScintilla、PCRE2、Botan 到 `.deps`，首次较慢），再重建 `build` 目录（Ninja，KDBX 开启，测试关闭）。
- `build_incr.bat [目标...]`：增量构建。用 `NOTEPADSAFE_BUILD_DIR` 指定已有目录时沿用该目录的生成器和缓存选项；新目录用 Ninja。
- `NOTEPADSAFE_WITH_KDBX=OFF` 构建不含加密后端的普通编辑器。

依赖锁定与更新规程见 [供应链规程](docs/SUPPLY_CHAIN.md)。

## 测试

- `run_tests.bat`：构建并运行核心测试（不含 `gui_smoke_*`）。其中部分测试仍会创建 Qt 应用对象。
- `run_all_gui_smoke.bat`：构建并运行 GUI 测试，**会在桌面上弹出真实窗口**，运行期间不要操作桌面，否则焦点相关用例可能失败。
- 静态 Qt 只带 Windows 平台插件，不要设置 `QT_QPA_PLATFORM=offscreen`。
- 每次运行都用独立的 `NOTEPADSAFE_DATADIR`（以及 `TEMP`/`TMP`），不要碰个人资料。
- 构建时自动运行 `tools/check_architecture.ps1` 和 `tools/check_async_threading.ps1`，违反依赖规则或复杂度预算即构建失败。

当前构建身份、测试结果和尚未关闭的问题见 [执行账本](docs/DAILY_USE_CLOSEOUT.md)。

## 数据位置

数据目录：环境变量 `NOTEPADSAFE_DATADIR`（绝对路径）优先，否则为 Qt GenericDataLocation 下的 `NotepadSafe`。普通文档的未保存修改用 Windows DPAPI 加密保存为草稿；加密文档和账号库以 KDBX 格式保存。

## 许可证

见仓库许可证文件；第三方依赖的许可与归属见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
