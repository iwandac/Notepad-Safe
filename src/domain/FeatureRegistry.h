#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct FeatureDescriptor {
    QString id;
    QString title;
    QString productContract;
};

class FeatureRegistry final {
public:
    static QList<FeatureDescriptor> features();
    // Frozen commands that must exist in the initial application shell.  This
    // is intentionally separate from discovery: deleting an QAction must make
    // the startup contract fail instead of silently shrinking the inventory.
    static QStringList requiredStartupCommandIds();
    static QString featureIdForCommand(const QString &commandId);
    static bool isKnownFeature(const QString &featureId);
};
