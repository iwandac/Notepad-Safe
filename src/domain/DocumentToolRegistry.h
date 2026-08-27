#pragma once

#include "DocumentTool.h"

#include <QHash>
#include <functional>

class DocumentToolRegistry final {
public:
    using Factory = std::function<std::unique_ptr<IDocumentTool>()>;

    bool registerTool(DocumentToolDescriptor descriptor, Factory factory);
    QList<DocumentToolDescriptor> descriptors() const;
    std::unique_ptr<IDocumentTool> create(const QString &id) const;
    DocumentToolResult run(const QString &id, const DocumentSnapshot &snapshot,
                           const DocumentToolContext &context = {}) const;
    int instantiatedCount() const noexcept { return instantiatedCount_; }

private:
    struct Entry { DocumentToolDescriptor descriptor; Factory factory; };
    QHash<QString, Entry> entries_;
    mutable int instantiatedCount_ = 0;
};
