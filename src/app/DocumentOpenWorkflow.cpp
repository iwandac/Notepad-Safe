#include "DocumentOpenWorkflow.h"

#include "EditorTab.h"
#include "DocumentProviderRegistry.h"
#include "FileContentProbe.h"
#include "FileTierPolicy.h"
#include "SessionManager.h"
#include "VirtualFileView.h"

#include <QDebug>

#include <climits>

bool openDocumentByPolicy(const DocumentProviderRegistry &providers,
                          EditorTab *tab, const QString &path,
                          std::optional<TextEncoding> forcedEncoding)
{
    if (!tab)
        return false;
    const FileContentProbeResult probe = probeFileContent(path);
    if (!forcedEncoding && probe.contentKind == DocumentContentKind::Binary) {
        QString error;
        if (!providers.open(QStringLiteral("binary.windowed"), tab, path,
                            forcedEncoding, &error)) {
            qWarning("[openDocumentByPolicy] binary mmap open failed: path='%s', error='%s'",
                     qUtf8Printable(path), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    if (probe.tier.usesMappedPreview()) {
        QString error;
        if (providers.open(QStringLiteral("text.windowed"), tab, path,
                           forcedEncoding, &error)) {
            return true;
        }
        qWarning("[openDocumentByPolicy] mmap open failed without synchronous fallback: path='%s', error='%s'",
                 qUtf8Printable(path), qUtf8Printable(error));
        return false;
    }
    QString error;
    return providers.open(QStringLiteral("text.editable"), tab, path,
                          forcedEncoding, &error);
}

bool openSessionDocument(const DocumentProviderRegistry &providers,
                         EditorTab *tab, const SessionTab &state)
{
    if (!tab || state.filePath.isEmpty())
        return false;
    if (state.presentation == QLatin1String("hex")) {
        QString error;
        if (!providers.open(QStringLiteral("binary.windowed"), tab,
                            state.filePath, std::nullopt, &error)) {
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
        if (!providers.open(QStringLiteral("binary.text-windowed"), tab,
                            state.filePath, std::nullopt, &error)) {
            qWarning("[session] explicit Text restore failed: path='%s', error='%s'",
                     qUtf8Printable(state.filePath), qUtf8Printable(error));
            return false;
        }
        return true;
    }
    return openDocumentByPolicy(providers, tab, state.filePath);
}

void restoreDocumentSessionView(EditorTab *tab, const SessionTab &state)
{
    if (!tab)
        return;
    if (state.cursorByteOffset >= 0) {
        const qint64 selectionStart = qMin(state.selectionStartByte,
                                           state.selectionEndByte);
        const qint64 selectionEnd = qMax(state.selectionStartByte,
                                         state.selectionEndByte);
        if (selectionStart >= 0 && selectionEnd > selectionStart) {
            tab->gotoByteOffset(selectionStart, int(qMin<qint64>(
                selectionEnd - selectionStart, INT_MAX)));
        } else {
            tab->gotoByteOffset(state.cursorByteOffset);
        }
    } else {
        tab->setCursorPosition(state.cursorLine, state.cursorIndex);
    }
    if (state.splitView && tab->canSplitView()) {
        tab->setSplitView(
            true, state.splitOrientation == QLatin1String("vertical")
                      ? Qt::Vertical : Qt::Horizontal);
    }
}
