# Notepad Safe

> 基于 Qt 6.11 静态库 + QScintilla 2.14.1 的 Windows 加密笔记本编辑器。
> 本仓库为独立产品仓库，clone 后可直接构建运行。

详细说明见：[docs/README.md](docs/README.md)
设计文档见：[docs/DESIGN.md](docs/DESIGN.md)

## 快速开始

```bat
:: 1. 设置 Qt 静态库路径（或跳过使用默认路径 E:\programs\Qt_6.11.1-static-Release_msvc2026_64\msvc2026_64）
set NOTEPADSAFE_QT6_DIR=你的Qt静态库路径\msvc2026_64

:: 2. 一键构建
build.bat

:: 3. 运行
build\notepad-safe.exe

:: 4. 运行所有测试（可选）
run_tests.bat
```
