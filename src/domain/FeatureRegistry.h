#pragma once

#include <QList>
#include <QString>

struct FeatureDescriptor {
    QString id;
    QString title;
    QString productContract;
};

class FeatureRegistry final {
public:
    static QList<FeatureDescriptor> features();
    static QString featureIdForCommand(const QString &commandId);
    static bool isKnownFeature(const QString &featureId);
};

