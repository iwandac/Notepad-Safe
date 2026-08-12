#include "SearchResultsDock.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QLabel>
#include <QHeaderView>

SearchResultsDock::SearchResultsDock(QWidget *parent)
    : QDockWidget(QStringLiteral("搜索结果"), parent)
{
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetFloatable);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    m_summary = new QLabel(container);
    layout->addWidget(m_summary);

    m_tree = new QTreeWidget(container);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("行"), QStringLiteral("列"),
                             QStringLiteral("内容")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree);

    setWidget(container);

    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &SearchResultsDock::onItemDoubleClicked);
}

void SearchResultsDock::setResults(const QVector<SearchMatch> &matches,
                                   const QString &pattern,
                                   const QString &fileName)
{
    m_tree->clear();
    for (const auto &m : matches) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(m.line + 1));
        item->setText(1, QString::number(m.index + 1));
        item->setText(2, m.context);
        item->setData(0, Qt::UserRole, m.line);
        item->setData(1, Qt::UserRole, m.index);
    }
    m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：找到 %3 处匹配")
                           .arg(fileName)
                           .arg(pattern)
                           .arg(matches.size()));
}

void SearchResultsDock::onItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
    if (!item)
        return;
    int line = item->data(0, Qt::UserRole).toInt();
    int index = item->data(1, Qt::UserRole).toInt();
    emit jumpTo(line, index);
}
