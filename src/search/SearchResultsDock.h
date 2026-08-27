#pragma once

#include <QDockWidget>
#include <memory>
#include "SearchResultTarget.h"
#include "SearchMatch.h"
#include "SearchResultsModel.h"

class QTreeView;
class QLabel;
class QPushButton;
class QStackedWidget;
class MultiSearchResultsModel;
class SearchResultsController;
class LineIndex;

// 搜索结果 Dock 窗口
class SearchResultsDock : public QDockWidget {
    Q_OBJECT
public:
    explicit SearchResultsDock(QWidget *parent = nullptr);
    ~SearchResultsDock() override;

    // ---- Editable Text results are adapted to the canonical result model.
    void setResults(const QVector<SearchMatch> &matches, const QString &pattern,
                    const QString &fileName,
                    const SearchResultTarget &target = {},
                    bool incomplete = false);

    // ---- Canonical result path: SearchResultsModel -> QTreeView.
    // model 通常以 Dock 为 QObject parent；多文件路径会显式接管所有权。
    void setResultsModel(SearchResultsModel *model, const QString &pattern,
                         const QString &fileName,
                         const SearchResultTarget &target = {});
    void beginSingleFileResults(const QString &pattern, const QString &fileName,
                                const SearchResultTarget &target,
                                TextEncoding encoding,
                                bool bytePositionMode);
    void appendSingleFilePreview(const SearchResultTarget &target,
                                 const QVector<LargeFileSearchMatch> &batch,
                                 TextEncoding encoding, bool bytePositionMode);
    void beginMultiFileResults(const QString &pattern, int targetCount);
    void beginMultiFileTarget(const SearchResultTarget &target,
                              TextEncoding encoding,
                              bool bytePositionMode);
    void appendMultiFilePreview(const SearchResultTarget &target,
                                const QVector<LargeFileSearchMatch> &batch,
                                TextEncoding encoding, bool bytePositionMode);
    void appendMultiFileResults(const SearchResultTarget &target,
                                const QVector<SearchMatch> &matches,
                                bool incomplete = false);
    // Transfers model ownership to the dock's virtual multi-file model.
    void appendMultiFileResults(const SearchResultTarget &target,
                                SearchResultsModel *model);
    void finishMultiFileResults(bool cancelled = false);
    void clearResults();
    void invalidateResultsFor(const QUuid &documentId);
    void closeResultsFor(const QUuid &documentId);
    void decorateResultsFor(const QUuid &documentId, const LineIndex &lineIndex);

signals:
    void rerunRequested(const QUuid &preferredDocumentId,
                        quint64 expectedRevision);
    void clearSearchHighlightsRequested(const QUuid &documentId);
    // All result rows use source byte offsets as the navigation authority.
    void jumpToByteOffset(qint64 byteOffset, int length);
    void jumpToDocumentByteOffset(const QUuid &documentId,
                                  quint64 expectedRevision,
                                  qint64 byteOffset, int length);

protected:
    // 事件过滤器：监听 m_treeView viewport 的鼠标事件，用于"点击哪一个匹配词"
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onModelDoubleClicked(const QModelIndex &index);

private:
    QStackedWidget *m_stack   = nullptr;
    QTreeView      *m_treeView = nullptr; // 全部结果共用的虚拟化控件
    SearchResultsModel *m_model = nullptr; // 当前绑定的统一结果模型（非 owning）
    MultiSearchResultsModel *m_multiModel = nullptr;
    QLabel         *m_summary = nullptr;
    QPushButton    *m_rerunButton = nullptr;

    // 上次按下位置对应的"行 / 匹配索引"（用于双击时跳转到指定匹配）
    int m_lastHitMatchIdx = -1;
    int m_lastHitRow = -1;
    std::unique_ptr<SearchResultsController> m_controller;
    QVector<LargeFileSearchMatch> m_singlePreviewMatches;

    void updateSummary(const QString &pattern, const QString &fileName,
                       qint64 totalMatches);
    void setRerunAvailable(bool available,
                           const SearchResultTarget &preferredTarget = {});
};
