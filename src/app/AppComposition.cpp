#include "AppComposition.h"

namespace {

class BuiltinDocumentProvider final : public IDocumentProvider {
public:
    explicit BuiltinDocumentProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor))
    {
    }

    DocumentProviderDescriptor descriptor() const override { return descriptor_; }

private:
    DocumentProviderDescriptor descriptor_;
};

} // namespace

AppComposition::AppComposition()
{
    const QList<DocumentProviderDescriptor> descriptors = {
        {QStringLiteral("text.editable"), QStringLiteral("Text"),
         DocumentContentKind::Text, false},
        {QStringLiteral("text.windowed"), QStringLiteral("Large text"),
         DocumentContentKind::Text, true},
        {QStringLiteral("binary.hex"), QStringLiteral("Hex"),
         DocumentContentKind::Binary, false},
        {QStringLiteral("binary.windowed"), QStringLiteral("Large hex"),
         DocumentContentKind::Binary, true},
    };
    for (const auto &descriptor : descriptors) {
        documentProviders_.registerProvider(descriptor, [descriptor]() {
            return std::make_unique<BuiltinDocumentProvider>(descriptor);
        });
    }
}

SecurityServiceBundle AppComposition::createSecurityServices()
{
    heavyFactoryCalls_.fetch_add(1, std::memory_order_relaxed);
    return createSecurityServiceBundle();
}
