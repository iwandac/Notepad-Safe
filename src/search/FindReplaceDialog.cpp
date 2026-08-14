#include "FindReplaceDialog.h"
#include "EditorTab.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "LargeFileSearch.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QRegularExpression>
#include <QMessageBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QMainWindow>
#include <QStatusBar>

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

    // 异步搜索状态提示（L2 大文件搜索时显示进度；L1 不显示）
    m_statusLabel = new QLabel(QStringLiteral(""));
    m_statusLabel->setStyleSheet(QStringLiteral("color:#888;"));
    layout->addWidget(m_statusLabel);

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
    // 切换 tab：先断开上一个 EditorTab 的 L2 搜索信号
    QObject::disconnect(m_l2ProgressConn);
    QObject::disconnect(m_l2FinishedConn);
    m_l2ProgressConn = QMetaObject::Connection{};
    m_l2FinishedConn = QMetaObject::Connection{};

    if (m_l2SearchInProgress) {
        // 上一个搜索在进行中：UI 恢复
        setL2SearchUiBusy(false);
    }
    m_tab = tab;
    if (m_tab) {
        // 连接新 tab 的 L2 异步搜索信号（即使当前是 L1 也连接，切换到 L2 时正常工作）
        m_l2ProgressConn = QObject::connect(
            m_tab, &EditorTab::findAllL2Progress,
            this,  &FindReplaceDialog::onL2FindAllProgress);
        m_l2FinishedConn = QObject::connect(
            m_tab, &EditorTab::findAllL2Finished,
            this,  &FindReplaceDialog::onL2FindAllFinished);
    }
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
    if (count > 0 && m_tab->isL2Mode() && m_resultsDock) {
        // L2 替换后，旧的 SearchResultsModel 内 byteOffset 已失效，清空避免错误跳转
        m_resultsDock->setResultsModel(nullptr, QString(), QString());
    }
}

void FindReplaceDialog::onFindAll()
{
    if (!m_tab || !m_resultsDock) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();

    if (m_tab->isL2Mode()) {
        // L2 大文件：纯异步 → 经 AsyncTaskManager 派发 → findAllL2Finished 设置 Dock
        setL2SearchUiBusy(true, QObject::tr("准备搜索…"));
        bool started = m_tab->findAllL2Async(pattern, m_regexCheck->isChecked(),
                                             m_caseCheck->isChecked(),
                                             m_wordCheck->isChecked(),
                                             m_resultsDock);
        if (!started) {
            setL2SearchUiBusy(false);
            m_resultsDock->setResults({}, pattern, m_tab->fileName());
            m_resultsDock->show();
            m_resultsDock->raise();
        }
        return;
    }

    // L1 小文件：QsciScintilla 内存扫描 → QTreeWidget
    auto matches = m_tab->findAll(pattern, m_regexCheck->isChecked(),
                                  m_caseCheck->isChecked(),
                                  m_wordCheck->isChecked());
    m_resultsDock->setResults(matches, pattern, m_tab->fileName());
    m_resultsDock->show();
    m_resultsDock->raise();
}

// ============================================================================
//  L2 异步搜索 UI 辅助
// ============================================================================
void FindReplaceDialog::setL2SearchUiBusy(bool busy, const QString& msg)
{
    m_l2SearchInProgress = busy;
    m_findNextBtn->setEnabled(!busy);
    m_findPrevBtn->setEnabled(!busy);
    m_replaceBtn->setEnabled(!busy);
    m_replaceAllBtn->setEnabled(!busy);
    m_findAllBtn->setEnabled(!busy);
    m_findEdit->setEnabled(!busy);
    m_replaceEdit->setEnabled(!busy);
    m_regexCheck->setEnabled(!busy);
    m_caseCheck->setEnabled(!busy);
    m_wordCheck->setEnabled(!busy);
    m_wrapCheck->setEnabled(!busy);
    if (m_statusLabel) m_statusLabel->setText(busy ? msg : QString());
}

void FindReplaceDialog::onL2FindAllProgress(int percent, qint64 done, qint64 total)
{
    if (!m_l2SearchInProgress) return;
    QString msg;
    if (percent < 0) {
        msg = QObject::tr("等待行索引构建完成…");
    } else {
        QString doneStr, totalStr;
        if (total >= qint64(1024)*1024*1024) {
            totalStr = QString::number(double(total)/(1024.0*1024.0*1024.0), 'f', 2) + QObject::tr(" GB");
            doneStr  = QString::number(double(done) /(1024.0*1024.0*1024.0), 'f', 2) + QObject::tr(" GB");
        } else if (total >= qint64(1024)*1024) {
            totalStr = QString::number(double(total)/(1024.0*1024.0), 'f', 2) + QObject::tr(" MB");
            doneStr  = QString::number(double(done) /(1024.0*1024.0), 'f', 2) + QObject::tr(" MB");
        } else {
            totalStr = QString::number(total) + QObject::tr(" B");
            doneStr  = QString::number(done)  + QObject::tr(" B");
        }
        msg = QObject::tr("搜索大文件… %1% (%2 / %3)").arg(percent).arg(doneStr).arg(totalStr);
    }
    if (m_statusLabel) m_statusLabel->setText(msg);
    // 同步刷新 MainWindow 状态栏（如果能拿到）
    if (auto *mw = qobject_cast<QMainWindow*>(window())) {
        mw->statusBar()->showMessage(msg, 0);
    }
}

void FindReplaceDialog::onL2FindAllFinished(SearchResultsModel* model,
                                            const QString& pattern,
                                            const QString& fileName)
{
    setL2SearchUiBusy(false);
    if (!m_resultsDock) {
        // Dock 不存在：model 的 QObject parent 为空（因为我们传 m_resultsDock=nullptr），此时手动删除
        if (model && static_cast<const QObject*>(model)->parent() == nullptr) delete model;
        return;
    }
    if (model) {
        m_resultsDock->setResultsModel(model, pattern, fileName);
    } else {
        m_resultsDock->setResults({}, pattern, fileName);
    }
    m_resultsDock->show();
    m_resultsDock->raise();
}
