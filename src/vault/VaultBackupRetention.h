#pragma once

#include "IVaultBackend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

// Shared naming and retention for encrypted user files. Callers must copy and
// verify the new backup BEFORE pruning, and prune BEFORE committing the original.
namespace VaultBackupRetention {

inline VaultError prepare(const QString &filePath, QString *backupPath)
{
    const QString directoryPath = filePath + QStringLiteral(".backups");
    const QFileInfo directoryInfo(directoryPath);
    if (directoryInfo.isSymbolicLink() || directoryInfo.isJunction()
        || !QDir().mkpath(directoryPath)) {
        return {VaultErrorCode::IoError,
                QStringLiteral("无法安全创建加密备份目录，原文件未被替换。"),
                QStringLiteral("vault.backup.directory")};
    }
    *backupPath = QDir(directoryPath).filePath(
        QFileInfo(filePath).completeBaseName() + QStringLiteral(".previous.kdbx"));
    const QFileInfo backupInfo(*backupPath);
    if (backupInfo.isSymbolicLink() || backupInfo.isJunction()
        || (backupInfo.exists() && !backupInfo.isFile())) {
        return {VaultErrorCode::IoError,
                QStringLiteral("加密备份路径不是普通文件，原文件未被替换。"),
                QStringLiteral("vault.backup.path")};
    }
    return VaultError::success();
}

inline VaultError prune(const QString &filePath)
{
    const QString directoryPath = filePath + QStringLiteral(".backups");
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymbolicLink() || directoryInfo.isJunction()) {
        return {VaultErrorCode::IoError,
                QStringLiteral("无法安全访问加密备份目录，原文件未被替换。"),
                QStringLiteral("vault.backup.directory")};
    }
    // Only the exact old naming scheme is managed. Never recurse or follow
    // links. Windows callers may spell the same source path in different case.
    QRegularExpression legacyName(
        QStringLiteral("\\A%1\\.([0-9]{8}-[0-9]{6}-[0-9]{3})\\.kdbx\\z")
            .arg(QRegularExpression::escape(QFileInfo(filePath).completeBaseName())));
#ifdef Q_OS_WIN
    legacyName.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
#endif
    const auto entries = QDir(directoryPath).entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymbolicLink() || entry.isJunction())
            continue;
        const auto match = legacyName.match(entry.fileName());
        if (!match.hasMatch()
            || !QDateTime::fromString(match.captured(1),
                                     QStringLiteral("yyyyMMdd-HHmmss-zzz")).isValid()) {
            continue;
        }
        QFile legacy(entry.absoluteFilePath());
        if (!legacy.remove()) {
            return {VaultErrorCode::IoError,
                    QStringLiteral("无法清理旧加密备份，原文件未被替换；请解除文件占用后重试。"),
                    QStringLiteral("vault.backup.prune: ") + legacy.errorString()};
        }
    }
    return VaultError::success();
}

} // namespace VaultBackupRetention
