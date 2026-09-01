#include "FindReplaceDialog.h"
#include "DocumentSearchSurface.h"
#include "SearchResultsDock.h"
#include "SearchResultsModel.h"
#include "SearchResultsFeedback.h"
#include "SingleFindAllRequest.h"
#include "SearchNavigationRequest.h"
#include "LargeFileSearch.h"
#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"
#include "ByteClipboard.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QMainWindow>
#include <QStatusBar>
#include <QComboBox>
#include <QTabWidget>
#include <QPointer>
#include <QSharedPointer>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QSizePolicy>
#include <memory>
#include <algorithm>
#include <utility>

namespace {
bool isSearchableDocument(DocumentSearchSurface *tab)
{
    return tab && !tab->isSecureDocumentMasked()
        && !tab->isSecureDocumentTransitioning();
}

SearchResultTarget resultTarget(DocumentSearchSurface *tab)
{
    return tab ? searchResultTarget(tab->documentState(), tab->fileName())
               : SearchResultTarget{};
}

QString compactSearchMessage(QString message, qsizetype limit = 72)
{
    message.replace(QLatin1Char('\n'), QLatin1Char(' '));
    message = message.simplified();
    if (message.size() <= limit)
        return message;
    return message.left(limit - 1) + QChar(0x2026);
}
}

FindReplaceDialog::FindReplaceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("查找 / 替换"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    buildUi();
}

void FindReplaceDialog::clearSensitiveState()
{
    if (m_navigationRequest) m_navigationRequest->retire();
    if (m_l2SearchInProgress) {
        m_searchSession.requestCancel();
        for (DocumentSearchSurface *tab : openTabs())
            tab->cancelFindAllL2();
    }
    m_searchSession.reset();
    disconnectSingleSearch();
    for (const QMetaObject::Connection &connection : std::as_const(m_multiConnections))
        disconnect(connection);
    m_multiConnections.clear();
    m_multiTabs.clear();
    m_multiNextIndex = 0;
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
    m_wordCheck->setToolTip(
        QStringLiteral("西文按单词边界匹配；中文、日文和韩文按完整短语精确匹配"));
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

    // A deliberately small authoring aid: it edits the same regex string as
    // expert mode instead of introducing a second query language or a lossy
    // visual AST. Selected text is wrapped; an empty selection gets an
    // editable placeholder.
    auto *assistantHeader = new QHBoxLayout;
    m_regexAssistantToggle = new QPushButton(QStringLiteral("正则助手"));
    m_regexAssistantToggle->setObjectName(QStringLiteral("regexAssistantToggle"));
    m_regexAssistantToggle->setCheckable(true);
    assistantHeader->addWidget(m_regexAssistantToggle);
    m_queryHintLabel = new QLabel;
    m_queryHintLabel->setObjectName(QStringLiteral("regexQueryHint"));
    m_queryHintLabel->setStyleSheet(QStringLiteral("color:#707070;"));
    m_queryHintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_queryHintLabel->setWordWrap(true);
    m_queryHintLabel->setMinimumWidth(0);
    m_queryHintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    assistantHeader->addWidget(m_queryHintLabel, 1);
    layout->addLayout(assistantHeader);

    m_regexAssistant = new QWidget;
    m_regexAssistant->setObjectName(QStringLiteral("regexAssistant"));
    auto *assistantLayout = new QHBoxLayout(m_regexAssistant);
    assistantLayout->setContentsMargins(0, 0, 0, 0);
    assistantLayout->setSpacing(4);
    struct AssistantItem {
        const char *label;
        const char *prefix;
        const char *suffix;
        const char *placeholder;
        const char *tip;
    };
    const AssistantItem items[] = {
        {"原样文本", "", "", "文本", "把选择内容按普通文字匹配"},
        {"任意内容", ".*?", "", "", "匹配任意长度内容，使用非贪婪方式"},
        {"数字", "\\d+", "", "", "匹配一个或多个数字"},
        {"单词", "\\w+", "", "", "匹配一个或多个文字、数字或下划线"},
        {"空白", "\\s+", "", "", "匹配一个或多个空白字符"},
        {"捕获", "(?<字段>", ")", "内容", "命名捕获可在替换中用 ${字段} 引用"},
        {"任选", "(?:", ")", "A|B", "匹配多个候选之一"},
        {"行首", "^", "", "", "只在行首匹配"},
        {"行尾", "$", "", "", "只在行尾匹配"},
    };
    for (const AssistantItem &item : items) {
        auto *button = new QPushButton(QString::fromUtf8(item.label));
        // The assistant operates on the current selection in the query edit.
        // Mouse clicks must not move focus and clear that selection before the
        // clicked handler reads it.
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(QString::fromUtf8(item.tip));
        assistantLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, item]() {
                    if (QString::fromUtf8(item.label) == QStringLiteral("原样文本")) {
                        QString text = m_findEdit->selectedText();
                        if (text.isEmpty()) text = QString::fromUtf8(item.placeholder);
                        m_findEdit->insert(QRegularExpression::escape(text));
                    } else {
                        insertRegexTemplate(QString::fromUtf8(item.prefix),
                                            QString::fromUtf8(item.suffix),
                                            QString::fromUtf8(item.placeholder));
                    }
                });
    }
    assistantLayout->addStretch();
    m_regexAssistant->setVisible(false);
    layout->addWidget(m_regexAssistant);

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
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        QByteArray rawBytes;
        QString clipboardText;
        bool recognizedRawBytes = false;
        if (m_hexBytesCheck && !m_hexBytesCheck->isChecked()
            && m_hexBytesCheck->isEnabled()
            && ByteClipboard::clipboardHexSearch(&rawBytes, &clipboardText)
            && (text == clipboardText
                || text == QGuiApplication::clipboard()->text())) {
            if (text != clipboardText) {
                const QSignalBlocker blockFind(m_findEdit);
                m_findEdit->setText(clipboardText);
            }
            m_hexBytesCheck->setChecked(true);
            recognizedRawBytes = true;
            if (!rawBytes.isEmpty())
                rawBytes.fill(0);
        }
        updateQueryUi();
        if (recognizedRawBytes && m_statusLabel) {
            m_statusLabel->setText(QStringLiteral(
                "已识别字符栏中的原始字节，按精确字节搜索。"));
        }
        emitFindParameters();
    });
    connect(m_regexCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            const QSignalBlocker blockHex(m_hexBytesCheck);
            const QSignalBlocker blockWord(m_wordCheck);
            m_hexBytesCheck->setChecked(false);
            m_wordCheck->setChecked(false);
        }
        updateQueryUi();
        emitFindParameters();
    });
    connect(m_regexAssistantToggle, &QPushButton::toggled,
            this, [this](bool visible) {
                if (visible && !m_regexCheck->isChecked())
                    m_regexCheck->setChecked(true);
                m_regexAssistant->setVisible(visible);
                adjustSize();
            });
    connect(m_hexBytesCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            const QSignalBlocker blockRegex(m_regexCheck);
            const QSignalBlocker blockCase(m_caseCheck);
            const QSignalBlocker blockWord(m_wordCheck);
            m_regexCheck->setChecked(false);
            m_caseCheck->setChecked(false);
            m_wordCheck->setChecked(false);
        }
        updateQueryUi();
        emitFindParameters();
    });
    connect(m_caseCheck, &QCheckBox::toggled, this, [this]() {
        updateQueryUi();
        emitFindParameters();
    });
    connect(m_wordCheck, &QCheckBox::toggled, this, [this]() {
        updateQueryUi();
        emitFindParameters();
    });
    connect(m_scopeCombo, &QComboBox::currentIndexChanged, this, [this]() {
        updateQueryUi();
    });
}

void FindReplaceDialog::setActiveTab(DocumentSearchSurface *tab)
{
    const bool retireSingle = m_tab != tab
        && m_searchSession.query().scope == SearchScope::CurrentDocument;
    const bool targetChanged = m_tab != tab;
    m_tab = tab;
    if (targetChanged && m_navigationRequest
        && m_navigationRequest->scope() == SearchScope::CurrentDocument)
        m_navigationRequest->retire();
    if (retireSingle) {
        m_searchSession.reset();
        disconnectSingleSearch();
        setL2SearchUiBusy(false);
    }
    updateQueryUi();
}

void FindReplaceDialog::disconnectSingleSearch()
{
    if (m_singleRequest) {
        m_singleRequest->retire();
        m_singleRequest->deleteLater();
        m_singleRequest.clear();
    }
}

void FindReplaceDialog::finishSingleSearch()
{
    m_searchSession.settleTarget();
    disconnectSingleSearch();
    setL2SearchUiBusy(false);
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

void FindReplaceDialog::rerunInvalidatedSearch(
    const QUuid &preferredDocumentId, quint64 expectedRevision)
{
    Q_UNUSED(expectedRevision);
    if (m_l2SearchInProgress)
        return;
    DocumentSearchSurface *preferredTarget = nullptr;
    if (!preferredDocumentId.isNull()) {
        for (DocumentSearchSurface *candidate : openTabs()) {
            if (candidate
                && candidate->documentState().id == preferredDocumentId) {
                preferredTarget = candidate;
                break;
            }
        }
    }
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

void FindReplaceDialog::notifyTabOpened(DocumentSearchSurface *tab)
{
    if (!tab || !m_l2SearchInProgress || !allOpenScope()
        || m_searchSession.cancelRequested()
        || m_searchSession.query().pattern.isEmpty()) {
        return;
    }
    for (const QPointer<DocumentSearchSurface> &known : std::as_const(m_multiTabs)) {
        if (known == tab)
            return;
    }
    if (!isSearchableDocument(tab)) {
        m_searchSession.skipLockedTarget();
        if (m_statusLabel) {
            m_statusLabel->setText(
                QStringLiteral("新打开的锁定文档不属于当前搜索；已跳过 %1 个锁定文档。")
                    .arg(m_searchSession.skippedLockedTargets()));
        }
        return;
    }
    if (m_searchSession.query().hexBytes && !tab->isHexPresentation()) {
        m_searchSession.skipTextTarget();
        if (m_statusLabel) {
            m_statusLabel->setText(
                QStringLiteral("新打开的文本文件不属于当前字节搜索；已跳过 %1 个文本文件。")
                    .arg(m_searchSession.skippedTextTargets()));
        }
        return;
    }

    const bool needsRestart = m_searchSession.dispatchComplete();
    m_multiTabs.push_back(tab);
    m_searchSession.setDispatchComplete(false);
    if (m_statusLabel) {
        m_statusLabel->setText(
            QStringLiteral("已将新打开的文件加入搜索；当前共 %1 个目标…")
                .arg(m_multiTabs.size()));
    }
    if (needsRestart) {
        const int generation = m_searchSession.generation();
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

void FindReplaceDialog::insertRegexTemplate(const QString &prefix,
                                            const QString &suffix,
                                            const QString &placeholder)
{
    const int start = m_findEdit->selectionStart() >= 0
        ? m_findEdit->selectionStart() : m_findEdit->cursorPosition();
    const QString selected = m_findEdit->selectedText();
    const QString body = selected.isEmpty() ? placeholder : selected;
    const QString inserted = prefix + body + suffix;
    m_findEdit->insert(inserted);
    if (selected.isEmpty() && !placeholder.isEmpty()) {
        m_findEdit->setSelection(start + prefix.size(), placeholder.size());
    } else {
        m_findEdit->setCursorPosition(start + inserted.size());
    }
    m_findEdit->setFocus();
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

    const bool busy = m_l2SearchInProgress;
    if (m_regexAssistantToggle)
        m_regexAssistantToggle->setEnabled(!busy && !hexBytesMode());
    if (m_queryHintLabel && !busy) {
        if (m_regexCheck->isChecked() && !m_findEdit->text().isEmpty()) {
            QString error;
            bool valid = false;
            const bool encodedTarget = m_tab && m_tab->isL2Mode()
                && !m_tab->isHexPresentation()
                && !EncodedRegexCompiler::isUtf8SubjectEncoding(
                    m_tab->displayEncoding());
            if (encodedTarget) {
                EncodedRegexCompiler::Output translated;
                valid = EncodedRegexCompiler::compile(
                    m_findEdit->text(), m_tab->displayEncoding(),
                    m_caseCheck->isChecked(), &translated, &error);
            } else {
                Pcre2Regex validator;
                valid = validator.compile(m_findEdit->text(), false,
                                          m_caseCheck->isChecked(), false,
                                          &error);
            }
            if (valid) {
                m_queryHintLabel->setText(QStringLiteral("正则有效"));
                m_queryHintLabel->setToolTip(QString());
                m_queryHintLabel->setStyleSheet(QStringLiteral("color:#39734a;"));
            } else {
                m_queryHintLabel->setText(compactSearchMessage(error));
                m_queryHintLabel->setToolTip(error);
                m_queryHintLabel->setStyleSheet(QStringLiteral("color:#a33a3a;"));
            }
        } else if (hexBytesMode()) {
            m_queryHintLabel->setText(QStringLiteral("原始字节搜索"));
            m_queryHintLabel->setStyleSheet(QStringLiteral("color:#707070;"));
        } else {
            m_queryHintLabel->setText(QStringLiteral("普通查找"));
            m_queryHintLabel->setStyleSheet(QStringLiteral("color:#707070;"));
        }
    }
    int searchableTargetCount = 0;
    for (DocumentSearchSurface *tab : openTabs()) {
        if (isSearchableDocument(tab))
            ++searchableTargetCount;
    }
    if (m_scopeCombo) {
        const int allOpenIndex = m_scopeCombo->findData(1);
        if (allOpenIndex >= 0) {
            m_scopeCombo->setItemText(
                allOpenIndex,
                QStringLiteral("全部打开文件 (%1)").arg(searchableTargetCount));
        }
    }
    bool hasBinaryTarget = isSearchableDocument(m_tab)
        && m_tab->isHexPresentation();
    bool hasSearchableTarget = isSearchableDocument(m_tab);
    if (allOpenScope()) {
        hasBinaryTarget = false;
        hasSearchableTarget = false;
        for (DocumentSearchSurface *tab : openTabs()) {
            if (!isSearchableDocument(tab))
                continue;
            hasSearchableTarget = true;
            if (tab->isHexPresentation()) {
                hasBinaryTarget = true;
            }
        }
    }
    // Keep one stable interpretation state. Programmatic callers can still
    // set disabled QCheckBoxes, so normalize here as a defensive domain gate;
    // real users are prevented from producing these combinations by the
    // enabled-state rules below.
    if (!hasBinaryTarget && m_hexBytesCheck->isChecked()) {
        const QSignalBlocker blocker(m_hexBytesCheck);
        m_hexBytesCheck->setChecked(false);
    }
    if (m_hexBytesCheck->isChecked()) {
        const QSignalBlocker blockRegex(m_regexCheck);
        const QSignalBlocker blockCase(m_caseCheck);
        const QSignalBlocker blockWord(m_wordCheck);
        m_regexCheck->setChecked(false);
        m_caseCheck->setChecked(false);
        m_wordCheck->setChecked(false);
    } else if (m_regexCheck->isChecked()) {
        const QSignalBlocker blockWord(m_wordCheck);
        m_wordCheck->setChecked(false);
    }

    const bool hex = hexBytesMode();
    const bool regex = m_regexCheck->isChecked();
    const bool caseSensitive = m_caseCheck->isChecked();
    const bool wholeWord = m_wordCheck->isChecked();
    const bool hasQuery = m_tab && !currentFindText().isEmpty();
    const bool queryExecutable = hasQuery && hasSearchableTarget
        && (!hex || hasBinaryTarget);

    m_hexBytesCheck->setEnabled(!busy && hasBinaryTarget && !regex
                                && !caseSensitive && !wholeWord);
    m_hexBytesCheck->setToolTip(hasBinaryTarget
        ? QStringLiteral("按原始字节搜索，例如 DE AD BE EF 或 0xDE,0xAD")
        : QStringLiteral("当前搜索范围没有处于十六进制视图的文档"));
    m_findEdit->setPlaceholderText(hex
        ? QStringLiteral("例如 DE AD BE EF") : QString());
    m_regexCheck->setEnabled(!busy && !hex && !wholeWord);
    m_caseCheck->setEnabled(!busy && !hex);
    m_wordCheck->setEnabled(!busy && !hex && !regex);
    m_wrapCheck->setEnabled(!busy);
    m_scopeCombo->setEnabled(!busy);
    m_findEdit->setEnabled(!busy);
    const bool navigationAllowed = !(m_navigationRequest && m_navigationRequest->active())
        && (!busy || m_allowNavigationDuringBusy);
    m_findNextBtn->setEnabled(navigationAllowed && queryExecutable);
    m_findPrevBtn->setEnabled(navigationAllowed && queryExecutable);
    m_findAllBtn->setEnabled(!busy && queryExecutable);

    const auto replaceableTextTarget = [](DocumentSearchSurface *tab) {
        return tab && !tab->isReadOnlyByPolicy() && !tab->isBinaryDocument()
            && !tab->isSecureDocumentMasked()
            && !tab->isSecureDocumentTransitioning();
    };
    const bool currentTargetReplaceable = replaceableTextTarget(m_tab);
    const bool currentReplaceAllowed = !busy && hasQuery && !allOpenScope() && !hex
                                       && currentTargetReplaceable;
    // Single-step Replace remains a current-document operation. Replace All
    // follows the selected scope and can transact across editable open tabs.
    m_replaceBtn->setEnabled(currentReplaceAllowed);
    // The replacement value remains editable for an empty query, but only
    // where the selected scope contains a policy-writable Text target.
    bool scopeHasReplaceableTarget = currentTargetReplaceable;
    if (allOpenScope()) {
        const auto targets = openTabs();
        scopeHasReplaceableTarget = std::any_of(
            targets.cbegin(), targets.cend(), replaceableTextTarget);
    }
    const bool replacementValueAllowed = !busy && !hex
        && scopeHasReplaceableTarget;
    m_replaceAllBtn->setEnabled(currentReplaceAllowed
        || (replacementValueAllowed && hasQuery && allOpenScope()));
    m_replaceEdit->setEnabled(replacementValueAllowed);
    if (!busy && m_statusLabel) {
        const QString byteModeNotice = QStringLiteral(
            "字节模式仅查找；编辑请回到十六进制视图。");
        const QString allOpenReplaceNotice = QStringLiteral(
            "全部替换只修改可编辑文本；执行前列出跳过项。");
        if (hex && m_replaceMode)
            m_statusLabel->setText(byteModeNotice);
        else if (allOpenScope() && m_replaceMode)
            m_statusLabel->setText(allOpenReplaceNotice);
        else if (m_statusLabel->text() == byteModeNotice
                 || m_statusLabel->text() == allOpenReplaceNotice)
            m_statusLabel->clear();
    }
}

QList<DocumentSearchSurface *> FindReplaceDialog::openTabs() const
{
    QList<DocumentSearchSurface *> result;
    if (!m_tabWidget)
        return result;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *tab = dynamic_cast<DocumentSearchSurface *>(m_tabWidget->widget(i)))
            result.push_back(tab);
    }
    return result;
}

void FindReplaceDialog::findAcrossTabs(bool forward)
{
    if (!m_tab || (m_navigationRequest && m_navigationRequest->active())) return;
    const QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    const bool allOpen = allOpenScope();
    const bool hex = hexBytesMode();
    if (!allOpen && (!isSearchableDocument(m_tab)
                    || (hex && !m_tab->isHexPresentation()))) {
        if (m_statusLabel) m_statusLabel->setText(!isSearchableDocument(m_tab)
            ? QStringLiteral("当前安全文档已锁定。")
            : QStringLiteral("字节搜索仅适用于十六进制视图。"));
        return;
    }
    SearchQuery query{pattern, allOpen ? SearchScope::AllOpenDocuments
                                     : SearchScope::CurrentDocument,
                      m_regexCheck->isChecked(), m_caseCheck->isChecked(),
                      m_wordCheck->isChecked(), hex};
    if (m_navigationRequest) m_navigationRequest->deleteLater();
    auto *created = new SearchNavigationRequest(
        query, openTabs(), m_tab, forward, m_wrapCheck->isChecked(), this);
    m_navigationRequest = created;
    QPointer<SearchNavigationRequest> request(created);
    QPointer<FindReplaceDialog> owner(this);
    connect(created, &SearchNavigationRequest::targetClosed, this,
        [this, request](DocumentSearchSurface *identity) {
            if (m_tab == identity) m_tab = nullptr;
            QTimer::singleShot(0, this, [this, request] {
                if (m_navigationRequest != request) return;
                updateNavigationUi(QStringLiteral("文档已关闭。"));
            });
        });
    connect(created, &SearchNavigationRequest::finished, this,
        [this, owner, request, allOpen](DocumentSearchSurface *target,
                                       bool found, QString error) {
            if (!request || m_navigationRequest != request) return;
            QPointer<DocumentSearchSurface> targetGuard(target);
            updateNavigationUi();
            if (!owner || m_navigationRequest != request) return;
            if (m_statusLabel) {
                m_statusLabel->setText(found ? QString() : (error.isEmpty()
                    ? QStringLiteral("未找到更多匹配") : compactSearchMessage(error)));
                m_statusLabel->setToolTip(error);
            }
            if (!found || !targetGuard) return;
            if (allOpen && m_tabWidget) m_tabWidget->setCurrentWidget(targetGuard);
            if (!owner || !targetGuard || m_navigationRequest != request) return;
            if (allOpen && m_tab != targetGuard) setActiveTab(targetGuard);
            if (owner && targetGuard && m_navigationRequest == request)
                targetGuard->setFocusEditor();
        });
    updateNavigationUi(forward ? QObject::tr("查找下一个…")
                               : QObject::tr("查找上一个…"));
    if (owner && request && m_navigationRequest == request) request->start();
}

void FindReplaceDialog::onReplace()
{
    if (!m_tab) return;
    QString pattern = currentFindText();
    if (pattern.isEmpty()) return;
    emitFindParameters();
    // Replace owns the currently selected match. Calling findNext first would
    // skip a match that the user just selected with Find Next/F3. Only locate
    // a match when the current selection is not a result of the same query.
    const bool regex = m_regexCheck->isChecked();
    const bool caseSensitive = m_caseCheck->isChecked();
    const bool wholeWord = m_wordCheck->isChecked();
    bool found = m_tab->currentSelectionMatchesSearch(
        pattern, regex, caseSensitive, wholeWord);
    if (!found) {
        found = m_tab->findNext(pattern, regex, caseSensitive, wholeWord,
                                true, m_wrapCheck->isChecked());
    }
    if (!found && !m_tab->lastSearchError().isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText(m_tab->lastSearchError());
        return;
    }
    if (found) {
        QString replaceError;
        if (!m_tab->replaceCurrentSearchMatch(
                pattern, currentReplaceText(), regex, caseSensitive,
                wholeWord, &replaceError)) {
            if (m_statusLabel)
                m_statusLabel->setText(replaceError);
            return;
        }
        m_tab->findNext(pattern, regex, caseSensitive, wholeWord,
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
            QPointer<DocumentSearchSurface> tab;
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
        for (DocumentSearchSurface *tab : openTabs()) {
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
            if (tab->isReadOnlyByPolicy()) {
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
            "跳过：只读大文件 %1 个，二进制文档 %2 个，锁定文档 %3 个。")
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

        QVector<QPointer<DocumentSearchSurface>> changed;
        qint64 replacedTotal = 0;
        QString failure;
        for (const PlannedTarget &target : std::as_const(plan)) {
            DocumentSearchSurface *tab = target.tab.data();
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
                    m_resultsDock->invalidateResultsFor(
                        tab->documentState().id);
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
        m_resultsDock->setResultsModel(
            nullptr, QString(), QString(), resultTarget(m_tab));
    }
}

void FindReplaceDialog::onFindAll()
{
    if (!m_tab || !m_resultsDock) return;
    const QPointer<FindReplaceDialog> owner(this);
    const QPointer<DocumentSearchSurface> target(m_tab);
    const SearchQuery query{currentFindText(), allOpenScope()
        ? SearchScope::AllOpenDocuments : SearchScope::CurrentDocument,
        m_regexCheck->isChecked(), m_caseCheck->isChecked(),
        m_wordCheck->isChecked(), hexBytesMode()};
    const bool single = query.scope == SearchScope::CurrentDocument;
    QPointer<SingleFindAllRequest> request;
    if (single) {
        disconnectSingleSearch();
        request = new SingleFindAllRequest(target, m_searchSession.begin(query), this);
        m_singleRequest = request;
        connect(request, &SingleFindAllRequest::targetClosed, this,
            [this, request, identity = target.data()] {
                if (!request || !m_searchSession.isCurrent(request->generation()))
                    return;
                const int generation = request->generation();
                if (m_tab == identity)
                    m_tab = nullptr;
                m_searchSession.settleTarget();
                disconnectSingleSearch();
                QTimer::singleShot(0, this, [this, generation] {
                    if (m_searchSession.isCurrent(generation))
                        setL2SearchUiBusy(false);
                });
            });
        setL2SearchUiBusy(false);
    }
    const QString pattern = query.pattern;
    if (!owner || !target || pattern.isEmpty()
        || (single && (!request || !request->current(m_searchSession, m_tab, false))))
        return;
    emitFindParameters();
    if (!owner || !target || (single && (!request || !request->current(m_searchSession, m_tab, false))))
        return;

    // Validate once before routing the query to one or many documents.  An
    // invalid expression is a query error, not a successful search with zero
    // results, and must not start any L2 background work.
    QString validationError;
    if (!validateFindAllQuery(query, target, &validationError)) {
        if (m_statusLabel)
            m_statusLabel->setText(validationError);
        return;
    }
    m_resultsDock->clearResults();
    if (!owner || !target || (single && (!request || !request->current(m_searchSession, m_tab, false))))
        return;
    if (!single) {
        startAllOpenFindAll();
        return;
    }
    m_searchSession.scheduleTarget();
    if (target->isL2Mode()) {
        // L2 大文件：纯异步 → 经 AsyncTaskManager 派发 → findAllL2Finished 设置 Dock
        const TextEncoding encodingAtStart = target->displayEncoding();
        const bool bytePositionModeAtStart =
            target->isBinaryDocument() || target->isHexPresentation();
        connect(request, &SingleFindAllRequest::progress,
            this, [this, request](int percent, qint64 done, qint64 total) {
                if (request && request->current(m_searchSession, m_tab))
                    onL2FindAllProgress(percent, done, total);
            });
        connect(request, &SingleFindAllRequest::partial,
            this, [this, target, request](const QVector<LargeFileSearchMatch> &batch,
                TextEncoding encoding, bool byteMode) {
                if (request && request->current(m_searchSession, m_tab) && m_resultsDock)
                    m_resultsDock->appendSingleFilePreview(resultTarget(target), batch, encoding, byteMode);
            });
        connect(request, &SingleFindAllRequest::completed,
            this, [this, target, request](SearchResultsModel *model,
                const QString &completedPattern, const QString &name) {
                onL2FindAllFinished(request, target, model, completedPattern, name);
            });
        setL2SearchUiBusy(true, QObject::tr("准备搜索…"), true);
        const bool started = request->startL2(query);
        // taskAdded may synchronously close or switch the actual target.
        if (!owner || !request || !request->current(m_searchSession, m_tab, false) || !m_resultsDock)
            return;
        if (!started) {
            auto *empty = new SearchResultsModel(m_resultsDock);
            empty->setIncompleteReason(QStringLiteral("搜索任务未能启动。"),
                QStringLiteral("未能开始查找；请检查文件是否可用后重试。"));
            onL2FindAllFinished(request, target, empty, pattern, target->fileName());
        } else {
            // findAllL2Async has synchronously flushed the visible overlay and
            // captured the final revision. Bind the result group only now.
            m_resultsDock->beginSingleFileResults(
                pattern, target->fileName(), resultTarget(target), encodingAtStart,
                bytePositionModeAtStart);
            if (owner && request)
                request->enableDelivery();
        }
        return;
    }

    // S1 is already UTF-8 inside QScintilla, but a near-threshold in-memory
    // scan is still real work. Snapshot on the UI thread, then compile, scan
    // and shape rows in the central scheduler.
    setL2SearchUiBusy(true, QObject::tr("正在搜索…"), true);
    auto completed =
        [this, owner, target, request, pattern](QVector<SearchMatch> matches, bool truncated,
                                QString error) mutable {
            if (!request || !request->current(m_searchSession, m_tab))
                return;
            finishSingleSearch();
            if (!owner || !request || !request->current(m_searchSession, m_tab, false))
                return;
            if (!error.isEmpty()) {
                if (m_statusLabel) {
                    m_statusLabel->setText(compactSearchMessage(error));
                    m_statusLabel->setToolTip(error);
                }
                return;
            }
            target->setSearchHighlights(matches);
            if (owner && request && request->current(m_searchSession, m_tab, false) && m_resultsDock) {
                m_resultsDock->setResults(matches, pattern, target->fileName(),
                                          resultTarget(target), truncated);
                m_resultsDock->show();
                m_resultsDock->raise();
            }
        };
    const bool started = target->findAllL1Async(
        pattern, query.regex, query.caseSensitive, query.wholeWord, this, completed);
    if (!owner || !request || !request->current(m_searchSession, m_tab))
        return;
    if (!started) {
        const QString error = target->lastSearchError().isEmpty()
            ? QStringLiteral("搜索任务未能启动。") : target->lastSearchError();
        completed({}, false, error);
    }
}

void FindReplaceDialog::startAllOpenFindAll()
{
    if (!m_resultsDock)
        return;
    const QString pattern = currentFindText();
    const QList<DocumentSearchSurface *> rawTabs = openTabs();
    const bool hex = hexBytesMode();

    SearchQuery query;
    query.pattern = pattern;
    query.scope = SearchScope::AllOpenDocuments;
    query.regex = m_regexCheck->isChecked();
    query.caseSensitive = m_caseCheck->isChecked();
    query.wholeWord = m_wordCheck->isChecked();
    query.hexBytes = hex;
    const int generation = m_searchSession.begin(std::move(query));
    disconnectSingleSearch();
    for (const auto &connection : std::as_const(m_multiConnections))
        QObject::disconnect(connection);
    m_multiConnections.clear();
    m_multiTabs.clear();
    m_multiTabs.reserve(rawTabs.size());
    for (DocumentSearchSurface *tab : rawTabs) {
        if (!isSearchableDocument(tab)) {
            m_searchSession.skipLockedTarget();
            continue;
        }
        if (hex && (!tab || !tab->isHexPresentation())) {
            m_searchSession.skipTextTarget();
            continue;
        }
        m_multiTabs.push_back(tab);
    }
    m_multiNextIndex = 0;
    m_resultsDock->beginMultiFileResults(pattern, m_multiTabs.size());
    const QString startStatus = QStringLiteral(
        "正在搜索 %1 个打开文件；已跳过文本 %2 个、锁定 %3 个…")
        .arg(m_multiTabs.size()).arg(m_searchSession.skippedTextTargets())
        .arg(m_searchSession.skippedLockedTargets());
    setL2SearchUiBusy(true, startStatus);
    QTimer::singleShot(0, this, [this, generation]() {
        dispatchNextMultiTarget(generation);
    });
}

void FindReplaceDialog::dispatchNextMultiTarget(int generation)
{
    if (!m_searchSession.isCurrent(generation))
        return;
    if (m_searchSession.cancelRequested()
        || m_multiNextIndex >= m_multiTabs.size()) {
        m_searchSession.setDispatchComplete(true);
        finishMultiSearchIfReady(generation);
        return;
    }

    const QPointer<DocumentSearchSurface> guard = m_multiTabs.at(m_multiNextIndex++);
    DocumentSearchSurface *tab = guard.data();
    if (tab && !tab->isL2Mode()) {
        m_searchSession.scheduleTarget();
        QPointer<DocumentSearchSurface> target(tab);
        const bool started = tab->findAllL1Async(
            m_searchSession.query().pattern, m_searchSession.query().regex,
            m_searchSession.query().caseSensitive,
            m_searchSession.query().wholeWord, this,
            [this, target, generation](QVector<SearchMatch> matches,
                                       bool truncated, QString error) mutable {
                if (!m_searchSession.isCurrent(generation))
                    return;
                if (target) {
                    target->setSearchHighlights(matches);
                    if (m_resultsDock)
                        m_resultsDock->appendMultiFileResults(
                            resultTarget(target), matches,
                            truncated || !error.isEmpty());
                }
                m_searchSession.settleTarget();
                finishMultiSearchIfReady(generation);
            });
        if (!started) {
            m_searchSession.settleTarget();
            if (m_resultsDock)
                m_resultsDock->appendMultiFileResults(
                    resultTarget(tab), QVector<SearchMatch>{}, true);
        }
    } else if (tab) {
        m_searchSession.scheduleTarget();
        QPointer<DocumentSearchSurface> target(tab);
        const TextEncoding encodingAtStart = tab->displayEncoding();
        const bool bytePositionModeAtStart =
            tab->isBinaryDocument() || tab->isHexPresentation();
        auto settled = QSharedPointer<bool>::create(false);
        m_multiConnections.push_back(connect(tab, &DocumentSearchSurface::findAllL2Progress,
            this, [this, target, generation, settled](int percent, qint64 done, qint64 total) {
                if (m_searchSession.isCurrent(generation) && !*settled && target && target == m_tab)
                    onL2FindAllProgress(percent, done, total);
            }));
        const auto partialConnection = connect(
            tab, &DocumentSearchSurface::findAllL2Partial, this,
            [this, target, generation](const QVector<LargeFileSearchMatch> &batch,
                                       const QString &, const QString &,
                                       TextEncoding encoding,
                                       bool bytePositionMode) {
                if (m_searchSession.isCurrent(generation) && target && m_resultsDock)
                    m_resultsDock->appendMultiFilePreview(
                        resultTarget(target), batch, encoding,
                        bytePositionMode);
            });
        m_multiConnections.push_back(partialConnection);
        const auto finishedConnection = connect(
            tab, &DocumentSearchSurface::findAllL2Finished, this,
            [this, target, generation, settled](SearchResultsModel *model,
                                                const QString &, const QString &) {
                if (!m_searchSession.isCurrent(generation) || *settled) {
                    if (model) model->deleteLater();
                    return;
                }
                *settled = true;
                if (target && model && m_resultsDock) {
                    m_resultsDock->appendMultiFileResults(
                        resultTarget(target), model);
                    model = nullptr;
                } else if (target && m_resultsDock) {
                    m_resultsDock->invalidateResultsFor(
                        target->documentState().id);
                }
                if (model)
                    model->deleteLater();
                m_searchSession.settleTarget();
                finishMultiSearchIfReady(generation);
            });
        m_multiConnections.push_back(finishedConnection);
        const auto destroyedConnection = connect(
            tab, &QObject::destroyed, this,
            [this, generation, settled]() {
                if (!m_searchSession.isCurrent(generation) || *settled)
                    return;
                *settled = true;
                m_searchSession.settleTarget();
                finishMultiSearchIfReady(generation);
            });
        m_multiConnections.push_back(destroyedConnection);

        if (tab->findAllL2Async(m_searchSession.query().pattern,
                                m_searchSession.query().regex,
                                m_searchSession.query().caseSensitive,
                                m_searchSession.query().wholeWord,
                                m_resultsDock, m_searchSession.query().hexBytes)) {
            if (m_resultsDock)
                m_resultsDock->beginMultiFileTarget(
                    resultTarget(tab), encodingAtStart,
                    bytePositionModeAtStart);
        } else {
            *settled = true;
            m_searchSession.settleTarget();
            if (m_resultsDock) {
                auto *incomplete = new SearchResultsModel(m_resultsDock);
                incomplete->setIncompleteReason(QStringLiteral("搜索任务未能启动。"),
                    QStringLiteral("未能开始查找；请检查文件是否可用后重试。"));
                m_resultsDock->appendMultiFileResults(
                    resultTarget(tab), incomplete);
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
    if (!m_searchSession.isCurrent(generation)
        || !m_searchSession.dispatchComplete()
        || m_searchSession.pendingTargets() > 0) {
        return;
    }
    for (const auto &connection : std::as_const(m_multiConnections))
        QObject::disconnect(connection);
    m_multiConnections.clear();
    if (m_resultsDock)
        m_resultsDock->finishMultiFileResults(m_searchSession.cancelRequested());
    const bool cancelled = m_searchSession.cancelRequested();
    m_multiTabs.clear();
    m_searchSession.clearQuery();
    setL2SearchUiBusy(false);
    if (cancelled && m_statusLabel)
        m_statusLabel->setText(QStringLiteral("搜索已取消，已保留完成的结果。"));
    else if ((m_searchSession.skippedTextTargets() > 0
              || m_searchSession.skippedLockedTargets() > 0)
             && m_statusLabel)
        m_statusLabel->setText(
            QStringLiteral("搜索完成：跳过文本 %1 个、锁定文档 %2 个。")
                .arg(m_searchSession.skippedTextTargets())
                .arg(m_searchSession.skippedLockedTargets()));
    m_searchSession.clearCancelRequest();
}

void FindReplaceDialog::cancelActiveSearch()
{
    if (!m_l2SearchInProgress)
        return;
    if (m_searchSession.query().scope == SearchScope::AllOpenDocuments) {
        m_searchSession.requestCancel();
        for (DocumentSearchSurface *tab : openTabs())
            tab->cancelFindAllL2();
        if (m_multiNextIndex >= m_multiTabs.size())
            m_searchSession.setDispatchComplete(true);
        finishMultiSearchIfReady(m_searchSession.generation());
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
void FindReplaceDialog::setL2SearchUiBusy(bool busy, const QString& msg,
                                          bool allowNavigation)
{
    m_l2SearchInProgress = busy;
    m_allowNavigationDuringBusy = busy && allowNavigation;
    m_cancelSearchBtn->setVisible(busy);
    m_cancelSearchBtn->setEnabled(busy);
    if (m_statusLabel) m_statusLabel->setText(busy ? msg : QString());
    updateQueryUi();
    if (!busy && !(m_navigationRequest && m_navigationRequest->active())) {
        if (auto *mw = qobject_cast<QMainWindow*>(window()))
            mw->statusBar()->clearMessage();
    }
}

void FindReplaceDialog::updateNavigationUi(const QString& msg)
{
    updateQueryUi();
    if (!m_statusLabel)
        return;
    if (!msg.isEmpty())
        m_statusLabel->setText(msg);
    else if (!m_l2SearchInProgress)
        m_statusLabel->clear();
}

void FindReplaceDialog::onL2FindAllProgress(int percent, qint64 done, qint64 total)
{
    if (!m_l2SearchInProgress) return;
    const QString msg = searchProgressText(percent, done, total);
    if (m_statusLabel) m_statusLabel->setText(msg);
    // 同步刷新 MainWindow 状态栏（如果能拿到）
    if (auto *mw = qobject_cast<QMainWindow*>(window())) {
        mw->statusBar()->showMessage(msg, 0);
    }
}

void FindReplaceDialog::onL2FindAllFinished(QPointer<SingleFindAllRequest> request,
                                            QPointer<DocumentSearchSurface> target,
                                            SearchResultsModel* model,
                                            const QString& pattern,
                                            const QString& fileName)
{
    if (!request || !request->current(m_searchSession, m_tab, false)
        || m_searchSession.pendingTargets() == 0)
        return;
    const QPointer<FindReplaceDialog> owner(this);
    const QPointer<SearchResultsModel> heldModel(model);
    auto reclaimUnpublished = qScopeGuard([heldModel] {
        if (heldModel)
            delete heldModel.data();
    });
    if (model)
        model->setParent(nullptr);
    if (!owner || !request || !request->current(m_searchSession, m_tab, false)
        || m_searchSession.pendingTargets() == 0)
        return;
    finishSingleSearch();
    if (!owner || !request || !request->current(m_searchSession, m_tab, false)
        || !m_resultsDock || (model && !heldModel))
        return;
    if (model) {
        heldModel->setParent(m_resultsDock);
        if (!owner || !heldModel || !request || !request->current(m_searchSession, m_tab, false))
            return;
        reclaimUnpublished.dismiss();
        m_resultsDock->setResultsModel(
            heldModel, pattern, fileName, resultTarget(target));
    } else
        m_resultsDock->invalidateResultsFor(request->documentId());
    if (!owner || !request || !request->current(m_searchSession, m_tab, false))
        return;
    m_resultsDock->show();
    m_resultsDock->raise();
}
