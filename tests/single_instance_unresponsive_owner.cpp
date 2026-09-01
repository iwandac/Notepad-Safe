#include "AppDataLocation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

namespace {
QString endpointForRoot(const QString &root)
{
    QString identity = QFileInfo(root).canonicalFilePath();
    if (identity.isEmpty())
        identity = QFileInfo(root).absoluteFilePath();
#ifdef Q_OS_WIN
    identity = QDir::toNativeSeparators(identity).toCaseFolded();
#endif
    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return QStringLiteral("NotepadSafe.SingleInstance.%1")
        .arg(QString::fromLatin1(digest));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 2 || app.arguments().size() > 3)
        return 64;
    const QString root = QDir::cleanPath(
        QFileInfo(app.arguments().at(1)).absoluteFilePath());
    if (!QDir().mkpath(root))
        return 65;

    QLockFile lock(QDir(root).filePath(
        QStringLiteral("notepad-safe.session.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 66;

    if (app.arguments().size() == 3
        && app.arguments().at(2) == QLatin1String("lock-only")) {
        return app.exec();
    }

    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(endpointForRoot(root)))
        return 67;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&]() {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            QObject::connect(socket, &QLocalSocket::readyRead, socket,
                             [socket]() { socket->readAll(); });
        }
    });
    return app.exec();
}
