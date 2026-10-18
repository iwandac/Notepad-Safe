#include "SearchResultsFeedback.h"

#include "SearchResultsAggregateModel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMenu>
#include <QToolButton>

QString searchProgressText(int percent, qint64 done, qint64 total)
{
    QString msg;
    if (percent < 0) {
        msg = QObject::tr("准备搜索…");
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
        const QString precisePercent = total > 0
            ? QString::number(qBound(0.0, double(done) * 100.0 / double(total),
                                     100.0), 'f', 2)
            : QString::number(qMax(0, percent), 'f', 2);
        msg = QObject::tr("搜索中 %1% · %2 / %3")
                  .arg(precisePercent, doneStr, totalStr);
    }
    return msg;
}

SearchResultsFeedback::SearchResultsFeedback(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *summaryRow = new QHBoxLayout;
    summaryRow->setContentsMargins(0, 0, 0, 0);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("searchResultsSummary"));
    summary_->setTextFormat(Qt::PlainText);
    summary_->setAccessibleName(QStringLiteral("查找状态"));
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setMinimumWidth(0);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rerunButton_ = new QPushButton(QStringLiteral("重新搜索"), this);
    rerunButton_->setObjectName(QStringLiteral("searchResultsRerunButton"));
    rerunButton_->setToolTip(QStringLiteral("使用当前查找条件重新生成有效结果"));
    rerunButton_->hide();
    historyButton_ = new QToolButton(this);
    historyButton_->setObjectName(QStringLiteral("searchHistoryButton"));
    historyButton_->setText(QStringLiteral("↶ 最近搜索"));
    historyButton_->setToolTip(QStringLiteral("用最近的查找条件重新查找全部"));
    historyButton_->setPopupMode(QToolButton::InstantPopup);
    historyButton_->setAutoRaise(true);
    historyMenu_ = new QMenu(historyButton_);
    historyMenu_->setObjectName(QStringLiteral("searchHistoryMenu"));
    historyButton_->setMenu(historyMenu_);
    historyButton_->hide();
    summaryRow->addWidget(historyButton_);
    summaryRow->addWidget(summary_, 1);
    summaryRow->addWidget(rerunButton_);
    detailsToggle_ = new QPushButton(QStringLiteral("技术详情"), this);
    detailsToggle_->setObjectName(QStringLiteral("searchResultsDetailsToggle"));
    detailsToggle_->setAccessibleDescription(QStringLiteral("展开或收起本次查找的原始诊断，不重新执行查找。"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->hide();
    summaryRow->addWidget(detailsToggle_);
    layout->addLayout(summaryRow);
    technicalDetails_ = new QPlainTextEdit(this);
    technicalDetails_->setObjectName(QStringLiteral("searchResultsTechnicalDetails"));
    technicalDetails_->setAccessibleName(QStringLiteral("查找技术详情"));
    technicalDetails_->setReadOnly(true);
    technicalDetails_->setMaximumHeight(140);
    technicalDetails_->hide();
    layout->addWidget(technicalDetails_);
    connect(detailsToggle_, &QPushButton::toggled, this, [this](bool expanded) {
        detailsToggle_->setText(expanded ? QStringLiteral("收起技术详情")
                                        : QStringLiteral("技术详情"));
        technicalDetails_->setVisible(expanded);
    });
    connect(rerunButton_, &QPushButton::clicked, this, &SearchResultsFeedback::rerunRequested);
}

void SearchResultsFeedback::setSummaryText(const QString &text)
{
    summary_->setText(text);
    summary_->setAccessibleDescription(text);
}

void SearchResultsFeedback::setRerunAvailable(bool available)
{
    rerunButton_->setVisible(available);
}

void SearchResultsFeedback::resetTechnicalDetails()
{
    detailsToggle_->setChecked(false);
    detailsToggle_->hide();
    technicalDetails_->hide();
    technicalDetails_->clear();
}

void SearchResultsFeedback::setTechnicalDetails(const QStringList &details)
{
    technicalDetails_->setPlainText(details.join(QStringLiteral("\n\n")));
    detailsToggle_->setVisible(!details.isEmpty());
    if (details.isEmpty()) detailsToggle_->setChecked(false);
}

QStringList searchResultDiagnostics(const SearchResultsModel *single,
                                    const MultiSearchResultsModel *multi)
{
    QStringList details;
    if (multi) {
        for (int row = 0; row < multi->rowCount(); ++row) {
            const auto *group = multi->groupFor(multi->index(row, 0));
            if (group && !group->incompleteReason.isEmpty())
                details << group->target.displayName + QLatin1Char('\n') + group->incompleteReason;
        }
    } else if (single && single->isIncomplete()) {
        details << single->incompleteReason();
    }
    return details;
}

void SearchResultsFeedback::setRecentSearches(const QStringList &labels)
{
    historyMenu_->clear();
    for (int i = 0; i < labels.size(); ++i) {
        QAction *action = historyMenu_->addAction(labels.at(i));
        connect(action, &QAction::triggered, this, [this, i] { emit recentSearchChosen(i); });
    }
    historyButton_->setVisible(!labels.isEmpty());
}
