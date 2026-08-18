#include "LargeFileSearch.h"
#include "LineIndex.h"
#include "PatchList.h"
#include "DynamicThresholds.h"
#include "AsyncTaskManager.h"
#include "Pcre2Regex.h"

#include <QFile>
#include <QDir>
#include <QDataStream>
#include <QtConcurrent>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QDateTime>
#include <QUuid>
#include <cstring>
#include <algorithm>
#include <utility>

namespace {

constexpr quint32 OVR_MAGIC   = 0x564F534Du; // 'MSOV' little endian stored
constexpr quint32 OVR_VERSION = 2;
constexpr int     OVERRUN_CTX_MAX = 512;
constexpr qint64  OVERRUN_FILE_LIMIT = Q_INT64_C(1024) * 1024 * 1024;

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

int hexNibble(QChar ch)
{
    if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
        return ch.unicode() - QLatin1Char('0').unicode();
    if (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
        return 10 + ch.unicode() - QLatin1Char('a').unicode();
    if (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))
        return 10 + ch.unicode() - QLatin1Char('A').unicode();
    return -1;
}

bool prepareSearchPattern(const QString &pattern, const LargeFileSearch::Options &opts,
                          QByteArray *literal, bool *usePcre2, QString *error)
{
    if (opts.hexBytesMode) {
        *usePcre2 = false;
        return LargeFileSearch::parseHexBytePattern(pattern, literal, error);
    }
    // Binary documents are not guaranteed to be valid UTF-8.  Their default
    // (non-regex) query is therefore a raw-byte literal search, with the KMP
    // path providing ASCII-only case folding when requested.  Routing that
    // state through PCRE2 made the unchecked "regular expression" option lie:
    // arbitrary binary files failed UTF-8 validation before a literal search
    // could start.  Text documents retain PCRE2's Unicode case folding and
    // Unicode word-boundary semantics.
    const bool binaryLiteral = opts.byteContextMode
        && !opts.regexMode && !opts.wholeWord;
    *usePcre2 = opts.regexMode || opts.wholeWord
        || (!opts.caseSensitive && !binaryLiteral);
    if (!*usePcre2)
        *literal = pattern.toUtf8();
    if (pattern.isEmpty() || (!*usePcre2 && literal->isEmpty())) {
        if (error)
            *error = QStringLiteral("查找文本为空。");
        return false;
    }
    return true;
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

bool LargeFileSearch::parseHexBytePattern(const QString &pattern,
                                          QByteArray *bytes,
                                          QString *error)
{
    if (!bytes)
        return false;
    bytes->clear();
    QString digits;
    digits.reserve(pattern.size());
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar ch = pattern.at(i);
        if (ch.isSpace() || ch == QLatin1Char('_') || ch == QLatin1Char('-')
            || ch == QLatin1Char(',')) {
            continue;
        }
        if (ch == QLatin1Char('0') && i + 1 < pattern.size()
            && (pattern.at(i + 1) == QLatin1Char('x')
                || pattern.at(i + 1) == QLatin1Char('X'))) {
            ++i;
            continue;
        }
        if (hexNibble(ch) < 0) {
            if (error) {
                *error = QObject::tr("十六进制字节串第 %1 个字符“%2”无效。")
                             .arg(i + 1).arg(ch);
            }
            return false;
        }
        digits.append(ch);
        if (digits.size() > 2 * 1024 * 1024) {
            if (error)
                *error = QObject::tr("十六进制搜索模式过长（最多 1 MiB 字节）。");
            return false;
        }
    }
    if (digits.isEmpty()) {
        if (error) *error = QObject::tr("请输入至少一个十六进制字节。");
        return false;
    }
    if ((digits.size() % 2) != 0) {
        if (error)
            *error = QObject::tr("十六进制字节必须由成对数字组成，例如 DE AD BE EF。");
        return false;
    }
    bytes->reserve(digits.size() / 2);
    for (qsizetype i = 0; i < digits.size(); i += 2) {
        bytes->append(char((hexNibble(digits.at(i)) << 4)
                           | hexNibble(digits.at(i + 1))));
    }
    return true;
}

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
    lineIndexCompleteAtSnapshot_ = lineIndex && !lineIndex->isIncomplete();
    patchList_ = patchList;
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
}

void LargeFileSearch::requestCancel() {
    cancel_.store(true, std::memory_order_relaxed);
}

// ============================================================================
//  溢出文件操作
// ============================================================================
bool LargeFileSearch::prepareOverrunFile(QString* errMsg) {
    if (overrunFp_) return true;
    QString fn = QStringLiteral("matches_%1_%2_%3.bin")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
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
    const auto sameDisplayGroup = [](const LargeFileSearchMatch &left,
                                     const LargeFileSearchMatch &right) {
        return left.line == right.line
               && left.contextByteOffset == right.contextByteOffset;
    };
    const auto writeOverrun = [this](const LargeFileSearchMatch &match) -> bool {
        if (!overrunFp_ && !prepareOverrunFile(nullptr)) {
            storageFailed_ = true;
            return false;
        }
        QDataStream stream(overrunFp_);
        stream.setByteOrder(QDataStream::LittleEndian);
        if (overrunFp_->pos() >= OVERRUN_FILE_LIMIT) {
            scanError_ = QObject::tr(
                "搜索结果已达到 1 GiB 临时存储上限；已保留此前结果，请缩小范围后重试。");
            return false;
        }
        if (match.line != overrunLastLine_
            || match.contextByteOffset != overrunLastContext_) {
            if ((overrunGroupedCount_ % 512) == 0) {
                overrunCheckpoints_.append(qMakePair(overrunGroupedCount_,
                                                      overrunFp_->pos()));
                overrunCheckpointBytes_.append(match.byteOffset);
            }
            ++overrunGroupedCount_;
            overrunLastLine_ = match.line;
            overrunLastContext_ = match.contextByteOffset;
        }
        stream << quint64(match.byteOffset) << qint32(match.length)
               << qint32(match.line) << qint32(match.col)
               << quint64(match.contextByteOffset);
        QByteArray context = match.lineContext;
        if (context.size() > OVERRUN_CTX_MAX)
            context.truncate(OVERRUN_CTX_MAX);
        stream << context;
        if (stream.status() != QDataStream::Ok) {
            storageFailed_ = true;
            return false;
        }
        ++overrunCount_;
        return true;
    };
    for (const LargeFileSearchMatch& m : batch) {
        if (!overrunFp_ && overrunCount_ == 0 && qint64(memMatches_.size()) < cap) {
            memMatches_.append(m);
        } else {
            if (!overrunFp_ && overrunCount_ == 0 && !memMatches_.isEmpty()
                && sameDisplayGroup(memMatches_.constLast(), m)) {
                int tail = memMatches_.size() - 1;
                while (tail > 0 && sameDisplayGroup(memMatches_.at(tail - 1), m))
                    --tail;
                const QVector<LargeFileSearchMatch> spilled = memMatches_.mid(tail);
                memMatches_.erase(memMatches_.begin() + tail, memMatches_.end());
                for (const LargeFileSearchMatch &previous : spilled) {
                    if (!writeOverrun(previous))
                        return;
                }
            }
            if (!writeOverrun(m))
                return;
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
    const qint64 originalMatch = patchList_
        ? patchList_->virtToOrig(m.byteOffset) : m.byteOffset;
    const bool indexed = lineIndex_ && lineIndexCompleteAtSnapshot_;
    if (!indexed) {
        // Search correctness and first results must not wait for the full-file
        // line index. Build an exact, bounded context around the authoritative
        // virtual byte range; the row is displayed by byte offset until line
        // decoration becomes available.
        constexpr qint64 lookBehind = 64 * 1024;
        constexpr qint64 lookAhead = 4 * 1024;
        constexpr qint64 prefixBytes = 64;
        const qint64 virtualSize = patchList_
            ? patchList_->virtualSize(mmapSize_) : mmapSize_;
        const qint64 scanStart = qMax<qint64>(0, m.byteOffset - lookBehind);
        const qint64 scanEnd = qMin(virtualSize,
            m.byteOffset + qint64(m.length) + lookAhead);
        const QByteArray raw = readVirtualBytes(scanStart, scanEnd);
        const int relative = int(qBound<qint64>(0, m.byteOffset - scanStart,
                                                raw.size()));
        const int matchEnd = qMin(raw.size(), relative + int(m.length));
        const int previousLf = raw.lastIndexOf('\n', relative - 1);
        const int previousCr = raw.lastIndexOf('\r', relative - 1);
        const int lineStart = qMax(previousLf, previousCr) + 1;
        const int nextLf = raw.indexOf('\n', matchEnd);
        const int nextCr = raw.indexOf('\r', matchEnd);
        int lineEnd = raw.size();
        if (nextLf >= 0) lineEnd = qMin(lineEnd, nextLf);
        if (nextCr >= 0) lineEnd = qMin(lineEnd, nextCr);
        int contextStart = qMax(lineStart, relative - int(prefixBytes));
        int contextEnd = qMin(lineEnd, contextStart + OVERRUN_CTX_MAX);
        if (contextEnd < matchEnd) {
            contextEnd = qMin(lineEnd, matchEnd + int(prefixBytes));
            contextStart = qMax(lineStart, contextEnd - OVERRUN_CTX_MAX);
        }
        while (contextStart < raw.size()
               && (static_cast<unsigned char>(raw.at(contextStart)) & 0xc0) == 0x80) {
            ++contextStart;
        }
        m.line = -1;
        m.col = qint32(qBound<qint64>(qint64(0),
                                      m.byteOffset - (scanStart + lineStart),
                                      qint64(INT32_MAX)));
        m.contextByteOffset = scanStart + contextStart;
        m.lineContext = raw.mid(contextStart, contextEnd - contextStart);
        return;
    }
    int line = 0;
    qint64 originalColumn = 0;
    lineIndex_->byteToLineCol(originalMatch, &line, &originalColumn);
    Q_UNUSED(originalColumn);
    const qint64 originalLineStart = lineIndex_->lineToByteOffset(line);
    const qint64 originalLineEnd = lineIndex_->lineToByteOffset(line + 1);
    const qint64 lineStart = patchList_
        ? patchList_->origToVirtForLineStart(originalLineStart) : originalLineStart;
    const qint64 lineEnd = patchList_
        ? patchList_->origToVirtForLineStart(originalLineEnd) : originalLineEnd;
    m.line = line;
    m.col = qint32(qBound<qint64>(qint64(0), m.byteOffset - lineStart,
                                  qint64(INT32_MAX)));
    const qint64 matchEnd = m.byteOffset + m.length;
    // Keep a compact prefix so the highlighted token is visible without
    // horizontal scrolling in a typical dock width, while preserving ample
    // trailing log context for diagnosis.
    constexpr qint64 prefixBytes = 64;
    qint64 desiredStart = qMax(lineStart, m.byteOffset - prefixBytes);
    qint64 end = qMin(lineEnd, desiredStart + OVERRUN_CTX_MAX);
    if (end < matchEnd) {
        end = qMin(lineEnd, matchEnd + prefixBytes);
        desiredStart = qMax(lineStart, end - OVERRUN_CTX_MAX);
    }

    // Include at most 3 bytes before the desired start, then advance over UTF-8
    // continuation bytes. This prevents a centered context from beginning in the
    // middle of a code point without ever reading an unbounded long line.
    const qint64 rawStart = qMax(lineStart, desiredStart - 3);
    QByteArray raw = readVirtualBytes(rawStart, end);
    int trim = int(desiredStart - rawStart);
    while (trim < raw.size()
           && (static_cast<unsigned char>(raw[trim]) & 0xc0) == 0x80) {
        ++trim;
    }
    qint64 start = rawStart + trim;
    QByteArray lineBytes = raw.mid(trim);
    // 去掉行尾 \r\n / \n
    while (end >= lineEnd && !lineBytes.isEmpty()
           && (lineBytes.endsWith('\n') || lineBytes.endsWith('\r')))
        lineBytes.chop(1);
    if (lineBytes.size() > OVERRUN_CTX_MAX) lineBytes.truncate(OVERRUN_CTX_MAX);
    m.contextByteOffset = start;
    m.lineContext = lineBytes;
}

void LargeFileSearch::fillByteContext(LargeFileSearchMatch& m) const
{
    constexpr qint64 BytesPerRow = 16;
    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    const qint64 rowStart = (m.byteOffset / BytesPerRow) * BytesPerRow;
    const qint64 rowEnd = qMin(virtualSize, rowStart + BytesPerRow);
    QByteArray preview = readVirtualBytes(rowStart, rowEnd);
    for (char &value : preview) {
        const uchar byte = uchar(value);
        if (byte < 0x20 || byte > 0x7e)
            value = '.';
    }
    m.line = qint32(qMin<qint64>(rowStart / BytesPerRow, INT32_MAX));
    m.col = qint32(m.byteOffset - rowStart);
    m.contextByteOffset = rowStart;
    m.lineContext = std::move(preview);
}

bool LargeFileSearch::scanAllBounded(
    qint64 virtualSize, const QString &pattern, const Options &opts,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<bool(const QVector<LargeFileSearchMatch>&)> &reportBatch)
{
    scanError_.clear();
    const auto closeSpool = qScopeGuard([this] { closeOverrunFile(); });
    constexpr qsizetype PublishBatchSize = 1000;
    QVector<LargeFileSearchMatch> pending;
    pending.reserve(PublishBatchSize);
    const auto publish = [&]() -> bool {
        if (pending.isEmpty())
            return true;
        appendMatches(pending);
        if (storageFailed_ || !scanError_.isEmpty())
            return false;
        if (reportBatch && !reportBatch(pending))
            return false;
        pending.clear();
        return true;
    };

    QByteArray literalBytes;
    bool usePcre2 = false;
    if (!prepareSearchPattern(pattern, opts, &literalBytes, &usePcre2, &scanError_))
        return false;

    if (!usePcre2) {
        if (literalBytes.isEmpty())
            return true;
        const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
        const QVector<int> failure = kmpBuildFail(literalBytes, caseSensitive);
        const int needleSize = literalBytes.size();
        int matched = 0;
        constexpr qint64 ReadChunk = Q_INT64_C(16) * 1024 * 1024;
        for (qint64 start = 0; start < virtualSize; ) {
            if (isCancelled && isCancelled())
                return false;
            const qint64 end = qMin(virtualSize, start + ReadChunk);
            const QByteArray bytes = readVirtualBytes(start, end);
            if (bytes.isEmpty() && end > start)
                break;
            for (qsizetype index = 0; index < bytes.size(); ++index) {
                const uchar value = foldCaseAscii(uchar(bytes.at(index)), caseSensitive);
                while (matched > 0
                       && foldCaseAscii(uchar(literalBytes.at(matched)), caseSensitive)
                              != value) {
                    matched = failure.at(matched - 1);
                }
                if (foldCaseAscii(uchar(literalBytes.at(matched)), caseSensitive) == value)
                    ++matched;
                if (matched == needleSize) {
                    LargeFileSearchMatch match;
                    match.byteOffset = start + index + 1 - needleSize;
                    match.length = needleSize;
                    if (opts.byteContextMode)
                        fillByteContext(match);
                    else
                        fillLineContext(match);
                    pending.append(std::move(match));
                    if (pending.size() >= PublishBatchSize && !publish())
                        return false;
                    matched = failure.at(matched - 1);
                }
            }
            start = end;
            if (reportProgress)
                reportProgress(start);
        }
    } else {
        Pcre2Regex compiled;
        const bool binaryWholeWord = opts.byteContextMode
            && !opts.regexMode && opts.wholeWord;
        if (!compiled.compile(pattern, !opts.regexMode, opts.caseSensitive,
                              opts.wholeWord, &scanError_,
                              !binaryWholeWord)) {
            return false;
        }

        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_snapshot_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        const bool needsSnapshot = patchList_ && patchList_->isModified();
        if (needsSnapshot) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                scanError_ = QObject::tr("无法创建正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            constexpr qint64 SnapshotChunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += SnapshotChunk) {
                if (isCancelled && isCancelled())
                    return false;
                const qint64 end = qMin(virtualSize, start + SnapshotChunk);
                const QByteArray bytes = readVirtualBytes(start, end);
                if (bytes.size() != end - start || snapshot.write(bytes) != bytes.size()) {
                    scanError_ = QObject::tr("无法写入正则搜索快照：%1")
                                     .arg(snapshot.errorString());
                    return false;
                }
            }
            if (!snapshot.flush()) {
                scanError_ = QObject::tr("无法刷新正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                scanError_ = QObject::tr("无法映射正则搜索快照：%1")
                                 .arg(snapshot.errorString());
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmapSnapshot = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });

        const Pcre2Regex::ScanResult result = compiled.scan(
            subject, virtualSize, isCancelled, reportProgress,
            [&](qint64 offset, qint64 length) {
                LargeFileSearchMatch match;
                match.byteOffset = offset;
                match.length = qint32(length);
                if (opts.byteContextMode)
                    fillByteContext(match);
                else
                    fillLineContext(match);
                pending.append(std::move(match));
                return pending.size() < PublishBatchSize || publish();
            }, &scanError_);
        if (result != Pcre2Regex::ScanResult::Complete)
            return false;
    }

    const bool ok = publish();
    return ok && !storageFailed_ && scanError_.isEmpty();
}

// ============================================================================
//  findAll（异步）
// ============================================================================
void LargeFileSearch::findAll(const QString& pattern, const Options& opts) {
    if (!mmapBase_ || (!lineIndex_ && !opts.byteContextMode)) {
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
    if (!prepareSearchPattern(pattern, opts, &literalBytes,
                              &usePcre2, &patternError)) {
        emit failed(patternError);
        return;
    }

    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    auto worker = [this, virtualSize, pattern, opts]() {
        const auto isCancelled = [this]() {
            return cancel_.load(std::memory_order_relaxed);
        };
        const bool ok = scanAllBounded(
            virtualSize, pattern, opts, isCancelled,
            [this, virtualSize](qint64 done) { emit progress(done, virtualSize); },
            [this](const QVector<LargeFileSearchMatch> &batch) {
                emit partialBatch(batch);
                return true;
            });
        if (!ok) {
            emit failed(storageFailed_ ? QStringLiteral("搜索结果临时存储写入失败。")
                                       : (!scanError_.isEmpty() ? scanError_
                                                               : QStringLiteral("已取消。")));
            return;
        }
        std::atomic_thread_fence(std::memory_order_release);
        emit progress(virtualSize, virtualSize);
        emit finished(qint64(memMatches_.size()) + overrunCount_);
    };

    QtConcurrent::run(worker);
}

// ============================================================================
//  findAllViaTaskManager：AsyncTaskManager 统一派发，AsyncTaskPanel 可见/可取消
// ============================================================================
qint64 LargeFileSearch::findAllViaTaskManager(const QString& pattern, const Options& opts,
                                              const QString& fileDisplayName)
{
    if (!mmapBase_ || (!lineIndex_ && !opts.byteContextMode)) {
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
    if (!prepareSearchPattern(pattern, opts, &literalBytes,
                              &usePcre2, &patternError)) {
        emit failed(patternError);
        return -1;
    }

    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;

    const QString displayName = fileDisplayName.isEmpty()
        ? QObject::tr("(未命名大文件)")
        : fileDisplayName;
    const QString title = QObject::tr("搜索大文件: %1").arg(displayName);
    const QString desc  = QObject::tr("文件: %1\n模式: %2\n大小写敏感: %3\n整词匹配: %4\n\n可在查找窗口或「后台任务」面板取消。进度按已扫描字节/总虚拟大小计算。")
        .arg(displayName)
        .arg(opts.regexMode ? QObject::tr("正则表达式") : QObject::tr("字面值"))
        .arg(opts.caseSensitive ? QObject::tr("是") : QObject::tr("否"))
        .arg(opts.wholeWord ? QObject::tr("是") : QObject::tr("否"));

    // 捕获 LargeFileSearch 原始指针为本地变量，worker 中使用需谨慎：
    //  我们通过信号的 QueuedConnection 把 finished/failed/progress 回主线程；
    //  若对象在中途被析构，LargeFileSearch::requestCancel()/cancel_ 原子标志保证安全退出，
    //  但要避免访问成员变量（除非对象仍存在）。这里我们用 QPointer + QMetaObject::invokeMethod
    //  做主线程安全回调；scanAllBounded/readVirtualBytes 本身只读数据源。
    QPointer<LargeFileSearch> that(this);
    lastTaskId_ = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileSearch,
        AsyncTaskPriority::High,
        title,
        desc,
        /* workerFn = */ [that, virtualSize, pattern, opts]
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

            that->cancel_.store(false, std::memory_order_release);
            const auto isCancelled = [&]() {
                return (cancel && cancel->load(std::memory_order_acquire))
                    || that->cancel_.load(std::memory_order_relaxed);
            };
            const bool ok = that->scanAllBounded(
                virtualSize, pattern, opts, isCancelled,
                [that, reportProgress, virtualSize](qint64 done) {
                    reportProgress(done, virtualSize);
                    QMetaObject::invokeMethod(that, "progress", Qt::QueuedConnection,
                                              Q_ARG(qint64, done), Q_ARG(qint64, virtualSize));
                },
                [that](const QVector<LargeFileSearchMatch> &batch) {
                    QMetaObject::invokeMethod(
                        that, "partialBatch", Qt::QueuedConnection,
                        Q_ARG(QVector<LargeFileSearchMatch>, batch));
                    return true;
                });
            if (!ok) {
                QMetaObject::invokeMethod(
                    that, "failed", Qt::QueuedConnection,
                    Q_ARG(QString, that->storageFailed_
                        ? QObject::tr("搜索结果临时存储写入失败。")
                        : (!that->scanError_.isEmpty() ? that->scanError_
                                                       : QObject::tr("已取消。"))));
                return;
            }
            std::atomic_thread_fence(std::memory_order_release);
            const qint64 count = qint64(that->memMatches_.size()) + that->overrunCount_;
            reportProgress(virtualSize, virtualSize);
            QMetaObject::invokeMethod(that, "progress", Qt::QueuedConnection,
                                      Q_ARG(qint64, virtualSize), Q_ARG(qint64, virtualSize));
            QMetaObject::invokeMethod(that, "finished", Qt::QueuedConnection,
                                      Q_ARG(qint64, count));
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

    QByteArray replBytes = replacement.toUtf8();

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
        Pcre2Regex compiled;
        QString expansionError;
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
            if (!compiled.expandReplacementAt(
                    subject, virtualSize, match.byteOffset, match.length,
                    replacement, &expanded, &expansionError)) {
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
