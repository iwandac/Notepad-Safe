#pragma once

#include <QtGlobal>

// ============================================================================
//  DynamicThresholds — 启动时探测物理 RAM + CPU 核心数，
//  集中管理 L1/L2 切换、Patch 上限、搜索线程、findAll 结果上限等阈值。
//
//  设计原则（DESIGN.md §5.1）：
//   1) 不设人为硬上限；32GB 机器自然比 8GB 机器跑得更远。
//   2) 任何场景下优先完成工作；压力到时提示 + 中间保存，不拒绝用户操作。
//
//  典型阈值（探测后动态计算）：
//          8GB RAM        16GB RAM       32GB RAM
//  L1_EDIT     2GB(固定)       2GB(固定)       2GB(固定)
//  PATCH_L1   400MB            800MB           2GB (顶)
//  MAX_MATCH  10M条           21M条          42M条
//  SEARCH_THR   4核              8核             8核 (顶)
//  PASTE       250MB           250MB           250MB? 不，按公式
// ============================================================================

struct DynamicThresholds {
    qint64 RAM_BYTES = 0;       // 物理内存总量（字节）
    int    CPU_CORES = 1;       // 逻辑 CPU 核心数

    // L1 完整编辑（QsciScintilla） vs L2 虚拟视图 切换阈值（字节）
    // 固定 2GB：≤ 2GB 走 L1 全量加载，> 2GB 走 L2 mmap 秒开
    qint64 L1_EDIT_LIMIT = 0;

    // Patch L1 热内存上限；达到即 spill 25% 到 l2Cold_
    // = min(RAM_BYTES/20, 2GB)
    qint64 PATCH_L1_LIMIT = 0;

    // 达到 PATCH_L1_LIMIT 的 80% 时非阻塞提示用户可中间保存
    qint64 PATCH_SOFT_WARN = 0;

    // findAll 结果在内存中保留的上限（超出则序列化到溢出文件，Dock 虚拟化读取）
    // 每条结果约 50B → (RAM_BYTES/16) / 50
    qint64 MAX_MATCHES_MEM = 0;

    // replaceAll 每个 UndoGroup 的替换批大小（一个 Ctrl+Z 撤销单位）
    int    REPLACE_BATCH_SIZE = 1000;

    // 单次粘贴大小上限（超过需要中间保存提示）= min(RAM_BYTES/32, 200MB)
    qint64 PASTE_LIMIT = 0;

    // 大文件多线程搜索线程数 = min(CPU_CORES, 8)
    int    SEARCH_THREADS = 1;

    // 每块扫描字节 = 16MB（4KB overlap 边界去重）
    qint64 SEARCH_CHUNK_BYTES = 16LL * 1024 * 1024;

    // ---- 构造 & 探测 ----
    DynamicThresholds();

    // 全局单例（启动时构造一次，全程序共享）
    static const DynamicThresholds& instance();

    // 手动构造（测试时覆盖探测结果）
    static DynamicThresholds makeForTesting(qint64 ramBytes, int cpuCores);
};
