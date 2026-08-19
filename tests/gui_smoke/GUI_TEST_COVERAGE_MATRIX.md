# GUI 全功能测试矩阵

本文件是开发与验证时的工作清单，不预写固定用户旅程。每次改动先选择受影响的功能 ID，
再结合交叉维度和历史缺陷哨兵动态设计真实操作。测试记录只需保存：覆盖 ID、实际输入、
可见结果、数据/磁盘结果、性能结果和证据路径。

## 1. 测试原则

1. 关键操作使用真实菜单、快捷键、鼠标、滚轮、拖放或窗口事件，不以内调函数代替。
2. 每项同时检查：屏幕可用性、领域状态、数据内容、持久化结果；内部 offset 正确不等于 GUI 正确。
3. 先验证改变的功能，再验证同 Provider 邻接功能和跨 Provider 一致性；避免全量重复跑。
4. 矩阵描述“必须证明什么”，具体操作随实现和已发现风险调整，不固化为长脚本。
5. 新增可见功能、状态或 Provider 必须新增/修改矩阵项；没有矩阵归属的 UI 不进入产品。
6. 每个会改变领域语义的 checkbox/radio/combo 都必须建立“参数契约”：稳定 objectName、领域参数、
   默认值、两态判别语料和至少一项正交组合。判别语料必须使两态产生不同结果；结果相同即失败，
   不能用“控件已勾选”代替行为验证。

## 2. 功能矩阵

| ID | 功能 | 必须覆盖的状态与边界 | 核心判定 |
|---|---|---|---|
| F01 | 启动/首帧 | 空会话、混合会话、CLI/文件关联显式文件、安全会话有效/过期 | 主窗口先可见；显式文件不被恢复抢焦点；无无关密码提示或卡顿 |
| F02 | 统一打开/路由 | 菜单、拖放、最近文件、Text/Hex/Secure/KDBX、S1–S3/B1–B3 | 同一入口按头部/内容/大小正确路由；重复路径/硬链接/DocumentId 只激活原标签 |
| F03 | 新建 | 首次身份、有效会话、无命名正文、关闭/崩溃前 | 默认安全草稿且立即可输入；不询问格式；恢复数据不含明文 |
| F04 | 保存 | Plain/Secure/Text/Hex、clean/dirty/conflict/read-only、写入失败 | 保持原类型；clean 严格 no-op；原子提交；失败保留原文件与 dirty |
| F05 | 另存为/转换 | Plain↔Secure、Text/Binary、同路径/新路径、取消/失败 | 只有此入口转换持久化类型；正文/字节/编码/EOL 正确；取消不改变当前 backing |
| F06 | 重载/外部变化 | clean/dirty、追加、截断、原子替换、格式/大小改变 | 阻止静默覆盖；重新完整路由；当前位置合理钳制；旧搜索结果失效 |
| F07 | 关闭/退出 | 当前/其他/全部、多个 dirty、安全草稿、后台任务、取消决策 | 预检全有或全无；取消不半关闭；任务/密钥/恢复点按策略清理 |
| F08 | 标签/右键 | 切换、移动、关闭其他/全部、复制路径、资源管理器、Text/Hex 重开 | 目标标签正确；能力与路径可见性正确；状态、选择、焦点不串标签 |
| T01 | Text 基本输入 | 空/有内容、Insert/显式 OVR、selection、Unicode/IME/Tab/换行 | 默认永远 Insert；输入不意外覆盖后文；正文、caret、dirty、状态栏一致 |
| T02 | Text 删除/剪贴板 | Backspace/Delete、正反向选择、行首/尾、Unicode、paste | 删除单位和目标正确；Cut/Copy/Paste/Select All 与可见选择及剪贴板一致 |
| T03 | Text Undo/Redo | 输入、删除、粘贴、行操作、替换、保存前后 | 历史边界正确；Undo/Redo capability 与实际历史一致；标签间不串历史 |
| T04 | 行操作 | 复制、删除、上移/下移、升/降排序；首/末/空行、多行选择 | 内容、选择、EOL、Undo 一致；Hex/只读明确禁用 |
| T05 | 导航/选择 | 鼠标点击/拖选、方向、Home/End、Page、滚轮、跨 wrap rows | caret 落在实际屏幕字符；选择连续；视图不回跳；状态栏行列正确 |
| T06 | 长行/wrap/标尺 | nowrap/wrap、超长行、横向末端、top/middle/EOF、resize | nowrap 全行可达；wrap 无横向条且滚动连续；display/source 映射正确 |
| T07 | 语言/高亮 | Plain、C/C++、Python、JSON、HTML、XML、SQL、YAML、Markdown | 菜单互斥、建议/手选/会话恢复正确；Hex/Accounts/只读能力符合设计 |
| T08 | 编码/EOL | UTF-8/BOM、UTF-16LE/BE、GB18030、CRLF/LF/CR/无尾换行 | 检测、重开、转换和磁盘字节正确；脏重开有保存/放弃/取消；状态栏准确 |
| L00 | Text 跨 Tier 同界面 | 同一文本切片分别进入 S1、S2 只读、S3 只读；宽窄/DPI/主题 | 共用 EditorSurface 视觉契约；字体/gutter/正文/滚动条/选择/高亮/菜单几何一致；只允许可编辑能力和档位文案不同 |
| H01 | Hex 统一布局 | B1/B2/B3/Secure Binary、宽窄/DPI/主题 | 各 Tier 同一视觉语言；地址/Hex/ASCII 铺满；无大片空白或多余控件 |
| H02 | Hex 滚动/地址 | 0/25/50/75/100%、滚轮/Page/方向、窗口边界/EOF | 恰好一条纵向滚动条；首行地址、global offset、正文一致；不回弹/假 EOF |
| H03 | Hex caret/选择 | Hex 高低 nibble、ASCII、正反向跨行/跨窗、末字节 | 两输入面 caret/selection 镜像同一 source byte range；状态栏 64 位 offset 正确 |
| H04 | Hex 输入模式 | 默认 OVR/显式 INS、切标签/重开、Hex/ASCII 输入 | 模式不泄漏；OVR 长度不变、INS 长度增加；非法字符不破坏投影 |
| H05 | Hex 删除/剪贴板 | Backspace/Delete/Cut/Paste、nibble 中间、selection、边界/EOF | 控制键先按语义处理，不被十六进制字符校验吞掉；目标字节/长度/Undo 正确 |
| H06 | Hex 保存/恢复 | 0/1/15/16/17 B、9 MiB、窗口边界修改、失败/崩溃 | 逐字节保存与重开一致；未修改 no-op；失败保原文件；patch 恢复正确 |
| H07 | Text/Hex 呈现 | Binary→Text→Hex、Text→Hex→Text、dirty/会话恢复 | 只改变呈现不改变 payload；位置/selection 可解释；保存仍按内容身份 |
| L01 | S2 即时只读 | 32 MiB 边界、256 MiB、不同编码/长行 | 首屏及时可操作；左侧全局导航信息存在；浏览不依赖完整加载/索引 |
| L02 | S2/S3 能力边界 | 32 MiB−1/等于/加1、2 GiB−1/等于/加1、解码失败、低内存 | S1 以下可编辑；S2/S3 始终只读且不创建后台编辑装载任务；输入/粘贴/删除/替换/保存修改均不可用，源 hash 不变 |
| L03 | S3/B2/B3 只读 | 2 GiB 边界、保留真实大文件、Text/Hex | 全文可浏览/搜索/有界复制；编辑/替换显式禁用；不创建完整内存副本 |
| L04 | 大文件连续浏览 | 拖动、滚轮、pixelDelta、Page、wrap、横向长行、静置 | 单滚动；窗口间无 gap/重复/回跳；物理 EOF 可达；UI heartbeat 持续 |
| L05 | 文件增长/轮转 | 追加、截断、同路径替换、搜索运行中变化 | 新尾可达；位置钳制；generation 正确失效/增量；无持久锁阻止外部替换 |
| S01 | 当前文件查找 | Text S1/S2/S3/Secure、Hex B1/B2/B3；literal/regex/bytes；所有合法 case/whole/wrap 组合；非法组合真实点击 | 每个选项只改变其声明语义；冲突控件灰显且不可改变；Find Next/Previous/All ranges 同序；F3/Shift+F3 选中真实正文 |
| S02 | 查找全部/Dock | 0/1/多命中、同一行多命中、head/middle/边界/EOF | 渐进分组、上下文和高亮正确；Dock 可见/关闭/重开状态正确 |
| S03 | Dock 双击导航 | 跨标签/Provider、快速 A/B/C、旧 revision、关闭目标 | 激活正确目标；最后请求胜出；正文可见 selection 精确；旧结果拒绝跳转 |
| S04 | 全部打开文件 | 混合 Text/Hex/Secure/locked/S1–S3、运行中增删标签/取消 | 显式 scope；受控并发和增量结果；跳过有原因；取消停止增长且保留完成项 |
| S05 | 搜索失效/增量 | 编辑 debounce、S3 追加、截断/替换、重跑 | 只影响变化 target；generation/revision 不混；旧高亮/结果不误导航 |
| S06 | 替换 | 当前/全部打开、单次/全部、空/短/长/Unicode、literal 元语法、regex 编号/命名捕获、eligible/skipped、失败/Undo/Redo | 当前选中项不跳过；literal 不展开；regex S1/L2 同语法；错误 mutation 前拒绝；每文档一个 Undo；成功不自动保存 |
| C01 | 首次安全身份 | 建立、恢复密钥展示/验证、取消、错误密码 | 流程闭环且返回文档；秘密不进日志/设置；错误不建立半身份 |
| C02 | 七天会话 | 有效、过期、重启、系统锁屏、显式锁定 | 有效期内加解密零提示；过期才询问；显式锁定清 capsule；状态不误导 |
| C03 | 安全 Note | 新建/打开/编辑/搜索/保存/崩溃恢复 | 解密后拥有完整 Text 体验；普通操作不出现保险箱分叉；密文/恢复点正确 |
| C04 | 外来/历史凭据 | 当前 keyVersion、历史版本、外来密码、恢复密钥、错误凭据 | 仅当前会话不能解密时询问；成功密封会话能力；错误不改文件 |
| C05 | 锁定与内存清理 | 文档、Undo、搜索、账号详情、剪贴板租约、后台任务 | 敏感明文和对象及时清理；恢复重新解密；无明文临时文件/任务标题泄漏 |
| A01 | 账户工作区/CRUD | 空状态、分组、创建/编辑/保存、切换/重启 | 与文档并列中央工作区；列表/详情一致；Ctrl+S/dirty 状态准确 |
| A02 | 账户搜索 | 标题/用户名/URL/标签/备注、无/多结果、键盘 | 只搜非秘密字段；选择/详情导航正确；不会进入文档搜索 Dock |
| A03 | 密码/TOTP | 生成/强度、遮罩/显示、复制、30 秒、剪贴板被用户替换 | 秘密短时可见；只清除匹配内容；TOTP 更新正确；日志/截图默认不泄露 |
| A04 | 历史/回收站 | 多次轮换、恢复旧版、反向恢复、删除/恢复/永久删除 | KeePass 历史完整；误删可恢复；永久删除明确确认且数据库持久化正确 |
| A05 | 外来 KDBX | 密码/key file、七天重开、外部冲突、保存失败 | 兼容打开；不创建空库；原子保存；错误/冲突保留旧库与当前修改 |
| U01 | QAction/capability | Empty/Text/Hex/S2 ReadOnly/S3 ReadOnly/Accounts/Security | 菜单/工具栏/快捷键同一动作；enabled/checked/visible 与实际可执行能力一致 |
| U02 | 状态栏 | 各 Provider、无标签/Accounts、最长文案、临时消息、任务 chip | 位置/编码/语言/大小/Unicode 总字符数（Hex 为字节数）/tier/dirty 准确；文字不重叠、裁切或残留旧标签信息 |
| U03 | 后台任务 | 索引/搜索/保存、排队/运行/取消/失败/完成；S2/S3 打开 | 阶段与真实状态一致；取消及时；S2/S3 打开不产生编辑装载任务；0 任务无线程噪音；详情脱敏且可清理 |
| U04 | Dock/工具栏/状态栏 | 显隐、关闭重开、中央工作区切换、重启恢复 | 布局和动作勾选一致；Accounts/Security 不显示无关 Dock；返回文档恢复合理 |
| U05 | 主题/DPI/无障碍 | System/Light/Dark/高对比、900–1800px、100–200% DPI | 正文/菜单/焦点/禁用态可读；核心动作可达；Tab 顺序、accessible name 正确 |
| U06 | 设置/会话 | 正常/损坏设置、多显示器移除、混合标签、缺失文件 | 安全默认回退；窗口不离屏；渐进恢复且当前/位置/呈现/dirty 正确 |
| U07 | 帮助/供应链 | About、版本、GPLv3、Qt/QScintilla/KeePassXC/Botan | UI 信息与实际构建一致，源码/许可入口有效，不展示未落地能力 |

## 3. 交叉维度

每次测试只组合与改动相关的维度；P0 数据、安全、滚动、输入和异步竞态至少做三元组合，
其余优先两两组合，避免无价值的全笛卡尔积。

| 维度 | 取值提醒 |
|---|---|
| 文件/内容 | 未命名安全草稿、Secure、Plain、KDBX；Text/Binary/Account |
| 大小/Provider | 0/1 B、32 MiB−1/32 MiB/32 MiB+1、S2、2 GiB−1/2 GiB/2 GiB+1、B1/B2/B3 |
| 编码/结构 | UTF-8/BOM、UTF-16LE/BE、GB18030；CRLF/LF/CR；空行、超长行、无尾换行 |
| 输入 | 鼠标、键盘、IME、滚轮、pixelDelta、拖放、CLI；selection/INS/OVR/Hex nibble |
| 布局 | 900/1400/1800px、100/125/200% DPI、四种主题、Dock 开关、最长本地化文案 |
| 生命周期 | clean、dirty、saving、conflict、read-only、stale、locked、recovering、error |
| 异步 | 正常、取消、失败、A/B/C 乱序、目标关闭/新增、源追加/截断/替换 |
| 持久化故障 | create/write/flush/replace 失败、权限不足、磁盘空间、AppData 丢失、设置损坏 |

## 4. 历史缺陷哨兵

这些不是临时 bug case，而是永久回归入口；相关架构改动必须优先验证。

| ID | 历史缺陷 | 必须触发失败的错误实现 |
|---|---|---|
| K01 | 编辑区双滚动/外内位置分离 | 出现第二条可见纵向滚动条，或 thumb、首行内容、地址不一致 |
| K02 | S2/S3 最大值不是真 EOF | 拖到最大后仍能连续向下很久，或尾 marker 不可见 |
| K03 | wrap 卡住/无法到底 | wrap 后拖动阻塞、display/source 映射错、EOF 不可达 |
| K04 | 状态栏文字重叠 | 任意宽度/DPI/消息/任务组合中 visible segment 几何相交或文本不可读 |
| K05 | 普通输入变覆盖 | 未显式 OVR 时输入替换了 caret 后已有字符，或模式跨标签泄漏 |
| K06 | Hex Backspace/Delete 语义错误 | 控制键未修改预期字节/长度、被当成非法十六进制字符，或文件起点 Backspace 错删首字节 |
| K07 | B1/B2/B3 Hex 像不同产品 | 同字节/窗口下布局、地址、bytesPerRow、选择或滚动语义无设计理由地不同 |
| K08 | Hex 大片空白/裁切 | Hex/ASCII 未利用有效区、窄窗裁 nibble、resize 产生不可达区域 |
| K09 | 搜索结果跳错/旧请求覆盖 | Dock 结果未选中实际关键词，或 A/B/C 乱序后不是 C 胜出 |
| K10 | 大文件预览假加载 | 任务显示完成但正文未更新、滚动回跳、看不到读取/解码/附加状态 |
| K11 | 小文件保存产生固定备份 | clean Save 改 mtime/identity，或普通小文件留下固定临时/备份副本 |
| K12 | 测试污染用户环境 | GUI 测试读取/回写真实 AppData，或在项目外普通目录遗留文件 |
| K13 | S2/S3 像另一款查看器 | 同一文本在 S1/S2/S3 的字体、gutter、滚动条位置、选择、高亮、菜单或切换前后 geometry 出现无能力理由的差异 |
| K14 | 查询选项只变 UI、不变语义 | 正则开/关在判别语料上结果相同；关闭正则仍解释元字符；或查找/查找全部/替换、S1/S2/S3 使用不同选项语义 |
| K15 | Hex caret 落在非字节位置 | 左右/Home/End 后 caret 位于分隔空格或文本行后；Hex/ASCII offset 不一致 |
| K16 | Hex OVR 偷偷改变长度 | OVR 在 EOF 粘贴变成 INS，或粘贴超过剩余空间时追加多余字节 |
| K17 | Hex 双投影/状态历史不同步 | Undo/Redo 后 Hex 与 ASCII 内容不同，或实际 OVR/INS 与长期状态栏不一致 |
| K18 | Hex 选择按投影字符而非源字节 | Shift/鼠标拖选只覆盖半个 nibble 或分隔符；Hex/ASCII/复制删除得到不同 byte range；拖选事件递归卡死 |
| K19 | Hex 命令忽略当前输入面 | ASCII 栏有焦点时主菜单 Copy/Cut/Paste 仍按 Hex 文本解释，或反之 |
| K20 | 查询状态产生非法组合 | Regex+整词、Regex+Hex、Hex+大小写/整词仍可同时勾选，或 Binary/只读/锁定文档仍启用替换 |
| K21 | Replace 跳过当前命中 | 用户已选中命中后点击 Replace 却先跳到下一处；或 L1 依赖另一套 Scintilla 查找会话导致无修改 |
| K22 | L2 批量替换显示 clean | PatchList 已改变但标签 dirty、revision、Undo/Redo、scroll range 或正文 repaint 未更新 |

## 5. 参数契约展开规则

矩阵中的 `S01/S06/U01` 不枚举所有控件组合。执行时按以下规则自动展开，新增控件同样适用：

| 参数 | 最小判别语料 | 必须不同的结果 |
|---|---|---|
| 正则关闭/开启 | 查询 `a.b`；正文同时含 `a.b`、`axb` | 关闭只命中 `a.b`；开启两者均命中；非法 `[` 仅开启时报告正则错误 |
| 大小写关闭/开启 | 查询 `Foo`；正文含 `Foo`、`foo` | 关闭命中两者；开启只命中同大小写 |
| 整词关闭/开启 | 查询 `cat`；正文含 `cat`、`scatter` | 关闭命中两者；开启只命中独立词 |
| 循环关闭/开启 | caret 位于最后命中之后，正文前部有命中 | 关闭停在边界；开启回到首个命中 |
| 当前/全部打开 | 两标签各有同一查询，另有不匹配标签 | 当前只返回活动标签；全部按受控并发返回两个目标 |
| 文本/十六进制字节 | 文本 `41 42` 与原始字节 `0x41 0x42` 可区分 | 文本按字符查询；bytes 按字节序列查询且互斥选项生效 |
| 替换模板 Literal/Regex | replacement `$1\\n${name}\\1`，Regex 含编号/命名捕获 | Literal 逐字符写入；Regex 按统一模板展开；缺失捕获在修改前失败 |

每个参数至少贯穿“查找下一个 + 查找全部”；影响替换时再贯穿“单次替换 + 全部替换”。
同一契约必须在适用的 S1/S2/S3/Secure Provider 上保持语义一致，Provider 只允许改变执行策略，
不得重解释 UI 参数。发现新的细节缺陷时，先补一组能区分正确/错误实现的语料和历史哨兵，
再修实现；不要把每个具体步骤扩写成长用户旅程。

自动执行采用四类 oracle：明确的预期 byte range、不同 UI 路径结果一致、等价变换
（例如 `literal(q) == regex(escape(q))`）以及包含/单调关系（例如 whole-word 结果是非 whole-word
结果的子集）。首个试点为 `tc_fr_018_regex_contract_model_runs_gui_variants`：由契约表循环生成真实
GUI 操作，覆盖 literal、regex、escaped regex、非法表达式、Find All、重复 Find Next 和边界行为。

搜索/替换专项的完整有限状态、query/command/Dock/Replace/Provider 矩阵由
`SEARCH_REPLACE_CONTRACT_MATRIX.md` 管理。当前自动证据包括 `tc_fr_019` 的 4 种目标上下文下
Literal 8、Regex 4、Hex 2 合法组合与冲突控件真实点击，以及 `tc_fr_020`–`tc_fr_026` 的
导航、Hex 格式、S1/L2 捕获替换、空/短/长/Unicode/Literal replacement、原子失败和 Undo/Redo；
`tc_fr_027` 进一步在保留的 4.9 GB S3 样本上覆盖正式对话框、异步 mmap 准备、结果 Dock、
64 位 byte range、真实双击跳转和单滚动条正文选区；`tc_fr_028` 从全新 S3 标签覆盖首次
Find Next、busy/cancel 状态、可见 Provider 选区、重复 forward/backward、非循环边界和 wrap；
`tc_fr_029` 以 60,050 个分行结果覆盖常驻上限、临时 spool、稀疏虚拟尾行读取及尾项双击；
`tc_fr_030` 以 2 GiB+ 稀疏有效 UTF-8 Text 覆盖 Regex/case、2 GiB 后 64 位位置、Dock/正文
双击及 S3 替换禁用边界；`tc_fr_031` 直接在保留的 4.9 GB Binary S3 上覆盖默认仅循环查找
的 Literal 文本输入，保证未勾正则时不会暗中触发 PCRE2 的 UTF-8 要求。
同一契约由 `tc_fr_032` 用含非法 UTF-8 的小型 Binary 做可移植快速门禁，并验证 ASCII
不区分大小写。

复杂模块使用 `GuiContractMatrix` 自动生成合法的 pairwise 状态组合，并在失败信息中输出稳定 case ID。
Hex 首个专项为 `tc_hex_delete_backspace_pairwise_contract`，自动覆盖 Hex/ASCII 输入面、OVR/INS、
Delete/Backspace、无选择/正向选择/反向选择、起点/中间/EOF；每案以独立原始字节模型计算预期，
同时验证 raw bytes、Hex 投影、ASCII 投影、dirty、Undo/Redo 和单滚动条约束。
Hex 的完整有限边界、操作原语及自动化强度由 `HEX_EDITOR_CONTRACT_MATRIX.md` 管理；专项修复不得
只补一条复现步骤，必须选择对应 `HX-*` 契约并展开相关状态组合。

## 6. 实际工作记录格式

每次验证只增加简短记录，不扩写预设旅程：

```text
Change: <改动摘要>
Rows: H02 H05 U02 + K01 K04 K06
Dimensions: B1/B2/B3; 16/17B + 9MiB; 900/1400px; mouse+keyboard; OVR/INS
Actions: <本次实际执行的关键输入>
Observed: <可见内容、滚动、状态、任务>
Data: <buffer/byte/hash/磁盘/恢复结果>
Performance: <首屏、响应、峰值或不适用>
Evidence: <测试名、日志、截图>
Result: PASS / FAIL / BLOCKED，失败进入问题清单
Cleanup: <临时数据、进程、AppData>
```

测试方法可以随着实现和新知识改进；功能矩阵、交叉维度和历史哨兵保持稳定，负责提醒不能漏掉
什么，而不是限制具体怎么测。
