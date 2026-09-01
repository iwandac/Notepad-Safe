#include "DocumentOpenWorkflow.h"

#include "DocumentProviderRegistry.h"
#include "FileContentProbe.h"
#include "FileTierPolicy.h"
#include "SessionManager.h"

#include <QDebug>

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
            DocumentPresentationKind::Hex, forcedEncoding, {},
            probe.tier.isReadOnlyByPolicy()};
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
            DocumentPresentationKind::Text, forcedEncoding, {},
            probe.tier.isReadOnlyByPolicy()};
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
    const std::optional<TextEncoding> restoredEncoding =
        encodingFromName(state.encoding);
    const bool restoreHex = state.presentation == QLatin1String("hex");
    // Freeze current classification even for a restored Hex presentation;
    // a later path replacement may add restrictions, never remove them.
    const FileContentProbeResult probe = probeFileContent(state.filePath);
    if (restoreHex || probe.contentKind == DocumentContentKind::Binary) {
        QString error;
        // A stored display encoding must not bypass Binary classification in
        // openDocumentByPolicy. Keep its byte-backed provider for either view;
        // the surface still derives read-only capability from current size.
        const DocumentPresentationKind presentation =
            state.presentation == QLatin1String("text")
                ? DocumentPresentationKind::Text
                : DocumentPresentationKind::Hex;
        const DocumentSurfaceOpenRequest request{
            state.filePath, DocumentContentKind::Binary, true,
            presentation, restoredEncoding, {}, probe.tier.isReadOnlyByPolicy()};
        if (!providers.openBest(surface, request, &error)) {
            qWarning("[session] Binary restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    return openDocumentByPolicy(providers, surface, state.filePath,
                                restoredEncoding);
}
