#pragma once

#include "DocumentSurface.h"

#include <QHash>
#include <functional>
#include <memory>

struct DocumentProviderDescriptor {
    QString id;
    QString displayName;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    bool windowed = false;
    DocumentPresentationKind presentation = DocumentPresentationKind::Text;
    int priority = 0;
};

class IDocumentProvider {
public:
    virtual ~IDocumentProvider() = default;
    virtual DocumentProviderDescriptor descriptor() const = 0;
    virtual bool open(IDocumentSurface *surface,
                      const DocumentSurfaceOpenRequest &request,
                      QString *error) = 0;
};

class DocumentProviderRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IDocumentProvider>()>;

    bool registerProvider(DocumentProviderDescriptor descriptor, Factory factory);
    QList<DocumentProviderDescriptor> descriptors() const;
    std::unique_ptr<IDocumentProvider> create(const QString &id) const;
    bool openBest(IDocumentSurface *surface,
                  const DocumentSurfaceOpenRequest &request,
                  QString *error = nullptr) const;
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry { DocumentProviderDescriptor descriptor; Factory factory; };
    QHash<QString, Entry> entries_;
    mutable int instantiatedCount_ = 0;
};
