#pragma once

#include "DocumentState.h"

#include <QHash>
#include <functional>
#include <memory>

struct DocumentProviderDescriptor {
    QString id;
    QString displayName;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    bool windowed = false;
};

class IDocumentProvider {
public:
    virtual ~IDocumentProvider() = default;
    virtual DocumentProviderDescriptor descriptor() const = 0;
};

class DocumentProviderRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IDocumentProvider>()>;

    bool registerProvider(DocumentProviderDescriptor descriptor, Factory factory);
    QList<DocumentProviderDescriptor> descriptors() const;
    std::unique_ptr<IDocumentProvider> create(const QString &id) const;
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry { DocumentProviderDescriptor descriptor; Factory factory; };
    QHash<QString, Entry> entries_;
    mutable int instantiatedCount_ = 0;
};

