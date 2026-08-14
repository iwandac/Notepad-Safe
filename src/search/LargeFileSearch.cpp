#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"
#include "AsyncTaskManager.h"
#include "AsyncThreadPool.h"

#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QSaveFile>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QFutureSynchronizer>
#include <QThreadPool>
#include <QCoreApplication>
#include <QDateTime>
#include <cstring>
#include <algorithm>

namespace {

constexpr quint32 OVR_MAGIC   = 0x564F534Du; // 'MSOV' little endian stored
constexpr quint32 OVR_VERSION = 1;
constexpr int     OVERRUN_CTX_MAX = 512;

QString overrunDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe");
    QDir().mkpath(d);
    return d;
}

// 大小写转换（只对 ASCII 做；UTF-8 不处理以保持复杂度可控）
inline uchar foldCaseAscii(uchar c, bool caseSensitive) {
    if (caseSensitive) return c;
    if (c >= 'A' && c <= 'Z') return uchar(c + ('a' - 'A'));
    return c;
}

} // namespace

// ============================================================================
//  构造 / 析构 / 数据源
// ============================================================================
LargeFileSearch::LargeFileSearch(QObject* parent)
    : QObject(parent)
    , overrunFp_(nullptr)
{
    maxMemMatches_ = DynamicThresholds::instance().MAX_MATCHES_MEM;
}

LargeFileSearch::~LargeFileSearch() {
    closeOverrunFile();
    if (!overrunFile_.isEmpty()) QFile::remove(overrunFile_);
}

void LargeFileSearch::setSource(const uchar* mmapBase, qint64 mmapSize,
                                const LineIndex* lineIndex, const PatchList* patchList)
{
    mmapBase_ = mmapBase;
    mmapSize_ = mmapSize;
    lineIndex_ = lineIndex;
    patchList_ = patchList;
    memMatches_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
}

void LargeFileSearch::requestCancel() {
    cancel_.store(true, std::memory_order_relaxed);
}

// ============================================================================
//  溢出文件操作
// ============================================================================
bool LargeFileSearch::prepareOverrunFile(QString* errMsg) {
    if (overrunFp_) return true;
    QString fn = QStringLiteral("matches_%1_%2.bin")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch());
    overrunFile_ = QDir(overrunDir()).filePath(fn);
    auto* f = new QFile(overrunFile_);
    if (!f->open(QIODevice::WriteOnly)) {
        if (errMsg) *errMsg = QStringLiteral("无法创建溢出文件：%1").arg(f->errorString());
        delete f;
        overrunFile_.clear();
        return false;
    }
    overrunFp_ = f;
    QDataStream s(overrunFp_);
    s.setByteOrder(QDataStream::LittleEndian);
    s << OVR_MAGIC << OVR_VERSION << quint64(0); // count 占位，收尾时回填
    return true;
}

void LargeFileSearch::closeOverrunFile() {
    if (overrunFp_) {
        // 回填 count
        overrunFp_->seek(8);
        QDataStream s(overrunFp_);
        s.setByteOrder(QDataStream::LittleEndian);
        s << quint64(overrunCount_);
        overrunFp_->close();
        delete overrunFp_; overrunFp_ = nullptr;
    }
}

void LargeFileSearch::appendMatches(const QVector<LargeFileSearchMatch>& batch) {
    qint64 cap = qint64(maxMemMatches_);
    for (const LargeFileSearchMatch& m : batch) {
        if (qint64(memMatches_.size()) < cap) {
            memMatches_.append(m);
        } else {
            // 溢出文件
            if (!overrunFp_ && !prepareOverrunFile(nullptr)) {
                // 溢出文件失败则放弃额外结果（保证不崩）
                continue;
            }
            QDataStream s(overrunFp_);
            s.setByteOrder(QDataStream::LittleEndian);
            s << quint64(m.byteOffset) << qint32(m.length)
              << qint32(m.line)       << qint32(m.col);
            QByteArray ctx = m.lineContext;
            if (ctx.size() > OVERRUN_CTX_MAX) ctx.truncate(OVERRUN_CTX_MAX);
            s << ctx;
            ++overrunCount_;
        }
    }
}

// ============================================================================
//  KMP
// ============================================================================
QVector<int> LargeFileSearch::kmpBuildFail(const QByteArray& needle, bool caseSensitive) {
    QVector<int> fail(needle.size(), 0);
    int k = 0;
    const int m = needle.size();
    for (int q = 1; q < m; ++q) {
        while (k > 0 &&
               foldCaseAscii(needle[k], caseSensitive) != foldCaseAscii(needle[q], caseSensitive))
            k = fail[k-1];
        if (foldCaseAscii(needle[k], caseSensitive) == foldCaseAscii(needle[q], caseSensitive))
            ++k;
        fail[q] = k;
    }
    return fail;
}

QVector<QPair<qint64, int>> LargeFileSearch::kmpMatches(const uchar* text, qint64 n,
                                                        const QByteArray& needle,
                                                        bool caseSensitive)
{
    QVector<QPair<qint64, int>> out;
    if (needle.isEmpty() || n <= 0) return out;
    const int m = needle.size();
    QVector<int> fail = kmpBuildFail(needle, caseSensitive);
    int q = 0;
    for (qint64 i = 0; i < n; ++i) {
        uchar c = foldCaseAscii(text[i], caseSensitive);
        while (q > 0 && foldCaseAscii(needle[q], caseSensitive) != c) q = fail[q-1];
        if (foldCaseAscii(needle[q], caseSensitive) == c) ++q;
        if (q == m) {
            qint64 pos = i + 1 - m;
            out.append(qMakePair(pos, m));
            q = fail[q-1];
        }
    }
    return out;
}

// ============================================================================
//  readVirtualBytes / fillLineContext
// ============================================================================
QByteArray LargeFileSearch::readVirtualBytes(qint64 start, qint64 end) const {
    if (!mmapBase_ || end <= start) return {};
    if (start < 0) start = 0;
    qint64 cap = mmapSize_;
    if (patchList_) {
        // patch 会影响虚拟大小，但无法直接查询；使用 renderRange 时会 clamp 到实际
        return patchList_->renderRange(mmapBase_, mmapSize_, start, end);
    }
    if (end > cap) end = cap;
    return QByteArray((const char*)(mmapBase_ + start), int(end - start));
}

void LargeFileSearch::fillLineContext(LargeFileSearchMatch& m) const {
    if (!lineIndex_) return;
    int line = m.line;
    qint64 start = lineIndex_->lineToByteOffset(line);
    qint64 end   = lineIndex_->lineToByteOffset(line + 1);
    if (end > start + OVERRUN_CTX_MAX + 4) end = start + OVERRUN_CTX_MAX + 4;
    QByteArray lineBytes = readVirtualBytes(start, end);
    // 去掉行尾 \r\n / \n
    while (!lineBytes.isEmpty() && (lineBytes.endsWith('\n') || lineBytes.endsWith('\r')))
        lineBytes.chop(1);
    if (lineBytes.size() > OVERRUN_CTX_MAX) lineBytes.truncate(OVERRUN_CTX_MAX);
    m.lineContext = lineBytes;
}

// ============================================================================
//  scanChunk
// ============================================================================
QVector<LargeFileSearchMatch> LargeFileSearch::scanChunk(qint64 chunkStart, qint64 chunkEnd,
                                                  const Options& opts,
                                                  const QRegularExpression* reOrNull,
                                                  const QByteArray& literalBytesOrEmpty,
                                                  qint64 overlapBack) const
{
    QVector<LargeFileSearchMatch> out;
    if (!mmapBase_ || !lineIndex_ || chunkEnd <= chunkStart) return out;

    // 读入这段虚拟字节（考虑 patch）
    QByteArray chunk = readVirtualBytes(chunkStart, chunkEnd);
    if (chunk.isEmpty()) return out;
    const uchar* text = (const uchar*)chunk.constData();
    qint64 n = chunk.size();

    QVector<QPair<qint64, int>> hits; // (relative pos in chunk, length)
    if (reOrNull) {
        QString s = QString::fromUtf8(chunk);
        auto it = reOrNull->globalMatch(s);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            if (m.capturedStart() < 0 || m.capturedLength() <= 0) continue;
            // 把 UTF-16 偏移转回字节偏移（非 ASCII 场景偏差，P1 按字符序近似；P2 用 UTF-8 偏移匹配器）
            qint64 bytePos = QStringView{s.data(), m.capturedStart()}.toUtf8().size();
            qint32 len = m.captured().toUtf8().size();
            hits.append(qMakePair(bytePos, len));
        }
    } else {
        hits = kmpMatches(text, n, literalBytesOrEmpty, opts.caseSensitive);
    }

    // 过滤：
    //   1) overlapBack 区内的匹配如果与前一块重复则丢弃（跨 overlap 边界的匹配保留）
    //      我们用「匹配起点不在 overlapBack 内 但匹配内容部分在 chunk 内」原则无法精确判定，
    //      因此采用：在前 overlapBack 字节内的纯匹配一律丢弃，后续由 UI 阶段去重。
    //      跨块匹配（起点 < overlapBack 且 start + length > overlapBack）保留。
    for (auto& hit : hits) {
        qint64 localStart = hit.first;
        qint32 len = hit.second;
        bool inBackOnly     = (localStart + len <= overlapBack);
        bool spansBoundary  = (localStart < overlapBack && localStart + len > overlapBack);
        bool inBody         = (localStart >= overlapBack);
        if (inBackOnly && !spansBoundary) continue; // 丢弃纯前一块重叠重复

        LargeFileSearchMatch m;
        m.byteOffset = chunkStart + localStart;
        m.length     = len;
        int ln; qint64 col64;
        lineIndex_->byteToLineCol(m.byteOffset, &ln, &col64);
        m.line = ln;
        m.col  = qint32(qMin<qint64>(INT32_MAX, col64));  // 长行 col 可能超 int32，钳到上限
        fillLineContext(m);
        out.append(std::move(m));
    }
    return out;
}

// ============================================================================
//  findAll（异步）
// ============================================================================
void LargeFileSearch::findAll(const QString& pattern, const Options& opts) {
    if (!mmapBase_ || !lineIndex_) {
        emit failed(QStringLiteral("数据源未设置。"));
        return;
    }
    // 如果已经被 requestCancel，立即取消不启动 worker（避免先 cancel 再 findAll 被重置失效）
    if (cancel_.exchange(false, std::memory_order_acq_rel)) {
        emit failed(QStringLiteral("已取消。"));
        return;
    }
    // 内存屏障：worker 前主线程所有写入可见；memMatches/overrunCount 初始值正确刷入
    std::atomic_thread_fence(std::memory_order_release);
    memMatches_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
    memMatches_.reserve(int(qMin<qint64>(maxMemMatches_, qint64(2000000))));

    QRegularExpression re;
    QByteArray literalBytes;
    bool useRegex = !opts.regex.pattern().isEmpty();
    if (useRegex) {
        re = opts.regex;
        if (!re.isValid()) {
            emit failed(QStringLiteral("正则无效：%1").arg(re.errorString()));
            return;
        }
    } else {
        literalBytes = pattern.toUtf8();
        if (literalBytes.isEmpty()) {
            emit failed(QStringLiteral("查找文本为空。"));
            return;
        }
    }

    const qint64 CHUNK = qint64(16) * 1024 * 1024;
    const qint64 OVERLAP = qint64(4) * 1024;

    // 把整个虚拟范围 [0, virtualSize) 切块；virtualSize 默认 = mmapSize_；
    // 有 patch 时，我们假定 patch 总长度变化不超过 4GB 上限（16MB 分块仍合理），
    // 采用 [0, mmapSize_ + 合理 patch 总长度)。这里简化：以 mmapSize_ + 512MB 作为
    // 上限扫描（真正的读取会被 renderRange 安全 clamp）。
    qint64 virtualSize = mmapSize_;
    if (patchList_ && patchList_->isModified()) virtualSize += qint64(512) * 1024 * 1024;

    // 多线程并行搜索：将 [0, virtualSize) 分成 N 段（N = SEARCH_THREADS），
    // 每段由独立线程并行扫描，最后合并结果。
    // 有 patch（isModified）时段边界可能不准确 → 降级为单线程。
    auto worker = [this, virtualSize, CHUNK, OVERLAP, opts, re, literalBytes, useRegex]() {
        const bool hasPatch = (patchList_ && patchList_->isModified());
        const int numThreads = DynamicThresholds::instance().SEARCH_THREADS;

        std::atomic<qint64> bytesScanned{0};

        auto isCancelled = [this]() -> bool {
            return cancel_.load(std::memory_order_relaxed);
        };

        using SegMatches = QVector<LargeFileSearchMatch>;

        // 单段扫描：在 [segStart, segEnd) 内用 16MB CHUNK + 4KB OVERLAP 机制
        // 段边界处也做 overlap（首块 start==0 时无 overlapBack，其余块均有）
        auto scanSegment = [&](qint64 segStart, qint64 segEnd) -> SegMatches {
            SegMatches segMatches;
            if (segEnd <= segStart) return segMatches;
            qint64 myLastProg = 0;
            for (qint64 start = segStart; start < segEnd; ) {
                if (isCancelled()) break;
                qint64 end = qMin(segEnd, start + CHUNK);
                qint64 scanStart = (start == 0) ? 0 : qMax<qint64>(0, start - OVERLAP);
                qint64 overlapBack = start - scanStart;
                QVector<LargeFileSearchMatch> batch = scanChunk(scanStart, end, opts,
                                                       useRegex ? &re : nullptr,
                                                       literalBytes,
                                                       overlapBack);
                if (!batch.isEmpty()) segMatches += batch;
                bytesScanned.fetch_add(end - start, std::memory_order_relaxed);
                qint64 total = bytesScanned.load(std::memory_order_relaxed);
                if (total - myLastProg >= CHUNK) {
                    myLastProg = total;
                    emit progress(total, virtualSize);
                }
                start = end;
            }
            return segMatches;
        };

        QVector<SegMatches> segResults;

        if (hasPatch || numThreads <= 1 || virtualSize <= CHUNK) {
            // 降级单线程：patch 修改 / 单核 / 小文件
            segResults.append(scanSegment(0, virtualSize));
        } else {
            // 多线程并行：N 段，用独立 QThreadPool 避免与外层 QtConcurrent::run 死锁
            // 通告 AsyncThreadPool UI：实际将启动 numThreads 条外部线程
            AsyncThreadPool::instance().incExternalActive(numThreads);
            struct ScopeDec { int n; ~ScopeDec() { AsyncThreadPool::instance().decExternalActive(n); } } dec_{numThreads};
            QThreadPool innerPool;
            innerPool.setMaxThreadCount(numThreads);
            QFutureSynchronizer<SegMatches> sync;
            for (int i = 0; i < numThreads; ++i) {
                qint64 segStart = quint64(virtualSize) * quint64(i) / quint64(numThreads);
                qint64 segEnd   = quint64(virtualSize) * quint64(i + 1) / quint64(numThreads);
                if (segStart >= segEnd) continue;
                sync.addFuture(QtConcurrent::run(&innerPool, scanSegment, segStart, segEnd));
            }
            sync.waitForFinished();
            innerPool.waitForDone();
            for (const auto& f : sync.futures()) {
                segResults.append(f.result());
            }
        }

        if (isCancelled()) {
            emit failed(QStringLiteral("已取消。"));
            return;
        }

        // 合并各段结果（段内已按 byteOffset 排序，段间也是有序的，直接拼接）
        QVector<LargeFileSearchMatch> merged;
        for (auto& sm : segResults) merged += sm;

        // 去重跨段 overlap 的匹配：按 byteOffset 排序 + unique
        std::sort(merged.begin(), merged.end(),
                  [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                      return a.byteOffset < b.byteOffset;
                  });
        {
            QVector<LargeFileSearchMatch> deduped;
            deduped.reserve(merged.size());
            qint64 lastOff = -1;
            for (auto& m : merged) {
                if (m.byteOffset != lastOff) { deduped.append(std::move(m)); lastOff = m.byteOffset; }
            }
            merged.swap(deduped);
        }

        // 一次性合并到主结果（appendMatches 非线程安全，此处 workerFn 线程独占）
        if (!merged.isEmpty()) {
            appendMatches(merged);
            emit partialBatch(merged);
        }

        // release 屏障：确保所有 memMatches_/overrunCount_ 写入对主线程 totalMatches() acquire 可见
        std::atomic_thread_fence(std::memory_order_release);
        qint64 n = qint64(memMatches_.size()) + overrunCount_;
        emit progress(virtualSize, virtualSize);
        emit finished(n);
    };

    QtConcurrent::run(worker);
}

// ============================================================================
//  findAllViaTaskManager：AsyncTaskManager 统一派发，AsyncTaskPanel 可见/可取消
// ============================================================================
qint64 LargeFileSearch::findAllViaTaskManager(const QString& pattern, const Options& opts,
                                              const QString& fileDisplayName)
{
    if (!mmapBase_ || !lineIndex_) {
        emit failed(QStringLiteral("数据源未设置。"));
        return -1;
    }
    // 如果已经被 requestCancel，立即取消不启动 worker
    if (cancel_.exchange(false, std::memory_order_acq_rel)) {
        emit failed(QStringLiteral("已取消。"));
        return -1;
    }
    std::atomic_thread_fence(std::memory_order_release);
    memMatches_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
    memMatches_.reserve(int(qMin<qint64>(maxMemMatches_, qint64(2000000))));

    QRegularExpression re;
    QByteArray literalBytes;
    bool useRegex = !opts.regex.pattern().isEmpty();
    if (useRegex) {
        re = opts.regex;
        if (!re.isValid()) {
            emit failed(QStringLiteral("正则无效：%1").arg(re.errorString()));
            return -1;
        }
    } else {
        literalBytes = pattern.toUtf8();
        if (literalBytes.isEmpty()) {
            emit failed(QStringLiteral("查找文本为空。"));
            return -1;
        }
    }

    const qint64 CHUNK = qint64(16) * 1024 * 1024;
    const qint64 OVERLAP = qint64(4) * 1024;
    qint64 virtualSize = mmapSize_;
    if (patchList_ && patchList_->isModified()) virtualSize += qint64(512) * 1024 * 1024;

    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("(未命名大文件)")
        : fileDisplayName;
    const QString title = QObject::tr("搜索大文件: %1").arg(displayName);
    const QString desc  = QObject::tr("文件: %1\n模式: %2\n大小写敏感: %3\n整词匹配: %4\n\n搜索任务走 AsyncThreadPool 统一调度，可在「后台任务」面板点击取消。进度按已扫描字节/总虚拟大小计算。")
        .arg(displayName)
        .arg(useRegex ? QObject::tr("正则表达式") : QObject::tr("字面值"))
        .arg(opts.caseSensitive ? QObject::tr("是") : QObject::tr("否"))
        .arg(opts.wholeWord ? QObject::tr("是") : QObject::tr("否"));

    // 捕获 LargeFileSearch 原始指针为本地变量，worker 中使用需谨慎：
    //  我们通过信号的 QueuedConnection 把 finished/failed/progress 回主线程；
    //  若对象在中途被析构，LargeFileSearch::requestCancel()/cancel_ 原子标志保证安全退出，
    //  但要避免访问成员变量（除非对象仍存在）。这里我们用 QPointer + QMetaObject::invokeMethod
    //  做主线程安全回调；scanChunk/readVirtualBytes 本身是 const，只要对象未析构即可调用。
    QPointer<LargeFileSearch> that(this);
    lastTaskId_ = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch,
        AsyncTaskPriority::High,
        title,
        desc,
        /* workerFn = */ [that, virtualSize, CHUNK, OVERLAP, opts, re, literalBytes, useRegex]
        (qint64 /*taskId*/, AsyncCancelPtr cancel,
         std::function<void(qint64,qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo&)> reportError)
        {
            Q_UNUSED(reportError);
            if (!that) {
                AsyncErrorInfo e;
                e.message = QObject::tr("LargeFileSearch 对象已析构，搜索中止。");
                reportError(e);
                return;
            }

            // 把 AsyncTaskManager 提供的 cancel 原子也写入 LargeFileSearch.cancel_
            // 这样 requestCancel() 和 AsyncTask 面板取消都会生效（任一 true 即中止）。
            that->cancel_.store(false, std::memory_order_release);

            const bool hasPatch = (that->patchList_ && that->patchList_->isModified());
            const int numThreads = DynamicThresholds::instance().SEARCH_THREADS;

            std::atomic<qint64> bytesScanned{0};

            // 双取消标志：Manager cancel 或对象自身 cancel_
            auto isCancelled = [&]() -> bool {
                return (cancel && cancel->load(std::memory_order_acquire))
                    || that->cancel_.load(std::memory_order_relaxed);
            };

            using SegMatches = QVector<LargeFileSearchMatch>;

            // 单段扫描：在 [segStart, segEnd) 内用 16MB CHUNK + 4KB OVERLAP 机制
            // 各段 worker 收集到本地 vector（appendMatches 非线程安全，不能直接调）
            auto scanSegment = [&](qint64 segStart, qint64 segEnd) -> SegMatches {
                SegMatches segMatches;
                if (segEnd <= segStart) return segMatches;
                qint64 myLastProg = 0;
                for (qint64 start = segStart; start < segEnd; ) {
                    if (isCancelled()) break;
                    qint64 end = qMin(segEnd, start + CHUNK);
                    qint64 scanStart = (start == 0) ? 0 : qMax<qint64>(0, start - OVERLAP);
                    qint64 overlapBack = start - scanStart;
                    QVector<LargeFileSearchMatch> batch = that->scanChunk(scanStart, end, opts,
                                                           useRegex ? &re : nullptr,
                                                           literalBytes,
                                                           overlapBack);
                    if (!batch.isEmpty()) segMatches += batch;
                    bytesScanned.fetch_add(end - start, std::memory_order_relaxed);
                    qint64 total = bytesScanned.load(std::memory_order_relaxed);
                    if (total - myLastProg >= CHUNK) {
                        myLastProg = total;
                        reportProgress(total, virtualSize);
                        QMetaObject::invokeMethod(that, "progress", Qt::QueuedConnection,
                                                  Q_ARG(qint64, total), Q_ARG(qint64, virtualSize));
                    }
                    start = end;
                }
                return segMatches;
            };

            QVector<SegMatches> segResults;

            if (hasPatch || numThreads <= 1 || virtualSize <= CHUNK) {
                // 降级单线程：patch 修改 / 单核 / 小文件（段边界在 patch 下不准确）
                segResults.append(scanSegment(0, virtualSize));
            } else {
                // 多线程并行：将 [0, virtualSize) 分成 numThreads 段
                // 通告 AsyncThreadPool UI：实际将启动 numThreads 条外部线程
                AsyncThreadPool::instance().incExternalActive(numThreads);
                struct ScopeDec { int n; ~ScopeDec() { AsyncThreadPool::instance().decExternalActive(n); } } dec_{numThreads};
                QThreadPool innerPool;
                innerPool.setMaxThreadCount(numThreads);
                QFutureSynchronizer<SegMatches> sync;
                for (int i = 0; i < numThreads; ++i) {
                    qint64 segStart = quint64(virtualSize) * quint64(i) / quint64(numThreads);
                    qint64 segEnd   = quint64(virtualSize) * quint64(i + 1) / quint64(numThreads);
                    if (segStart >= segEnd) continue;
                    sync.addFuture(QtConcurrent::run(&innerPool, scanSegment, segStart, segEnd));
                }
                sync.waitForFinished();
                innerPool.waitForDone();
                for (const auto& f : sync.futures()) {
                    segResults.append(f.result());
                }
            }

            if (isCancelled()) {
                QMetaObject::invokeMethod(that, "failed", Qt::QueuedConnection,
                                          Q_ARG(QString, QObject::tr("已取消。")));
                return;
            }

            // 合并各段结果（段内已按 byteOffset 排序，段间也是有序的，直接拼接）
            QVector<LargeFileSearchMatch> merged;
            for (auto& sm : segResults) merged += sm;

            // 去重跨段 overlap 的匹配：按 byteOffset 排序 + unique
            std::sort(merged.begin(), merged.end(),
                      [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                          return a.byteOffset < b.byteOffset;
                      });
            {
                QVector<LargeFileSearchMatch> deduped;
                deduped.reserve(merged.size());
                qint64 lastOff = -1;
                for (auto& m : merged) {
                    if (m.byteOffset != lastOff) { deduped.append(std::move(m)); lastOff = m.byteOffset; }
                }
                merged.swap(deduped);
            }

            // 一次性合并到主结果（appendMatches 非线程安全，此处 workerFn 线程独占）
            if (!merged.isEmpty()) {
                that->appendMatches(merged);
                QMetaObject::invokeMethod(that, "partialBatch", Qt::QueuedConnection,
                                          Q_ARG(QVector<LargeFileSearchMatch>, merged));
            }

            std::atomic_thread_fence(std::memory_order_release);
            qint64 n = qint64(that->memMatches_.size()) + that->overrunCount_;
            reportProgress(virtualSize, virtualSize);
            QMetaObject::invokeMethod(that, "progress", Qt::QueuedConnection,
                                      Q_ARG(qint64, virtualSize), Q_ARG(qint64, virtualSize));
            QMetaObject::invokeMethod(that, "finished", Qt::QueuedConnection, Q_ARG(qint64, n));
        },
        /* batchId = */ 0,
        /* categoryTag = */ QLatin1String("Search"),
        /* progressTotalInit = */ virtualSize
    );
    return lastTaskId_;
}

// ============================================================================
//  findNext / findPrev
// ============================================================================
qint64 LargeFileSearch::findNext(qint64 fromByte, LargeFileSearchMatch* outMatch) const {
    // 线性扫：P1 不做二分，避免复杂溢出索引；100 万条量级可接受。
    for (const auto& m : memMatches_) {
        if (m.byteOffset > fromByte) {
            if (outMatch) *outMatch = m;
            return m.byteOffset;
        }
    }
    // 溢出文件：顺序读取（P1 简化）。P2 可加二分索引。
    if (overrunCount_ > 0 && QFile::exists(overrunFile_)) {
        QFile f(overrunFile_);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(8+8); // 跳过 MAGIC+VERSION+COUNT 占位（实际 COUNT 无用，顺序读）
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            while (!s.atEnd()) {
                quint64 bo; qint32 len, ln, col; QByteArray ctx;
                s >> bo >> len >> ln >> col >> ctx;
                if (bo > quint64(fromByte)) {
                    if (outMatch) {
                        outMatch->byteOffset = qint64(bo);
                        outMatch->length = len;
                        outMatch->line = ln;
                        outMatch->col = col;
                        outMatch->lineContext = ctx;
                    }
                    return qint64(bo);
                }
            }
        }
    }
    return -1;
}
qint64 LargeFileSearch::findPrev(qint64 fromByte, LargeFileSearchMatch* outMatch) const {
    qint64 best = -1;
    const LargeFileSearchMatch* bestM = nullptr;
    for (const auto& m : memMatches_) {
        if (m.byteOffset < fromByte && (best < 0 || m.byteOffset > best)) {
            best = m.byteOffset; bestM = &m;
        }
    }
    if (overrunCount_ > 0 && QFile::exists(overrunFile_)) {
        QFile f(overrunFile_);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(16);
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            static thread_local LargeFileSearchMatch tmp;
            while (!s.atEnd()) {
                quint64 bo; qint32 len, ln, col; QByteArray ctx;
                s >> bo >> len >> ln >> col >> ctx;
                if (bo < quint64(fromByte) && qint64(bo) > best) {
                    best = qint64(bo);
                    tmp.byteOffset = qint64(bo); tmp.length = len; tmp.line = ln;
                    tmp.col = col; tmp.lineContext = ctx;
                    bestM = &tmp;
                }
            }
        }
    }
    if (best >= 0 && bestM && outMatch) *outMatch = *bestM;
    return best;
}

// ============================================================================
//  replaceAll（§5.6 步骤 3，同步执行，1000 条 = 1 UndoGroup）
// ============================================================================
bool LargeFileSearch::replaceAll(const QString& pattern, const QString& replacement,
                                 const Options& optsIn, PatchList& patches,
                                 ReplaceCallbacks cb, QString* errMsg,
                                 qint64* outReplacedCount)
{
    if (outReplacedCount) *outReplacedCount = 0;
    Options opts = optsIn;
    opts.forward = true;
    // Step 1：先跑一次 findAll（同步的本地版本，避免信号队列）
    cancel_.store(false);
    memMatches_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }

    QRegularExpression re;
    QByteArray literalBytes;
    bool useRegex = !opts.regex.pattern().isEmpty();
    if (useRegex) {
        re = opts.regex;
        if (!re.isValid()) { if (errMsg) *errMsg = QStringLiteral("正则无效"); return false; }
    } else {
        literalBytes = pattern.toUtf8();
        if (literalBytes.isEmpty()) { if (errMsg) *errMsg = QStringLiteral("pattern 空"); return false; }
    }
    QByteArray replBytes = replacement.toUtf8();

    // Step 1 简化：直接收集所有匹配（分批收集，但这里内存直接做；溢出则失败——replaceAll
    // 必须有完整匹配列表才能正确按 byteOffset 从大到小替换）
    QVector<LargeFileSearchMatch> allMatches;
    const qint64 CHUNK = qint64(16) * 1024 * 1024;
    const qint64 OVERLAP = qint64(4) * 1024;
    qint64 virtualSize = mmapSize_;
    if (patchList_ && patchList_->isModified()) virtualSize += qint64(512) * 1024 * 1024;

    for (qint64 start = 0; start < virtualSize; ) {
        if (cancel_.load()) { if (errMsg) *errMsg = QStringLiteral("取消"); return false; }
        qint64 end = qMin(virtualSize, start + CHUNK);
        qint64 scanStart = (start == 0) ? 0 : qMax<qint64>(0, start - OVERLAP);
        qint64 overlapBack = start - scanStart;
        QVector<LargeFileSearchMatch> batch = scanChunk(scanStart, end, opts,
                                               useRegex ? &re : nullptr,
                                               literalBytes,
                                               overlapBack);
        for (auto& m : batch) allMatches.append(std::move(m));
        start = end;
        if (!patchList_ && start >= mmapSize_) break;
    }
    // 去重（跨 OVERLAP 可能重复的匹配）：按 byteOffset 排序 + unique
    std::sort(allMatches.begin(), allMatches.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) { return a.byteOffset < b.byteOffset; });
    {
        QVector<LargeFileSearchMatch> dedup;
        qint64 last = -1;
        for (auto& m : allMatches) {
            if (m.byteOffset != last) { dedup.append(m); last = m.byteOffset; }
        }
        allMatches.swap(dedup);
    }

    qint64 total = allMatches.size();
    if (total == 0) {
        if (cb.progressBatch) cb.progressBatch(0, 0);
        if (outReplacedCount) *outReplacedCount = 0;
        return true;
    }

    // Step 2：压力检测（估算替换导致的内存增量 = Σ|replBytes.size() - m.length|）
    qint64 estSize = 0;
    for (auto& m : allMatches) estSize += qAbs<qint64>(replBytes.size() - m.length);
    if (cb.onPressureSoftWarn && patches.pressureWillExceedSoftWarn(estSize)) {
        bool decision = cb.onPressureSoftWarn(
            QObject::tr("预估替换将使 Patch 达到 ~%1 MB，建议中间保存后再继续。是否立即保存并继续？")
                .arg(double(patches.l1Bytes() + estSize)/1048576., 1, 'f', 1));
        if (decision) {
            if (!mmapBase_ || !patchList_) {
                if (errMsg) *errMsg = QStringLiteral("缺少数据源无法中间保存");
                return false;
            }
            QString path = cb.needSaveAsPath ? cb.needSaveAsPath() : QString();
            if (path.isEmpty()) {
                if (errMsg) *errMsg = QStringLiteral("未提供保存路径。");
                return false;
            }
            std::atomic<bool> c{false};
            if (!patches.applyAndSave(mmapBase_, mmapSize_, path, {}, &c, errMsg)) return false;
            patches.resetAfterSave();
        }
    }

    // Step 3：1000 条 = 1 UndoGroup；按 byteOffset 从大到小替换（避免 offset 漂移）
    //   每一条 replace 内部把 oldSnapshot 和 newText 正确填好，保证 undo 严格回滚。
    const int BATCH = DynamicThresholds::instance().REPLACE_BATCH_SIZE;
    qint64 done = 0;
    std::sort(allMatches.begin(), allMatches.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) { return a.byteOffset > b.byteOffset; });

    for (qint64 bStart = 0; bStart < total; bStart += BATCH) {
        if (cancel_.load()) { if (errMsg) *errMsg = QStringLiteral("取消"); return false; }
        patches.beginUndoGroup();
        qint64 bEnd = qMin<qint64>(bStart + BATCH, total);
        for (qint64 i = bStart; i < bEnd; ++i) {
            const LargeFileSearchMatch& m = allMatches[int(i)];
            QByteArray oldBytes;
            if (mmapBase_) {
                oldBytes = readVirtualBytes(m.byteOffset, m.byteOffset + m.length);
            }
            if (oldBytes.size() < m.length) oldBytes.resize(m.length, '\0');
            QString localErr;
            bool ok = patches.replaceWithSnapshots(m.byteOffset, m.length, replBytes, oldBytes,
                                                   &localErr);
            if (!ok) {
                if (errMsg) *errMsg = QObject::tr("第 %1 处替换失败：%2").arg(i).arg(localErr);
                patches.endUndoGroup();
                return false;
            }
        }
        patches.endUndoGroup();
        done = bEnd;
        if (cb.progressBatch) cb.progressBatch(done, total);

        if (cb.onPressureSoftWarn && patches.needSoftWarn()) {
            bool doSave = cb.onPressureSoftWarn(
                QObject::tr("Patch 占用已达 %1/%2 MB，是否保存后继续？")
                    .arg(double(patches.l1Bytes())/1048576., 1, 'f', 1)
                    .arg(double(patches.l1Limit())/1048576., 1, 'f', 1));
            if (doSave) {
                QString path = cb.needSaveAsPath ? cb.needSaveAsPath() : QString();
                if (path.isEmpty()) { if (errMsg) *errMsg = QStringLiteral("未提供保存路径。"); return false; }
                std::atomic<bool> c{false};
                if (!patches.applyAndSave(mmapBase_, mmapSize_, path, {}, &c, errMsg)) return false;
                patches.resetAfterSave();
            }
        }
    }
    if (outReplacedCount) *outReplacedCount = done;
    return true;
}
