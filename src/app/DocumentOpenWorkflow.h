#pragma once

#include "TextEncoding.h"
#include "DocumentSurface.h"

#include <optional>

class DocumentProviderRegistry;
struct SessionTab;

bool openDocumentByPolicy(
    const DocumentProviderRegistry &providers, IDocumentSurface *surface,
    const QString &path,
    std::optional<TextEncoding> forcedEncoding = std::nullopt);
bool openSessionDocument(const DocumentProviderRegistry &providers,
                         IDocumentSurface *surface, const SessionTab &state);
