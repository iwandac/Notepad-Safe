#include "SearchResultsViewPolicy.h"

#include "SearchResultsDelegate.h"
#include "SearchResultsModel.h"

#include <QAbstractItemModel>
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QTreeView>

void SearchResultsViewPolicy::configure(QTreeView *view)
{
    if (!view)
        return;
    view->setAlternatingRowColors(true);
    view->setUniformRowHeights(true);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover, true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (!view->itemDelegate()
        || view->itemDelegate()->objectName()
               != QLatin1String("searchResultsDelegate")) {
        auto *delegate = createSearchResultsDelegate(view);
        delegate->setObjectName(QStringLiteral("searchResultsDelegate"));
        view->setItemDelegate(delegate);
    }
    QHeaderView *header = view->header();
    header->setStretchLastSection(false);
    if (view->model()
        && view->model()->columnCount() >= SearchResultsModel::COL_COUNT) {
        header->setSectionResizeMode(SearchResultsModel::COL_LINE,
                                     QHeaderView::ResizeToContents);
        header->setSectionResizeMode(SearchResultsModel::COL_CONTEXT,
                                     QHeaderView::Stretch);
        header->setSectionResizeMode(SearchResultsModel::COL_MATCHCOUNT,
                                     QHeaderView::ResizeToContents);
    }
}

void SearchResultsViewPolicy::bindModel(QTreeView *view,
                                        QAbstractItemModel *model,
                                        bool hierarchical)
{
    if (!view)
        return;
    view->setModel(model);
    view->setRootIsDecorated(hierarchical);
    // QTreeView rebuilds header sections when a model is replaced, therefore
    // the invariant must be re-applied after every bind.
    configure(view);
}
