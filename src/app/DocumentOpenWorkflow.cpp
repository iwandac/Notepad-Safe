#include "DocumentOpenWorkflow.h"

#include "DocumentProviderRegistry.h"
#include "FileContentProbe.h"
#include "FileTierPolicy.h"
#include "SessionManager.h"
#include "VirtualFileView.h"

#include <QDebug>

#include <climits>

bool openDocumentByPolicy(const DocumentProviderRegistry &providers,
                          IDocumentSurface *surface, const QString &path,
                          std::optional<TextEncoding> forcedEncoding)
{
    if (!surface)
        return false;
    const FileContentProbeResult probe = probeFileContent(path);
    if (!forcedEncoding && probe.contentKind == DocumentContentKind::Binary) {
        QString error;
        const DocumentSurfaceOpenRequest request{
            path, DocumentContentKind::Binary, true,
            DocumentPresentationKind::Hex, forcedEncoding};
        if (!providers.openBest(surface, request, &error)) {
            qWarning("[openDocumentByPolicy] binary mmap open failed: path='%s', error='%s'",
                     qUtf8Printable(path), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    if (probe.tier.usesMappedPreview()) {
        QString error;
        const DocumentSurfaceOpenRequest request{
            path, DocumentContentKind::Text, true,
            DocumentPresentationKind::Text, forcedEncoding};
        if (providers.openBest(surface, request, &error)) {
            return true;
        }
        qWarning("[openDocumentByPolicy] mmap open failed without synchronous fallback: path='%s', error='%s'",
                 qUtf8Printable(path), qUtf8Printable(error));
        return false;
    }
    QString error;
    const DocumentSurfaceOpenRequest request{
        path, DocumentContentKind::Text, false,
        DocumentPresentationKind::Text, forcedEncoding};
    return providers.openBest(surface, request, &error);
}

bool openSessionDocument(const DocumentProviderRegistry &providers,
                         IDocumentSurface *surface, const SessionTab &state)
{
    if (!surface || state.filePath.isEmpty())
        return false;
    if (state.presentation == QLatin1String("hex")) {
        QString error;
        const DocumentSurfaceOpenRequest request{
            state.filePath, DocumentContentKind::Binary, true,
            DocumentPresentationKind::Hex, std::nullopt};
        if (!providers.openBest(surface, request, &error)) {
            qWarning("[session] explicit Hex restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    if (state.presentation == QLatin1String("text")
        && probeFileContent(state.filePath).contentKind
               == DocumentContentKind::Binary) {
        QString error;
        // Explicit text view for a binary file is a distinct current-product
        // route; it intentionally uses the text window provider.
        const DocumentSurfaceOpenRequest request{
            state.filePath, DocumentContentKind::Binary, true,
            DocumentPresentationKind::Text, std::nullopt};
        if (!providers.openBest(surface, request, &error)) {
            qWarning("[session] explicit Text restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    return openDocumentByPolicy(providers, surface, state.filePath);
}
