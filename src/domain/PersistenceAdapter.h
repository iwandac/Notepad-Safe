#pragma once

#include <QString>

struct PersistenceAdapterDescriptor {
    QString id;
    QString displayName;
    int priority = 0;
    bool encryptedAtRest = false;
};

class IPersistenceAdapter {
public:
    virtual ~IPersistenceAdapter() = default;
    virtual PersistenceAdapterDescriptor descriptor() const = 0;
    virtual bool recognizes(const QString &path) const = 0;
};
