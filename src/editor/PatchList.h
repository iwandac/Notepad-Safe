#pragma once

#include <QtGlobal>
#include <QVector>
#include <QByteArray>
#include <QString>
#include <QList>
#include <QFile>
#include <functional>
#include <atomic>

// ============================================================================
//  PatchList — 完整实现（物理式 Undo/Redo + L2 磁盘溢出 + applyAndSave 流式）
//
//  编辑语义（虚拟文件坐标系 = 应用 patch 后看到的字节位置）：
//    所有 insert/remove/replace 接收的 byteOffset，是「用户当前看到」的位置；
//    内部存储的 Patch.byteOffset 也是同一虚拟坐标系。
//
//  Undo/Redo（物理式，而非叠加逆 patch）：
//    - 每次编辑都会把新 Patch insert 到 l1Hot_[idx] 并对后续 patch 做 shift
//    - UndoGroup 中记录 patchIndices（指向 l1Hot_ 的下标）+ patchesSnapshot（移除后重放用）
//    - 为支持 insert/remove l1Hot_ 后 patchIndices 仍正确：每次 l1Hot_ 变动都遍历
//      undoStack_/redoStack_ 中所有 group 的 patchIndices 做 ±1 调整
//    - undo 栈顶 group = 从大到小 remove patchIndices 对应的 patch（含反向 shift）
//    - redo 栈顶 group = 按 patchesSnapshot 中 byteOffset 从小到大重新 insert（含正向 shift）
//
//  L2 溢出（原设计保持不变）：
//    l1Bytes 超过上限 → 将最旧 25% 的 patch 序列化到临时文件，
//    从 l1Hot_ 中移除，l2Cold_ 记录文件名和覆盖的虚拟偏移范围。
//    renderRange 和 applyAndSave 同时考虑 l1 + l2 的所有 patch。
//    L2 溢出时清空 undo/redo 历史（避免冷 patch 参与 undo/redo 的索引管理）。
// ============================================================================

struct Patch {
    qint64   byteOffset;   // 虚拟字节偏移（升序唯一）
    qint32   oldLength;    // 0 = 纯插入
    QByteArray newText;    // 空 = 纯删除
    // 用于 undo：oldLength>0 时保存被删除/替换的原字节。
    // 渲染时不读取此字段。
    QByteArray oldSnapshot;
    // 仅当 oldLength==0（纯插入）时有效：该插入操作对应的原始 mmap 偏移位置。
    // 用于 origToVirtForLineStart：快速判断是否为"行首前缀插入"（正好在 orig line start 边界）。
    qint64   origInsertOffset = -1;
};

struct UndoGroup {
    QVector<int>    patchIndices;      // 指向 l1Hot_ 的下标（每次 l1Hot_ 变动都实时同步）
    QVector<Patch>  patchesSnapshot;   // undo 后保存移除的 patch 副本（redo 重放用，按 byteOffset 升序）
    bool            applied = true;    // true = 已生效（undoStack 中）；false = 已 undo（redoStack 中）
    qint64          netLengthDelta = 0; // 该 group 生效时的全局净长度差（统计用）
};

class PatchList {
    friend class LargeFileSearch;
public:
    PatchList();
    ~PatchList();

    static qint64 dynamicL1Limit();
    qint64 l1Bytes() const { return l1Bytes_; }
    qint64 l1Limit() const { return l1Limit_; }
    bool   needSoftWarn() const { return l1Bytes_ >= l1Limit_ * 4 / 5; }
    int    l1Count() const { return l1Hot_.size(); }
    int    l2Count() const { return l2Cold_.size(); }

    // 净长度差：所有 applied patch 的 Σ(newText.size() - oldLength)
    qint64 netLengthDelta() const;
    // 虚拟文件大小：mmapSize + netDelta + gapBytes（patch 间空洞）
    // 当 patch.byteOffset 超出当前虚拟末尾时产生 gap，virtSize 须包含 gap
    qint64 virtualSize(qint64 mmapSize) const;
    int    patchCount() const { return l1Hot_.size(); }
    const Patch* patchAt(int i) const { return (i>=0 && i<l1Hot_.size()) ? &l1Hot_[i] : nullptr; }

    // ---- 基本编辑 ----
    bool insert(qint64 byteOffset, const QByteArray& data, QString* errMsg = nullptr);
    bool remove(qint64 byteOffset, qint32 length, QString* errMsg = nullptr);
    bool replace(qint64 byteOffset, qint32 oldLength, const QByteArray& newData, QString* errMsg = nullptr);

    // 调试辅助（netDelta 别名，诊断用）
    qint64 netDelta() const { return netLengthDelta(); }

    // ---- 带快照的编辑（undo 严格可回滚要求调用者提供 oldSnapshot）----
    bool removeWithSnapshot(qint64 byteOffset, qint32 length,
                            const QByteArray& oldSnapshot, QString* errMsg = nullptr);
    bool replaceWithSnapshots(qint64 byteOffset, qint32 oldLength,
                              const QByteArray& newData, const QByteArray& oldSnapshot,
                              QString* errMsg = nullptr);

    bool isModified() const { return !(l1Hot_.isEmpty() && l2Cold_.isEmpty()); }

    // ---- UndoGroup ----
    void beginUndoGroup();
    void endUndoGroup();
    bool canUndo() const { return !undoStack_.isEmpty() && undoStack_.last().applied; }
    bool canRedo() const { return !redoStack_.isEmpty() && !redoStack_.last().applied; }
    bool undo(QString* errMsg = nullptr);
    bool redo(QString* errMsg = nullptr);

    // ---- 渲染 ----
    // 基于 mmap 的原始内容 + l1 + l2 应用 patch，返回 [start, end) 字节
    // 注意：start/end 是【虚拟坐标】（编辑后用户看到的位置）
    QByteArray renderRange(const uchar* mmapBase, qint64 mmapSize, qint64 start, qint64 end) const;

    // ---- 坐标转换（原始 mmap 偏移 ↔ 虚拟编辑后偏移）----
    // lineIndex_ 基于原始 mmap，patch.byteOffset 是虚拟坐标。
    // drawContentLine / 光标定位需要在这两套坐标系之间转换。
    // origToVirt: 原始 mmap 偏移 → 虚拟偏移（编辑后用户看到的位置）
    // virtToOrig: 虚拟偏移 → 原始 mmap 偏移
    qint64 origToVirt(qint64 origByte) const;
    qint64 virtToOrig(qint64 virtByte) const;

    // origToVirtForLineStart: 语义 = origToVirt，但对「恰好 origByte == 原始某行起始位置」
    //   并且该位置存在 oldLength==0 的纯 insert patch 时，把这些纯 insert 的**起始**虚拟字节
    //   作为返回值（而不是 origToVirt 原本的「纯 insert 之后」的位置）。
    // 用途：计算虚拟坐标下 Line N 的起始字节，保证在行首插入的内容归属这一行（而不是上一行的末尾）。
    qint64 origToVirtForLineStart(qint64 origLineStart) const;

    // ---- 保存（applyAndSave：流式 4MB 合并写盘 + progress + cancel）----
    bool applyAndSave(const uchar* mmapBase, qint64 mmapSize, const QString& outPath,
                      std::function<void(qint64,qint64)> progress = {},
                      std::atomic<bool>* cancel = nullptr,
                      QString* errMsg = nullptr);

    void resetAfterSave();
    void setSensitive(bool sensitive) { sensitive_ = sensitive; }
    bool isSensitive() const { return sensitive_; }

    // ---- 高级：replaceAll 压力检测（返回 true=需要中间保存提示）----
    // estimatedEditSizeBytes = totalReplacements * avg(newText.size()) - totalDeleted
    bool pressureWillExceedSoftWarn(qint64 estimatedEditSizeBytes) const;

    // Crash recovery persists the normalized final patch set inside DraftStore's
    // encrypted stream. Undo history is intentionally not restored.
    bool writeRecoveryJournal(QIODevice *output, QString *errMsg = nullptr) const;
    bool restoreRecoveryJournal(QIODevice *input, QString *errMsg = nullptr);

private:
    friend bool PatchList_removeWithSnapshot(PatchList&, qint64, qint32, const QByteArray&, QString*);
    friend bool PatchList_replaceWithSnapshots(PatchList&, qint64, qint32, const QByteArray&, const QByteArray&, QString*);

    QVector<Patch> l1Hot_;
    qint64         l1Bytes_ = 0;
    qint64         l1Limit_;

    struct ColdPatchFile {
        QString  filePath;
        qint64   fileSize  = 0;
        qint64   minOffset = 0;   // 覆盖的虚拟偏移范围 [min, max]
        qint64   maxOffset = 0;
        qint64   l1BytesAtSave = 0;  // 溢出前这部分的内存
        int      count     = 0;
    };
    QList<ColdPatchFile> l2Cold_;

    QList<UndoGroup> undoStack_;
    QList<UndoGroup> redoStack_;

    bool inExplicitGroup_ = false;
    bool sensitive_ = false;

    // ---- 内部 helper ----
    int  lowerBound(qint64 pos) const;
    bool validateRange(qint64 byteOffset, qint32 length, QString* errMsg) const;
    void pushPatchIndexToActiveGroup(int patchIdx, qint64 netDelta);

    // 对 [fromIndex, l1Hot_.size()) 的所有 patch 做偏移平移
    // delta>0 表示这些 patch 的 byteOffset 要加 delta（发生在它们之前插入了内容）
    // delta<0 表示减（发生在它们之前删除了内容）
    void shiftOffsetsAfter(int fromIndex, qint64 delta);

    // ===== undo/redo 物理式索引同步 helper =====
    // l1Hot_.insert(idx, ...) 之后调用：所有 undo/redo group 中 patchIndex >= idx 的都 +1
    void adjustPatchIndicesOnInsert(int idx);
    // l1Hot_.remove(idx, n) 之前调用：所有 undo/redo group 中 patchIndex > idx 的都 -n；
    // 如遇到 patchIndex == idx（在其他 group 中也引用的，理论不会发生）记为 -1（无效）
    void adjustPatchIndicesOnRemove(int idx, int n = 1);
    // 物理从 l1Hot_ 移除 patch（含 shiftOffsetsAfter + l1Bytes_ 更新 + adjustPatchIndicesOnRemove）
    qint64 physicallyRemovePatchAt(int idx);
    // 物理向 l1Hot_ 插入 patch（含 shiftOffsetsAfter + l1Bytes_ 更新 + adjustPatchIndicesOnInsert）
    // 返回实际插入的 index
    int physicallyInsertPatch(Patch&& p);

    // L2 溢出：从 l1Hot_ 头移除 25% 到新冷文件
    void spillIfNeeded();

    // 合并 l1 + l2 的「patch 视图」用于 renderRange / applyAndSave：
    struct MergedView {
        QVector<Patch>         l2Cache;          // l2 反序列化出来的对象（保持住）
        QVector<const Patch*> sorted;            // 按 byteOffset 升序（&l1Hot_[i] 或 &l2Cache[j]）
    };
    void buildMergedView(MergedView& out) const;
    // 调试辅助（public 版本，供诊断工具打印）
    void buildMergedViewPublic(MergedView& out) const { buildMergedView(out); }

    // renderRange 内部：基于 MergedView.sorted 构建 spans（双游标算法）
    static void buildRenderSpansFromSorted(const QVector<const Patch*>& sorted,
                                           const uchar* mmapBase, qint64 mmapSize,
                                           qint64 start, qint64 end,
                                           QVector<QPair<const uchar*, qint64>>& out);

    // 冷文件读写格式
    static bool serializeColdFile(const QString& path, const QVector<Patch>& batch,
                                  qint64* outFileSize, QString* errMsg);
    static bool deserializeColdFile(const QString& path, QVector<Patch>& out,
                                    ColdPatchFile* metaFill, QString* errMsg);
    static QString makeL2TempFilePath(int seq);
};
