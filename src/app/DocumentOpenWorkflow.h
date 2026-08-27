#pragma once

#include "TextEncoding.h"

#include <optional>

class EditorTab;
class DocumentProviderRegistry;
struct SessionTab;

bool openDocumentByPolicy(
    const DocumentProviderRegistry &providers, EditorTab *tab, const QString &path,
    std::optional<TextEncoding> forcedEncoding = std::nullopt);
bool openSessionDocument(const DocumentProviderRegistry &providers,
                         EditorTab *tab, const SessionTab &state);
void restoreDocumentSessionView(EditorTab *tab, const SessionTab &state);
