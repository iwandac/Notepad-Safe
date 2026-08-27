#pragma once

#include "TextEncoding.h"

#include <optional>

class EditorTab;
struct SessionTab;

bool openDocumentByPolicy(
    EditorTab *tab, const QString &path,
    std::optional<TextEncoding> forcedEncoding = std::nullopt);
bool openSessionDocument(EditorTab *tab, const SessionTab &state);
void restoreDocumentSessionView(EditorTab *tab, const SessionTab &state);
