# Notepad Safe

> 基于 Qt 6.11 静态库 + QScintilla 2.14.1 的 Windows 加密笔记本编辑器。
> 本仓库为**独立产品仓库**，clone 后可直接构建运行。
> （原 Credential Vault 项目中的 `apps/qt-notepad` 已独立为该仓库，并统一重命名为 **Notepad Safe**）

---

## 1. 特性速览

- ✅ **单 exe**：静态构建，无 DLL 依赖，分发即用
- ✅ **多标签页** + 会话自动恢复（崩溃/退出不丢未保存内容）
- ✅ **编码检测**（UTF-8 / UTF-16 LE/BE / GBK，BOM + 启发式）
- ✅ **原子保存**（.tmp → rename，防止崩溃损坏原文件）
- ✅ **按需滚动条**（内容未超出窗口时不显示空滚动条）
- ✅ **设置 ↔ 菜单勾选强同步**（重启后自动换行等菜单勾选与实际行为一致）
- ✅ **查找 / 正则替换 / 全部替换**，结果 Dock 可虚拟化跳转
- ✅ **语法高亮**：C++/Python/JSON/HTML/SQL/XML/YAML/Markdown/纯文本
- ✅ **书签 / 行排序 / 复制行 / 删除行 / 上下移行**
- ✅ **深色模式**（跟随系统 + 手动切换）
- ✅ **大文件分块加载**（进度条 + 不卡 UI；≥ L2 方案见 docs/DESIGN.md §5）
- ✅ **产品图标**（根目录 `notepad_safe.png` → EXE 图标 + 窗口运行时图标）
- ✅ **全自动化测试**：CTest 5 个测试套件覆盖编码/搜索/会话/设置同步/大文件

## 2. 构建

### 2.1 前置条件（一次性准备）

| 组件 | 说明 |
|---|---|
| MSVC 工具链 | 安装 VS 2022 及以上，勾选"使用 C++ 的桌面开发" |
| Qt 6.11.x 静态库（MSVC 64-bit，/MT 构建） | 需自行编译静态版 Qt（推荐路径 `E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64`） |
| CMake ≥ 3.22 | 或 Visual Studio 自带 |
| Ninja | 或 Visual Studio 自带 |
| Python 3（可选） | 用于需要用 `resources/make_ico.py` 重新生成图标时 |

> QScintilla 2.14.1 的源码和已构建静态库 `qscintilla2_qt6.lib` 已内置在本仓库 `third_party/`，
> 无需用户再下载构建。

### 2.2 设置 Qt 路径

把 Qt 静态库路径写入环境变量（推荐；不写入也可用默认路径 `E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64`）：

```bat
setx NOTEPADSAFE_QT6_DIR "你的Qt静态库路径\msvc2026_64"
:: 之后重开终端生效，或当前会话 set 临时生效
```

### 2.3 一键构建

```bat
cd notepad-safe
build.bat
```

脚本内部流程：
1. 调用 `vcvars64.bat`（若未在 VS x64 Native Tools 环境）
2. `cmake -G Ninja -S . -B build`（QScintilla 相对路径；Qt 路径从环境变量/缓存）
3. `cmake --build build -j`
4. 产出：`build\notepad-safe.exe`（单文件 ~24MB）

### 2.4 运行所有测试

```bat
run_tests.bat
```

5 个 CTest 目标：`textencoding` / `search` / `session` / `settings_ui` / `largefile`。
全部通过即视为构建与行为合格。

## 3. 使用

直接双击 `build\notepad-safe.exe` 或命令行：

```bat
build\notepad-safe.exe            :: 启动空白窗口
build\notepad-safe.exe a.txt      :: 启动并打开指定文件（支持多文件空格分隔）
```

**图标说明**：
- EXE 文件图标 = `resources/app.ico`（16/32/48/64/128/256 六尺寸，Windows 资源管理器使用）
- 窗口 / 任务栏图标 = `notepad_safe.png`（通过 `resources/app_resources.qrc` 内嵌，运行时 setWindowIcon）

**数据目录**：
- 会话 + 自动保存：`%LOCALAPPDATA%\NotepadSafe\session.json` / `autosave\*.tmp`
- 设置：`HKEY_CURRENT_USER\Software\NotepadSafe\NotepadSafe`（或等效 ini，由 Qt QSettings 决定）

> 为与原 Credential Vault 产品隔离，独立后 Notepad Safe 不读取/写入任何
> `CredentialVault` 名下的设置或文件。

## 4. 目录概览

```
notepad-safe/
├── CMakeLists.txt              # 主目标 notepad-safe + 5 个 CTest
├── build.bat / run_tests.bat   # 构建 & 测试脚本（相对脚本自身路径）
├── main.cpp / MainWindow.*     # 主程序入口 & 主窗口
├── EditorTab.*                 # 单个编辑器标签（QsciScintilla）
├── TextEncoding.*              # 编码检测 / 转换
├── FindReplaceDialog.*         # 查找替换对话框
├── SearchResultsDock.*         # 搜索结果 Dock
├── SessionManager.*            # 会话 & 自动保存
├── notepad_safe.png            # 原始图标（1254×1254，产品根）
├── resources/
│   ├── notepad_safe.png        # 运行时 PNG（app_resources.qrc 引用）
│   ├── app.ico                 # 6 尺寸 ICO（EXE 图标）
│   ├── app_icon.rc             # RC：IDI_APPLICATION + VS_VERSION_INFO
│   ├── app_resources.qrc       # QRC：运行时图标
│   └── make_ico.py             # 从 notepad_safe.png 重新生成 app.ico（辅助）
├── third_party/
│   └── QScintilla_src-2.14.1/  # QScintilla 源码 + release/qscintilla2_qt6.lib
├── tests/                      # 5 个测试套件
└── docs/
    ├── README.md               # 用户文档（本文件）
    └── DESIGN.md               # 设计文档（模块/方案/验收标准）
```

## 5. 设计文档 & 大文件方案

详见 [docs/DESIGN.md](docs/DESIGN.md)：

- 模块设计（§3-§4）
- **大文件 L1/L2 分级架构方案**（§5），包含：
  - 动态阈值（RAM/CPU 探测，无硬性人为上限）
  - `VirtualFileView` 自绘虚拟视图 + `LineIndex` delta 压缩
  - `PatchList` L1/L2 分级溢出 + UndoGroup 原子分段
  - `LargeFileSearch` 多线程分块 + Dock 虚拟化结果溢出
  - `replaceAll` 分段原子 + 中间保存提示 + 保存后继续（`applyAndSaveThenReset`）
  - 量化验收表（8GB / 32GB 两档明确数值）
- 构建配置（§6）
- 测试策略（§7）
- 变更日志（§8）

## 6. 许可证 & 第三方

- Notepad Safe 代码：**GPL v3**（与 QScintilla 许可证一致）
- Qt：根据您的 Qt 许可证（GPL / LGPL / 商业）
- QScintilla 2.14.1：GPL v3，源码位于本仓库 `third_party/QScintilla_src-2.14.1`
