# 文档索引

更新：2026-10-17。

**先读 [产品目标与核心旅程](PRODUCT_GOALS.md)。** 它定义不变的目标、边界和 12 条必须跑通的核心旅程，是验收入口；下列文档是需要细节时的参考。冲突时以源码与可复现结果为准，再修正文档。

## 方向

| 文档 | 内容 |
|---|---|
| [产品目标与核心旅程](PRODUCT_GOALS.md) | 一句话目标、不变边界、12 条核心旅程 |
| [功能取舍](FEATURE_DECISIONS.md) | 新功能做/不做，低性价比功能的删除与简化，以及实现进度 |
| [架构评估](ARCHITECTURE_ASSESSMENT.md) | 架构问题、故障隔离思路、目标结构、迁移顺序、现有架构检查 |
| [新界面功能核对](UI_FEATURE_MAP.md) | 每项现有功能在新界面中的位置与实现进度 |

## 功能现状

按源码逐项记录的当前行为：[搜索](SEARCH_CURRENT_STATE.md)、[编辑与视图](EDIT_CURRENT_STATE.md)、[编码与换行](ENCODING_CURRENT_STATE.md)、[十六进制](HEX_CURRENT_STATE.md)、[只读大文件与跟随](LARGE_FILE_CURRENT_STATE.md)、[保存与恢复](SAVE_RECOVERY_CURRENT_STATE.md)、[加密与锁定](SECURITY_CURRENT_STATE.md)、[账号库](ACCOUNTS_CURRENT_STATE.md)。

正则的匹配与替换语义（含非 UTF 编码的子集规则）见 [正则语义](REGEX_SEMANTICS.md)。

## 实现与工程

| 文档 | 内容 |
|---|---|
| [源码架构](DESIGN.md) | 模块结构、依赖、状态所有权、持久化与安全 |
| [供应链规程](SUPPLY_CHAIN.md) | 依赖锁定、离线构建、摘要与更新 |
| [执行账本](DAILY_USE_CLOSEOUT.md) | 当前构建身份、测试结果、已知限制、尚未关闭的问题 |

测试索引在 `tests/gui_smoke`：[GUI 矩阵](../tests/gui_smoke/GUI_TEST_COVERAGE_MATRIX.md)、[十六进制契约](../tests/gui_smoke/HEX_EDITOR_CONTRACT_MATRIX.md)、[搜索契约](../tests/gui_smoke/SEARCH_REPLACE_CONTRACT_MATRIX.md)。列出用例不表示已运行；运行结果见执行账本。

早期的验收轮次记录、128 项回归清单、迭代路线和已完成的实施计划已移除，可在 Git 历史中找回。
