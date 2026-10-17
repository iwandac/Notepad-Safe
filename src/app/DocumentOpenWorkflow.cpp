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
    // Content decides the view: Binary is always Hex. Only a windowed
    // read-only Text document may be restored in its Hex presentation.
    const FileContentProbeResult probe = probeFileContent(state.filePath);
    const bool binary = probe.contentKind == DocumentContentKind::Binary;
    const bool windowedTextAsHex = !binary && probe.tier.usesMappedPreview()
        && state.presentation == QLatin1String("hex");
    if (binary || windowedTextAsHex) {
        QString error;
        // A stored display encoding must not bypass Binary classification in
        // openDocumentByPolicy; the surface still derives read-only
        // capability from current size.
        const DocumentSurfaceOpenRequest request{
            state.filePath,
            binary ? DocumentContentKind::Binary : DocumentContentKind::Text, true,
            DocumentPresentationKind::Hex, restoredEncoding, {},
            probe.tier.isReadOnlyByPolicy()};
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
