# Notepad Safe — 设计文档

> 基于 Qt 6.11 静态库 + QScintilla 2.14.1 的独立 Windows 加密笔记本编辑器。
> 目标：Notepad++ 级编辑体验 + 加密存储，单 exe 无 DLL 依赖。
> 项目已从原 Credential Vault 仓库独立为本产品仓库。

## 1. 技术栈 & 产物

| 项 | 值 |
|---|---|
| 语言 | C++ 17（MSVC） |
| UI 框架 | Qt 6.11.x 静态库（/MT） |
| 编辑器控件 | QScintilla 2.14.1（Scintilla for Qt） |
| 构建系统 | CMake 3.22+ / Ninja |
| 编译器 | MSVC 2022+（或兼容 vcvars64） |
| 产物 | `notepad-safe.exe`（单文件，静态链接 ~24MB，无 DLL 依赖） |
| 应用名 / 组织名 | QSettings / QStandardPaths 统一使用 `NotepadSafe` |

## 2. 模块结构

```
notepad-safe/
├── CMakeLists.txt              # 项目根：目标 notepad-safe + 5 个测试
├── build.bat                   # 一键构建（vcvars64 → cmake → ninja）
├── run_tests.bat               # 一键运行全部 CTest
├── main.cpp                    # 入口：QApplication + NotepadSafe 组织/应用名
├── MainWindow.h/cpp            # 主窗口：QTabWidget + 菜单 + 工具栏 + 状态栏 + Dock
├── EditorTab.h/cpp             # 单个编辑器标签：QsciScintilla + 文件状态 + 编码
├── TextEncoding.h/cpp          # 编码检测（UTF-8/GBK/UTF-16 BOM + 启发式）
├── FindReplaceDialog.h/cpp     # 查找/替换对话框（正则、大小写、整词）
├── SearchResultsDock.h/cpp     # 搜索结果 Dock（QDockWidget + QTreeWidget）
├── SessionManager.h/cpp        # 会话管理（自动缓存未保存文件 + 启动恢复）
├── notepad_safe.png            # 产品根原始图标（1254×1254）
├── resources/
│   ├── notepad_safe.png        # 拷贝：供 qrc 运行时 setWindowIcon 使用
│   ├── app.ico                 # 6 尺寸 16/32/48/64/128/256，EXE 图标资源
│   ├── app_icon.rc             # RC：IDI_APPLICATION + VS_VERSION_INFO
│   ├── app_resources.qrc       # QRC：运行时 PNG 图标
│   └── make_ico.py             # 由 notepad_safe.png 重新生成 app.ico 的辅助脚本
├── third_party/
│   └── QScintilla_src-2.14.1/  # QScintilla 源码 + 静态库 qscintilla2_qt6.lib
├── tests/
│   ├── test_textencoding.cpp   # 编码检测单元测试
│   ├── test_search.cpp         # 正则搜索逻辑测试
│   ├── test_session.cpp        # 会话序列化测试
│   ├── test_settings_ui.cpp    # 设置持久化 & 菜单 UI 勾选同步（防回归）
│   └── test_largefile.cpp      # 大文件分块加载回归测试
└── docs/
    ├── DESIGN.md               # 本文件
    └── README.md               # 使用 & 构建说明（根 README.md 也有）
```

## 3. 功能清单（Notepad++ 对标）

### M1 — 已实现

| 功能 | 实现方式 | 状态 |
|---|---|---|
| 多标签页 | QTabWidget + EditorTab，关闭按钮、中键关闭 | ✅ |
| 文件拖放 | QMainWindow::dropEvent + QsciScintilla::dropEvent | ✅ |
| 编码检测 | BOM 检测 + UTF-8 启发式 + GBK 回退 | ✅ |
| 原子保存 | 写 .tmp → rename（防崩溃损坏） | ✅ |
| 查找/替换 | FindReplaceDialog（正则、大小写、整词、向前/向后） | ✅ |
| 搜索结果 Dock | QDockWidget + QTreeWidget，双击跳转 | ✅ |
| 会话恢复 | SessionManager 序列化未保存标签到 JSON | ✅ |
| 状态栏 | 行/列、编码、文件大小、修改状态、语法类型 | ✅ |
| 语法高亮 | C++/Python/JSON/HTML/SQL/XML/YAML/Markdown/纯文本 | ✅ |
| 行操作 | 复制行、删除行、上移/下移行、行排序 | ✅ |
| 书签 | Scintilla marker + 快捷键切换/导航 | ✅ |
| 行号 + 当前行高亮 | QsciScintilla 内置 | ✅ |
| 自动缩进 + 智能缩进 | QsciScintilla autoIndent | ✅ |
| 80 列标尺 | QsciScintilla EdgeLine | ✅ |
| 最近文件列表 | QSettings 存储，最多 10 个 | ✅ |
| Dark Mode | 跟随系统主题 + 手动切换 | ✅ |
| 字体一致性 | 全局 Consolas 10pt，可配置 | ✅ |
| 高 DPI | Qt 6 自动缩放 + manifest 声明 | ✅ |
| 按需滚动条 | 内容未溢出窗口时不显示横/纵滚动条（防回归） | ✅ |
| 设置-菜单勾选同步 | 设置恢复后强制同步菜单勾选（防回归） | ✅ |
| 大文件分块加载 | 进度对话框 + QsciScintilla 只读先关再追加 | ✅ |

### M2 — 规划

| 功能 | 说明 |
|---|---|
| 分屏视图 | 横向/纵向拆分，同一文件双视图 |
| 文档地图 | 缩略图侧栏（Scintilla 的 minibuffer） |
| 列编辑模式 | Scintilla virtual space + 多光标 |
| 多选多光标 | Scintilla 5.x 多选 API |
| 代码折叠 | QsciScintilla 折叠开关 |
| 宏录制 | Scintilla 宏 + QsciMacro |
| 打印 | QsciPrinter + QPrintDialog |
| 十六进制编辑 | 独立 HexView 窗口 |
| 用户定义语法 | JSON 配置 + 动态词法器 |
| 文件比较 | 双 EditorTab 差异对比 |
| 拼写检查 | hunspell 集成 |

## 4. 关键设计

### 4.1 编码处理

- 打开文件：先读前 4 字节判断 BOM（UTF-8/UTF-16LE/UTF-16BE），无 BOM 则尝试 UTF-8 严格解码，失败回退 GBK
- 保存文件：默认 UTF-8 无 BOM；用户可在"编码"菜单切换（UTF-8 BOM / UTF-16LE / GBK / 系统默认）
- 内部统一用 QString（UTF-16），与 QsciScintilla 交互用 `text()` / `setText()`

### 4.2 会话恢复

- 路径：`%LOCALAPPDATA%\NotepadSafe\session.json`
- 内容：每个未保存标签的 {临时文件路径, 原始文件路径, 光标位置, 语法类型}
- 临时文件：未保存标签的内容定期（30s 或失焦时）写入 `%LOCALAPPDATA%\NotepadSafe\autosave\*.tmp`
- 启动时：读取 session.json，恢复未保存标签，标签名加 "(已恢复)" 后缀
- 加密文件不在会话中保存内容（只存路径，需重新解锁）

### 4.3 查找/替换

- 非模态对话框（可一边编辑一边查找）
- 正则模式用 QRegularExpression（PCRE 兼容）
- "查找全部"结果输出到 SearchResultsDock，双击跳转
- 高亮所有匹配（Scintilla indicator + 持久标记）
- 替换/全部替换支持回溯引用（`\1`、`$1`）

### 4.4 拖放

- 主窗口接受文件 URL 拖放 → 在新标签打开
- EditorTab 内的 QsciScintilla 接受文本拖放 → 插入光标位置
- 拖出到桌面：不支持（避免误操作）

### 4.5 原子保存

```
1. 写到 currentFile + ".tmp"
2. flush + fsync
3. rename .tmp → currentFile（Windows 用 MoveFileEx + REPLACE_EXISTING）
4. 失败回退：保留 .tmp，提示用户
```

### 4.6 设置 & 菜单勾选同步（防回归）

> 目标："恢复自动换行设置后，菜单勾选 ✔ 必须与实际行为一致"。  
> 根因：之前只把值写入成员变量，没有回写到 menu action 的 checked 状态。

流程（见 `MainWindow::loadSettings` → `syncGlobalMenuCheckStates`）：

```
createActions()            // 新建所有 QAction（默认 checked=false 或默认值）
    ↓
createMenus()              // action 插入菜单（此时 UI = 默认，非持久化值）
    ↓
loadSettings()             // QSettings 读回，覆盖成员 m_wordWrap / m_rulerMode 等
    ↓
syncGlobalMenuCheckStates()  // 按成员值反向 setChecked() 到所有 action
                              // （本函数在任何修改设置的地方都要调用）
    ↓
applyDarkMode() / restoreSession()  // 其他 UI 应用
```

`test_settings_ui.cpp` 模拟 "开程序 → 改设置 → save → 重启程序 → 再读" 的生命周期，
断言成员值与 action 勾选值始终一致。

### 4.7 按需滚动条（防回归）

原则：只有真的需要滚动才显示滚动条。

- 纵向：内容行数 > 可见行数才显示
- 横向：开启自动换行 → 永远不显示；关闭自动换行 → 最宽行长 > 可见列数才显示
- 应用：Scintilla 的 `SCI_SETSCROLLWIDTHTRACKING(true)` + `wrapMode` 关联 + `ScrollWidth` 跟踪最长行
- 空文件：自动换行=关，内容为空 → 两条滚动条都不显示

### 4.8 大文件分块加载（当前 L1 实现，L2 在 §5 方案中）

当前实现（> 20MB 触发）：

```
QProgressDialog 打开（模态但 processEvents 不卡 UI）
    ↓
分块读 1MB → UTF-8 decode → 将 editor 先 setReadOnly(false) 再 SCI_APPENDTEXT
                                           ↑
                           关键修复：Scintilla 在 readOnly 下 InsertString 直接返回 0，
                                    导致"加载完成后编辑框无内容"。
    ↓
close progress → setReadOnly(按文件类型/用户设置) → sendScintilla(Scintilla::SCI_EMPTYUNDOBUFFER)
```

`test_largefile.cpp` 覆盖：
- readOnly=true 下 SCI_APPENDTEXT 静默失败（基线 bug）
- readOnly=false 下分块追加 100 万行 ≈ 160MB 正确、行数匹配、光标首位置顶

## 5. 大文件架构方案（L1/L2 分级 + mmap + Patch 分级溢出 + 动态阈值）

> 状态：已评审、方案确定、代码未全部实现（当前 §4.8 仍是 L1 分块加载，L2 VirtualFileView 为下一阶段）。

### 5.1 设计原则

1. **优先完成工作**：任何场景下，检测到压力 → 提示 → 保存（落盘）→ 继续执行。
   不以"内存上限"为由拒绝用户的编辑/替换任务。
2. **匹配机器性能**：所有阈值在启动时探测物理 RAM 和 CPU 核心数后动态决定，
   不设人为硬上限；32GB 机器自然比 8GB 机器跑得更远。
3. **整体考虑**：打开、滚动、复制、粘贴、编辑、搜索、替换、跳转、书签、撤销、语法高亮，
   所有功能都要在 1GB+ 场景下定义明确行为（含 L1/L2 差异）。
4. **UI 永不卡**：主线程单次执行 < 50ms；任何 > 500ms 任务显示进度条 + 取消按钮；
   取消响应 < 100ms；Windows 永不显示"未响应"。

### 5.2 分级模式（L1 完整编辑 / L2 虚拟视图）

**切换阈值（动态）**：

```cpp
struct DynamicThresholds {
    qint64 RAM_BYTES;    int CPU_CORES;

    // L1/L2 切换阈值：小文件进 Scintilla 完整编辑，大文件进 VirtualFileView
    qint64 L1_EDIT_LIMIT;    // = min(RAM_BYTES/128, 128*1024*1024)
                              // 1GB RAM: 8MB, 8GB: 64MB, 16GB+: 128MB（顶）

    // Patch L1 热内存上限
    qint64 PATCH_L1_LIMIT;   // = min(RAM_BYTES/20, 2GB)
    qint64 PATCH_SOFT_WARN;  // = PATCH_L1_LIMIT * 0.8（达到时非阻塞提示）

    // findAll 结果内存中上限（超出序列化到溢出文件，Dock 虚拟化读取）
    qint64 MAX_MATCHES_MEM;  // = (RAM_BYTES/16) / 50  (每条 ~50B)
                              // 8GB: ~10M 条, 16GB: ~21M, 32GB: ~42M

    int    REPLACE_BATCH_SIZE = 1000;
    qint64 PASTE_LIMIT;      // = min(RAM_BYTES/32, 200MB)
    int    SEARCH_THREADS;   // = min(CPU_CORES, 8)
};
```

**L1/L2 模式对照表**：

| 维度 | L1 完整编辑（< L1_EDIT_LIMIT） | L2 虚拟视图（≥ L1_EDIT_LIMIT） |
|---|---|---|
| 文件进入方式 | `setText` / `SCI_APPENDTEXT` 完整加载 | `QFile::map()` 内存映射，OS 按需 4KB/页 |
| 打开耗时（1GB） | N/A（1GB 永远不会进 L1） | **< 2s**（SSD，含 LineIndex） |
| 内存占用（1GB） | N/A（不会进 L1） | **≤ 150MB**（mmap + LineIndex + patch） |
| 底层控件 | `QsciScintilla` | `VirtualFileView : QAbstractScrollArea`（自绘） |
| 滚动帧率（1GB） | N/A | **≥ 55fps**（只绘制可见 50~100 行） |
| 编辑/粘贴 | Scintilla 完整支持（多光标等） | Patch 系统，原文件只读 + patch 增量 |
| 代码折叠 | 完整支持 | **不支持**（1GB 成本太高） |
| 语法高亮 | QsciLexer 全文 | 可见行自定义轻量词法器（<2ms/行） |
| 书签 | Scintilla marker | `QSet<qint64>`（行首 byteOffset），自绘 |
| 撤销/重做 | Scintilla Undo | Patch UndoGroup 栈：replaceAll 每 1000 条 = 1 原子 |
| 搜索/替换 | Scintilla 原生 | 多线程分块搜索（§5.5） + Patch 分段替换 |
| 保存 | QSaveFile 原子（已有） | PatchList::applyAndSaveThenReset 流式写 |

### 5.3 VirtualFileView + LineIndex（L2 基础设施）

```cpp
// ---------- LineIndex：行偏移索引（后台构建 + delta 压缩）----------
class LineIndex {
    QByteArray deltaEncoded_;             // varint 编码的行偏移差值（约 2B/行 → 1GB 10M 行 ≈ 20MB）
    QVector<qint64> blockIndex_;          // 每 1000 行一个绝对偏移锚点（加速二分）
    qint64 totalLines_ = 0;
public:
    bool buildFromMmap(const uchar*, qint64, Fn<void(qint64,qint64)> progress, atomic<bool>& cancel);
    void byteToLineCol(qint64 byteOffset, int* line, int* col) const;
    qint64 lineToByteOffset(int line) const;
    qint64 totalLines() const { return totalLines_; }
};

// ---------- VirtualFileView：自绘滚动视图 ----------
class VirtualFileView : public QAbstractScrollArea {
    QFile* mmapFile_ = nullptr;
    uchar* mmapBase_ = nullptr;  qint64 mmapSize_ = 0;
    LineIndex lineIndex_;        PatchList patches_;
    qint64 cursorByte_ = 0;      qint64 selAnchorByte_ = 0;
    QSet<qint64> bookmarks_;
public:
    bool openFile(const QString&);      // mmap + 后台 LineIndex（进度条 + 取消）
    void remapFile(const QString&);     // 保存后 unmap → mmap → 重建 LineIndex
    void closeFile();
    bool applyAndSaveThenReset(const QString& out,
        std::atomic<bool>& cancel, QString* err);
    ... paintEvent, drawVisibleLines, resizeEvent, 光标/选中/输入 ...
};
```

### 5.4 PatchList（分级存储 + UndoGroup 原子 + 自动溢出）

```cpp
struct Patch {
    qint64     byteOffset;   // 原 mmap 字节偏移（升序唯一）
    qint32     oldLength;    // 0 = 纯插入
    QByteArray newText;      // 空 = 纯删除
};

struct UndoGroup { QVector<int> patchIndices; bool applied = true; };

class PatchList {
    QVector<Patch> l1Hot_;    // 热内存，按 byteOffset 升序
    qint64        l1Bytes_ = 0;
    struct ColdPatchFile {     // 溢出到磁盘的 mmap 只读 patch 段
        QString filePath; QFile* file; uchar* mmapPtr;
        qint64 fileSize, minOffset, maxOffset; int count;
    };
    QList<ColdPatchFile> l2Cold_;
    QList<UndoGroup> undoStack_, redoStack_;
    QUuid patchDirId_;
public:
    static qint64 dynamicL1Limit();                  // RAM/20 封顶 2GB
    bool insert(qint64, const QByteArray&, QString*);
    bool remove(qint64, qint32, QString*);
    bool replace(qint64, qint32, const QByteArray&, QString*);
    void beginUndoGroup(); void endUndoGroup();
    bool undo(QString*); bool redo(QString*);
    qint64 l1Bytes() const; qint64 l1Limit() const;
    bool needSoftWarn() const;                       // 80% 非阻塞提示
    QByteArray renderRange(const uchar*, qint64 start, qint64 end) const;
    bool applyAndSave(const uchar*, qint64, const QString& out,
        Fn<void(qint64,qint64)> progress, atomic<bool>& cancel, QString* err);
    void resetAfterSave();                           // 保存后全部清零
private:
    void spillIfNeeded();                            // 溢出最旧 25% 到 l2
    struct MergedIter; MergedIter mergedIterator() const;
};
```

**自动溢出策略**：

```
每次 insert/replace 结束：
  if (l1Bytes_ > dynamicL1Limit()):
      spillCount = l1Hot_.size() / 4
      编码 [0, spillCount) → patch_XXXX.bin（QSaveFile → mmap 只读推入 l2Cold_）
      l1Hot_.erase(begin, begin + spillCount) → l1Bytes_ -= Σ size
```

### 5.5 LargeFileSearch（多线程分块搜索 + Dock 虚拟化溢出）

```
findAll(pattern, regex, cs, wholeWord):
 Step 1: 按 16MB/块 + 4KB overlap 划分 N 块
 Step 2: QThreadPool 投递 N 个 SearchChunkTask（min(CPU,8) 线程）
           · re.globalMatch，每 1000 条 → partialMatchBatch 信号
         主线程：
           · 前 MAX_MATCHES_MEM 条 → 内存
           · 超出部分 → matchesOverrun.bin（序列化）
           · 每批进度条 + 状态栏 + 取消检查
 Step 3: searchFinished(totalMatches)
         若 >100 万条 → 状态栏软提示"建议精确正则"（不截断）

 Dock 虚拟化（SearchResultsModel : QAbstractItemModel）：
   · rowCount() 立即返回（O(1)）
   · data(idx) 若超内存 → overrun.bin 二分读取（<1ms，仅可见行调用）
   · doubleClicked → view.scrollToByte(match.byteOffset, true, match.length)
```

### 5.6 replaceAll（分段原子 + 中间保存 + 保存后继续）

```
Step 1 [后台扫描收集] → 所有匹配 list（内存 + overrun.bin） progress 0→10%
Step 2 [压力检测 + 用户选择（非阻塞）]
    if (估算替换大小 > PATCH_L1_LIMIT * 0.5) 弹非阻塞：
       [立即保存并清空 patch 后继续（推荐）]  [直接执行替换]  [取消替换]
Step 3 [分段 1000 条 = 1 UndoGroup = 原子]（10%→90%）
    · 按 byteOffset 从大到小替换（避免前面 patch 影响后面 offset）
    · 每批 EndUndoGroup → Ctrl+Z 一次撤销本段
    · 中途压力达 80% → 非阻塞提示中间保存（保存后重映射 + patch 清零 → 继续）
    · Esc / 取消按钮 → 停止，保留已替换 batch（Ctrl+Z 可逐段撤销）
Step 4 [收尾]
    progress 100%，若替换 >10 万条 → 状态栏常驻 [立即保存]
```

**applyAndSaveThenReset（保存后继续）关键流程**：

```
A. 4MB/块流式写 outPath.tmp：
     mergedIterator() 同时推进 mmap 光标 + L1/L2 patch 合并
     取消 → saveFile.cancelWriting() 回退
     commit 原子 rename
B. remapFile：
     unmap 旧 → 销毁 lineIndex_ (~20MB)
     patches_.resetAfterSave() → 清空 patch（删临时文件）
     mmap 新文件 → 后台 LineIndex 重建（进度条 + 取消）
     1GB 在 SSD 全程 < 10s
```

### 5.7 量化验收标准

| 指标 | 8GB 笔记本（DDR4 + SATA SSD） | 32GB 工作站（DDR5 + NVMe SSD） |
|---|---|---|
| 1GB 文件打开 | < 2s | < 1.5s |
| 1GB 滚动帧率 | ≥ 50fps | ≥ 60fps |
| findAll 1GB UTF-8 | < 15s | < 8s |
| findNext 首次命中（前 100MB 内） | < 500ms | < 300ms |
| replaceAll 100 万处 | 可完成（中间自动提示保存 1-2 次 < 2 min） | 内存内 < 1 min |
| 保存 1GB 文件 + 重映射 | < 30s（SATA）/ < 10s（NVMe） | < 10s（NVMe） |
| patch L1/L2 能力 | 无上限（磁盘允许）+ 透明溢出 | 无上限 |
| findAll 结果支持 | ≥ 1000 万条（Dock 虚拟化） | ≥ 4000 万条 |
| 主线程单次阻塞 | < 50ms | < 30ms |
| 取消响应（任何时刻） | < 100ms | < 50ms |
| replaceAll 单段撤销原子性 | 1000 处替换整体撤销无残留 | 同左 |
| 1GB 编辑内存峰值 | ≤ (150MB + 编辑 patch) | ≤ (150MB + 编辑 patch) |
| exe 体积增长 | < 300KB | < 300KB |

### 5.8 新增模块与实施阶段

**新增文件**（尚未实现，预留）：

| 文件 | 估计行数 | 职责 |
|---|---|---|
| `VirtualFileView.h/cpp` | ~1000 行 | 自绘滚动视图（paintEvent/滚动/光标/选中/跳转/书签/编辑/语法高亮） |
| `LineIndex.h/cpp` | ~300 行 | varint delta 压缩行偏移索引 + 后台构建 + 二分 |
| `PatchList.h/cpp` | ~600 行 | L1/L2 溢出 + UndoGroup 原子 + renderRange + applyAndSave |
| `LargeFileSearch.h/cpp` | ~500 行 | mmap+patch 分块搜索 + regex 缓存 + 溢出文件 + findAll/findNext/replaceAll |
| `SearchResultsModel.h/cpp` | ~200 行 | QAbstractItemModel 虚拟化：内存/溢出二分读取 |
| `DynamicThresholds.h/cpp` | ~100 行 | GlobalMemoryStatusEx 探测，阈值集中管理 |

**实施分阶段（约 8.5 天）**：

| 阶段 | 交付物 | 耗时 |
|---|---|---|
| P1 L2 基础视图 | DynamicThresholds + VirtualFileView（mmap + paintEvent + 滚动 + 跳转）+ LineIndex 后台构建 | 2 天 |
| P2 搜索系统 | LargeFileSearch + SearchResultsModel Dock 虚拟化 + 双击跳转 + findNext | 2 天 |
| P3 编辑系统 | PatchList（L1/L2 溢出 + renderRange + UndoGroup）+ VirtualFileView 输入/粘贴/删除 + Ctrl+Z/Y | 2 天 |
| P4 替换 & 保存 | replaceAll 分段原子 + 中间保存提示 + applyAndSaveThenReset + remapFile | 1.5 天 |
| P5 测试 | test_largefile 扩充（30+ 断言） + 1GB 稀疏场景 + replaceAll 撤销 20 段验证 | 1 天 |

## 6. 构建配置

- Qt 路径（优先级从高到低）：
  1. 环境变量 `NOTEPADSAFE_QT6_DIR`
  2. CMake 缓存 `NOTEPADSAFE_QT6_DIR`
  3. 默认值 `E:/programs/Qt_6.11.1-static-Release_msvc2026_64/msvc2026_64`
- `CMAKE_MSVC_RUNTIME_LIBRARY`: `MultiThreaded`（/MT，与 Qt 静态库匹配）
- `qt_import_plugins`: QWindowsIntegrationPlugin、QSvgPlugin、QSvgIconPlugin
- QScintilla 静态库：`third_party/QScintilla_src-2.14.1/src/release/qscintilla2_qt6.lib`
- 构建：`build.bat`（vcvars64 + cmake -G Ninja + ninja）
- 测试：`run_tests.bat`（运行所有 CTest）

## 7. 测试

CMakeLists 中定义 5 个测试目标：

| 目标 | 源文件 | 覆盖 |
|---|---|---|
| `test_textencoding` | tests/test_textencoding.cpp | UTF-8/UTF-8 BOM/UTF-16LE/UTF-16BE、往返、overlong、编码名 |
| `test_search` | tests/test_search.cpp | 普通/大小写/正则/整词/空/多行/Unicode/特殊字符/捕获组 |
| `test_session` | tests/test_session.cpp | 空会话加载、有效会话、损坏 JSON、清理、autosave 目录、默认值 |
| `test_settings_ui` | tests/test_settings_ui.cpp | 设置持久化 & 菜单勾选同步（防 UI 状态不同步回归） |
| `test_largefile` | tests/test_largefile.cpp | 大文件分块加载基线 bug（readOnly 下 AppendText 静默失败）+ 修复验证 |

运行方式：

```bat
:: 一键
notepad-safe\run_tests.bat

:: 或通过 CTest
cd notepad-safe\build
ctest --output-on-failure
```

SessionManager 提供测试用构造函数 `SessionManager(const QString &dataDir, ...)`，
避免访问真实 `%LOCALAPPDATA%\NotepadSafe`。

## 8. 变更日志

### v0.3.2 (产品独立化)

从原 Credential Vault 仓库 `apps/qt-notepad` 独立为本仓库，命名统一为 **Notepad Safe**：

- 目录：产品根从 `apps/qt-notepad/` 提升为 `notepad-safe/`（独立仓库根目录）
- 名称统一：`qt-notepad` → `notepad-safe`；`Credential Notepad` → `Notepad Safe`；
  QSettings 组织/应用名从 `CredentialVault/CredentialNotepad` → `NotepadSafe/NotepadSafe`；
  `%LOCALAPPDATA%\CredentialVault\...` → `%LOCALAPPDATA%\NotepadSafe\...`
- 第三方：`third_party/QScintilla_src-2.14.1` 复制到产品内，脱离原仓库相对位置
- 构建：CMakeLists / build.bat / run_tests.bat 路径全部相对产品根；Qt 路径通过环境变量
  `NOTEPADSAFE_QT6_DIR` 可覆盖，不再硬编码唯一路径
- 图标：使用 `notepad_safe.png` 生成 6 尺寸 `resources/app.ico`（EXE 图标 + VS_VERSION_INFO）
  + `resources/app_resources.qrc`（运行时窗口/任务栏图标）
- 文档：本 DESIGN.md 和根 README.md 独立；原 Credential Vault 全局 DESIGN.md 仅保留链接占位
- 版本元信息：RC 中 `VS_VERSION_INFO` 填入公司/文件/产品名 `Notepad Safe` v0.3.1.0

### v0.3.1 (L1/L2 大文件方案确立)

根据评审反馈全面重构 v0.3.0 方案。核心变更：v0.3.0 只考虑"搜索异步"且设 100MB 编辑硬上限，
不符合"优先完成工作、匹配机器性能"原则。v0.3.1 全面改为 L1/L2 分级 + Patch 分级溢出 + 动态阈值。

- 新增 §5.1-5.2 L1/L2 分级：< RAM/128（封顶 128MB）→ Scintilla；≥ → VirtualFileView
- 新增 §5.3 `VirtualFileView`（QAbstractScrollArea 派生）+ `LineIndex`（delta 压缩）
- 新增 §5.4 `PatchList`：L1 内存热 patch + L2 磁盘 mmap 透明溢出，无上限
- 新增 §5.5 `LargeFileSearch` + `SearchResultsModel`（虚拟化 Dock）
- 新增 §5.6 `replaceAll` 1000 条/UndoGroup 分段、中间自动保存提示、保存后继续
- 新增 §5.7 量化验收表（8GB / 32GB 两档）

### v0.3.0 （已废弃，见 v0.3.1 重构）

### v0.2.1

修复：
- `TextEncoding.cpp`：`toSystemCodepage` / `encodeText` 类型转换（char16_t/ushort/WideCharToMultiByte）
- `EditorTab.cpp`：`findAll` `m.capturedText()` → `m.captured(0)`
- `MainWindow.cpp`：缺失 `<QActionGroup>` / `<QStyle>` include；syntaxChanged 信号 lambda 参数类型
- `MainWindow.cpp`：`restoreSession` 重构，新增 `EditorTab::setFilePath`
- `FindReplaceDialog`：`setReplaceMode` 误改"查找:" 标签文本 → 改为正确隐藏替换控件
- `FindReplaceDialog`：新增 `findParametersChanged` 信号，供 F3/Shift+F3 使用
- `MainWindow`：新增 30 秒自动保存定时器，仅在有未保存标签时触发 `saveSessionNow`

新增：
- `EditorTab::setFilePath(const QString &path)`
- `SessionManager(const QString &dataDir, ...)` — 测试用构造函数
- 3 个测试文件：test_textencoding.cpp、test_search.cpp、test_session.cpp
- run_tests.bat
