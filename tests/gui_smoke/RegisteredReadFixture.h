#pragma once

#include "AppComposition.h"
#include "EditorTab.h"
#include "FileTierPolicy.h"
#include <QFileInfo>

// Exercise the application's registered admission rules, not a test-only
// allow-all policy. The immutable snapshot outlives this cheap composition.
inline PersistenceReadPolicy registeredReadFixturePolicy()
{
    AppComposition composition;
    return composition.persistenceAdapters().readPolicySnapshot();
}

inline bool openWindowedFixture(
    EditorTab *tab, const QString &path, QString *error = nullptr,
    L2Presentation presentation = L2Presentation::Text,
    L2ContentKind kind = L2ContentKind::Text,
    std::optional<TextEncoding> encoding = {})
{
    DocumentSurfaceOpenRequest request;
    request.path = path;
    request.windowed = true;
    request.presentation = presentation == L2Presentation::Hex
        ? DocumentPresentationKind::Hex : DocumentPresentationKind::Text;
    request.contentKind = kind == L2ContentKind::Binary
        ? DocumentContentKind::Binary : DocumentContentKind::Text;
    request.forcedEncoding = encoding;
    request.readPolicy = registeredReadFixturePolicy();
    request.requireReadOnly = FileTierPolicy::classify(
        QFileInfo(path).size()).isReadOnlyByPolicy();
    return tab->openSurface(request, error);
}
