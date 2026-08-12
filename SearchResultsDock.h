#pragma once

#include <QDockWidget>
#include "EditorTab.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

// 搜索结果 Dock 窗口
class SearchResultsDock : public QDockWidget {
    Q_OBJECT
public:
    explicit SearchResultsDock(QWidget *parent = nullptr);

    void setResults(const QVector<SearchMatch> &matches, const QString &pattern,
                    const QString &fileName);

signals:
    // 双击结果跳转到指定位置
    void jumpTo(int line, int index);

private slots:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);

private:
    QTreeWidget *m_tree = nullptr;
    QLabel *m_summary = nullptr;
};
