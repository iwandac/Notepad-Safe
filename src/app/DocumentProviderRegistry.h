#pragma once

#include "DocumentState.h"
#include "TextEncoding.h"

#include <QHash>
#include <functional>
#include <memory>

class EditorTab;

enum class DocumentPresentationKind { Text, Hex };

struct DocumentProviderDescriptor {
    QString id;
    QString displayName;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    bool windowed = false;
    DocumentPresentationKind presentation = DocumentPresentationKind::Text;
};

class IDocumentProvider {
public:
    virtual ~IDocumentProvider() = default;
    virtual DocumentProviderDescriptor descriptor() const = 0;
    virtual bool open(EditorTab *tab, const QString &path,
                      std::optional<TextEncoding> forcedEncoding,
                      QString *error) = 0;
};

class DocumentProviderRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IDocumentProvider>()>;

    bool registerProvider(DocumentProviderDescriptor descriptor, Factory factory);
    QList<DocumentProviderDescriptor> descriptors() const;
    std::unique_ptr<IDocumentProvider> create(const QString &id) const;
    bool open(const QString &id, EditorTab *tab, const QString &path,
              std::optional<TextEncoding> forcedEncoding = std::nullopt,
              QString *error = nullptr) const;
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry { DocumentProviderDescriptor descriptor; Factory factory; };
    QHash<QString, Entry> entries_;
    mutable int instantiatedCount_ = 0;
};
