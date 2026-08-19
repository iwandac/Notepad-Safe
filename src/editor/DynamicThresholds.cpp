#include "DynamicThresholds.h"

#include <QThread>

#ifdef Q_OS_WIN
#  include <Windows.h>
#endif

namespace {

qint64 clampInt64(qint64 v, qint64 lo, qint64 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

DynamicThresholds computeFrom(qint64 ramBytes, int cpuCores) {
    if (ramBytes <= 0) ramBytes = 1LL * 1024 * 1024 * 1024;     // 保底 1GB
    if (cpuCores <= 0) cpuCores = 1;

    DynamicThresholds t;
    t.RAM_BYTES = ramBytes;
    t.CPU_CORES = cpuCores;

    // Patch L1：RAM/20，封顶 2GB
    t.PATCH_L1_LIMIT  = clampInt64(ramBytes / 20,
                                   64LL * 1024 * 1024,
                                   2LL * 1024 * 1024 * 1024);
    t.PATCH_SOFT_WARN = qint64(t.PATCH_L1_LIMIT * 0.8);

    // A match owns bounded context plus Qt container metadata; the previous
    // 50-byte estimate ignored the QByteArray payload and could reserve tens of
    // gigabytes on a high-memory workstation. Keep the resident result prefix
    // inside an 8-32 MiB producer budget. The grouped Qt model owns another
    // bounded copy, keeping producer + presentation near the 64 MiB contract.
    const qint64 resultBudget = clampInt64(ramBytes / 128,
                                          8LL * 1024 * 1024,
                                          32LL * 1024 * 1024);
    constexpr qint64 conservativeBytesPerMatch = 640;
    t.MAX_MATCHES_MEM = clampInt64(resultBudget / conservativeBytesPerMatch,
                                   qint64(10000), qint64(50000));

    t.REPLACE_BATCH_SIZE = 1000;

    // Paste：RAM/32，封顶 200MB
    t.PASTE_LIMIT      = clampInt64(ramBytes / 32,
                                    10LL * 1024 * 1024,
                                    200LL * 1024 * 1024);

    t.SEARCH_THREADS   = qBound(1, cpuCores, 10);
    return t;
}

} // namespace

DynamicThresholds::DynamicThresholds() {
    // 轻量默认构造：不调用任何 Qt API / Win32 探测，避免在函数静态对象初始化
    // （instance()）中因 QsciScintilla / QApplication 状态未就绪而栈溢出。
    // 真正的探测逻辑放在 probeAndFill()，由 instance() 中「静态标志」延迟执行。
}

static DynamicThresholds probeOnce() {
    qint64 ramBytes = 0;
    int    cpuCores = 1;

#if defined(Q_OS_WIN)
    // 用 GlobalMemoryStatusEx 获取真实物理内存总量
    MEMORYSTATUSEX msx;
    msx.dwLength = sizeof(msx);
    if (::GlobalMemoryStatusEx(&msx)) {
        ramBytes = (qint64)msx.ullTotalPhys;
    }
#endif
    // 理想核心数这里直接读取 QThread::idealThreadCount；确保调用晚于 QCoreApplication 构造
    int qCores = QThread::idealThreadCount();
    if (qCores > 0) cpuCores = qCores;

    return computeFrom(ramBytes, cpuCores);
}

const DynamicThresholds& DynamicThresholds::instance() {
    static const DynamicThresholds s_inst = probeOnce();
    return s_inst;
}

DynamicThresholds DynamicThresholds::makeForTesting(qint64 ramBytes, int cpuCores) {
    return computeFrom(ramBytes, cpuCores);
}
