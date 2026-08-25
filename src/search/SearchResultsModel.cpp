#include "SearchResultsModel.h"
#include "LineIndex.h"

#include <QApplication>
#include <QPalette>
#include <QMap>
#include <QStringConverter>
#include <algorithm>
#include <limits>

namespace {
constexpr quint32 OverrunMagic = 0x564F534Du;
constexpr quint32 OverrunVersion1 = 1;
constexpr quint32 OverrunVersion2 = 2;
constexpr qint64 OverrunCheckpointStride = 512;

bool readHeader(QDataStream &stream, quint32 *version, quint64 *count)
{
    quint32 magic = 0;
    quint32 parsedVersion = 0;
    quint64 parsedCount = 0;
    stream >> magic >> parsedVersion >> parsedCount;
    if (stream.status() != QDataStream::Ok || magic != OverrunMagic
        || (parsedVersion != OverrunVersion1 && parsedVersion != OverrunVersion2)) {
        return false;
    }
    if (version) *version = parsedVersion;
    if (count) *count = parsedCount;
    return true;
}

bool readRecord(QDataStream &stream, quint32 version, LargeFileSearchMatch *match)
{
    if (!match)
        return false;
    quint64 byteOffset = 0;
    qint32 length = 0;
    qint32 line = 0;
    qint32 col = 0;
    quint64 contextOffset = 0;
    QByteArray context;
    stream >> byteOffset >> length >> line >> col;
    if (version >= OverrunVersion2)
        stream >> contextOffset;
    stream >> context;
    if (stream.status() != QDataStream::Ok)
        return false;
    match->byteOffset = qint64(byteOffset);
    match->length = length;
    match->line = line;
    match->col = col;
    match->contextByteOffset = version >= OverrunVersion2
        ? qint64(contextOffset) : qMax<qint64>(0, qint64(byteOffset) - col);
    match->lineContext = std::move(context);
    return true;
}

void prepareDisplayContext(GroupedSearchLine *group, TextEncoding encoding)
{
    if (!group)
        return;
    const DecodedTextProjection projection = decodeTextProjection(
        group->lineContext, encoding, false, false);
    group->displayContext = projection.text;
    group->sourceBoundaries = projection.sourceBoundaries;
    group->utf16Boundaries.clear();
    group->utf16Boundaries.reserve(group->sourceBoundaries.size());
    int utf16 = 0;
    group->utf16Boundaries.append(0);
    while (utf16 < group->displayContext.size()
           && group->utf16Boundaries.size() < group->sourceBoundaries.size()) {
        const int units = group->displayContext.at(utf16).isHighSurrogate()
                && utf16 + 1 < group->displayContext.size()
                && group->displayContext.at(utf16 + 1).isLowSurrogate()
            ? 2 : 1;
        utf16 += units;
        group->utf16Boundaries.append(utf16);
    }
    while (group->utf16Boundaries.size() < group->sourceBoundaries.size())
        group->utf16Boundaries.append(group->displayContext.size());
}

QPair<qint32, qint32> displayRange(const GroupedSearchLine &group,
                                   const LargeFileSearchMatch &match)
{
    const qint64 relative64 = match.contextByteOffset >= 0
        ? match.byteOffset - match.contextByteOffset : match.col;
    const int relative = int(qBound<qint64>(0, relative64,
                                            match.lineContext.size()));
    const int byteLength = qBound(0, int(match.length),
                                  match.lineContext.size() - relative);
    const auto boundaryIndex = [&group](int byteOffset) {
        const auto found = std::lower_bound(
            group.sourceBoundaries.cbegin(), group.sourceBoundaries.cend(),
            qint32(byteOffset));
        return found == group.sourceBoundaries.cend()
            ? qMax(0, int(group.sourceBoundaries.size()) - 1)
            : int(found - group.sourceBoundaries.cbegin());
    };
    const int startIndex = boundaryIndex(relative);
    const int endIndex = boundaryIndex(relative + byteLength);
    const int charStart = group.utf16Boundaries.value(startIndex, 0);
    const int charEnd = group.utf16Boundaries.value(
        endIndex, group.displayContext.size());
    const int charLength = qMax(0, charEnd - charStart);
    return qMakePair(qint32(charStart), qint32(charLength));
}

void appendMatch(GroupedSearchLine *group, const LargeFileSearchMatch &match,
                 TextEncoding encoding)
{
    if (!group)
        return;
    if (group->matchByteRanges.isEmpty()) {
        group->line = match.line;
        group->contextByteOffset = match.contextByteOffset >= 0
            ? match.contextByteOffset : qMax<qint64>(0, match.byteOffset - match.col);
        group->lineContext = match.lineContext;
        prepareDisplayContext(group, encoding);
        group->firstByteOffset = match.byteOffset;
        group->firstLength = match.length;
    }
    group->matchPositions.append(displayRange(*group, match));
    group->matchByteRanges.append(qMakePair(match.byteOffset, match.length));
}
} // namespace

SearchResultsModel::SearchResultsModel(QObject* parent)
    : QAbstractItemModel(parent)
{}

SearchResultsModel::~SearchResultsModel() = default;

void SearchResultsModel::setTextEncoding(TextEncoding encoding)
{
    if (textEncoding_ == encoding)
        return;
    textEncoding_ = encoding;
    if (!memoryMatches_.isEmpty()) {
        beginResetModel();
        buildGrouped(memoryMatches_);
        cachedOverrunRow_ = -1;
        cachedOverrunLine_ = {};
        endResetModel();
    }
}

void SearchResultsModel::clear() {
    beginResetModel();
    memoryMatches_.clear();
    groupedLines_.clear();
    totalMatchCount_ = 0;
    incompleteReason_.clear();
    overrunFile_.clear();
    overrunCount_ = 0;
    overrunCheckpoints_.clear();
    overrunGroupedCount_ = 0;
    overrunVersion_ = 0;
    overrunDataOffset_ = 0;
    cachedOverrunRow_ = -1;
    cachedOverrunLine_ = {};
    endResetModel();
}

void SearchResultsModel::buildGrouped(const QVector<LargeFileSearchMatch>& matches) {
    // 按 line 分组，同一行的匹配合并到一条 GroupedSearchLine
    // matches 已按 byteOffset 排序（LargeFileSearch 保证），因此同一行内也是有序的
    groupedLines_.clear();
    if (matches.isEmpty()) return;

    int currentLine = -1;
    qint64 currentContextOffset = -1;
    GroupedSearchLine* cur = nullptr;

    for (const auto& m : matches) {
        if (m.line != currentLine || m.contextByteOffset != currentContextOffset) {
            // 新行
            GroupedSearchLine g;
            g.line = m.line;
            g.contextByteOffset = m.contextByteOffset;
            g.lineContext = m.lineContext;
            appendMatch(&g, m, textEncoding_);
            groupedLines_.append(std::move(g));
            cur = &groupedLines_.last();
            currentLine = m.line;
            currentContextOffset = m.contextByteOffset;
        } else {
            // 同一行的后续匹配
            if (cur) {
                appendMatch(cur, m, textEncoding_);
            }
        }
    }
}

void SearchResultsModel::buildOverrunIndex()
{
    overrunCheckpoints_.clear();
    overrunGroupedCount_ = 0;
    overrunVersion_ = 0;
    overrunDataOffset_ = 0;
    if (overrunCount_ <= 0 || overrunFile_.isEmpty() || !QFile::exists(overrunFile_))
        return;

    QFile file(overrunFile_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint64 storedCount = 0;
    if (!readHeader(stream, &overrunVersion_, &storedCount))
        return;
    Q_UNUSED(storedCount);
    overrunDataOffset_ = file.pos();

    qint32 previousLine = std::numeric_limits<qint32>::min();
    qint64 previousContext = std::numeric_limits<qint64>::min();
    while (!stream.atEnd()) {
        const qint64 recordOffset = file.pos();
        LargeFileSearchMatch match;
        if (!readRecord(stream, overrunVersion_, &match))
            break;
        if (match.line == previousLine && match.contextByteOffset == previousContext)
            continue;
        if ((overrunGroupedCount_ % OverrunCheckpointStride) == 0)
            overrunCheckpoints_.append({overrunGroupedCount_, recordOffset});
        ++overrunGroupedCount_;
        previousLine = match.line;
        previousContext = match.contextByteOffset;
    }
}

bool SearchResultsModel::readOverrunGroupedLine(qint64 relativeRow,
                                                GroupedSearchLine *line) const
{
    if (!line || relativeRow < 0 || relativeRow >= overrunGroupedCount_
        || overrunCheckpoints_.isEmpty()) {
        return false;
    }
    auto checkpoint = std::upper_bound(
        overrunCheckpoints_.cbegin(), overrunCheckpoints_.cend(), relativeRow,
        [](qint64 row, const OverrunCheckpoint &candidate) {
            return row < candidate.groupedRow;
        });
    if (checkpoint == overrunCheckpoints_.cbegin())
        return false;
    --checkpoint;

    QFile file(overrunFile_);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(checkpoint->fileOffset))
        return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    qint64 currentRow = checkpoint->groupedRow - 1;
    qint32 previousLine = std::numeric_limits<qint32>::min();
    qint64 previousContext = std::numeric_limits<qint64>::min();
    GroupedSearchLine current;
    while (!stream.atEnd()) {
        LargeFileSearchMatch match;
        if (!readRecord(stream, overrunVersion_, &match))
            break;
        const bool newGroup = match.line != previousLine
                              || match.contextByteOffset != previousContext;
        if (newGroup) {
            if (currentRow == relativeRow) {
                *line = std::move(current);
                return true;
            }
            ++currentRow;
            if (currentRow > relativeRow)
                return false;
            current = {};
            previousLine = match.line;
            previousContext = match.contextByteOffset;
        }
        if (currentRow == relativeRow)
            appendMatch(&current, match, textEncoding_);
    }
    if (currentRow == relativeRow && !current.matchByteRanges.isEmpty()) {
        *line = std::move(current);
        return true;
    }
    return false;
}

void SearchResultsModel::setResults(const QVector<LargeFileSearchMatch>& memoryMatches,
                                    const QString& overrunFilePath, qint64 overrunCount,
                                    const QVector<QPair<qint64, qint64>>& checkpoints,
                                    qint64 groupedOverrunCount,
                                    quint32 overrunVersion)
{
    beginResetModel();
    memoryMatches_ = memoryMatches;
    incompleteReason_.clear();
    overrunFile_ = overrunFilePath;
    overrunCount_ = overrunCount;
    totalMatchCount_ = qint64(memoryMatches.size()) + overrunCount;

    // 按 byteOffset 排序内存 matches（保证同 line 内有序）
    const auto orderedBefore = [](const LargeFileSearchMatch& a,
                                  const LargeFileSearchMatch& b) {
        return a.byteOffset < b.byteOffset;
    };
    if (std::is_sorted(memoryMatches.cbegin(), memoryMatches.cend(), orderedBefore)) {
        buildGrouped(memoryMatches);
    } else {
        QVector<LargeFileSearchMatch> sorted = memoryMatches;
        std::sort(sorted.begin(), sorted.end(), orderedBefore);
        buildGrouped(sorted);
    }

    overrunCheckpoints_.clear();
    if (groupedOverrunCount >= 0 && (!checkpoints.isEmpty() || overrunCount == 0)) {
        overrunGroupedCount_ = groupedOverrunCount;
        overrunVersion_ = overrunVersion > 0 ? overrunVersion : OverrunVersion2;
        overrunCheckpoints_.reserve(checkpoints.size());
        for (const auto &checkpoint : checkpoints)
            overrunCheckpoints_.append({checkpoint.first, checkpoint.second});
    } else {
        // Compatibility for pre-v2 producers. Current searches provide the
        // writer-built sparse index and never rescan a huge spool on the UI thread.
        buildOverrunIndex();
    }
    cachedOverrunRow_ = -1;
    cachedOverrunLine_ = {};

    endResetModel();
}

bool SearchResultsModel::hasUnknownLineNumbers() const
{
    return std::any_of(memoryMatches_.cbegin(), memoryMatches_.cend(),
                       [](const LargeFileSearchMatch &match) {
                           return match.line < 0;
                       });
}

bool SearchResultsModel::decorateLineNumbers(const LineIndex &lineIndex)
{
    if (bytePositionMode_ || lineIndex.isIncomplete() || !hasUnknownLineNumbers())
        return false;

    QVector<LargeFileSearchMatch> decorated = memoryMatches_;
    bool changed = false;
    for (LargeFileSearchMatch &match : decorated) {
        if (match.line >= 0)
            continue;
        int line = -1;
        qint64 column = -1;
        lineIndex.byteToLineCol(match.byteOffset, &line, &column);
        if (line < 0)
            continue;
        match.line = qint32(line);
        match.col = qint32(qBound<qint64>(qint64(0), column,
                         qint64(std::numeric_limits<qint32>::max())));
        changed = true;
    }
    if (!changed)
        return false;

    beginResetModel();
    memoryMatches_ = std::move(decorated);
    buildGrouped(memoryMatches_);
    cachedOverrunRow_ = -1;
    cachedOverrunLine_ = {};
    endResetModel();
    return true;
}

int SearchResultsModel::groupedCount() const
{
    return int(qMin<qint64>(qint64(groupedLines_.size()) + overrunGroupedCount_,
                            std::numeric_limits<int>::max()));
}

const GroupedSearchLine *SearchResultsModel::lineForRow(int row) const
{
    if (row < 0 || row >= groupedCount())
        return nullptr;
    if (row < groupedLines_.size())
        return &groupedLines_.at(row);
    const qint64 relative = qint64(row) - groupedLines_.size();
    if (cachedOverrunRow_ != relative) {
        GroupedSearchLine loaded;
        if (!readOverrunGroupedLine(relative, &loaded))
            return nullptr;
        cachedOverrunLine_ = std::move(loaded);
        cachedOverrunRow_ = relative;
    }
    return &cachedOverrunLine_;
}

qint64 SearchResultsModel::matchByteOffset(const QModelIndex& index) const {
    const GroupedSearchLine *line = index.isValid() ? lineForRow(index.row()) : nullptr;
    return line ? line->firstByteOffset : -1;
}

int SearchResultsModel::matchLength(const QModelIndex& index) const {
    const GroupedSearchLine *line = index.isValid() ? lineForRow(index.row()) : nullptr;
    return line ? line->firstLength : 0;
}

bool SearchResultsModel::matchByteOffsetAtIndex(int row, int matchIdx, qint64* outByteOffset, int* outLength) const {
    const GroupedSearchLine *loaded = lineForRow(row);
    if (!loaded) return false;
    const GroupedSearchLine& g = *loaded;
    if (g.matchByteRanges.isEmpty()) return false;
    int idx = matchIdx;
    if (idx < 0) idx = 0;
    if (idx >= g.matchByteRanges.size()) idx = g.matchByteRanges.size() - 1;
    if (outByteOffset) *outByteOffset = g.matchByteRanges[idx].first;
    if (outLength)     *outLength     = g.matchByteRanges[idx].second;
    return true;
}

int SearchResultsModel::hitTestMatchIndex(int row, int xInColumn, int columnWidth, const QFontMetrics& fm) const {
    Q_UNUSED(columnWidth);
    const GroupedSearchLine *loaded = lineForRow(row);
    if (!loaded) return -1;
    const GroupedSearchLine& g = *loaded;
    if (g.matchPositions.isEmpty()) return -1;

    // 左 padding 4px（与 delegate 的 textRect.adjusted(4,0,-4,0) 一致）
    const int pad = 4;
    int x = xInColumn - pad;
    if (x < 0) return -1;

    const QString &text = g.displayContext;
    if (text.isEmpty()) return -1;

    // 直接用实际字体宽度计算每个 UTF-16 命中范围的像素区间；中文、emoji
    // 和比例字体不再依赖“一个字符约等于一个 ASCII 字宽”的近似。
    for (int i = 0; i < g.matchPositions.size(); ++i) {
        const int start = qBound(0, int(g.matchPositions[i].first), text.size());
        const int end = qBound(start, start + int(g.matchPositions[i].second),
                               text.size());
        const int startX = fm.horizontalAdvance(text.left(start));
        const int endX = fm.horizontalAdvance(text.left(end));
        if (x >= startX && x <= endX)
            return i;
    }

    // 点击了上下文空白处时，选择像素距离最近的命中。
    int nearest = -1;
    int nearestDist = INT_MAX;
    for (int i = 0; i < g.matchPositions.size(); ++i) {
        const int start = qBound(0, int(g.matchPositions[i].first), text.size());
        const int startX = fm.horizontalAdvance(text.left(start));
        const int dist = qAbs(startX - x);
        if (dist < nearestDist) { nearestDist = dist; nearest = i; }
    }
    return nearest;
}

const GroupedSearchLine& SearchResultsModel::groupedLineAt(int row) const {
    static GroupedSearchLine empty;
    const GroupedSearchLine *line = lineForRow(row);
    return line ? *line : empty;
}

QModelIndex SearchResultsModel::index(int row, int column, const QModelIndex& parent) const {
    if (parent.isValid()) return {};
    if (row < 0 || row >= groupedCount()) return {};
    if (column < 0 || column >= COL_COUNT) return {};
    return createIndex(row, column, nullptr);
}

QModelIndex SearchResultsModel::parent(const QModelIndex&) const { return {}; }

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return groupedCount();
}

int SearchResultsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return COL_COUNT;
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex& idx) const {
    if (!idx.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

QVariant SearchResultsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) return {};
    if (role != Qt::DisplayRole) return {};
    switch (section) {
    case COL_LINE:       return bytePositionMode_
        ? QStringLiteral("偏移") : QStringLiteral("行");
    case COL_CONTEXT:    return QStringLiteral("内容");
    case COL_MATCHCOUNT: return QStringLiteral("匹配数");
    default: return {};
    }
}

QVariant SearchResultsModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid()) return {};
    const GroupedSearchLine *loaded = lineForRow(idx.row());
    if (!loaded) return {};
    const GroupedSearchLine& g = *loaded;

    if (role == ByteOffsetRole) {
        return qulonglong(g.firstByteOffset);
    }
    if (role == LengthRole) {
        return g.firstLength;
    }
    if (role == MatchCountInLineRole) {
        return int(g.matchPositions.size());
    }
    if (role == MatchByteRangesRole) {
        QVariantList list;
        for (const auto& r : g.matchByteRanges) {
            QVariantMap m;
            m.insert("byteOffset", qulonglong(r.first));
            m.insert("length", r.second);
            list.append(m);
        }
        return list;
    }
    if (role == LineRole)
        return g.line;
    if (role == ContextRole)
        return g.displayContext;
    if (role == MatchPositionsRole) {
        QVariantList list;
        for (const auto &range : g.matchPositions) {
            QVariantMap item;
            item.insert(QStringLiteral("start"), range.first);
            item.insert(QStringLiteral("length"), range.second);
            list.append(item);
        }
        return list;
    }
    if (role == Qt::ToolTipRole) {
        switch (idx.column()) {
        case COL_CONTEXT: {
            const QString location = bytePositionMode_ || g.line < 0
                ? QObject::tr("偏移 0x%1")
                      .arg(QString::number(quint64(g.firstByteOffset), 16).toUpper())
                : QObject::tr("第 %1 行").arg(g.line + 1);
            return QObject::tr("%1 · %2 处匹配\n%3\n双击跳转")
                .arg(location).arg(g.matchPositions.size())
                .arg(g.displayContext);
        }
        case COL_MATCHCOUNT:
            return QObject::tr("该行有 %1 处匹配").arg(g.matchPositions.size());
        default:
            break;
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (idx.column() == COL_LINE || idx.column() == COL_MATCHCOUNT)
            return int(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole) return {};

    switch (idx.column()) {
    case COL_LINE:
        return bytePositionMode_
            ? QStringLiteral("0x%1").arg(
                  QString::number(quint64(g.firstByteOffset), 16).toUpper())
            : g.line < 0
            ? QStringLiteral("0x%1…").arg(
                  QString::number(quint64(g.firstByteOffset), 16).toUpper())
            : QString::number(g.line + 1);  // UI 1-based
    case COL_CONTEXT:
        return g.displayContext;
    case COL_MATCHCOUNT:
        return QString::number(g.matchPositions.size());
    default:
        return {};
    }
}
