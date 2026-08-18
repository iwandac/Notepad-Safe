#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QVector>

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QComboBox;
class QTabWidget;
class EditorTab;
class SearchResultsDock;

// 非模态查找/替换对话框
class FindReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindReplaceDialog(QWidget *parent = nullptr);

    void setActiveTab(EditorTab *tab);
    void setSearchResultsDock(SearchResultsDock *dock);
    void setTabWidget(QTabWidget *tabs);
    // Adds a document opened while an all-open Find All generation is still
    // running. Completed searches remain stable until the user reruns them.
    void notifyTabOpened(EditorTab *tab);
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
    void rerunInvalidatedSearch(EditorTab *preferredTarget);

    // L2 异步搜索回调
    void onL2FindAllProgress(int percent, qint64 done, qint64 total);
    void onL2FindAllFinished(class SearchResultsModel* model,
                             const QString& pattern,
                             const QString& fileName);

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
    QPushButton *m_findNextBtn = nullptr;
    QPushButton *m_findPrevBtn = nullptr;
    QPushButton *m_replaceBtn = nullptr;
    QPushButton *m_replaceAllBtn = nullptr;
    QPushButton *m_findAllBtn = nullptr;
    QPushButton *m_cancelSearchBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel    *m_statusLabel = nullptr;   // 异步搜索状态提示（L2 搜索中显示"搜索 42%…"）
    QComboBox *m_scopeCombo = nullptr;

    EditorTab *m_tab = nullptr;
    SearchResultsDock *m_resultsDock = nullptr;
    QTabWidget *m_tabWidget = nullptr;

    // 上一个 EditorTab 的 L2 信号连接（切换 tab 时需要 disconnect）
    QMetaObject::Connection m_l2ProgressConn;
    QMetaObject::Connection m_l2PartialConn;
    QMetaObject::Connection m_l2FinishedConn;
    QMetaObject::Connection m_rerunConnection;
    bool m_l2SearchInProgress = false;
    bool m_replaceMode = false;
    int m_multiSearchGeneration = 0;
    int m_pendingMultiL2 = 0;
    QVector<QMetaObject::Connection> m_multiConnections;
    QVector<QPointer<EditorTab>> m_multiTabs;
    int m_multiNextIndex = 0;
    QString m_multiPattern;
    bool m_multiRegex = false;
    bool m_multiCaseSensitive = false;
    bool m_multiWholeWord = false;
    bool m_multiHexBytes = false;
    int m_multiSkippedTextTargets = 0;
    int m_multiSkippedLockedTargets = 0;
    bool m_multiDispatchComplete = false;
    bool m_multiCancelRequested = false;

    void buildUi();
    QString currentFindText() const;
    QString currentReplaceText() const;
    void emitFindParameters();
    // L2 异步搜索期间：按钮启用/禁用 + 状态提示
    void setL2SearchUiBusy(bool busy, const QString& msg = QString());
    bool allOpenScope() const;
    bool hexBytesMode() const;
    void updateQueryUi();
    QList<EditorTab *> openTabs() const;
    void findAcrossTabs(bool forward);
    void startAllOpenFindAll();
    void dispatchNextMultiTarget(int generation);
    void finishMultiSearchIfReady(int generation);
    void cancelActiveSearch();
};
