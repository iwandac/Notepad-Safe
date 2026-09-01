#include "AppDataLocation.h"
#include "DedicatedThread.h"
#include "SingleInstanceCoordinator.h"

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLocalServer>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace {
QByteArray frame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(payload.size());
    result.append(payload);
    return result;
}

QJsonObject responseFrom(QLocalSocket *socket)
{
    QByteArray bytes;
    quint32 size = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3500) {
        bytes.append(socket->readAll());
        if (bytes.size() >= 4 && size == 0) {
            QDataStream stream(bytes.left(4));
            stream.setByteOrder(QDataStream::BigEndian);
            stream >> size;
        }
        if (size > 0 && bytes.size() == int(size + 4))
            return QJsonDocument::fromJson(bytes.mid(4)).object();
        if (size > 0 && bytes.size() > int(size + 4))
            return {};
        if (socket->state() == QLocalSocket::UnconnectedState) {
            bytes.append(socket->readAll());
            if (size > 0 && bytes.size() == int(size + 4))
                return QJsonDocument::fromJson(bytes.mid(4)).object();
            return {};
        }
        QTest::qWait(10);
    }
    return {};
}

QJsonObject blockingTransact(const QString &endpoint,
                             const QByteArray &request)
{
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(100))
        return {};
    if (socket.write(request) != request.size())
        return {};
    socket.flush();
    if (socket.bytesToWrite() > 0
        && !socket.waitForBytesWritten(100)) {
        return {};
    }
    QByteArray bytes;
    quint32 size = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500) {
        bytes.append(socket.readAll());
        if (bytes.size() >= 4 && size == 0) {
            QDataStream stream(bytes.left(4));
            stream.setByteOrder(QDataStream::BigEndian);
            stream >> size;
        }
        if (size > 0 && bytes.size() == int(size + 4))
            return QJsonDocument::fromJson(bytes.mid(4)).object();
        if (socket.state() == QLocalSocket::UnconnectedState)
            return {};
        socket.waitForReadyRead(20);
    }
    return {};
}

QJsonObject openRequest(int version = 1)
{
    return {{QStringLiteral("version"), version},
            {QStringLiteral("action"), QStringLiteral("open_files")},
            {QStringLiteral("paths"),
             QJsonArray{QDir::cleanPath(QDir::tempPath()
                                        + QStringLiteral("/ipc-contract.txt"))}}};
}

QJsonObject requestWithPaths(const QStringList &paths)
{
    QJsonArray array;
    for (const QString &path : paths)
        array.append(path);
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("action"), QStringLiteral("open_files")},
            {QStringLiteral("paths"), array}};
}

QJsonObject activationRequest()
{
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("action"), QStringLiteral("activate")},
            {QStringLiteral("paths"), QJsonArray{}}};
}
} // namespace

class SingleInstanceContractTest final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        profile_ = std::make_unique<QTemporaryDir>();
        QVERIFY(profile_->isValid());
        qputenv("NOTEPADSAFE_DATADIR", profile_->path().toUtf8());
        QCOMPARE(resolveNotepadSafeDataRoot(),
                 QDir::cleanPath(QFileInfo(profile_->path()).absoluteFilePath()));
        primary_ = std::make_unique<SingleInstanceCoordinator>();
        const auto result = primary_->start({});
        QCOMPARE(result.disposition,
                 SingleInstanceCoordinator::StartResult::Disposition::Primary);
    }

    void cleanup()
    {
        primary_.reset();
        profile_.reset();
        qunsetenv("NOTEPADSAFE_DATADIR");
        QCoreApplication::processEvents();
    }

    void preDeliveryRequestIsQueuedBeforeAckAndDrainedInOrder()
    {
        QSignalSpy requests(primary_.get(),
                           &SingleInstanceCoordinator::requestReceived);
        const QString first = QDir::cleanPath(
            QDir::tempPath() + QStringLiteral("/queue-first.txt"));
        const QString second = QDir::cleanPath(
            QDir::tempPath() + QStringLiteral("/queue-second.txt"));
        QCOMPARE(transact(frame(requestWithPaths({first})))
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QCOMPARE(transact(frame(requestWithPaths({second})))
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QCOMPARE(requests.size(), 0);
        primary_->setDeliveryReady();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 2000);
        QCOMPARE(requests.at(0).at(0).toStringList(), QStringList{first});
        QCOMPARE(requests.at(1).at(0).toStringList(), QStringList{second});
    }

    void queuedRequestCountCapacityRejectsAtomically()
    {
        for (int i = 0; i < 32; ++i) {
            const QJsonObject response = transact(frame(activationRequest()));
            QCOMPARE(response.value(QStringLiteral("status")).toString(),
                     QStringLiteral("accepted"));
        }
        const QJsonObject overflow = transact(frame(activationRequest()));
        QCOMPARE(overflow.value(QStringLiteral("status")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(overflow.value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_queue_full"));

        QSignalSpy requests(primary_.get(),
                           &SingleInstanceCoordinator::requestReceived);
        primary_->setDeliveryReady();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 32, 2000);
    }

    void queuedPathCountCapacityRejectsAtomically()
    {
        const QString base = QDir::cleanPath(QDir::tempPath());
        for (int requestIndex = 0; requestIndex < 8; ++requestIndex) {
            QStringList paths;
            for (int pathIndex = 0; pathIndex < 32; ++pathIndex) {
                paths.append(base + QStringLiteral("/queue-%1-%2.txt")
                                      .arg(requestIndex).arg(pathIndex));
            }
            QCOMPARE(transact(frame(requestWithPaths(paths)))
                         .value(QStringLiteral("status")).toString(),
                     QStringLiteral("accepted"));
        }
        const QJsonObject overflow = transact(frame(requestWithPaths(
            {base + QStringLiteral("/queue-overflow.txt")})));
        QCOMPARE(overflow.value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_queue_full"));
    }

    void queuedUtf8CapacityRejectsAtomically()
    {
        const QString base = QDir::cleanPath(QDir::tempPath()) + '/';
        const QString longPart(30000, QLatin1Char('a'));
        for (int requestIndex = 0; requestIndex < 8; ++requestIndex) {
            const QString suffix = QString::number(requestIndex);
            const QStringList paths{
                base + longPart + QStringLiteral("-a-") + suffix,
                base + longPart + QStringLiteral("-b-") + suffix};
            QCOMPARE(transact(frame(requestWithPaths(paths)))
                         .value(QStringLiteral("status")).toString(),
                     QStringLiteral("accepted"));
        }
        const QStringList overflowPaths{
            base + longPart + QStringLiteral("-overflow-a"),
            base + longPart + QStringLiteral("-overflow-b")};
        const QJsonObject overflow =
            transact(frame(requestWithPaths(overflowPaths)));
        QCOMPARE(overflow.value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_queue_full"));
    }

    void failedAckRollsBackQueueWithoutDelivery()
    {
        QSignalSpy requests(primary_.get(),
                           &SingleInstanceCoordinator::requestReceived);
        const QString path = QDir::cleanPath(
            QDir::tempPath() + QStringLiteral("/ack-failed.txt"));
        QCOMPARE(primary_->enqueueRequest({path}, []() { return false; }),
                 SingleInstanceCoordinator::EnqueueResult::AckFailed);
        primary_->setDeliveryReady();
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);

        QCOMPARE(transact(frame(requestWithPaths({path})))
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 1000);
    }

    void incompatibleVersionIsRejectedOnStableEndpoint()
    {
        QSignalSpy requests(primary_.get(),
                           &SingleInstanceCoordinator::requestReceived);
        const QJsonObject response = transact(frame(openRequest(2)));
        QCOMPARE(response.value(QStringLiteral("status")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(response.value(QStringLiteral("code")).toString(),
                 QStringLiteral("incompatible_version"));
        QCOMPARE(requests.size(), 0);
        QVERIFY(!primary_->endpointName().contains(QStringLiteral(".v1.")));
    }

    void undefinedContentFieldIsRejected()
    {
        QJsonObject request = openRequest();
        request.insert(QStringLiteral("body"), QStringLiteral("secret"));
        const QJsonObject response = transact(frame(request));
        QCOMPARE(response.value(QStringLiteral("code")).toString(),
                 QStringLiteral("undefined_field"));
    }

    void oversizeHeaderIsRejected()
    {
        QByteArray request;
        QDataStream stream(&request, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::BigEndian);
        stream << quint32(64 * 1024 + 1);
        const QJsonObject response = transact(request);
        QCOMPARE(response.value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_too_large"));
    }

    void trailingDataIsRejected()
    {
        QByteArray request = frame(openRequest());
        request.append('x');
        const QJsonObject response = transact(request);
        QCOMPARE(response.value(QStringLiteral("code")).toString(),
                 QStringLiteral("trailing_data"));
    }

    void partialAndIdleConnectionsExpire()
    {
        QLocalSocket partial;
        partial.connectToServer(primary_->endpointName());
        QVERIFY(partial.waitForConnected(1000));
        const QByteArray request = frame(openRequest());
        partial.write(request.left(2));
        partial.flush();
        QTRY_VERIFY_WITH_TIMEOUT(partial.bytesAvailable() > 0, 3500);
        QCOMPARE(responseFrom(&partial).value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_timeout"));

        QLocalSocket idle;
        idle.connectToServer(primary_->endpointName());
        QVERIFY(idle.waitForConnected(1000));
        QTRY_VERIFY_WITH_TIMEOUT(idle.bytesAvailable() > 0, 3500);
        QCOMPARE(responseFrom(&idle).value(QStringLiteral("code")).toString(),
                 QStringLiteral("request_timeout"));
    }

    void excessLiveConnectionIsRejected()
    {
        std::vector<std::unique_ptr<QLocalSocket>> holders;
        for (int i = 0; i < 8; ++i) {
            auto socket = std::make_unique<QLocalSocket>();
            socket->connectToServer(primary_->endpointName());
            QVERIFY(socket->waitForConnected(1000));
            holders.push_back(std::move(socket));
            QCoreApplication::processEvents();
        }

        QLocalSocket excess;
        excess.connectToServer(primary_->endpointName());
        QVERIFY(excess.waitForConnected(1000));
        const QJsonObject response = responseFrom(&excess);
        QCOMPARE(response.value(QStringLiteral("status")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(response.value(QStringLiteral("code")).toString(),
                 QStringLiteral("server_busy"));
        for (const auto &socket : holders)
            socket->abort();
    }

    void eightSlowConnectionsExpireTogetherAndSlotsRecover()
    {
        std::vector<std::unique_ptr<QLocalSocket>> slow;
        const QByteArray partial = frame(openRequest()).left(2);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 8; ++i) {
            auto socket = std::make_unique<QLocalSocket>();
            socket->connectToServer(primary_->endpointName());
            QVERIFY(socket->waitForConnected(1000));
            QCOMPARE(socket->write(partial), partial.size());
            socket->flush();
            slow.push_back(std::move(socket));
        }
        for (const auto &socket : slow) {
            QCOMPARE(responseFrom(socket.get())
                         .value(QStringLiteral("code")).toString(),
                     QStringLiteral("request_timeout"));
        }
        QVERIFY2(elapsed.elapsed() < 4500,
                 "slow connections were handled serially instead of by deadline");

        QCOMPARE(transact(frame(openRequest()))
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
    }

    void shutdownUnderEightIdleConnectionsIsBounded()
    {
        std::vector<std::unique_ptr<QLocalSocket>> idle;
        for (int i = 0; i < 8; ++i) {
            auto socket = std::make_unique<QLocalSocket>();
            socket->connectToServer(primary_->endpointName());
            QVERIFY(socket->waitForConnected(1000));
            idle.push_back(std::move(socket));
        }
        QElapsedTimer elapsed;
        elapsed.start();
        primary_.reset();
        QVERIFY2(elapsed.elapsed() < 1000,
                 "coordinator shutdown did not join the I/O thread promptly");
        for (const auto &socket : idle) {
            QTRY_COMPARE_WITH_TIMEOUT(socket->state(),
                                      QLocalSocket::UnconnectedState, 1000);
        }
    }

    void shutdownAdmissionGateIsRaceSafeAndLockOutlivesJoin()
    {
        QSignalSpy delivered(primary_.get(),
                             &SingleInstanceCoordinator::requestReceived);
        const QString endpoint = primary_->endpointName();
        const QString lockPath = primary_->lockFilePath();
        const QByteArray request = frame(activationRequest());
        std::atomic_bool begin{false};
        std::atomic_bool stopReturned{false};
        std::atomic_int acceptedBeforeOrDuringStop{0};
        std::atomic_int attemptsAfterStop{0};
        std::atomic_int acceptedAfterStop{0};
        DedicatedThread requester;
        QVERIFY(requester.start([&]() {
            while (!begin.load(std::memory_order_acquire))
                QThread::yieldCurrentThread();
            while (!stopReturned.load(std::memory_order_acquire)) {
                if (blockingTransact(endpoint, request)
                        .value(QStringLiteral("status")).toString()
                    == QLatin1String("accepted")) {
                    ++acceptedBeforeOrDuringStop;
                }
            }
            for (int i = 0; i < 8; ++i) {
                ++attemptsAfterStop;
                if (blockingTransact(endpoint, request)
                        .value(QStringLiteral("status")).toString()
                    == QLatin1String("accepted")) {
                    ++acceptedAfterStop;
                }
            }
        }));
        begin.store(true, std::memory_order_release);
        QTest::qWait(20);
        primary_->stopAcceptingAndDrain();
        stopReturned.store(true, std::memory_order_release);
        requester.join();
        QCOMPARE(delivered.size(), acceptedBeforeOrDuringStop.load());
        QCOMPARE(attemptsAfterStop.load(), 8);
        QCOMPARE(acceptedAfterStop.load(), 0);

        QLockFile overlappingWriter(lockPath);
        overlappingWriter.setStaleLockTime(0);
        QVERIFY(!overlappingWriter.tryLock(0));
        QCOMPARE(overlappingWriter.error(), QLockFile::LockFailedError);

        primary_.reset();
        QLockFile successor(lockPath);
        successor.setStaleLockTime(0);
        QVERIFY(successor.tryLock(1000));
    }

    void delayedSecondRequestFrameDispatchesAtMostOnce()
    {
        QSignalSpy requests(primary_.get(),
                           &SingleInstanceCoordinator::requestReceived);
        primary_->setDeliveryReady();
        QLocalSocket socket;
        socket.connectToServer(primary_->endpointName());
        QVERIFY(socket.waitForConnected(1000));
        const QByteArray request = frame(openRequest());
        QCOMPARE(socket.write(request), request.size());
        socket.flush();
        QCOMPARE(responseFrom(&socket).value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 1000);
        QTest::qWait(100);
        socket.write(request);
        socket.flush();
        QTest::qWait(100);
        QCOMPARE(requests.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(),
                                  QLocalSocket::UnconnectedState, 1000);
    }

    void strictResponseSchemaFailsClosed_data()
    {
        QTest::addColumn<QByteArray>("response");
        QJsonObject accepted{{QStringLiteral("version"), 1},
                             {QStringLiteral("status"),
                              QStringLiteral("accepted")}};
        QJsonObject extra = accepted;
        extra.insert(QStringLiteral("extra"), true);
        QTest::newRow("undefined-field") << frame(extra);
        QJsonObject acceptedWithCode = accepted;
        acceptedWithCode.insert(QStringLiteral("code"),
                                QStringLiteral("server_busy"));
        QTest::newRow("accepted-with-code") << frame(acceptedWithCode);
        QTest::newRow("missing-status") << frame(
            {{QStringLiteral("version"), 1}});
        QTest::newRow("wrong-status-type") << frame(
            {{QStringLiteral("version"), 1},
             {QStringLiteral("status"), 1}});
        QTest::newRow("unknown-rejection-code") << frame(
            {{QStringLiteral("version"), 1},
             {QStringLiteral("status"), QStringLiteral("rejected")},
             {QStringLiteral("code"), QStringLiteral("invented")}});
        QByteArray twoFrames = frame(accepted);
        twoFrames.append(frame(accepted));
        QTest::newRow("second-frame") << twoFrames;
    }

    void strictResponseSchemaFailsClosed()
    {
        QFETCH(QByteArray, response);
        primary_.reset();
        SingleInstanceCoordinator identity;
        QLockFile owner(identity.lockFilePath());
        owner.setStaleLockTime(0);
        QVERIFY(owner.tryLock(0));

        std::mutex startupMutex;
        std::condition_variable startupChanged;
        bool startupReady = false;
        bool listening = false;
        DedicatedThread fakeOwner;
        QVERIFY(fakeOwner.start([&]() {
            QLocalServer server;
            server.setSocketOptions(QLocalServer::UserAccessOption);
            listening = server.listen(identity.endpointName());
            {
                std::lock_guard<std::mutex> lock(startupMutex);
                startupReady = true;
            }
            startupChanged.notify_one();
            if (!listening || !server.waitForNewConnection(1500))
                return;
            std::unique_ptr<QLocalSocket> socket(
                server.nextPendingConnection());
            if (!socket)
                return;
            socket->waitForReadyRead(1000);
            socket->readAll();
            socket->write(response);
            socket->flush();
            if (socket->bytesToWrite() > 0)
                socket->waitForBytesWritten(500);
            socket->disconnectFromServer();
            if (socket->state() != QLocalSocket::UnconnectedState)
                socket->waitForDisconnected(500);
        }));
        {
            std::unique_lock<std::mutex> lock(startupMutex);
            QVERIFY(startupChanged.wait_for(lock, std::chrono::seconds(2),
                                            [&]() { return startupReady; }));
        }
        QVERIFY(listening);
        SingleInstanceCoordinator secondary;
        const auto result = secondary.start({});
        QCOMPARE(result.disposition,
                 SingleInstanceCoordinator::StartResult::Disposition::Failed);
        QVERIFY(result.exitCode != 0);
        fakeOwner.join();
    }

    void rejectionCodesMapToActionableMessages_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("messagePart");
        QTest::newRow("incompatible")
            << QStringLiteral("incompatible_version") << 3
            << QStringLiteral("不兼容");
        QTest::newRow("busy")
            << QStringLiteral("server_busy") << 4
            << QStringLiteral("较多打开请求");
        QTest::newRow("queue-full")
            << QStringLiteral("request_queue_full") << 4
            << QStringLiteral("队列已满");
        QTest::newRow("timeout")
            << QStringLiteral("request_timeout") << 5
            << QStringLiteral("限定时间");
        QTest::newRow("invalid")
            << QStringLiteral("invalid_request") << 3
            << QStringLiteral("无效");
    }

    void rejectionCodesMapToActionableMessages()
    {
        QFETCH(QString, code);
        QFETCH(int, exitCode);
        QFETCH(QString, messagePart);
        const QByteArray response = frame(
            {{QStringLiteral("version"), 1},
             {QStringLiteral("status"), QStringLiteral("rejected")},
             {QStringLiteral("code"), code}});
        SingleInstanceCoordinator::StartResult result;
        QVERIFY(runAgainstFakeOwner(response, false, &result));
        QCOMPARE(result.disposition,
                 SingleInstanceCoordinator::StartResult::Disposition::Failed);
        QCOMPARE(result.exitCode, exitCode);
        QVERIFY2(result.userMessage.contains(messagePart),
                 qPrintable(result.userMessage));
    }

    void forwardHandshakeHonorsFiveSecondTotalBudget()
    {
        SingleInstanceCoordinator::StartResult result;
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(runAgainstFakeOwner({}, true, &result));
        const qint64 elapsedMs = elapsed.elapsed();
        QCOMPARE(result.disposition,
                 SingleInstanceCoordinator::StartResult::Disposition::Failed);
        QCOMPARE(result.exitCode, 5);
        QVERIFY2(elapsedMs >= 4500,
                 "unresponsive handshake returned before its budget");
        QVERIFY2(elapsedMs <= 5250,
                 qPrintable(QStringLiteral("handshake exceeded total budget: %1ms")
                                .arg(elapsedMs)));
    }

    void endpointIdentityIsStableWhenProfileDirectoryAppears()
    {
        const QString originalProfile = profile_->path();
        const QString futureProfile = QDir(originalProfile).filePath(
            QStringLiteral("future-profile"));
        QVERIFY(!QFileInfo::exists(futureProfile));
        qputenv("NOTEPADSAFE_DATADIR", futureProfile.toUtf8());
        SingleInstanceCoordinator beforeCreation;
        QVERIFY(QDir().mkpath(futureProfile));
        SingleInstanceCoordinator afterCreation;
        QCOMPARE(beforeCreation.endpointName(), afterCreation.endpointName());
        qputenv("NOTEPADSAFE_DATADIR", originalProfile.toUtf8());
    }

    void primaryLockExcludesAnotherWriter()
    {
        QVERIFY(QFileInfo::exists(primary_->lockFilePath()));
        QLockFile contender(primary_->lockFilePath());
        contender.setStaleLockTime(0);
        QVERIFY(!contender.tryLock(0));
        QCOMPARE(contender.error(), QLockFile::LockFailedError);
    }

    void unavailableDataRootFailsClosed()
    {
        const QString originalProfile = profile_->path();
        const QString regularFile = QDir(originalProfile).filePath(
            QStringLiteral("not-a-directory"));
        QFile file(regularFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        qputenv("NOTEPADSAFE_DATADIR", regularFile.toUtf8());
        SingleInstanceCoordinator unavailable;
        const auto result = unavailable.start({});
        QCOMPARE(result.disposition,
                 SingleInstanceCoordinator::StartResult::Disposition::Failed);
        QVERIFY(!result.userMessage.isEmpty());
        qputenv("NOTEPADSAFE_DATADIR", originalProfile.toUtf8());
    }

private:
    bool runAgainstFakeOwner(
        const QByteArray &response, bool holdWithoutResponse,
        SingleInstanceCoordinator::StartResult *result)
    {
        primary_.reset();
        SingleInstanceCoordinator identity;
        QLockFile owner(identity.lockFilePath());
        owner.setStaleLockTime(0);
        if (!owner.tryLock(0))
            return false;

        std::mutex startupMutex;
        std::condition_variable startupChanged;
        bool startupReady = false;
        bool listening = false;
        DedicatedThread fakeOwner;
        if (!fakeOwner.start([&]() {
                QLocalServer server;
                server.setSocketOptions(QLocalServer::UserAccessOption);
                listening = server.listen(identity.endpointName());
                {
                    std::lock_guard<std::mutex> lock(startupMutex);
                    startupReady = true;
                }
                startupChanged.notify_one();
                if (!listening || !server.waitForNewConnection(1500))
                    return;
                std::unique_ptr<QLocalSocket> socket(
                    server.nextPendingConnection());
                if (!socket)
                    return;
                socket->waitForReadyRead(1000);
                socket->readAll();
                if (holdWithoutResponse) {
                    socket->waitForDisconnected(6000);
                    return;
                }
                socket->write(response);
                socket->flush();
                if (socket->bytesToWrite() > 0)
                    socket->waitForBytesWritten(500);
                socket->disconnectFromServer();
                if (socket->state() != QLocalSocket::UnconnectedState)
                    socket->waitForDisconnected(500);
            })) {
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(startupMutex);
            if (!startupChanged.wait_for(lock, std::chrono::seconds(2),
                                         [&]() { return startupReady; })) {
                fakeOwner.join();
                return false;
            }
        }
        if (!listening) {
            fakeOwner.join();
            return false;
        }
        SingleInstanceCoordinator secondary;
        *result = secondary.start({});
        fakeOwner.join();
        return true;
    }

    QJsonObject transact(const QByteArray &request)
    {
        QLocalSocket socket;
        socket.connectToServer(primary_->endpointName());
        if (!socket.waitForConnected(1000))
            return {};
        socket.write(request);
        socket.flush();
        QElapsedTimer timer;
        timer.start();
        while (socket.bytesAvailable() == 0 && timer.elapsed() < 3500)
            QTest::qWait(10);
        return responseFrom(&socket);
    }

    std::unique_ptr<QTemporaryDir> profile_;
    std::unique_ptr<SingleInstanceCoordinator> primary_;
};

QTEST_GUILESS_MAIN(SingleInstanceContractTest)
#include "test_single_instance.moc"
