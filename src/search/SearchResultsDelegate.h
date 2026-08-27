#pragma once

class QAbstractItemDelegate;
class QFontMetrics;
class QModelIndex;
class QObject;

QAbstractItemDelegate *createSearchResultsDelegate(QObject *parent);
int hitTestSearchResultMatch(const QModelIndex &index, int xInColumn,
                             int columnWidth, const QFontMetrics &metrics);
