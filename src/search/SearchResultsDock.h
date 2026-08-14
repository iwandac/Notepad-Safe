#pragma once

#include <QDockWidget>
#include "EditorTab.h"
#include "SearchResultsModel.h"

class QTreeWidget;
class QTreeView;
class QTreeWidgetItem;
class QLabel;
class QStackedWidget;
class QMouseEvent;

// 搜索结果 Dock 窗口
class SearchResultsDock : public QDockWidget {
    Q_OBJECT
public:
    explicit SearchResultsDock(QWidget *parent = nullptr);

    // ---- L1（QsciScintilla）路径：QVector<SearchMatch> → QTreeWidget
    void setResults(const QVector<SearchMatch> &matches, const QString &pattern,
                    const QString &fileName);

    // ---- L2 虚拟化路径：SearchResultsModel（内存 + overrun.bin 二分读取）→ QTreeView
    // model：调用方持有生命周期；Dock 仅使用不拥有。传 nullptr = 清空。
    void setResultsModel(SearchResultsModel *model, const QString &pattern,
                         const QString &fileName);

signals:
    // L1 跳转（line, index：Scintilla 的 0-based 行/列）
    void jumpTo(int line, int index);
    // L2 跳转（byteOffset：虚拟字节偏移；length：匹配长度用于定位选中）
    void jumpToByteOffset(qint64 byteOffset, int length);

protected:
    // 事件过滤器：监听 m_treeView viewport 的鼠标事件，用于"点击哪一个匹配词"
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onModelDoubleClicked(const QModelIndex &index);

private:
    QStackedWidget *m_stack   = nullptr;  // 0: QTreeWidget（L1）, 1: QTreeView（L2）
    QTreeWidget    *m_tree    = nullptr;  // L1 控件
    QTreeView      *m_treeView = nullptr; // L2 虚拟化控件
    SearchResultsModel *m_model = nullptr; // 当前绑定的 L2 模型（非 owning）
    QLabel         *m_summary = nullptr;

    // 上次按下位置对应的"行 / 匹配索引"（用于双击时跳转到指定匹配）
    int m_lastHitMatchIdx = -1;
    int m_lastHitRow = -1;

    void updateSummary(const QString &pattern, const QString &fileName,
                       qint64 totalMatches);
};
