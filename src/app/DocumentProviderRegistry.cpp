#include "DocumentProviderRegistry.h"

bool DocumentProviderRegistry::registerProvider(DocumentProviderDescriptor descriptor,
                                                Factory factory)
{
    if (descriptor.id.isEmpty() || !factory || entries_.contains(descriptor.id))
        return false;
    const QString id = descriptor.id;
    entries_.insert(id, Entry{std::move(descriptor), std::move(factory)});
    return true;
}

QList<DocumentProviderDescriptor> DocumentProviderRegistry::descriptors() const
{
    QList<DocumentProviderDescriptor> result;
    result.reserve(entries_.size());
    for (const Entry &entry : entries_)
        result.push_back(entry.descriptor);
    return result;
}

std::unique_ptr<IDocumentProvider> DocumentProviderRegistry::create(const QString &id) const
{
    const auto it = entries_.constFind(id);
    if (it == entries_.cend())
        return {};
    ++instantiatedCount_;
    return it->factory();
}

bool DocumentProviderRegistry::openBest(
    IDocumentSurface *surface, const DocumentSurfaceOpenRequest &request,
    QString *error) const
{
    const Entry *selected = nullptr;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        const DocumentProviderDescriptor &candidate = it->descriptor;
        if (candidate.contentKind != request.contentKind
            || candidate.windowed != request.windowed
            || candidate.presentation != request.presentation) {
            continue;
        }
        if (!selected || candidate.priority > selected->descriptor.priority
            || (candidate.priority == selected->descriptor.priority
                && candidate.id < selected->descriptor.id)) {
            selected = &(*it);
        }
    }
    if (!selected) {
        if (error) {
            *error = QStringLiteral("没有适用于当前文档请求的提供器。");
        }
        return false;
    }
    std::unique_ptr<IDocumentProvider> provider = create(selected->descriptor.id);
    if (!provider) {
        if (error)
            *error = QStringLiteral("无法创建文档提供器：%1")
                         .arg(selected->descriptor.id);
        return false;
    }
    return provider->open(surface, request, error);
}
