# Notepad Safe

Windows 本地文本编辑器、二进制查看/编辑器与 KDBX 账号保险箱。基于 C++17、Qt 6.11.1 静态库和 QScintilla。版本 0.3.1，当前仍为待完整回归的开发候选。

本版目标：个人日用编辑与恢复、只读大日志定位、账号取用与锁定。冻结新功能，延期 AI、代码可视化和 Hex 扩展；安装、签名、发布和分发不在范围内。

产品目标、不变边界和必须跑通的 12 条核心旅程见 **[产品目标与核心旅程](docs/PRODUCT_GOALS.md)**，这是验收入口；细化子例见[详细验证清单](docs/REGRESSION_CHECKLIST.md)。下列能力是源码现状，不能自动作为体验差异的验收豁免。

## 当前能力

- 普通小文本：完整编辑、查找替换、编码/行尾、语法高亮、书签和分屏。
- 小型二进制：字节编辑、Text/Hex 表示、撤销重做；其 Text 投影不能套用普通文本全部编辑能力。
- 普通文件达到 32 MiB 后统一窗口化只读：浏览、搜索、定位、复制；不编辑、不普通保存/另存、不启用自动换行。只读普通 Text 仍可转换编码并输出新文件，这是独立操作。
- **2 GiB 不是功能门槛**。源码残留的大小枚举不产生另一套能力。
- KDBX 后端开启时，新建默认安全草稿，安全正文载荷严格小于 32 MiB。关闭后端的构建可新建普通草稿，不能提供安全存储。
- 安全文档与账号共用底层 KDBX 能力，但账号秘密不进入普通文档编辑/搜索路径。
- AI 只有接口和注册表，没有接通的推理产品功能。

以上表示源码已有路径，不代表完整客户端回归通过。适用模式和限制见[功能清单](docs/FEATURE_LEDGER.md)。

## 构建与运行

需要 MSVC x64 C++ 工具链、CMake ≥3.22、Ninja、Python 3，以及精确 Qt 6.11.1 静态 /MT 工具链。应用依赖源码由仓库锁定并离线构建；不是仅 clone 即可运行。

```bat
set NOTEPADSAFE_QT6_DIR=你的Qt静态库目录\msvc2026_64
build.bat
build\notepad-safe.exe
```

`build.bat` 会重建 build 目录，默认 KDBX=ON、Tests/Diagnostics=OFF。已有构建增量使用 `build_incr.bat notepad-safe`，可用 NOTEPADSAFE_BUILD_DIR 指定既有目录。NOTEPADSAFE_WITH_KDBX=OFF 可构建普通编辑器。详见[供应链规程](docs/SUPPLY_CHAIN.md)。

测试入口为 run_tests.bat 和 run_all_gui_smoke.bat。前者虽然标为 non-GUI，仍包含使用 QApplication 的测试，**不能视为不占桌面的白名单**。执行前按[回归方法](docs/TARGET_EXPERIENCE.md)核对入口与隔离方式。

## 数据与当前状态

数据根由 src/domain/AppDataLocation.cpp 决定：绝对路径环境变量 NOTEPADSAFE_DATADIR 优先，否则为 Qt GenericDataLocation/NotepadSafe。测试使用隔离 profile 和合成数据，不复用个人资料。

普通修改稿由 DraftStore 使用 Windows DPAPI 保存；安全文档由安全持久化路径保存。恢复与原子保存存在失败边界，不能承诺任何崩溃或并发写入下绝不丢失。损坏会话清单保全流程及候选证据见[收尾记录](docs/DAILY_USE_CLOSEOUT.md)。

当前[源码架构](docs/DESIGN.md)、[收尾计划](docs/ITERATION_ROADMAP.md)、[文档索引](docs/README.md)。许可证和依赖归属见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 及仓库许可证文件。
