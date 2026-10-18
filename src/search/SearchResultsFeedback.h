#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;
class QMenu;
class QPlainTextEdit;
class SearchResultsModel;
class MultiSearchResultsModel;

// Presentation only. The dock supplies already-decided summaries and raw
// diagnostics; this widget owns no query, result, generation or cancel state.
class SearchResultsFeedback final : public QWidget {
    Q_OBJECT
public:
    explicit SearchResultsFeedback(QWidget *parent = nullptr);
    void setSummaryText(const QString &text);
    void setRerunAvailable(bool available);
    void setTechnicalDetails(const QStringList &details);
    void resetTechnicalDetails();
    // Newest first; an empty list hides the button.
    void setRecentSearches(const QStringList &labels);

signals:
    void rerunRequested();
    void recentSearchChosen(int index);

private:
    QLabel *summary_ = nullptr;
    QPushButton *rerunButton_ = nullptr;
    QToolButton *historyButton_ = nullptr;
    QMenu *historyMenu_ = nullptr;
    QPushButton *detailsToggle_ = nullptr;
    QPlainTextEdit *technicalDetails_ = nullptr;
};

// A temporary read-only projection. Neither model ownership nor stored
// completeness/target identity is changed by collecting diagnostics.
QStringList searchResultDiagnostics(const SearchResultsModel *single,
                                    const MultiSearchResultsModel *multi);
QString searchProgressText(int percent, qint64 done, qint64 total);
