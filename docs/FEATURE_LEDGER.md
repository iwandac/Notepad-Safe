# Notepad Safe 当前功能总账

> 基线：`9a80eec`（功能修复基线），架构迁移检查点从该 revision 向前比较。
> 规则：运行时所有有 objectName 的 QAction 必须由 `FeatureRegistry` 绑定本表中的稳定 FeatureId；未知命令使启动合同测试失败。删除、降级或改变下列能力必须先获得产品裁决。

| FeatureId | 当前用户能力（必须保留） | 主要命令/入口 | 回归证据族 |
|---|---|---|---|
| NSP-SHELL | 最小 Shell、工作区切换、会话渐进恢复、设置恢复、单实例外部打开 | `menu.*`、WorkspaceStack、StartupCoordinator | startup composition、startup E2E、session、settings |
| NSP-FILE | 新建默认安全草稿；统一打开 Text/Binary/Secure/Accounts KDBX；保存保持格式；另存为显式转换；关闭/退出提示 | `file.*`、`tabContext.*`、拖放、CLI、最近文件 | smallfixes、secure documents、vault、testfiles、session |
| NSP-EDIT | Undo/Redo、Cut/Copy/Paste、Select All、行复制/删除/移动/大小写/排序、缩进、补全、Hex 插入/覆盖 | `edit.*`、工具栏、快捷键、上下文 | coord editops、smallfixes、large readonly |
| NSP-SEARCH | 当前/全部标签查找与替换、上一处/下一处、单/多文件结果、byte 精确导航、书签 | `search.*`、`bookmark.*`、SearchResultsDock | find/replace、Dock compact quick sentinel、large-file encoded search |
| NSP-LANGUAGE | Plain 及当前语言语法呈现、扩展名猜测 | `language.*` | smallfixes/menu UI、session |
| NSP-VIEW | 系统/亮/暗/高对比、换行、分屏、符号、折叠、Text/Hex、标尺、缩放、面板/工具栏/状态栏 | `view.*`、状态栏 | smallfixes、coord、large readonly |
| NSP-ENCODING | 编码检测/强制显示、UTF/代码页转换、BOM、EOL 转换并经另存为保护 | `encoding.*` | textencoding、encoded search、testfiles、smallfixes |
| NSP-ACCOUNTS | Accounts 一级工作区、KDBX 账号 CRUD/TOTP/历史/回收站、Security Center、锁定、主密码与恢复密钥 | `tools.accounts`、`tools.securityCenter`、`tools.lockNow` | vault contract、KDBX backend、security identity、vault/secure GUI |
| NSP-TASKS | 状态栏任务、任务面板、进度、取消和安全串行队列 | `tools.tasks`、状态栏任务控件 | async architecture gate、搜索/安全 GUI journeys |
| NSP-HELP | About、版本和依赖信息 | `help.*` | smallfixes/menu UI、release package |

## 不回退判定

- “入口存在”不等于功能保留：文件内容/hash、KDBX 可重开、session 字段、undo、可见状态和耗时均需对应证据。
- GUI harness 必须链接与正式 EXE 相同产品 target；测试不能重复编译产品 `.cpp`。
- 历史回归哨兵固定进入 quick：`quick.search_dock_compact` 与 `quick.readonly_repeated_copy`。
- `testfiles` 八文件矩阵是本机验收 fixture，不清理、不提交；覆盖不同大小、Text/Binary 和 GBK/UTF-8/BOM/UTF-16LE/BE。
- KDBX OFF 只验证 editor/search/tools 不依赖具体后端；安全行为必须由 KDBX ON 验收。

