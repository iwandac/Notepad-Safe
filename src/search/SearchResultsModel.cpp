#include "SearchResultsModel.h"

#include <QApplication>
#include <QPalette>
#include <QMap>
#include <algorithm>

SearchResultsModel::SearchResultsModel(QObject* parent)
    : QAbstractItemModel(parent)
{}

SearchResultsModel::~SearchResultsModel() = default;

void SearchResultsModel::clear() {
    beginResetModel();
    groupedLines_.clear();
    totalMatchCount_ = 0;
    mem_.clear();
    overrunFile_.clear();
    overrunCount_ = 0;
    endResetModel();
}

void SearchResultsModel::buildGrouped(const QVector<LargeFileSearchMatch>& matches) {
    // 按 line 分组，同一行的匹配合并到一条 GroupedSearchLine
    // matches 已按 byteOffset 排序（LargeFileSearch 保证），因此同一行内也是有序的
    groupedLines_.clear();
    if (matches.isEmpty()) return;

    int currentLine = -1;
    GroupedSearchLine* cur = nullptr;

    for (const auto& m : matches) {
        if (m.line != currentLine) {
            // 新行
            GroupedSearchLine g;
            g.line = m.line;
            g.lineContext = m.lineContext;
            g.firstByteOffset = m.byteOffset;
            g.firstLength = m.length;
            g.matchPositions.append(qMakePair(m.col, m.length));
            g.matchByteRanges.append(qMakePair(m.byteOffset, m.length));
            groupedLines_.append(std::move(g));
            cur = &groupedLines_.last();
            currentLine = m.line;
        } else {
            // 同一行的后续匹配
            if (cur) {
                cur->matchPositions.append(qMakePair(m.col, m.length));
                cur->matchByteRanges.append(qMakePair(m.byteOffset, m.length));
            }
        }
    }
}

void SearchResultsModel::loadOverrunIntoGrouped() {
    if (overrunCount_ <= 0 || overrunFile_.isEmpty() || !QFile::exists(overrunFile_))
        return;

    QFile f(overrunFile_);
    if (!f.open(QIODevice::ReadOnly)) return;
    f.seek(16); // 跳过 magic(4)+version(4)+count(8)
    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);

    // 读入所有 overrun matches
    QVector<LargeFileSearchMatch> overrunMatches;
    overrunMatches.reserve(int(qMin<qint64>(overrunCount_, 1000000)));
    while (!s.atEnd()) {
        quint64 bo; qint32 len, ln, col; QByteArray ctx;
        s >> bo >> len >> ln >> col >> ctx;
        if (s.status() != QDataStream::Ok) break;
        LargeFileSearchMatch m;
        m.byteOffset = qint64(bo);
        m.length = len;
        m.line = ln;
        m.col = col;
        m.lineContext = ctx;
        overrunMatches.append(std::move(m));
    }
    f.close();

    // 合并 mem_ + overrunMatches，按 byteOffset 排序后重新分组
    QVector<LargeFileSearchMatch> all;
    all.reserve(mem_.size() + overrunMatches.size());
    all.append(mem_);
    all.append(overrunMatches);
    std::sort(all.begin(), all.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                  return a.byteOffset < b.byteOffset;
              });
    buildGrouped(all);
}

void SearchResultsModel::setResults(const QVector<LargeFileSearchMatch>& memoryMatches,
                                    const QString& overrunFilePath, qint64 overrunCount)
{
    beginResetModel();
    mem_ = memoryMatches;
    overrunFile_ = overrunFilePath;
    overrunCount_ = overrunCount;
    totalMatchCount_ = qint64(memoryMatches.size()) + overrunCount;

    // 按 byteOffset 排序内存 matches（保证同 line 内有序）
    QVector<LargeFileSearchMatch> sorted = memoryMatches;
    std::sort(sorted.begin(), sorted.end(),
              [](const LargeFileSearchMatch& a, const LargeFileSearchMatch& b) {
                  return a.byteOffset < b.byteOffset;
              });
    buildGrouped(sorted);

    // 如果有溢出文件，读取并合并到分组
    if (overrunCount > 0) {
        loadOverrunIntoGrouped();
    }

    endResetModel();
}

qint64 SearchResultsModel::matchByteOffset(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= groupedLines_.size()) return -1;
    return groupedLines_[index.row()].firstByteOffset;
}

int SearchResultsModel::matchLength(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= groupedLines_.size()) return 0;
    return groupedLines_[index.row()].firstLength;
}

bool SearchResultsModel::matchByteOffsetAtIndex(int row, int matchIdx, qint64* outByteOffset, int* outLength) const {
    if (row < 0 || row >= groupedLines_.size()) return false;
    const GroupedSearchLine& g = groupedLines_[row];
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
    if (row < 0 || row >= groupedLines_.size()) return -1;
    const GroupedSearchLine& g = groupedLines_[row];
    if (g.matchPositions.isEmpty()) return -1;

    // 左 padding 4px（与 delegate 的 textRect.adjusted(4,0,-4,0) 一致）
    const int pad = 4;
    int x = xInColumn - pad;
    if (x < 0) return -1;

    QString text = QString::fromUtf8(g.lineContext);
    if (text.isEmpty()) return -1;

    // 用 fm 的 char 宽度找到点击位置对应的 UTF-16 字符索引
    // 等宽字体下：col = x / charWidth
    int charW = fm.horizontalAdvance(QLatin1Char('0'));
    if (charW <= 0) charW = 8;
    int colApprox = x / qMax(1, charW);
    // 在 colApprox 附近精确匹配（±10 范围）
    int lo = qMax(0, colApprox - 10);
    int hi = qMin(text.size(), colApprox + 10);
    int bestIdx = -1;
    int bestXDiff = INT_MAX;
    for (int c = lo; c < hi; ++c) {
        int cx = fm.horizontalAdvance(text.left(c + 1));
        int diff = qAbs(cx - x);
        if (diff < bestXDiff) { bestXDiff = diff; bestIdx = c + 1; }
    }
    if (bestIdx < 0) bestIdx = colApprox;
    int clickCol = bestIdx; // UTF-16 字符索引（近似 UTF-8 col）

    // 遍历 matchPositions，找哪个 match 覆盖 clickCol
    for (int i = 0; i < g.matchPositions.size(); ++i) {
        int mc = g.matchPositions[i].first;
        int ml = g.matchPositions[i].second;
        // 近似：matchPositions.col 是 UTF-8 字节偏移，我们近似相等（因为 ASCII 时一致）
        // 给出 ±3 的容差
        if (clickCol >= mc - 3 && clickCol <= mc + ml + 3) {
            return i;
        }
    }
    // 如果没有覆盖的，选最近的一个
    int nearest = -1;
    int nearestDist = INT_MAX;
    for (int i = 0; i < g.matchPositions.size(); ++i) {
        int mc = g.matchPositions[i].first;
        int dist = qAbs(mc - clickCol);
        if (dist < nearestDist) { nearestDist = dist; nearest = i; }
    }
    return nearest;
}

const GroupedSearchLine& SearchResultsModel::groupedLineAt(int row) const {
    static GroupedSearchLine empty;
    if (row < 0 || row >= groupedLines_.size()) return empty;
    return groupedLines_[row];
}

QModelIndex SearchResultsModel::index(int row, int column, const QModelIndex& parent) const {
    if (parent.isValid()) return {};
    if (row < 0 || row >= groupedLines_.size()) return {};
    if (column < 0 || column >= COL_COUNT) return {};
    return createIndex(row, column, nullptr);
}

QModelIndex SearchResultsModel::parent(const QModelIndex&) const { return {}; }

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return groupedLines_.size();
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
    case COL_LINE:       return QStringLiteral("行");
    case COL_CONTEXT:    return QStringLiteral("内容");
    case COL_MATCHCOUNT: return QStringLiteral("匹配数");
    default: return {};
    }
}

QVariant SearchResultsModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid()) return {};
    if (idx.row() < 0 || idx.row() >= groupedLines_.size()) return {};

    const GroupedSearchLine& g = groupedLines_[idx.row()];

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
    // ToolTip：显示全部匹配词的位置+列，便于查看超长行中的多匹配
    if (role == Qt::ToolTipRole) {
        switch (idx.column()) {
        case COL_CONTEXT: {
            QString tt = QObject::tr("第 %1 行共 %2 处匹配\n")
                .arg(g.line + 1).arg(g.matchPositions.size());
            for (int i = 0; i < g.matchPositions.size() && i < 50; ++i) {
                int col = g.matchPositions[i].first;
                int len = g.matchPositions[i].second;
                qint64 bo = i < g.matchByteRanges.size() ? g.matchByteRanges[i].first : g.firstByteOffset;
                // 截取附近上下文（最多 60 字符）
                QString lineText = QString::fromUtf8(g.lineContext);
                int ctxStart = qMax(0, col - 10);
                int ctxEnd = qMin(lineText.size(), col + len + 20);
                QString snippet = lineText.mid(ctxStart, ctxEnd - ctxStart);
                tt += QObject::tr("  #%1 列%2-%3(字节%4): …%5…\n")
                    .arg(i + 1).arg(col + 1).arg(col + len).arg(bo).arg(snippet);
            }
            if (g.matchPositions.size() > 50) {
                tt += QObject::tr("  …还有 %1 处匹配").arg(g.matchPositions.size() - 50);
            }
            tt += QObject::tr("\n双击任意匹配词可跳转到正文对应位置（根据点击位置选择匹配词）");
            return tt;
        }
        case COL_MATCHCOUNT:
            return QObject::tr("该行有 %1 处匹配").arg(g.matchPositions.size());
        default:
            break;
        }
    }
    if (role != Qt::DisplayRole) return {};

    switch (idx.column()) {
    case COL_LINE:
        return QString::number(g.line + 1);  // UI 1-based
    case COL_CONTEXT:
        return QString::fromUtf8(g.lineContext);
    case COL_MATCHCOUNT:
        return QString::number(g.matchPositions.size());
    default:
        return {};
    }
}
