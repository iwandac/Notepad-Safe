#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QUuid>
#include <QVector>
#include "SearchSession.h"

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QTabWidget;
class QWidget;
class DocumentSearchSurface;
class SearchResultsDock;
class SingleFindAllRequest;
class SearchNavigationRequest;

// 非模态查找/替换对话框
class FindReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindReplaceDialog(QWidget *parent = nullptr);

    void setActiveTab(DocumentSearchSurface *tab);
    void setSearchResultsDock(SearchResultsDock *dock);
    void setTabWidget(QTabWidget *tabs);
    // Adds a document opened while an all-open Find All generation is still
    // running. Completed searches remain stable until the user reruns them.
    void notifyTabOpened(DocumentSearchSurface *tab);
    void setReplaceMode(bool replace);
    void showFindMode();
    void showReplaceMode();
    void findNextFromShortcut(bool forward);
    void clearSensitiveState();

signals:
    // 通知主窗口当前查找参数（供 F3 / Shift+F3 使用）
    void findParametersChanged(const QString &pattern, bool regex,
                               bool caseSensitive, bool wholeWord);

private slots:
    void onFindNext();
    void onFindPrev();
    void onReplace();
    void onReplaceAll();
    void onFindAll();
    void rerunInvalidatedSearch(const QUuid &preferredDocumentId,
                                quint64 expectedRevision);

    // L2 异步搜索回调
    void onL2FindAllProgress(int percent, qint64 done, qint64 total);

private:
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QLabel *m_findLabel = nullptr;
    QLabel *m_replaceLabel = nullptr;
    QCheckBox *m_regexCheck = nullptr;
    QCheckBox *m_hexBytesCheck = nullptr;
    QCheckBox *m_caseCheck = nullptr;
    QCheckBox *m_wordCheck = nullptr;
    QCheckBox *m_wrapCheck = nullptr;
    QPushButton *m_regexAssistantToggle = nullptr;
    QWidget *m_regexAssistant = nullptr;
    QLabel *m_queryHintLabel = nullptr;
    QPushButton *m_findNextBtn = nullptr;
    QPushButton *m_findPrevBtn = nullptr;
    QPushButton *m_replaceBtn = nullptr;
    QPushButton *m_replaceAllBtn = nullptr;
    QPushButton *m_findAllBtn = nullptr;
    QPushButton *m_cancelSearchBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel    *m_statusLabel = nullptr;   // 异步搜索状态提示（L2 搜索中显示"搜索 42%…"）
    QComboBox *m_scopeCombo = nullptr;

    DocumentSearchSurface *m_tab = nullptr;
    SearchResultsDock *m_resultsDock = nullptr;
    QTabWidget *m_tabWidget = nullptr;

    QPointer<SingleFindAllRequest> m_singleRequest;
    QPointer<SearchNavigationRequest> m_navigationRequest;
    QMetaObject::Connection m_rerunConnection;
    bool m_l2SearchInProgress = false;
    bool m_allowNavigationDuringBusy = false;
    bool m_replaceMode = false;
    SearchSession m_searchSession;
    QVector<QMetaObject::Connection> m_multiConnections;
    QVector<QPointer<DocumentSearchSurface>> m_multiTabs;
    int m_multiNextIndex = 0;

    void buildUi();
    void disconnectSingleSearch();
    void finishSingleSearch();
    void onL2FindAllFinished(QPointer<SingleFindAllRequest> request,
                            QPointer<DocumentSearchSurface> target,
                            class SearchResultsModel *model,
                            const QString &pattern, const QString &fileName);
    QString currentFindText() const;
    QString currentReplaceText() const;
    void emitFindParameters();
    // L2 异步搜索期间：按钮启用/禁用 + 状态提示
    void setL2SearchUiBusy(bool busy, const QString& msg = QString(),
                           bool allowNavigation = false);
    void updateNavigationUi(const QString& msg = QString());
    bool allOpenScope() const;
    bool hexBytesMode() const;
    void updateQueryUi();
    void insertRegexTemplate(const QString &prefix, const QString &suffix,
                             const QString &placeholder);
    QList<DocumentSearchSurface *> openTabs() const;
    void findAcrossTabs(bool forward);
    void startAllOpenFindAll();
    void dispatchNextMultiTarget(int generation);
    void finishMultiSearchIfReady(int generation);
    void cancelActiveSearch();
};
