#pragma once

#include <QAbstractItemModel>
#include <QVector>
#include <QString>
#include <QFile>
#include <QDataStream>
#include <QFontMetrics>

#include "LargeFileSearch.h"  // LargeFileSearchMatch

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
    QByteArray lineContext;             // 整行文本
    QVector<QPair<qint32, qint32>> matchPositions; // (col, length) 列内的匹配位置
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
        MatchByteRangesRole = Qt::UserRole + 5    // 返回 QList<QPair<qint64,qint32>> 字节范围列表
    };

    explicit SearchResultsModel(QObject* parent = nullptr);
    ~SearchResultsModel() override;

    // 设置数据源：内存 matches + 溢出文件路径
    // 内部自动按行分组去重
    void setResults(const QVector<LargeFileSearchMatch>& memoryMatches,
                    const QString& overrunFilePath,
                    qint64 overrunCount);

    void clear();

    int groupedCount() const { return groupedLines_.size(); }
    qint64 totalMatches() const { return totalMatchCount_; }

    // 给 Dock 双击跳转用（保持兼容：返回首匹配）
    qint64 matchByteOffset(const QModelIndex& index) const;
    int matchLength(const QModelIndex& index) const;

    // 按匹配索引跳转：matchIdx = -1 时返回首匹配
    bool matchByteOffsetAtIndex(int row, int matchIdx, qint64* outByteOffset, int* outLength) const;

    // 上下文列 x 像素映射为"第几个匹配"；返回 -1 表示不在任何匹配区
    int hitTestMatchIndex(int row, int xInColumn, int columnWidth, const QFontMetrics& fm) const;

    // 给 delegate 用：获取某行的分组数据
    const GroupedSearchLine& groupedLineAt(int row) const;
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
    QVector<GroupedSearchLine> groupedLines_;
    qint64 totalMatchCount_ = 0;

    // 原始数据保留（供 overrun 读取等场景）
    QVector<LargeFileSearchMatch> mem_;
    QString overrunFile_;
    qint64 overrunCount_ = 0;

    // 从 flat matches 构建分组
    void buildGrouped(const QVector<LargeFileSearchMatch>& matches);
    // 从 overrun 文件读取所有匹配并合并到分组
    void loadOverrunIntoGrouped();
};
