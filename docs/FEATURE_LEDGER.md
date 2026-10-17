# 当前功能与回归边界

本表于 2026-10-02 按当前工作树源码重建，包含尚未提交的保存与恢复收尾修改。它记录**实现了什么、在什么状态下可用、还要验证什么**；不是已通过的验收报告。本轮未构建、未运行测试、未操作客户端。历史版本的通过数量不能作为当前工作树的通过数量。

命令清单以 [`FeatureRegistry`](../src/domain/FeatureRegistry.cpp) 为入口，实际可用性还必须同时核对 [`CommandRegistry::applyContext`](../src/app/CommandRegistry.cpp)、[`MainWindow::syncCommandStates`](../src/window/MainWindowCommands.cpp) 和具体执行函数。注册了 QAction 不等于能力正确，菜单启用也不等于底层允许执行。

本表的“限制”是实现现状，不代表用户已认可其体验。2026-10-02 最新验收主线是轻快日用、离线加密安全和只读共同阅读体验一致；具体步骤、现状差距与45组功能映射见 [详细验证清单](REGRESSION_CHECKLIST.md)。未支持的阅读能力不能仅因当前被禁用就计为通过。

## 1. 当前运行模式

普通文件按 `max(源字节数, 可获得的 UTF-8 大小估计)` 分类；实际打开还会复核文件和读取授权。**有效大小小于 32 MiB 才进入普通编辑档；等于或大于 32 MiB 统一只读。**`FileTierPolicy` 仍有 2 GiB 分类常量和 `LargeReadOnly/HugeReadOnly` 枚举，但二者具有相同能力、相同窗口化打开路径，2 GiB 不是现行功能门槛。超大偏移仍需测试，但不应建立第二套功能承诺。

| 模式 | 内容与表示 | 实际能力 | 限制与源码 |
|---|---|---|---|
| 空工作区 | 没有文档 | 打开、新建、工作区切换、设置、任务等应用操作 | 启动不隐式创建文档或安全身份；见 [`StartupCoordinator`](../src/app/StartupCoordinator.cpp)、[`MainWindow`](../src/window/MainWindow.cpp) |
| 普通小文本 | QScintilla 完整文本 | 编辑、搜索替换、撤销、语法、普通/安全另存 | 编辑过程中增长到保存快照上限附近需单独回归，不能只测打开时大小；见 [`EditorTabPersistence`](../src/editor/EditorTabPersistence.cpp) |
| 普通小二进制，Hex 表示 | VirtualFileView + PatchList + Hex/字符区投影 | 字节输入、插入/覆盖、删除、剪贴板、撤销、搜索、保存 | 不提供普通文本行操作、语法、搜索替换，不能新建为安全文档；见 [`EditorTabHex`](../src/editor/EditorTabHex.cpp) |
| 普通小二进制，Text 表示 | 原始字节的解码投影 | 浏览、复制、搜索、切回 Hex；仍可保存既有字节模型 | **Text 投影只读**，不能把解码后的替代字符当作可逆字节编辑；见 [`bridgePopulateScintilla`](../src/editor/EditorTabLargeDocument.cpp) |
| 普通只读文件 | WindowedReadOnlyProvider 的 Text 或 Hex 表示（Binary 只用 Hex；Text 可在标签右键切到 Hex 查看） | 窗口化浏览、全局字节定位、跨窗口选区、复制、查找、统计、Tail | 无编辑、普通保存/另存、自动换行、语法呈现；只读 **Text** 仍有“转换编码并另存新文件”；见 [`openLargeFileL2`](../src/editor/EditorTabLargeDocumentPersistence.cpp) |
| 安全文本/安全二进制 | 解锁后的文本编辑或字节模型 | 编辑、搜索、加密保存、加密恢复、显式另存转换 | 有效载荷必须 **小于 32 MiB**，不会降级为普通只读安全大文件；见 [`SecureDocumentService`](../src/vault/SecureDocumentService.cpp) |
| 安全文档已锁定/切换中 | 遮蔽或切换状态 | 显式解锁、关闭等生命周期操作 | `documentState()` 返回空内容能力；不允许把缓存正文继续用于搜索、复制或工具；见 [`EditorTab::documentState`](../src/editor/EditorTab.cpp) |
| Accounts 工作区 | 独立 KDBX 账号条目 | 账号编辑、搜索、取用、锁定等 | 不作为普通文档参与全标签搜索、文本替换或文档工具；见 [`VaultDock`](../src/vault/VaultDock.cpp) |

新建行为与构建能力有关：安全服务可用时创建加密草稿并要求安全会话；不可用时创建普通未命名标签。KDBX OFF 是编辑器独立构建能力，不具有 KDBX ON 的安全功能。依据：[`MainWindow::newFile`](../src/window/MainWindowDocuments.cpp)、[`AppComposition`](../src/app/AppComposition.cpp)。

## 2. 源码功能清单

下面的 `NSP-*` 对应现有 `FeatureRegistry`。每行是一个需要独立追踪边界的功能组；最后一栏是回归要求，不表示已经执行或通过。

| FeatureId / 功能组 | 实现入口与适用状态 | 必须覆盖的操作边界 |
|---|---|---|
| NSP-SHELL：启动与单实例 | [`main`](../src/main.cpp)、[`SingleInstanceCoordinator`](../src/app/SingleInstanceCoordinator.cpp)、[`StartupCoordinator`](../src/app/StartupCoordinator.cpp) | 空配置、已有会话、带路径启动、第二进程、原进程忙/失联、首帧后恢复；不要用测试窗口启动替代正式进程 |
| NSP-SHELL：工作区 | [`MainWindow`](../src/window/MainWindow.cpp) 的文档、Accounts、安全中心 | 菜单/快捷键随当前工作区切换；辅助工作区中接到外部普通打开请求时的焦点和现有编辑保留 |
| NSP-SHELL：设置与外观 | [`MainWindowCommands`](../src/window/MainWindowCommands.cpp)、[`MainWindowSession`](../src/window/MainWindowSession.cpp) | 亮/暗/系统模式、系统高对比、重启恢复、菜单勾选同步、窄窗口和 DPI；高对比由系统状态驱动 |
| NSP-FILE：新建与统一打开 | [`MainWindowDocuments`](../src/window/MainWindowDocuments.cpp)、[`MainWindowOpenRouting`](../src/window/MainWindowOpenRouting.cpp)、[`DocumentOpenWorkflow`](../src/app/DocumentOpenWorkflow.cpp) | 空文件、文本/二进制识别、编码扩张、`.nsafe`/Accounts KDBX、取消凭据、已打开同一物理文件、原文件替换 |
| NSP-FILE：外部打开 | [`MainWindowExternalOpen`](../src/window/MainWindowExternalOpen.cpp) | CLI/拖放/单实例请求、目录/缺失项/有效项混合、失败反馈、已有 dirty 标签复用；外部安全文件请求不自动触发解锁 |
| NSP-FILE：最近文件与标签菜单 | [`MainWindowCommands`](../src/window/MainWindowCommands.cpp) | 最近文件去重/清空/缺失反馈；保存、另存、关闭其他/全部、复制路径、资源管理器定位；未命名草稿不暴露内部恢复路径 |
| NSP-FILE：普通保存 | [`PlainFilePersistence`](../src/app/PlainFilePersistence.cpp)、[`EditorTabPersistence`](../src/editor/EditorTabPersistence.cpp) | 文本编码、BOM、原始字节补丁、快照上限、不可表示字符、外部修改、写入/提交/重开失败；核对实际字节和 dirty 状态 |
| NSP-FILE：另存与格式转换 | [`SaveAsDialog`](../src/app/SaveAsDialog.cpp)、[`MainWindow::saveAsFile`](../src/window/MainWindowDocuments.cpp) | 未加密/安全两种类型、后缀、取消、目标已存在/已打开、安全转明文提示、失败后保留原身份和内容 |
| NSP-FILE：重载与关闭 | [`MainWindowDocuments`](../src/window/MainWindowDocuments.cpp)、[`MainWindowSession`](../src/window/MainWindowSession.cpp) | dirty 保存/放弃/取消，保存失败后重试，关闭当前/其他/全部，活动搜索或恢复中关闭，不误关邻近标签 |
| NSP-SHELL：会话恢复 | [`SessionManager`](../src/session/SessionManager.cpp)、[`DraftStore`](../src/session/DraftStore.cpp)、[`WorkspaceRecoveryCoordinator`](../src/window/WorkspaceRecoveryCoordinator.cpp) | clean 路径与加密未保存稿区分、标签顺序/当前项、分段恢复中用户操作、重试/移除/放弃、快照失败保留旧恢复资料 |
| NSP-SHELL：异常清单保全 | [`WorkspaceRecoveryCoordinator`](../src/window/WorkspaceRecoveryCoordinator.cpp)、[`SessionManager`](../src/session/SessionManager.cpp) | 损坏/未知版本/不可读清单；默认取消；显式保全选择、复制校验失败、重试、保全后当前会话写入失败；不能把错误当空会话 |
| NSP-EDIT：文本输入与历史 | [`EditorTab`](../src/editor/EditorTab.cpp)、[`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 输入、删除、选区覆盖、Unicode/IME、多行粘贴、撤销/重做分组、保存点、活动分屏；适用普通/解锁安全文本 |
| NSP-EDIT：剪贴板 | [`DocumentClipboardCommands`](../src/editor/DocumentClipboardCommands.cpp)、[`ByteClipboard`](../src/editor/ByteClipboard.h)、[`MainWindowCommands`](../src/window/MainWindowCommands.cpp) | 文本与 Hex/字符区焦点、反向/跨窗口选区、内部原始字节 MIME 和对外文本、重复复制、64 MiB 字节载荷边界、安全剪贴板清理 |
| NSP-EDIT：行操作 | [`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 复制、删除、上移/下移、合并；首末行、无末尾换行、CR/LF/CRLF、多行选区及一次撤销 |
| NSP-EDIT：排序与大小写 | [`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 升/降序、Unicode 大小写、空行、重复行、选区端点、换行保真、活动分屏；排序实现须重点复核，见第 4 节 |
| NSP-EDIT：多选 | [`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 选择下一个/所有相同内容、空选区、重叠匹配、循环末端、修改与撤销；仅原生可编辑文本 |
| NSP-EDIT：缩进与补全 | [`MainWindow`](../src/window/MainWindow.cpp)、[`EditorTab`](../src/editor/EditorTab.cpp) | Tab/空格、宽度 2/4/8、Backspace 回退缩进、文档词语补全；设置在新标签/分屏/只读模式中的应用 |
| NSP-EDIT：Hex 输入 | [`EditorTabHex`](../src/editor/EditorTabHex.cpp)、[`PatchList`](../src/editor/PatchList.cpp) | 高/低半字节输入、字符区输入、插入/覆盖、EOF 追加、Delete/Backspace、完整选区替换、无效输入、IME 不破坏字节网格 |
| NSP-VIEW：Text/Hex 表示 | [`EditorTabLargeDocument`](../src/editor/EditorTabLargeDocument.cpp)、[`WindowedReadOnlyProvider`](../src/editor/WindowedReadOnlyProvider.cpp) | 小二进制及窗口化文件双表示、切换前补丁同步、光标/选区源字节一致、解码错误；小文本直接编辑界面没有完整双表示路径 |
| NSP-VIEW：阅读设置 | [`MainWindow`](../src/window/MainWindow.cpp)、[`EditorTab`](../src/editor/EditorTab.cpp) | 自动换行、空白/行尾符号、标尺及 80/100/120 列、缩放/重置、状态栏/结果面板；窗口化只读强制不换行 |
| NSP-LANGUAGE：语法与折叠 | [`FeatureRegistry`](../src/domain/FeatureRegistry.cpp)、[`EditorTab`](../src/editor/EditorTab.cpp)、[`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 扩展名猜测、手选语言、恢复保留、当前/全部折叠、匹配括号跳转/选择；不把 lexer 当代码解析器 |
| NSP-SEARCH：当前/全部打开文件查找 | [`FindReplaceDialog`](../src/search/FindReplaceDialog.cpp)、[`SearchNavigationRequest`](../src/search/SearchNavigationRequest.cpp) | 前后方向、循环开关、标签顺序、普通文本/二进制/只读混合、锁定跳过、无匹配、空查询、查询变更；没有目录搜索入口 |
| NSP-SEARCH：解释选项 | [`FindReplaceDialog`](../src/search/FindReplaceDialog.cpp)、[`EditableTextSearch`](../src/search/EditableTextSearch.cpp)、[`LargeFileSearch`](../src/search/LargeFileSearch.cpp) | 字面量/正则/大小写/整词的合法组合；正则与整词互斥，字节模式与正则/大小写/整词互斥；非法程序化状态也有归一化 |
| NSP-SEARCH：正则与助手 | [`Pcre2Regex`](../src/search/Pcre2Regex.cpp)、[`EncodedRegexCompiler`](../src/search/EncodedRegexCompiler.cpp)、[`FindReplaceDialog`](../src/search/FindReplaceDialog.cpp) | 零宽、跨行、捕获/命名捕获、错误位置、编码映射；助手编辑同一条正则文本，并非 AI 或另一套查询语言 |
| NSP-SEARCH：字节搜索 | [`FindReplaceDialog`](../src/search/FindReplaceDialog.cpp)、[`EditorTabSearch`](../src/editor/EditorTabSearch.cpp) | Hex 视图限定、字节格式、奇数/非法字符、00/FF、多窗口/EOF、原字节精确定位；字节模式只查找不替换 |
| NSP-SEARCH：替换 | [`FindReplaceDialog::onReplaceAll`](../src/search/FindReplaceDialog.cpp)、[`EditableTextSearch`](../src/search/EditableTextSearch.cpp) | 当前单次/全部替换、捕获展开、空替换、不可表示输出、一次撤销；只修改可编辑 Text，不修改 Binary/只读/锁定文档 |
| NSP-SEARCH：跨标签全部替换 | [`FindReplaceDialog::onReplaceAll`](../src/search/FindReplaceDialog.cpp) | 全体预检、跳过清单、默认取消、确认后 revision 变化/关闭、异常回滚、每文件一个撤销步骤、不自动保存 |
| NSP-SEARCH：结果与异步生命周期 | [`SearchResultsController`](../src/search/SearchResultsController.cpp)、[`SearchResultsModel`](../src/search/SearchResultsModel.cpp)、[`LargeFileSearchTasks`](../src/search/LargeFileSearchTasks.cpp) | 增量/容量/部分结果、结果行紧凑、重复导航、取消与重跑、旧结果失效、编辑/编码切换/关闭/外部替换时禁止错误跳转 |
| NSP-SEARCH：书签 | [`EditorTabCommands`](../src/editor/EditorTabCommands.cpp) | 标记/下一处/上一处/清除、首末端循环、编辑后位置；当前实现绑定原生编辑器，窗口化全局书签不作为已交付能力 |
| NSP-LARGEFILE：只读浏览 | [`WindowedReadOnlyProvider`](../src/editor/WindowedReadOnlyProvider.cpp)、[`ReadOnlyTextAnalysis`](../src/file_runtime/ReadOnlyTextAnalysis.cpp) | 32 MiB 前后、超长行、多字节跨窗口、首尾定位、大偏移、全局滚动、Text/Hex 选区、统计未知/进行中/完成；只读限制不能被快捷键绕过 |
| NSP-LARGEFILE：普通读取准入 | [`OrdinaryReadSource`](../src/file_runtime/OrdinaryReadSource.cpp)、[`ReadOnlySearchSource`](../src/search/ReadOnlySearchSource.cpp) | 同句柄身份、删除/替换/截断、原普通文件变成安全文件、读取失败、映射回收；打开时只读判定不能被后续较小替换文件放宽 |
| NSP-LARGEFILE：跟随末尾 | [`ReadOnlyTailController`](../src/editor/ReadOnlyTailController.cpp)、[`DocumentTailActions`](../src/window/DocumentTailActions.cpp) | 默认关闭；普通只读 Text/Hex；追加跟随，浏览/选区/导航暂停，显式恢复到末尾；异常源变化停止；搜索与统计不可抢回阅读位置 |
| NSP-ENCODING：检测与显示 | [`TextEncoding`](../src/editor/TextEncoding.cpp)、[`MainWindowCommands`](../src/window/MainWindowCommands.cpp) | BOM、样本中部/EOF、非法字节、跨多字节边界、强制显示、dirty 小文本先保存/取消；Binary 切编码不改变原始字节身份 |
| NSP-ENCODING：转换输出 | [`MainWindow::convertEncodingFromAction`](../src/window/MainWindowCommands.cpp)、[`transcodeFileEncoding`](../src/editor/TextEncoding.cpp) | 已命名普通 Text，包括只读大文件；输出新路径、拒绝源同路径/已打开目标、不可表示字符、错误源编码、失败/取消、完成后以指定编码打开 |
| NSP-ENCODING：行尾 | [`MainWindow`](../src/window/MainWindow.cpp)、[`EditorTab`](../src/editor/EditorTab.cpp) | CRLF/LF/CR、混合行尾、空文件和末行、dirty/撤销/保存重开；仅可编辑原生 Text，不与编码转换混为一个功能 |
| NSP-SECURE：安全身份与凭据 | [`SecuritySessionService`](../src/vault/SecuritySessionService.cpp)、[`FileCredentialDialog`](../src/window/FileCredentialDialog.cpp)、[`MainWindowDocuments`](../src/window/MainWindowDocuments.cpp) | 首次身份、正确/错误口令、恢复密钥、外来文件、取消、设备会话失效；KDBX ON 验收 |
| NSP-SECURE：安全文档持久化 | [`SecureDocumentPersistence`](../src/app/SecureDocumentPersistence.cpp)、[`SecureDocumentService`](../src/vault/SecureDocumentService.cpp) | 文本/二进制、未命名草稿/命名文件、载荷接近 32 MiB、保存/自动保存/本机加密恢复失败、磁盘冲突、重开核对 |
| NSP-SECURE：锁定与恢复 | [`MainWindowDocuments`](../src/window/MainWindowDocuments.cpp)、[`EditorTabPersistence`](../src/editor/EditorTabPersistence.cpp) | 搜索/剪贴板/撤销等缓存清理、锁定后无内容能力、重解锁；主动锁定失败可取消，到期时保存和恢复均失败可丢弃未保存内容以清除明文 |
| NSP-ACCOUNTS：文件与条目 | [`VaultDock`](../src/vault/VaultDock.cpp)、[`VaultDockEntries`](../src/vault/VaultDockEntries.cpp)、[`VaultService`](../src/vault/VaultService.cpp) | 新建/打开/关闭/锁定；条目新建/编辑/取消/保存/克隆/移入回收站/恢复；标题、用户名、网址、标签、分组、额外网址、自定义字段、过期时间、备注 |
| NSP-ACCOUNTS：检索与历史 | [`VaultDockEntries`](../src/vault/VaultDockEntries.cpp) | 标题/用户名/网址/标签/备注过滤、排序、历史查看/恢复、回收站状态、dirty 切换、外部文件变化；秘密不进入普通文档结果 |
| NSP-ACCOUNTS：秘密取用 | [`VaultDockEntries`](../src/vault/VaultDockEntries.cpp) | 密码显隐及定时隐藏、受保护复制及条件清除、用户名复制、网址打开、密码生成选项/强度/健康检查、TOTP 配置/刷新/复制 |
| NSP-ACCOUNTS：安全中心 | [`MainWindow`](../src/window/MainWindow.cpp)、[`SecuritySessionService`](../src/vault/SecuritySessionService.cpp) | 会话状态、立即锁定、修改主密码、查看/复制/导出/验证恢复密钥；失败提示、输入清理和工作区切换 |
| NSP-TASKS：任务反馈 | [`AsyncTaskManager`](../src/async/AsyncTaskManager.cpp)、[`AsyncTaskPanelDialog`](../src/async/AsyncTaskPanelDialog.cpp)、[`StatusBarTaskWidget`](../src/async/StatusBarTaskWidget.cpp) | 排队/运行/成功/失败/取消、真实可取消能力、连续进度、选中项实时刷新、详情展开/复制及滚动位置、关闭窗口后的任务清理 |
| NSP-HELP：关于 | [`MainWindowCommands`](../src/window/MainWindowCommands.cpp) | 产品版本、依赖和构建信息与候选实际身份一致 |

语法菜单的当前集合：Plain、C/C++、Python、JSON、HTML、XML、SQL、YAML、Markdown、JavaScript、Java、C#、CSS、Bash、Batch、CMake、Makefile、Lua、Ruby、Perl、Diff、Properties、ASM。编码显示/转换集合：GBK、GB18030、Big5、Shift-JIS、Windows-1252、UTF-8、UTF-8 BOM、UTF-16LE、UTF-16BE。两者均可在 [`FeatureRegistry`](../src/domain/FeatureRegistry.cpp) 与 [`MainWindow::createMenus`](../src/window/MainWindow.cpp) 核对，不能从第三方库支持范围推断本产品菜单能力。

## 3. 已有测试资产与覆盖要求

以下只说明仓库存在对应测试源码。是否编译进某个配置、实际选择了哪些 case、是否执行、候选是否一致，必须在回归记录中另外核实。`NSP-RECOVERY` 等 CTest label 不等于新增的运行时 FeatureRegistry 条目。

| 功能域 | 现有测试源码入口 | 不能由其自动推导的结论 |
|---|---|---|
| 命令/构建边界 | [`test_architecture_contracts`](../tests/test_architecture_contracts.cpp)、[`test_filetierpolicy`](../tests/test_filetierpolicy.cpp)、[`test_startup_composition`](../tests/test_startup_composition.cpp) | 枚举/对象存在不能证明显示、焦点、内容语义正确 |
| 文本/Hex/菜单 | [`smoke_smallfixes_menu_ui`](../tests/gui_smoke/smoke_smallfixes_menu_ui.cpp)、[`smoke_coord_editops`](../tests/gui_smoke/smoke_coord_editops.cpp)、[`smoke_windowed_hex_mouse`](../tests/gui_smoke/smoke_windowed_hex_mouse.cpp) | 历史 case 名包含 L2/S3 不代表对应模式现今可编辑；必须核对 fixture 和真实打开路径 |
| 编码与搜索 | [`test_textencoding`](../tests/test_textencoding.cpp)、[`test_largefile_encoded_search`](../tests/test_largefile_encoded_search.cpp)、[`smoke_find_replace`](../tests/gui_smoke/smoke_find_replace.cpp) | 部分编码/合法选项覆盖不能替代非法字节、输出文件和异步交错的完整矩阵 |
| 搜索生命期与容量 | [`smoke_search_lifecycle`](../tests/gui_smoke/smoke_search_lifecycle.cpp)、[`smoke_search_capacity`](../tests/gui_smoke/smoke_search_capacity.cpp)、[`smoke_search_external_changes`](../tests/gui_smoke/smoke_search_external_changes.cpp)、[`smoke_search_malformed_encoding`](../tests/gui_smoke/smoke_search_malformed_encoding.cpp) | fake/注入失败与真实操作系统错误、正式进程关闭不是同一层证据 |
| 大文件/Tail | [`smoke_large_readonly`](../tests/gui_smoke/smoke_large_readonly.cpp)、[`smoke_tail_follow`](../tests/gui_smoke/smoke_tail_follow.cpp)、[`smoke_tail_presentation`](../tests/gui_smoke/smoke_tail_presentation.cpp)、[`test_ordinary_read_source`](../tests/test_ordinary_read_source.cpp) | 内容正确不等于长时间资源稳定或首屏性能达标 |
| 保存/恢复收尾 | [`test_closeout_persistence`](../tests/test_closeout_persistence.cpp)、[`test_session`](../tests/test_session.cpp)、[`smoke_recovery_preservation`](../tests/gui_smoke/smoke_recovery_preservation.cpp)、[`smoke_recovery_intent`](../tests/gui_smoke/smoke_recovery_intent.cpp)、[`smoke_recovery_issue`](../tests/gui_smoke/smoke_recovery_issue.cpp) | 后端保全测试不证明新确认对话框、默认按钮、失败后继续编辑和再次关闭流程 |
| 安全/Accounts | [`test_kdbx_backend`](../tests/test_kdbx_backend.cpp)、[`test_security_identity`](../tests/test_security_identity.cpp)、[`smoke_secure_documents`](../tests/gui_smoke/smoke_secure_documents.cpp)、[`smoke_vault`](../tests/gui_smoke/smoke_vault.cpp) | OFF 不能证明安全行为；遮罩出现不能证明秘密缓存已经清理 |
| 正式进程/视觉 | [`e2e_single_instance`](../tests/e2e_single_instance.ps1)、[`e2e_hex_process_recovery`](../tests/e2e_hex_process_recovery.cpp)、[`e2e_startup_verify`](../tests/e2e_startup_verify.cpp)、[`smoke_hex_accessibility`](../tests/gui_smoke/smoke_hex_accessibility.cpp) | Qt 内部属性不证明 Windows UIA、实机 DPI、可读性及键盘完整可达 |

回归用例必须记录：用例 ID、功能组、模式/配置、输入文件与字节摘要、前置状态、操作序列、预期正文/字节/dirty/视图/磁盘结果、自动化或实机层级、候选版本、执行结果和证据路径。未执行、受阻和仅静态检查必须分列，不计为通过。

组合覆盖以当前状态模型为边界，至少包括：

1. **内容与模式**：普通 Text、Binary Hex、Binary Text、只读 Text/Hex、安全 Text/Binary、锁定/切换中、Accounts、空工作区；不适用组合写出原因。
2. **内容边界**：空/单字节、32 MiB 前后及编码膨胀、超长行、无末尾换行、混合 EOL、BOM、非 BMP 字符、非法字节、首尾/跨窗口选区。超过 2 GiB 只用于偏移与资源压力验证。
3. **编辑序列**：操作 → 撤销 → 重做 → 保存 → 重开；只读文本大文件 Text/Hex 查看切换前后字节一致；普通文本增长到保存快照上限时不丢内容。
4. **搜索序列**：查找 → 编辑/切编码/换表示/关闭 → 点击旧结果；取消 → 重跑；跨标签替换确认前后内容变化；搜索容量耗尽后禁止以不完整匹配集合替换。
5. **持久化序列**：两个以上 dirty 文档中一个快照失败、清单提交失败、外部写入、路径替换、目标被占用、恢复途中关闭取消/重试；核对全部旧稿和当前正文责任。
6. **安全序列**：正在搜索/编辑/复制 → 主动锁定或到期 → 再解锁；分别注入安全保存失败、恢复快照失败、二者均失败；正文、账号秘密与恢复材料不得跨入普通会话和普通搜索。
7. **副本生命周期**：正式 KDBX 固定保留一份保存前密文，连续保存验证内容和数量；旧时间戳副本在验证新备份后迁移清理。随机另存候选与托管草稿不生成持久备份，恢复点复用固定路径；覆盖备份、清理旧副本和提交原件被占用后的重试见 SAV-09。异常会话保全、用户文件和账号条目历史不混作自动文件副本删除。

涉及正文保全、字节保真、覆盖冲突、秘密隔离的边界逐项执行；一般显示选项采用成对组合并补焦点、生命周期等高风险多因素组合。只跑三条日用旅程不能代表完整回归。

## 4. 本次源码梳理发现的能力差异与待验证点

- **大文件已经降级**：后台升级为可编辑大文件的能力关闭；旧大文件编辑恢复日志入口明确拒绝。2 GiB 残留分类不是产品分档，文档和测试解释不能继续把它当成功能边界。
- **只读仍有转换输出**：普通只读 Text 的编码转换会写到新文件；普通 `Save/Save As` 被持久化层拒绝。两者要分别验证，不能概括成“大文件不能产生任何新文件”。
- **菜单状态与执行权限尚非一处权威**：`CommandRegistry::applyContext` 对 `file.saveAs` 主要使用“有文档”，而 `PlainFilePersistence::save` 检查 `Save` capability 后拒绝只读。当前可能出现入口可点击、到执行阶段才拒绝的情况；这是界面一致性待复核点，不能写成“所有入口均已正确禁用”。
- **书签和排序需要重点复核**：`EditorTabCommands` 中书签、排序直接使用 `m_editor`；窗口化可见编辑器可能不是这个对象。排序还把 `text(line)` 结果重新用换行拼接。需以 CR/LF/CRLF 和末行用例验证，当前只记静态风险，不记已复现缺陷。
- **共同显示设置需要动态差分**：`setTabWidth` 和 `applyEdgeSettings` 当前只更新原生编辑器，未同步给窗口化只读 Provider。默认样式相等不能证明运行中修改Tab宽、标尺、主题和缩放仍一致；具体执行PAR-03至07。
- **编辑增长与会话输出有容量边界**：普通快照以大于32MiB拒绝，DraftStore以大于等于32MiB拒绝；已有普通文档编辑增长须验证保存/恢复闭环。Session读取限制256项/4MiB，写端是否会发布自身无法重读的清单需验证。执行EDIT-11、REC-09，静态风险尚非已复现Bug。
- **普通保存仍有竞争窗口**：新代码在写候选并释放底层句柄后再次比较磁盘版本，但最后比较与 `QSaveFile::commit` 之间没有文件系统条件提交。不能宣称已经消除所有并发覆盖风险。
- **保存错误不一定表示磁盘未变**：普通保存提交成功后，版本验证或底层重开仍可能失败并返回 Error；回归必须区分提交前失败与提交后失败，核对磁盘文件、内容基线和错误提示。
- **视图有界不代表搜索映射有界**：窗口化投影限制显示数据量，但 [`ReadOnlySearchSource`](../src/search/ReadOnlySearchSource.cpp) 的搜索准备仍有 `map(0, size)` 路径。超大文件资源、地址空间、取消和解除映射需要独立证据，不能由首屏内存样本推导。
- **收尾修改需要新增 GUI 证据**：异常清单的保全/取消对话框与多个 dirty 标签失败组合，不能仅用 `test_closeout_persistence` 中的后端结果替代用户流程。
- **AI 与代码解析没有交付实现**：存在 [`InferenceProvider`](../src/domain/InferenceProvider.h)、[`InferenceProviderRegistry`](../src/domain/InferenceProviderRegistry.cpp)、[`DocumentToolRegistry`](../src/domain/DocumentToolRegistry.cpp) 等接口；当前装配没有真实推理/文档工具注册或完整用户入口。`AnalyzeLocally/AnalyzeRemotelyWithConsent` 能力位也不代表这些产品功能已经存在。

本次仅更新功能依据和回归边界，不把任何历史通过数迁移为新结论。收尾范围与交付状态见 [当前收尾清单](DAILY_USE_CLOSEOUT.md)；架构依据见 [架构说明](DESIGN.md)。
