#include "FindReplaceDialog.h"
#include "EditorTab.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "LargeFileSearch.h"
#include "Pcre2Regex.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QMainWindow>
#include <QStatusBar>
#include <QComboBox>
#include <QTabWidget>
#include <QPointer>
#include <QSharedPointer>
#include <utility>

FindReplaceDialog::FindReplaceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("查找 / 替换"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    buildUi();
}

void FindReplaceDialog::clearSensitiveState()
{
    if (m_l2SearchInProgress) {
        m_multiCancelRequested = true;
        for (EditorTab *tab : openTabs())
            tab->cancelFindAllL2();
    }
    ++m_multiSearchGeneration;
    for (const QMetaObject::Connection &connection : std::as_const(m_multiConnections))
        disconnect(connection);
    m_multiConnections.clear();
    m_pendingMultiL2 = 0;
    m_multiTabs.clear();
    m_multiNextIndex = 0;
    if (!m_multiPattern.isEmpty())
        m_multiPattern.fill(QChar(0));
    m_multiPattern.clear();
    m_multiDispatchComplete = true;
    m_multiCancelRequested = false;
    if (m_findEdit)
        m_findEdit->clear();
    if (m_replaceEdit)
        m_replaceEdit->clear();
    if (m_hexBytesCheck)
        m_hexBytesCheck->setChecked(false);
    setL2SearchUiBusy(false);
    if (m_statusLabel)
        m_statusLabel->clear();
    hide();
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
    m_findEdit->setObjectName(QStringLiteral("findPatternEdit"));
    m_findEdit->setClearButtonEnabled(true);
    findLayout->addWidget(m_findLabel);
    findLayout->addWidget(m_findEdit);
    layout->addLayout(findLayout);

    auto *scopeLayout = new QHBoxLayout;
    auto *scopeLabel = new QLabel(QStringLiteral("范围:"));
    m_scopeCombo = new QComboBox;
    m_scopeCombo->setObjectName(QStringLiteral("searchScopeCombo"));
    m_scopeCombo->addItem(QStringLiteral("当前文件"), 0);
    m_scopeCombo->addItem(QStringLiteral("全部打开文件"), 1);
    scopeLayout->addWidget(scopeLabel);
    scopeLayout->addWidget(m_scopeCombo, 1);
    layout->addLayout(scopeLayout);

    // 替换输入
    auto *replaceLayout = new QHBoxLayout;
    m_replaceLabel = new QLabel(QStringLiteral("替换:"));
    m_replaceEdit = new QLineEdit;
    m_replaceEdit->setObjectName(QStringLiteral("replaceTextEdit"));
    m_replaceEdit->setClearButtonEnabled(true);
    replaceLayout->addWidget(m_replaceLabel);
    replaceLayout->addWidget(m_replaceEdit);
    layout->addLayout(replaceLayout);

    // 选项
    auto *optionsLayout = new QHBoxLayout;
    m_regexCheck = new QCheckBox(QStringLiteral("正则表达式"));
    m_regexCheck->setObjectName(QStringLiteral("regexSearchCheck"));
    m_hexBytesCheck = new QCheckBox(QStringLiteral("十六进制字节"));
    m_hexBytesCheck->setObjectName(QStringLiteral("hexBytesCheck"));
    m_caseCheck = new QCheckBox(QStringLiteral("区分大小写"));
    m_caseCheck->setObjectName(QStringLiteral("caseSensitiveSearchCheck"));
    m_wordCheck = new QCheckBox(QStringLiteral("整词匹配"));
    m_wordCheck->setObjectName(QStringLiteral("wholeWordSearchCheck"));
    m_wrapCheck = new QCheckBox(QStringLiteral("循环查找"));
    m_wrapCheck->setObjectName(QStringLiteral("wrapSearchCheck"));
    m_wrapCheck->setChecked(true);
    optionsLayout->addWidget(m_regexCheck);
    optionsLayout->addWidget(m_hexBytesCheck);
    optionsLayout->addWidget(m_caseCheck);
    optionsLayout->addWidget(m_wordCheck);
    optionsLayout->addWidget(m_wrapCheck);
    optionsLayout->addStretch();
    layout->addLayout(optionsLayout);

    // 按钮
    auto *btnLayout = new QHBoxLayout;
    m_findNextBtn = new QPushButton(QStringLiteral("查找下一个"));
    m_findNextBtn->setObjectName(QStringLiteral("findNextButton"));
    m_findPrevBtn = new QPushButton(QStringLiteral("查找上一个"));
    m_findPrevBtn->setObjectName(QStringLiteral("findPreviousButton"));
    m_replaceBtn = new QPushButton(QStringLiteral("替换"));
    m_replaceBtn->setObjectName(QStringLiteral("replaceButton"));
    m_replaceAllBtn = new QPushButton(QStringLiteral("全部替换"));
    m_replaceAllBtn->setObjectName(QStringLiteral("replaceAllButton"));
    m_findAllBtn = new QPushButton(QStringLiteral("查找全部"));
    m_findAllBtn->setObjectName(QStringLiteral("findAllButton"));
    m_closeBtn = new QPushButton(QStringLiteral("关闭"));
    m_closeBtn->setObjectName(QStringLiteral("closeSearchButton"));
    m_cancelSearchBtn = new QPushButton(QStringLiteral("取消搜索"));
    m_cancelSearchBtn->setObjectName(QStringLiteral("cancelSearchButton"));
    m_cancelSearchBtn->setVisible(false);
    btnLayout->addWidget(m_findNextBtn);
    btnLayout->addWidget(m_findPrevBtn);
    btnLayout->addWidget(m_replaceBtn);
    btnLayout->addWidget(m_replaceAllBtn);
    btnLayout->addWidget(m_findAllBtn);
    btnLayout->addWidget(m_cancelSearchBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_closeBtn);
    layout->addLayout(btnLayout);

    // 异步搜索状态提示（L2 大文件搜索时显示进度；L1 不显示）
    m_statusLabel = new QLabel(QStringLiteral(""));
    m_statusLabel->setObjectName(QStringLiteral("findStatusLabel"));
    m_statusLabel->setStyleSheet(QStringLiteral("color:#888;"));
    layout->addWidget(m_statusLabel);

    // 连接
    connect(m_findNextBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindNext);
    connect(m_findPrevBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindPrev);
    connect(m_replaceBtn, &QPushButton::clicked, this, &FindReplaceDialog::onReplace);
    connect(m_replaceAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAll);
    connect(m_findAllBtn, &QPushButton::clicked, this, &FindReplaceDialog::onFindAll);
    connect(m_cancelSearchBtn, &QPushButton::clicked,
            this, &FindReplaceDialog::cancelActiveSearch);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &FindReplaceDialog::onFindNext);
    // 查找参数变化时通知主窗口
    connect(m_findEdit, &QLineEdit::textChanged, this, &FindReplaceDialog::emitFindParameters);
    connect(m_regexCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
    connect(m_regexCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_hexBytesCheck->isChecked())
            m_hexBytesCheck->setChecked(false);
        updateQueryUi();
    });
    connect(m_hexBytesCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_regexCheck->isChecked())
            m_regexCheck->setChecked(false);
        updateQueryUi();
    });
    connect(m_caseCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
    connect(m_wordCheck, &QCheckBox::toggled, this, &FindReplaceDialog::emitFindParameters);
    connect(m_scopeCombo, &QComboBox::currentIndexChanged, this, [this]() {
        updateQueryUi();
    });
}

void FindReplaceDialog::setActiveTab(EditorTab *tab)
{
    // 切换 tab：先断开上一个 EditorTab 的 L2 搜索信号
    QObject::disconnect(m_l2ProgressConn);
    QObject::disconnect(m_l2PartialConn);
    QObject::disconnect(m_l2FinishedConn);
    m_l2ProgressConn = QMetaObject::Connection{};
    m_l2PartialConn = QMetaObject::Connection{};
    m_l2FinishedConn = QMetaObject::Connection{};

    if (m_l2SearchInProgress && !allOpenScope()) {
        // 上一个搜索在进行中：UI 恢复
        setL2SearchUiBusy(false);
    }
    m_tab = tab;
    if (m_tab) {
        // 连接新 tab 的 L2 异步搜索信号（即使当前是 L1 也连接，切换到 L2 时正常工作）
        m_l2ProgressConn = QObject::connect(
            m_tab, &EditorTab::findAllL2Progress,
            this,  &FindReplaceDialog::onL2FindAllProgress);
        m_l2PartialConn = QObject::connect(
            m_tab, &EditorTab::findAllL2Partial, this,
            [this, tab](const QVector<LargeFileSearchMatch> &batch,
                        const QString &, const QString &) {
                if (!allOpenScope() && m_resultsDock)
                    m_resultsDock->appendSingleFilePreview(tab, batch);
            });
        m_l2FinishedConn = QObject::connect(
            m_tab, &EditorTab::findAllL2Finished,
            this,  &FindReplaceDialog::onL2FindAllFinished);
    }
    updateQueryUi();
}

void FindReplaceDialog::setSearchResultsDock(SearchResultsDock *dock)
{
    QObject::disconnect(m_rerunConnection);
    m_resultsDock = dock;
    if (m_resultsDock) {
        m_rerunConnection = connect(
            m_resultsDock, &SearchResultsDock::rerunRequested,
            this, &FindReplaceDialog::rerunInvalidatedSearch);
    }
}

void FindReplaceDialog::rerunInvalidatedSearch(EditorTab *preferredTarget)
{
    if (m_l2SearchInProgress)
        return;
    if (preferredTarget) {
        if (m_tabWidget) {
            const int index = m_tabWidget->indexOf(preferredTarget);
            if (index < 0)
                return;
            m_tabWidget->setCurrentIndex(index);
        }
        setActiveTab(preferredTarget);
    }
    if (currentFindText().isEmpty()) {
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("查找内容已清除，请重新输入。"));
        showFindMode();
        return;
    }
    onFindAll();
}

void FindReplaceDialog::setTabWidget(QTabWidget *tabs)
{
    m_tabWidget = tabs;
}

void FindReplaceDialog::notifyTabOpened(EditorTab *tab)
{
    if (!tab || !m_l2SearchInProgress || !allOpenScope()
        || m_multiCancelRequested || m_multiPattern.isEmpty()) {
        return;
    }
    for (const QPointer<EditorTab> &known : std::as_const(m_multiTabs)) {
        if (known == tab)
            return;
    }
    if (m_multiHexBytes && !tab->isBinaryDocument()) {
        ++m_multiSkippedTextTargets;
        if (m_statusLabel) {
            m_statusLabel->setText(
                QStringLiteral("新打开的文本文件不属于当前字节搜索；已跳过 %1 个文本文件。")
                    .arg(m_multiSkippedTextTargets));
        }
        return;
    }

    const bool needsRestart = m_multiDispatchComplete;
    m_multiTabs.push_back(tab);
    m_multiDispatchComplete = false;
    if (m_statusLabel) {
        m_statusLabel->setText(
            QStringLiteral("已将新打开的文件加入搜索；当前共 %1 个目标…")
                .arg(m_multiTabs.size()));
    }
    if (needsRestart) {
        const int generation = m_multiSearchGeneration;
        QTimer::singleShot(0, this, [this, generation]() {
            dispatchNextMultiTarget(generation);
        });
    }
}

void FindReplaceDialog::setReplaceMode(bool replace)
{
    m_replaceMode = replace;
    m_replaceLabel->setVisible(replace);
    m_replaceEdit->setVisible(replace);
    m_replaceBtn->setVisible(replace);
    m_replaceAllBtn->setVisible(replace);
    updateQueryUi();
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
    findAcrossTabs(true);
}

void FindReplaceDialog::onFindPrev()
{
    findAcrossTabs(false);
}

void FindReplaceDialog::findNextFromShortcut(bool forward)
{
    findAcrossTabs(forward);
}

bool FindReplaceDialog::allOpenScope() const
{
    return m_scopeCombo && m_scopeCombo->currentData().toInt() == 1;
}

bool FindReplaceDialog::hexBytesMode() const
{
    return m_hexBytesCheck && m_hexBytesCheck->isChecked();
}

void FindReplaceDialog::updateQueryUi()
{
    if (!m_hexBytesCheck)
        return;
    bool hasBinaryTarget = m_tab && m_tab->isBinaryDocument();
    if (allOpenScope()) {
        hasBinaryTarget = false;
        for (EditorTab *tab : openTabs()) {
            if (tab && tab->isBinaryDocument()) {
                hasBinaryTarget = true;
                break;
            }
        }
    }
    if (!hasBinaryTarget && hexBytesMode())
        m_hexBytesCheck->setChecked(false);

    const bool hex = hexBytesMode();
    m_hexBytesCheck->setEnabled(!m_l2SearchInProgress && hasBinaryTarget);
    m_hexBytesCheck->setToolTip(hasBinaryTarget
        ? QStringLiteral("按原始字节搜索，例如 DE AD BE EF 或 0xDE,0xAD")
        : QStringLiteral("当前搜索范围没有 Binary/Hex 文档"));
    m_findEdit->setPlaceholderText(hex
        ? QStringLiteral("例如 DE AD BE EF") : QString());
    m_regexCheck->setEnabled(!m_l2SearchInProgress && !hex);
    m_caseCheck->setEnabled(!m_l2SearchInProgress && !hex);
    m_wordCheck->setEnabled(!m_l2SearchInProgress && !hex);

    const bool currentReplaceAllowed = !m_l2SearchInProgress && !allOpenScope() && !hex
                                       && m_tab && !m_tab->isReadOnlyByPolicy();
    bool allOpenReplaceAllowed = false;
    if (!m_l2SearchInProgress && allOpenScope() && !hex) {
        for (EditorTab *tab : openTabs()) {
            if (tab && !tab->isL2Mode() && !tab->isReadOnlyByPolicy()
                && !tab->isBinaryDocument() && !tab->isSecureDocumentMasked()
                && !tab->isSecureDocumentTransitioning()) {
                allOpenReplaceAllowed = true;
                break;
            }
        }
    }
    // Single-step Replace remains a current-document operation. Replace All
    // follows the selected scope and can transact across editable open tabs.
    m_replaceBtn->setEnabled(currentReplaceAllowed);
    m_replaceAllBtn->setEnabled(currentReplaceAllowed || allOpenReplaceAllowed);
    m_replaceEdit->setEnabled(!m_l2SearchInProgress && !hex);
    if (!m_l2SearchInProgress && m_statusLabel) {
        if (hex && m_replaceMode)
            m_statusLabel->setText(QStringLiteral("十六进制字节模式当前只执行查找；字节编辑请直接在 Hex 视图完成。"));
        else if (allOpenScope() && m_replaceMode)
            m_statusLabel->setText(QStringLiteral(
                "全部替换会修改可编辑文本标签；只读大文件、Hex 和锁定文档将在确认前列出。"));
        else
            m_statusLabel->clear();
    }
}

QList<EditorTab *> FindReplaceDialog::openTabs() const
{
    QList<EditorTab *> result;
    if (!m_tabWidget)
        return result;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *tab = qobject_cast<EditorTab *>(m_tabWidget->widget(i)))
            result.push_back(tab);
    }
    return result;
}

void FindReplaceDialog::findAcrossTabs(bool forward)
{
    if (!m_tab)
        return;
    const QString pattern = currentFindText();
    if (pattern.isEmpty())
        return;
    emitFindParameters();
    if (m_statusLabel) m_statusLabel->clear();
    bool queryError = false;
    const bool hex = hexBytesMode();
    const auto findIn = [this, &pattern, forward, &queryError, hex](EditorTab *tab, bool wrap) {
        if (hex && (!tab || !tab->isBinaryDocument()))
            return false;
        const bool found = tab && tab->findNext(
            pattern, m_regexCheck->isChecked(), m_caseCheck->isChecked(),
            m_wordCheck->isChecked(), forward, wrap, hex);
        if (!found && tab && !tab->lastSearchError().isEmpty()) {
            queryError = true;
            if (m_statusLabel)
                m_statusLabel->setText(tab->lastSearchError());
        }
        return found;
    };
    if (!allOpenScope() || !m_tabWidget) {
        if (hex && !m_tab->isBinaryDocument()) {
            if (m_statusLabel)
                m_statusLabel->setText(QStringLiteral("十六进制字节搜索仅适用于 Binary/Hex 文档。"));
            return;
        }
        const bool found = findIn(m_tab, m_wrapCheck->isChecked());
        if (!found && !queryError && m_statusLabel)
            m_statusLabel->setText(QStringLiteral("未找到更多匹配。"));
        return;
    }

    const QList<EditorTab *> tabs = openTabs();
    if (tabs.isEmpty())
        return;
    int start = tabs.indexOf(m_tab);
    if (start < 0)
        start = 0;
    if (findIn(tabs.at(start), false))
        return;
    if (queryError) return;

    const auto findInTabAt = [this, &tabs, &findIn, forward](int index) {
        EditorTab *candidate = tabs.at(index);
        candidate->resetSearchPosition(forward);
        if (findIn(candidate, false)) {
            m_tabWidget->setCurrentWidget(candidate);
            // MainWindow normally synchronizes m_tab from currentChanged.
            // Keep the dialog correct when embedded by another host as well.
            if (m_tab != candidate)
                setActiveTab(candidate);
            candidate->setFocusEditor();
            return true;
        }
        return false;
    };

    if (forward) {
        for (int index = start + 1; index < tabs.size(); ++index) {
            if (findInTabAt(index) || queryError)
                return;
        }
    } else {
        for (int index = start - 1; index >= 0; --index) {
            if (findInTabAt(index) || queryError)
                return;
        }
    }

    if (m_wrapCheck->isChecked()) {
        if (forward) {
            for (int index = 0; index < start; ++index) {
                if (findInTabAt(index) || queryError)
                    return;
            }
        } else {
            for (int index = tabs.size() - 1; index > start; --index) {
                if (findInTabAt(index) || queryError)
                    return;
            }
        }
        EditorTab *origin = tabs.at(start);
        origin->resetSearchPosition(forward);
        if (findIn(origin, false)) {
            m_tabWidget->setCurrentWidget(origin);
            if (m_tab != origin)
                setActiveTab(origin);
            origin->setFocusEditor();
            return;
        }
    }
    if (!queryError && m_statusLabel) {
        m_statusLabel->setText(m_wrapCheck->isChecked()
            ? QStringLiteral("未找到匹配。")
            : QStringLiteral("已到达搜索范围边界，没有更多匹配。"));
    }
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
    if (!found && !m_tab->lastSearchError().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText(m_tab->lastSearchError());
        return;
    }
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

    if (allOpenScope()) {
        struct PlannedTarget {
            QPointer<EditorTab> tab;
            quint64 revision = 0;
            int matches = 0;
        };
        QVector<PlannedTarget> plan;
        qint64 totalMatches = 0;
        int skippedReadOnly = 0;
        int skippedBinary = 0;
        int skippedLocked = 0;

        // Freeze and preflight the complete manifest before touching any tab.
        // A query/preflight error aborts the whole transaction; policy-
        // incompatible targets are explicitly summarized as skips.
        for (EditorTab *tab : openTabs()) {
            if (!tab)
                continue;
            if (tab->isSecureDocumentMasked() || tab->isSecureDocumentTransitioning()) {
                ++skippedLocked;
                continue;
            }
            if (tab->isBinaryDocument()) {
                ++skippedBinary;
                continue;
            }
            if (tab->isL2Mode() || tab->isReadOnlyByPolicy()) {
                ++skippedReadOnly;
                continue;
            }
            const QVector<SearchMatch> matches = tab->findAll(
                pattern, m_regexCheck->isChecked(),
                m_caseCheck->isChecked(), m_wordCheck->isChecked());
            if (!tab->lastSearchError().isEmpty() || tab->lastFindAllWasTruncated()) {
                const QString reason = !tab->lastSearchError().isEmpty()
                    ? tab->lastSearchError()
                    : QStringLiteral("匹配数量超过安全结果上限");
                if (m_statusLabel)
                    m_statusLabel->setText(QStringLiteral("预检失败：%1：%2")
                                               .arg(tab->fileName(), reason));
                QMessageBox::warning(this, QStringLiteral("无法执行全部替换"),
                    QStringLiteral("尚未修改任何文件。\n%1：%2")
                        .arg(tab->fileName(), reason));
                return;
            }
            if (matches.isEmpty())
                continue;
            plan.push_back({tab, tab->contentRevision(), int(matches.size())});
            totalMatches += matches.size();
        }

        const QString skipSummary = QStringLiteral(
            "跳过：只读/未晋升大文件 %1 个，Binary/Hex %2 个，锁定文档 %3 个。")
                .arg(skippedReadOnly).arg(skippedBinary).arg(skippedLocked);
        if (plan.isEmpty()) {
            const QString message = QStringLiteral("没有可替换的匹配。%1").arg(skipSummary);
            if (m_statusLabel) m_statusLabel->setText(message);
            QMessageBox::information(this, QStringLiteral("全部替换"), message);
            return;
        }

        const QString confirmation = QStringLiteral(
            "将在 %1 个可编辑文本文件中替换 %2 处匹配。\n%3\n\n"
            "每个文件形成一个撤销步骤；不会自动保存。是否继续？")
                .arg(plan.size()).arg(totalMatches).arg(skipSummary);
        if (QMessageBox::question(this, QStringLiteral("确认全部打开文件替换"),
                                  confirmation,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            if (m_statusLabel) m_statusLabel->setText(QStringLiteral("已取消，未修改任何文件。"));
            return;
        }

        QVector<QPointer<EditorTab>> changed;
        qint64 replacedTotal = 0;
        QString failure;
        for (const PlannedTarget &target : std::as_const(plan)) {
            EditorTab *tab = target.tab.data();
            if (!tab || tab->contentRevision() != target.revision) {
                failure = tab
                    ? QStringLiteral("“%1”在确认后发生变化").arg(tab->fileName())
                    : QStringLiteral("一个目标标签已关闭");
                break;
            }
            const int replaced = tab->replaceAll(
                pattern, currentReplaceText(), m_regexCheck->isChecked(),
                m_caseCheck->isChecked(), m_wordCheck->isChecked());
            if (!tab->lastSearchError().isEmpty() || replaced != target.matches) {
                failure = !tab->lastSearchError().isEmpty()
                    ? QStringLiteral("%1：%2").arg(tab->fileName(), tab->lastSearchError())
                    : QStringLiteral("%1：预期 %2 处，实际替换 %3 处")
                          .arg(tab->fileName()).arg(target.matches).arg(replaced);
                break;
            }
            if (replaced > 0) {
                changed.push_back(tab);
                replacedTotal += replaced;
            }
        }

        if (!failure.isEmpty()) {
            for (auto it = changed.crbegin(); it != changed.crend(); ++it) {
                if (*it)
                    (*it)->undo();
            }
            const QString message = QStringLiteral(
                "替换未完成，已撤销本批次中先前修改的文件。\n%1").arg(failure);
            if (m_statusLabel) m_statusLabel->setText(message);
            QMessageBox::warning(this, QStringLiteral("全部替换已回滚"), message);
            return;
        }

        if (m_resultsDock) {
            for (const auto &tab : std::as_const(changed)) {
                if (tab)
                    m_resultsDock->invalidateResultsFor(tab);
            }
        }
        const QString completed = QStringLiteral(
            "已在 %1 个文件中替换 %2 处；文件尚未保存。%3")
                .arg(changed.size()).arg(replacedTotal).arg(skipSummary);
        if (m_statusLabel) m_statusLabel->setText(completed);
        QMessageBox::information(this, QStringLiteral("全部替换完成"), completed);
        return;
    }

    int count = m_tab->replaceAll(pattern, currentReplaceText(),
                                  m_regexCheck->isChecked(),
                                  m_caseCheck->isChecked(),
                                  m_wordCheck->isChecked());
    if (!m_tab->lastSearchError().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText(m_tab->lastSearchError());
        QMessageBox::warning(this, QStringLiteral("替换失败"),
                             m_tab->lastSearchError());
        return;
    }
    QMessageBox::information(this, QStringLiteral("替换完成"),
                             QStringLiteral("已替换 %1 处匹配。").arg(count));
    if (count > 0 && m_tab->isL2Mode() && m_resultsDock) {
        // L2 替换后，旧的 SearchResultsModel 内 byteOffset 已失效，清空避免错误跳转
        m_resultsDock->setResultsModel(nullptr, QString(), QString(), m_tab);
    }
}

void FindReplaceDialog::onFindAll()
{
    if (!m_tab || !m_resultsDock) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();

    // Validate once before routing the query to one or many documents.  An
    // invalid expression is a query error, not a successful search with zero
    // results, and must not start any L2 background work.
    QString validationError;
    const bool hex = hexBytesMode();
    QByteArray bytePattern;
    Pcre2Regex validator;
    const bool valid = hex
        ? LargeFileSearch::parseHexBytePattern(pattern, &bytePattern, &validationError)
        : validator.compile(pattern, !m_regexCheck->isChecked(),
                            m_caseCheck->isChecked(), m_wordCheck->isChecked(),
                            &validationError);
    bytePattern.fill('\0');
    if (!valid) {
        if (m_statusLabel)
            m_statusLabel->setText(validationError);
        return;
    }
    if (hex && !allOpenScope() && !m_tab->isBinaryDocument()) {
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("十六进制字节搜索仅适用于 Binary/Hex 文档。"));
        return;
    }
    m_resultsDock->clearResults();

    if (allOpenScope()) {
        startAllOpenFindAll();
        return;
    }

    if (m_tab->isL2Mode()) {
        // L2 大文件：纯异步 → 经 AsyncTaskManager 派发 → findAllL2Finished 设置 Dock
        setL2SearchUiBusy(true, QObject::tr("准备搜索…"));
        bool started = m_tab->findAllL2Async(pattern, m_regexCheck->isChecked(),
                                             m_caseCheck->isChecked(),
                                             m_wordCheck->isChecked(),
                                             m_resultsDock, hex);
        if (!started) {
            setL2SearchUiBusy(false);
            auto *empty = new SearchResultsModel(m_resultsDock);
            empty->setIncompleteReason(QStringLiteral("搜索任务未能启动。"));
            m_resultsDock->setResultsModel(empty, pattern, m_tab->fileName(), m_tab);
            m_resultsDock->show();
            m_resultsDock->raise();
        } else {
            // findAllL2Async has synchronously flushed the visible overlay and
            // captured the final revision. Bind the result group only now.
            m_resultsDock->beginSingleFileResults(pattern, m_tab->fileName(), m_tab);
        }
        return;
    }

    // L1 小文件：QsciScintilla 内存扫描 → QTreeWidget
    auto matches = m_tab->findAll(pattern, m_regexCheck->isChecked(),
                                  m_caseCheck->isChecked(),
                                  m_wordCheck->isChecked());
    if (!m_tab->lastSearchError().isEmpty()) {
        if (m_statusLabel)
            m_statusLabel->setText(m_tab->lastSearchError());
        return;
    }
    m_tab->setSearchHighlights(matches);
    m_resultsDock->setResults(matches, pattern, m_tab->fileName(), m_tab,
                              m_tab->lastFindAllWasTruncated());
    m_resultsDock->show();
    m_resultsDock->raise();
}

void FindReplaceDialog::startAllOpenFindAll()
{
    if (!m_resultsDock)
        return;
    const QString pattern = currentFindText();
    const QList<EditorTab *> rawTabs = openTabs();

    ++m_multiSearchGeneration;
    const int generation = m_multiSearchGeneration;
    for (const auto &connection : std::as_const(m_multiConnections))
        QObject::disconnect(connection);
    m_multiConnections.clear();
    m_pendingMultiL2 = 0;
    m_multiTabs.clear();
    m_multiTabs.reserve(rawTabs.size());
    const bool hex = hexBytesMode();
    m_multiSkippedTextTargets = 0;
    for (EditorTab *tab : rawTabs) {
        if (hex && (!tab || !tab->isBinaryDocument())) {
            ++m_multiSkippedTextTargets;
            continue;
        }
        m_multiTabs.push_back(tab);
    }
    m_multiNextIndex = 0;
    m_multiPattern = pattern;
    m_multiRegex = m_regexCheck->isChecked();
    m_multiCaseSensitive = m_caseCheck->isChecked();
    m_multiWholeWord = m_wordCheck->isChecked();
    m_multiHexBytes = hex;
    m_multiDispatchComplete = false;
    m_multiCancelRequested = false;
    m_resultsDock->beginMultiFileResults(pattern, m_multiTabs.size());
    const QString startStatus = hex && m_multiSkippedTextTargets > 0
        ? QStringLiteral("正在搜索 %1 个 Binary/Hex 文档；已跳过 %2 个文本文件…")
              .arg(m_multiTabs.size()).arg(m_multiSkippedTextTargets)
        : QStringLiteral("正在搜索全部打开文件…");
    setL2SearchUiBusy(true, startStatus);
    QTimer::singleShot(0, this, [this, generation]() {
        dispatchNextMultiTarget(generation);
    });
}

void FindReplaceDialog::dispatchNextMultiTarget(int generation)
{
    if (generation != m_multiSearchGeneration)
        return;
    if (m_multiCancelRequested || m_multiNextIndex >= m_multiTabs.size()) {
        m_multiDispatchComplete = true;
        finishMultiSearchIfReady(generation);
        return;
    }

    const QPointer<EditorTab> guard = m_multiTabs.at(m_multiNextIndex++);
    EditorTab *tab = guard.data();
    if (tab && !tab->isL2Mode()) {
        const QVector<SearchMatch> matches = tab->findAll(
            m_multiPattern, m_multiRegex, m_multiCaseSensitive, m_multiWholeWord);
        const bool incomplete = tab->lastFindAllWasTruncated()
                                || !tab->lastSearchError().isEmpty();
        tab->setSearchHighlights(matches);
        if (m_resultsDock)
            m_resultsDock->appendMultiFileResults(tab, matches, incomplete);
    } else if (tab) {
        ++m_pendingMultiL2;
        QPointer<EditorTab> target(tab);
        auto settled = QSharedPointer<bool>::create(false);
        const auto partialConnection = connect(
            tab, &EditorTab::findAllL2Partial, this,
            [this, target, generation](const QVector<LargeFileSearchMatch> &batch,
                                       const QString &, const QString &) {
                if (generation == m_multiSearchGeneration && target && m_resultsDock)
                    m_resultsDock->appendMultiFilePreview(target, batch);
            });
        m_multiConnections.push_back(partialConnection);
        const auto finishedConnection = connect(
            tab, &EditorTab::findAllL2Finished, this,
            [this, target, generation, settled](SearchResultsModel *model,
                                                const QString &, const QString &) {
                if (generation != m_multiSearchGeneration || *settled) {
                    if (model) model->deleteLater();
                    return;
                }
                *settled = true;
                if (target && model && m_resultsDock) {
                    m_resultsDock->appendMultiFileResults(target, model);
                    model = nullptr;
                } else if (target && m_resultsDock) {
                    m_resultsDock->invalidateResultsFor(target);
                }
                if (model)
                    model->deleteLater();
                m_pendingMultiL2 = qMax(0, m_pendingMultiL2 - 1);
                finishMultiSearchIfReady(generation);
            });
        m_multiConnections.push_back(finishedConnection);
        const auto destroyedConnection = connect(
            tab, &QObject::destroyed, this,
            [this, generation, settled]() {
                if (generation != m_multiSearchGeneration || *settled)
                    return;
                *settled = true;
                m_pendingMultiL2 = qMax(0, m_pendingMultiL2 - 1);
                finishMultiSearchIfReady(generation);
            });
        m_multiConnections.push_back(destroyedConnection);

        if (tab->findAllL2Async(m_multiPattern, m_multiRegex,
                                m_multiCaseSensitive, m_multiWholeWord,
                                m_resultsDock, m_multiHexBytes)) {
            if (m_resultsDock)
                m_resultsDock->beginMultiFileTarget(tab);
        } else {
            *settled = true;
            m_pendingMultiL2 = qMax(0, m_pendingMultiL2 - 1);
            if (m_resultsDock) {
                auto *incomplete = new SearchResultsModel(m_resultsDock);
                incomplete->setIncompleteReason(QStringLiteral("搜索任务未能启动。"));
                m_resultsDock->appendMultiFileResults(tab, incomplete);
            }
        }
    }

    // Dispatch at most one in-memory document per event-loop turn. This keeps
    // all-open search cancellable even when many near-threshold S1 tabs exist.
    QTimer::singleShot(0, this, [this, generation]() {
        dispatchNextMultiTarget(generation);
    });
}

void FindReplaceDialog::finishMultiSearchIfReady(int generation)
{
    if (generation != m_multiSearchGeneration
        || !m_multiDispatchComplete || m_pendingMultiL2 > 0) {
        return;
    }
    for (const auto &connection : std::as_const(m_multiConnections))
        QObject::disconnect(connection);
    m_multiConnections.clear();
    if (m_resultsDock)
        m_resultsDock->finishMultiFileResults(m_multiCancelRequested);
    const bool cancelled = m_multiCancelRequested;
    m_multiTabs.clear();
    if (!m_multiPattern.isEmpty())
        m_multiPattern.fill(QChar(0));
    m_multiPattern.clear();
    setL2SearchUiBusy(false);
    if (cancelled && m_statusLabel)
        m_statusLabel->setText(QStringLiteral("搜索已取消，已保留完成的结果。"));
    else if (m_multiHexBytes && m_multiSkippedTextTargets > 0 && m_statusLabel)
        m_statusLabel->setText(
            QStringLiteral("搜索完成：已搜索 Binary/Hex 文档，跳过 %1 个文本文件。")
                .arg(m_multiSkippedTextTargets));
    m_multiCancelRequested = false;
}

void FindReplaceDialog::cancelActiveSearch()
{
    if (!m_l2SearchInProgress)
        return;
    if (allOpenScope()) {
        m_multiCancelRequested = true;
        for (EditorTab *tab : openTabs())
            tab->cancelFindAllL2();
        if (m_multiNextIndex >= m_multiTabs.size())
            m_multiDispatchComplete = true;
        finishMultiSearchIfReady(m_multiSearchGeneration);
    } else if (m_tab) {
        m_tab->cancelFindAllL2();
    }
    if (m_l2SearchInProgress) {
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral("正在取消搜索…"));
        m_cancelSearchBtn->setEnabled(false);
    }
}

// ============================================================================
//  L2 异步搜索 UI 辅助
// ============================================================================
void FindReplaceDialog::setL2SearchUiBusy(bool busy, const QString& msg)
{
    m_l2SearchInProgress = busy;
    m_findNextBtn->setEnabled(!busy);
    m_findPrevBtn->setEnabled(!busy);
    m_findAllBtn->setEnabled(!busy);
    m_cancelSearchBtn->setVisible(busy);
    m_cancelSearchBtn->setEnabled(busy);
    m_findEdit->setEnabled(!busy);
    m_wrapCheck->setEnabled(!busy);
    m_scopeCombo->setEnabled(!busy);
    if (m_statusLabel) m_statusLabel->setText(busy ? msg : QString());
    updateQueryUi();
    if (!busy) {
        if (auto *mw = qobject_cast<QMainWindow*>(window()))
            mw->statusBar()->clearMessage();
    }
}

void FindReplaceDialog::onL2FindAllProgress(int percent, qint64 done, qint64 total)
{
    if (!m_l2SearchInProgress) return;
    QString msg;
    if (percent < 0) {
        msg = QObject::tr("正在准备大文件搜索…");
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
    // In all-open-files mode the per-target aggregation connection owns this
    // completion. Handling the active tab here as a single-file result would
    // replace the multi-file tree halfway through the batch.
    if (allOpenScope())
        return;
    setL2SearchUiBusy(false);
    auto *target = qobject_cast<EditorTab *>(sender());
    if (!m_resultsDock) {
        // Dock 不存在：model 的 QObject parent 为空（因为我们传 m_resultsDock=nullptr），此时手动删除
        if (model && static_cast<const QObject*>(model)->parent() == nullptr) delete model;
        return;
    }
    if (model) {
        m_resultsDock->setResultsModel(model, pattern, fileName, target);
    } else {
        m_resultsDock->invalidateResultsFor(target);
    }
    m_resultsDock->show();
    m_resultsDock->raise();
}
