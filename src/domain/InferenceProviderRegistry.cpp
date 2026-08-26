#include "InferenceProviderRegistry.h"

bool InferenceProviderRegistry::registerProvider(InferenceProviderManifest manifest,
                                                 Factory factory)
{
    if (manifest.id.isEmpty() || manifest.version.isEmpty() || !factory
        || entries_.contains(manifest.id)) {
        return false;
    }
    const QString id = manifest.id;
    entries_.insert(id, Entry{std::move(manifest), std::move(factory)});
    return true;
}

QList<InferenceProviderManifest> InferenceProviderRegistry::manifests() const
{
    QList<InferenceProviderManifest> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_)
        result.push_back(entry.manifest);
    return result;
}

std::unique_ptr<IInferenceProvider> InferenceProviderRegistry::create(
    const QString &id) const
{
    const auto it = entries_.constFind(id);
    if (it == entries_.cend())
        return {};
    ++instantiatedCount_;
    return it->factory();
}

