#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QLockFile;
class SingleInstanceContractTest;

// Owns the per-profile Windows local endpoint before any session-capable UI is
// constructed. A secondary process can only submit one bounded, versioned
// activation/open request; document content and editor state are never part of
// this protocol.
class SingleInstanceCoordinator final : public QObject {
    Q_OBJECT
public:
    struct StartResult {
        enum class Disposition { Primary, Forwarded, Failed };
        Disposition disposition = Disposition::Failed;
        int exitCode = 2;
        QString userMessage;
    };

    explicit SingleInstanceCoordinator(QObject *parent = nullptr);
    ~SingleInstanceCoordinator() override;

    StartResult start(const QStringList &paths);
    // Idempotently closes the forwarding gate and joins the I/O worker while
    // retaining the session-writer lock until this coordinator is destroyed.
    void stopAccepting();
    // Close the gate, join the worker, then synchronously deliver every
    // request that was already ACKed. Must be called on the coordinator's
    // owning thread before the final session save begins.
    void stopAcceptingAndDrain();
    void setDeliveryReady();
    QString endpointName() const { return endpointName_; }
    QString lockFilePath() const { return lockFilePath_; }

signals:
    void requestReceived(const QStringList &paths);

private:
    friend class SingleInstanceContractTest;
    struct IoState;
    struct PendingState;
    enum class EnqueueResult { Accepted, QueueFull, Stopping, AckFailed };
    bool startIoThread();
    void stopIoThread();
    EnqueueResult enqueueRequest(const QStringList &paths,
                                 const std::function<bool()> &acknowledge);
    void drainPendingRequests();
    StartResult forwardToPrimary(const QStringList &paths);

    QString dataRoot_;
    QString endpointName_;
    QString lockFilePath_;
    std::unique_ptr<QLockFile> ownerLock_;
    std::unique_ptr<IoState> ioState_;
    std::unique_ptr<PendingState> pendingState_;
};
