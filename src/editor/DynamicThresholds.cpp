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

    // L1/L2 切换门槛：固定 2GB
    // ≤ 2GB 直接走 Scintilla (L1 全量加载)；> 2GB 走 L2 mmap 秒开。
    t.L1_EDIT_LIMIT   = 2LL * 1024 * 1024 * 1024;

    // Patch L1：RAM/20，封顶 2GB
    t.PATCH_L1_LIMIT  = clampInt64(ramBytes / 20,
                                   64LL * 1024 * 1024,
                                   2LL * 1024 * 1024 * 1024);
    t.PATCH_SOFT_WARN = qint64(t.PATCH_L1_LIMIT * 0.8);

    // findAll 内存结果上限：(RAM/16) / 50B 每条
    qint64 perMatch = 50;
    t.MAX_MATCHES_MEM = clampInt64((ramBytes / 16) / perMatch,
                                   (qint64)100000,                  // 下限 10 万条
                                   (qint64)400000000);             // 顶 4 亿条（实际极少到）

    t.REPLACE_BATCH_SIZE = 1000;

    // Paste：RAM/32，封顶 200MB
    t.PASTE_LIMIT      = clampInt64(ramBytes / 32,
                                    10LL * 1024 * 1024,
                                    200LL * 1024 * 1024);

    t.SEARCH_THREADS   = qBound(1, cpuCores, 8);
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
