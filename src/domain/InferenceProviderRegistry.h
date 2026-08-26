#pragma once

#include "InferenceProvider.h"

#include <QHash>
#include <functional>
#include <memory>

class InferenceProviderRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IInferenceProvider>()>;

    bool registerProvider(InferenceProviderManifest manifest, Factory factory);
    QList<InferenceProviderManifest> manifests() const;
    std::unique_ptr<IInferenceProvider> create(const QString &id) const;
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry { InferenceProviderManifest manifest; Factory factory; };
    QHash<QString, Entry> entries_;
    mutable int instantiatedCount_ = 0;
};

