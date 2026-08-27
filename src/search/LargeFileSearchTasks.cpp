#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"
#include "AsyncTaskManager.h"
#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"

#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QDateTime>
#include <QUuid>
#include <QPointer>
#include <cstring>
#include <algorithm>
#include <utility>

namespace {

constexpr quint32 OVR_MAGIC = 0x564F534Du;
constexpr quint32 OVR_VERSION = 2;

QString overrunDir() {
    QString d = QDir::tempPath() + QStringLiteral("/notepad-safe");
    QDir().mkpath(d);
    return d;
}

bool readOverrunHeader(QDataStream &stream, quint32 *version, quint64 *count)
{
    quint32 magic = 0;
    quint32 parsedVersion = 0;
    quint64 parsedCount = 0;
    stream >> magic >> parsedVersion >> parsedCount;
    if (stream.status() != QDataStream::Ok || magic != OVR_MAGIC
        || (parsedVersion != 1 && parsedVersion != OVR_VERSION)) {
        return false;
    }
    if (version) *version = parsedVersion;
    if (count) *count = parsedCount;
    return true;
}

bool readOverrunRecord(QDataStream &stream, quint32 version,
                       LargeFileSearchMatch *match)
{
    if (!match)
        return false;
    quint64 byteOffset = 0;
    qint32 length = 0;
    qint32 line = 0;
    qint32 col = 0;
    quint64 contextByteOffset = 0;
    QByteArray context;
    stream >> byteOffset >> length >> line >> col;
    if (version >= 2)
        stream >> contextByteOffset;
    stream >> context;
    if (stream.status() != QDataStream::Ok)
        return false;
    match->byteOffset = qint64(byteOffset);
    match->length = length;
    match->line = line;
    match->col = col;
    match->contextByteOffset = version >= 2
        ? qint64(contextByteOffset)
        : qMax<qint64>(0, qint64(byteOffset) - col);
    match->lineContext = std::move(context);
    return true;
}

} // namespace

void LargeFileSearch::findAll(const QString& pattern, const Options& opts) {
    findAllViaTaskManager(pattern, opts, QString());
}

// ============================================================================
//  findAllViaTaskManager：AsyncTaskManager 统一派发，AsyncTaskPanel 可见/可取消
// ============================================================================
qint64 LargeFileSearch::findAllViaTaskManager(const QString& pattern, const Options& opts,
                                              const QString& fileDisplayName)
{
    if (!mmapBase_) {
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
    storageFailed_ = false;
    scanError_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunCheckpointBytes_.clear();
    overrunGroupedCount_ = 0;
    overrunLastLine_ = std::numeric_limits<qint32>::min();
    overrunLastContext_ = std::numeric_limits<qint64>::min();
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }
    memMatches_.reserve(int(qMin<qint64>(maxMemMatches_, qint64(2000000))));

    QByteArray literalBytes;
    bool usePcre2 = false;
    QString patternError;
    if (!preparePattern(pattern, opts, &literalBytes,
                              &usePcre2, &patternError)) {
        emit failed(patternError);
        return -1;
    }

    if (!reserveWorker()) {
        emit failed(QStringLiteral("已有搜索正在运行。"));
        return -1;
    }
    auto releaseOnSubmitFailure = qScopeGuard([this] { releaseWorker(); });

    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;

    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("(未命名大文件)")
        : fileDisplayName;
    const QString title = QObject::tr("搜索大文件: %1").arg(displayName);
    const QString desc  = QObject::tr("%1 · %2 · 区分大小写:%3 · 整词:%4")
        .arg(displayName)
        .arg(opts.regexMode ? QObject::tr("正则表达式") : QObject::tr("字面值"))
        .arg(opts.caseSensitive ? QObject::tr("是") : QObject::tr("否"))
        .arg(opts.wholeWord ? QObject::tr("是") : QObject::tr("否"));

    lastTaskId_ = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch,
        AsyncTaskPriority::High,
        title,
        desc,
        /* workerFn = */ [this, virtualSize, pattern, opts]
        (qint64 taskId, AsyncCancelPtr cancel,
         std::function<void(qint64,qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo&)> reportError)
        {
            Q_UNUSED(reportError);
            auto release = qScopeGuard([this] { releaseWorker(); });
            const auto isCancelled = [&]() {
                return (cancel && cancel->load(std::memory_order_acquire))
                    || cancel_.load(std::memory_order_relaxed);
            };
            Options taskOptions = opts;
            taskOptions.reportPhase = [taskId](
                const QString &phase, int index, int count,
                qint64 done, qint64 total) {
                AsyncTaskManager::instance().updateTaskPhase(
                    taskId, phase, index, count, done, total);
            };
            const bool ok = scanAllBounded(
                virtualSize, pattern, taskOptions, isCancelled,
                [this, reportProgress, virtualSize](qint64 done) {
                    reportProgress(done, virtualSize);
                    QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                                              Q_ARG(qint64, done), Q_ARG(qint64, virtualSize));
                },
                [this](const QVector<LargeFileSearchMatch> &batch) {
                    QMetaObject::invokeMethod(
                        this, "partialBatch", Qt::QueuedConnection,
                        Q_ARG(QVector<LargeFileSearchMatch>, batch));
                    return true;
                });
            if (!ok) {
                QMetaObject::invokeMethod(
                    this, "failed", Qt::QueuedConnection,
                    Q_ARG(QString, storageFailed_
                        ? QObject::tr("搜索结果临时存储写入失败。")
                        : (!scanError_.isEmpty() ? scanError_
                                                : QObject::tr("已取消。"))));
                return;
            }
            std::atomic_thread_fence(std::memory_order_release);
            const qint64 count = qint64(memMatches_.size()) + overrunCount_;
            reportProgress(virtualSize, virtualSize);
            QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                                      Q_ARG(qint64, virtualSize), Q_ARG(qint64, virtualSize));
            QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection,
                                      Q_ARG(qint64, count));
        },
        /* batchId = */ 0,
        /* categoryTag = */ QLatin1String("Search"),
        /* progressTotalInit = */ virtualSize
    );
    releaseOnSubmitFailure.dismiss();
    return lastTaskId_;
}

qint64 LargeFileSearch::findOneViaTaskManager(
    const QString &pattern, const Options &opts, qint64 fromByte,
    bool forward, bool wrap, const QString &fileDisplayName,
    QObject *callbackContext, OneMatchCallback callback)
{
    if (!mmapBase_) {
        if (callback)
            callback(false, {}, QObject::tr("数据源未就绪。"));
        return -1;
    }
    QString validationError;
    QByteArray literal;
    bool usePcre2 = false;
    if (!preparePattern(pattern, opts, &literal, &usePcre2,
                              &validationError)) {
        if (callback)
            callback(false, {}, validationError);
        return -1;
    }
    if (!reserveWorker()) {
        if (callback)
            callback(false, {}, QObject::tr("搜索任务正在运行。"));
        return -1;
    }
    auto releaseOnSubmitFailure = qScopeGuard([this] { releaseWorker(); });
    cancel_.store(false, std::memory_order_release);
    const qint64 virtualSize = patchList_
        ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("大文件") : fileDisplayName;
    QPointer<QObject> guard(callbackContext);
    lastTaskId_ = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch, AsyncTaskPriority::Highest,
        forward ? QObject::tr("查找下一个: %1").arg(displayName)
                : QObject::tr("查找上一个: %1").arg(displayName),
        QObject::tr("定向扫描，找到一处即返回"),
        [this, virtualSize, pattern, opts, fromByte, forward, wrap,
         guard, callback = std::move(callback)]
        (qint64 taskId, AsyncCancelPtr cancel,
         std::function<void(qint64, qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo &)> reportError) mutable {
            auto release = qScopeGuard([this] { releaseWorker(); });
            const auto isCancelled = [this, cancel]() {
                return (cancel && cancel->load(std::memory_order_acquire))
                    || cancel_.load(std::memory_order_acquire);
            };
            LargeFileSearchMatch found;
            found.byteOffset = -1;
            QString error;
            Options taskOptions = opts;
            taskOptions.reportPhase = [taskId](
                const QString &phase, int index, int count,
                qint64 done, qint64 total) {
                AsyncTaskManager::instance().updateTaskPhase(
                    taskId, phase, index, count, done, total);
            };
            const bool ok = scanOneBounded(
                virtualSize, pattern, taskOptions, fromByte, forward, wrap,
                isCancelled,
                [reportProgress, virtualSize](qint64 done) {
                    reportProgress(qMin(done, virtualSize), virtualSize);
                },
                &found, &error);
            if (!ok && error.isEmpty() && isCancelled())
                error = QObject::tr("已取消。");
            if (!ok && !error.isEmpty() && error != QObject::tr("已取消。")) {
                AsyncErrorInfo taskError;
                taskError.message = error;
                reportError(taskError);
            }
            // The callback re-enables the navigation controls.  Release the
            // engine reservation before posting it so an immediate second F3
            // or button click cannot observe an enabled UI backed by a still-
            // busy search lane.
            releaseWorker();
            release.dismiss();
            if (!guard || !callback)
                return;
            const bool matched = ok && found.byteOffset >= 0;
            QMetaObject::invokeMethod(
                guard, [callback = std::move(callback), matched,
                        found = std::move(found), error = std::move(error)]() mutable {
                    callback(matched, std::move(found), std::move(error));
                }, Qt::QueuedConnection);
        },
        0, QLatin1String("SearchNavigation"), virtualSize);
    releaseOnSubmitFailure.dismiss();
    return lastTaskId_;
}

// ============================================================================
//  findNext / findPrev
// ============================================================================
qint64 LargeFileSearch::findNext(qint64 fromByte, LargeFileSearchMatch* outMatch) const {
    // Resident prefix is bounded; the spool starts from the nearest sparse
    // checkpoint so F3 latency does not grow linearly with total result count.
    for (const auto& m : memMatches_) {
        if (m.byteOffset > fromByte) {
            if (outMatch) *outMatch = m;
            return m.byteOffset;
        }
    }
    if (overrunCount_ > 0 && QFile::exists(overrunFile_)) {
        QFile f(overrunFile_);
        if (f.open(QIODevice::ReadOnly)) {
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            quint32 version = 0;
            if (!readOverrunHeader(s, &version, nullptr))
                return -1;
            const auto checkpoint = std::upper_bound(
                overrunCheckpointBytes_.cbegin(), overrunCheckpointBytes_.cend(),
                fromByte);
            if (checkpoint != overrunCheckpointBytes_.cbegin()) {
                const qsizetype index = std::distance(overrunCheckpointBytes_.cbegin(),
                                                      checkpoint - 1);
                if (index < overrunCheckpoints_.size())
                    f.seek(overrunCheckpoints_.at(index).second);
            }
            while (!s.atEnd()) {
                LargeFileSearchMatch match;
                if (!readOverrunRecord(s, version, &match))
                    break;
                if (match.byteOffset > fromByte) {
                    if (outMatch) *outMatch = match;
                    return match.byteOffset;
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
            QDataStream s(&f);
            s.setByteOrder(QDataStream::LittleEndian);
            quint32 version = 0;
            if (!readOverrunHeader(s, &version, nullptr))
                return best;
            const auto checkpoint = std::lower_bound(
                overrunCheckpointBytes_.cbegin(), overrunCheckpointBytes_.cend(),
                fromByte);
            if (checkpoint == overrunCheckpointBytes_.cbegin())
                return best;
            const qsizetype checkpointIndex = std::distance(
                overrunCheckpointBytes_.cbegin(), checkpoint - 1);
            if (checkpointIndex < overrunCheckpoints_.size())
                f.seek(overrunCheckpoints_.at(checkpointIndex).second);
            static thread_local LargeFileSearchMatch tmp;
            while (!s.atEnd()) {
                LargeFileSearchMatch match;
                if (!readOverrunRecord(s, version, &match))
                    break;
                if (match.byteOffset >= fromByte)
                    break;
                if (match.byteOffset < fromByte && match.byteOffset > best) {
                    best = match.byteOffset;
                    tmp = std::move(match);
                    bestM = &tmp;
                }
            }
        }
    }
    if (best >= 0 && bestM && outMatch) *outMatch = *bestM;
    return best;
}

// ============================================================================
//  replaceAll（§5.6 步骤 3，同步执行；进度分批，用户可见事务始终为 1 UndoGroup）
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
    storageFailed_ = false;
    scanError_.clear();
    closeOverrunFile();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunCheckpointBytes_.clear();
    overrunGroupedCount_ = 0;
    overrunLastLine_ = std::numeric_limits<qint32>::min();
    overrunLastContext_ = std::numeric_limits<qint64>::min();
    if (!overrunFile_.isEmpty()) { QFile::remove(overrunFile_); overrunFile_.clear(); }

    bool replacementEncodingOk = true;
    QByteArray replBytes = opts.byteContextMode
        ? replacement.toUtf8()
        : encodeTextFragment(replacement, opts.textEncoding,
                             &replacementEncodingOk);
    if (!opts.regexMode && !replacementEncodingOk) {
        if (errMsg) {
            *errMsg = QObject::tr(
                "替换文本包含 %1 无法表示的字符；尚未修改文件。")
                          .arg(encodingName(opts.textEncoding));
        }
        return false;
    }

    // Step 1 简化：直接收集所有匹配（分批收集，但这里内存直接做；溢出则失败——replaceAll
    // 必须有完整匹配列表才能正确按 byteOffset 从大到小替换）
    QVector<LargeFileSearchMatch> allMatches;
    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    bool resultLimitExceeded = false;
    bool zeroLengthReplacementUnsupported = false;
    const bool scanOk = scanAllBounded(
        virtualSize, pattern, opts,
        [this] { return cancel_.load(std::memory_order_relaxed); },
        {},
        [&](const QVector<LargeFileSearchMatch> &batch) {
            if (std::any_of(batch.cbegin(), batch.cend(),
                            [](const LargeFileSearchMatch &match) {
                                return match.length == 0;
                            })) {
                zeroLengthReplacementUnsupported = true;
                return false;
            }
            if (qint64(allMatches.size()) + batch.size() > maxMemMatches_) {
                resultLimitExceeded = true;
                return false;
            }
            allMatches += batch;
            return true;
        });
    if (!scanOk) {
        if (errMsg) {
            if (zeroLengthReplacementUnsupported) {
                *errMsg = QObject::tr(
                    "批量替换暂不接受零长度匹配；请让表达式至少消费一个字符。");
            } else if (resultLimitExceeded) {
                *errMsg = QObject::tr(
                    "匹配超过安全批量替换上限（%1 处）。请缩小查找范围后分批替换。")
                              .arg(maxMemMatches_);
            } else if (!scanError_.isEmpty()) {
                *errMsg = scanError_;
            } else {
                *errMsg = QObject::tr("取消");
            }
        }
        return false;
    }

    qint64 total = allMatches.size();
    if (total == 0) {
        if (cb.progressBatch) cb.progressBatch(0, 0);
        if (outReplacedCount) *outReplacedCount = 0;
        return true;
    }

    // Offsets and capture expansions are defined against the same immutable
    // pre-replacement snapshot. Sort first so the replacement byte vector
    // remains index-aligned while patches are later applied back-to-front.
    std::sort(allMatches.begin(), allMatches.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                  return a.byteOffset > b.byteOffset;
              });
    QVector<QByteArray> expandedReplacements;
    const auto wipeExpansions = qScopeGuard([&] {
        if (!replBytes.isEmpty())
            replBytes.fill(0);
        for (QByteArray &value : expandedReplacements) {
            if (!value.isEmpty())
                value.fill(0);
        }
    });
    if (opts.regexMode) {
        const bool encodedByteRegex = !opts.byteContextMode
            && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
        Pcre2Regex compiled;
        QString expansionError;
        if (encodedByteRegex) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "原始编码文本视图为只读；请转换并另存为 UTF-8 后替换。");
            }
            return false;
        }
        if (!compiled.compile(pattern, false, opts.caseSensitive,
                              opts.wholeWord, &expansionError)) {
            if (errMsg) *errMsg = expansionError;
            return false;
        }

        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_replace_snapshot_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        if (patchList_ && patchList_->isModified()) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法创建正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            constexpr qint64 SnapshotChunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += SnapshotChunk) {
                if (cancel_.load()) {
                    if (errMsg) *errMsg = QObject::tr("取消");
                    return false;
                }
                const qint64 end = qMin(virtualSize, start + SnapshotChunk);
                QByteArray chunk = readVirtualBytes(start, end);
                const bool writeOk = chunk.size() == end - start
                    && snapshot.write(chunk) == chunk.size();
                if (!chunk.isEmpty())
                    chunk.fill(0);
                if (!writeOk) {
                    if (errMsg) {
                        *errMsg = QObject::tr("无法写入正则替换快照：%1")
                                      .arg(snapshot.errorString());
                    }
                    return false;
                }
            }
            if (!snapshot.flush()) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法刷新正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                if (errMsg) {
                    *errMsg = QObject::tr("无法映射正则替换快照：%1")
                                  .arg(snapshot.errorString());
                }
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmapSnapshot = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });
        expandedReplacements.reserve(allMatches.size());
        for (const LargeFileSearchMatch &match : std::as_const(allMatches)) {
            QByteArray expanded;
            const bool expandedOk = compiled.expandReplacementAt(
                subject, virtualSize, match.byteOffset, match.length,
                replacement, &expanded, &expansionError);
            if (!expandedOk) {
                if (errMsg) *errMsg = expansionError;
                return false;
            }
            expandedReplacements.append(std::move(expanded));
        }
    }

    // Step 2：压力检测（估算替换导致的内存增量 = Σ|replBytes.size() - m.length|）
    qint64 estSize = 0;
    qint64 undoBytes = patches.l1Bytes();
    for (qsizetype index = 0; index < allMatches.size(); ++index) {
        const qsizetype replacementSize = opts.regexMode
            ? expandedReplacements.at(index).size() : replBytes.size();
        estSize += qAbs<qint64>(replacementSize - allMatches.at(index).length);
        const qint64 recordBytes = qint64(allMatches.at(index).length)
            + replacementSize + 128;
        if (recordBytes < 0 || undoBytes > patches.l1Limit() - recordBytes) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "全部替换超过当前可撤销内存预算；尚未修改文件。请缩小范围后分批执行。");
            }
            return false;
        }
        undoBytes += recordBytes;
    }
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

    // Step 3：一次 Replace All = 一个 UndoGroup。BATCH 只控制进度/取消
    // 检查频率，不能改变用户可见的撤销边界。预算检查保证该 group 不会
    // spill 到会清空 undo history 的 L2 cold storage。
    const int BATCH = DynamicThresholds::instance().REPLACE_BATCH_SIZE;
    qint64 done = 0;
    qint64 applied = 0;
    bool groupOpen = true;
    patches.beginUndoGroup();
    const auto rollbackUncommitted = qScopeGuard([&] {
        if (!groupOpen)
            return;
        patches.endUndoGroup();
        if (applied > 0) {
            QString rollbackError;
            if (!patches.undo(&rollbackError) && errMsg) {
                *errMsg += QObject::tr("；自动回滚失败：%1").arg(rollbackError);
            }
        }
    });
    for (qint64 bStart = 0; bStart < total; bStart += BATCH) {
        if (cancel_.load()) { if (errMsg) *errMsg = QStringLiteral("取消"); return false; }
        qint64 bEnd = qMin<qint64>(bStart + BATCH, total);
        for (qint64 i = bStart; i < bEnd; ++i) {
            const LargeFileSearchMatch& m = allMatches[int(i)];
            QByteArray oldBytes;
            if (mmapBase_) {
                oldBytes = readVirtualBytes(m.byteOffset, m.byteOffset + m.length);
            }
            if (oldBytes.size() < m.length) oldBytes.resize(m.length, '\0');
            const QByteArray &replacementBytes = opts.regexMode
                ? expandedReplacements.at(int(i)) : replBytes;
            QString localErr;
            bool ok = patches.replaceWithSnapshots(m.byteOffset, m.length,
                                                   replacementBytes, oldBytes,
                                                   &localErr);
            if (!ok) {
                if (errMsg) *errMsg = QObject::tr("第 %1 处替换失败：%2").arg(i).arg(localErr);
                return false;
            }
            ++applied;
        }
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
    patches.endUndoGroup();
    groupOpen = false;
    if (outReplacedCount) *outReplacedCount = done;
    return true;
}
