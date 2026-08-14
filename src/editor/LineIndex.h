#pragma once

#include <QtGlobal>
#include <QVector>
#include <QByteArray>
#include <functional>
#include <atomic>

class QIODevice;

// ============================================================================
//  LineIndex — 大文件（mmap）的行偏移索引。
//
//  内存目标（§5.3）：~2 字节/行 → 1GB 约 1000 万行 ≈ 20MB。
//  压缩格式：相邻行偏移差值（delta）做 base-128 varint 编码，
//           每 BLOCK_LINES（默认 1000）行保存一个绝对偏移锚点，支持 O(log n) 二分查找。
//
//  API 契约：
//    · buildFromMmap() 可后台线程调用；每 progressStep() 行发一次进度回调，
//      cancel 置 true 时尽快退出返回 false。
//    · byteToLineCol() / lineToByteOffset() 只读，可多线程并发调用（无锁）。
// ============================================================================

class LineIndex {
public:
    static constexpr int BLOCK_LINES = 1000;

    using FnProgress = std::function<void(qint64 linesDone, qint64 bytesDone)>;

    LineIndex() = default;

    // 从 mmap 字节区间构建行索引。行分隔符识别 '\n'。
    //   - 不以 '\n' 结尾的最后一段视作一行
    //   - progress 回调：每 progressStep 字节调用一次（可 0，使用内部默认约 16MB）
    //   - cancel：外部设置为 true 时中断构建，返回 false
    bool buildFromMmap(const uchar* data, qint64 size,
                       FnProgress progress = {},
                       std::atomic<bool>* cancel = nullptr,
                       qint64 progressStepBytes = 0);

    // 字节偏移 → (行号 0-based, 列号 0-based)
    // byteOffset 超出范围时，clamp 到最后一行的末尾
    // 注意：col 是「行内字节偏移」，二进制文件单行可达 GB 级，必须用 qint64，
    //       旧的 int 输出在长行时溢出为负 → 光标/渲染错乱（P0 截断根因之一）。
    void byteToLineCol(qint64 byteOffset, int* line, qint64* col) const;

    // 0-based 行号 → 行首字节偏移。
    // line 超范围返回文件末尾字节偏移（即 size）
    qint64 lineToByteOffset(int line) const;

    qint64 totalLines() const;
    qint64 totalBytes() const { return totalBytes_; }
    bool   isIncomplete() const { return incomplete_; }
    // 真实最长行的字节数（含行分隔符）。用于水平滚动条范围计算。
    // incomplete 状态下为扫描范围内的最长行（保守值）。
    qint64 maxLineBytes() const { return maxLineBytes_; }

    // 当前已有真实锚点覆盖到的最大字节偏移（不含估算部分）。
    // incomplete 状态下 < totalBytes_；构建完成后 == totalBytes_。
    // 用于 lineToByteOffset 在 incomplete 时 clamp，避免用估算偏移读出「错误字节」
    // （P0 内容完整性：宁可显示真实但靠后的内容，也不显示伪造偏移处的错字节）。
    qint64 realCoverageBytes() const { return realCoverageBytes_; }

    // 秒开半初始化：仅 set 总字节，放 1 个空锚点，totalLines 用估算值，
    // incomplete_=true，后续等后台构建完成后 swap 进来覆盖。
    // estAvgBytesPerLine 默认 80，估算总条数；与真实不符时滚动条会在 swap 后轻微跳动（可接受）。
    void setIncompleteWithFileSize(qint64 fileSize, qint64 estAvgBytesPerLine = 80);

    // 秒开首屏快速锚：对 data[0..min(size, 256KB)] 扫描换行，
    // 填充真实 block anchors/varint，保证首屏 paint 显示真实行内容。
    void quickBuildFirstRange(const uchar* data, qint64 size, qint64 maxScanBytes = 256 * 1024);

    // 后台 build 完成后主线程调用：把 completed 的所有字段 swap 进来，
    // incomplete_ 置 false；completed 对象自身进入清后丢弃状态。
    void setCompletedAndSwapIn(LineIndex&& completed);

    // 清空
    void clear();

    // ---------- 辅助（用于测试 & 调试）----------
    int blockCount() const { return blockIndex_.size(); }
    // 把 blockIndex 和 varint 编码写进 QIODevice（调试 dump）
    bool serialize(QIODevice* out) const;
    bool deserialize(QIODevice* in);

private:
    // block 锚点：绝对字节偏移 + 该行号 + deltaEncoded 字节下标
    // deltaEncodedPos 加速 lineToByteOffset/byteToLineCol：O(log blocks) 二分 + 块内最多 1000 次 varint 解码
    struct BlockAnchor { qint64 firstLine; qint64 byteOffset; qint64 deltaEncodedPos; };

    QByteArray            deltaEncoded_;  // varint 编码的行偏移差值序列
    QVector<BlockAnchor>  blockIndex_;    // 每 BLOCK_LINES 行一个锚点（加速二分）
    qint64                totalLines_ = 0;
    qint64                totalBytes_ = 0;
    qint64                maxLineBytes_ = 0;      // 真实最长行字节数（含分隔符），用于水平滚动范围
    qint64                realCoverageBytes_ = 0; // 真实锚点覆盖到的最大字节偏移
    bool                  incomplete_ = false;   // 秒开标记：true=后台仍在构建，totalLines 用估计值
    qint64                estimatedLines_ = 0;   // incomplete=true 时的行数估计
    qint64                estLineBytes_ = 80;    // incomplete=true 时每行平均字节估计（>=1）

    // 在 [blockIdxLo, blockIdxHi) 的块范围内，对 varint 编码做线性扫描
    // 找到包含 targetByteOffset 的那一行；返回 行号 & 行首偏移
    struct ScanResult { int line; qint64 lineStartByte; };
    ScanResult scanBlockRangeForByte(int blockIdxLo, int blockIdxHi,
                                     qint64 targetByteOffset,
                                     const uchar* dataFallback = nullptr,
                                     qint64 dataSize = 0) const;

    // 编码/解码 varint
    static int  encodeVarint(QByteArray& out, quint64 value);
    // 返回解析消耗的字节数；失败返回 0
    static int  decodeVarint(const uchar* p, const uchar* end, quint64* outValue);
};
