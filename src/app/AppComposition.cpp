#include "AppComposition.h"
#include "DocumentContentPort.h"
#include "FileTierPolicy.h"
#include "KdbxFormatProbe.h"
#include "PlainFilePersistence.h"
#include "SecureDocumentPersistence.h"
#include "SecurityServiceFactory.h"

#include <utility>

namespace {

class BuiltinDocumentProvider final : public IDocumentProvider {
public:
    BuiltinDocumentProvider(DocumentProviderDescriptor descriptor,
                            const PersistenceAdapterRegistry &persistence)
        : descriptor_(std::move(descriptor)), persistence_(persistence)
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
        auto configured = request;
        // Resolve on the UI thread at each open, including routes registered
        // after this provider was created. Only this immutable value escapes.
        configured.readPolicy = persistence_.readPolicySnapshot();
        return surface->openSurface(configured, error);
    }

private:
    DocumentProviderDescriptor descriptor_;
    const PersistenceAdapterRegistry &persistence_;
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
        {QStringLiteral("text.hex-windowed"), QStringLiteral("Large text as hex"),
         DocumentContentKind::Text, true, DocumentPresentationKind::Hex, 100},
    };
    for (const auto &descriptor : descriptors) {
        documentProviders_.registerProvider(descriptor, [this, descriptor]() {
            return std::make_unique<BuiltinDocumentProvider>(descriptor, persistenceAdapters_);
        });
    }
    persistenceAdapters_.registerAdapter(
        PlainFilePersistence().descriptor(), [] {
            return std::make_unique<PlainFilePersistence>();
        });
    persistenceAdapters_.registerRoute(
        {QStringLiteral("secure.kdbx"), QStringLiteral("KDBX"),
         100, DocumentPersistence::SecureFile},
        [](const QString &path) {
            return path.endsWith(QLatin1String(".nsafe"), Qt::CaseInsensitive)
                || path.endsWith(QLatin1String(".kdbx"), Qt::CaseInsensitive)
                || KdbxFormatProbe::inspectFile(path).recognized;
        }, {12, [](const QString &path, QByteArrayView prefix) {
            return path.endsWith(QLatin1String(".nsafe"), Qt::CaseInsensitive)
                || path.endsWith(QLatin1String(".kdbx"), Qt::CaseInsensitive)
                || KdbxFormatProbe::inspectHeader(prefix).recognized;
        }});
}

AppComposition::SecurityRuntime AppComposition::createSecurityRuntime()
{
    heavyFactoryCalls_.fetch_add(1, std::memory_order_relaxed);
    SecurityServiceBundle services = createSecurityServiceBundle();
    auto persistence = std::make_unique<SecureDocumentPersistence>(
        std::move(services.documents));
    ISecureDocumentPersistence *documents = persistence.get();
    const QString id = persistence->descriptor().id;
    if (!persistenceAdapters_.registerAdapter(std::move(persistence))) {
        documents = dynamic_cast<ISecureDocumentPersistence *>(
            persistenceAdapters_.adapter(id));
    }
    return {std::move(services.session), documents};
}
