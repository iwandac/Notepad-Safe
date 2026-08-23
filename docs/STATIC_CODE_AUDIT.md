# 静态代码与构建边界审查

审查日期：2026-08-18

## 结论

默认产品构建现在只生成 `notepad-safe`、其锁定供应链静态库以及发布/SBOM 目标。单元测试、
GUI smoke、E2E、诊断程序和已禁用实验均不再进入默认 compile/link 图。被禁用实现集中保存在
`archive/disabled/`，该目录没有 CMake、CTest 或批处理脚本入口。

这次结论是“构建边界已收敛”，不是“全部产品行为已经成熟”。`VirtualFileView` 和
`PatchList` 仍是 B1 小型二进制/Hex 编辑与安全二进制内存载荷的活动实现，不能因为旧 S2
实验被归档而删除；S2/S3/B2/B3 则只走 `WindowedReadOnlyProvider`。

## 构建图整改

- 新增 `NOTEPADSAFE_BUILD_TESTS=OFF`：单元、集成、E2E 和 GUI smoke 显式选择后才生成。
- 新增 `NOTEPADSAFE_BUILD_DIAGNOSTICS=OFF`：诊断目标还要求 tests 同时开启，否则配置失败。
- `Qt6::Test` 只在 tests 开启时查找，产品配置不再携带测试组件依赖。
- `build.bat` 固定关闭 tests/diagnostics，并只构建 `notepad-safe`。
- `run_tests.bat` 显式开启 tests，只构建 `notepadsafe_non_gui_tests` 聚合目标并运行非 GUI CTest，
  不再重复逐个运行一次后又跑 CTest；运行目录与 `NOTEPADSAFE_BUILD_DIR` 保持一致。
- `run_all_gui_smoke.bat` 显式开启 tests 和 KDBX，并覆盖当前六个 GUI smoke 目标；移除了失效的
  绝对路径，运行目录也与 `NOTEPADSAFE_BUILD_DIR` 保持一致。
- `diag_lineindex` 与 `diag_scroll_lines` 是仍有价值的开发诊断，但仅在两个开关都开启时生成。

## 已归档、未参与编译的代码

| 归档 | 原用途 | 处置依据 |
|---|---|---|
| `archive/disabled/s2-editing/test_largefile.cpp` | 旧 2,000+ 行 L2 可编辑/补丁/后台装载综合测试 | 多项断言与当前 S2/S3 永久只读策略冲突 |
| `archive/disabled/s2-editing/diag_editops.cpp` | 大文件 PatchList 编辑/保存诊断 | 不属于现行 B1 Hex 验收 |
| `archive/disabled/s2-editing/diag_gui_l2_editops.cpp` | 228 MiB GUI 编辑实验 | 会制造 S2 可编辑的错误信号 |
| `archive/disabled/s2-editing/diag_save_l2_chain.cpp` | L2 patch 保存链实验 | 现行大文件没有保存修改链 |
| `archive/disabled/s2-editing/L2_EDITOR_GUI_SMOKE_TEST_CASES.md` | 旧可编辑 L2 GUI 矩阵 | 含已失效路径、预期和已知失败 |
| `archive/disabled/legacy-virtual-file-view/` | 同步全量索引和模态进度兼容入口 | 只有被归档测试调用，生产异步路径不使用 |
| `archive/disabled/async-task-types/` | 预留的加密/解密/大文件保存任务类型 | 没有生产提交者，容易被误认为已实现异步能力 |

## 已修复的活动逻辑问题

1. `AsyncThreadPool` 的排队计数只增不减，且所谓 Highest 保留槽实际从不拒绝任务，返回值也
   被唯一调用者忽略。现已删除伪队列/保留槽接口，直接映射到 `QThreadPool` 原生优先级。
2. `AsyncTaskManager` 原先在 worker 取得线程之前就把任务标成 Running。高负载时排队任务会
   假装运行，取消也会误走运行中分支。现在 worker 真正开始时才排队发送 Running 状态。
3. `VirtualFileView` 中无赋值路径的旧 `QFuture<LineIndex>` / watcher、等待和析构代码已移除；
   行索引只由 `AsyncTaskManager` 路径拥有。
4. 仅供旧测试使用的 `openFileSync`、永远警告但返回成功的
   `buildLineIndexWithProgress`、未使用的 progress parent 参数及无用 include 已从产品类移除。
5. `VirtualFileView`/`EditorTab` 顶部仍宣称 S2 可提升编辑的旧注释已改为当前 B1 与
   S2/S3/B2/B3 边界，减少后续维护误判。
6. 测试脚本原先在设置自定义构建目录后仍固定从 `build/` 运行程序，可能测试旧二进制；GUI
   全套脚本也没有保证 KDBX 目标被配置。现已统一构建/运行目录并显式启用所需后端。

## 保留但需要继续关注

- 2026-08-22 调度收口已取代 2026-08-18 的固定 10 worker 方案：应用总 worker 上限为
  `clamp(逻辑核心数 - 1, 2, 12)`，其中包含一个串行安全 lane；`LineIndex` 不再拥有私有池，
  批扫描由中央池执行。任务中心显示应用自有后台 worker 总占用；操作系统、Qt GUI/IO 内部线程
  不属于该工作队列统计。
- `L2Presentation` 等历史类型名因会话/API 兼容暂时保留，但注释和 capability 已明确：名称
  不代表 S2 可编辑或后台晋升。
- 静态审查无法替代真实 GUI 动态验收；本批没有据此宣称滚动、Hex、搜索或加密交互成熟。

## 验证证据

- KDBX=ON、tests=OFF、diagnostics=OFF 的 MSVC/NMake Release 产品在隔离目录完成构建和链接；
  验证结束后清理该构建目录，不把审查产物留在仓库。
- 默认 target help 中没有 `test_*`、`gui_smoke_*`、`e2e_*`、`diag_*` 或归档目标。
- 显式 tests=ON 后，`test_search` 32/32、`test_session` 59/59 通过，
  `test_filetierpolicy` 退出码 0。
- `test_session` 在受限沙箱的 offscreen QApplication 初始化会停滞；同一二进制在 Windows
  桌面平台正常完成，因此记录为运行环境差异，不计作产品失败或虚假通过。
