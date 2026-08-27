#include "AppComposition.h"
#include "KdbxFormatProbe.h"

#include <QFileInfo>

namespace {

class BuiltinDocumentProvider final : public IDocumentProvider {
public:
    explicit BuiltinDocumentProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor))
    {
    }

    DocumentProviderDescriptor descriptor() const override { return descriptor_; }

    bool open(IDocumentSurface *surface,
              const DocumentSurfaceOpenRequest &request,
              QString *error) override
    {
        if (!surface) {
            if (error) *error = QStringLiteral("文档视图不可用。");
            return false;
        }
        return surface->openSurface(request, error);
    }

private:
    DocumentProviderDescriptor descriptor_;
};

class PlainFilePersistenceAdapter final : public IPersistenceAdapter {
public:
    PersistenceAdapterDescriptor descriptor() const override
    {
        return {QStringLiteral("plain.file"), QStringLiteral("Plain file"),
                0, DocumentPersistence::PlainFile};
    }
    bool recognizes(const QString &path) const override
    {
        return QFileInfo(path).isFile();
    }
};

class KdbxPersistenceAdapter final : public IPersistenceAdapter {
public:
    PersistenceAdapterDescriptor descriptor() const override
    {
        return {QStringLiteral("secure.kdbx"), QStringLiteral("KDBX"),
                100, DocumentPersistence::SecureFile};
    }
    bool recognizes(const QString &path) const override
    {
        return KdbxFormatProbe::inspectFile(path).recognized;
    }
};

} // namespace

AppComposition::AppComposition()
{
    const QList<DocumentProviderDescriptor> descriptors = {
        {QStringLiteral("text.editable"), QStringLiteral("Text"),
         DocumentContentKind::Text, false, DocumentPresentationKind::Text, 100},
        {QStringLiteral("text.windowed"), QStringLiteral("Large text"),
         DocumentContentKind::Text, true, DocumentPresentationKind::Text, 100},
        {QStringLiteral("binary.windowed"), QStringLiteral("Large hex"),
         DocumentContentKind::Binary, true, DocumentPresentationKind::Hex, 100},
        {QStringLiteral("binary.text-windowed"), QStringLiteral("Binary text"),
         DocumentContentKind::Binary, true, DocumentPresentationKind::Text, 100},
    };
    for (const auto &descriptor : descriptors) {
        documentProviders_.registerProvider(descriptor, [descriptor]() {
            return std::make_unique<BuiltinDocumentProvider>(descriptor);
        });
    }
    persistenceAdapters_.registerAdapter(
        KdbxPersistenceAdapter().descriptor(), [] {
            return std::make_unique<KdbxPersistenceAdapter>();
        });
    persistenceAdapters_.registerAdapter(
        PlainFilePersistenceAdapter().descriptor(), [] {
            return std::make_unique<PlainFilePersistenceAdapter>();
        });
}

SecurityServiceBundle AppComposition::createSecurityServices()
{
    heavyFactoryCalls_.fetch_add(1, std::memory_order_relaxed);
    return createSecurityServiceBundle();
}
