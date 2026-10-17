# 文档索引

2026-10-02 按当前源码及未提交的保存/恢复修复重新整理。实现存在、测试存在、本轮执行通过是三个不同结论。

**先读 [产品目标与核心旅程](PRODUCT_GOALS.md)。** 它定义不变的目标、边界和 12 条必须跑通的核心旅程，是验收入口；下列文档是旅程失败或需要细节时的参考。当前Release仅为中间验证构建，核心旅程尚未执行。

## 当前文档

| 文档 | 职责 |
|---|---|
| [产品目标与核心旅程](PRODUCT_GOALS.md) | 一句话目标、不变边界、12 条核心旅程及其状态、完成标准 |
| [功能取舍](FEATURE_DECISIONS.md) | 新功能做/不做，现有低性价比功能的删除与简化 |
| [架构评估与建议](ARCHITECTURE_ASSESSMENT.md) | 现状架构问题、故障隔离思路、目标结构与迁移顺序 |
| 功能现状：[搜索](SEARCH_CURRENT_STATE.md)、[编辑](EDIT_CURRENT_STATE.md)、[编码](ENCODING_CURRENT_STATE.md)、[十六进制](HEX_CURRENT_STATE.md)、[大文件](LARGE_FILE_CURRENT_STATE.md)、[保存恢复](SAVE_RECOVERY_CURRENT_STATE.md)、[加密锁定](SECURITY_CURRENT_STATE.md)、[账号库](ACCOUNTS_CURRENT_STATE.md) | 按源码逐项记录的当前行为，及与新界面原型的差异 |
| [新界面功能核对](UI_FEATURE_MAP.md) | 现有功能在新界面中的位置 |
| [项目入口](../README.md) | 产品范围、构建入口、数据位置 |
| [功能清单](FEATURE_LEDGER.md) | 现有功能、模式适用性、能力限制 |
| [源码架构](DESIGN.md) | 真实依赖、状态所有权、持久化、安全及复杂度 |
| [回归方法](TARGET_EXPERIENCE.md) | 组合状态、操作序列、正确性判据和证据要求 |
| [详细验证清单](REGRESSION_CHECKLIST.md) | 128个主用例、参数夹具、17组外观/输入组合、45组功能映射及当前差距 |
| [统一回归计划](ITERATION_ROADMAP.md) | 128项的统一批次、下一步、构建时机、退出条件 |
| [唯一执行账本](DAILY_USE_CLOSEOUT.md) | 当前汇总、保存/恢复/副本改动的实际结果、EXE身份与历史记录 |
| [架构检查边界](ARCHITECTURE_CONVERGENCE.md) | 现有检查保护什么、不能证明什么 |
| [正则专项](REGEX_ENGINE_EXPLORATION.md) | PCRE2 与原始编码适配的语义和拒绝规则 |
| [供应链规程](SUPPLY_CHAIN.md) | 依赖锁、离线构建、摘要与更新 |

冲突时先查源码与可复现结果，再修正文档。正式回归必须绑定当前候选，历史报告不可继承为本轮通过。

## 历史证据，仅供追溯

- [第 1 轮 Hex](ROUND1_HEX_PRODUCT_ACCEPTANCE.md)
- [第 2 轮核心旅程](ROUND2_CORE_JOURNEYS_PRODUCT_ACCEPTANCE.md)
- [第 3 轮性能与恢复](ROUND3_PERFORMANCE_RECOVERY_ACCEPTANCE.md)
- [验证时间线](VALIDATION_TIMELINE.md)
- [早期静态审查](STATIC_CODE_AUDIT.md)

保留当时的样本、失败、测量与实现变化，不作为当前任务指令或整体验收结论。S2/S3、B2/B3 只用于识别历史夹具；当前大文件能力统一只读。

已移除未来 AI、代码可视化、分发的分轮计划及重复外部产品参考。原文可由 Git 历史找回；不因此改变现有代码、安全约束或测试入口。重新立项时须重新决定范围，不自动恢复旧路线。

测试目录的 [GUI 矩阵](../tests/gui_smoke/GUI_TEST_COVERAGE_MATRIX.md)、[Hex 契约](../tests/gui_smoke/HEX_EDITOR_CONTRACT_MATRIX.md)、[搜索契约](../tests/gui_smoke/SEARCH_REPLACE_CONTRACT_MATRIX.md)是专项索引；适用性应对照当前功能清单，列出用例不表示已运行。

本次整理已执行架构边界检查、异步线程边界检查、文档本地链接检查和 `git diff --check`，均通过。没有构建、运行产品测试或操作客户端；已有未提交的保存/恢复源码修改保持原状。
