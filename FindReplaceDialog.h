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

    EditorTab *m_tab = nullptr;
    SearchResultsDock *m_resultsDock = nullptr;

    void buildUi();
    QString currentFindText() const;
    QString currentReplaceText() const;
    void emitFindParameters();
};
