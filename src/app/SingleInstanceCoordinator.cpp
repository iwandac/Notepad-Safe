#include "SingleInstanceCoordinator.h"
#include "AppDataLocation.h"
#include "DedicatedThread.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace {
constexpr int ProtocolVersion = 1;
constexpr int MaximumPaths = 32;
constexpr int MaximumPathBytes = 32 * 1024;
constexpr int MaximumPayloadBytes = 64 * 1024;
constexpr int HandshakeTimeoutMs = 5 * 1000;
constexpr int ServerReadTimeoutMs = 2 * 1000;
constexpr int MaximumLiveConnections = 8;
constexpr int MaximumQueuedRequests = 32;
constexpr int MaximumQueuedPaths = 256;
constexpr qsizetype MaximumQueuedPathBytes = 512 * 1024;

SingleInstanceCoordinator::StartResult failedStart(
    int exitCode, const QString &userMessage)
{
    return {SingleInstanceCoordinator::StartResult::Disposition::Failed,
            exitCode, userMessage};
}

QByteArray frame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray framed;
    QDataStream output(&framed, QIODevice::WriteOnly);
    output.setByteOrder(QDataStream::BigEndian);
    output << quint32(payload.size());
    framed.append(payload);
    return framed;
}

QString endpointForDataRoot(const QString &dataRoot)
{
    QString identity = QFileInfo(dataRoot).canonicalFilePath();
    if (identity.isEmpty())
        identity = QFileInfo(dataRoot).absoluteFilePath();
#ifdef Q_OS_WIN
    identity = QDir::toNativeSeparators(identity).toCaseFolded();
#endif
    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    // The endpoint is deliberately version-independent. A newer executable
    // must meet the existing owner and receive an explicit protocol rejection,
    // never listen on a parallel endpoint and become a second session writer.
    return QStringLiteral("NotepadSafe.SingleInstance.%1")
        .arg(QString::fromLatin1(digest));
}

QString normalizedAbsolutePath(const QString &path)
{
    if (path.contains(QChar::Null))
        return {};
    const QFileInfo supplied(path);
    if (!supplied.isAbsolute())
        return {};
    QString normalized = supplied.canonicalFilePath();
    if (normalized.isEmpty())
        normalized = supplied.absoluteFilePath();
    return QDir::cleanPath(normalized);
}

bool decodeRequest(const QByteArray &payload, QStringList *paths,
                   QString *failureCode)
{
    if (!paths || payload.isEmpty() || payload.size() > MaximumPayloadBytes)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_json");
        return false;
    }
    const QJsonObject object = document.object();
    static const QSet<QString> allowedKeys{
        QStringLiteral("version"), QStringLiteral("action"),
        QStringLiteral("paths")};
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowedKeys.contains(it.key())) {
            if (failureCode)
                *failureCode = QStringLiteral("undefined_field");
            return false;
        }
    }
    if (object.value(QStringLiteral("version")).toInt(-1) != ProtocolVersion) {
        if (failureCode)
            *failureCode = QStringLiteral("incompatible_version");
        return false;
    }
    const QString action = object.value(QStringLiteral("action")).toString();
    const QJsonValue pathValue = object.value(QStringLiteral("paths"));
    if ((action != QLatin1String("activate")
         && action != QLatin1String("open_files"))
        || !pathValue.isArray()) {
        if (failureCode)
            *failureCode = QStringLiteral("undefined_action");
        return false;
    }
    const QJsonArray pathArray = pathValue.toArray();
    if (pathArray.size() > MaximumPaths
        || (action == QLatin1String("activate") && !pathArray.isEmpty())
        || (action == QLatin1String("open_files") && pathArray.isEmpty())) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_path_count");
        return false;
    }
    QStringList normalized;
    normalized.reserve(pathArray.size());
    for (const QJsonValue &value : pathArray) {
        if (!value.isString()
            || value.toString().toUtf8().size() > MaximumPathBytes) {
            if (failureCode)
                *failureCode = QStringLiteral("invalid_path");
            return false;
        }
        const QString path = normalizedAbsolutePath(value.toString());
        if (path.isEmpty()) {
            if (failureCode)
                *failureCode = QStringLiteral("invalid_path");
            return false;
        }
        normalized.append(path);
    }
    *paths = normalized;
    return true;
}

QByteArray encodeRequest(const QStringList &paths, QString *error)
{
    if (paths.size() > MaximumPaths) {
        if (error)
            *error = QObject::tr("一次最多可以打开 %1 个文件。").arg(MaximumPaths);
        return {};
    }
    QJsonArray array;
    for (const QString &supplied : paths) {
        const QString path = normalizedAbsolutePath(supplied);
        if (path.isEmpty() || path.toUtf8().size() > MaximumPathBytes) {
            if (error)
                *error = QObject::tr("文件路径无效或过长，未发送打开请求。");
            return {};
        }
        array.append(path);
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), ProtocolVersion);
    object.insert(QStringLiteral("action"), paths.isEmpty()
                      ? QStringLiteral("activate")
                      : QStringLiteral("open_files"));
    object.insert(QStringLiteral("paths"), array);
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.size() > MaximumPayloadBytes) {
        if (error)
            *error = QObject::tr("打开请求过大，未发送给现有窗口。");
        return {};
    }
    return frame(object);
}


bool readOneFrame(QLocalSocket *socket, QByteArray *payload, int timeoutMs)
{
    QByteArray received;
    QElapsedTimer elapsed;
    elapsed.start();
    quint32 expected = 0;
    const auto consumeAvailable = [&]() -> int {
        received.append(socket->readAll());
        if (received.size() >= 4 && expected == 0) {
            QDataStream input(received.left(4));
            input.setByteOrder(QDataStream::BigEndian);
            input >> expected;
            if (expected == 0 || expected > MaximumPayloadBytes)
                return -1;
        }
        if (expected > 0 && received.size() == int(4 + expected)) {
            *payload = received.mid(4);
            return 2;
        }
        if (expected > 0 && received.size() > int(4 + expected))
            return -1;
        return 0;
    };
    while (elapsed.elapsed() < timeoutMs) {
        const int status = consumeAvailable();
        if (status < 0)
            return false;
        if (socket->state() == QLocalSocket::UnconnectedState)
            return status == 2;
        const int remaining = qMax(1, timeoutMs - int(elapsed.elapsed()));
        if (!socket->waitForReadyRead(qMin(remaining, 250))) {
            // A Windows named-pipe close can complete in the same wait as the
            // final frame. Drain after every false wait before inspecting the
            // transport state.
            const int finalStatus = consumeAvailable();
            if (finalStatus < 0
                || socket->state() == QLocalSocket::UnconnectedState) {
                return finalStatus == 2;
            }
        }
    }
    return false;
}

bool decodeResponse(const QByteArray &payload, bool *accepted,
                    QString *failureCode)
{
    if (!accepted)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_json");
        return false;
    }
    const QJsonObject object = document.object();
    const QJsonValue version = object.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toInt(-1) != ProtocolVersion) {
        if (failureCode)
            *failureCode = QStringLiteral("incompatible_version");
        return false;
    }
    const QJsonValue statusValue = object.value(QStringLiteral("status"));
    if (!statusValue.isString()) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_schema");
        return false;
    }
    const QString status = statusValue.toString();
    if (status == QLatin1String("accepted")) {
        if (object.size() != 2 || object.contains(QStringLiteral("code"))) {
            if (failureCode)
                *failureCode = QStringLiteral("invalid_schema");
            return false;
        }
        *accepted = true;
        return true;
    }
    const QJsonValue code = object.value(QStringLiteral("code"));
    if (status != QLatin1String("rejected") || object.size() != 3
        || !code.isString() || code.toString().isEmpty()) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_schema");
        return false;
    }
    static const QSet<QString> definedCodes{
        QStringLiteral("incompatible_version"),
        QStringLiteral("invalid_json"),
        QStringLiteral("undefined_field"),
        QStringLiteral("undefined_action"),
        QStringLiteral("invalid_path_count"),
        QStringLiteral("invalid_path"),
        QStringLiteral("invalid_request"),
        QStringLiteral("request_too_large"),
        QStringLiteral("trailing_data"),
        QStringLiteral("request_timeout"),
        QStringLiteral("server_busy"),
        QStringLiteral("request_queue_full")};
    if (!definedCodes.contains(code.toString())) {
        if (failureCode)
            *failureCode = QStringLiteral("invalid_schema");
        return false;
    }
    if (failureCode)
        *failureCode = code.toString();
    *accepted = false;
    return true;
}

int remainingHandshakeMs(const QElapsedTimer &timer)
{
    return qMax(0, HandshakeTimeoutMs - int(timer.elapsed()));
}

SingleInstanceCoordinator::StartResult rejectedStart(const QString &code)
{
    using Disposition =
        SingleInstanceCoordinator::StartResult::Disposition;
    if (code == QLatin1String("incompatible_version")) {
        return {Disposition::Failed, 3,
                QObject::tr("正在运行的 Notepad Safe 与此版本不兼容。请关闭旧窗口后重试。")};
    }
    if (code == QLatin1String("server_busy")) {
        return {Disposition::Failed, 4,
                QObject::tr("现有 Notepad Safe 窗口正在处理较多打开请求。请稍后重试；本次没有打开第二个会话。")};
    }
    if (code == QLatin1String("request_queue_full")) {
        return {Disposition::Failed, 4,
                QObject::tr("现有 Notepad Safe 窗口的待打开队列已满。请等待当前文件处理完成后重试；本次没有打开第二个会话。")};
    }
    if (code == QLatin1String("request_timeout")) {
        return {Disposition::Failed, 5,
                QObject::tr("打开请求未能在限定时间内完整送达。请稍后重试；本次没有打开第二个会话。")};
    }
    return {Disposition::Failed, 3,
            QObject::tr("现有 Notepad Safe 窗口拒绝了无效的打开请求。请重新启动两个窗口后重试；本次没有打开第二个会话。")};
}
} // namespace

struct SingleInstanceCoordinator::IoState {
    std::atomic_bool stop{false};
    DedicatedThread thread;
    std::mutex startupMutex;
    std::condition_variable startupChanged;
    bool startupReady = false;
    bool listenSucceeded = false;
};

struct SingleInstanceCoordinator::PendingState {
    std::mutex mutex;
    std::deque<QStringList> requests;
    int pathCount = 0;
    qsizetype pathBytes = 0;
    bool accepting = true;
    bool deliveryReady = false;
    bool deliveryScheduled = false;
};

SingleInstanceCoordinator::SingleInstanceCoordinator(QObject *parent)
    : QObject(parent)
    , dataRoot_(resolveNotepadSafeDataRoot())
    , endpointName_(endpointForDataRoot(dataRoot_))
    , lockFilePath_(QDir(dataRoot_).filePath(
          QStringLiteral("notepad-safe.session.lock")))
    , ioState_(std::make_unique<IoState>())
    , pendingState_(std::make_unique<PendingState>())
{
}

SingleInstanceCoordinator::~SingleInstanceCoordinator()
{
    stopAccepting();
    ownerLock_.reset();
}

bool SingleInstanceCoordinator::startIoThread()
{
    stopIoThread();
    ioState_ = std::make_unique<IoState>();
    IoState *state = ioState_.get();
    const QString endpoint = endpointName_;
    const bool started = state->thread.start([this, state, endpoint]() {
        const auto reportStartup = [state](bool succeeded) {
            {
                std::lock_guard<std::mutex> lock(state->startupMutex);
                if (state->startupReady)
                    return;
                state->listenSucceeded = succeeded;
                state->startupReady = true;
            }
            state->startupChanged.notify_one();
        };
        try {
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.setMaxPendingConnections(MaximumLiveConnections);
        const bool listening = server.listen(endpoint);
        reportStartup(listening);
        if (!listening)
            return;

        struct Connection {
            std::unique_ptr<QLocalSocket> socket;
            QByteArray buffer;
            QElapsedTimer age;
        };
        std::vector<Connection> connections;
        const auto sendResponse = [](QLocalSocket *socket,
                                     const QByteArray &response) -> bool {
            if (!socket || socket->write(response) != response.size()) {
                if (socket)
                    socket->abort();
                return false;
            }
            socket->flush();
            if (socket->bytesToWrite() > 0
                && !socket->waitForBytesWritten(250)
                && socket->bytesToWrite() > 0) {
                socket->abort();
                return false;
            }
            socket->disconnectFromServer();
            if (socket->state() != QLocalSocket::UnconnectedState)
                socket->waitForDisconnected(250);
            if (socket->state() != QLocalSocket::UnconnectedState)
                socket->abort();
            return true;
        };
        const auto reject = [&](QLocalSocket *socket, const QString &code) {
            const QJsonObject response{
                {QStringLiteral("version"), ProtocolVersion},
                {QStringLiteral("status"), QStringLiteral("rejected")},
                {QStringLiteral("code"), code}};
            sendResponse(socket, frame(response));
        };

        while (!state->stop.load(std::memory_order_acquire)) {
            bool timedOut = false;
            server.waitForNewConnection(25, &timedOut);
            if (state->stop.load(std::memory_order_acquire))
                break;
            while (QLocalSocket *accepted = server.nextPendingConnection()) {
                if (state->stop.load(std::memory_order_acquire)) {
                    accepted->abort();
                    delete accepted;
                    break;
                }
                if (connections.size() >= MaximumLiveConnections) {
                    reject(accepted, QStringLiteral("server_busy"));
                    delete accepted;
                    continue;
                }
                Connection connection;
                connection.socket.reset(accepted);
                connection.age.start();
                connections.push_back(std::move(connection));
            }

            for (auto it = connections.begin(); it != connections.end();) {
                if (state->stop.load(std::memory_order_acquire))
                    break;
                QLocalSocket *socket = it->socket.get();
                if (socket->bytesAvailable() == 0
                    && socket->state() != QLocalSocket::UnconnectedState) {
                    socket->waitForReadyRead(1);
                }
                it->buffer.append(socket->readAll());
                bool completed = false;
                QString failureCode;
                quint32 payloadSize = 0;
                if (it->buffer.size() > MaximumPayloadBytes + 4) {
                    failureCode = QStringLiteral("request_too_large");
                } else if (it->buffer.size() >= 4) {
                    QDataStream input(it->buffer.left(4));
                    input.setByteOrder(QDataStream::BigEndian);
                    input >> payloadSize;
                    if (payloadSize == 0
                        || payloadSize > MaximumPayloadBytes) {
                        failureCode = QStringLiteral("request_too_large");
                    } else if (it->buffer.size() > int(4 + payloadSize)) {
                        failureCode = QStringLiteral("trailing_data");
                    } else if (it->buffer.size() == int(4 + payloadSize)) {
                        QStringList paths;
                        if (!decodeRequest(it->buffer.mid(4), &paths,
                                           &failureCode)) {
                            if (failureCode.isEmpty())
                                failureCode = QStringLiteral("invalid_request");
                        } else if (state->stop.load(
                                       std::memory_order_acquire)) {
                            socket->abort();
                            completed = true;
                        } else {
                            const QJsonObject response{
                                {QStringLiteral("version"), ProtocolVersion},
                                {QStringLiteral("status"),
                                 QStringLiteral("accepted")}};
                            const EnqueueResult enqueueResult = enqueueRequest(
                                paths, [&]() {
                                    return sendResponse(socket,
                                                        frame(response));
                                });
                            if (enqueueResult == EnqueueResult::QueueFull) {
                                failureCode = QStringLiteral(
                                    "request_queue_full");
                            } else if (enqueueResult
                                       == EnqueueResult::Stopping) {
                                socket->abort();
                                completed = true;
                            } else {
                                // The request is authoritative once queued.
                                // A transport failure cannot safely roll it
                                // back because the peer may have read the ACK.
                                completed = true;
                            }
                        }
                    }
                }
                if (!completed && failureCode.isEmpty()
                    && it->age.elapsed() >= ServerReadTimeoutMs) {
                    failureCode = QStringLiteral("request_timeout");
                }
                if (!failureCode.isEmpty()) {
                    reject(socket, failureCode);
                    completed = true;
                }
                if (!completed
                    && socket->state() == QLocalSocket::UnconnectedState) {
                    completed = true;
                }
                if (completed)
                    it = connections.erase(it);
                else
                    ++it;
            }
        }
        server.close();
        for (auto &connection : connections)
            connection.socket->abort();
        } catch (...) {
            reportStartup(false);
        }
    });

    if (!started)
        return false;

    std::unique_lock<std::mutex> lock(state->startupMutex);
    if (!state->startupChanged.wait_for(
            lock, std::chrono::seconds(2),
            [state]() { return state->startupReady; })) {
        lock.unlock();
        stopIoThread();
        return false;
    }
    const bool succeeded = state->listenSucceeded;
    lock.unlock();
    if (!succeeded)
        stopIoThread();
    return succeeded;
}

void SingleInstanceCoordinator::stopIoThread()
{
    if (!ioState_)
        return;
    ioState_->stop.store(true, std::memory_order_release);
    ioState_->thread.join();
}

void SingleInstanceCoordinator::stopAccepting()
{
    if (pendingState_) {
        std::lock_guard<std::mutex> lock(pendingState_->mutex);
        pendingState_->accepting = false;
    }
    stopIoThread();
}

void SingleInstanceCoordinator::stopAcceptingAndDrain()
{
    stopAccepting();
    {
        std::lock_guard<std::mutex> lock(pendingState_->mutex);
        pendingState_->deliveryReady = true;
        pendingState_->deliveryScheduled = true;
    }
    drainPendingRequests();
}

SingleInstanceCoordinator::EnqueueResult
SingleInstanceCoordinator::enqueueRequest(
    const QStringList &paths, const std::function<bool()> &acknowledge)
{
    qsizetype requestBytes = 0;
    for (const QString &path : paths)
        requestBytes += path.toUtf8().size();

    bool scheduleDelivery = false;
    bool acknowledged = false;
    {
        std::lock_guard<std::mutex> lock(pendingState_->mutex);
        if (!pendingState_->accepting)
            return EnqueueResult::Stopping;
        if (pendingState_->requests.size() >= MaximumQueuedRequests
            || pendingState_->pathCount + paths.size() > MaximumQueuedPaths
            || pendingState_->pathBytes + requestBytes
                > MaximumQueuedPathBytes) {
            return EnqueueResult::QueueFull;
        }
        pendingState_->requests.push_back(paths);
        pendingState_->pathCount += paths.size();
        pendingState_->pathBytes += requestBytes;
        // Keep the admission mutex through the bounded ACK write. Shutdown
        // closes this same gate, so no accepted ACK can cross the shutdown
        // linearization point.
        acknowledged = acknowledge && acknowledge();
        if (!acknowledged) {
            pendingState_->requests.pop_back();
            pendingState_->pathCount -= paths.size();
            pendingState_->pathBytes -= requestBytes;
            return EnqueueResult::AckFailed;
        }
        if (pendingState_->deliveryReady
            && !pendingState_->deliveryScheduled) {
            pendingState_->deliveryScheduled = true;
            scheduleDelivery = true;
        }
    }
    if (scheduleDelivery) {
        QMetaObject::invokeMethod(
            this, &SingleInstanceCoordinator::drainPendingRequests,
            Qt::QueuedConnection);
    }
    return EnqueueResult::Accepted;
}

void SingleInstanceCoordinator::setDeliveryReady()
{
    bool drain = false;
    {
        std::lock_guard<std::mutex> lock(pendingState_->mutex);
        pendingState_->deliveryReady = true;
        if (!pendingState_->requests.empty()
            && !pendingState_->deliveryScheduled) {
            pendingState_->deliveryScheduled = true;
            drain = true;
        }
    }
    if (drain)
        drainPendingRequests();
}

void SingleInstanceCoordinator::drainPendingRequests()
{
    while (true) {
        QStringList paths;
        {
            std::lock_guard<std::mutex> lock(pendingState_->mutex);
            if (!pendingState_->deliveryReady
                || pendingState_->requests.empty()) {
                pendingState_->deliveryScheduled = false;
                return;
            }
            paths = std::move(pendingState_->requests.front());
            pendingState_->requests.pop_front();
            pendingState_->pathCount -= paths.size();
            for (const QString &path : paths)
                pendingState_->pathBytes -= path.toUtf8().size();
        }
        emit requestReceived(paths);
    }
}

SingleInstanceCoordinator::StartResult
SingleInstanceCoordinator::start(const QStringList &paths)
{
    if (!QDir().mkpath(dataRoot_) && !QFileInfo(dataRoot_).isDir()) {
        return failedStart(2,
                           tr("无法访问 Notepad Safe 的用户数据目录；为避免损坏会话，本次没有启动第二个窗口。"));
    }

    ownerLock_ = std::make_unique<QLockFile>(lockFilePath_);
    ownerLock_->setStaleLockTime(0);
    if (ownerLock_->tryLock(0)) {
        if (startIoThread()) {
            return {StartResult::Disposition::Primary, 0, {}};
        }
        ownerLock_.reset();
        return failedStart(2,
                           tr("无法启动 Notepad Safe 的本地窗口连接；为避免损坏会话，本次没有启动。"));
    }

    if (ownerLock_->error() == QLockFile::PermissionError
        || ownerLock_->error() == QLockFile::UnknownError) {
        ownerLock_.reset();
        return failedStart(2,
                           tr("无法确认 Notepad Safe 的会话所有权；为避免损坏会话，本次没有启动第二个窗口。"));
    }
    // LockFailedError means a live owner holds the only session-writer right.
    // This process may only forward to that owner; QLocalServer::listen() is
    // deliberately never attempted on this path because Windows permits
    // multiple server instances with the same named-pipe name.
    return forwardToPrimary(paths);
}

SingleInstanceCoordinator::StartResult
SingleInstanceCoordinator::forwardToPrimary(const QStringList &paths)
{
    QString encodeError;
    const QByteArray request = encodeRequest(paths, &encodeError);
    if (request.isEmpty()) {
        return failedStart(2, encodeError);
    }

    QElapsedTimer timer;
    timer.start();
    while (remainingHandshakeMs(timer) > 0) {
        QLocalSocket socket;
        socket.connectToServer(endpointName_, QIODevice::ReadWrite);
        int remaining = remainingHandshakeMs(timer);
        if (remaining <= 0)
            break;
        const bool connected = socket.waitForConnected(qMin(250, remaining));
        if (!connected) {
            // The lock owner can die before its forwarding endpoint becomes
            // ready. Recheck ownership only by winning the same QLockFile;
            // never infer ownership from named-pipe listen success.
            if (ownerLock_ && ownerLock_->tryLock(0)) {
                if (startIoThread()) {
                    return {StartResult::Disposition::Primary, 0, {}};
                }
                ownerLock_.reset();
                return failedStart(2,
                                   tr("无法启动 Notepad Safe 的本地窗口连接；为避免损坏会话，本次没有启动。"));
            }
            if (ownerLock_
                && (ownerLock_->error() == QLockFile::PermissionError
                    || ownerLock_->error() == QLockFile::UnknownError)) {
                ownerLock_.reset();
                return failedStart(2,
                                   tr("无法确认 Notepad Safe 的会话所有权；为避免损坏会话，本次没有启动第二个窗口。"));
            }
            remaining = remainingHandshakeMs(timer);
            if (remaining <= 0)
                break;
            QThread::msleep(static_cast<unsigned long>(qMin(50, remaining)));
            continue;
        }
        if (remainingHandshakeMs(timer) <= 0)
            break;
        if (socket.write(request) != request.size()) {
            return failedStart(4,
                               tr("现有 Notepad Safe 窗口没有响应。请稍后重试；本次没有打开第二个会话。"));
        }
        socket.flush();
        remaining = remainingHandshakeMs(timer);
        if (remaining <= 0)
            break;
        if (socket.bytesToWrite() > 0
            && !socket.waitForBytesWritten(qMin(500, remaining))
            && socket.bytesToWrite() > 0) {
            return failedStart(4,
                               tr("现有 Notepad Safe 窗口没有响应。请稍后重试；本次没有打开第二个会话。"));
        }
        QByteArray payload;
        remaining = remainingHandshakeMs(timer);
        if (remaining <= 0)
            break;
        if (!readOneFrame(&socket, &payload, remaining)) {
            return failedStart(5,
                               tr("现有 Notepad Safe 窗口没有响应。请稍后重试；本次没有打开第二个会话。"));
        }
        bool accepted = false;
        QString responseFailure;
        if (!decodeResponse(payload, &accepted, &responseFailure)) {
            return rejectedStart(responseFailure);
        }
        if (accepted) {
            return {StartResult::Disposition::Forwarded, 0, {}};
        }
        return rejectedStart(responseFailure);
    }
    return failedStart(6,
                       tr("无法连接到现有 Notepad Safe 窗口。请稍后重试；本次没有打开第二个会话。"));
}
