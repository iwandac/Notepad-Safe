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
    entries_.push_back(Entry{std::move(descriptor), {}, std::move(factory), {}, {}});
    return true;
}

bool PersistenceAdapterRegistry::registerRoute(
    PersistenceAdapterDescriptor descriptor, Route route, PersistenceReadProbe readProbe)
{
    const bool duplicate = std::any_of(
        entries_.cbegin(), entries_.cend(), [&descriptor](const Entry &entry) {
            return entry.descriptor.id == descriptor.id;
        });
    if (descriptor.id.trimmed().isEmpty() || !route || duplicate)
        return false;
    entries_.push_back(Entry{std::move(descriptor), std::move(route), {}, {}, readProbe});
    return true;
}

bool PersistenceAdapterRegistry::registerAdapter(
    std::unique_ptr<IPersistenceAdapter> adapter)
{
    if (!adapter)
        return false;
    const PersistenceAdapterDescriptor descriptor = adapter->descriptor();
    if (descriptor.id.trimmed().isEmpty())
        return false;
    const auto existing = std::find_if(
        entries_.begin(), entries_.end(), [&descriptor](const Entry &entry) {
            return entry.descriptor.id == descriptor.id;
        });
    if (existing != entries_.end()) {
        if (existing->instance || existing->factory)
            return false;
        existing->instance = std::move(adapter);
        ++instantiatedCount_;
        return true;
    }
    entries_.push_back(Entry{descriptor, {}, {}, std::move(adapter), {}});
    ++instantiatedCount_;
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

PersistenceReadPolicy PersistenceAdapterRegistry::readPolicySnapshot() const
{
    PersistenceReadPolicy policy;
    for (const Entry &entry : entries_) {
        if (entry.descriptor.persistence != DocumentPersistence::SecureFile)
            continue;
        const PersistenceReadProbe probe = entry.readProbe;
        // Missing or unbounded recognition must not silently become permission
        // to read a format through an ordinary provider. Never load a factory.
        if (!probe.recognizes || probe.prefixBytes <= 0
            || probe.prefixBytes > PersistenceReadPolicy::MaximumPrefixBytes)
            return {};
        policy.rules_.append({entry.descriptor.id, probe});
        policy.prefixBytes_ = qMax(policy.prefixBytes_, probe.prefixBytes);
    }
    policy.available_ = !policy.rules_.isEmpty();
    return policy;
}

std::optional<PersistenceAdapterDescriptor>
PersistenceAdapterRegistry::resolve(const QString &path)
{
    for (const auto &descriptor : descriptors()) {
        const auto found = std::find_if(
            entries_.begin(), entries_.end(), [&descriptor](const Entry &entry) {
                return entry.descriptor.id == descriptor.id;
            });
        if (found == entries_.end())
            continue;
        Entry &entry = *found;
        if (entry.route && entry.route(path))
            return descriptor;
        if (!entry.instance) {
            if (!entry.factory)
                continue;
            entry.instance = entry.factory();
            ++instantiatedCount_;
        }
        if (entry.instance && entry.instance->recognizes(path))
            return descriptor;
    }
    return std::nullopt;
}

PersistenceResult PersistenceAdapterRegistry::saveBest(
    IDocumentContentPort *content, const PersistenceSaveRequest &request)
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
            if (!entry.factory)
                continue;
            entry.instance = entry.factory();
            if (entry.instance)
                ++instantiatedCount_;
        }
        if (entry.instance && entry.instance->recognizes(request.targetPath))
            return entry.instance->save(content, request);
    }
    return {PersistenceStatus::Unsupported,
            QStringLiteral("没有可保存该目标格式的持久化适配器。"), {}};
}

PersistenceResult PersistenceAdapterRegistry::saveFor(
    DocumentPersistence persistence, IDocumentContentPort *content,
    const PersistenceSaveRequest &request)
{
    for (const auto &descriptor : descriptors()) {
        if (descriptor.persistence != persistence)
            continue;
        const auto found = std::find_if(
            entries_.begin(), entries_.end(), [&descriptor](const Entry &entry) {
                return entry.descriptor.id == descriptor.id;
            });
        if (found == entries_.end())
            continue;
        Entry &entry = *found;
        if (!entry.instance) {
            if (!entry.factory)
                continue;
            entry.instance = entry.factory();
            if (entry.instance)
                ++instantiatedCount_;
        }
        if (entry.instance && entry.instance->recognizes(request.targetPath))
            return entry.instance->save(content, request);
    }
    return {PersistenceStatus::Unsupported,
            QStringLiteral("当前构建没有可保存所选持久化类型的适配器。"), {}};
}

PersistenceResult PersistenceAdapterRegistry::save(
    const QString &id, IDocumentContentPort *content,
    const PersistenceSaveRequest &request)
{
    const auto found = std::find_if(
        entries_.begin(), entries_.end(), [&id](const Entry &entry) {
            return entry.descriptor.id == id;
        });
    if (found == entries_.end()) {
        return {PersistenceStatus::Unsupported,
                QStringLiteral("未注册持久化适配器：%1").arg(id), {}};
    }
    Entry &entry = *found;
    if (!entry.instance) {
        if (entry.factory) {
            entry.instance = entry.factory();
            if (entry.instance)
                ++instantiatedCount_;
        }
    }
    if (!entry.instance) {
        return {PersistenceStatus::Error,
                QStringLiteral("无法创建持久化适配器：%1").arg(id), {}};
    }
    return entry.instance->save(content, request);
}

IPersistenceAdapter *PersistenceAdapterRegistry::adapter(const QString &id)
{
    const auto found = std::find_if(
        entries_.begin(), entries_.end(), [&id](const Entry &entry) {
            return entry.descriptor.id == id;
        });
    if (found == entries_.end())
        return nullptr;
    if (!found->instance && found->factory) {
        found->instance = found->factory();
        ++instantiatedCount_;
    }
    return found->instance.get();
}
