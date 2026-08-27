#pragma once

#include "PersistenceAdapter.h"

#include <QList>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class PersistenceAdapterRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IPersistenceAdapter>()>;

    bool registerAdapter(PersistenceAdapterDescriptor descriptor,
                         Factory factory);
    QList<PersistenceAdapterDescriptor> descriptors() const;
    std::optional<PersistenceAdapterDescriptor> resolve(const QString &path);
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry {
        PersistenceAdapterDescriptor descriptor;
        Factory factory;
        std::unique_ptr<IPersistenceAdapter> instance;
    };
    std::vector<Entry> entries_;
    int instantiatedCount_ = 0;
};
