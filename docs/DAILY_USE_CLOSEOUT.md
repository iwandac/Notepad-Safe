# 个人日用版收尾（2026-10-02）

本页是保存、恢复、单副本及后续功能回归的唯一执行账本；当前范围和执行顺序见 [ITERATION_ROADMAP.md](ITERATION_ROADMAP.md)。基线 `988b097`，版本仍为 0.3.1，交付形态为本地候选 EXE，不推送、发布或打包分发。

## 当前汇总

- **计划状态**：已整理128个主用例、45组功能映射和17组外观/输入组合；完整参数子例和回归执行未完成，目前没有主用例被证明全部适用子例关闭。
- **当前构建**：`build-final/notepad-safe.exe`，33,144,320字节，SHA-256 `6297B31E82726D7B29CDEB7B2978026A9C2C06C6E5CE35B3EF626E603442B002`；基于 `988b097` 加保存/恢复和单副本的未提交修改，Release / KDBX ON。这是中间验证构建，验收候选尚未冻结，也没有发布。
- **当前有效执行**：单副本定向及相邻安全底层测试5/5通过，22.15秒；只支持下节列明的子例。SAV-09整行以及保存/恢复总批次仍未完成，历史10/10、18/18、7/7不累加为当前版本通过数。
- **尚未完成**：当前版本保存/恢复的统一相邻复测、完整编辑及跨模式组合、真实客户端与重启/强杀、安全生命周期、KDBX OFF和性能。BD2桌面状态须在实际使用前核对并交接，不由旧记录推定当前可用或仍占用。
- **下一行动**：依统一计划完成VAL准备与首批子例展开，继续同一批保存/恢复/副本回归；不因本次文档整理重建或重复运行已通过的五组测试。

## 当前执行证据

### 2026-10-17：新外壳验证（分支 `feat/simplify-and-shell`）

构建 `build-final/notepad-safe.exe`，33,209,344 字节，SHA-256 `14064B38C4EBE92C34E773DA3D3022A717651C55A6A8691D67EE1913061F2F62`，基于 `4b116a9`，Release / KDBX ON / Qt 6.11.1 静态链接。依赖（Botan、QScintilla）在本轮用 `prepare_dependencies.bat` 离线重建后重新链接。

- **核心测试** 17 组全部通过，新增 `test_theme_tokens`、`test_shell_logic`、`test_command_palette_model`；架构检查通过（MainWindow 方法数 112/112，已到上限）。
- **GUI 套件**（真实 Windows 平台插件、串行）：smallfixes 71/73（两项为已登记的原有问题；新增的外观、隐藏菜单栏快捷键、状态栏分段、命令面板、保存失败提示条用例全部通过），startup_shell 7/7，secure_documents 14/14，large_readonly 25/25，hex_readonly_search、tail_presentation、windowed_hex_mouse 全部通过；recovery_issue 14/16、document_product_language 22/24、findreplace 40/41 的失败均为分支起点即有的问题；tail_follow 20/21 为功能删除阶段遗留的未解决项；hex_accessibility 4/5 因未设置 `NOTEPADSAFE_EXPECTED_DPR` 环境变量（受阻，不是产品结果）。
- **桌面检查**：截图确认标题栏、主菜单、绘制标签、状态栏分段、命令面板、深色外观；双击标题栏最大化正常且不溢出屏幕。检查中发现应用样式表占位符错位（菜单文字为白色、禁用项为蓝色）及状态栏原生分隔框，均已修复（`4b116a9`）。拖动、贴靠布局浮层、边缘缩放、Win+方向键未逐项人工验证（受阻）。
- **启动时间（J1）**：正式启动探针 30 次冷启动，从进程创建到窗口可见（含观察器开销）中位 537 ms、P95 793 ms、最大 966 ms，未达到 500 ms 目标。探针仍查找已隐藏的菜单栏，后续“打开文件菜单”阶段无法完成，需要按新外壳更新；与历史 693 ms P95 的测量点不同，不能直接比较。

### 2026-10-17：功能删除验证（分支 `feat/simplify-and-shell`）

构建 `build-final/notepad-safe.exe`，33,105,920 字节，SHA-256 `76BC7A5580FE4CF5758E7F6E5B32B3211626AA76C2249747CC2FCBA9FEA58A0B`，基于 `d89ba7c`，Release / KDBX ON / Qt 6.11.1 静态链接。删除了分支起点之后所有比最新改动头文件旧的项目目标文件（180/238）后全量重编：中文编译器提示使 CMake 漏掉间接包含的头文件依赖，此前的增量构建里有布局不一致的旧目标文件，两个恢复类套件的崩溃即由此造成。

核心测试 14 组全部通过（含新增 `test_document_open_routing` 7/7 与 `test_settings_ui`）。GUI 套件使用真实 Windows 平台插件串行执行；为区分原有问题，另在分支起点 `2de41ad` 建对照构建复跑了失败项。

| 套件 | 结果 | 说明 |
|---|---|---|
| findreplace | 40/41 | `tc_fr_024` 在分支起点同样失败，原有问题 |
| secure_documents | 14/14 | 含新增两项加密二进制用例 |
| smallfixes | 64/68 | `tc_external_open_preserves_dirty_and_active_search`、`tc_gui_large_conversion_rejects_unsampled_invalid_source_byte` 为原有问题；两项十六进制契约用例本次因桌面焦点被占用失败，前两轮在本分支通过 |
| large_readonly | 25/25 | 含新增 `windowedTextReopensAsHexAndRestoresAcrossSession`；它发现了只读大文本以十六进制恢复时缺少提供器的问题，已在 `d89ba7c` 修复 |
| tail_follow | 20/21 | `sourceFaultStopsAndNeverAutoResumes(truncate)` 在本分支三次运行均失败、分支起点通过，**未解决**，交最终审查 |
| document_product_language | 22/24 | `realLargeSearchCancellationKeepsStoppedReason` 两行在分支起点同样失败 |
| recovery_issue | 14/16 | `tc_2b1_modified_without_draft_does_not_load_original` 两行在分支起点同样失败 |
| readonly_search_source、windowed_hex_mouse | 全部通过 | |
| hex_readonly_search、tail_presentation | 各 1 项失败 | 焦点/主题相关，前两轮在本分支通过，本次运行时桌面在使用中 |

已删除或改写的用例见三份测试矩阵顶部的 2026-10-17 说明。


### 2026-10-02：单副本保留定向验证

中间验证构建基于 HEAD `988b097` 加当前未提交工作树，`build-final` Release / KDBX ON / Qt 6.11.1。`notepad-safe` 和两份测试程序重新构建成功，架构边界检查、`git diff --check`通过。源码、夹具与程序SHA-256见本机生成的 [证据清单](../build-final/backup-retention-evidence.json)，构建输出见 [构建日志](../build-final/backup-retention-build.log)。

| 范围 | 实测结果与判据 | 证据层次 |
|---|---|---|
| KDBX单副本轮换 | 3个不同版本每次仅1份previous，字节等于保存前原件，独立解锁读到上一版内容；首次后端创建无备份 | `test_kdbx_backend`，服务/真实文件 |
| 旧时间戳迁移 | 多份历史收敛为1；Windows大小写变体清理；点号近似名、非法日期、额外后缀、其他文件与同名目录内容保留 | `test_kdbx_backend`，服务/真实文件 |
| 提交前故障 | Windows共享句柄分别阻止备份替换、历史删除和原件提交，达到指定错误阶段；原件不变，解除后同service重试成功；损坏当前文件时既有备份不变 | `test_kdbx_backend`，真实OS故障 |
| 安全文档副本生命周期 | 3次草稿保存后重开与迁移；3次checkpoint复用1文件并在保存后消费；3次另存覆盖都保留目标上一正文和原密文字节，无随机候选/备份目录残留；备份占用失败后重试；明文目标不产生明文备份 | `security_backup_retention`，真实服务/文件/独立读取 |
| 相邻安全合同 | 身份、密码轮换、恢复验证3组现有用例通过 | `security_identity`、`security_identity_rotation`、`security_recovery_verification` |

五组CTest在隔离的 `build-final/backup-retention-temp` 下串行执行，**5/5通过，22.15秒**，见 [主机执行日志](../build-final/backup-retention-ctest-host.log)。首轮沙箱中Qt临时文件写入被拒绝，5组夹具初始化失败，不能算产品通过或有效产品缺陷；[原失败日志](../build-final/backup-retention-ctest.log)保留，随后主机执行通过。

本轮没有占用桌面，没有执行完整GUI、性能、强杀或链接/目录联接的动态删除边界；提交后最终验证失败的状态协调也未证明可重试。旧版孤立草稿/随机候选备份未全盘清扫，异常保全资料与账号历史保留独立责任。上述未测范围继续保持未执行，不将SAV-09整行标为全部通过。

以下保留较早保存/恢复工作的结果和设计判断，不作为当前中间构建已经复验的证据。

## 本版目标与必要流程

| 目标 | 必须走通的流程 | 验收证据 |
|---|---|---|
| 个人离线编辑器 | 新建/打开 → 编辑 → 保存 → 关闭重启；未保存内容恢复；外部修改冲突 | 内容逐字节一致；保存/恢复失败不能误报成功或丢弃旧稿 |
| 只读大日志定位 | 打开大日志 → 正则搜索 → 结果回到原日志对应位置 | 原文件未改变；取消可用；位置与合成夹具一致 |
| 账号保险箱 | 创建合成库和账号 → 取用 → 锁定 → 重开 | 重开内容一致；锁定后秘密不可取用；不使用真实凭据 |

保留 `>=32 MiB` 只读、安全格式只经另存为转换、账号秘密隔离等边界。日志结果回原日志不包含解析代码路径跳源码。

## 冻结与延期

冻结新功能；延期 AI、代码可视化、继续扩展 Hex。保留已有功能，不做泛化重构或新增治理体系。安装、签名、公开发布、软件安装升级均不在本次范围。

## 退出条件

1. 优先用隔离数据复现并修复正文快照失败误报成功、无效清单导致恢复稿清理；验证普通保存并发冲突，记录仍无法保证的边界。
2. 当前源码构建成功；定向及相邻无桌面测试通过。历史测试数量不能充当本版证据。
3. 同一候选 EXE 完成上述三条真实 GUI 入口及[详细验证清单](REGRESSION_CHECKLIST.md)中适用的功能、模式一致性、离线安全和失败重试/重启子例；三条旅程不能代替完整组合回归，未完成不得称为完全交付。
4. 首屏性能历史 P95 为 693.458 ms，500 ms 门仍未通过；无本轮测量不得改写。需要后续复测或用户明确接受例外。

共享桌面与 BD2 的占用以任务间明确交接为准，不在本页固定占用方。未取得桌面时不启动 GUI、自动 UI 测试或鼠标键盘操作。只运行经入口确认的 QCoreApplication/无应用测试；使用临时 profile、合成文件和保险箱，构建单并发。GUI 空闲也不代表首屏性能测量所需的低负载条件已满足。

## 历史记录：单副本修复前的保存与恢复验证

以下为2026-10-02较早保存/恢复任务的记录：基于`988b097`的当时工作树，Qt 6.11.1 / MSVC Release / KDBX=ON。初始干净仅描述该旧任务的起点；此后已有未提交改动。该任务未推送发布、未启动GUI。

- 历史候选（此路径现已被上方中间构建替换）：`build-final/notepad-safe.exe`，33,138,176 字节；SHA256 `8560102798A01CF55EE35F35C1843792201BD9D941A3496D4F9E4F7F95076500`。包含当时的无效清单保全流程；该历史构建没有GUI验收结论。
- 修复前：新建 QCoreApplication 测试有效复现 9 个失败场景，日志 `build-final/closeout-before.txt`。第一次沙箱运行无法写入临时夹具，其结果已排除；经授权运行后才取得有效复现。
- 第一轮修复后：9 个场景全部通过（Qt 总计 11 项含初始化/清理），日志 `build-final/closeout-after.txt`。覆盖首次/旧稿的导出与真实 I/O 失败、失败后重试、新旧正文区分、旧清单及超过7天恢复稿保留、损坏/未知版本/错误结构清单、保存期间外部写入及冲突后重试。
- 首个候选的无窗口 CTest 10/10 通过，10.93 秒：`textencoding`、`pcre2_regex`、`largefile_encoded_search`、`closeout_persistence`、`filetierpolicy`、`vault_contract`、`kdbx_backend`、`security_identity`、`security_identity_rotation`、`security_recovery_verification`。日志 `build-final/closeout-core-tests.log`；下述保全改动后的定向证据单列，不把此记录当成后续全套复测。
- 架构和异步线程边界检查通过，日志 `build-final/closeout-architecture.log`、`build-final/closeout-threading.log`；`git diff --check` 通过。旧 `test_session` 已更新严格拒绝超限清单的预期并编译，因使用 QApplication 未运行。
- 保全改动后的最终定向测试：`test_closeout_persistence` 18/18（16 个场景加初始化/清理），636 ms，日志 `build-final/closeout-preservation-tests.txt`；包括保全目录被文件占用、旧稿拒绝读取、最新正文导出失败、清单禁止替换、各故障解除后重试、新旧正文独立可恢复，以及共享句柄与条件替换实验。已有 `test_windows_read_rename` 7/7（5 个场景加初始化/清理），26 ms，日志 `build-final/closeout-native-rename.txt`。全部使用 QCoreApplication。
- 新候选单并发构建成功：`build-final/closeout-preservation-build.log`。最终架构检查 MainWindow 5188/5190 行，通过；异步线程边界通过；日志分别为 `build-final/closeout-preservation-architecture.log`、`build-final/closeout-preservation-threading.log`。构建中曾因 MainWindow 行预算和旧对象方法签名失败，已归回现有恢复协调器并重编译相关对象解决，没有提高预算或新增框架。

实际修复：必要正文写入失败不再发布清单或清理未引用的恢复稿，不会通过强制关闭的成功条件；无效清单与正常空会话区分，禁止自动覆盖/清理并在启动状态栏提示，明确关闭时可选择先保全再建立当前恢复点；普通文件在候选写入及底层句柄释放后再检查外部版本。

2026-10-02 源码复核限定：普通有效会话仍可同路径重写既有 draft，再提交 manifest（`SessionManager::captureAsyncSaveEntries/writeAsyncSave`）。因此某正文或 manifest 失败时，不等于之前成功写入的 draft 会回滚；旧清单可能引用新正文。这与无效清单保全使用新路径的流程不同。`test_session_preservation` 已有同路径重封预期，新 closeout 测试主要使用单个 SessionPort；多文档部分成功/失败仍需补组合证据。不能把本轮修复表述为跨文件事务或整个旧快照集合字节不变。

## 无效清单的保全与退出

上一候选的缺陷触发条件：现有 `session.json` 损坏、不可读、版本或结构不支持，关闭窗口又无条件要求 `saveSessionNow(true)` 成功。即使当前没有未保存正文，该组合也会反复取消退出。

当前流程只在明确关闭窗口时提供“保全旧资料并保存当前会话”/“取消”，默认取消。自动保存、锁定、会话到期不弹此选择，不延迟安全内容遮蔽。保全方案不猜测未知版本的内容类型，也不宣称旧会话已自动修复。

1. 后台复制原始清单及本应用清理范围内的旧恢复文件到数据目录下 `recovery-preserved/<UUID>/`；逐文件比较长度与 SHA256。源文件不移动、不删除。
2. 全部成功才写入 `recovery-index.json`，记录原路径与副本路径。没有该索引的目录属于未完成保全，仍不自动删除。副本保留既有密文格式，不解密落盘。
3. 当前需要恢复的正文写到新 `.ndraft` 路径，不覆盖旧稿；全部正文成功后才原子提交新清单。此次保全提交不执行旧稿清理。
4. 新清单提交成功才返回可退出；保全失败、正文失败或提交失败都保留原始资料和当前内存内容，解除磁盘/权限问题后可重试。后续普通自动清理只处理活动 autosave 目录，不进入保全目录。

保全副本供后续修复使用，原始损坏清单可能无法直接恢复全部标签；索引保留路径关系。无窗口测试通过独立 `SessionManager` 验证副本中的合成加密旧稿仍可解密，同时重开的活动 profile 恢复最新正文。未增加未知格式转换或全新恢复工具。

## 普通保存竞争的证据与有限方案

当前保证限于：需要预期版本的保存，在快照前及候选写好、底层句柄释放后各检查一次；检测到版本不同/缺失/不可读就取消候选，不接受提交。版本证据是身份、大小、修改时间及有界采样哈希，不是全文件比较；它不能证明任意外部写入者从未改动文件，也不是条件替换。

`qsavefileHasNoConditionalCommit` 在“检查通过”之后确定性插入另一个 `QSaveFile` 提交，再提交本地候选，观察到外部版本被覆盖。它是底层原语证据，不假称已在生产函数最后两条指令间完成跨进程调度。`externalWriteBeforeCommit` 则在真实产品保存路径的底层释放回调插入写入，验证新增末次检查会拒绝覆盖。

| 方案 | 本轮判断 |
|---|---|
| 保留末次检查 | 已实施；缩短已复现长窗口，不能提供跨进程绝对无竞争承诺 |
| 在目标上持有共享限制句柄 | 本机实验显示不允许删除共享会阻断自己提交；即使共享读/写/删除，普通 Qt/MoveFileEx 替换打开目标仍失败。提交前释放句柄仍有窗口；不直接引入生产 |
| 改为 POSIX 语义重命名 | 既有诊断在本机证实 `FileRenameInfoEx` 可在旧读句柄仍打开时替换目标。旧句柄仍读旧文件，目标路径已指向新身份；其他写入者也可使用该原语，因此换 API 本身仍不是预期版本条件提交 |
| 协作锁文件 | 只能协调遵守同一锁协议的写入者，对任意编辑器/工具不能成为保证，因此不冒充解决方案 |
| 原生替换并保留被替换版本 | 可考虑局限于 `PlainFilePersistence` 的后续方案；`ReplaceFileW` 有备份参数，但不是按预期版本比较后替换，且存在部分失败状态。必须定义冲突副本、失败/崩溃恢复、用户选择与清理，不能简单改一处 API 就宣称完成 |

官方原语边界：[Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)、[Windows CreateFileW 共享模式](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)、[ReplaceFileW 备份及失败状态](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)。本机行为以 Qt 6.11.1 测试日志为准，在线 Qt 文档当前版本不代表本项目升级。

## 尚未关闭的门槛

- 普通保存仍不是文件系统级 compare-and-swap。最后一次检查到原子替换间仍有极小并发窗口；本轮只修复已复现的长窗口，不能承诺对任意外部写入者无丢失覆盖。该项仍需设计/验证，不能标为完全修复。
- 无效清单不再无条件阻止退出：明确选择保全并成功保存后可继续。真实权限/磁盘错误仍会阻止不安全退出；旧损坏清单的内容修复需在保全副本上继续，未操作真实资料。
- 三条真实 GUI 流程、关闭失败后的窗口行为、状态栏可见性、跨进程重启、剪贴板清理和大日志结果定位未验收；新增保全选择框也没有本轮窗口证据。三条旅程只是验收入口，不替代[功能组合回归](TARGET_EXPERIENCE.md)。桌面使用需明确交接，不能用 Core 测试替代。
- 本轮未运行 KDBX=OFF、ASAN、全套 GUI/E2E 或首屏性能复测；693.458 ms 的历史 P95 仍未达到 500 ms。当前交付是可继续验收的候选，非完成版。
