#include "AppComposition.h"
#include "EditorTab.h"
#include "KdbxFormatProbe.h"
#include "VirtualFileView.h"

#include <QFileInfo>

namespace {

class BuiltinDocumentProvider final : public IDocumentProvider {
public:
    explicit BuiltinDocumentProvider(DocumentProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor))
    {
    }

    DocumentProviderDescriptor descriptor() const override { return descriptor_; }

    bool open(EditorTab *tab, const QString &path,
              std::optional<TextEncoding> forcedEncoding,
              QString *error) override
    {
        if (!tab) {
            if (error) *error = QStringLiteral("文档视图不可用。");
            return false;
        }
        if (!descriptor_.windowed)
            return tab->loadFile(path, forcedEncoding);
        const bool binary = descriptor_.contentKind == DocumentContentKind::Binary;
        const bool hex = descriptor_.presentation == DocumentPresentationKind::Hex;
        const L2Presentation presentation = hex
            ? L2Presentation::Hex : L2Presentation::Text;
        const L2ContentKind contentKind = binary
            ? L2ContentKind::Binary : L2ContentKind::Text;
        const bool opened = tab->openLargeFileL2(
            path, error, presentation, contentKind, forcedEncoding);
        if (opened && hex)
            tab->setViewModeL2(ViewMode::Hex);
        return opened;
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
         DocumentContentKind::Text, false},
        {QStringLiteral("text.windowed"), QStringLiteral("Large text"),
         DocumentContentKind::Text, true},
        {QStringLiteral("binary.windowed"), QStringLiteral("Large hex"),
         DocumentContentKind::Binary, true, DocumentPresentationKind::Hex},
        {QStringLiteral("binary.text-windowed"), QStringLiteral("Binary text"),
         DocumentContentKind::Binary, true, DocumentPresentationKind::Text},
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
