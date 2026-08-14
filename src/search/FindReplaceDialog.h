#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class EditorTab;
class SearchResultsDock;

// 非模态查找/替换对话框
class FindReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindReplaceDialog(QWidget *parent = nullptr);

    void setActiveTab(EditorTab *tab);
    void setSearchResultsDock(SearchResultsDock *dock);
    void setReplaceMode(bool replace);
    void showFindMode();
    void showReplaceMode();

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
    QCheckBox *m_caseCheck = nullptr;
    QCheckBox *m_wordCheck = nullptr;
    QCheckBox *m_wrapCheck = nullptr;
    QPushButton *m_findNextBtn = nullptr;
    QPushButton *m_findPrevBtn = nullptr;
    QPushButton *m_replaceBtn = nullptr;
    QPushButton *m_replaceAllBtn = nullptr;
    QPushButton *m_findAllBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QLabel    *m_statusLabel = nullptr;   // 异步搜索状态提示（L2 搜索中显示"搜索 42%…"）

    EditorTab *m_tab = nullptr;
    SearchResultsDock *m_resultsDock = nullptr;

    // 上一个 EditorTab 的 L2 信号连接（切换 tab 时需要 disconnect）
    QMetaObject::Connection m_l2ProgressConn;
    QMetaObject::Connection m_l2FinishedConn;
    bool m_l2SearchInProgress = false;

    void buildUi();
    QString currentFindText() const;
    QString currentReplaceText() const;
    void emitFindParameters();
    // L2 异步搜索期间：按钮启用/禁用 + 状态提示
    void setL2SearchUiBusy(bool busy, const QString& msg = QString());
};
