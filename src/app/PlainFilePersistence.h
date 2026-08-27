#pragma once

#include "PersistenceAdapter.h"

class PlainFilePersistence final : public IPersistenceAdapter {
public:
    PersistenceAdapterDescriptor descriptor() const override;
    bool recognizes(const QString &path) const override;
    PersistenceResult save(IDocumentContentPort *content,
                           const PersistenceSaveRequest &request) override;
};
