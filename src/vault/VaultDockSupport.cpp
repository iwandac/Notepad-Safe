#include "VaultDockSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>

std::unique_ptr<QSettings> createSecuritySettings()
{
    const QString dataDir = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("NOTEPADSAFE_DATADIR")).trimmed();
    if (!dataDir.isEmpty() && QFileInfo(dataDir).isAbsolute()) {
        QDir directory(dataDir);
        directory.mkpath(QStringLiteral("."));
        return std::make_unique<QSettings>(
            directory.filePath(QStringLiteral("settings.ini")),
            QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(QStringLiteral("NotepadSafe"),
                                       QStringLiteral("NotepadSafe"));
}
