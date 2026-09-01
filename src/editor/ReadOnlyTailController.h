#pragma once

#include "OrdinaryReadSource.h"
#include <QPointer>
#include <QWidget>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class WindowedReadOnlyProvider;

// One document's reading intent, not another source, watcher or query owner.
class ReadOnlyTailController final : public QWidget
{
    Q_OBJECT
public:
    enum class State { Off, Following, Paused, Stopped };
    Q_ENUM(State)
    explicit ReadOnlyTailController(WindowedReadOnlyProvider *provider);
    State state() const { return m_state; }
    bool tailEnabled() const { return m_state != State::Off; }
    bool canResume() const { return m_state == State::Paused; }
    QString statusText() const;
    void setTailEnabled(bool enabled);
    void resumeAtEnd();

signals:
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class WindowedReadOnlyProvider;
    struct Candidate {
        DiskFileStamp stamp;
        QByteArray eofBytes;
        QString stopReason;
        quint64 intent = 0;
        bool readable = true;
    };
    enum class Projection { Refresh, Preserved, Failed };
    Candidate prepareCandidate(const DiskFileStamp &stamp,
        const std::function<QByteArray(qint64, qsizetype)> &readAt) const;
    Projection acceptCandidate(const Candidate &candidate, QString *error);
    void observeSource(const DiskFileStamp &stamp, DiskFileState state);
    void sourceFailed(const OrdinaryReadResult &failure);
    void pauseForReading();
    void reset();
    void setState(State state, const QString &reason = {});
    void updateStatus();
    bool followEnd(QString *error = nullptr);
    bool internalProjection() const;
    std::optional<quint64> projectionIntent() const;
    bool permitsProjection(const std::optional<quint64> &intent) const;
    bool atEnd() const;
    QString changedReason(const DiskFileStamp &stamp) const;

    QPointer<WindowedReadOnlyProvider> m_provider;
    State m_state = State::Off;
    QString m_stopReason;
    // This is only the bounded evidence for the user's current Tail intent.
    // The provider's accepted version remains the document source authority.
    DiskFileStamp m_checkpoint;
    QByteArray m_eofBytes;
    quint64 m_intent = 0;
    bool m_followingInternally = false;
    QLabel *m_status = nullptr;
    QPushButton *m_resume = nullptr;
};
