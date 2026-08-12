#include "FindReplaceDialog.h"
#include "EditorTab.h"
#include "SearchResultsDock.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QRegularExpression>
#include <QMessageBox>

FindReplaceDialog::FindReplaceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("查找 / 替换"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    buildUi();
}

void FindReplaceDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // 查找输入
    auto *findLayout = new QHBoxLayout;
    m_findLabel = new QLabel(QStringLiteral("查找:"));
    m_findEdit = new QLineEdit;
    m_findEdit->setClearButtonEnabled(true);
    findLayout->addWidget(m_findLabel);
    findLayout->addWidget(m_findEdit);
    layout->addLayout(findLayout);

    // 替换输入
    auto *replaceLayout = new QHBoxLayout;
    m_replaceLabel = new QLabel(QStringLiteral("替换:"));
    m_replaceEdit = new QLineEdit;
    m_replaceEdit->setClearButtonEnabled(true);
    replaceLayout->addWidget(m_replaceLabel);
    replaceLayout->addWidget(m_replaceEdit);
    layout->addLayout(replaceLayout);

    // 选项
    auto *optionsLayout = new QHBoxLayout;
    m_regexCheck = new QCheckBox(QStringLiteral("正则表达式"));
    m_caseCheck = new QCheckBox(QStringLiteral("区分大小写"));
    m_wordCheck = new QCheckBox(QStringLiteral("整词匹配"));
    m_wrapCheck = new QCheckBox(QStringLiteral("循环查找"));
    m_wrapCheck->setChecked(true);
    optionsLayout->addWidget(m_regexCheck);
    optionsLayout->addWidget(m_caseCheck);
    optionsLayout->addWidget(m_wordCheck);
    optionsLayout->addWidget(m_wrapCheck);
    optionsLayout->addStretch();
    layout->addLayout(optionsLayout);

    // 按钮
    auto *btnLayout = new QHBoxLayout;
    m_findNextBtn = new QPushButton(QStringLiteral("查找下一个"));
    m_findPrevBtn = new QPushButton(QStringLiteral("查找上一个"));
    m_replaceBtn = new QPushButton(QStringLiteral("替换"));
    m_replaceAllBtn = new QPushButton(QStringLiteral("全部替换"));
    m_findAllBtn = new QPushButton(QStringLiteral("查找全部"));
    m_closeBtn = new QPushButton(QStringLiteral("关闭"));
    btnLayout->addWidget(m_findNextBtn);
    btnLayout->addWidget(m_findPrevBtn);
    btnLayout->addWidget(m_replaceBtn);
    btnLayout->addWidget(m_replaceAllBtn);
    btnLayout->addWidget(m_findAllBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_closeBtn);
    layout->addLayout(btnLayout);

    // 连接
    connect(m_findNextBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindNext);
    connect(m_findPrevBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindPrev);
    connect(m_replaceBtn, &QPushButton::clicked, this, &FindReplaceDialog::onReplace);
    connect(m_replaceAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAll);
    connect(m_findAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindAll);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &FindReplaceDialog::onFindNext);
    // 查找参数变化时通知主窗口
    connect(m_findEdit, &QLineEdit::textChanged, this, &FindReplaceDialog::emitFindParameters);
    connect(m_regexCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
    connect(m_caseCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
    connect(m_wordCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
}

void FindReplaceDialog::setActiveTab(EditorTab *tab)
{
    m_tab = tab;
}

void FindReplaceDialog::setSearchResultsDock(SearchResultsDock *dock)
{
    m_resultsDock = dock;
}

void FindReplaceDialog::setReplaceMode(bool replace)
{
    m_replaceLabel->setVisible(replace);
    m_replaceEdit->setVisible(replace);
    m_replaceBtn->setVisible(replace);
    m_replaceAllBtn->setVisible(replace);
}

void FindReplaceDialog::showFindMode()
{
    setReplaceMode(false);
    setWindowTitle(QStringLiteral("查找"));
    show();
    raise();
    activateWindow();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void FindReplaceDialog::showReplaceMode()
{
    setReplaceMode(true);
    setWindowTitle(QStringLiteral("替换"));
    show();
    raise();
    activateWindow();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

QString FindReplaceDialog::currentFindText() const
{
    return m_findEdit->text();
}

QString FindReplaceDialog::currentReplaceText() const
{
    return m_replaceEdit->text();
}

void FindReplaceDialog::emitFindParameters()
{
    emit findParametersChanged(m_findEdit->text(), m_regexCheck->isChecked(),
                               m_caseCheck->isChecked(), m_wordCheck->isChecked());
}

void FindReplaceDialog::onFindNext()
{
    if (!m_tab) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    m_tab->findNext(pattern, m_regexCheck->isChecked(),
                    m_caseCheck->isChecked(), m_wordCheck->isChecked(),
                    true, m_wrapCheck->isChecked());
}

void FindReplaceDialog::onFindPrev()
{
    if (!m_tab) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    m_tab->findNext(pattern, m_regexCheck->isChecked(),
                    m_caseCheck->isChecked(), m_wordCheck->isChecked(),
                    false, m_wrapCheck->isChecked());
}

void FindReplaceDialog::onReplace()
{
    if (!m_tab) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    // 先查找当前匹配，然后替换，然后跳到下一个
    bool found = m_tab->findNext(pattern, m_regexCheck->isChecked(),
                                 m_caseCheck->isChecked(), m_wordCheck->isChecked(),
                                 true, m_wrapCheck->isChecked());
    if (found) {
        m_tab->replaceCurrent(currentReplaceText());
        m_tab->findNext(pattern, m_regexCheck->isChecked(),
                        m_caseCheck->isChecked(), m_wordCheck->isChecked(),
                        true, m_wrapCheck->isChecked());
    }
}

void FindReplaceDialog::onReplaceAll()
{
    if (!m_tab) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    int count = m_tab->replaceAll(pattern, currentReplaceText(),
                                  m_regexCheck->isChecked(),
                                  m_caseCheck->isChecked(),
                                  m_wordCheck->isChecked());
    QMessageBox::information(this, QStringLiteral("替换完成"),
                             QStringLiteral("已替换 %1 处匹配。").arg(count));
}

void FindReplaceDialog::onFindAll()
{
    if (!m_tab || !m_resultsDock) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();

    auto matches = m_tab->findAll(pattern, m_regexCheck->isChecked(),
                                  m_caseCheck->isChecked(),
                                  m_wordCheck->isChecked());
    m_resultsDock->setResults(matches, pattern, m_tab->fileName());
    m_resultsDock->show();
    m_resultsDock->raise();
}
