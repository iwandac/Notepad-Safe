#pragma once

#include <QByteArrayView>
#include <QString>
#include <QVector>

// A registered format's pure, bounded recognition rule. A function pointer
// deliberately cannot capture a registry, QObject, adapter factory or session.
struct PersistenceReadProbe {
    qsizetype prefixBytes = 0;
    bool (*recognizes)(const QString &path, QByteArrayView prefix) = nullptr;
};

class PersistenceAdapterRegistry;

// Immutable after the registry constructs it; safe to copy into a worker.
// It only denies ordinary reading of registered secure formats. It cannot
// select/instantiate a backend or authorize an interactive unlock.
class PersistenceReadPolicy final {
public:
    static constexpr qsizetype MaximumPrefixBytes = 4096;

    bool isAvailable() const noexcept { return available_; }
    qsizetype requiredPrefixBytes() const noexcept { return prefixBytes_; }
    QString secureAdapterFor(const QString &path, QByteArrayView prefix) const
    {
        if (!available_) return {};
        for (const Rule &rule : rules_) {
            if (rule.probe.recognizes(path, prefix.first(qMin(prefix.size(), rule.probe.prefixBytes))))
                return rule.adapterId;
        }
        return {};
    }

private:
    friend class PersistenceAdapterRegistry;
    struct Rule {
        QString adapterId;
        PersistenceReadProbe probe;
    };
    QVector<Rule> rules_;
    qsizetype prefixBytes_ = 0;
    bool available_ = false;
};
