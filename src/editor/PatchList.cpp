#include "PatchList.h"
#include "DynamicThresholds.h"

#include <QDir>
#include <QSaveFile>
#include <QFile>
#include <QDataStream>
#include <QCoreApplication>
#include <QDateTime>
#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <climits>
#include <memory>
#include <utility>

namespace {

QString tempDirNotepadSafe() {
    QString dir = QDir::tempPath() + QStringLiteral("/notepad-safe");
    QDir().mkpath(dir);
    return dir;
}

static inline qint64 patchNetDelta(const Patch& p) {
    return qint64(p.newText.size()) - p.oldLength;
}

static inline qint64 patchMemoryBytes(const Patch& p) {
    return qint64(p.newText.size()) + qint64(p.oldSnapshot.size());
}

} // namespace

// ============================================================================
//  基础构造 / 阈值 / 二分 / 析构
// ============================================================================
PatchList::PatchList() : l1Limit_(dynamicL1Limit()) {}
PatchList::~PatchList() {
    resetAfterSave();
}
qint64 PatchList::dynamicL1Limit() { return DynamicThresholds::instance().PATCH_L1_LIMIT; }

int PatchList::lowerBound(qint64 pos) const {
    int lo = 0, hi = l1Hot_.size();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (l1Hot_[mid].byteOffset < pos) lo = mid + 1;
        else                               hi = mid;
    }
    return lo;
}

bool PatchList::validateRange(qint64 byteOffset, qint32 length, QString* errMsg) const {
    if (byteOffset < 0)      { if (errMsg) *errMsg = QStringLiteral("byteOffset 不能为负"); return false; }
    if (length < 0)          { if (errMsg) *errMsg = QStringLiteral("length 不能为负"); return false; }
    return true;
}

void PatchList::shiftOffsetsAfter(int fromIndex, qint64 delta) {
    if (delta == 0) return;
    for (int i = fromIndex; i < l1Hot_.size(); ++i) l1Hot_[i].byteOffset += delta;
}

// ============================================================================
//  undo/redo 物理式索引同步 helper
// ============================================================================
void PatchList::adjustPatchIndicesOnInsert(int idx) {
    auto adjustOne = [&](QList<UndoGroup>& stack) {
        for (auto& g : stack) {
            for (int i = 0; i < g.patchIndices.size(); ++i) {
                if (g.patchIndices[i] >= idx && g.patchIndices[i] != -1) {
                    ++g.patchIndices[i];
                }
            }
        }
    };
    adjustOne(undoStack_);
    adjustOne(redoStack_);
}

void PatchList::adjustPatchIndicesOnRemove(int idx, int n) {
    auto adjustOne = [&](QList<UndoGroup>& stack) {
        for (auto& g : stack) {
            for (int i = 0; i < g.patchIndices.size(); ++i) {
                int& v = g.patchIndices[i];
                if (v == -1) continue;
                if (v >= idx && v < idx + n) {
                    v = -1; // 被移除的位置（理论不会发生在其他 group 引用中）
                } else if (v >= idx + n) {
                    v -= n;
                }
            }
        }
    };
    adjustOne(undoStack_);
    adjustOne(redoStack_);
}

qint64 PatchList::physicallyRemovePatchAt(int idx) {
    if (idx < 0 || idx >= l1Hot_.size()) return 0;
    const Patch& p = l1Hot_[idx];
    qint64 delta = -patchNetDelta(p); // 反向净差（移除 p 后全局虚拟长度变化量）
    // 1. 调整 undo/redo 其他 group 的 patchIndices 引用
    adjustPatchIndicesOnRemove(idx, 1);
    // 2. 关键：删除后 idx 之后（现仍然是 idx 开始，因为 remove 会让后面元素前移一位）的
    //    patch byteOffset 按 -patchNetDelta(p) = delta 平移，保证后续 patch 的 byteOffset
    //    仍为"当前虚拟坐标"（与 physicallyInsertPatch 对称）。
    if (idx < l1Hot_.size() - 1 && patchNetDelta(p) != 0) {
        // 注意：下面用 idx（remove 之后原来 idx+1 的元素就会到 idx 位置）
        shiftOffsetsAfter(idx, -patchNetDelta(p));
    }
    // 3. 从 l1Hot_ 移除并更新 l1Bytes_
    l1Bytes_ -= patchMemoryBytes(p);
    l1Hot_.remove(idx);
    return delta;
}

int PatchList::physicallyInsertPatch(Patch&& p) {
    qint64 delta = patchNetDelta(p);
    int idx = lowerBound(p.byteOffset);
    // 1. 先 adjustPatchIndicesOnInsert（因为 l1Hot_ 即将 insert，后续 index 都会 +1）
    adjustPatchIndicesOnInsert(idx);
    // 2. 插入元素
    l1Hot_.insert(idx, std::move(p));
    // 3. 关键：把 idx+1（即当前插入点之后的所有 patch）的 byteOffset 按 delta 平移，
    //    保证所有 patch.byteOffset 始终是【当前虚拟文件坐标系下的偏移】，从而让
    //    buildRenderSpansFromSorted / origToVirt 等双游标算法可以用 p.byteOffset - virtCursor
    //    精确得到 gap（否则 gap 会少计/多计之前的 insert 大小，导致 render 错位）。
    if (idx + 1 < l1Hot_.size() && delta != 0) {
        shiftOffsetsAfter(idx + 1, delta);
    }
    // 4. 更新 l1Bytes_
    l1Bytes_ += patchMemoryBytes(l1Hot_[idx]);
    return idx;
}

void PatchList::pushPatchIndexToActiveGroup(int patchIdx, qint64 netDelta) {
    if (inExplicitGroup_) {
        if (!undoStack_.isEmpty()) {
            UndoGroup& g = undoStack_.last();
            g.patchIndices.append(patchIdx);
            g.netLengthDelta += netDelta;
        }
    } else {
        UndoGroup g;
        g.patchIndices.append(patchIdx);
        g.netLengthDelta = netDelta;
        g.applied = true;
        undoStack_.append(g);
    }
    redoStack_.clear();
}

// ============================================================================
//  公开 insert / remove / replace：基于 physicallyInsertPatch
// ============================================================================
bool PatchList::insert(qint64 byteOffset, const QByteArray& data, QString* errMsg) {
    if (!validateRange(byteOffset, 0, errMsg)) return false;
    if (data.isEmpty()) return true;
    Patch p;
    p.byteOffset = byteOffset;
    p.oldLength = 0;
    p.newText = data;
    // 保存"插入位置对应的原始偏移"（此时 patch 自身尚未插入，virtToOrig 精确映射插入时刻 virt→orig）。
    p.origInsertOffset = virtToOrig(byteOffset);
    int idx = physicallyInsertPatch(std::move(p));
    qint64 shift = data.size();
    pushPatchIndexToActiveGroup(idx, shift);
    spillIfNeeded();
    return true;
}

bool PatchList_removeWithSnapshot(PatchList& self, qint64 byteOffset, qint32 length,
                                  const QByteArray& deletedBytes, QString* errMsg)
{
    if (!self.validateRange(byteOffset, length, errMsg)) return false;
    if (length == 0) return true;
    Patch p;
    p.byteOffset = byteOffset;
    p.oldLength = length;
    p.newText.clear();
    p.oldSnapshot = deletedBytes;
    int idx = self.physicallyInsertPatch(std::move(p));
    qint64 shift = -qint64(length);
    self.pushPatchIndexToActiveGroup(idx, shift);
    self.spillIfNeeded();
    return true;
}

bool PatchList_replaceWithSnapshots(PatchList& self, qint64 byteOffset, qint32 oldLength,
                                    const QByteArray& oldBytes, const QByteArray& newData,
                                    QString* errMsg)
{
    if (oldLength < 0) oldLength = 0;
    if (!self.validateRange(byteOffset, oldLength, errMsg)) return false;
    if (oldLength == 0 && newData.isEmpty()) return true;

    // A same-size overwrite inside bytes produced by an earlier patch edits
    // that patch's payload in place.  Stacking an overlapping patch would
    // render both payloads (an inserted A0 followed by low-nibble input B used
    // to become "AB A0") and could grow the file on every second digit.
    // Treat the two nibbles as one byte-entry undo unit: the original patch
    // keeps its disk snapshot, so Undo returns to the state before that byte
    // was entered.
    if (oldLength == newData.size()) {
        const qint64 replaceEnd = byteOffset + oldLength;
        for (int i = self.l1Hot_.size() - 1; i >= 0; --i) {
            Patch &existing = self.l1Hot_[i];
            const qint64 payloadEnd = existing.byteOffset
                                      + existing.newText.size();
            if (byteOffset < existing.byteOffset || replaceEnd > payloadEnd)
                continue;
            const int payloadOffset = int(byteOffset - existing.byteOffset);
            if (self.sensitive_) {
                for (int j = 0; j < oldLength; ++j)
                    existing.newText[payloadOffset + j] = 0;
            }
            existing.newText.replace(payloadOffset, oldLength, newData);
            self.redoStack_.clear();
            return true;
        }

        // A later same-size overwrite of the exact original virtual range
        // supersedes the previous overwrite. Preserve the first patch's
        // original snapshot so Undo still returns to the disk byte.
        for (int i = 0; i < self.l1Hot_.size(); ++i) {
            Patch &existing = self.l1Hot_[i];
            if (existing.byteOffset != byteOffset
                || existing.oldLength != oldLength
                || existing.newText.size() != newData.size()) {
                continue;
            }
            if (self.sensitive_ && !existing.newText.isEmpty())
                existing.newText.fill(0);
            existing.newText = newData;
            self.redoStack_.clear();
            return true;
        }
    }

    const bool explicitGroup = self.inExplicitGroup_;
    if (!explicitGroup) self.beginUndoGroup();

    Patch rp;
    rp.byteOffset = byteOffset;
    rp.oldLength = oldLength;
    rp.newText = newData;
    rp.oldSnapshot = oldBytes;
    int idx = self.physicallyInsertPatch(std::move(rp));
    qint64 delta = qint64(newData.size()) - oldLength;
    self.pushPatchIndexToActiveGroup(idx, delta);
    self.spillIfNeeded();

    if (!explicitGroup) self.endUndoGroup();
    return true;
}

bool PatchList::remove(qint64 byteOffset, qint32 length, QString* errMsg) {
    QByteArray empty;
    return PatchList_removeWithSnapshot(*this, byteOffset, length, empty, errMsg);
}

bool PatchList::removeWithSnapshot(qint64 byteOffset, qint32 length,
                                   const QByteArray& oldSnapshot, QString* errMsg)
{
    QByteArray snap = oldSnapshot;
    if (snap.size() < length) {
        if (errMsg) *errMsg = QStringLiteral("oldSnapshot 长度不足（undo 无法保证回滚）");
        return false;
    }
    return PatchList_removeWithSnapshot(*this, byteOffset, length, snap, errMsg);
}

bool PatchList::replace(qint64 byteOffset, qint32 oldLength, const QByteArray& newData, QString* errMsg) {
    QByteArray emptyOld;
    return PatchList_replaceWithSnapshots(*this, byteOffset, oldLength, emptyOld, newData, errMsg);
}

bool PatchList::replaceWithSnapshots(qint64 byteOffset, qint32 oldLength,
                                     const QByteArray& newData, const QByteArray& oldSnapshot,
                                     QString* errMsg)
{
    if (oldSnapshot.size() < oldLength) {
        if (errMsg) *errMsg = QStringLiteral("oldSnapshot 长度不足（undo 无法保证回滚）");
        return false;
    }
    return PatchList_replaceWithSnapshots(*this, byteOffset, oldLength, oldSnapshot, newData, errMsg);
}

qint64 PatchList::netLengthDelta() const {
    qint64 total = 0;
    for (const Patch& p : l1Hot_) total += patchNetDelta(p);
    return total;
}

qint64 PatchList::virtualSize(qint64 mmapSize) const {
    MergedView view;
    buildMergedView(view);
    if (view.sorted.isEmpty()) return mmapSize;

    qint64 virtCursor = 0;
    qint64 origCursor = 0;
    for (const Patch* p : view.sorted) {
        qint64 gapV = p->byteOffset - virtCursor;
        if (gapV < 0) gapV = 0;
        if (gapV > 0) {
            qint64 origRemain = qMax(qint64(0), mmapSize - origCursor);
            qint64 fromOrig = qMin(gapV, origRemain);
            origCursor += fromOrig;
            virtCursor += gapV;  // gap（含 NUL 填充空洞）推进虚拟游标
        }
        virtCursor += p->newText.size();
        origCursor += p->oldLength;
    }
    // 尾部剩余原文件
    qint64 tailV = qMax(qint64(0), mmapSize - origCursor);
    virtCursor += tailV;
    return virtCursor;
}

// ============================================================================
//  UndoGroup 显式包装 + undo / redo（物理式移除 / 重新插入）
// ============================================================================
void PatchList::beginUndoGroup() {
    inExplicitGroup_ = true;
    undoStack_.append(UndoGroup());
}

void PatchList::endUndoGroup() {
    inExplicitGroup_ = false;
    if (!undoStack_.isEmpty() && undoStack_.last().patchIndices.isEmpty()) {
        undoStack_.removeLast();
    }
}

bool PatchList::undo(QString* errMsg) {
    if (undoStack_.isEmpty() || !undoStack_.last().applied) {
        if (errMsg) *errMsg = QStringLiteral("没有可撤销的操作");
        return false;
    }
    if (l2Count() > 0) {
        if (errMsg) *errMsg = QStringLiteral("Patch 已 L2 溢出，undo/redo 历史已清空");
        return false;
    }

    UndoGroup g = undoStack_.last();
    undoStack_.removeLast();
    g.applied = false;
    g.patchesSnapshot.clear();
    g.patchesSnapshot.reserve(g.patchIndices.size());

    // 每次从 g.patchIndices 找当前有效的最大 index（从大到小移除，避免 index 错乱）
    while (true) {
        int best = -1;
        int bestSlot = -1;
        for (int i = 0; i < g.patchIndices.size(); ++i) {
            int v = g.patchIndices[i];
            if (v != -1 && (best == -1 || v > best)) { best = v; bestSlot = i; }
        }
        if (best == -1) break;
        // 保存该 patch 副本到 snapshot
        g.patchesSnapshot.append(l1Hot_[best]);
        // 在 physicallyRemovePatchAt 之前，先手动同步 g.patchIndices 自身的索引：
        //   1) bestSlot 置 -1（标记已处理）
        //   2) 对 g 中其他条目：如果 patchIndex > best，则 -1（best 位置被删除后后续 index 左移）
        //      如果 patchIndex == best 且不是 bestSlot（理论不应该有重复），也置 -1
        g.patchIndices[bestSlot] = -1;
        for (int j = 0; j < g.patchIndices.size(); ++j) {
            if (g.patchIndices[j] > best) {
                --g.patchIndices[j];
            }
        }
        // 物理移除（会对 undoStack_/redoStack_ 中其他 group 的 patchIndices 做 adjustPatchIndicesOnRemove，
        // 本地 g 已经手动调整过了）
        physicallyRemovePatchAt(best);
    }

    // patchesSnapshot 目前是从大到小的 byteOffset 顺序，redo 时需要从小到大；sort：
    std::stable_sort(g.patchesSnapshot.begin(), g.patchesSnapshot.end(),
                     [](const Patch& a, const Patch& b) { return a.byteOffset < b.byteOffset; });
    // 清空 patchIndices（redo 时重新记录）
    g.patchIndices.clear();

    redoStack_.append(g);
    return true;
}

bool PatchList::redo(QString* errMsg) {
    if (redoStack_.isEmpty() || redoStack_.last().applied) {
        if (errMsg) *errMsg = QStringLiteral("没有可重做的操作");
        return false;
    }
    if (l2Count() > 0) {
        if (errMsg) *errMsg = QStringLiteral("Patch 已 L2 溢出，redo 历史不可用");
        return false;
    }

    UndoGroup g = redoStack_.last();
    redoStack_.removeLast();
    g.applied = true;

    // 按 byteOffset 从小到大 insert patchesSnapshot（g.patchesSnapshot 已按 byteOffset 升序）
    QVector<int> newIndices;
    newIndices.reserve(g.patchesSnapshot.size());
    for (const Patch& srcP : g.patchesSnapshot) {
        Patch copy = srcP;  // deep copy QByteArray
        int newIdx = physicallyInsertPatch(std::move(copy));
        newIndices.append(newIdx);
    }
    g.patchIndices = std::move(newIndices);
    g.patchesSnapshot.clear();

    undoStack_.append(g);
    return true;
}

// ============================================================================
//  L2 溢出 + 序列化
// ============================================================================
void PatchList::spillIfNeeded() {
    // Decrypted secure payload deltas must never enter a plaintext spill file.
    // Their source payload is bounded below 32 MiB, so retaining patches in
    // memory is both safer and within the document admission budget.
    if (sensitive_) return;
    if (l1Bytes_ <= l1Limit_) return;
    if (l1Hot_.isEmpty()) return;
    int spillCount = qMax(1, l1Hot_.size() / 4);
    spillCount = qMin(spillCount, l1Hot_.size());

    QVector<Patch> batch;
    batch.reserve(spillCount);
    qint64 minOff = LLONG_MAX, maxOff = LLONG_MIN;
    qint64 batchBytes = 0;
    // 先做 adjustPatchIndicesOnRemove(0, spillCount)，再从 l1Hot_ 头部移出
    adjustPatchIndicesOnRemove(0, spillCount);
    for (int i = 0; i < spillCount; ++i) {
        batch.append(l1Hot_[i]);
        batchBytes += patchMemoryBytes(l1Hot_[i]);
        minOff = qMin(minOff, l1Hot_[i].byteOffset);
        maxOff = qMax(maxOff, l1Hot_[i].byteOffset + qint64(l1Hot_[i].newText.size()));
    }
    static int s_seq = 0;
    QString path = makeL2TempFilePath(++s_seq);
    qint64 fileSize = 0;
    QString err;
    if (!serializeColdFile(path, batch, &fileSize, &err)) return;
    ColdPatchFile meta;
    meta.filePath = path;
    meta.fileSize = fileSize;
    meta.minOffset = minOff;
    meta.maxOffset = maxOff;
    meta.l1BytesAtSave = batchBytes;
    meta.count = batch.size();
    l2Cold_.append(meta);
    l1Hot_.remove(0, spillCount);
    l1Bytes_ -= batchBytes;
    // 清空 undo/redo 历史（冷 patch 不再参与索引管理）
    undoStack_.clear();
    redoStack_.clear();
}

QString PatchList::makeL2TempFilePath(int seq) {
    QString dir = tempDirNotepadSafe();
    QString name = QStringLiteral("l2_%1_%2_%3.bin")
        .arg(qApp ? qApp->applicationPid() : qint64(QCoreApplication::applicationPid()))
        .arg(seq)
        .arg(QDateTime::currentMSecsSinceEpoch());
    return QDir(dir).filePath(name);
}

bool PatchList::serializeColdFile(const QString& path, const QVector<Patch>& batch,
                                  qint64* outFileSize, QString* errMsg)
{
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) {
        if (errMsg) *errMsg = QObject::tr("无法打开冷文件写入：%1").arg(sf.errorString());
        return false;
    }
    QDataStream s(&sf);
    s.setByteOrder(QDataStream::LittleEndian);
    s << quint32(0x4C325046);
    s << quint32(1);
    s << quint64(batch.size());
    for (const Patch& p : batch) {
        s << quint64(p.byteOffset);
        s << qint32(p.oldLength);
        s << p.newText;
        s << p.oldSnapshot;
    }
    if (!sf.commit()) {
        if (errMsg) *errMsg = QObject::tr("冷文件提交失败：%1").arg(sf.errorString());
        return false;
    }
    QFileInfo fi(path);
    if (outFileSize) *outFileSize = fi.size();
    return true;
}

bool PatchList::deserializeColdFile(const QString& path, QVector<Patch>& out,
                                    ColdPatchFile* metaFill, QString* errMsg)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errMsg) *errMsg = QObject::tr("无法打开冷文件读取：%1").arg(f.errorString());
        return false;
    }
    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);
    quint32 magic, version;
    quint64 count;
    s >> magic >> version >> count;
    if (magic != 0x4C325046u || version != 1) {
        if (errMsg) *errMsg = QObject::tr("冷文件 magic/version 不匹配");
        return false;
    }
    out.clear();
    out.reserve(int(count));
    qint64 minOff = LLONG_MAX, maxOff = LLONG_MIN;
    qint64 l1Bytes = 0;
    for (quint64 i = 0; i < count; ++i) {
        quint64 bo; qint32 ol; QByteArray nt, os;
        s >> bo >> ol >> nt >> os;
        Patch p;
        p.byteOffset = qint64(bo);
        p.oldLength = ol;
        p.newText = nt;
        p.oldSnapshot = os;
        out.append(std::move(p));
        minOff = qMin(minOff, qint64(bo));
        maxOff = qMax(maxOff, qint64(bo) + nt.size());
        l1Bytes += nt.size() + os.size();
    }
    if (metaFill) {
        metaFill->filePath = path;
        metaFill->fileSize = f.size();
        metaFill->minOffset = minOff;
        metaFill->maxOffset = maxOff;
        metaFill->l1BytesAtSave = l1Bytes;
        metaFill->count = int(count);
    }
    return true;
}

bool PatchList::writeRecoveryJournal(QIODevice *output, QString *errMsg) const
{
    if (!output || !output->isWritable()) {
        if (errMsg) *errMsg = QObject::tr("恢复日志输出设备不可写。");
        return false;
    }
    QVector<Patch> patches;
    patches.reserve(l1Hot_.size() + l2Cold_.size() * 16);
    for (const ColdPatchFile &cold : l2Cold_) {
        QVector<Patch> batch;
        QString error;
        if (!deserializeColdFile(cold.filePath, batch, nullptr, &error)) {
            if (errMsg) *errMsg = QObject::tr("读取编辑恢复分片失败：%1").arg(error);
            return false;
        }
        for (Patch &patch : batch)
            patches.append(std::move(patch));
    }
    for (const Patch &patch : l1Hot_)
        patches.append(patch);
    std::stable_sort(patches.begin(), patches.end(), [](const Patch &left,
                                                        const Patch &right) {
        if (left.byteOffset != right.byteOffset)
            return left.byteOffset < right.byteOffset;
        const bool leftRemove = left.newText.isEmpty() && left.oldLength > 0;
        const bool rightRemove = right.newText.isEmpty() && right.oldLength > 0;
        return leftRemove != rightRemove && leftRemove;
    });

    QDataStream stream(output);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0x4E50524Au) << quint32(1) << quint64(patches.size());
    for (const Patch &patch : std::as_const(patches)) {
        stream << patch.byteOffset << patch.oldLength << patch.newText
               << patch.origInsertOffset;
        if (stream.status() != QDataStream::Ok) {
            if (errMsg) *errMsg = QObject::tr("写入编辑恢复日志失败。");
            return false;
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool PatchList::restoreRecoveryJournal(QIODevice *input, QString *errMsg)
{
    if (!input || !input->isReadable()) {
        if (errMsg) *errMsg = QObject::tr("恢复日志输入设备不可读。");
        return false;
    }
    QDataStream stream(input);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0;
    quint32 version = 0;
    quint64 count = 0;
    stream >> magic >> version >> count;
    if (stream.status() != QDataStream::Ok || magic != 0x4E50524Au
        || version != 1 || count == 0 || count > 1000000) {
        if (errMsg) *errMsg = QObject::tr("编辑恢复日志头无效。");
        return false;
    }

    QVector<Patch> restored;
    restored.reserve(int(count));
    qint64 totalBytes = 0;
    qint64 previousOffset = -1;
    for (quint64 index = 0; index < count; ++index) {
        Patch patch;
        stream >> patch.byteOffset >> patch.oldLength >> patch.newText
               >> patch.origInsertOffset;
        totalBytes += patch.newText.size();
        if (stream.status() != QDataStream::Ok || patch.byteOffset < 0
            || patch.oldLength < 0 || patch.byteOffset < previousOffset
            || patch.newText.size() > 128 * 1024 * 1024
            || totalBytes > Q_INT64_C(512) * 1024 * 1024) {
            if (errMsg) *errMsg = QObject::tr("编辑恢复日志内容无效或超过 512 MiB 安全上限。");
            return false;
        }
        previousOffset = patch.byteOffset;
        restored.append(std::move(patch));
    }
    if (!stream.atEnd()) {
        if (errMsg) *errMsg = QObject::tr("编辑恢复日志尾部包含无效数据。");
        return false;
    }

    resetAfterSave();
    l1Hot_ = std::move(restored);
    l1Bytes_ = 0;
    for (const Patch &patch : std::as_const(l1Hot_))
        l1Bytes_ += patch.newText.size();
    undoStack_.clear();
    redoStack_.clear();
    // Do not turn a successfully decrypted recovery journal back into a
    // plaintext cold-spill file during startup. The journal is bounded to
    // 512 MiB above and remains in memory until a later explicit edit/save.
    return isModified();
}

// ============================================================================
//  buildMergedView：合并 l1 + l2 → 升序指针数组
// ============================================================================
void PatchList::buildMergedView(MergedView& out) const {
    out.l2Cache.clear();
    out.sorted.clear();
    out.sorted.reserve(l1Hot_.size() + l2Cold_.size() * 16);
    QString err;
    for (const auto& cf : l2Cold_) {
        QVector<Patch> batch;
        ColdPatchFile dummy;
        if (!deserializeColdFile(cf.filePath, batch, &dummy, &err)) continue;
        int start = out.l2Cache.size();
        for (int i = 0; i < batch.size(); ++i) out.l2Cache.append(std::move(batch[i]));
        for (int i = 0; i < out.l2Cache.size() - start; ++i) out.sorted.append(&out.l2Cache[start + i]);
    }
    for (int i = 0; i < l1Hot_.size(); ++i) out.sorted.append(&l1Hot_[i]);
    std::stable_sort(out.sorted.begin(), out.sorted.end(),
                     [](const Patch* a, const Patch* b) {
                         // 主序：byteOffset 升序
                         if (a->byteOffset != b->byteOffset)
                             return a->byteOffset < b->byteOffset;
                         // 次序：同一个 byteOffset 上，pure remove（newText 空）必须排在
                         //       有 newText 的 patch（insert/replace）前面，使它先建立
                         //       deleteSkipRemaining「欠账」，后续 insert 的 newText 被
                         //       正确消耗，避免「已输出的 newText 无法被回滚」的问题。
                         bool aPureRemove = (a->newText.isEmpty() && a->oldLength > 0);
                         bool bPureRemove = (b->newText.isEmpty() && b->oldLength > 0);
                         if (aPureRemove != bPureRemove) return aPureRemove;
                         return false;
                     });
}

// ============================================================================
//  buildRenderSpansFromSorted —— 双游标算法（virtual + original）
//
//  **数学不变式**：本函数的 virtCursor / origCursor 推进规则必须与
//  virtualSize()、virtToOrig()、origToVirt() 100% 一致：
//
//    for each patch:
//        gap = p.byteOffset - virtCursor
//        if gap > 0:
//            fromOrig = min(gap, mmapSize - origCursor)
//            输出 mmap[origCursor .. origCursor+fromOrig-1]
//            origCursor += fromOrig
//            virtCursor += fromOrig                // orig 部分同步推进 virt
//            hole = gap - fromOrig                 // 超出 mmap 尾部 = 虚拟 append
//            if hole > 0:
//                输出 NUL * hole
//                virtCursor += hole                // NUL 也推进 virt（虚拟填充）
//        // patch.newText：直接全部输出（Insert/Replace 非空）
//        if !p.newText.isEmpty():
//            输出 p.newText
//            virtCursor += p.newText.size()
//        // patch 覆盖范围：origCursor 总是 += oldLength（Replace/PureRemove 都一样）
//        //  对 Pure Remove：这就是「删除语义」——跳过 orig 的 oldLength 字节，
//        //                  virtCursor 不推进（newText 空）。
//        origCursor += p.oldLength
//    // tail：剩余 orig 原文件尾部
//    tail = mmapSize - origCursor
//    if tail > 0:
//        输出 mmap[origCursor .. origCursor+tail-1]
//        virtCursor += tail
//
//  这样最终 virtCursor 必 == virtualSize(mmapSize)，不会再出现「保存到最后一块少 N 字节」。
// ============================================================================
void PatchList::buildRenderSpansFromSorted(const QVector<const Patch*>& sorted,
                                            const uchar* mmapBase, qint64 mmapSize,
                                            qint64 start, qint64 end,
                                            QVector<QPair<const uchar*, qint64>>& out)
{
    out.clear();
    if (start >= end) return;

    qint64 virtCursor = 0;  // 与 virtualSize/virtToOrig 完全同步
    qint64 origCursor = 0;
    const int n = sorted.size();
    int i = 0;

    const qint64 mmapLen = mmapBase ? qint64(mmapSize) : qint64(0);

    // appendSpan：向后端输出 [data, data+len) 字节，并裁剪到 [start, end)
    auto appendSpan = [&](const uchar* data, qint64 len) {
        if (len <= 0) return;
        qint64 segStart = virtCursor;
        qint64 segEnd   = virtCursor + len;
        qint64 ovSt = qMax(segStart, start);
        qint64 ovEd = qMin(segEnd,   end);
        if (ovSt < ovEd) {
            qint64 off = ovSt - segStart;
            qint64 spanLen = ovEd - ovSt;
            if (mmapBase && data >= mmapBase && data < mmapBase + mmapLen) {
                // 来自 mmap：严格限制在 mmap 边界内
                qint64 dataRel = qint64(data - mmapBase);
                qint64 maxAvail = mmapLen - dataRel;
                if (off >= maxAvail) {
                    spanLen = 0;
                } else {
                    qint64 avail = maxAvail - off;
                    if (spanLen > avail) spanLen = avail;
                }
            }
            if (spanLen > 0) {
                out.append(qMakePair(data + off, spanLen));
            }
        }
        virtCursor = segEnd;
    };

    static QByteArray s_zeroFill;
    constexpr int ZF_CHUNK = 65536;
    if (s_zeroFill.size() != ZF_CHUNK) s_zeroFill.fill('\0', ZF_CHUNK);

    while (i < n) {
        const Patch& p = *sorted[i];
        // 1) gap：从 virtCursor 对齐到 p.byteOffset
        qint64 gapV = p.byteOffset - virtCursor;
        if (gapV < 0) gapV = 0;
        if (gapV > 0) {
            qint64 origRemain = qMax(qint64(0), mmapLen - origCursor);
            qint64 fromOrig = qMin(gapV, origRemain);
            qint64 fromHole = gapV - fromOrig;
            if (fromOrig > 0) {
                appendSpan(mmapBase + origCursor, fromOrig);
                origCursor += fromOrig;
            }
            if (fromHole > 0) {
                qint64 remain = fromHole;
                const uchar* zf = (const uchar*)s_zeroFill.constData();
                while (remain > 0) {
                    qint64 piece = qMin<qint64>(remain, ZF_CHUNK);
                    appendSpan(zf, piece);
                    remain -= piece;
                }
            }
        }
        // 2) 输出 patch.newText（Insert / Replace）
        if (!p.newText.isEmpty()) {
            appendSpan((const uchar*)p.newText.constData(), p.newText.size());
        }
        // 3) 推进 origCursor += oldLength（Replace / Pure Remove 都一样，与 virtualSize 对齐）
        origCursor += p.oldLength;
        ++i;
    }
    // 4) tail：剩余 orig 文件尾部
    qint64 tailV = qMax(qint64(0), mmapLen - origCursor);
    if (tailV > 0) {
        appendSpan(mmapBase + origCursor, tailV);
        origCursor += tailV;
    }
}

QByteArray PatchList::renderRange(const uchar* mmapBase, qint64 mmapSize, qint64 start, qint64 end) const {
    if (end <= start) return {};
    MergedView view;
    buildMergedView(view);
    if (view.sorted.isEmpty()) {
        qint64 len = end - start;
        if (mmapBase && qint64(start) + len > mmapSize) len = qMax(qint64(0), mmapSize - start);
        if (len <= 0) return {};
        return QByteArray((const char*)(mmapBase + start), int(len));
    }
    QVector<QPair<const uchar*, qint64>> spans;
    buildRenderSpansFromSorted(view.sorted, mmapBase, mmapSize, start, end, spans);
    qint64 total = 0;
    for (const auto& s : spans) total += s.second;
    if (total < 0 || total > qint64(INT_MAX)) {
        return {};
    }
    QByteArray out(int(total), Qt::Uninitialized);
    char* dst = out.data();
    for (const auto& s : spans) {
        if (s.second <= 0) continue;
        if (s.second > qint64(INT_MAX)) return {};
        std::memcpy(dst, s.first, size_t(s.second));
        dst += s.second;
    }
    out.resize(int(dst - out.constData()));
    return out;
}

// ============================================================================
//  坐标转换：原始 mmap 偏移 ↔ 虚拟编辑后偏移
//
//  lineIndex_ 基于原始 mmap（只读），patch.byteOffset 是虚拟坐标。
//  drawContentLine / 光标定位需要在这两套坐标系之间转换。
//
//  双游标算法：virtCursor/origCursor 同步推进。
//    gap 区域（无 patch）：virt 和 orig 同步增长
//    patch 区域：virt += newText.size()，orig += oldLength
// ============================================================================
qint64 PatchList::origToVirt(qint64 origByte) const {
    if (l2Cold_.isEmpty() && l1Hot_.isEmpty()) return origByte;

    auto compute = [&](const QVector<const Patch*>& sorted) -> qint64 {
        qint64 virtCursor = 0, origCursor = 0;
        for (const Patch* p : sorted) {
            qint64 gap = p->byteOffset - virtCursor;
            if (gap > 0) {
                if (origByte < origCursor + gap)
                    return virtCursor + (origByte - origCursor);
                virtCursor += gap;
                origCursor += gap;
            }
            if (origByte < origCursor + p->oldLength)
                return virtCursor;  // 原始字节被 patch 覆盖，映射到 patch 起始虚拟位置
            virtCursor += p->newText.size();
            origCursor += p->oldLength;
        }
        return virtCursor + (origByte - origCursor);
    };

    if (l2Cold_.isEmpty()) {
        QVector<const Patch*> sorted;
        sorted.reserve(l1Hot_.size());
        for (const auto& p : l1Hot_) sorted.append(&p);
        return compute(sorted);
    }
    MergedView view;
    buildMergedView(view);
    return compute(view.sorted);
}

qint64 PatchList::origToVirtForLineStart(qint64 origLineStart) const {
    // 先按普通 origToVirt 取得"后偏移"位置（把所有行首纯 insert 都算在了后移里），
    // 再把那些正好在 origLineStart 的纯 insert（通过 Patch::origInsertOffset 精确匹配）
    // 的 newText 长度退回，结果即"虚拟行起始字节"（行首插入内容算作该行前缀，而不是上一行末尾）。
    if (l2Cold_.isEmpty() && l1Hot_.isEmpty()) return origLineStart;
    qint64 v = origToVirt(origLineStart);
    for (const auto& p : l1Hot_) {
        if (p.oldLength != 0) continue;
        if (p.origInsertOffset == origLineStart) v -= p.newText.size();
    }
    if (v < 0) v = 0;
    return v;
}

qint64 PatchList::virtToOrig(qint64 virtByte) const {
    if (l2Cold_.isEmpty() && l1Hot_.isEmpty()) return virtByte;

    auto compute = [&](const QVector<const Patch*>& sorted) -> qint64 {
        qint64 virtCursor = 0, origCursor = 0;
        for (const Patch* p : sorted) {
            qint64 gap = p->byteOffset - virtCursor;
            if (gap > 0) {
                if (virtByte < virtCursor + gap)
                    return origCursor + (virtByte - virtCursor);
                virtCursor += gap;
                origCursor += gap;
            }
            if (virtByte < virtCursor + p->newText.size())
                return origCursor;  // 虚拟字节在插入内容内，映射到 patch 起始原始位置
            virtCursor += p->newText.size();
            origCursor += p->oldLength;
        }
        return origCursor + (virtByte - virtCursor);
    };

    if (l2Cold_.isEmpty()) {
        QVector<const Patch*> sorted;
        sorted.reserve(l1Hot_.size());
        for (const auto& p : l1Hot_) sorted.append(&p);
        return compute(sorted);
    }
    MergedView view;
    buildMergedView(view);
    return compute(view.sorted);
}

// ============================================================================
//  applyAndSave 流式 4MB
// ============================================================================
bool PatchList::applyAndSave(const uchar* mmapBase, qint64 mmapSize, const QString& outPath,
                             std::function<void(qint64,qint64)> progress,
                             std::atomic<bool>* cancel,
                             QString* errMsg) {
    qint64 netDelta = 0;
    MergedView view;
    buildMergedView(view);
    for (const Patch* p : view.sorted) netDelta += patchNetDelta(*p);
    // 使用 virtualSize 而非 mmapSize+netDelta：当 patch 间存在 gap（byteOffset
    // 超出当前虚拟末尾）时，virtualSize 包含 gap 的 NUL 填充字节，确保保存不丢字节
    const qint64 virtSize = virtualSize(mmapSize);

    auto doWrite = [&](QIODevice& dev, QString* perr) -> bool {
        constexpr qint64 BLK = qint64(4) * 1024 * 1024;
        for (qint64 pos = 0; pos < virtSize; pos += BLK) {
            qint64 ed = qMin(virtSize, pos + BLK);
            QByteArray chunk = renderRange(mmapBase, mmapSize, pos, ed);
            if (chunk.size() != ed - pos) {
                if (perr) {
                    qint64 d = ed - pos - chunk.size();
                    *perr = QObject::tr("渲染块大小不匹配（块 %1..%2，期望 %3 字节，实际 %4 字节，差 %5）\n"
                                        "诊断：netDelta=%6, virtSize=%7, mmapSize=%8, patchCount=%9")
                        .arg(pos).arg(ed)
                        .arg(ed - pos).arg(qint64(chunk.size())).arg(d)
                        .arg(netDelta).arg(virtSize).arg(mmapSize)
                        .arg(int(view.sorted.size()));
                }
                return false;
            }
            qint64 w = dev.write(chunk);
            if (w != chunk.size()) {
                if (perr) *perr = QObject::tr("写入失败：%1").arg(dev.errorString());
                return false;
            }
            if (progress) progress(ed, virtSize);
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                if (perr) *perr = QObject::tr("用户取消。");
                return false;
            }
        }
        return true;
    };

    // 首选：QSaveFile 原子写入
    {
        QSaveFile sf(outPath);
        if (!sf.open(QIODevice::WriteOnly)) {
            if (errMsg) *errMsg = QObject::tr("无法写入：%1").arg(sf.errorString());
            return false;
        }
        bool ok = doWrite(sf, errMsg);
        if (!ok) {
            sf.cancelWriting();
            return false;
        }
        if (sf.commit()) return true;
        // 原子提交失败：保存错误信息用于 fallback
        QString atomicErr = sf.errorString();
        // QSaveFile 写入已成功但 commit 失败（典型 Windows：临时文件被占用、
        // 目标路径权限奇怪、杀毒软件锁文件等）。
        // Fallback：直接用 QFile 覆盖写入（牺牲原子性换取可用保存）。
        QFile plain(outPath);
        if (!plain.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errMsg) *errMsg = QObject::tr("原子提交失败：%1；降级覆盖写入亦失败：%2")
                                        .arg(atomicErr, plain.errorString());
            return false;
        }
        bool fok = doWrite(plain, errMsg);
        plain.close();
        if (!fok) return false;
        if (errMsg) *errMsg = QObject::tr("已降级为普通覆盖写入（原原子提交失败：%1）。文件已保存。")
                                    .arg(atomicErr);
        return true;
    }
}

void PatchList::resetAfterSave() {
    if (sensitive_) {
        const auto scrubPatch = [](Patch &patch) {
            if (!patch.newText.isEmpty())
                patch.newText.fill(0);
            if (!patch.oldSnapshot.isEmpty())
                patch.oldSnapshot.fill(0);
        };
        for (Patch &patch : l1Hot_)
            scrubPatch(patch);
        const auto scrubStack = [&scrubPatch](QList<UndoGroup> &stack) {
            for (UndoGroup &group : stack) {
                for (Patch &patch : group.patchesSnapshot)
                    scrubPatch(patch);
            }
        };
        scrubStack(undoStack_);
        scrubStack(redoStack_);
    }
    l1Hot_.clear();
    l1Bytes_ = 0;
    for (const auto& f : l2Cold_) QFile::remove(f.filePath);
    l2Cold_.clear();
    undoStack_.clear();
    redoStack_.clear();
    inExplicitGroup_ = false;
}

bool PatchList::pressureWillExceedSoftWarn(qint64 estimatedEditSizeBytes) const {
    return l1Bytes_ + estimatedEditSizeBytes > l1Limit_ * 4 / 5;
}
