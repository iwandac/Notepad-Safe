#pragma once

#include "SecurityServiceFactory.h"
#include "DocumentProviderRegistry.h"
#include "DocumentToolRegistry.h"
#include "InferenceProviderRegistry.h"
#include "PersistenceAdapterRegistry.h"

#include <atomic>

// The single composition root owns concrete-adapter factories.  Descriptors
// and registries are cheap values; creating an adapter is explicit and counted
// so startup tests can prove that first paint did not instantiate heavy work.
class AppComposition final {
public:
    AppComposition();

    SecurityServiceBundle createSecurityServices();
    void recordHeavyFactoryCall() noexcept
    {
        heavyFactoryCalls_.fetch_add(1, std::memory_order_relaxed);
    }

    int heavyFactoryCalls() const noexcept
    {
        return heavyFactoryCalls_.load(std::memory_order_relaxed);
    }

    DocumentToolRegistry &documentTools() noexcept { return documentTools_; }
    const DocumentToolRegistry &documentTools() const noexcept { return documentTools_; }
    DocumentProviderRegistry &documentProviders() noexcept { return documentProviders_; }
    const DocumentProviderRegistry &documentProviders() const noexcept
    {
        return documentProviders_;
    }
    InferenceProviderRegistry &inferenceProviders() noexcept
    {
        return inferenceProviders_;
    }
    const InferenceProviderRegistry &inferenceProviders() const noexcept
    {
        return inferenceProviders_;
    }
    PersistenceAdapterRegistry &persistenceAdapters() noexcept
    {
        return persistenceAdapters_;
    }
    const PersistenceAdapterRegistry &persistenceAdapters() const noexcept
    {
        return persistenceAdapters_;
    }

private:
    std::atomic<int> heavyFactoryCalls_{0};
    DocumentToolRegistry documentTools_;
    DocumentProviderRegistry documentProviders_;
    InferenceProviderRegistry inferenceProviders_;
    PersistenceAdapterRegistry persistenceAdapters_;
};
