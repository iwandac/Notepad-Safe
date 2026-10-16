# 当前架构检查边界

更新：2026-10-02。取代旧重构阶段计划。实际拓扑见 [DESIGN.md](DESIGN.md)，这里仅说明已有检查能力。

## 检查入口

| 入口 | 保护内容 | 不能证明 |
|---|---|---|
| tools/check_architecture.ps1 | CMake 重复编译、部分禁用依赖、装配/持久化/搜索所有权、复杂度预算 | 完整 C++ 依赖图、状态一致性或客户端正确性 |
| tools/check_async_threading.ps1 | 应用线程/线程池创建点与允许运行时边界 | 取消、映射释放、重入或退出安全 |
| tests/test_architecture_contracts.cpp | 正式组件接口、注册表及命令等合同 | 真实工具/AI 已接通或正式 EXE 通过 |
| tests/test_startup_composition.cpp | 装配与延迟实例化 | 首屏性能分布与实际输入响应 |

```powershell
powershell -NoProfile -File tools/check_architecture.ps1
powershell -NoProfile -File tools/check_async_threading.ps1
```

两个脚本不启动客户端；测试目标仍须核对 QApplication/窗口和隔离方式。

## 维护边界

- 产品源编译进正式运行时库，测试链接这些库，不复制实现。
- AppComposition 选择具体提供器和适配器，主窗口通过端口访问持久化/安全服务。
- SessionManager 通过 ISessionDocumentPort 捕获/恢复，不依赖 EditorTab/Widgets。
- DocumentSession 持有身份、revision、位置与安全元数据；dirty 和 capabilities 仍由 EditorTab 聚合，不宣称已完全领域化。
- VirtualFileView/PatchList 仍服务小型二进制与安全二进制，不能按名字当旧实验删除。
- 大文件全部只读；S2/S3 枚举与 L2 名称不代表另一编辑档位。
- AsyncTaskManager::instance() 仍是共享单例。专用线程归入既有调度，不宣称全局运行时已消失。

## 复杂度预算

非空行、if 和方法数是防回退信号，不是语义复杂度证明。MainWindow 当前已接近聚合行数预算，分支/方法也接近上限；搬文件、压缩代码或提高阈值不能自动说明架构改善。

本版不安排全面重构，只在确定缺陷修复需要时调整职责，以缺陷及相邻状态转换验证。架构检查通过不能替代功能组合回归。
