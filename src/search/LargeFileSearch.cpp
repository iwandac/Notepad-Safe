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

QString escapeRawRegexLiteral(const QString &input)
{
    static const QString metacharacters = QStringLiteral("\\.^$|()[]{}*+?");
    QString escaped;
    escaped.reserve(input.size() * 2);
    for (const QChar ch : input) {
        if (metacharacters.contains(ch))
            escaped.append(QLatin1Char('\\'));
        escaped.append(ch);
    }
    return escaped;
}

uint edgeCodePoint(const QString &text, bool first)
{
    if (text.isEmpty()) return 0;
    const int index = first ? 0 : text.size() - 1;
    const QChar edge = text.at(index);
    if (first && edge.isHighSurrogate() && text.size() > 1
        && text.at(1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(edge, text.at(1));
    }
    if (!first && edge.isLowSurrogate() && text.size() > 1
        && text.at(index - 1).isHighSurrogate()) {
        return QChar::surrogateToUcs4(text.at(index - 1), edge);
    }
    return edge.unicode();
}

bool needsRawWordBoundary(uint codePoint)
{
    switch (QChar::script(codePoint)) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
    case QChar::Script_Hangul:
    case QChar::Script_Bopomofo:
        return false;
    default:
        return codePoint == uint('_') || QChar::isLetterOrNumber(codePoint);
    }
}

QString rawSearchExpression(const QString &pattern,
                            const LargeFileSearch::Options &opts)
{
    QString expression = opts.regexMode ? pattern : escapeRawRegexLiteral(pattern);
    if (opts.wholeWord) {
        const bool leftBoundary = opts.regexMode
            || needsRawWordBoundary(edgeCodePoint(pattern, true));
        const bool rightBoundary = opts.regexMode
            || needsRawWordBoundary(edgeCodePoint(pattern, false));
        expression = (leftBoundary ? QStringLiteral("(?<!\\w)") : QString())
            + QStringLiteral("(?:") + expression + QStringLiteral(")")
            + (rightBoundary ? QStringLiteral("(?!\\w)") : QString());
    }
    return expression;
}

std::function<qint64(qint64)> makeEncodedBoundaryNormalizer(
    const uchar *subject, qint64 subjectSize, TextEncoding encoding,
    qint64 initialBoundary = 0)
{
    if (encoding == TextEncoding::Windows1252)
        return [](qint64 candidate) { return candidate; };

    auto cursor = std::make_shared<qint64>(initialBoundary);
    return [subject, subjectSize, encoding, cursor](qint64 candidate) {
        while (*cursor < candidate && *cursor < subjectSize) {
            // All supported DBCS encodings encode ASCII as one byte. Skip
            // long ASCII log runs eight bytes at a time before falling back
            // to the authoritative decoder for multibyte units.
            if (encoding != TextEncoding::Utf16LE
                && encoding != TextEncoding::Utf16BE) {
                constexpr quint64 HighBits = Q_UINT64_C(0x8080808080808080);
                while (*cursor + qint64(sizeof(quint64)) <= candidate) {
                    quint64 word = 0;
                    std::memcpy(&word, subject + *cursor, sizeof(word));
                    if (word & HighBits)
                        break;
                    *cursor += sizeof(word);
                }
                if (*cursor >= candidate)
                    break;
            }
            const qint64 next = EncodedRegexCompiler::nextUnitOffset(
                subject, subjectSize, *cursor, encoding);
            if (next <= *cursor)
                return subjectSize + 1;
            *cursor = next;
        }
        return *cursor;
    };
}

std::function<qint64(qint64)> makeByteCandidateFinder(
    const uchar *subject, qint64 subjectSize, const QByteArray &needle,
    bool caseSensitive, qint64 initialOffset,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    const std::function<qint64(qint64)> &normalizeStart,
    qint64 maxCandidateStartExclusive = (std::numeric_limits<qint64>::max)())
{
    if (needle.isEmpty())
        return {};
    int anchorIndex = 0;
    for (int index = 0; index < needle.size(); ++index) {
        if (uchar(needle.at(index)) != 0) {
            anchorIndex = index;
            break;
        }
    }
    const uchar anchor = uchar(needle.at(anchorIndex));
    const uchar foldedAnchor = foldCaseAscii(anchor, false);
    const uchar alternateAnchor = foldedAnchor >= 'a' && foldedAnchor <= 'z'
        ? uchar(foldedAnchor - ('a' - 'A')) : foldedAnchor;
    struct State {
        qint64 searchOffset = 0;
        qint64 lastReported = 0;
    };
    auto state = std::make_shared<State>();
    state->searchOffset = initialOffset + anchorIndex;
    state->lastReported = initialOffset;
    const qint64 candidateLimit = maxCandidateStartExclusive
            == (std::numeric_limits<qint64>::max)()
        ? subjectSize
        : qBound<qint64>(0, maxCandidateStartExclusive, subjectSize);
    const qint64 anchorSearchLimit = candidateLimit >= subjectSize - anchorIndex
        ? subjectSize : candidateLimit + anchorIndex;
    return [subject, subjectSize, needle, caseSensitive, anchorIndex,
            anchor, foldedAnchor, alternateAnchor, state, isCancelled,
            reportProgress, normalizeStart, candidateLimit,
            anchorSearchLimit](qint64 minimumOffset) {
        if (minimumOffset >= candidateLimit)
            return subjectSize + 1;
        state->searchOffset = qMax(
            state->searchOffset, minimumOffset + qint64(anchorIndex));
        constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
        constexpr qint64 ProgressStep = Q_INT64_C(1) * 1024 * 1024;
        while (state->searchOffset < anchorSearchLimit) {
            if (isCancelled && isCancelled())
                return qint64(-1);
            const qint64 chunkEnd = qMin(anchorSearchLimit,
                                         state->searchOffset + Chunk);
            const size_t available = size_t(chunkEnd - state->searchOffset);
            const void *first = std::memchr(subject + state->searchOffset,
                                            anchor, available);
            const void *alternate = nullptr;
            if (!caseSensitive && alternateAnchor != foldedAnchor) {
                alternate = std::memchr(subject + state->searchOffset,
                                        alternateAnchor, available);
                first = std::memchr(subject + state->searchOffset,
                                    foldedAnchor, available);
            }
            const auto *hit = static_cast<const uchar *>(first);
            const auto *alternateHit = static_cast<const uchar *>(alternate);
            if (!hit || (alternateHit && alternateHit < hit))
                hit = alternateHit;
            if (!hit) {
                state->searchOffset = chunkEnd;
                const qint64 examined = qMin(
                    candidateLimit,
                    qMax(minimumOffset, state->searchOffset - anchorIndex));
                if (reportProgress && examined > state->lastReported) {
                    state->lastReported = examined;
                    reportProgress(examined);
                }
                continue;
            }
            const qint64 hitOffset = hit - subject;
            state->searchOffset = hitOffset + 1;
            const qint64 candidate = hitOffset - anchorIndex;
            if (reportProgress
                && candidate >= state->lastReported + ProgressStep) {
                state->lastReported = qMin(candidate, candidateLimit);
                reportProgress(state->lastReported);
            }
            if (candidate < minimumOffset || candidate < 0
                || candidate >= candidateLimit
                || candidate > subjectSize - needle.size())
                continue;
            bool same = true;
            for (int index = 0; index < needle.size(); ++index) {
                if (foldCaseAscii(subject[candidate + index], caseSensitive)
                    != foldCaseAscii(uchar(needle.at(index)), caseSensitive)) {
                    same = false;
                    break;
                }
            }
            if (!same)
                continue;
            const qint64 normalized = normalizeStart
                ? normalizeStart(candidate) : candidate;
            if (normalized == candidate)
                return candidate;
        }
        return subjectSize + 1;
    };
}

std::function<qint64(qint64)> makeStartByteCandidateFinder(
    const uchar *subject, qint64 subjectSize,
    const Pcre2Regex::StartByteFilter &filter, qint64 initialOffset,
    bool validUtf8, const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    qint64 maxCandidateStartExclusive = (std::numeric_limits<qint64>::max)())
{
    if (filter.isEmpty())
        return {};
    struct State { qint64 offset = 0; qint64 lastReported = 0; };
    auto state = std::make_shared<State>();
    state->offset = initialOffset;
    state->lastReported = initialOffset;
    const qint64 candidateLimit = maxCandidateStartExclusive
            == (std::numeric_limits<qint64>::max)()
        ? subjectSize
        : qBound<qint64>(0, maxCandidateStartExclusive, subjectSize);
    return [subject, subjectSize, filter, validUtf8, state, isCancelled,
            reportProgress, candidateLimit](qint64 minimumOffset) {
        if (minimumOffset >= candidateLimit)
            return subjectSize + 1;
        state->offset = qMax(state->offset, minimumOffset);
        if (validUtf8) {
            while (state->offset < subjectSize
                   && (subject[state->offset] & 0xc0u) == 0x80u) {
                ++state->offset;
            }
        }
        constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
        constexpr quint64 HighBits = Q_UINT64_C(0x8080808080808080);
        constexpr qint64 ProgressStep = Q_INT64_C(1) * 1024 * 1024;
        while (state->offset < candidateLimit) {
            if (isCancelled && isCancelled())
                return qint64(-1);
            const qint64 end = qMin(candidateLimit, state->offset + Chunk);
            const uchar *best = nullptr;
            const size_t available = size_t(end - state->offset);
            for (char candidate : filter.asciiBytes) {
                const auto *hit = static_cast<const uchar *>(std::memchr(
                    subject + state->offset, uchar(candidate), available));
                if (hit && (!best || hit < best))
                    best = hit;
            }
            if (filter.anyNonAscii) {
                qint64 cursor = state->offset;
                const qint64 highLimit = best ? qint64(best - subject) : end;
                while (cursor + qint64(sizeof(quint64)) <= highLimit) {
                    quint64 word = 0;
                    std::memcpy(&word, subject + cursor, sizeof(word));
                    if (word & HighBits)
                        break;
                    cursor += sizeof(word);
                }
                while (cursor < highLimit && subject[cursor] < 0x80u)
                    ++cursor;
                if (cursor < highLimit)
                    best = subject + cursor;
            }
            if (best) {
                const qint64 result = best - subject;
                state->offset = result + 1;
                if (reportProgress
                    && result >= state->lastReported + ProgressStep) {
                    state->lastReported = result;
                    reportProgress(result);
                }
                return result;
            }
            state->offset = end;
            if (reportProgress && end > state->lastReported) {
                state->lastReported = end;
                reportProgress(end);
            }
        }
        return subjectSize + 1;
    };
}

QByteArray safeInvalidUtfAsciiLiteralFilter(
    const QString &pattern, const LargeFileSearch::Options &opts,
    bool invalidUtfText)
{
    if (!invalidUtfText || pattern.isEmpty())
        return {};
    static const QString regexMetacharacters =
        QStringLiteral("\\.^$|()[]{}*+?");
    QByteArray bytes;
    bytes.reserve(pattern.size());
    for (const QChar ch : pattern) {
        if (ch.unicode() > 0x7f)
            return {};
        if (opts.regexMode && regexMetacharacters.contains(ch))
            return {};
        // Unicode caseless matching gives ASCII K/S non-ASCII equivalents
        // (Kelvin sign and long s). Do not use an ASCII-only prefilter when it
        // could suppress those authoritative PCRE2 matches.
        if (!opts.caseSensitive
            && (ch == QLatin1Char('k') || ch == QLatin1Char('K')
                || ch == QLatin1Char('s') || ch == QLatin1Char('S'))) {
            return {};
        }
        bytes.append(char(ch.unicode()));
    }
    return bytes;
}

// A UTF PCRE2 scan has two real pieces of work: validating the mapped bytes
// before PCRE2_NO_UTF_CHECK is safe, then matching.  Both phases use source
// byte offsets, so forwarding them directly makes the task jump to 100% after
// validation and then move backwards when matching starts.  Keep one
// monotonic user-visible scale and reserve a bounded share for validation.
class RegexProgressPhases final
{
public:
    RegexProgressPhases(qint64 total, bool validatesUtf8,
                        std::function<void(qint64)> report)
        : total_(qMax<qint64>(0, total)),
          validationBudget_(validatesUtf8 ? total_ / 5 : 0),
          report_(std::move(report))
    {
    }

    std::function<void(qint64)> validationReporter()
    {
        if (validationBudget_ <= 0 || !report_)
            return {};
        return [this](qint64 done) {
            emitMonotonic(scale(done, 0, validationBudget_));
        };
    }

    std::function<void(qint64)> searchReporter()
    {
        if (!report_)
            return {};
        return [this](qint64 done) {
            emitMonotonic(scale(done, validationBudget_,
                                total_ - validationBudget_));
        };
    }

private:
    qint64 scale(qint64 done, qint64 base, qint64 span) const
    {
        if (total_ <= 0)
            return 0;
        const qint64 clamped = qBound<qint64>(0, done, total_);
        const long double fraction = static_cast<long double>(clamped)
            / static_cast<long double>(total_);
        return qBound<qint64>(0, base + qint64(fraction * span), total_);
    }

    void emitMonotonic(qint64 done)
    {
        if (done <= lastReported_ && lastReported_ != 0)
            return;
        lastReported_ = done;
        report_(done);
    }

    qint64 total_ = 0;
    qint64 validationBudget_ = 0;
    qint64 lastReported_ = 0;
    std::function<void(qint64)> report_;
};

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
    const bool encodedText = !opts.byteContextMode
        && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
    *usePcre2 = encodedText || opts.regexMode || opts.wholeWord
        || (!opts.caseSensitive && !binaryLiteral);
    if (!*usePcre2) {
        bool representable = true;
        *literal = encodeTextFragment(pattern, opts.textEncoding, &representable);
        if (!representable) {
            if (error) {
                *error = QObject::tr("查找文本包含 %1 无法表示的字符。")
                             .arg(encodingName(opts.textEncoding));
            }
            return false;
        }
    }
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
    stopWorkers(true);
    closeOverrunFile();
    if (!overrunFile_.isEmpty()) QFile::remove(overrunFile_);
}

void LargeFileSearch::setSource(const uchar* mmapBase, qint64 mmapSize,
                                const LineIndex* lineIndex, const PatchList* patchList)
{
    // All four source objects are borrowed. Never replace them while a queued
    // or running search can still dereference the previous snapshot.
    stopWorkers(false);
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

bool LargeFileSearch::reserveWorker()
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (shuttingDown_ || workerReservations_ != 0)
        return false;
    ++workerReservations_;
    return true;
}

void LargeFileSearch::releaseWorker()
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (workerReservations_ > 0)
        --workerReservations_;
    if (workerReservations_ == 0)
        workerIdle_.notify_all();
}

void LargeFileSearch::stopWorkers(bool permanent)
{
    cancel_.store(true, std::memory_order_release);
    const qint64 taskId = lastTaskId_;
    if (taskId > 0)
        AsyncTaskManager::instance().cancel(taskId);

    std::unique_lock<std::mutex> lock(workerMutex_);
    if (permanent)
        shuttingDown_ = true;
    workerIdle_.wait(lock, [this] { return workerReservations_ == 0; });
    lastTaskId_ = 0;
    if (!permanent)
        cancel_.store(false, std::memory_order_release);
}

void LargeFileSearch::cancelAndWait()
{
    stopWorkers(false);
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
    constexpr qint64 SideContext = 8;
    const qint64 virtualSize = patchList_ ? patchList_->virtualSize(mmapSize_) : mmapSize_;
    const qint64 rowStart = (m.byteOffset / BytesPerRow) * BytesPerRow;
    const qint64 matchEnd = qMin(virtualSize,
        m.byteOffset + qMax<qint64>(0, m.length));
    qint64 contextStart = qMax<qint64>(0, m.byteOffset - SideContext);
    qint64 contextEnd = qMin(virtualSize,
        qMax(contextStart + BytesPerRow, matchEnd + SideContext));
    if (contextEnd - contextStart > OVERRUN_CTX_MAX) {
        contextEnd = qMin(virtualSize, matchEnd + SideContext);
        contextStart = qMax<qint64>(0, contextEnd - OVERRUN_CTX_MAX);
    }
    QByteArray preview = readVirtualBytes(contextStart, contextEnd);
    for (char &value : preview) {
        const uchar byte = uchar(value);
        if (byte < 0x20 || byte > 0x7e)
            value = '.';
    }
    m.line = qint32(qMin<qint64>(rowStart / BytesPerRow, INT32_MAX));
    m.col = qint32(m.byteOffset - rowStart);
    m.contextByteOffset = contextStart;
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
        if ((!patchList_ || !patchList_->isModified()) && mmapBase_) {
            const auto nextCandidate = makeByteCandidateFinder(
                mmapBase_, virtualSize, literalBytes, caseSensitive, 0,
                isCancelled, reportProgress, {});
            qint64 candidate = nextCandidate ? nextCandidate(0) : virtualSize + 1;
            while (candidate >= 0 && candidate <= virtualSize - literalBytes.size()) {
                LargeFileSearchMatch match;
                match.byteOffset = candidate;
                match.length = literalBytes.size();
                if (opts.byteContextMode)
                    fillByteContext(match);
                else
                    fillLineContext(match);
                pending.append(std::move(match));
                const bool firstVisibleResult = memMatches_.isEmpty()
                    && overrunCount_ == 0 && pending.size() == 1;
                if ((firstVisibleResult || pending.size() >= PublishBatchSize)
                    && !publish()) {
                    return false;
                }
                candidate = nextCandidate(candidate + 1);
            }
            if (candidate < 0)
                return false;
            return publish();
        }
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
                    const bool firstVisibleResult = memMatches_.isEmpty()
                        && overrunCount_ == 0 && pending.size() == 1;
                    if ((firstVisibleResult || pending.size() >= PublishBatchSize)
                        && !publish())
                        return false;
                    matched = failure.at(matched - 1);
                }
            }
            start = end;
            if (reportProgress)
                reportProgress(start);
        }
    } else {
        const bool invalidUtfText = !opts.byteContextMode
            && opts.tolerateInvalidEncoding
            && (opts.textEncoding == TextEncoding::Utf8
                || opts.textEncoding == TextEncoding::Utf8Bom);
        const QByteArray invalidUtfLiteralFilter =
            safeInvalidUtfAsciiLiteralFilter(pattern, opts, invalidUtfText);
        const bool encodedByteRegex = !opts.byteContextMode
            && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
        Pcre2Regex compiled;
        QByteArray requiredLiteral;
        bool requiredLiteralCaseSensitive = true;
        Pcre2Regex::StartByteFilter startByteFilter;
        const bool binaryWholeWord = opts.byteContextMode
            && !opts.regexMode && opts.wholeWord;
        if (encodedByteRegex) {
            EncodedRegexCompiler::Output translated;
            const QString expression = rawSearchExpression(pattern, opts);
            if (!EncodedRegexCompiler::compile(
                    expression, opts.textEncoding, opts.caseSensitive,
                    &translated, &scanError_)
                || !compiled.compileBytePattern(
                    translated.bytePattern, translated.caseSensitive,
                    &scanError_)) {
                return false;
            }
            requiredLiteral = translated.requiredLiteral;
            requiredLiteralCaseSensitive =
                translated.requiredLiteralCaseSensitive;
        } else {
            if (!compiled.compile(pattern, !opts.regexMode, opts.caseSensitive,
                                  opts.wholeWord, &scanError_,
                                  !binaryWholeWord, invalidUtfText)) {
                return false;
            }
            startByteFilter = compiled.startByteFilter();
        }

        const bool validatesUtf8 = !encodedByteRegex && !binaryWholeWord
            && !invalidUtfText;
        RegexProgressPhases progressPhases(
            virtualSize, validatesUtf8, reportProgress);
        const auto regexProgress = progressPhases.searchReporter();
        const auto validationProgress = progressPhases.validationReporter();

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

        const auto recordMatch = [&](qint64 offset, qint64 length) {
                LargeFileSearchMatch match;
                match.byteOffset = offset;
                match.length = qint32(length);
                if (opts.byteContextMode)
                    fillByteContext(match);
                else
                    fillLineContext(match);
                pending.append(std::move(match));
                const bool firstVisibleResult = memMatches_.isEmpty()
                    && overrunCount_ == 0 && pending.size() == 1;
                return (!firstVisibleResult && pending.size() < PublishBatchSize)
                    || publish();
            };
        if (encodedByteRegex) {
            const auto normalizeStart = makeEncodedBoundaryNormalizer(
                subject, virtualSize, opts.textEncoding);
            const auto nextCandidate = makeByteCandidateFinder(
                subject, virtualSize, requiredLiteral,
                requiredLiteralCaseSensitive, 0, isCancelled,
                regexProgress, normalizeStart);
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {},
                [subject, virtualSize, encoding = opts.textEncoding](qint64 offset) {
                    return EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, offset, encoding);
                }, 0, normalizeStart, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else if (!invalidUtfLiteralFilter.isEmpty()) {
            const auto nextCandidate = makeByteCandidateFinder(
                subject, virtualSize, invalidUtfLiteralFilter,
                opts.caseSensitive, 0, isCancelled, regexProgress, {});
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else if (!startByteFilter.isEmpty()) {
            const auto nextCandidate = makeStartByteCandidateFinder(
                subject, virtualSize, startByteFilter, 0, !invalidUtfText,
                isCancelled, regexProgress);
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, nextCandidate,
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        } else {
            const Pcre2Regex::ScanResult result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                recordMatch, &scanError_, {}, {}, 0, {}, {},
                (std::numeric_limits<qint64>::max)(), validationProgress);
            if (result != Pcre2Regex::ScanResult::Complete)
                return false;
        }
    }

    const bool ok = publish();
    return ok && !storageFailed_ && scanError_.isEmpty();
}

bool LargeFileSearch::scanOneBounded(
    qint64 virtualSize, const QString &pattern, const Options &opts,
    qint64 fromByte, bool forward, bool wrap,
    const std::function<bool()> &isCancelled,
    const std::function<void(qint64)> &reportProgress,
    LargeFileSearchMatch *match, QString *error) const
{
    if (match) {
        *match = {};
        match->byteOffset = -1;
    }
    if (error) error->clear();

    QByteArray literalBytes;
    bool usePcre2 = false;
    if (!prepareSearchPattern(pattern, opts, &literalBytes, &usePcre2, error))
        return false;

    if (!usePcre2 && (!patchList_ || !patchList_->isModified()) && mmapBase_) {
        const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
        const auto findFirstInRange = [&](qint64 start, qint64 end,
                                          LargeFileSearchMatch *accepted) {
            const auto next = makeByteCandidateFinder(
                mmapBase_, virtualSize, literalBytes, caseSensitive, start,
                isCancelled, reportProgress, {}, end);
            const qint64 candidate = next ? next(start) : virtualSize + 1;
            if (candidate < 0)
                return false;
            if (candidate < end) {
                accepted->byteOffset = candidate;
                accepted->length = literalBytes.size();
            }
            return true;
        };
        const auto findLastInRange = [&](qint64 start, qint64 end,
                                         LargeFileSearchMatch *accepted) {
            const auto next = makeByteCandidateFinder(
                mmapBase_, virtualSize, literalBytes, caseSensitive, start,
                isCancelled, reportProgress, {}, end);
            qint64 candidate = next ? next(start) : virtualSize + 1;
            while (candidate >= 0 && candidate < end) {
                accepted->byteOffset = candidate;
                accepted->length = literalBytes.size();
                candidate = next(candidate + 1);
            }
            return candidate >= 0;
        };
        LargeFileSearchMatch found;
        found.byteOffset = -1;
        bool ok = true;
        if (forward) {
            const qint64 start = fromByte < 0 ? 0
                : (fromByte >= virtualSize ? virtualSize : fromByte + 1);
            ok = findFirstInRange(start, virtualSize, &found);
            if (ok && found.byteOffset < 0 && wrap && start > 0)
                ok = findFirstInRange(0, start, &found);
        } else {
            const qint64 end = fromByte == std::numeric_limits<qint64>::max()
                ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
            ok = findLastInRange(0, end, &found);
            if (ok && found.byteOffset < 0 && wrap && end < virtualSize)
                ok = findLastInRange(end, virtualSize, &found);
        }
        if (!ok)
            return false;
        if (found.byteOffset >= 0) {
            if (opts.byteContextMode)
                fillByteContext(found);
            else
                fillLineContext(found);
            if (match)
                *match = std::move(found);
        }
        return true;
    }

    const bool encodedByteRegex = usePcre2 && !opts.byteContextMode
        && !EncodedRegexCompiler::isUtf8SubjectEncoding(opts.textEncoding);
    const bool binaryWholeWord = opts.byteContextMode
        && !opts.regexMode && opts.wholeWord;
    Pcre2Regex compiled;
    QByteArray requiredLiteral;
    bool requiredLiteralCaseSensitive = true;
    Pcre2Regex::StartByteFilter startByteFilter;
    bool invalidUtfText = false;
    if (usePcre2) {
        if (encodedByteRegex) {
            EncodedRegexCompiler::Output translated;
            const QString expression = rawSearchExpression(pattern, opts);
            if (!EncodedRegexCompiler::compile(
                    expression, opts.textEncoding, opts.caseSensitive,
                    &translated, error)
                || !compiled.compileBytePattern(
                    translated.bytePattern, translated.caseSensitive, error)) {
                return false;
            }
            requiredLiteral = translated.requiredLiteral;
            requiredLiteralCaseSensitive =
                translated.requiredLiteralCaseSensitive;
        } else {
            invalidUtfText = !opts.byteContextMode
                && opts.tolerateInvalidEncoding
                && (opts.textEncoding == TextEncoding::Utf8
                    || opts.textEncoding == TextEncoding::Utf8Bom);
            if (!compiled.compile(pattern, !opts.regexMode, opts.caseSensitive,
                                  opts.wholeWord, error,
                                  !binaryWholeWord, invalidUtfText)) {
                return false;
            }
            startByteFilter = compiled.startByteFilter();
        }
    }
    const QByteArray invalidUtfLiteralFilter =
        safeInvalidUtfAsciiLiteralFilter(pattern, opts, invalidUtfText);
    const bool validatesUtf8 = usePcre2 && !encodedByteRegex
        && !binaryWholeWord && !invalidUtfText;
    RegexProgressPhases progressPhases(
        virtualSize, validatesUtf8, reportProgress);
    const auto regexProgress = progressPhases.searchReporter();
    const auto validationProgress = progressPhases.validationReporter();

    if (usePcre2) {
        const uchar *subject = mmapBase_;
        QTemporaryFile snapshot(QDir(overrunDir()).filePath(
            QStringLiteral("regex_navigation_XXXXXX.tmp")));
        uchar *snapshotMap = nullptr;
        if (patchList_ && patchList_->isModified()) {
            snapshot.setAutoRemove(true);
            if (!snapshot.open()) {
                if (error) *error = QObject::tr("无法创建搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            constexpr qint64 Chunk = Q_INT64_C(16) * 1024 * 1024;
            for (qint64 start = 0; start < virtualSize; start += Chunk) {
                if (isCancelled && isCancelled())
                    return false;
                const qint64 end = qMin(virtualSize, start + Chunk);
                const QByteArray bytes = readVirtualBytes(start, end);
                if (bytes.size() != end - start
                    || snapshot.write(bytes) != bytes.size()) {
                    if (error) *error = QObject::tr("无法写入搜索快照：%1")
                                            .arg(snapshot.errorString());
                    return false;
                }
            }
            if (!snapshot.flush()) {
                if (error) *error = QObject::tr("无法刷新搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            snapshotMap = snapshot.map(0, virtualSize);
            if (!snapshotMap && virtualSize > 0) {
                if (error) *error = QObject::tr("无法映射搜索快照：%1")
                                        .arg(snapshot.errorString());
                return false;
            }
            subject = snapshotMap;
        }
        const auto unmap = qScopeGuard([&] {
            if (snapshotMap)
                snapshot.unmap(snapshotMap);
        });
        const std::function<qint64(qint64)> advance = encodedByteRegex
            ? std::function<qint64(qint64)>(
                [subject, virtualSize, encoding = opts.textEncoding](qint64 offset) {
                    return EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, offset, encoding);
                })
            : std::function<qint64(qint64)>();

        LargeFileSearchMatch found;
        found.byteOffset = -1;
        bool domainFailure = false;
        const auto accept = [&](qint64 offset, qint64 length, bool stopAtFirst) {
            if (length < 0 || length > std::numeric_limits<qint32>::max()) {
                domainFailure = true;
                if (error) *error = QObject::tr("单个匹配过长。");
                return false;
            }
            found.byteOffset = offset;
            found.length = qint32(length);
            return !stopAtFirst;
        };
        const auto runForward = [&](qint64 start, qint64 endExclusive) {
            const auto normalizeStart = encodedByteRegex
                ? makeEncodedBoundaryNormalizer(
                    subject, virtualSize, opts.textEncoding, start)
                : std::function<qint64(qint64)>();
            std::function<qint64(qint64)> nextCandidate;
            if (encodedByteRegex) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, requiredLiteral,
                    requiredLiteralCaseSensitive, start, isCancelled,
                    regexProgress, normalizeStart, endExclusive);
            } else if (!invalidUtfLiteralFilter.isEmpty()) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, invalidUtfLiteralFilter,
                    opts.caseSensitive, start, isCancelled,
                    regexProgress, {}, endExclusive);
            } else {
                nextCandidate = makeStartByteCandidateFinder(
                    subject, virtualSize, startByteFilter, start,
                    !invalidUtfText, isCancelled,
                    regexProgress, endExclusive);
            }
            const auto result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                [&](qint64 offset, qint64 length) {
                    return accept(offset, length, true);
                }, error, {}, advance, start, normalizeStart, nextCandidate,
                endExclusive, validationProgress);
            return result != Pcre2Regex::ScanResult::Failed
                && !domainFailure
                && (!(isCancelled && isCancelled()));
        };

        bool ok = true;
        if (forward) {
            const qint64 start = fromByte < 0 ? 0
                : fromByte >= virtualSize ? virtualSize
                : encodedByteRegex
                    ? EncodedRegexCompiler::nextUnitOffset(
                        subject, virtualSize, fromByte, opts.textEncoding)
                    : fromByte + 1;
            ok = runForward(start, virtualSize + 1);
            if (ok && found.byteOffset < 0 && wrap && start > 0)
                ok = runForward(0, start);
        } else {
            const qint64 end = fromByte == std::numeric_limits<qint64>::max()
                ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
            const auto normalizeStart = encodedByteRegex
                ? makeEncodedBoundaryNormalizer(
                    subject, virtualSize, opts.textEncoding)
                : std::function<qint64(qint64)>();
            std::function<qint64(qint64)> nextCandidate;
            if (encodedByteRegex) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, requiredLiteral,
                    requiredLiteralCaseSensitive, 0, isCancelled,
                    regexProgress, normalizeStart, end);
            } else if (!invalidUtfLiteralFilter.isEmpty()) {
                nextCandidate = makeByteCandidateFinder(
                    subject, virtualSize, invalidUtfLiteralFilter,
                    opts.caseSensitive, 0, isCancelled,
                    regexProgress, {}, end);
            } else {
                nextCandidate = makeStartByteCandidateFinder(
                    subject, virtualSize, startByteFilter, 0,
                    !invalidUtfText, isCancelled,
                    regexProgress, end);
            }
            const auto result = compiled.scan(
                subject, virtualSize, isCancelled, regexProgress,
                [&](qint64 offset, qint64 length) {
                    if (offset >= end) {
                        return false;
                    }
                    return accept(offset, length, false);
                }, error, {}, advance, 0, normalizeStart, nextCandidate, end,
                validationProgress);
            ok = result != Pcre2Regex::ScanResult::Failed
                && !domainFailure
                && (!(isCancelled && isCancelled()));
            if (ok && found.byteOffset < 0 && wrap && end < virtualSize) {
                found.byteOffset = -1;
                const auto wrappedNormalizer = encodedByteRegex
                    ? makeEncodedBoundaryNormalizer(
                        subject, virtualSize, opts.textEncoding, end)
                    : std::function<qint64(qint64)>();
                std::function<qint64(qint64)> wrappedCandidate;
                if (encodedByteRegex) {
                    wrappedCandidate = makeByteCandidateFinder(
                        subject, virtualSize, requiredLiteral,
                        requiredLiteralCaseSensitive, end, isCancelled,
                        regexProgress, wrappedNormalizer, virtualSize + 1);
                } else if (!invalidUtfLiteralFilter.isEmpty()) {
                    wrappedCandidate = makeByteCandidateFinder(
                        subject, virtualSize, invalidUtfLiteralFilter,
                        opts.caseSensitive, end, isCancelled,
                        regexProgress, {}, virtualSize + 1);
                } else {
                    wrappedCandidate = makeStartByteCandidateFinder(
                        subject, virtualSize, startByteFilter, end,
                        !invalidUtfText, isCancelled,
                        regexProgress, virtualSize + 1);
                }
                const auto wrapped = compiled.scan(
                    subject, virtualSize, isCancelled, regexProgress,
                    [&](qint64 offset, qint64 length) {
                        return accept(offset, length, false);
                    }, error, {}, advance, end, wrappedNormalizer,
                    wrappedCandidate, virtualSize + 1, validationProgress);
                ok = wrapped != Pcre2Regex::ScanResult::Failed
                    && !domainFailure
                    && (!(isCancelled && isCancelled()));
            }
        }
        if (!ok)
            return false;
        if (found.byteOffset >= 0) {
            if (opts.byteContextMode)
                fillByteContext(found);
            else
                fillLineContext(found);
            if (match) *match = std::move(found);
        }
        return true;
    }

    constexpr qint64 CoreBytes = Q_INT64_C(16) * 1024 * 1024;
    constexpr qint64 PrefixBytes = Q_INT64_C(64) * 1024;
    constexpr qint64 OverlapBytes = Q_INT64_C(16) * 1024 * 1024;
    qint64 scannedBytes = 0;

    const auto scanCore = [&](qint64 coreStart, qint64 coreEnd,
                              LargeFileSearchMatch *accepted) -> bool {
        if (isCancelled && isCancelled())
            return false;
        const qint64 windowStart = qMax<qint64>(0, coreStart - PrefixBytes);
        const qint64 windowEnd = qMin(virtualSize, coreEnd + OverlapBytes);
        const QByteArray bytes = readVirtualBytes(windowStart, windowEnd);
        if (bytes.size() != windowEnd - windowStart) {
            if (error) *error = QObject::tr("无法读取搜索窗口。");
            return false;
        }

        bool found = false;
        LargeFileSearchMatch best;
        const auto consider = [&](qint64 localOffset, qint64 length) {
            const qint64 globalOffset = windowStart + localOffset;
            if (globalOffset < coreStart || globalOffset >= coreEnd)
                return true;
            if (length < 0 || length > std::numeric_limits<qint32>::max()) {
                if (error) *error = QObject::tr("单个匹配过长。");
                return false;
            }
            best.byteOffset = globalOffset;
            best.length = qint32(length);
            found = true;
            // 前向扫描的首个结果就是最近结果；后向扫描保留本窗口最后一个。
            return !forward;
        };

        if (!usePcre2) {
            if (literalBytes.isEmpty())
                return true;
            const bool caseSensitive = opts.hexBytesMode ? true : opts.caseSensitive;
            const auto *windowSubject =
                reinterpret_cast<const uchar *>(bytes.constData());
            const auto nextCandidate = makeByteCandidateFinder(
                windowSubject, bytes.size(), literalBytes, caseSensitive, 0,
                isCancelled, {}, {});
            qint64 candidate = nextCandidate ? nextCandidate(0) : bytes.size() + 1;
            while (candidate >= 0
                   && candidate <= bytes.size() - literalBytes.size()) {
                if (!consider(candidate, literalBytes.size()) && found)
                    break;
                candidate = nextCandidate(candidate + 1);
            }
            if (candidate < 0 && !found)
                return false;
        } else {
            const auto noCancel = [&]() {
                return isCancelled && isCancelled();
            };
            const auto *windowSubject =
                reinterpret_cast<const uchar *>(bytes.constData());
            const auto result = compiled.scan(
                windowSubject, bytes.size(), noCancel, {}, consider, error, {},
                encodedByteRegex
                    ? std::function<qint64(qint64)>(
                        [windowSubject, size = qint64(bytes.size()),
                         encoding = opts.textEncoding](qint64 offset) {
                            return EncodedRegexCompiler::nextUnitOffset(
                                windowSubject, size, offset, encoding);
                        })
                    : std::function<qint64(qint64)>());
            if (result == Pcre2Regex::ScanResult::Failed)
                return false;
            if (result == Pcre2Regex::ScanResult::Cancelled && !found)
                return false;
        }

        if (found && accepted)
            *accepted = best;
        return true;
    };

    const auto scanForwardRange = [&](qint64 start, qint64 end,
                                      LargeFileSearchMatch *accepted) {
        for (qint64 coreStart = start; coreStart < end; ) {
            const qint64 coreEnd = qMin(end, coreStart + CoreBytes);
            if (!scanCore(coreStart, coreEnd, accepted))
                return false;
            scannedBytes += coreEnd - coreStart;
            if (reportProgress) reportProgress(scannedBytes);
            if (accepted && accepted->byteOffset >= 0)
                return true;
            coreStart = coreEnd;
        }
        return true;
    };
    const auto scanReverseRange = [&](qint64 start, qint64 end,
                                      LargeFileSearchMatch *accepted) {
        for (qint64 coreEnd = end; coreEnd > start; ) {
            const qint64 coreStart = qMax(start, coreEnd - CoreBytes);
            if (!scanCore(coreStart, coreEnd, accepted))
                return false;
            scannedBytes += coreEnd - coreStart;
            if (reportProgress) reportProgress(scannedBytes);
            if (accepted && accepted->byteOffset >= 0)
                return true;
            coreEnd = coreStart;
        }
        return true;
    };

    LargeFileSearchMatch found;
    found.byteOffset = -1;
    bool ok = true;
    if (forward) {
        const qint64 start = fromByte < 0 ? 0
            : (fromByte >= virtualSize ? virtualSize : fromByte + 1);
        ok = scanForwardRange(start, virtualSize, &found);
        if (ok && found.byteOffset < 0 && wrap && start > 0)
            ok = scanForwardRange(0, start, &found);
    } else {
        const qint64 end = fromByte == std::numeric_limits<qint64>::max()
            ? virtualSize : qBound<qint64>(0, fromByte, virtualSize);
        ok = scanReverseRange(0, end, &found);
        if (ok && found.byteOffset < 0 && wrap && end < virtualSize)
            ok = scanReverseRange(end, virtualSize, &found);
    }
    if (!ok)
        return false;
    if (found.byteOffset >= 0) {
        if (opts.byteContextMode)
            fillByteContext(found);
        else
            fillLineContext(found);
        if (match) *match = std::move(found);
    }
    return true;
}

// ============================================================================
//  findAll（异步）
// ============================================================================
void LargeFileSearch::findAll(const QString& pattern, const Options& opts) {
    findAllViaTaskManager(pattern, opts, QString());
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
        (qint64 /*taskId*/, AsyncCancelPtr cancel,
         std::function<void(qint64,qint64)> reportProgress,
         std::function<void(const AsyncErrorInfo&)> reportError)
        {
            Q_UNUSED(reportError);
            auto release = qScopeGuard([this] { releaseWorker(); });
            const auto isCancelled = [&]() {
                return (cancel && cancel->load(std::memory_order_acquire))
                    || cancel_.load(std::memory_order_relaxed);
            };
            const bool ok = scanAllBounded(
                virtualSize, pattern, opts, isCancelled,
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
    if (!mmapBase_ || (!lineIndex_ && !opts.byteContextMode)) {
        if (callback)
            callback(false, {}, QObject::tr("数据源未就绪。"));
        return -1;
    }
    QString validationError;
    QByteArray literal;
    bool usePcre2 = false;
    if (!prepareSearchPattern(pattern, opts, &literal, &usePcre2,
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
        (qint64, AsyncCancelPtr cancel,
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
            const bool ok = scanOneBounded(
                virtualSize, pattern, opts, fromByte, forward, wrap,
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
