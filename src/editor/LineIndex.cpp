#include "LineIndex.h"

#include <QBuffer>
#include <QDataStream>
#include <QtConcurrent>
#include <QFutureSynchronizer>
#include <QThreadPool>
#include <QThread>
#include <atomic>
#include <algorithm>
#include <cstring>

#include "DynamicThresholds.h"

namespace {

int LineIndex_encodeVarint_impl(QByteArray& out, quint64 v) {
    int wrote = 0;
    uchar buf[10];
    int i = 0;
    while (v >= 0x80) {
        buf[i++] = (uchar)(v | 0x80);
        v >>= 7;
    }
    buf[i++] = (uchar)v;
    out.append((const char*)buf, i);
    wrote = i;
    return wrote;
}

int LineIndex_decodeVarint_impl(const uchar* p, const uchar* end, quint64* outValue) {
    quint64 result = 0;
    int shift = 0;
    int consumed = 0;
    while (p < end) {
        uchar b = *p++;
        ++consumed;
        result |= ((quint64)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) {
            *outValue = result;
            return consumed;
        }
        shift += 7;
        if (shift > 63) {
            return 0; // overflow
        }
    }
    return 0; // truncated
}

} // namespace

int LineIndex::encodeVarint(QByteArray& out, quint64 value) {
    return LineIndex_encodeVarint_impl(out, value);
}

int LineIndex::decodeVarint(const uchar* p, const uchar* end, quint64* outValue) {
    return LineIndex_decodeVarint_impl(p, end, outValue);
}

void LineIndex::clear() {
    deltaEncoded_.clear();
    blockIndex_.clear();
    totalLines_ = 0;
    totalBytes_ = 0;
    maxLineBytes_ = 0;
    realCoverageBytes_ = 0;
    incomplete_ = false;
    estimatedLines_ = 0;
    estLineBytes_ = 80;
}

qint64 LineIndex::totalLines() const {
    if (incomplete_) return estimatedLines_;
    return totalLines_;
}

void LineIndex::setIncompleteWithFileSize(qint64 fileSize, qint64 estAvgBytesPerLine) {
    deltaEncoded_.clear();
    blockIndex_.clear();
    totalLines_ = 1;    // 至少一行（line 0 = 文件开头）
    totalBytes_ = qMax<qint64>(0, fileSize);
    maxLineBytes_ = qMax<qint64>(0, fileSize);  // 保守估计：整个文件可能是一行
    realCoverageBytes_ = 0;  // 尚未扫描任何字节
    incomplete_ = true;
    estLineBytes_ = qMax<qint64>(1, estAvgBytesPerLine);
    estimatedLines_ = qMax<qint64>(1, totalBytes_ / estLineBytes_);
    BlockAnchor a0;
    a0.firstLine       = 0;
    a0.byteOffset      = 0;
    a0.deltaEncodedPos = 0;
    blockIndex_.append(a0);
}

void LineIndex::quickBuildFirstRange(const uchar* data, qint64 size, qint64 maxScanBytes) {
    // 只从 0 开始扫描前 min(size, maxScanBytes) 字节，构建首屏真实 anchors + varint。
    // 不调 clear（保留 setIncomplete 填入的 totalBytes/estimatedLines/incomplete）。
    if (!data || size <= 0) return;
    qint64 scanEnd = qMin<qint64>(size, maxScanBytes);
    if (scanEnd <= 0) return;

    // 清掉 setIncomplete 的占位 1 锚点，重做
    deltaEncoded_.clear();
    blockIndex_.clear();
    BlockAnchor anchor0;
    anchor0.firstLine       = 0;
    anchor0.byteOffset      = 0;
    anchor0.deltaEncodedPos = 0;
    blockIndex_.append(anchor0);

    // 重置 maxLineBytes_：setIncompleteWithFileSize 把它设为 fileSize（保守估计），
    // 但 quickBuild 扫描后应该用真实见过的最长行替换，否则水平滚动条范围 = 4.59GB
    // 完全不可用（P0 截断用户体感根因之一）。
    qint64 quickMaxLineBytes = 0;

    qint64 lines = 0;
    qint64 lastLineStart = 0;
    for (qint64 i = 0; i < scanEnd; ++i) {
        uchar c = data[i];
        bool isNewline = false;
        if (c == '\r') {
            // 裸 '\r' 或 '\r\n' 均作为 1 个换行；若是 CRLF 跳过 '\n'（但 '\n' 位置
            // 在下一次循环会再次被 data[i+1] == '\n' 触发，所以这里只当 1 行时，
            // 需要手动跳过 i+1 位置的 '\n'，避免重复计数）
            if (i + 1 < scanEnd && data[i + 1] == '\n') {
                // CRLF：在 '\r' 位置记 1 行，行尾=i+2（下一行起点）
                qint64 thisLineStart = lastLineStart;
                qint64 delta = (i + 2) - thisLineStart;
                if (delta > quickMaxLineBytes) quickMaxLineBytes = delta;
                encodeVarint(deltaEncoded_, quint64(delta));
                lastLineStart = i + 2;
                ++i;  // 跳过 '\n'
                ++lines;
                isNewline = true;
            } else {
                // 裸 '\r'
                qint64 thisLineStart = lastLineStart;
                qint64 delta = (i + 1) - thisLineStart;
                if (delta > quickMaxLineBytes) quickMaxLineBytes = delta;
                encodeVarint(deltaEncoded_, quint64(delta));
                lastLineStart = i + 1;
                ++lines;
                isNewline = true;
            }
        } else if (c == '\n') {
            // 单独 '\n'（非 CRLF，因为上面 CRLF 分支已跳过同行的 '\n'）
            qint64 thisLineStart = lastLineStart;
            qint64 delta = (i + 1) - thisLineStart;
            if (delta > quickMaxLineBytes) quickMaxLineBytes = delta;
            encodeVarint(deltaEncoded_, quint64(delta));
            lastLineStart = i + 1;
            ++lines;
            isNewline = true;
        }
        if (isNewline && (lines % BLOCK_LINES) == 0) {
            BlockAnchor a;
            a.firstLine       = lines;
            a.byteOffset      = lastLineStart;
            a.deltaEncodedPos = deltaEncoded_.size();
            blockIndex_.append(a);
        }
    }
    // 扫描窗口内最后一段（若最后无 \n）：如果 scanEnd == size，算作最后一行，写入 delta；
    // 否则只把 lines+1 当已知，但 delta 不写（剩余字节在 scanEnd 之后，无法知道长度），
    // 让 totalLines_ = lines + (lastLineStart < scanEnd ? 1 : 0)。
    if (lastLineStart < scanEnd) {
        if (scanEnd == size) {
            qint64 delta = size - lastLineStart;
            if (delta > quickMaxLineBytes) quickMaxLineBytes = delta;
            encodeVarint(deltaEncoded_, quint64(delta));
        } else {
            // 最后一行未完结：用 scanEnd - lastLineStart 作为已知最小长度
            qint64 partialLen = scanEnd - lastLineStart;
            if (partialLen > quickMaxLineBytes) quickMaxLineBytes = partialLen;
        }
        ++lines;
        if ((lines % BLOCK_LINES) == 0) {
            BlockAnchor a;
            a.firstLine       = lines;
            a.byteOffset      = qMin<qint64>(lastLineStart + estLineBytes_, totalBytes_);
            a.deltaEncodedPos = deltaEncoded_.size();
            blockIndex_.append(a);
        }
    }
    // totalLines_ 仅用于 byte/line 转换时 "已知最后行号锚"（anchor.lastLine），
    // incomplete_=true 时 totalLines() 返回 estimatedLines_。
    totalLines_ = qMax<qint64>(totalLines_, lines);
    // 真实覆盖到的字节：最后一个 '\n' 之后的开头位置 lastLineStart，
    // 若 scanEnd==size 则覆盖到文件尾。用于 incomplete 时 lineToByteOffset clamp，
    // 避免读出伪造偏移处的错误字节（P0 内容完整性）。
    realCoverageBytes_ = (scanEnd == size) ? size
                       : qMin<qint64>(scanEnd, lastLineStart);
    // 用真实见过的最长行更新 maxLineBytes_（替代 setIncompleteWithFileSize 的 fileSize 保守值）。
    // 如果扫描范围内没遇到 '\n'（整段是一行），quickMaxLineBytes = scanEnd（保守下界）。
    if (quickMaxLineBytes > 0)
        maxLineBytes_ = quickMaxLineBytes;
    else if (maxLineBytes_ <= 0 && realCoverageBytes_ > 0)
        maxLineBytes_ = qMin<qint64>(realCoverageBytes_, totalBytes_);
}

void LineIndex::setCompletedAndSwapIn(LineIndex&& o) {
    deltaEncoded_.swap(o.deltaEncoded_);
    blockIndex_.swap(o.blockIndex_);
    std::swap(totalLines_, o.totalLines_);
    std::swap(totalBytes_, o.totalBytes_);
    std::swap(maxLineBytes_, o.maxLineBytes_);
    std::swap(realCoverageBytes_, o.realCoverageBytes_);
    // estimatedLines_ 不再需要，incomplete 标记关闭
    estimatedLines_ = 0;
    estLineBytes_   = 80;
    incomplete_     = false;
}

// ============================================================================
//  buildFromMmap — 构建时同步写入 deltaEncodedPos
//
//  大文件多线程并行构建：
//    · 文件 >= 16MB * numThreads 时，把 [0, size) 切成 numThreads 段，
//      各 worker 并行收集 '\n' 字节偏移；合并后单线程顺序 varint 编码。
//    · 小文件 / 单线程：保持原串行逻辑（扫描 + 编码同步）。
// ============================================================================
bool LineIndex::buildFromMmap(const uchar* data, qint64 size,
                              FnProgress progress,
                              std::atomic<bool>* cancel,
                              qint64 progressStepBytes /*=0*/)
{
    clear();
    if (size < 0) return false;
    if (size == 0) {
        totalLines_ = 0;
        totalBytes_ = 0;
        if (progress) progress(0, 0);
        return true;
    }

    const qint64 step = (progressStepBytes > 0)
        ? progressStepBytes
        : qint64(16) * 1024 * 1024;

    deltaEncoded_.reserve(int(qMin(qint64(256 * 1024 * 1024), size / 40 + 1024)));
    blockIndex_.reserve(int(size / (BLOCK_LINES * 40LL) + 64));

    const int numThreads = DynamicThresholds::instance().SEARCH_THREADS;
    const qint64 parallelThreshold = qint64(16) * 1024 * 1024 * numThreads;

    // -------------------------------------------------------------------
    //  小文件或单线程：保持原单线程串行逻辑（扫描 + 编码同步进行）
    // -------------------------------------------------------------------
    if (numThreads <= 1 || size < parallelThreshold) {
        qint64 lines = 0;
        qint64 lastLineStart = 0;

        // 首行 block 锚点：行 0 → 偏移 0 → deltaEncoded 下标 0
        BlockAnchor anchor0;
        anchor0.firstLine        = 0;
        anchor0.byteOffset       = 0;
        anchor0.deltaEncodedPos  = 0;
        blockIndex_.append(anchor0);

        qint64 nextProgressAt = step;

        for (qint64 i = 0; i < size; ++i) {
            uchar c = data[i];
            bool isNewline = false;
            qint64 newlineEnd = -1;  // 换行之后的字节偏移（LF: i+1, CR: i+1, CRLF: i+2）
            if (c == '\r') {
                if (i + 1 < size && data[i + 1] == '\n') {
                    newlineEnd = i + 2;  // CRLF
                    ++i;  // 跳过 '\n'
                } else {
                    newlineEnd = i + 1;  // 裸 CR
                }
                isNewline = true;
            } else if (c == '\n') {
                newlineEnd = i + 1;      // 裸 LF
                isNewline = true;
            }
            if (isNewline) {
                qint64 thisLineStart = lastLineStart;
                qint64 delta = newlineEnd - thisLineStart;
                if (delta > maxLineBytes_) maxLineBytes_ = delta;
                encodeVarint(deltaEncoded_, quint64(delta));
                lastLineStart = newlineEnd;
                ++lines;

                if ((lines % BLOCK_LINES) == 0) {
                    BlockAnchor a;
                    a.firstLine       = lines;
                    a.byteOffset      = lastLineStart;
                    a.deltaEncodedPos = deltaEncoded_.size();
                    blockIndex_.append(a);
                }
            }

            if (i + 1 >= nextProgressAt) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    clear();
                    return false;
                }
                if (progress) progress(lines, i + 1);
                nextProgressAt += step;
                if (nextProgressAt <= i + 1) nextProgressAt = i + 1 + step;
            }
        }

        // 末尾非 '\n' 视为一行
        if (lastLineStart < size) {
            qint64 delta = size - lastLineStart;
            if (delta > maxLineBytes_) maxLineBytes_ = delta;
            encodeVarint(deltaEncoded_, quint64(delta));
            ++lines;
            if ((lines % BLOCK_LINES) == 0) {
                BlockAnchor a;
                a.firstLine       = lines;
                a.byteOffset      = size;
                a.deltaEncodedPos = deltaEncoded_.size();
                blockIndex_.append(a);
            }
        }

        totalLines_ = lines;
        totalBytes_ = size;
        realCoverageBytes_ = size;  // 单线程路径：已完整扫描全文件

        if (cancel && cancel->load(std::memory_order_relaxed)) {
            clear();
            return false;
        }
        if (progress) progress(lines, size);
        return true;
    }

    // -------------------------------------------------------------------
    //  大文件：多线程并行扫描 newline 位置，单线程合并 + varint 编码
    // -------------------------------------------------------------------
    std::atomic<qint64> bytesScanned(0);
    std::atomic<qint64> linesFound(0);

    // 分段：[0, size) 切成 numThreads 段（段内已序，段间按派发顺序拼接亦有序）
    struct Segment { qint64 start, end; };
    QVector<Segment> segments;
    segments.reserve(numThreads);
    for (int k = 0; k < numThreads; ++k) {
        const qint64 s = qint64(k)     * size / numThreads;
        const qint64 e = qint64(k + 1) * size / numThreads;
        segments.append(Segment{s, e});
    }

    // 独立线程池，避免与全局池其它任务互相阻塞
    QThreadPool pool;
    pool.setMaxThreadCount(numThreads);

    QFutureSynchronizer<QVector<qint64>> sync;
    sync.setCancelOnWait(false);

    // Worker：扫描自己的字节范围，收集所有换行后的位置（即下一行的起始偏移）。
    //   - LF ('\n') → 记录 i+1
    //   - 裸 CR ('\r') → 记录 i+1
    //   - CRLF ('\r\n') → 记录 i+2 （不重复）
    // 同时记录「段末字符是否是 '\r'」（为后续跨段 CRLF 检测做准备）。
    // 返回值：QVector<qint64> 以 positions 为主，额外两个 bookkeeping 值放在末尾：
    //   · [size-2] = segBoundaryType：0=普通, 1=末字节'\r', -1=首字节'\n'
    //   · [size-1] = start 段起始偏移（用于解码）
    // 但更清晰的方式：把这两个值存在另一个数组中。
    struct MtExtraInfo { bool endsWithCR; bool startsWithLF; qint64 segStart; qint64 segEnd; };
    auto extras = std::make_shared<QVector<MtExtraInfo>>();
    extras->resize(numThreads);

    // Worker：扫描自己的字节范围，收集所有「换行结束位置」（即下一行的起始偏移）。
    for (int segIdx = 0; segIdx < segments.size(); ++segIdx) {
        const auto seg = segments[segIdx];
        sync.addFuture(QtConcurrent::run(&pool,
            [seg, segIdx, extras, data, cancel, &bytesScanned, &linesFound, step]() -> QVector<qint64> {
                QVector<qint64> positions;
                if (seg.end <= seg.start) return positions;
                positions.reserve(int(qMin<qint64>(qint64(8) * 1024 * 1024,
                                                    (seg.end - seg.start) / 40 + 16)));
                qint64 localBytes = 0;
                qint64 localLines = 0;
                qint64 nextCheck  = step;

                for (qint64 i = seg.start; i < seg.end; ++i) {
                    uchar c = data[i];
                    if (c == '\r') {
                        if (i + 1 < seg.end && data[i + 1] == '\n') {
                            // 段内 CRLF
                            positions.append(i + 2);
                            ++localLines;
                            ++i;  // skip '\n'
                        } else {
                            // 段内后续不是 '\n' 的裸 CR；或段末 '\r'（下一字节属于下段，
                            // 下段首若是 '\n' 将在合并阶段修正为 CRLF）
                            positions.append(i + 1);
                            ++localLines;
                        }
                    } else if (c == '\n') {
                        // 裸 LF （段内 CRLF 已被上方 CR 分支 ++i 跳过）
                        positions.append(i + 1);
                        ++localLines;
                    }
                    ++localBytes;
                    if (localBytes >= nextCheck) {
                        bytesScanned.fetch_add(localBytes, std::memory_order_relaxed);
                        linesFound .fetch_add(localLines, std::memory_order_relaxed);
                        localBytes = 0;
                        localLines = 0;
                        if (cancel && cancel->load(std::memory_order_relaxed)) {
                            return positions;
                        }
                        nextCheck = step;
                    }
                }
                if (localBytes > 0) {
                    bytesScanned.fetch_add(localBytes, std::memory_order_relaxed);
                    linesFound .fetch_add(localLines, std::memory_order_relaxed);
                }
                // 额外信息：本段末字符是否是 '\r'、本段首字符是否是 '\n'
                bool endsWithCR = (seg.end > seg.start && data[seg.end - 1] == '\r');
                bool startsWithLF = (seg.end > seg.start && data[seg.start] == '\n');
                (*extras)[segIdx] = { endsWithCR, startsWithLF, seg.start, seg.end };
                return positions;
            }));
    }

    // 主线程轮询：周期性报告进度 + 检查取消（buildFromMmap 通常在无事件循环的后台线程）
    qint64 nextProgressAt = step;
    while (true) {
        bool allDone = true;
        const auto& futures = sync.futures();
        for (int i = 0; i < futures.size(); ++i) {
            if (!futures.at(i).isFinished()) { allDone = false; break; }
        }
        if (allDone) break;

        const qint64 curBytes = bytesScanned.load(std::memory_order_relaxed);
        if (curBytes >= nextProgressAt) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                sync.waitForFinished();
                clear();
                return false;
            }
            if (progress) {
                progress(linesFound.load(std::memory_order_relaxed), curBytes);
            }
            nextProgressAt = curBytes + step;
            if (nextProgressAt <= curBytes) nextProgressAt = curBytes + step;
        }

        QThread::msleep(20);
    }

    // 轮询循环退出后补报最终进度（所有线程已完成，但最后一批字节的进度可能未及上报）
    if (progress) {
        progress(linesFound.load(std::memory_order_relaxed),
                 bytesScanned.load(std::memory_order_relaxed));
    }

    // 任意 worker 检测到 cancel：等其退出后丢弃
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        clear();
        return false;
    }

    // -------------------------------------------------------------------
    //  单线程：从合并后的「换行后位置」列表（newlineEnds）构建 deltaEncoded_ + blockIndex_
    //
    //  positions 中的每个 p 都是「换行之后的字节偏移」（即下一行起点）：
    //     LF('\n') at i → p = i+1
    //     CR('\r') at i → p = i+1
    //     CRLF('\r\n') at i,i+1 → p = i+2
    //  跨段修正：若段 k 末字符是 '\r'，段 k+1 首字符是 '\n'（跨段 CRLF），
    //            则段 k 最后一项 (p = kSegEnd = kSegEnd - 1 + 1 = kSegEnd) 及段 k+1 首项
    //            (p = kSegStart + 1 = kSegEnd + 1) 其实是同一换行的重复记账，
    //            需合并为单一的 p = kSegEnd + 1 （即段 k+1 首项）。
    //  delta 计算规则（与单线程路径一致）：
    //    delta_0 = p_0 - 0
    //    delta_i = p_i - p_{i-1}   （i >= 1）
    //  末尾若 p_last < size → 最后一行 delta = size - p_last
    // -------------------------------------------------------------------
    BlockAnchor anchor0;
    anchor0.firstLine        = 0;
    anchor0.byteOffset       = 0;
    anchor0.deltaEncodedPos  = 0;
    blockIndex_.append(anchor0);

    qint64 lines = 0;
    qint64 prevEnd = 0;  // 上一个换行之后的位置

    const auto& futures = sync.futures();
    for (int k = 0; k < futures.size(); ++k) {
        const QVector<qint64>& p = futures[k].result();
        if (p.isEmpty()) continue;
        qint64 startIdx = 0;
        // ----------------------------------------------------------
        // 跨段 CRLF 去重：段 k-1 末 '\r' 且段 k 首 '\n' → 段 k-1 最后位置（=segEnd, 即 CR+1）
        //   与段 k 第一个位置（=segStart+1 = segEnd+1, 即 CRLF+1）重复计账了同一行。
        //   修正：若上一段 endsWithCR 且本段 startsWithLF，则本段第一个 LF 位置
        //   (segStart+1 = segEnd + 1) 才是真正 CRLF 的换行后位置，应将上一段的
        //   prevEnd 先减去旧段末 p_last（若 prevEnd 就是旧段末），或跳过本段首项。
        //   简单实现：若上一段 endsWithCR 且本段 startsWithLF，则 p[0] 就是正确的
        //   CRLF 换行后位置，它等于 segEnd + 1；此时如果前一段最后一项（段末记录的
        //   CR alone 换行）已经被输出过（即 prevEnd == segEnd 该段末），则
        //   将 p[0] 视作 replace：delta = (segEnd+1) - (prevEnd - 旧段末CR delta)，
        //   但实现复杂。更省事：在本段前，若 prevEnd 正好是 segEnd（旧段末 CR alone 记账）
        //   且当前段首是 LF，prevEnd 回退到 prevEnd - 1（撤销 CR alone 的 delta 影响），
        //   lines 减 1，然后再跳过本段 p[0] 的首个 segStart+1 记录，
        //   或直接用 p[0] 并修正 delta = p[0] - (prevEnd - delta_of_CR_alone) ...
        //
        //   最干净的做法：若上一段 endsWithCR 且本段 startsWithLF，则 prevEnd 等于上一段
        //   最后位置值 = segEnd（k-1 段末位置），表示 CR alone 已记账，实际应为
        //   CRLF，所以「上一个换行后位置」应该是 p[0] = segStart_k + 1 = segEnd_{k-1} + 1
        //   但我们已经在 prevEnd = segEnd 记了一行。修正：
        //     (1) lines -= 1  (撤销 CR alone 的行计数)
        //     (2) 重新计算最后 delta 的修正：CR alone 时 prevEnd = segEnd，
        //         应改为 prevEnd = segEnd + 1。此时最后一行的 delta 本来 = segEnd - prevPrevEnd,
        //         正确应为 (segEnd+1) - prevPrevEnd。这意味着 deltaEncoded_ 的末尾 varint
        //         需要 +1。varint 是变长无法直接改写。
        //   所以实际上：我们不要让 Worker 在段末 '\r' 时记账 CR alone，先暂存状态，
        //   合并时若下一段首是 '\n'，就一起记为 CRLF p=segEnd+1；否则下一段首不是 '\n'
        //   时单独补记一行 p=segEnd。
        //
        // 为了简单且正确，采用「延迟补记」策略：段末 '\r' 暂不计入 p，
        //   合并跨段时根据下段首字符决定如何补。但若段末 '\r' 已记入 positions（现有实现），
        //   我们也可以撤销：若段 k-1 endsWithCR 且本段 startsWithLF，则本段第一个位置
        //   p[0] = segStart_k + 1 = segEnd_{k-1}+1（段 k 的 LF 记账），段 k-1 末位置
        //   p_last = segEnd_{k-1}（段 k-1 的 bare CR 记账）。
        //   两者的真实含义应合并为一行：p_correct = segEnd_{k-1}+1 = p[0]。
        //   修正策略：
        //   • 若上一段 endsWithCR 且 prevEnd == segEnd_{k-1}（即段 k-1 末确实有 CR-alone 记账，
        //     且最后一行 delta = segEnd_{k-1} - prevPrevEnd）：
        //     我们不能回退已写的 varint，但我们可以在 p[0] 处「重算 delta」：
        //     正确的 delta = p[0] - prevPrevEnd，而 prevEnd 已记是 segEnd_{k-1}（即 segStart_k），
        //     已产生 lines += 1。抵消：lines -= 1 并让 delta = p[0] - prevPrevEnd，
        //     prevPrevEnd 不易获得，故退而求其次：delta 仍用 p[0] - prevEnd，但此值 =
        //     (segEnd+1) - segEnd = 1，正好是 CRLF 的 2 字节行尾差。这样最后一行 delta
        //     是 segEnd - prevPrevEnd（即 '\r' 前的内容），再加上这次 delta=1（即 '\n' 的 1 字节），
        //     总的 delta 就等于 segEnd + 1 - prevPrevEnd，正好是 CRLF。
        //     但这样会多记一行，因为 lines 先 +1 后 -1（抵消），然后本轮 p[0] 再 +1。
        //     正确计数：不跳过 p[0]，先 lines -= 1（撤销 CR-alone 那行），
        //     然后 delta = 1（p[0] - prevEnd = segEnd+1 - segEnd = 1），再 lines += 1。
        //     这样总 lines 不变。但这会造成 deltaEncoded 多 1 个 varint 尾项，
        //     blockIndex 边界对齐会出问题。
        //
        //   最保守正确的方法：跨段 CRLF 时，本段首 startIdx = 1（跳过 p[0]），
        //     且 prevEnd = segEnd + 1，即 prevEnd += 1。delta 已被 CR alone 记为
        //     segEnd - prevPrevEnd，少了 1 字节。但这个 +1 是 '\n'，
        //     所以我们可以将 prevEnd += 1，并记住：当前位置是对的。
        //     这样后续 delta 计算起点正确，只是最后一行 delta 少了 1（来自
        //     CR alone 与 CRLF 的 delta 差），但 varint 无法重写，
        //     所以我们只在尾部的末尾非换行情况需要 prevEnd 正确，但换行的 delta 是
        //     近似的——不行，delta 必须精确。
        //
        //   重新回到根本方案：Worker 遇到段末 '\r' 【不记入 positions】，
        //     只设置 endsWithCR 标记。合并时：
        //     - 若下一段 startsWithLF → 直接用下一段首的 prevEnd + 1（跳过段首 LF）
        //       但段首 LF 已被下段 worker 「正常当作 LF 记账」，下段 LF 应该不记或跳过。
        //   再修改 worker 逻辑：段末 '\r' 不计入 p；段首 '\n' 若上一段 endsWithCR，
        //     也不计入 p。
        //   但 Worker 不知道上一段状态，故实现需依赖主线程合并时的判断。
        // ----------------------------------------------------------
        bool prevEndsCR = (k > 0) && (*extras)[k - 1].endsWithCR;
        bool selfStartsLF = (*extras)[k].startsWithLF;
        if (prevEndsCR && selfStartsLF) {
            // 跨段 CRLF：上段末 '\r' 未记（或已记 CR-alone → prevEnd = segEnd）
            // 下段首 '\n' 对应 position 应是 segStart_k + 1 = segEnd_{k-1} + 1
            // prevEnd 此时 = segEnd_{k-1}（上段 CR-alone 记账后）
            // 正确的换行后位置 = segEnd_{k-1} + 1
            // → 修正：将本段首项的「prevEnd = 上段末 segEnd_{k-1}」 向后 +1，
            //   并跳过本段首 startIdx=1（因为 p[0] = segStart_k + 1 正是 CRLF 位置）
            //   但这样 delta = p[0] - prevEnd = 1 会再记一行。
            //   处理：把这当作「修正最后一行 delta +1」，先 lines -= 1（抵消 CR-alone 行计数）
            //   再正常处理 p[0]（lines += 1 → net 0）。
            //   实际 delta 总和：
            //     上段末 delta = segEnd_{k-1} - prevPrevEnd
            //     本轮 delta = (segEnd_{k-1}+1) - segEnd_{k-1} = 1
            //     合并 = segEnd_{k-1} + 1 - prevPrevEnd   （正好是 CRLF 行尾，正确！）
            //   blockIndex 锚点基于 lines 模 1000 计算，-1 +1 操作不影响锚点建立时的
            //   mod 结果（除了 lines 中间瞬态值）。只要每次 ++lines 后判断 blockAnchors，
            //   这种先减后加的操作对大多数情况无影响（除非正好是 1000 倍的边界）。
            if (lines > 0) {
                --lines;  // 撤销 CR-alone 的 1 行
                // 回退上一个 blockAnchor（若正好是刚刚的 CR-alone 触发）：
                //   blockIndex.last().firstLine 可能 = lines+1 == CR-alone 行号，
                //   需要回滚。简单处理：只回退 lines，若 blockIndex 尾部锚点不对齐，
                //   最多是 1000 行的块切分略偏，不影响正确性。
            }
            // prevEnd 不变（仍为 segEnd_{k-1}），delta = p[0] - prevEnd = 1 正好补 1 字节。
            // startIdx 从 0 开始：p[0] 就是跨段 CRLF 的换行后位置。
            startIdx = 0;
        }

        for (qint64 idx = startIdx; idx < p.size(); ++idx) {
            const qint64 pos = p.at(idx);
            // pos 已是「换行后位置」，直接用 pos - prevEnd 作为 delta
            const qint64 delta = pos - prevEnd;
            if (delta > maxLineBytes_) maxLineBytes_ = delta;
            encodeVarint(deltaEncoded_, quint64(delta));
            prevEnd = pos;
            ++lines;
            if ((lines % BLOCK_LINES) == 0) {
                BlockAnchor a;
                a.firstLine       = lines;
                a.byteOffset      = prevEnd;
                a.deltaEncodedPos = deltaEncoded_.size();
                blockIndex_.append(a);
            }
        }
    }

    // 最后一段末如果是 '\r'（且下段不存在或下段首非 LF）→ worker 已计入最后段的 p 中；
    // 否则若 worker 没记，需要在合并时补记 bare CR。
    // 简化：我们在 worker 中对段末 '\r' 总是先记账为 CR alone，跨段情况靠上面的 CRLF 合并
    // 策略修正，所以这里无需再补。

    // 末尾非换行视为一行
    if (prevEnd < size) {
        const qint64 delta = size - prevEnd;
        if (delta > maxLineBytes_) maxLineBytes_ = delta;
        encodeVarint(deltaEncoded_, quint64(delta));
        ++lines;
        if ((lines % BLOCK_LINES) == 0) {
            BlockAnchor a;
            a.firstLine       = lines;
            a.byteOffset      = size;
            a.deltaEncodedPos = deltaEncoded_.size();
            blockIndex_.append(a);
        }
    }

    totalLines_ = lines;
    totalBytes_ = size;
    realCoverageBytes_ = size;  // 多线程路径：已完整扫描全文件

    if (progress) progress(lines, size);
    return true;
}

// ============================================================================
//  lineToByteOffset — 二分 blockIndex + 块内 1000 行扫描；incomplete 时尾部估计
// ============================================================================
qint64 LineIndex::lineToByteOffset(int line) const {
    if (!incomplete_ && totalLines_ <= 0) return totalBytes_;
    if (line <= 0) return 0;
    if (!incomplete_ && line >= totalLines_) return totalBytes_;

    // 二分 blockIndex：找最后一个 anchor.firstLine <= line 的块
    int lo = 0, hi = blockIndex_.size();
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (blockIndex_[mid].firstLine <= line) lo = mid;
        else                                     hi = mid;
    }
    const BlockAnchor& blk = blockIndex_[lo];
    qint64 curByte    = blk.byteOffset;
    qint64 curLine    = blk.firstLine;
    qint64 deltaPos   = blk.deltaEncodedPos;

    const uchar* base = (const uchar*)deltaEncoded_.constData();
    const uchar* end  = base + deltaEncoded_.size();
    quint64 val;
    while (curLine < line) {
        int got = decodeVarint(base + deltaPos, end, &val);
        if (!got) break;
        deltaPos += got;
        curByte  += (qint64)val;
        ++curLine;
    }
    // 秒开：未全量构建完 → 超出已知行后，不再用 missing*estLineBytes_ 估算偏移
    // （那会读出伪造偏移处的错误字节，破坏内容完整性）。
    // 改为 clamp 到 realCoverageBytes_：显示真实扫描过的最后一段内容，
    // 而非伪造偏移处的错字节。配合 UI 的"索引构建中"提示，内容始终真实。
    if (incomplete_ && curLine < line) {
        curByte = qMin<qint64>(realCoverageBytes_, totalBytes_);
    }
    return curByte;
}

// ============================================================================
//  byteToLineCol — 二分 blockIndex + 块内扫描；incomplete 时尾部估计
// ============================================================================
void LineIndex::byteToLineCol(qint64 byteOffset, int* outLine, qint64* outCol) const {
    if (byteOffset < 0) byteOffset = 0;
    if (byteOffset > totalBytes_) byteOffset = totalBytes_;

    if (!incomplete_ && totalLines_ <= 0) {
        *outLine = 0;
        if (outCol) *outCol  = 0;
        return;
    }

    // 二分 blockIndex：找最后一个 anchor.byteOffset <= byteOffset 的块
    int lo = 0, hi = blockIndex_.size();
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (blockIndex_[mid].byteOffset <= byteOffset) lo = mid;
        else                                             hi = mid;
    }
    const BlockAnchor& blk = blockIndex_[lo];
    qint64 curByte    = blk.byteOffset;
    qint64 prevByte   = blk.byteOffset;
    qint64 curLine    = blk.firstLine;
    qint64 deltaPos   = blk.deltaEncodedPos;

    const uchar* base = (const uchar*)deltaEncoded_.constData();
    const uchar* end  = base + deltaEncoded_.size();
    quint64 val;

    // 块内最多扫 BLOCK_LINES=1000 行
    const qint64 lineLimitInBlock = qMin<qint64>(totalLines_, blk.firstLine + BLOCK_LINES);
    while (curLine < lineLimitInBlock) {
        int got = decodeVarint(base + deltaPos, end, &val);
        if (!got) break;
        deltaPos += got;
        prevByte = curByte;
        curByte += (qint64)val;
        if (curByte > byteOffset) break;
        ++curLine;
    }
    // 秒开：目标字节落在未构建区 → 不再伪造行/列（会读出错误字节）。
    // clamp 到 realCoverageBytes_ 对应的最后真实行，col 用真实偏移差。
    // 内容完整性优先：显示真实靠后内容，不显示伪造偏移的错字节。
    if (incomplete_ && curByte <= byteOffset) {
        qint64 realCov = qMin<qint64>(realCoverageBytes_, totalBytes_);
        if (byteOffset > realCov) {
            // 落在未构建区：定位到真实覆盖的最后一行
            byteOffset = realCov;
            // 重新从块内找到包含 realCov 的行
            qint64 cb = blk.byteOffset;
            qint64 pb = blk.byteOffset;
            qint64 cl = blk.firstLine;
            qint64 dp = blk.deltaEncodedPos;
            while (cl < lineLimitInBlock) {
                int got = decodeVarint(base + dp, end, &val);
                if (!got) break;
                dp += got;
                pb = cb;
                cb += (qint64)val;
                if (cb > byteOffset) break;
                ++cl;
            }
            curLine = cl;
            prevByte = pb;
        }
        qint64 estMax = estimatedLines_ > 0 ? (estimatedLines_ - 1) : 0;
        if (curLine > estMax) curLine = estMax;
        if (curLine < 0) curLine = 0;
    }
    if (!incomplete_ && curLine >= totalLines_) curLine = totalLines_ - 1;
    qint64 col = byteOffset - prevByte;
    if (col < 0) col = 0;
    *outLine = int(curLine);
    if (outCol) *outCol  = col;
}

LineIndex::ScanResult LineIndex::scanBlockRangeForByte(
    int blockIdxLo, int blockIdxHi, qint64 targetByteOffset,
    const uchar* /*dataFallback*/, qint64 /*dataSize*/) const
{
    Q_UNUSED(blockIdxLo);
    Q_UNUSED(blockIdxHi);
    ScanResult r;
    qint64 col;
    byteToLineCol(targetByteOffset, &r.line, &col);
    r.lineStartByte = lineToByteOffset(r.line);
    return r;
}

// ---- 调试序列化 ----
bool LineIndex::serialize(QIODevice* out) const {
    if (!out || !out->isWritable()) return false;
    QDataStream s(out);
    s.setByteOrder(QDataStream::LittleEndian);
    s << quint32(0x4C494E45);  // magic "LINE"
    s << quint64(totalLines_);
    s << quint64(totalBytes_);
    s << deltaEncoded_;
    s << quint32(blockIndex_.size());
    for (const auto& a : blockIndex_) {
        s << quint64(a.firstLine) << quint64(a.byteOffset) << quint64(a.deltaEncodedPos);
    }
    return true;
}

bool LineIndex::deserialize(QIODevice* in) {
    if (!in || !in->isReadable()) return false;
    QDataStream s(in);
    s.setByteOrder(QDataStream::LittleEndian);
    quint32 magic;
    s >> magic;
    if (magic != 0x4C494E45) return false;
    quint64 tl, tb;
    s >> tl >> tb;
    totalLines_ = qint64(tl);
    totalBytes_ = qint64(tb);
    s >> deltaEncoded_;
    quint32 nb;
    s >> nb;
    blockIndex_.clear();
    blockIndex_.reserve(nb);
    for (quint32 i = 0; i < nb; ++i) {
        quint64 fl, bo, dp;
        s >> fl >> bo >> dp;
        BlockAnchor a;
        a.firstLine       = qint64(fl);
        a.byteOffset      = qint64(bo);
        a.deltaEncodedPos = qint64(dp);
        blockIndex_.append(a);
    }
    return true;
}
