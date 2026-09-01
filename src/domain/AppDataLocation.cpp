#include "AppDataLocation.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

QString resolveNotepadSafeDataRoot()
{
    const QString configured = QProcessEnvironment::systemEnvironment()
                                   .value(QStringLiteral("NOTEPADSAFE_DATADIR"))
                                   .trimmed();
    if (!configured.isEmpty()) {
        const QFileInfo info(configured);
        if (info.isAbsolute())
            return QDir::cleanPath(info.absoluteFilePath());
        qWarning("[AppDataLocation] NOTEPADSAFE_DATADIR is not absolute; "
                 "using the default profile location");
    }
    return QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/NotepadSafe"));
}
