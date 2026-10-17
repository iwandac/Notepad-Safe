#pragma once

#include "ThemeTokens.h"

#include <QFrame>
#include <QTimer>
#include <QVector>

#include <functional>

class QHBoxLayout;
class QLabel;
class QToolButton;

enum class NoticeKind { Info, Error };

struct NoticeAction {
    QString label;
    std::function<void()> run;
};

// One non-blocking notice under the title bar, with up to two actions and a
// dismiss button. A newer notice replaces the current one. autoHideMs <= 0
// keeps it until dismissed or an action runs.
class NoticeBar final : public QFrame {
    Q_OBJECT

public:
    explicit NoticeBar(QWidget *parent = nullptr);

    void setTokens(const ThemeTokens &tokens);
    void showNotice(NoticeKind kind, const QString &text,
                    QVector<NoticeAction> actions = {}, int autoHideMs = 0);
    void dismiss();

private:
    void applyStyle();

    ThemeTokens m_tokens = ThemeTokens::light();
    NoticeKind m_kind = NoticeKind::Info;
    QLabel *m_text = nullptr;
    QHBoxLayout *m_actions = nullptr;
    QToolButton *m_dismiss = nullptr;
    QTimer m_autoHide;
};
