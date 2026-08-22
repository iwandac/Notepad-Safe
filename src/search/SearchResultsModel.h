#pragma once

#include <QAbstractItemModel>
#include <QVector>
#include <QString>
#include <QFile>
#include <QDataStream>
#include <QFontMetrics>

#include "LargeFileSearch.h"  // LargeFileSearchMatch
#include "TextEncoding.h"

class LineIndex;

// ============================================================================
//  SearchResultsModel — 给 SearchResultsDock 的 QAbstractItemModel。
//
//  按行去重：同一行的多个匹配只显示一行，匹配位置存储在 matchPositions 中。
//  Dock 的自定义 delegate 用 matchPositions 对上下文列中的关键词染色。
//
//  列定义（3 列）：
//     0: 行号 (1-based)
//     1: 上下文（整行文本，delegate 对匹配段染色）
//     2: 匹配数（该行匹配了几次）
//
//  UserRole+1：返回 firstByteOffset (qint64)；双击时 Dock 用它 gotoByteOffset。
//  UserRole+2：返回 firstLength (int)。
// ============================================================================

// 一行的分组结果：同一行所有匹配合并为一条
struct GroupedSearchLine {
    qint32 line = 0;                    // 0-based 行号
    qint64 contextByteOffset = 0;       // lineContext 起点的全局字节位置
    QByteArray lineContext;             // 整行文本
    QVector<QPair<qint32, qint32>> matchPositions; // (UTF-16 start, length)，用于精确绘制/点击
    QVector<QPair<qint64, qint32>> matchByteRanges; // (byteOffset, length) 每个匹配的字节范围（用于按"指定匹配"跳转）
    qint64 firstByteOffset = 0;         // 第一个匹配的字节偏移（双击跳转用，保持兼容）
    qint32 firstLength = 0;             // 第一个匹配的长度（保持兼容）
};

class SearchResultsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Columns { COL_LINE = 0, COL_CONTEXT, COL_MATCHCOUNT, COL_COUNT };
    enum Roles {
        ByteOffsetRole = Qt::UserRole + 1,  // 首匹配 byteOffset
        LengthRole = Qt::UserRole + 2,      // 首匹配 length
        MatchIndexRole = Qt::UserRole + 3,  // 点击上下文时用：鼠标 x 映射到第几个匹配，返回 index
        MatchCountInLineRole = Qt::UserRole + 4,  // 本行匹配个数
        MatchByteRangesRole = Qt::UserRole + 5,   // 返回字节范围列表
        LineRole = Qt::UserRole + 6,
        ContextRole = Qt::UserRole + 7,
        MatchPositionsRole = Qt::UserRole + 8     // UTF-16 上下文内的高亮范围
    };

    explicit SearchResultsModel(QObject* parent = nullptr);
    ~SearchResultsModel() override;

    // 设置数据源：内存 matches + 溢出文件路径
    // 内部自动按行分组去重
    void setResults(const QVector<LargeFileSearchMatch>& memoryMatches,
                    const QString& overrunFilePath,
                    qint64 overrunCount,
                    const QVector<QPair<qint64, qint64>>& overrunCheckpoints = {},
                    qint64 overrunGroupedCount = -1,
                    quint32 overrunVersion = 0);

    void clear();

    int groupedCount() const;
    qint64 totalMatches() const { return totalMatchCount_; }
    void setIncompleteReason(const QString &reason) { incompleteReason_ = reason; }
    QString incompleteReason() const { return incompleteReason_; }
    bool isIncomplete() const { return !incompleteReason_.isEmpty(); }
    void setBytePositionMode(bool enabled) { bytePositionMode_ = enabled; }
    bool bytePositionMode() const { return bytePositionMode_; }
    void setTextEncoding(TextEncoding encoding) { textEncoding_ = encoding; }
    bool hasUnknownLineNumbers() const;

    // Search is allowed to publish byte-offset results before the background
    // line index is complete.  Once a completed index becomes available,
    // decorate the in-memory result prefix in place; authoritative byte ranges
    // and navigation do not change and the search is not rerun.
    bool decorateLineNumbers(const LineIndex &lineIndex);

    // 给 Dock 双击跳转用（保持兼容：返回首匹配）
    qint64 matchByteOffset(const QModelIndex& index) const;
    int matchLength(const QModelIndex& index) const;

    // 按匹配索引跳转：matchIdx = -1 时返回首匹配
    bool matchByteOffsetAtIndex(int row, int matchIdx, qint64* outByteOffset, int* outLength) const;

    // 上下文列 x 像素映射为"第几个匹配"；返回 -1 表示不在任何匹配区
    int hitTestMatchIndex(int row, int xInColumn, int columnWidth, const QFontMetrics& fm) const;

    // 给 delegate 用：获取某行的分组数据
    const GroupedSearchLine& groupedLineAt(int row) const;
    // Only the in-memory prefix. Call groupedLineAt(row) for virtual rows.
    const QVector<GroupedSearchLine>& groupedLines() const { return groupedLines_; }

    // QAbstractItemModel 实现
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    QVector<LargeFileSearchMatch> memoryMatches_;
    QVector<GroupedSearchLine> groupedLines_;
    qint64 totalMatchCount_ = 0;
    QString incompleteReason_;
    bool bytePositionMode_ = false;
    TextEncoding textEncoding_ = TextEncoding::Utf8;

    QString overrunFile_;
    qint64 overrunCount_ = 0;

    struct OverrunCheckpoint {
        qint64 groupedRow = 0;
        qint64 fileOffset = 0;
    };
    QVector<OverrunCheckpoint> overrunCheckpoints_;
    qint64 overrunGroupedCount_ = 0;
    quint32 overrunVersion_ = 0;
    qint64 overrunDataOffset_ = 0;
    mutable qint64 cachedOverrunRow_ = -1;
    mutable GroupedSearchLine cachedOverrunLine_;

    // 从 flat matches 构建分组
    void buildGrouped(const QVector<LargeFileSearchMatch>& matches);
    void buildOverrunIndex();
    bool readOverrunGroupedLine(qint64 relativeRow, GroupedSearchLine *line) const;
    const GroupedSearchLine *lineForRow(int row) const;
};
