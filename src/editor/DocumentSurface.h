#pragma once

#include "DocumentState.h"
#include "TextEncoding.h"

#include <QString>
#include <optional>

enum class DocumentPresentationKind { Text, Hex };

struct DocumentSurfaceOpenRequest {
    QString path;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    bool windowed = false;
    DocumentPresentationKind presentation = DocumentPresentationKind::Text;
    std::optional<TextEncoding> forcedEncoding;
};

// The application layer opens a document through this narrow port.  It must
// not know about EditorTab, QScintilla, mmap widgets, or other concrete UI
// implementation details.
class IDocumentSurface {
public:
    virtual ~IDocumentSurface() = default;
    virtual bool openSurface(const DocumentSurfaceOpenRequest &request,
                             QString *error) = 0;
};
