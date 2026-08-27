#pragma once

class QAbstractItemModel;
class QTreeView;

// One presentation policy for every result source.  Single-file, multi-file,
// editable, mmap text and Hex results may replace the model, but they may not
// replace the dock's compact geometry or column contract.
class SearchResultsViewPolicy final {
public:
    static void configure(QTreeView *view);
    static void bindModel(QTreeView *view, QAbstractItemModel *model,
                          bool hierarchical);
};
