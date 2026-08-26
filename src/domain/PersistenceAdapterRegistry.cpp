#include "PersistenceAdapterRegistry.h"

#include <algorithm>

bool PersistenceAdapterRegistry::registerAdapter(
    PersistenceAdapterDescriptor descriptor, Factory factory)
{
    const bool duplicate = std::any_of(
        entries_.cbegin(), entries_.cend(), [&descriptor](const Entry &entry) {
            return entry.descriptor.id == descriptor.id;
        });
    if (descriptor.id.trimmed().isEmpty() || !factory || duplicate) {
        return false;
    }
    entries_.push_back(Entry{std::move(descriptor), std::move(factory), {}});
    return true;
}

QList<PersistenceAdapterDescriptor> PersistenceAdapterRegistry::descriptors() const
{
    QList<PersistenceAdapterDescriptor> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_)
        result.push_back(entry.descriptor);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        if (left.priority != right.priority)
            return left.priority > right.priority;
        return left.id < right.id;
    });
    return result;
}

QString PersistenceAdapterRegistry::resolve(const QString &path)
{
    for (const auto &descriptor : descriptors()) {
        const auto found = std::find_if(
            entries_.begin(), entries_.end(), [&descriptor](const Entry &entry) {
                return entry.descriptor.id == descriptor.id;
            });
        if (found == entries_.end())
            continue;
        Entry &entry = *found;
        if (!entry.instance) {
            entry.instance = entry.factory();
            ++instantiatedCount_;
        }
        if (entry.instance && entry.instance->recognizes(path))
            return descriptor.id;
    }
    return {};
}
