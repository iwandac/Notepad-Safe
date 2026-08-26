#include "DocumentToolRegistry.h"

bool DocumentToolRegistry::registerTool(DocumentToolDescriptor descriptor,
                                        Factory factory)
{
    if (descriptor.id.isEmpty() || !factory || entries_.contains(descriptor.id))
        return false;
    const QString id = descriptor.id;
    entries_.insert(id, Entry{std::move(descriptor), std::move(factory)});
    return true;
}

QList<DocumentToolDescriptor> DocumentToolRegistry::descriptors() const
{
    QList<DocumentToolDescriptor> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_)
        result.push_back(entry.descriptor);
    return result;
}

std::unique_ptr<IDocumentTool> DocumentToolRegistry::create(const QString &id) const
{
    const auto it = entries_.constFind(id);
    if (it == entries_.cend())
        return {};
    ++instantiatedCount_;
    return it->factory();
}

