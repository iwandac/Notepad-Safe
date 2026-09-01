#include "RecoveryIssuePage.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QString recoveryCause(const QString &code, RecoveryIssuePage::Kind kind)
{
    // Codes are selected by the recovery authority. Raw backend prose is
    // deliberately not parsed, translated or guessed at by this view.
    if (code == QLatin1String("[recovery.original.missing]"))
        return QObject::tr("找不到可打开的原文件。请确认路径指向文件，而不是目录，再重试打开。");
    if (code == QLatin1String("[recovery.original.invalid]"))
        return QObject::tr("这项记录包含未保存修改、恢复稿或安全文档信息，不能作为普通原文件直接打开。请查看技术详情后重试。");
    if (code == QLatin1String("[recovery.original.unreadable]"))
        return QObject::tr("当前无法读取原文件，请检查文件是否可用后重试打开。");
    if (code == QLatin1String("[recovery.original.secure]")
        || code == QLatin1String("[recovery.source.secure]"))
        return QObject::tr("此路径当前是安全文件，不能按普通文件恢复；请在窗口内用文件→打开。本次不会解锁。");
    if (code == QLatin1String("[recovery.draft.missing]"))
        return QObject::tr("未找到此项未保存内容的恢复稿，不能用磁盘原文件替代。请检查恢复稿后重试。");
    if (code == QLatin1String("[recovery.draft.invalid]"))
        return QObject::tr("恢复稿格式无效或无法安全读取，未保存内容尚未恢复。请查看技术详情后检查恢复稿，再重试。");
    if (code == QLatin1String("[recovery.draft.unlock]"))
        return QObject::tr("当前无法解锁加密恢复稿。请确认使用原来的 Windows 用户环境后重试。");
    if (code == QLatin1String("[recovery.source.changed]"))
        return QObject::tr("原文件已移动或发生变化，未保存内容尚未恢复。请找回对应的原文件后重试。");
    if (code == QLatin1String("[recovery.discard.failed]"))
        return QObject::tr("未能完成放弃恢复操作。请查看技术详情，检查恢复稿状态后再操作。");
    return kind == RecoveryIssuePage::Kind::OriginalFile
        ? QObject::tr("原文件尚未打开。请查看技术详情，检查文件后重试打开。")
        : QObject::tr("未保存内容尚未恢复。请查看技术详情，检查恢复稿后重试。");
}

} // namespace

RecoveryIssuePage::RecoveryIssuePage(SessionTab state, Kind kind, QString diagnosticCode,
                                     QString detail, QWidget *parent)
    : QWidget(parent), state_(std::move(state)), kind_(kind)
{
    const bool originalFile = kind_ == Kind::OriginalFile;
    setObjectName(QStringLiteral("RecoveryIssuePage"));
    setAccessibleName(originalFile ? tr("待打开的原文件") : tr("未完成的文档恢复"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 28, 28, 28);
    // A small workspace may need to scroll the complete explanation/actions;
    // do not compress wrapped labels to fit a fixed-height centered card.
    auto *pageScroll = new QScrollArea(this);
    pageScroll->setObjectName(QStringLiteral("recoveryIssuePageScroll"));
    pageScroll->setFrameShape(QFrame::NoFrame);
    pageScroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
    contentLayout->addStretch();
    auto *panel = new QFrame(content);
    panel->setObjectName(QStringLiteral("recoveryIssuePanel"));
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setMaximumWidth(720);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(panel);
    layout->setSpacing(12);

    const QString fileName = state_.filePath.isEmpty()
        ? tr("未命名文档") : QFileInfo(state_.filePath).fileName();
    auto *title = new QLabel((originalFile ? tr("未能重新打开“%1”")
                                           : tr("未能恢复“%1”的未保存修改")).arg(fileName), panel);
    title->setObjectName(QStringLiteral("recoveryIssueTitle"));
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *summary = new QLabel(
        originalFile
            ? tr("原文件没有被本次恢复修改。请检查文件后重试打开，或在窗口内用文件→打开。"
                 "移除标签只移除此待打开项。")
            : tr("原文件没有被本次恢复修改；未保存内容尚未恢复。"
                 "你可以在文件状态恢复后重试，也可以明确放弃此项未保存恢复。"),
        panel);
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    summary->setObjectName(QStringLiteral("recoveryIssueSummary"));
    layout->addWidget(summary);

    causeLabel_ = new QLabel(panel);
    causeLabel_->setObjectName(QStringLiteral("recoveryIssueCause"));
    causeLabel_->setTextFormat(Qt::PlainText);
    causeLabel_->setWordWrap(true);
    layout->addWidget(causeLabel_);
    detailsToggle_ = new QPushButton(tr("技术详情"), panel);
    detailsToggle_->setObjectName(QStringLiteral("recoveryIssueDetailsToggle"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setAccessibleDescription(tr("展开或收起原始恢复诊断；不会重试或删除文件。"));
    layout->addWidget(detailsToggle_, 0, Qt::AlignLeft);
    codeLabel_ = new QLabel(panel);
    codeLabel_->setObjectName(QStringLiteral("recoveryIssueCode"));
    codeLabel_->setTextFormat(Qt::PlainText);
    codeLabel_->setWordWrap(true);
    codeLabel_->setTextInteractionFlags(Qt::TextSelectableByKeyboard
                                        | Qt::TextSelectableByMouse);
    layout->addWidget(codeLabel_);
    detailLabel_ = new QLabel(panel);
    detailLabel_->setObjectName(QStringLiteral("recoveryIssueDetail"));
    detailLabel_->setTextFormat(Qt::PlainText);
    detailLabel_->setWordWrap(true);
    detailLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    detailLabel_->setTextInteractionFlags(Qt::TextSelectableByKeyboard
                                          | Qt::TextSelectableByMouse);
    // Keep the original selectable label and full diagnostic text, but bound
    // its viewport so a long backend message cannot grow the entire card.
    auto *detailScroll = new QScrollArea(panel);
    detailScroll->setObjectName(QStringLiteral("recoveryIssueDetailScroll"));
    detailScroll->setAccessibleName(tr("恢复技术详情"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setMaximumHeight(180);
    detailScroll->setWidget(detailLabel_);
    layout->addWidget(detailScroll);
    codeLabel_->hide();
    detailLabel_->hide();
    detailScroll->hide();
    connect(detailsToggle_, &QPushButton::toggled, this, [this, detailScroll](bool expanded) {
        detailsToggle_->setText(expanded ? tr("收起技术详情") : tr("技术详情"));
        codeLabel_->setVisible(expanded);
        detailLabel_->setVisible(expanded);
        detailScroll->setVisible(expanded);
    });

    auto *actions = new QHBoxLayout;
    auto *retry = new QPushButton(originalFile ? tr("重试打开") : tr("重试恢复"), panel);
    retry->setObjectName(QStringLiteral("recoveryIssueRetry"));
    discardButton_ = new QPushButton(originalFile ? tr("移除标签") : tr("放弃恢复"), panel);
    discardButton_->setObjectName(originalFile ? QStringLiteral("recoveryIssueRemove")
                                              : QStringLiteral("recoveryIssueDiscard"));
    actions->addWidget(retry);
    actions->addWidget(discardButton_);
    actions->addStretch();
    layout->addLayout(actions);

    auto *confirmation = new QHBoxLayout;
    auto *warning = new QLabel(tr("这会放弃此项未保存恢复；存在的受管恢复稿将被删除，原文件不会被修改。"), panel);
    warning->setWordWrap(true);
    warning->setTextFormat(Qt::PlainText);
    confirmDiscardButton_ = new QPushButton(tr("确认放弃恢复"), panel);
    confirmDiscardButton_->setObjectName(
        QStringLiteral("recoveryIssueConfirmDiscard"));
    cancelDiscardButton_ = new QPushButton(tr("取消"), panel);
    cancelDiscardButton_->setObjectName(
        QStringLiteral("recoveryIssueCancelDiscard"));
    confirmation->addWidget(warning, 1);
    confirmation->addWidget(confirmDiscardButton_);
    confirmation->addWidget(cancelDiscardButton_);
    layout->addLayout(confirmation);
    warning->setVisible(false);
    confirmDiscardButton_->setVisible(false);
    cancelDiscardButton_->setVisible(false);

    connect(retry, &QPushButton::clicked, this, &RecoveryIssuePage::retryRequested);
    connect(discardButton_, &QPushButton::clicked, this,
            [this, warning]() {
        if (kind_ == Kind::OriginalFile) {
            emit originalRemovalRequested();
            return;
        }
        discardButton_->setEnabled(false);
        warning->setVisible(true);
        confirmDiscardButton_->setVisible(true);
        cancelDiscardButton_->setVisible(true);
        confirmDiscardButton_->setFocus(Qt::OtherFocusReason);
    });
    connect(cancelDiscardButton_, &QPushButton::clicked, this,
            [this, warning]() {
        warning->setVisible(false);
        confirmDiscardButton_->setVisible(false);
        cancelDiscardButton_->setVisible(false);
        discardButton_->setEnabled(true);
        discardButton_->setFocus(Qt::OtherFocusReason);
    });
    connect(confirmDiscardButton_, &QPushButton::clicked,
            this, &RecoveryIssuePage::discardConfirmed);

    // The card receives its actual available width (up to 720) before its
    // height-for-width is measured. AlignHCenter on the widget would instead
    // shrink it to a heuristic sizeHint while retaining a wider height budget.
    auto *panelRow = new QHBoxLayout;
    panelRow->setContentsMargins(0, 0, 0, 0);
    panelRow->addStretch(0);
    panelRow->addWidget(panel, 1);
    panelRow->addStretch(0);
    contentLayout->addLayout(panelRow);
    contentLayout->addStretch();
    pageScroll->setWidget(content);
    outer->addWidget(pageScroll);
    updateFailure(diagnosticCode, detail);
}

void RecoveryIssuePage::updateFailure(const QString &diagnosticCode,
                                      const QString &detail)
{
    diagnosticCode_ = diagnosticCode;
    causeLabel_->setText(recoveryCause(diagnosticCode_, kind_));
    causeLabel_->setAccessibleName(tr("恢复状态"));
    causeLabel_->setAccessibleDescription(causeLabel_->text());
    codeLabel_->setText(diagnosticCode_);
    detailLabel_->setText(detail.isEmpty()
        ? (kind_ == Kind::OriginalFile ? tr("原文件暂时无法打开，请检查文件后重试。")
                                      : tr("未保存内容暂时无法恢复，请检查恢复稿后重试。"))
        : detail);
    detailsToggle_->setChecked(false);
}
