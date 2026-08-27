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
    using Route = std::function<bool(const QString &)>;

    bool registerAdapter(PersistenceAdapterDescriptor descriptor,
                         Factory factory);
    bool registerRoute(PersistenceAdapterDescriptor descriptor, Route route);
    bool registerAdapter(std::unique_ptr<IPersistenceAdapter> adapter);
    QList<PersistenceAdapterDescriptor> descriptors() const;
    std::optional<PersistenceAdapterDescriptor> resolve(const QString &path);
    PersistenceResult saveBest(IDocumentContentPort *content,
                               const PersistenceSaveRequest &request);
    PersistenceResult saveFor(DocumentPersistence persistence,
                              IDocumentContentPort *content,
                              const PersistenceSaveRequest &request);
    PersistenceResult save(const QString &id, IDocumentContentPort *content,
                           const PersistenceSaveRequest &request);
    IPersistenceAdapter *adapter(const QString &id);
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry {
        PersistenceAdapterDescriptor descriptor;
        Route route;
        Factory factory;
        std::unique_ptr<IPersistenceAdapter> instance;
    };
    std::vector<Entry> entries_;
    int instantiatedCount_ = 0;
};
