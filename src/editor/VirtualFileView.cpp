#include "VirtualFileView.h"
#include "SharedReadOnlyFile.h"
#include "LargeFileSearch.h"
#include "DynamicThresholds.h"
#include "AsyncTaskManager.h"

#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QApplication>
#include <QThread>
#include <QScrollBar>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QFutureWatcher>
#include <QPointer>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>

#include <cstring>
#include <algorithm>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
#include <QStringDecoder>

namespace {

constexpr qint64  LINEBUILD_PROGRESS_STEP = qint64(16) * 1024 * 1024;

QFont pickMonospaceFont() {
    QStringList candidates{ QStringLiteral("Consolas"),
                            QStringLiteral("Courier New") };
    for (const auto& name : candidates) {
        QFont f(name);
        f.setStyleHint(QFont::TypeWriter);
        QFontInfo fi(f);
        if (fi.family().compare(name, Qt::CaseInsensitive) == 0) {
            f.setPointSize(10);
            return f;
        }
    }
    QFont f;
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(10);
    return f;
}

qint64 estimateLongestLineBytes(const LineIndex& idx, qint64 fileSize) {
    // 使用 LineIndex 构建时记录的真实最长行字节数。
    // 旧实现 avg*20 严重低估长行；中间实现 INT_MAX/4 导致 maxLineChars_*cw int 溢出。
    // 现在 LineIndex::buildFromMmap 记录真实 maxLineBytes_，这里直接取用。
    qint64 m = idx.maxLineBytes();
    if (m < 1) m = qMax<qint64>(1, qMin(fileSize, qint64(80)));  // 兜底
    return m;
}

// UTF-8 解码：从 src[pos] 开始解码一个 Unicode 码点。
// 返回消耗的字节数（1-4），codepoint 写入 *cp。
// 无效/不完整序列返回 1 且 *cp = 0xFFFD（替换字符）。
int decodeUtf8Char(const uchar* src, int len, int pos, uint* cp) {
    if (pos >= len) { *cp = 0; return 0; }
    uchar c = src[pos];
    if (c < 0x80) { *cp = c; return 1; }
    int nb; uint val;
    if      ((c & 0xE0) == 0xC0) { nb = 2; val = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { nb = 3; val = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { nb = 4; val = c & 0x07; }
    else { *cp = 0xFFFD; return 1; } // 无效首字节
    if (pos + nb > len) { *cp = 0xFFFD; return 1; } // 不完整序列
    for (int j = 1; j < nb; ++j) {
        if ((src[pos + j] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        val = (val << 6) | (src[pos + j] & 0x3F);
    }
    *cp = val;
    return nb;
}

} // namespace

// ============================================================================
//  构造/析构
// ============================================================================
VirtualFileView::VirtualFileView(QWidget* parent)
    : QAbstractScrollArea(parent)
    , fm_(font_)
{
    font_ = pickMonospaceFont();
    setFont(font_);
    fm_ = QFontMetrics(font_);
    setMouseTracking(false);  // VFV 不做任何绘制/鼠标交互；Scintilla overlay 是唯一 UI
    setFocusPolicy(Qt::NoFocus);  // 焦点永远在 overlay Scintilla
    // VFV 不处理 IME；Scintilla 是唯一输入。避免 inputMethodEvent 路由到这里
    setAttribute(Qt::WA_InputMethodEnabled, false);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    viewport()->setAttribute(Qt::WA_DontShowOnScreen, false);  // 仍然要可见（作为 Scintilla 父容器）
    verticalScrollBar()->setSingleStep(3);
    horizontalScrollBar()->setSingleStep(4 * qMax(1, charWidth()));
    updateMetrics();
    initScrollBars();

    // ---- 小 chunk 滚动 3 档节流：connect + 初始化成员 ----
    m_scrollDebounceTimer = new QTimer(this);
    m_scrollDebounceTimer->setInterval(120);         // Qt 官方推荐 100~250ms debounce
    m_scrollDebounceTimer->setSingleShot(true);
    m_prefetchWatcher = new QFutureWatcher<QVector<ChunkSlot>>(this);
    connectScrollBarThrottling();
    connect(m_scrollDebounceTimer, &QTimer::timeout, this, &VirtualFileView::onScrollDebouncedL2);
    connect(m_prefetchWatcher, &QFutureWatcher<QVector<ChunkSlot>>::finished,
            this, &VirtualFileView::onPrefetchFinished);
    connect(m_prefetchWatcher, &QFutureWatcher<QVector<ChunkSlot>>::canceled,
            this, &VirtualFileView::onPrefetchFinished);
}

VirtualFileView::~VirtualFileView() {
    // 崩溃修复：DTOR 开始立刻置标志，closeFile 内部自动跳过所有 UI/emit 副作用
    m_inDestruction = true;

    // 析构前先 cancel 正在运行的 prefetch / debounce
    if (m_scrollDebounceTimer) m_scrollDebounceTimer->stop();
    if (m_prefetchWatcher) {
        if (m_prefetchWatcher->isRunning()) m_prefetchWatcher->cancel();
        m_prefetchWatcher->waitForFinished();
    }
    if (m_prefetchTaskId >= 0) {
        AsyncTaskManager::instance().cancel(m_prefetchTaskId);
    }

    // ⚠️  析构上下文：allowProcessEvents=false，绝对不能 processEvents！
    closeFile(/*allowProcessEvents=*/false);

    if (search_) { delete search_; search_ = nullptr; }
    if (navigationSearch_) { delete navigationSearch_; navigationSearch_ = nullptr; }
}

// ============================================================================
//  打开/关闭文件
// ============================================================================
bool VirtualFileView::openFile(const QString& filePath, QString* errMsg)
{
    closeFile();
    QString openError;
    if (!openSharedReadOnlyFile(file_, QFileInfo(filePath).absoluteFilePath(), &openError)) {
        if (errMsg) *errMsg = QObject::tr("无法打开文件：%1").arg(openError);
        return false;
    }
    qint64 size = file_.size();
    if (size < 0) { if (errMsg) *errMsg = QObject::tr("文件大小无效"); return false; }
    if (size == 0) {
        mmapSize_ = 0;
        lineIndex_.clear();
        bookmarks_.clear();
        cursorByte_ = 0;
        selStartByte_ = selEndByte_ = 0;
        patches_.resetAfterSave();
        lastL2Count_ = 0;
        if (search_) search_->setSource(nullptr, 0, &lineIndex_, &patches_);
        if (navigationSearch_)
            navigationSearch_->setSource(nullptr, 0, &lineIndex_, &patches_);
        updateMetrics();
        updateScrollBarsFromContent();
        viewport()->update();
        emit fileOpenedChanged(true);
        emit saveNeededChanged(false);
        emit lineBuildProgress(0, 0);
        emit lineBuildFinished(true);
        return true;
    }

    // ---- 异步 mmap：不在 UI 线程做 file_.map() + quickBuildFirstRange ----
    // 首次打开大文件时，file_.map() 可能触发杀软同步扫描 + 缺页 IO，
    // quickBuildFirstRange 扫前 1MB 触发首次缺页。这些操作移到 worker 线程，
    // UI 线程立即返回，viewport 显示"正在加载..."。
    m_loading = true;
    mmapSize_ = size;  // 记录文件大小用于 loading 显示和后续 quickBuild
    viewport()->update();  // 立即显示 loading 状态
    emit fileOpenedChanged(true);
    emit lineBuildProgress(0, size);

    QPointer<VirtualFileView> that(this);
    QString pathCopy = filePath;

    // Mapping is source preparation, not a line-index task. Binary/Hex paths
    // must not appear to users as if they are interpreting arbitrary bytes as
    // a text line model.
    const bool buildQuickIndex = m_fullLineIndexBuildEnabled
        && m_viewMode == ViewMode::Text;
    m_mmapTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::Other,
        AsyncTaskPriority::High,
        QObject::tr("映射文件: %1").arg(QFileInfo(filePath).fileName()),
        QObject::tr("正在将文件映射到内存..."),
        [that, size, pathCopy, buildQuickIndex](qint64, AsyncCancelPtr cancel,
                               std::function<void(qint64,qint64)> reportProgress,
                               std::function<void(const AsyncErrorInfo&)> reportError)
        {
            if (!that) return;
            // 1) mmap — 可能在首次打开时阻塞（杀软扫描 + 缺页 IO）
            if (cancel && cancel->load(std::memory_order_acquire)) return;
            uchar* p = that->file_.map(0, size);
            if (!p) {
                AsyncErrorInfo e;
                e.message = QObject::tr("无法映射文件到内存（QFile::map 失败）。");
                reportError(e);
                return;
            }
            // 2) Trusted Text may build one bounded first-screen anchor.
            // Binary/Hex skips this scan entirely; source bytes alone are the
            // authority and switching presentation cannot justify a global
            // line model.
            if (cancel && cancel->load(std::memory_order_acquire)) {
                that->file_.unmap(p);
                return;
            }
            LineIndex quickIndex;
            quickIndex.setIncompleteWithFileSize(size);
            if (buildQuickIndex)
                quickIndex.quickBuildFirstRange(p, size, 1024 * 1024);
            if (reportProgress)
                reportProgress(buildQuickIndex
                                   ? qMin<qint64>(size, 1024 * 1024)
                                   : size,
                               size);

            // 3) 通知 UI 线程：mmap + quickBuild 完成
            QMetaObject::invokeMethod(that, "onMmapReady", Qt::QueuedConnection,
                                      Q_ARG(quintptr, reinterpret_cast<quintptr>(p)),
                                      Q_ARG(qint64, size),
                                      Q_ARG(LineIndex, quickIndex),
                                      Q_ARG(bool, buildQuickIndex));
        },
        0, QLatin1String("Mmap"), size
    );

    return true;
}

bool VirtualFileView::openMemory(QByteArray bytes, ViewMode mode, QString* errMsg)
{
    closeFile();
    if (mode != ViewMode::Hex) {
        if (errMsg)
            *errMsg = QObject::tr("内存安全载荷当前只支持十六进制视图。");
        if (!bytes.isEmpty())
            bytes.fill(0);
        return false;
    }

    m_memoryBacking = std::move(bytes);
    m_memoryBacked = true;
    patches_.setSensitive(true);
    mmapSize_ = m_memoryBacking.size();
    mmapBase_ = m_memoryBacking.isEmpty()
                    ? nullptr
                    : reinterpret_cast<uchar *>(m_memoryBacking.data());
    m_viewMode = mode;
    m_loading = false;
    lineIndex_.clear();
    bookmarks_.clear();
    cursorByte_ = 0;
    selStartByte_ = selEndByte_ = 0;
    patches_.resetAfterSave();
    lastL2Count_ = 0;
    if (search_)
        search_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
    if (navigationSearch_)
        navigationSearch_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);

    m_activeWindowStart = 0;
    m_activeWindowLen = 0;
    m_lastComputedW = computeActiveWindowBytes();
    lruClear();
    m_previewJumpByteHint = -1;
    (void)alignActiveWindowTo(0, m_lastComputedW);
    updateMetrics();
    updateScrollBarsFromContent();
    viewport()->update();
    emit fileOpenedChanged(true);
    emit saveNeededChanged(false);
    emit lineBuildProgress(mmapSize_, mmapSize_);
    emit lineBuildFinished(true);
    if (!m_inDestruction && m_activeWindowLen > 0) {
        emit activeWindowChanged(m_activeWindowStart,
                                 m_activeWindowStart + m_activeWindowLen);
    }
    return true;
}

void VirtualFileView::onMmapReady(quintptr ptr, qint64 size,
                                  LineIndex quickIndex, bool quickIndexBuilt) {
    m_loading = false;
    m_mmapTaskId = -1;

    uchar* p = reinterpret_cast<uchar*>(ptr);
    if (!p) {
        emit lineBuildFinished(false);
        return;
    }

    mmapBase_ = p;
    mmapSize_ = size;
    lineIndex_ = quickIndex;

    bookmarks_.clear();
    m_buildProgressBytes = quickIndexBuilt
        ? qMin<qint64>(size, 1024 * 1024) : 0;
    cursorByte_ = 0;
    selStartByte_ = selEndByte_ = 0;
    patches_.resetAfterSave();
    lastL2Count_ = 0;
    if (search_) search_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
    if (navigationSearch_)
        navigationSearch_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);

    // ---- 小 chunk overlay：复位 activeWindow ----
    m_activeWindowStart = 0;
    m_activeWindowLen   = 0;
    m_lastComputedW     = 0;
    lruClear();
    m_previewJumpByteHint = -1;

    updateMetrics();
    updateScrollBarsFromContent();

    // ---- 初始化 W_active + align 到文件起点（cursor=0 中心），同步一次到 EditorTab overlay ----
    {
        const qint64 w = computeActiveWindowBytes();
        m_lastComputedW = w;
        (void)alignActiveWindowTo(0, w);
    }

    // ---- 吞吐采样（worker 线程）：只对真正的大文件采样。小二进制文件
    // 也会使用 mmap/Hex，但为几百字节的文件创建后台任务只会制造 UI 噪声。 ----
    {
        const qint64 sampleBytes = qMin<qint64>(Q_INT64_C(64) * 1024 * 1024, size);
        if (size >= Q_INT64_C(32) * 1024 * 1024
            && sampleBytes > 0 && mmapBase_ != nullptr) {
            QPointer<VirtualFileView> that(this);
            const uchar* basePtr = mmapBase_;
            AsyncTaskManager::instance().submit(
                AsyncTaskType::Other,
                AsyncTaskPriority::Low,
                QObject::tr("采样吞吐"),
                QObject::tr("测量磁盘读取速度..."),
                [that, basePtr, sampleBytes, size](qint64, AsyncCancelPtr cancel,
                    std::function<void(qint64,qint64)>,
                    std::function<void(const AsyncErrorInfo&)>)
                {
                    if (!that) return;
                    if (cancel && cancel->load(std::memory_order_acquire)) return;
                    // 读一次 64MB（逐字节 XOR 防编译器优化掉），QElapsedTimer 计时
                    QElapsedTimer t; t.start();
                    quint8 acc = 0;
                    for (qint64 i = 0; i < sampleBytes; i += qint64(4096)) {
                        acc = quint8(acc ^ quint8(basePtr[i]));
                    }
                    qint64 ms = qMax<qint64>(1, t.elapsed());
                    qint64 bps = sampleBytes * qint64(1000) / ms;
                    Q_UNUSED(acc);
                    // 跨线程安全：通过 queued invokeMethod 写回（防止 this 被 destroy）
                    QMetaObject::invokeMethod(that, "setSampledThroughputBytesPerSec",
                        Qt::QueuedConnection, Q_ARG(qint64, bps));
                    // 之后再按新吞吐重算 W_active + 若窗口变了再 activeWindowChanged
                    QMetaObject::invokeMethod(that, [that, size]() {
                        if (!that) return;
                        if (!that->isOpen()) return;
                        const qint64 wPrev = that->m_lastComputedW;
                        const qint64 wNew = that->computeActiveWindowBytes();
                        if (wNew != wPrev) {
                            that->m_lastComputedW = wNew;
                            that->lruClear();  // chunk size 变了旧槽无意义
                            if (that->alignActiveWindowTo(that->cursorByte_, wNew)) {
                                if (!that->m_inDestruction) {
                                    emit that->activeWindowChanged(
                                        that->m_activeWindowStart,
                                        that->m_activeWindowStart + that->m_activeWindowLen);
                                }
                            }
                        }
                    }, Qt::QueuedConnection);
                },
                0, QLatin1String("ThroughputSample"), sampleBytes
            );
        }
    }

    viewport()->update();
    emit fileOpenedChanged(true);
    emit saveNeededChanged(false);
    emit lineBuildProgress(qMin<qint64>(Q_INT64_C(1024) * 1024, mmapSize_), mmapSize_);

    // ---- L3（落点）对齐一次：若 activeWindow 变了，发信号让 EditorTab 灌 overlay Scintilla ----
    if (!m_inDestruction && m_activeWindowLen > 0) {
        emit activeWindowChanged(m_activeWindowStart, m_activeWindowStart + m_activeWindowLen);
    }

    // Hex 按固定 16 字节分行，不依赖文本 LineIndex。到这里 mmap 已经可用，
    // 直接结束加载状态，避免创建无价值的后台索引任务。
    if (m_viewMode == ViewMode::Hex || !m_fullLineIndexBuildEnabled) {
        emit lineBuildProgress(0, 0);
        emit lineBuildFinished(true);
        return;
    }

    // 后台全量构建（不阻塞 UI，完成后 swap 进真实 anchors）
    startAsyncLineBuild();
}

bool VirtualFileView::remapFile(QString* errMsg) {
    if (m_memoryBacked) {
        if (errMsg)
            *errMsg = QObject::tr("内存安全载荷不能通过磁盘 remap。");
        return false;
    }
    if (!isOpen() && file_.fileName().isEmpty()) {
        if (errMsg) *errMsg = QObject::tr("remapFile 前请先 openFile 或保存过一次文件");
        return false;
    }
    QString path = file_.fileName();
    closeFile();
    return openFile(path, errMsg);
}

void VirtualFileView::closeFile(bool allowProcessEvents /*= true*/) {
    const bool closingMemoryBacked = m_memoryBacked;
    // processEvents below may deliver a queued result from the line-index task
    // being canceled. That result belongs to the old mapping and must not
    // re-enter task progress updates while closeFile waits for termination.
    m_closingFile = true;
    ++m_mappingGeneration;

    // LargeFileSearch borrows mmapBase_, lineIndex_ and patches_. Its worker
    // may still be queued when cancellation is requested, so cross its own
    // completion barrier before any of those objects are reset or unmapped.
    if (search_)
        search_->cancelAndWait();
    if (navigationSearch_)
        navigationSearch_->cancelAndWait();
    // ========================================================================
    //  取消异步 mmap 任务（如果正在加载中）
    // ========================================================================
    if (m_mmapTaskId > 0) {
        AsyncTaskManager::instance().cancel(m_mmapTaskId);
        m_mmapTaskId = -1;
    }
    m_loading = false;

    // ========================================================================
    //  关键修复（WM_CLOSE 0xC0000005 崩溃根因）：
    //   后台 LineIndex 构建由 AsyncTaskManager worker 持有。
    //   如果 cancel 之后直接 unmap mmapBase_，worker 线程仍在
    //   buildLineIndexOnWorker(mmapBase_, ...) 读内存 → 访问违例 0xC0000005。
    //   所以必须：1) cancel  2) 忙等 worker 真正进入终态 (Finished/Canceled/Failed)
    //   3) 之后才能 unmap / 关闭文件。
    // ========================================================================
    if (m_lineBuildTaskId > 0) {
        // 先通过 Manager 设置取消（线程安全的内部状态切换 + cancel 原子置 true）
        AsyncTaskManager::instance().cancel(m_lineBuildTaskId);

        // cancel 原子也要再置一次（双重保险：worker 可能刚读完 task.cancel 但还没开始工作）
        if (lineBuildCancel_) lineBuildCancel_->store(true, std::memory_order_release);

        if (allowProcessEvents) {
            // Wait for the worker's own completion flag, not the manager's UI
            // state. The latter is finalized by a queued callback; pumping the
            // event loop here can re-enter the old result callback and deadlock
            // during Save As/backing-store switches.
            QElapsedTimer wt; wt.start();
            constexpr int MAX_WAIT_MS = 10000;
            const auto done = m_lineBuildWorkerDone;
            while (done && !done->load(std::memory_order_acquire)
                   && wt.elapsed() < MAX_WAIT_MS) {
                QThread::msleep(1);
            }
            const bool terminated = !done || done->load(std::memory_order_acquire);
            if (!terminated) {
                qWarning("[VirtualFileView::closeFile] LineIndex worker #%lld "
                         "cancel 后 %d ms 仍未退出；拒绝继续派发旧映射回调。",
                         m_lineBuildTaskId, MAX_WAIT_MS);
            } else {
                qDebug("[VirtualFileView::closeFile] LineIndex worker #%lld "
                       "已在 %lld ms 内安全终止，可以安全 unmap。",
                       m_lineBuildTaskId, wt.elapsed());
            }
        } else {
            // ================================================================
            //  析构场景：禁止 processEvents！
            //    此时对象正处于析构阶段，processEvents 会派发指向本对象的
            //    QueuedConnection（onAsyncLineBuildSwapped）导致 use-after-free 崩溃。
            //    保守等待：最多 500ms（LineIndex cancel 检查粒度 16MB，100MB 文件最坏
            //    只需要处理 16MB 就退出——约 10~30ms，500ms 有 16 倍余量绝对够）。
            //    这里不查 task.state（state 依赖 finishTaskImpl 主线程 QueuedConnection
            //    才能更新，我们不 processEvents 就永远看不到）；但 cancel 原子是跨线程
            //    可见的，worker 线程自己检测后就会退出。
            // ================================================================
            constexpr int DESTRUCT_WAIT_MS = 500;
            QElapsedTimer wt; wt.start();
            // 实际 sleep 时间：至少给 worker 一个时间片从用户态调度到内核态并检查 cancel。
            while (wt.elapsed() < DESTRUCT_WAIT_MS) {
                QThread::msleep(1);
                // 1ms * 500 次 = 500ms 上限；实际通常 10~30ms worker 就退出了，
                // 但我们没有跨线程「done 原子」可查（要改 startAsyncLineBuild 成本高），
                // 所以直接等满余量。500ms 对「关闭软件」场景是可接受的。
            }
            qDebug("[VirtualFileView::closeFile(dtor-safe)] AsyncTaskManager LineIndex task #%lld "
                   "cancel 后保守等待 %d ms（析构上下文禁止 processEvents），可以安全 unmap。",
                   m_lineBuildTaskId, DESTRUCT_WAIT_MS);
        }
        m_lineBuildTaskId = -1;
        m_lineBuildWorkerDone.reset();
    }
    // The current AsyncTaskManager task owns the only line-index worker.
    if (lineBuildCancel_) lineBuildCancel_->store(true, std::memory_order_relaxed);

    // Prefetch workers also hold the raw mapping pointer. Cancel and wait for
    // every generation still in flight before unmapping or scrubbing memory.
    if (m_scrollDebounceTimer) m_scrollDebounceTimer->stop();
    if (m_prefetchWatcher && m_prefetchWatcher->isRunning())
        m_prefetchWatcher->cancel();
    if (m_prefetchTaskId >= 0) {
        AsyncTaskManager::instance().cancel(m_prefetchTaskId);
        m_prefetchTaskId = -1;
    }
    if (m_prefetchCancel)
        m_prefetchCancel->store(true, std::memory_order_release);
    if (!m_prefetchWorkers.isEmpty()) {
        QElapsedTimer wait;
        wait.start();
        const int maximumWaitMs = allowProcessEvents ? 10000 : 500;
        const auto allFinished = [this]() {
            return std::all_of(m_prefetchWorkers.cbegin(), m_prefetchWorkers.cend(),
                               [](const auto &done) {
                                   return !done || done->load(std::memory_order_acquire);
                               });
        };
        while (!allFinished() && wait.elapsed() < maximumWaitMs)
            QThread::msleep(1);
        m_prefetchWorkers.clear();
    }
    m_prefetchCancel.reset();

    if (mmapBase_ && !m_memoryBacked) {
        file_.unmap(mmapBase_);
        mmapBase_ = nullptr;
    }
    if (m_memoryBacked) {
        mmapBase_ = nullptr;
        if (!m_memoryBacking.isEmpty())
            m_memoryBacking.fill(0);
        m_memoryBacking.clear();
        m_memoryBacking.squeeze();
        m_memoryBacked = false;
    }
    mmapSize_ = 0;
    m_buildProgressBytes = 0;
    if (file_.isOpen()) file_.close();
    lineIndex_.clear();
    bookmarks_.clear();
    cursorByte_ = 0;
    selStartByte_ = selEndByte_ = 0;
    patches_.resetAfterSave();
    patches_.setSensitive(false);
    lastL2Count_ = 0;
    if (search_) search_->setSource(nullptr, 0, nullptr, nullptr);
    if (navigationSearch_)
        navigationSearch_->setSource(nullptr, 0, nullptr, nullptr);

    // ---- 关闭文件时一并清理小 chunk 状态 ----
    if (closingMemoryBacked) {
        for (ChunkSlot &slot : m_chunkLru) {
            if (!slot.bytes.isEmpty())
                slot.bytes.fill(0);
        }
    }
    lruClear();
    m_activeWindowStart = 0;
    m_activeWindowLen = 0;
    m_lastComputedW = 0;
    m_previewJumpByteHint = -1;

    // ========================================================================
    //  ⚠️  崩溃修复（0xC0000005 关键！）：析构阶段禁止任何「对外副作用」的调用
    //    包括：信号 emit、viewport()->update()、updateScrollBarsFromContent()。
    //    因为：
    //      · VirtualFileView 作为 EditorTab 的子对象，在 EditorTab 的 QWidget
    //        基类析构阶段被 delete；此时 EditorTab 子类部分已经析构完毕（vtable
    //        已不再是 EditorTab 的），但 connect 的信号 receiver 指针仍挂着
    //        EditorTab*。任何信号 emit 都会尝试 invoke EditorTab 上的 slot/lambda，
    //        访问半析构对象 → 0xC0000005。
    //      · viewport()/scrollbars 在 ~QWidget 中可能已开始被父级先清理，
    //        任何 update/paint 路径调用都不安全。
    //    只在非析构（正常 openFile/closeFile 切换）时做 UI/信号更新。
    // ========================================================================
    if (!m_inDestruction) {
        updateScrollBarsFromContent();
        viewport()->update();
        emit fileOpenedChanged(false);
        emit saveNeededChanged(false);
    }
    m_closingFile = false;
}

// ============================================================================
//  度量 & 滚动条
// ============================================================================
void VirtualFileView::updateMetrics() {
    fm_ = QFontMetrics(font_);
    lineHeight_ = fm_.height();
    charWidth_  = fm_.horizontalAdvance(QLatin1Char('0'));
    if (charWidth_ <= 0) charWidth_ = 8;
    if (lineHeight_ <= 0) lineHeight_ = 16;

    if (m_viewMode == ViewMode::Hex) {
        // 16 进制模式：地址列宽度 = 固定 8 位 hex + "  " 2 空格（与内容区之间的分隔）
        // 右侧：16*(2 hex + 1 空格) + 2 空格 + 16 ascii 字符（= 文本模式的 gutterWidth 语义）
        // gutterWidth_ 用于 overlay 地址区宽度：
        //   地址列宽度：8 hex charWidth + 2 (pad) + 14 (bookmark padding)
        gutterWidth_ = 10 * charWidth_ + 2 * 4 + 14;  // 8 hex + 2 space + pad
        maxLineChars_ = m_hexBytesPerRow * 4 + 14;
        // maxLineDigits_：hex 地址固定 8 位（最多 32 位 = 4GB，够用）；>4GB 时自动扩展
        qint64 lines = hexTotalLines();
        int digits = 1;
        for (qint64 n = 10; lines >= n; n *= 10) ++digits;
        maxLineDigits_ = qMax(digits, 8);
    } else {
        qint64 totalLines = qMax<qint64>(lineIndex_.totalLines(), 1);
        int digits = 1;
        for (qint64 n = 10; totalLines >= n; n *= 10) ++digits;
        maxLineDigits_ = qMax(digits, 1);
        gutterWidth_ = (maxLineDigits_ + 1) * charWidth_ + 2 * 4 + 14;
        maxLineChars_ = lineIndex_.totalLines() > 0
            ? int(estimateLongestLineBytes(lineIndex_, mmapSize_))
            : 80;
    }
}

int VirtualFileView::lineHeight() const { return lineHeight_; }
int VirtualFileView::charWidth()  const { return charWidth_;  }
int VirtualFileView::gutterWidth() const { return gutterWidth_; }
int VirtualFileView::firstVisibleLine() const { return verticalScrollBar()->value(); }
int VirtualFileView::visibleLineCount() const {
    int h = viewport()->height();
    int lh = qMax(lineHeight(), 1);
    return (h + lh - 1) / lh + 1;
}
int VirtualFileView::horizontalOffsetPx() const { return horizontalScrollBar()->value(); }

void VirtualFileView::initScrollBars() {
    verticalScrollBar()->setRange(0, 0);
    horizontalScrollBar()->setRange(0, 0);
}

void VirtualFileView::updateScrollBarsFromContent() {
    // ---- Hex 视图：按固定 16B/行计算，不依赖 LineIndex ----
    if (m_viewMode == ViewMode::Hex) {
        qint64 totalLines = hexTotalLines();
        if (totalLines <= 0) { verticalScrollBar()->setRange(0, 0); horizontalScrollBar()->setRange(0, 0); return; }
        int h = viewport()->height();
        int lh = qMax(lineHeight(), 1);
        int pageStep = qMax(1, h / lh);
        qint64 vmax = qMax<qint64>(0, totalLines - pageStep);
        verticalScrollBar()->setRange(0, int(vmax > INT_MAX ? INT_MAX : vmax));
        verticalScrollBar()->setPageStep(pageStep);

        // 每行固定宽度（字符数）：
        //   16*(2 hex + 1 space) = 48  +  中间 gutter 2  +  ASCII 16 = 66
        // 最宽还需考虑最后 8 + 9 字节分组（8 + 1 space + 7*3 + 1 = 31 hex 位，7+7 ascii）
        // 保守用 68 字符 × charWidth
        int vpW = qMax(1, viewport()->width() - gutterWidth());
        const int hexCharsPerLine = m_hexBytesPerRow * 3 + 2 + m_hexBytesPerRow;
        qint64 maxPx64 = qint64(hexCharsPerLine) * qint64(qMax(1, charWidth()));
        int hPageStep = qMax(1, vpW / qMax(1, charWidth()));
        qint64 hmax64 = maxPx64 - qint64(vpW);
        int hmax = (hmax64 > 0) ? int(qMin<qint64>(hmax64, INT_MAX)) : 0;
        horizontalScrollBar()->setRange(0, hmax);
        horizontalScrollBar()->setPageStep(hPageStep);
        return;
    }

    // ---- Text 视图：按 LineIndex ----
    qint64 totalLines = lineIndex_.totalLines();
    if (totalLines <= 0) { verticalScrollBar()->setRange(0, 0); horizontalScrollBar()->setRange(0, 0); return; }
    int h = viewport()->height();
    int lh = qMax(lineHeight(), 1);
    int pageStep = qMax(1, h / lh);
    qint64 vmax = qMax<qint64>(0, totalLines - pageStep);
    verticalScrollBar()->setRange(0, int(vmax > INT_MAX ? INT_MAX : vmax));
    verticalScrollBar()->setPageStep(pageStep);

    // 自动换行模式：禁用水平滚动条（内容折行显示）
    if (m_wordWrap) {
        horizontalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setPageStep(1);
        return;
    }

    // 水平滚动范围：用 qint64 计算避免 int 溢出，clamp 到 INT_MAX。
    // 旧实现 maxLineChars_*charWidth() 在长行时溢出成负数 → hmax=0 → 无法水平滚动。
    int vpW = qMax(1, viewport()->width() - gutterWidth());
    qint64 maxChars = estimateLongestLineBytes(lineIndex_, mmapSize_);
    qint64 maxPx64 = maxChars * qint64(qMax(1, charWidth()));
    int hPageStep = qMax(1, vpW / qMax(1, charWidth()));
    qint64 hmax64 = maxPx64 - qint64(vpW);
    int hmax = (hmax64 > 0) ? int(qMin<qint64>(hmax64, INT_MAX)) : 0;
    horizontalScrollBar()->setRange(0, hmax);
    horizontalScrollBar()->setPageStep(hPageStep);
}

void VirtualFileView::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    updateMetrics();
    updateScrollBarsFromContent();
}

void VirtualFileView::scrollContentsBy(int /*dx*/, int /*dy*/) {
    // 方案：VFV 不自绘，Scintilla overlay 盖满 viewport 是唯一 UI。
    // 默认 QAbstractScrollArea::scrollContentsBy 会 viewport()->scroll(dx,dy)
    // 移动所有子 widget（含 overlay Scintilla），导致它被甩出可视区。
    // 这里改为 no-op：滚动条变化由 3 档节流 → activeWindowChanged → 灌新窗口处理，
    // 不做像素级 scroll。
}

void VirtualFileView::wheelEvent(QWheelEvent* e) {
    // VFV 不做自绘；但 wheelEvent 仍保留为"用户在 VFV 滚动条区域或空 viewport（无 overlay）
    // 上滚轮时仍能驱动 VFV 全局滚动条走 3 档节流"。Scintilla overlay 上的 wheel 由 Scintilla
    // 自己处理，并通过桥 3 debounce 回写 VFV。
    QPoint angle = e->angleDelta();
    if (!angle.isNull()) {
        if (angle.y() != 0) {
            int steps = angle.y() / 120;
            verticalScrollBar()->setValue(qBound(
                verticalScrollBar()->minimum(),
                verticalScrollBar()->value() - steps,
                verticalScrollBar()->maximum()));
        } else if (angle.x() != 0) {
            int steps = angle.x() / 120;
            horizontalScrollBar()->setValue(qBound(
                horizontalScrollBar()->minimum(),
                horizontalScrollBar()->value() - steps * horizontalScrollBar()->singleStep(),
                horizontalScrollBar()->maximum()));
        }
    }
    e->accept();
}

// ============================================================================
//  显示编码 & 自动换行
// ============================================================================
void VirtualFileView::setDisplayEncoding(TextEncoding enc) {
    if (m_displayEncoding == enc) return;
    m_displayEncoding = enc;
    viewport()->update();
}

void VirtualFileView::setWordWrap(bool on) {
    if (m_wordWrap == on) return;
    m_wordWrap = on;
    updateScrollBarsFromContent();
    viewport()->update();
}

void VirtualFileView::setViewMode(ViewMode mode) {
    if (m_viewMode == mode) return;
    m_viewMode = mode;
    updateMetrics();
    updateScrollBarsFromContent();
    viewport()->update();
    emit viewModeChanged(m_viewMode);
    // Hex does not need a line index, so initial binary routing deliberately
    // skips it. Once the user asks for Text, start the index in the background;
    // otherwise the byte-progress fallback becomes permanent and cannot offer
    // stable Notepad-style line scrolling or absolute line numbers.
    if (m_viewMode == ViewMode::Text && m_fullLineIndexBuildEnabled
        && mmapBase_ && mmapSize_ > 0 && lineIndex_.isIncomplete()
        && m_lineBuildTaskId <= 0) {
        startAsyncLineBuild();
    }
}

void VirtualFileView::setHexBytesPerRow(int bytesPerRow) {
    bytesPerRow = qBound(8, bytesPerRow, 64);
    if (m_hexBytesPerRow == bytesPerRow)
        return;
    const qint64 anchor = cursorByte_;
    m_hexBytesPerRow = bytesPerRow;
    updateMetrics();
    updateScrollBarsFromContent();
    gotoByteOffset(anchor, false);
}

// 将字节区间按 m_displayEncoding 解码为 Unicode 码点数组
QVector<uint> VirtualFileView::decodeBytesForDisplay(const uchar* data, int len) const {
    QVector<uint> result;
    if (!data || len <= 0) return result;

    switch (m_displayEncoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom: {
        int pos = 0;
        // 跳过 BOM
        if (m_displayEncoding == TextEncoding::Utf8Bom && len >= 3 &&
            data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
            pos = 3;
        }
        result.reserve(len - pos);
        while (pos < len) {
            uint cp;
            int nb = decodeUtf8Char(data, len, pos, &cp);
            result.append(cp);
            pos += nb;
        }
        break;
    }
    case TextEncoding::GBK:
    case TextEncoding::GB18030:
    case TextEncoding::Big5:
    case TextEncoding::ShiftJis:
    case TextEncoding::Windows1252: {
#ifdef Q_OS_WIN
        quint32 codePage = 0;
        windowsCodePageForEncoding(m_displayEncoding, &codePage);
        int wlen = MultiByteToWideChar(codePage, 0, (const char*)data, len, nullptr, 0);
        if (wlen > 0) {
            QVector<wchar_t> wstr(wlen);
            MultiByteToWideChar(codePage, 0, (const char*)data, len, wstr.data(), wlen);
            result.reserve(wlen);
            for (int i = 0; i < wlen; ++i) {
                const quint16 first = quint16(wstr[i]);
                if (QChar::isHighSurrogate(first) && i + 1 < wlen
                    && QChar::isLowSurrogate(quint16(wstr[i + 1]))) {
                    result.append(QChar::surrogateToUcs4(
                        first, quint16(wstr[++i])));
                } else {
                    result.append(first);
                }
            }
        }
#else
        // 非 Windows：用 Qt 的 GB18030 解码器
        auto decoder = QStringDecoder("GB18030");
        if (decoder.isValid()) {
            QString s = decoder(QByteArray((const char*)data, len));
            result.reserve(s.size());
            for (const QChar& c : s)
                result.append(c.unicode());
        }
#endif
        break;
    }
    case TextEncoding::Utf16LE: {
        const QString text = decodeTextFragment(
            QByteArray(reinterpret_cast<const char *>(data), len),
            TextEncoding::Utf16LE);
        result = text.toUcs4();
        break;
    }
    case TextEncoding::Utf16BE: {
        const QString text = decodeTextFragment(
            QByteArray(reinterpret_cast<const char *>(data), len),
            TextEncoding::Utf16BE);
        result = text.toUcs4();
        break;
    }
    }
    return result;
}

void VirtualFileView::computeCursorLineCol(int* line, qint64* col) const {
    // cursorByte_ 始终是虚拟坐标。lineIndex_ 基于原始 mmap，需经 virtToOrig 转换算行号；
    // 列号用虚拟坐标差（用户看到的列）。
    // col 用 qint64：二进制文件单行可达 GB 级，int 会溢出为负（P0 截断根因）。
    if (patches_.isModified()) {
        qint64 origCursor = patches_.virtToOrig(cursorByte_);
        lineIndex_.byteToLineCol(origCursor, line, col);
        qint64 virtLineStart = patches_.origToVirtForLineStart(lineIndex_.lineToByteOffset(*line));
        if (col) {
            *col = cursorByte_ - virtLineStart;
            if (*col < 0) *col = 0;
        }
    } else {
        lineIndex_.byteToLineCol(cursorByte_, line, col);
    }
}

int VirtualFileView::cursorLine() const {
    if (!isOpen()) return 0;
    int line = 0; qint64 col = 0;
    computeCursorLineCol(&line, &col);
    return line;
}

qint64 VirtualFileView::cursorColByte() const {
    if (!isOpen()) return 0;
    int line = 0; qint64 col = 0;
    computeCursorLineCol(&line, &col);
    return col;
}

void VirtualFileView::paintEvent(QPaintEvent* /*e*/) {
    // ==========================================================================
    //  方案：VFV 不做任何绘制。Scintilla overlay 盖满 viewport，是唯一 UI。
    //  这里保持空壳（不创建 QPainter，不画行/行号/光标/水印），仅对未完成的状态不做
    //  任何可见输出——加载提示 / 索引构建 banner / L1 水印一律移到 EditorTab 侧
    //  的独立 QLabel 浮层（后续补齐时不走 VFV paintEvent）。
    // ==========================================================================
}

// ============================================================================
//  鼠标（左键点击 / 拖拽选择）
// ============================================================================
void VirtualFileView::viewportPointToCursorPos(const QPoint& pt, int* outLine, qint64* outVirtCol) const {
    // P0/P2 修复：全程使用虚拟坐标 + qint64。
    // 旧实现用 lineIndex_.lineToByteOffset（原始坐标）算 cursorByte_，编辑后原始≠虚拟，
    // 光标落在错误偏移 → 插入错位成"行末自动换行"。且 int lineLen 在长行溢出为负。
    const int gw = gutterWidth();
    const int lh = qMax(lineHeight(), 1);
    const int first = firstVisibleLine();
    int line = first + pt.y() / lh;
    line = qBound(0, line, int(lineIndex_.totalLines()) - 1);
    int xInContent = pt.x() - gw + horizontalOffsetPx();
    qint64 colBytes = (xInContent <= 0) ? 0 : (qint64(xInContent) / qMax(1, charWidth()));

    // 原始行边界 → 转虚拟坐标（编辑后行边界随之变化）
    qint64 origStart = lineIndex_.lineToByteOffset(line);
    qint64 origEnd   = lineIndex_.lineToByteOffset(line + 1);
    if (origEnd < origStart) origEnd = origStart;
    qint64 vStart = patches_.origToVirt(origStart);
    qint64 vEnd   = patches_.origToVirt(origEnd);
    qint64 virtSize = patches_.virtualSize(mmapSize_);
    if (vStart > virtSize) vStart = virtSize;
    if (vEnd   > virtSize) vEnd   = virtSize;

    // 行末 \n/\r 检查：用虚拟内容（编辑后行末可能变化）
    qint64 displayEnd = vEnd;
    if (displayEnd > vStart) {
        QByteArray lb = patches_.renderRange(mmapBase_, mmapSize_, displayEnd - 1, displayEnd);
        if (!lb.isEmpty() && (lb.at(0) == '\n' || lb.at(0) == '\r')) {
            --displayEnd;
            if (displayEnd > vStart) {
                QByteArray lb2 = patches_.renderRange(mmapBase_, mmapSize_, displayEnd - 1, displayEnd);
                if (!lb2.isEmpty() && lb2.at(0) == '\r') --displayEnd;
            }
        }
    }
    qint64 lineLen = displayEnd - vStart;  // qint64，长行不溢出
    if (lineLen < 0) lineLen = 0;
    colBytes = qBound<qint64>(0, colBytes, lineLen);
    *outLine = line;
    if (outVirtCol) *outVirtCol = colBytes;
}

void VirtualFileView::mousePressEvent(QMouseEvent* e) {
    // VFV 不处理任何鼠标事件；Scintilla overlay 是唯一 UI（焦点/事件全走 Scintilla）。
    // ignore 使事件可能冒泡到父容器，避免在 overlay 外的 viewport 区域点击时出现无响应。
    e->ignore();
}

void VirtualFileView::mouseMoveEvent(QMouseEvent* e) {
    e->ignore();
}

// ============================================================================
//  键盘 / IME（方案不使用 VFV 自绘；全走 overlay Scintilla）
// ============================================================================
void VirtualFileView::keyPressEvent(QKeyEvent* e) {
    // VFV 不接收焦点（NoFocus）；正常情况下该函数不会被触发。
    // ignore 防止任何情况下把事件消费掉。
    QAbstractScrollArea::keyPressEvent(e);
    e->ignore();
}

QVariant VirtualFileView::inputMethodQuery(Qt::InputMethodQuery /*query*/) const {
    // VFV WA_InputMethodEnabled = false；IME 查询无意义
    return QVariant();
}

void VirtualFileView::inputMethodEvent(QInputMethodEvent* e) {
    // VFV 不处理 IME；Scintilla 是唯一输入。
    e->ignore();
}

// ============================================================================
//  导航
// ============================================================================
void VirtualFileView::gotoLine(int line, bool /*selectTo*/) {
    if (!isOpen()) return;
    if (m_viewMode == ViewMode::Hex) {
        line = qBound(0, line, int(hexTotalLines()) - 1);
        cursorByte_ = hexLineToByteOffset(line);
    } else {
        line = qBound(0, line, int(lineIndex_.totalLines()) - 1);
        // P2 修复：cursorByte_ 是虚拟坐标，必须用 origToVirtForLineStart（确保
        // 行首纯 insert 被归属到当前行而不是上一行末尾）。
        cursorByte_ = patches_.origToVirtForLineStart(lineIndex_.lineToByteOffset(line));
    }
    int wantTop = qMax(0, line - visibleLineCount() / 2);
    verticalScrollBar()->setValue(wantTop);
    int cLine; qint64 cCol64;
    if (m_viewMode == ViewMode::Hex) {
        cLine = hexByteToLine(cursorByte_);
        cCol64 = cursorByte_ - hexLineToByteOffset(cLine);
    } else {
        computeCursorLineCol(&cLine, &cCol64);
    }
    emit cursorPositionChanged(cLine, int(qMin<qint64>(INT_MAX, cCol64)));
    viewport()->update();
}

void VirtualFileView::gotoByteOffset(qint64 byteOffset, bool /*selectTo*/) {
    if (!isOpen()) return;
    // P0/P2 修复：clamp 到虚拟文件大小（mmapSize_ + patch 净差），非 mmapSize_。
    // 旧实现 clamp 到 mmapSize_，编辑后无法光标到追加内容。
    qint64 virtSize = patches_.virtualSize(mmapSize_);
    cursorByte_ = qMax<qint64>(0, qMin(byteOffset, virtSize));
    int line; qint64 col64;
    if (m_viewMode == ViewMode::Hex) {
        line = hexByteToLine(cursorByte_);
        col64 = cursorByte_ - hexLineToByteOffset(line);
    } else {
        computeCursorLineCol(&line, &col64);
    }

    // 垂直滚动：把目标行放到视口中央
    verticalScrollBar()->setValue(qMax(0, line - visibleLineCount() / 2));

    // 水平滚动：确保匹配位置在可见范围内（col 用 qint64 算像素，长行不溢出）
    int cw = qMax(1, charWidth());
    int vpW = qMax(1, viewport()->width() - gutterWidth());
    qint64 colPx64;
    if (m_viewMode == ViewMode::Hex) {
        // Hex byte grid uses three display columns per source byte.
        colPx64 = (col64 >= 8 ? col64 * 3 + 1 : col64 * 3) * cw;
    } else {
        colPx64 = col64 * cw;
    }
    int hCur = horizontalScrollBar()->value();
    int hMax = horizontalScrollBar()->maximum();
    if (colPx64 < qint64(hCur) + 20) {
        horizontalScrollBar()->setValue(qMax(0, int(colPx64 - 20)));
    }
    else if (colPx64 > qint64(hCur) + vpW - 20) {
        qint64 target = colPx64 - vpW + 40;
        horizontalScrollBar()->setValue(qMin(hMax, target));
    }

    emit cursorPositionChanged(line, int(qMin<qint64>(INT_MAX, col64)));
    viewport()->update();
}

void VirtualFileView::toggleBookmarkAtCursor() {
    int cLine; qint64 cCol64;
    computeCursorLineCol(&cLine, &cCol64);
    Q_UNUSED(cCol64);
    if (bookmarks_.contains(cLine)) bookmarks_.remove(cLine);
    else                                bookmarks_.insert(cLine);
    viewport()->update();
}

int VirtualFileView::prevBookmark(int fromLine) const {
    if (fromLine < 0) { int l; qint64 c; computeCursorLineCol(&l, &c); fromLine = l; }
    if (bookmarks_.isEmpty()) return -1;
    int best = -1;
    for (int l : bookmarks_) if (l < fromLine && l > best) best = l;
    if (best < 0) for (int l : bookmarks_) if (l > best) best = l;
    return best;
}
int VirtualFileView::nextBookmark(int fromLine) const {
    if (fromLine < 0) { int l; qint64 c; computeCursorLineCol(&l, &c); fromLine = l; }
    if (bookmarks_.isEmpty()) return -1;
    int best = -1, total = int(lineIndex_.totalLines());
    for (int l : bookmarks_) if (l > fromLine && (best < 0 || l < best)) best = l;
    if (best < 0) { best = total; for (int l : bookmarks_) if (l < best) best = l; best = (best == total ? -1 : best); }
    return best;
}
void VirtualFileView::clearBookmarks() { bookmarks_.clear(); viewport()->update(); }

// ============================================================================
//  读 & 保存
// ============================================================================
QByteArray VirtualFileView::readByteRange(qint64 startByte, qint64 endByte) const {
    if (!isOpen()) return {};
    if (startByte < 0) startByte = 0;
    const qint64 vSize = patches_.virtualSize(mmapSize_);
    if (endByte > vSize) endByte = vSize;
    if (endByte <= startByte) return {};
    return patches_.renderRange(mmapBase_, mmapSize_, startByte, endByte);
}

bool VirtualFileView::saveAs(const QString& outPath,
                             std::function<void(qint64,qint64)> progress,
                             std::atomic<bool>* cancel,
                             QString* errMsg)
{
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("没有打开的文件可保存。"); return false; }
    bool ok = patches_.applyAndSave(mmapBase_, mmapSize_, outPath, progress, cancel, errMsg);
    if (ok) {
        // 非 const：VirtualFileView* self = const_cast; 但 resetAfterSave 是保存后副作用，
        // 需要在非 const 方法提供。saveAs 语义是「写盘到新路径不 reset」，因此这里不调。
    }
    return ok;
}

bool VirtualFileView::insertAtCursor(const QByteArray& bytes, QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    const qint64 currentSize = patches_.virtualSize(mmapSize_);
    if (m_virtualSizeLimitExclusive > 0
        && currentSize + bytes.size() >= m_virtualSizeLimitExclusive) {
        if (errMsg)
            *errMsg = QObject::tr("安全文档必须小于 32 MiB；本次编辑未应用。");
        return false;
    }
    qint64 pos = cursorByte_;
    bool ok = patches_.insert(pos, bytes, errMsg);
    if (!ok) return false;
    cursorByte_ = pos + bytes.size();
    selStartByte_ = selEndByte_ = cursorByte_;
    if (patches_.needSoftWarn() || patches_.l2Count() > lastL2Count_) {
        emit patchPressureWarn();
        lastL2Count_ = patches_.l2Count();
    }
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
    return true;
}

bool VirtualFileView::backspaceBytes(int nBytes, QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    if (nBytes <= 0) return true;
    if (cursorByte_ == 0) return true;
    qint64 pos = qMax<qint64>(0, cursorByte_ - nBytes);
    qint32 len = qint32(cursorByte_ - pos);
    QByteArray oldBytes = readByteRange(pos, pos + len);
    if (oldBytes.size() < len) oldBytes.resize(len, '\0');
    bool ok = patches_.removeWithSnapshot(pos, len, oldBytes, errMsg);
    if (!ok) return false;
    cursorByte_ = pos;
    selStartByte_ = selEndByte_ = cursorByte_;
    if (patches_.needSoftWarn() || patches_.l2Count() > lastL2Count_) {
        emit patchPressureWarn();
        lastL2Count_ = patches_.l2Count();
    }
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
    return true;
}

bool VirtualFileView::deleteBytesAhead(int nBytes, QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    if (nBytes <= 0) return true;
    qint64 virtSize = patches_.virtualSize(mmapSize_);
    if (cursorByte_ + nBytes > virtSize) nBytes = int(virtSize - cursorByte_);
    if (nBytes <= 0) return true;
    QByteArray oldBytes = readByteRange(cursorByte_, cursorByte_ + nBytes);
    if (oldBytes.size() < nBytes) oldBytes.resize(nBytes, '\0');
    bool ok = patches_.removeWithSnapshot(cursorByte_, nBytes, oldBytes, errMsg);
    if (!ok) return false;
    selStartByte_ = selEndByte_ = cursorByte_;
    if (patches_.needSoftWarn() || patches_.l2Count() > lastL2Count_) {
        emit patchPressureWarn();
        lastL2Count_ = patches_.l2Count();
    }
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
    return true;
}

bool VirtualFileView::pasteBytes(const QByteArray& bytes, QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    if (bytes.isEmpty()) return true;
    if (bytes.size() > DynamicThresholds::instance().PASTE_LIMIT) {
        emit patchPressureWarn();
    }
    return insertAtCursor(bytes, errMsg);
}

bool VirtualFileView::replaceBytes(qint64 byteOffset, qint32 oldLength, const QByteArray& newData,
                                   QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    if (byteOffset < 0) byteOffset = 0;
    const qint64 virtSize = patches_.virtualSize(mmapSize_);
    if (byteOffset > virtSize) byteOffset = virtSize;
    const qint64 maxLen = virtSize - byteOffset;
    if (oldLength < 0) oldLength = 0;
    if (qint64(oldLength) > maxLen) oldLength = qint32(maxLen);

    if (m_virtualSizeLimitExclusive > 0
        && virtSize - oldLength + newData.size()
               >= m_virtualSizeLimitExclusive) {
        if (errMsg)
            *errMsg = QObject::tr("安全文档必须小于 32 MiB；本次编辑未应用。");
        return false;
    }

    // 1) 如果当前有选区被替换，先把选区删掉，保证"替换当前"不会插在旁边
    if (hasSelection() && qint64(oldLength) == 0) {
        if (!removeSelection(errMsg)) return false;
        byteOffset = cursorByte_; // 删除后光标落在删除点，后续插入点对齐
    }

    QByteArray oldBytes;
    if (oldLength > 0) {
        oldBytes = readByteRange(byteOffset, byteOffset + oldLength);
        if (oldBytes.size() < oldLength) oldBytes.resize(oldLength, '\0');
    }

    QString err;
    const bool ok = oldLength > 0
        ? patches_.replaceWithSnapshots(byteOffset, oldLength, newData, oldBytes, &err)
        : patches_.insert(byteOffset, newData, &err);
    if (!ok) {
        if (errMsg) *errMsg = err;
        return false;
    }

    // 光标落在新内容末尾，便于"替换并继续找下一个"
    cursorByte_ = byteOffset + qint64(newData.size());
    selStartByte_ = selEndByte_ = cursorByte_;

    if (patches_.needSoftWarn() || patches_.l2Count() > lastL2Count_) {
        emit patchPressureWarn();
        lastL2Count_ = patches_.l2Count();
    }
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
    return true;
}

void VirtualFileView::notifyExternalPatchMutation()
{
    const qint64 size = virtualFileSize();
    cursorByte_ = qBound<qint64>(0, cursorByte_, size);
    selStartByte_ = qBound<qint64>(0, selStartByte_, size);
    selEndByte_ = qBound<qint64>(0, selEndByte_, size);
    selAnchorByte_ = qBound<qint64>(0, selAnchorByte_, size);
    updateScrollBarsFromContent();
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
}

bool VirtualFileView::removeSelection(QString* errMsg) {
    if (!isOpen()) { if (errMsg) *errMsg = QObject::tr("文件未打开。"); return false; }
    if (!hasSelection()) return true;
    const qint64 s = selectionStart();
    const qint64 e = selectionEnd();
    const qint32 len = qint32(e - s);
    if (len <= 0) { selStartByte_ = selEndByte_ = cursorByte_; return true; }
    QByteArray oldBytes = readByteRange(s, e);
    if (oldBytes.size() < len) oldBytes.resize(len, '\0');
    QString err;
    const bool ok = patches_.removeWithSnapshot(s, len, oldBytes, &err);
    if (!ok) { if (errMsg) *errMsg = err; return false; }
    cursorByte_ = s;
    selStartByte_ = selEndByte_ = cursorByte_;
    if (patches_.needSoftWarn() || patches_.l2Count() > lastL2Count_) {
        emit patchPressureWarn();
        lastL2Count_ = patches_.l2Count();
    }
    emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    viewport()->update();
    return true;
}

// ============================================================================
//  小 chunk + ±1 预读 + Scintilla 小窗口：公开 API / 工具函数
// ============================================================================
qint64 VirtualFileView::computeDisplayWindowBytes() const {
    // Hex expands every source byte to roughly three display characters and
    // duplicates it in the ASCII document. A 256 KiB source projection forced
    // close to 1 MiB of formatted UI to be rebuilt for a one-byte INS/DEL.
    // 64 KiB still covers more than a thousand rows while keeping structural
    // edits responsive; OVR edits use the in-place projection fast path.
    if (m_viewMode == ViewMode::Hex)
        return Q_INT64_C(64) * 1024;
    // W_display = 11 screens × 每屏字节数（真实 viewport 尺寸 + 当前字体度量，不猜）
    const int rows = qMax(1, visibleLineCount());
    const int vpW  = qMax(1, viewport()->width() - gutterWidth());
    const int cw   = qMax(1, charWidth());
    const int cols = qMax(1, vpW / cw);
    // bytesPerCol ≈ UTF-8 中文混合典型值 2；×1.5 覆盖 tab / CRLF / 长行预留
    const qint64 bytesPerRow = qint64(cols) * 2 * 3 / 2;  // cols*2*1.5，整数避免浮点
    const qint64 perScreen  = qMax<qint64>(1024, bytesPerRow * qint64(rows));
    return qBound<qint64>(Q_INT64_C(256)*1024, qint64(11)*perScreen, Q_INT64_C(8)*1024*1024);
}

qint64 VirtualFileView::computeActiveWindowBytes() const {
    // W_active = min(W_display, W_throughput)
    const qint64 wDisplay = computeDisplayWindowBytes();
    const qint64 fallbackBps = Q_INT64_C(100) * 1024 * 1024;  // 未采样 fallback 100MB/s
    const qint64 bps = (m_sampledThroughputBps > 0) ? m_sampledThroughputBps : fallbackBps;
    // 预算 10ms → clamp [256KB, 8MB]（VSCode LFV 参考 5MB；CSV-ClearView page 硬顶思路）
    qint64 wThroughput = bps / qint64(100);  // bps * 0.010s
    wThroughput = qBound<qint64>(Q_INT64_C(256)*1024, wThroughput, Q_INT64_C(8)*1024*1024);
    return qMin<qint64>(wDisplay, wThroughput);
}

bool VirtualFileView::alignActiveWindowTo(qint64 centerByte, qint64 wBytes) {
    const qint64 vSize = virtualFileSize();
    const qint64 w = qBound<qint64>(Q_INT64_C(64)*1024, wBytes, qMax<qint64>(Q_INT64_C(64)*1024, vSize));
    centerByte = qBound<qint64>(0, centerByte, vSize);

    // 尝试让 center 落在窗口中心 ± 25% 范围：若当前窗口命中，不移动（无缝）。
    qint64 start = m_activeWindowStart;
    qint64 len   = m_activeWindowLen;
    const bool sameLen = (len == w);
    if (sameLen && len > 0) {
        const qint64 l25 = start + len / 4;
        const qint64 u75 = start + (len * 3) / 4;
        if (centerByte >= l25 && centerByte <= u75) {
            return false;  // 无需切换 → 无缝衔接
        }
    }

    // 否则用 center 作为中心重算 start，并对 prev/next chunk 保证有重叠窗口
    // 让新 start = max(0, center - w/2)，并在 vSize 尾部 clamp
    qint64 newStart = (centerByte > w/2) ? (centerByte - w/2) : qint64(0);
    qint64 newLen   = w;
    if (newStart + newLen > vSize) {
        newStart = qMax<qint64>(0, vSize - newLen);
        newLen   = vSize - newStart;   // 尾部短一点也没问题
        if (newLen <= 0) { newStart = qMax<qint64>(0, vSize - 1); newLen = (vSize > 0) ? 1 : 0; }
    }
    // Hex scrolling is expressed in global rows.  The projected window must
    // therefore start on the same row grid; otherwise margin address 0 is an
    // arbitrary byte while the outer scrollbar still denotes row N, and the
    // two visible positions drift after every distant jump.  At EOF keep the
    // aligned prefix as well (the window may grow by less than one row).
    const bool windowTouchesEof = (newStart + newLen >= vSize);
    if (m_viewMode == ViewMode::Hex && m_hexBytesPerRow > 0 && newStart > 0) {
        newStart = (newStart / m_hexBytesPerRow) * m_hexBytesPerRow;
        newLen = windowTouchesEof ? (vSize - newStart)
                                  : qMin(w, vSize - newStart);
    }
    if (newStart == m_activeWindowStart && newLen == m_activeWindowLen) return false;
    m_activeWindowStart = newStart;
    m_activeWindowLen   = newLen;
    return true;
}

bool VirtualFileView::alignActiveWindowToTop(qint64 topByte, qint64 wBytes)
{
    const qint64 virtualSize = virtualFileSize();
    const qint64 windowBytes = qBound<qint64>(
        Q_INT64_C(64) * 1024, wBytes,
        qMax<qint64>(Q_INT64_C(64) * 1024, virtualSize));
    topByte = qBound<qint64>(0, topByte, virtualSize);
    const qint64 newStart = qMin(topByte,
                                 qMax<qint64>(0, virtualSize - windowBytes));
    const qint64 newLength = qMax<qint64>(
        0, qMin(windowBytes, virtualSize - newStart));
    if (newStart == m_activeWindowStart && newLength == m_activeWindowLen)
        return false;
    m_activeWindowStart = newStart;
    m_activeWindowLen = newLength;
    return true;
}

void VirtualFileView::syncActiveWindowToCursor(qint64 centerByte) {
    if (!isOpen()) return;
    const qint64 w = computeActiveWindowBytes();
    if (w != m_lastComputedW) {
        m_lastComputedW = w;
        lruClear();
    }
    const bool changed = alignActiveWindowTo(centerByte, w);
    if (changed && !m_inDestruction) {
        emit activeWindowChanged(m_activeWindowStart, m_activeWindowStart + m_activeWindowLen);
    }
}

// ============================================================================
//  Chunk LRU（硬顶 kChunkSlotsHardCap = 12，CSV ClearView 50-page 思想按比例缩小）
// ============================================================================
void VirtualFileView::lruClear() {
    m_chunkLru.clear();
    m_chunkLru.squeeze();
}

VirtualFileView::ChunkSlot* VirtualFileView::lruFindChunk(qint64 chunkId) {
    const qint64 tick = ++m_lruTickCounter;
    for (auto& s : m_chunkLru) {
        if (s.chunkId == chunkId) {
            s.lastUseTick = tick;
            return &s;
        }
    }
    return nullptr;
}

void VirtualFileView::lruPutChunk(qint64 chunkId, QByteArray bytes) {
    if (chunkId < 0) return;
    // 若已存在，刷新字节并更新 tick
    const qint64 tick = ++m_lruTickCounter;
    for (auto& s : m_chunkLru) {
        if (s.chunkId == chunkId) {
            s.bytes = std::move(bytes);
            s.lastUseTick = tick;
            return;
        }
    }
    // 超过硬顶 → 淘汰 lastUseTick 最小者（LRU）
    if (m_chunkLru.size() >= kChunkSlotsHardCap) {
        int victim = 0;
        for (int i = 1; i < m_chunkLru.size(); ++i) {
            if (m_chunkLru[i].lastUseTick < m_chunkLru[victim].lastUseTick) victim = i;
        }
        m_chunkLru[victim] = ChunkSlot{chunkId, std::move(bytes), tick};
        return;
    }
    m_chunkLru.push_back(ChunkSlot{chunkId, std::move(bytes), tick});
}

// ============================================================================
//  滚动条 → 中心字节（Hex/Text 都能给出大致中心；精确由 Scintilla overlay 决定）
// ============================================================================
qint64 VirtualFileView::computeViewportCenterByteFromScrollbars() const {
    const qint64 vSize = virtualFileSize();
    if (vSize <= 0) return 0;
    if (m_viewMode == ViewMode::Hex) {
        const int vLine = verticalScrollBar()->value() + visibleLineCount() / 2;
        const qint64 off = hexLineToByteOffset(vLine);
        // 再加 8 字节让它偏该行中心；hscroll 暂时忽略（hex 中心字节精度 ±8 足够预览）
        return qBound<qint64>(0, off + m_hexBytesPerRow / 2, vSize);
    }
    // Text 模式：用 LineIndex → 中间一行的起始字节 + 最长行的一半，估算"中心字节"
    const qint64 total = lineIndex_.totalLines();
    const int mid = int(qBound<qint64>(0, qint64(verticalScrollBar()->value()) + qint64(visibleLineCount())/2, total-1));
    // A document first opened as Hex intentionally skips the full text line
    // index. After switching to Text, a distant thumb drag must still address
    // the whole file instead of clamping to the quick index's first 256 KiB.
    // Until the index is complete, the global thumb is a byte-progress scale.
    if (lineIndex_.isIncomplete() && lineIndex_.realCoverageBytes() < vSize) {
        const qint64 minimum = verticalScrollBar()->minimum();
        const qint64 span = qMax<qint64>(
            1, qint64(verticalScrollBar()->maximum()) - minimum);
        const qint64 value = qBound<qint64>(
            minimum, qint64(verticalScrollBar()->value()),
            qint64(verticalScrollBar()->maximum())) - minimum;
        return qBound<qint64>(0, (vSize * value) / span, vSize);
    }
    const qint64 lineStart = lineIndex_.lineToByteOffset(mid);
    qint64 lineLen;
    if (mid + 1 < total) lineLen = lineIndex_.lineToByteOffset(mid + 1) - lineStart;
    else lineLen = vSize - lineStart;
    const qint64 colByte = qMin(lineLen/2, qint64(hscrollBytesApprox()));
    return qBound<qint64>(0, lineStart + colByte, vSize);
}

// ============================================================================
//  连接滚动条三档节流：sliderMoved(L1 预览) → debounce → L2 → release → L3 精确
// ============================================================================
void VirtualFileView::connectScrollBarThrottling() {
    auto* vsb = verticalScrollBar();
    auto* hsb = horizontalScrollBar();
    // L1：拖动过程（高频）→ 只做水印预览 + 记录最新值 + 启动/重置 debounce 120ms
    connect(vsb, &QScrollBar::sliderMoved, this, &VirtualFileView::onScrollSliderMovedPreviewL1);
    connect(hsb, &QScrollBar::sliderMoved, this, &VirtualFileView::onScrollSliderMovedPreviewL1);
    // L3：滑块松开 → 精确落点
    connect(vsb, &QScrollBar::sliderReleased, this, &VirtualFileView::onScrollSliderReleasedL3);
    connect(hsb, &QScrollBar::sliderReleased, this, &VirtualFileView::onScrollSliderReleasedL3);
    // Arrow/page/wheel/keyboard operations emit actionTriggered, while plain
    // valueChanged is also emitted by programmatic range, caret and projection
    // alignment. Feeding every valueChanged back into the loader made those
    // internal corrections look like fresh user requests and could oscillate
    // by a screen forever. Read the final value on the next event turn only
    // for an actual scrollbar action; drag gestures remain owned by
    // sliderMoved/sliderReleased above.
    connect(vsb, &QScrollBar::actionTriggered, this, [this, vsb](int) {
        if (vsb->isSliderDown())
            return;
        QTimer::singleShot(0, this, [this, vsb]() {
            if (isOpen() && !vsb->isSliderDown())
                onScrollSliderMovedPreviewL1(vsb->value());
        });
    });
}

void VirtualFileView::onScrollSliderMovedPreviewL1(int /*value*/) {
    if (!isOpen()) return;
    const qint64 center = computeViewportCenterByteFromScrollbars();
    // L1：只画水印 + 缓存 pending 值 + 重置 debounce；不读 chunk、不灌 Scintilla
    m_previewJumpByteHint = center;
    m_lastScrollValuePending = center;
    if (m_scrollDebounceTimer) m_scrollDebounceTimer->start();  // single-shot，120ms
    viewport()->update();
}

void VirtualFileView::onScrollDebouncedL2() {
    if (!isOpen()) return;
    // L2：用户手指离开 120ms 内没再甩 → 把 pending center 当目标做 ±1 chunk 预取
    const qint64 center = (m_lastScrollValuePending > 0)
        ? m_lastScrollValuePending
        : computeViewportCenterByteFromScrollbars();
    m_previewJumpByteHint = -1;  // 清水印（进入实际重灌阶段）
    viewport()->update();

    // 1) 对齐 activeWindow（不 emit，等 prefetch 完成再 emit，避免半半拉拉灌 Scintilla）
    const qint64 w = computeActiveWindowBytes();
    if (w != m_lastComputedW) {
        m_lastComputedW = w;
        lruClear();
    }
    const bool byteProgressText = m_viewMode == ViewMode::Text
        && lineIndex_.isIncomplete()
        && lineIndex_.realCoverageBytes() < virtualFileSize();
    const bool windowChanged = byteProgressText
        ? alignActiveWindowToTop(center, w)
        : alignActiveWindowTo(center, w);

    // 2) 计算 prev/cur/next 3 个 chunk
    const qint64 cBytes = currentChunkBytes();
    auto chunkOf = [cBytes](qint64 b) { return (cBytes > 0) ? (b / cBytes) : qint64(0); };
    const qint64 s = m_activeWindowStart;
    const qint64 e = qMax(s, m_activeWindowStart + qMax<qint64>(1, m_activeWindowLen) - 1);
    QVector<qint64> ids;
    const qint64 idFirst = chunkOf(s);
    const qint64 idLast  = chunkOf(e);
    for (qint64 i = idFirst - 1; i <= idLast + 1; ++i) ids.push_back(i);

    // 3) 检查 LRU 是否全命中：全命中就直接 emit（零 IO 无缝）；否则后台读
    bool allHit = !ids.isEmpty();
    for (qint64 id : ids) {
        if (id < 0) continue;
        if (id * cBytes >= qMax<qint64>(1, mmapSize_)) continue;
        if (!lruFindChunk(id)) { allHit = false; break; }
    }
    if (allHit) {
        if (windowChanged && !m_inDestruction) {
            emit activeWindowChanged(m_activeWindowStart,
                                     m_activeWindowStart + m_activeWindowLen);
        }
        return;
    }
    // Cache fill alone is not a document change. Rebuilding an identical
    // Scintilla projection after a background prefetch resets the native caret
    // to its end and corrupts the foreground ASCII/Hex editing position.
    submitPrefetchForChunks(ids, /*cancelOngoing=*/true, windowChanged);
}

void VirtualFileView::onScrollSliderReleasedL3() {
    if (!isOpen()) return;
    // 停止 debounce + 清水印：落点精确做一次
    if (m_scrollDebounceTimer) m_scrollDebounceTimer->stop();
    m_previewJumpByteHint = -1;
    const qint64 center = computeViewportCenterByteFromScrollbars();
    const bool byteProgressText = m_viewMode == ViewMode::Text
        && lineIndex_.isIncomplete()
        && lineIndex_.realCoverageBytes() < virtualFileSize();
    if (byteProgressText) {
        const qint64 w = computeActiveWindowBytes();
        if (w != m_lastComputedW) {
            m_lastComputedW = w;
            lruClear();
        }
        if (alignActiveWindowToTop(center, w) && !m_inDestruction) {
            emit activeWindowChanged(m_activeWindowStart,
                                     m_activeWindowStart + m_activeWindowLen);
        }
    } else {
        syncActiveWindowToCursor(center);
    }
    viewport()->update();
}

// ============================================================================
//  Prefetch（prev/cur/next 3 chunk）：AsyncTaskManager 跑 worker，支持甩滚 cancel
// ============================================================================
void VirtualFileView::submitPrefetchForChunks(const QVector<qint64>& chunkIds,
                                               bool cancelOngoing,
                                               bool refreshProjection) {
    if (!isOpen()) return;
    if (chunkIds.isEmpty()) return;

    // 1) 先取消上一轮：这是"减少不必要计算"的关键一步（甩滚时旧位置预取直接丢）
    if (cancelOngoing) {
        if (m_prefetchWatcher && m_prefetchWatcher->isRunning()) m_prefetchWatcher->cancel();
        if (m_prefetchTaskId >= 0) {
            AsyncTaskManager::instance().cancel(m_prefetchTaskId);
            m_prefetchTaskId = -1;
        }
        if (m_prefetchCancel) m_prefetchCancel->store(true, std::memory_order_release);
    }

    const qint64 cBytes = currentChunkBytes();
    QPointer<VirtualFileView> that(this);
    const uchar* base = mmapBase_;
    const qint64 fSize = mmapSize_;

    // 过滤合法 chunkId（<0 / 越界排除）
    QVector<qint64> validIds;
    validIds.reserve(chunkIds.size());
    for (qint64 id : chunkIds) {
        if (id < 0) continue;
        const qint64 start = id * cBytes;
        if (start >= fSize) continue;
        validIds.push_back(id);
    }
    if (validIds.isEmpty()) return;

    m_prefetchCancel = QSharedPointer<std::atomic<bool>>::create(false);
    AsyncCancelPtr outerCancel = m_prefetchCancel;
    const auto workerDone = std::make_shared<std::atomic_bool>(false);
    m_prefetchWorkers.erase(
        std::remove_if(m_prefetchWorkers.begin(), m_prefetchWorkers.end(),
                       [](const auto &done) {
                           return !done || done->load(std::memory_order_acquire);
                       }),
        m_prefetchWorkers.end());
    m_prefetchWorkers.append(workerDone);
    const qint64 activeStart = m_activeWindowStart;
    const qint64 activeLen   = m_activeWindowLen;
    const quint64 mappingGeneration = m_mappingGeneration;
    const bool sensitive = m_memoryBacked;

    m_prefetchTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::Other,
        AsyncTaskPriority::Normal,
        QObject::tr("预读文件块"),
        QObject::tr("准备相邻文件块以无缝滚动..."),
        // WorkerFn = void(taskId, cancel, reportProgress, reportError)
        [that, base, fSize, cBytes, validIds, outerCancel, activeStart, activeLen,
         mappingGeneration, sensitive, workerDone, refreshProjection]
        (qint64, AsyncCancelPtr innerCancel,
         std::function<void(qint64,qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo&)>)
        {
            struct CompletionGuard {
                std::shared_ptr<std::atomic_bool> done;
                ~CompletionGuard() {
                    done->store(true, std::memory_order_release);
                }
            } completion{workerDone};
            const qint64 total = validIds.size();
            QVector<QPair<qint64, QByteArray>> result;  // <chunkId, bytes>
            result.reserve(total);
            const auto scrubResult = [&result, sensitive]() {
                if (!sensitive)
                    return;
                for (auto &item : result) {
                    if (!item.second.isEmpty())
                        item.second.fill(0);
                }
            };
            for (qint64 k = 0; k < total; ++k) {
                const qint64 id = validIds[int(k)];
                const qint64 start = id * cBytes;
                const qint64 end   = qMin(start + cBytes, fSize);
                const qint64 len   = qMax<qint64>(0, end - start);
                // 取消检查（任一 cancel 原子为真立即终止）
                if ((outerCancel && outerCancel->load(std::memory_order_acquire)) ||
                    (innerCancel && innerCancel->load(std::memory_order_acquire))) {
                    scrubResult();
                    return;
                }
                QByteArray buf;
                if (len > 0 && base != nullptr) {
                    const int len32 = (len > INT_MAX) ? INT_MAX : int(len);
                    buf = QByteArray::fromRawData(reinterpret_cast<const char*>(base + start), len32);
                    // 深拷贝，避免 QByteArray 引用已 unmap 的内存
                    buf = QByteArray(buf.constData(), buf.size());
                }
                result.push_back(qMakePair(id, std::move(buf)));
                if (reportProgress) reportProgress(k + 1, total);
            }
            // Worker 线程结束 → 回到主线程写 LRU + 信号（通过 invokeMethod queued）
            auto queuedResult = std::make_shared<
                QVector<QPair<qint64, QByteArray>>>(std::move(result));
            const bool queued = QMetaObject::invokeMethod(that,
                [that, queuedResult, activeStart, activeLen,
                 mappingGeneration, sensitive, refreshProjection]() mutable {
                    auto &result = *queuedResult;
                    const auto scrubQueued = [&result, sensitive]() {
                        if (!sensitive)
                            return;
                        for (auto &item : result) {
                            if (!item.second.isEmpty())
                                item.second.fill(0);
                        }
                    };
                    if (!that || that->m_inDestruction
                        || that->m_mappingGeneration != mappingGeneration
                        || !that->isOpen()) {
                        scrubQueued();
                        return;
                    }
                    // 如果期间用户又甩了一次导致 activeWindow 改了，就不要
                    // 拿旧预取结果去强制覆盖新窗口，防止"灌错字节窗口"。
                    const bool windowStillMatched =
                        (that->m_activeWindowStart == activeStart) &&
                        (that->m_activeWindowLen   == activeLen);
                    for (auto &p : result) {
                        if (p.first >= 0)
                            that->lruPutChunk(p.first, std::move(p.second));
                    }
                    if (refreshProjection && windowStillMatched
                        && that->m_activeWindowLen > 0) {
                        emit that->activeWindowChanged(
                            that->m_activeWindowStart,
                            that->m_activeWindowStart + that->m_activeWindowLen);
                    }
                }, Qt::QueuedConnection);
            if (!queued && sensitive) {
                for (auto &item : *queuedResult) {
                    if (!item.second.isEmpty())
                        item.second.fill(0);
                }
            }
        },
        0, QLatin1String("ChunkPrefetch"), validIds.size()
    );
}

void VirtualFileView::onPrefetchFinished() {
    // 兼容保留：目前 prefetch 结果由 worker lambda 直接 queued invoke 写回，
    // 不走 watcher future。保留槽以应对后续把 submit 接入 QFuture 的改造需求。
    if (m_inDestruction || !isOpen()) return;
    if (m_activeWindowLen > 0) {
        emit activeWindowChanged(m_activeWindowStart,
                                 m_activeWindowStart + m_activeWindowLen);
    }
}

bool VirtualFileView::undo(QString* errMsg) {
    int l0 = patches_.l2Count();
    bool ok = patches_.undo(errMsg);
    if (ok || !patches_.isModified()) emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    if (patches_.l2Count() != l0) emit patchPressureWarn();
    viewport()->update();
    return ok;
}
bool VirtualFileView::redo(QString* errMsg) {
    int l0 = patches_.l2Count();
    bool ok = patches_.redo(errMsg);
    if (ok || !patches_.isModified()) emit saveNeededChanged(patches_.isModified());
    emit undoRedoChanged();
    if (patches_.l2Count() != l0) emit patchPressureWarn();
    viewport()->update();
    return ok;
}

// ============================================================================
//  searchEngine 懒创建
// ============================================================================
LargeFileSearch* VirtualFileView::searchEngine() {
    if (!search_) {
        search_ = new LargeFileSearch(this);
        if (isOpen()) search_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
    }
    return search_;
}

LargeFileSearch* VirtualFileView::navigationSearchEngine() {
    if (!navigationSearch_) {
        navigationSearch_ = new LargeFileSearch(this);
        if (isOpen())
            navigationSearch_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
    }
    return navigationSearch_;
}

void VirtualFileView::setSearchHighlights(const QVector<QPair<qint64, qint32>>& positions) {
    searchHighlights_ = positions;
    viewport()->update();
}

void VirtualFileView::clearSearchHighlights() {
    searchHighlights_.clear();
    viewport()->update();
}

// ============================================================================
//  选择 & 剪贴板
// ============================================================================
QByteArray VirtualFileView::selectedBytes() const {
    qint64 s = selectionStart();
    qint64 e = selectionEnd();
    if (s >= e) return {};
    return readByteRange(s, e);
}

void VirtualFileView::selectAll() {
    if (!isOpen()) return;
    selStartByte_ = 0;
    selEndByte_   = patches_.virtualSize(mmapSize_);
    selAnchorByte_ = selStartByte_;
    cursorByte_   = selEndByte_;
    viewport()->update();
}

void VirtualFileView::copy() const {
    if (!hasSelection()) return;
    QByteArray bytes = selectedBytes();
    if (bytes.isEmpty()) return;
    QString text = QString::fromUtf8(bytes);
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

bool VirtualFileView::cut(QString* errMsg) {
    if (!hasSelection()) return true;
    qint64 s = selectionStart();
    qint64 e = selectionEnd();
    Q_ASSERT(e >= s);
    copy();
    // 删除选中：先移动光标到 start，然后 delete (e-s) 字节 ahead
    cursorByte_ = s;
    selStartByte_ = s;
    selEndByte_ = s;
    selAnchorByte_ = s;
    qint64 n = e - s;
    // PatchList 删除字节数不能超过 INT_MAX（实际不会这么大，单次 cut 最多 2GB 够离谱了）
    int ni = n > INT_MAX ? INT_MAX : static_cast<int>(n);
    bool ok = deleteBytesAhead(ni, errMsg);
    viewport()->update();
    return ok;
}

bool VirtualFileView::paste(QString* errMsg) {
    QString text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    if (text.isEmpty()) return true;
    QByteArray bytes = text.toUtf8();
    return pasteBytes(bytes, errMsg);
}

// ============================================================================
//  后台秒开 LineIndex 构建
// ============================================================================
void VirtualFileView::startAsyncLineBuild() {
    // 重置构建状态：若有旧 task 先 AsyncTaskManager 取消
    if (m_lineBuildTaskId > 0) {
        AsyncTaskManager::instance().cancel(m_lineBuildTaskId);
        m_lineBuildTaskId = -1;
    }
    if (lineBuildCancel_) lineBuildCancel_->store(true, std::memory_order_release);
    lineBuildCancel_.clear();

    if (!mmapBase_ || mmapSize_ <= 0) {
        emit lineBuildFinished(false);
        return;
    }

    const uchar* data = mmapBase_;
    qint64 size       = mmapSize_;
    const quint64 generation = m_mappingGeneration;
    m_lineBuildWorkerDone = std::make_shared<std::atomic_bool>(false);
    const auto workerDone = m_lineBuildWorkerDone;
    QPointer<VirtualFileView> that(this);
    QFileInfo fi(file_.fileName());
    const QString title = tr("构建行索引: %1").arg(fi.fileName().isEmpty() ? tr("(未命名)") : fi.fileName());
    const QString desc  = tr("文件路径: %1\n文件大小: %2 字节\n\n根据 DESIGN.md §6 秒开方案：先以 incomplete 模式返回粗估行数立即可滚动，后台在 AsyncThreadPool 增量构建完整 anchors。用户可在「后台任务」面板随时点击取消。")
        .arg(file_.fileName().isEmpty() ? QLatin1String("-") : file_.fileName())
        .arg(size);

    m_lineBuildTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LineIndexBuild,
        AsyncTaskPriority::Normal,
        title,
        desc,
        /* workerFn = */ [data, size, that, generation, workerDone](qint64 /*taskId*/, AsyncCancelPtr cancel,
                                            std::function<void(qint64,qint64)> reportProgress,
                                            std::function<void(const AsyncErrorInfo&)> reportError)
        {
            struct CompletionFlag {
                std::shared_ptr<std::atomic_bool> done;
                ~CompletionFlag() {
                    if (done) done->store(true, std::memory_order_release);
                }
            } completion{workerDone};
            Q_UNUSED(reportError);
            auto wrappedProgress = [&reportProgress, that](qint64 bytesDone, qint64 bytesTotal) {
                if (reportProgress) reportProgress(bytesDone, bytesTotal);
                if (!that.isNull()) {
                    QMetaObject::invokeMethod(that, [that, bytesDone]() {
                        if (that) {
                            that->m_buildProgressBytes = bytesDone;
                            that->viewport()->update();
                        }
                    }, Qt::QueuedConnection);
                }
            };
            LineIndex result = VirtualFileView::buildLineIndexOnWorker(data, size, cancel, wrappedProgress);
            bool ok = true;
            if (size > 0 && result.totalBytes() == 0 && !result.isIncomplete()) {
                ok = false;
            }
            if (that.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(that,
                [that, generation, result = std::move(result), ok]() mutable {
                    if (!that || that->m_mappingGeneration != generation
                        || that->m_closingFile || !that->mmapBase_) {
                        return;
                    }
                    that->onAsyncLineBuildSwapped(std::move(result), ok);
                }, Qt::QueuedConnection);
        },
        /* batchId = */ 0,
        /* categoryTag = */ QLatin1String("LineIndex"),
        /* progressTotalInit = */ size
    );
    // 给 AsyncTaskManager 的 cancel 指针也保存一份，便于析构/重开取消
    // 说明：AsyncTaskManager 会在 submit 里分配 cancelPtr；我们也可以通过 taskById(m_lineBuildTaskId).cancel 拿
    AsyncTask t = AsyncTaskManager::instance().taskById(m_lineBuildTaskId);
    if (t.cancel) lineBuildCancel_ = t.cancel;
}

LineIndex VirtualFileView::buildLineIndexOnWorker(const uchar* data, qint64 size,
                                                  AsyncCancelPtr cancel,
                                                  std::function<void(qint64,qint64)> reportProgress)
{
    LineIndex result;
    std::atomic<bool>* cancelPtr = cancel ? cancel.data() : nullptr;
    (void)result.buildFromMmap(data, size,
        [&reportProgress](qint64 /*linesDone*/, qint64 bytesDone){ if (reportProgress) reportProgress(bytesDone, -1); },
        cancelPtr, LINEBUILD_PROGRESS_STEP);
    return result;
}

void VirtualFileView::onAsyncLineBuildSwapped(LineIndex result, bool buildOk)
{
    if (m_closingFile || !mmapBase_)
        return;
    // Preserve the byte represented by the old incomplete progress thumb.
    // Reusing its raw integer value after installing the real line range makes
    // the visible content jump to an unrelated line.
    const qint64 viewportAnchor = computeViewportCenterByteFromScrollbars();
    bool ok = buildOk;
    try {
        if (ok) {
            if (mmapSize_ > 0 && result.totalBytes() == 0 && !result.isIncomplete()) {
                ok = false;
            } else {
                lineIndex_.setCompletedAndSwapIn(std::move(result));
                updateMetrics();
                updateScrollBarsFromContent();
                int anchorLine = 0;
                qint64 anchorColumn = 0;
                lineIndex_.byteToLineCol(
                    qBound<qint64>(0, viewportAnchor, mmapSize_),
                    &anchorLine, &anchorColumn);
                {
                    QSignalBlocker blocker(verticalScrollBar());
                    verticalScrollBar()->setValue(qBound(
                        verticalScrollBar()->minimum(), anchorLine,
                        verticalScrollBar()->maximum()));
                }
                m_buildProgressBytes = mmapSize_;  // 构建完成
                viewport()->update();
                if (search_) search_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
                if (navigationSearch_)
                    navigationSearch_->setSource(mmapBase_, mmapSize_, &lineIndex_, &patches_);
                emit lineBuildProgress(mmapSize_, mmapSize_);
                // 同步更新任务进度到 100%（防止 QueuedConnection 进度包在任务入 history 后丢失）
                if (m_lineBuildTaskId > 0) {
                    AsyncTaskManager::instance().updateTaskProgress(m_lineBuildTaskId, mmapSize_, mmapSize_);
                }
            }
        }
    } catch (...) {
        ok = false;
    }
    m_lineBuildTaskId = -1;
    onAsyncLineBuildFinished();
    emit lineBuildFinished(ok);
}

void VirtualFileView::onAsyncLineBuildFinished() {
    // 保持旧接口兼容：外部如果仍 connect 到这个信号，无需改动（我们已经在 swapped 之后 emit lineBuildFinished(ok) 上面发出）
}
