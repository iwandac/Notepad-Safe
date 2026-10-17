#pragma once

#include "DocumentState.h"

#include <QString>
#include <optional>

class QWidget;

struct SaveAsSelection {
    QString path;
    DocumentPersistence persistence = DocumentPersistence::PlainFile;
};

// Owns the Save As presentation and the authoritative format choice.  The
// caller receives a typed decision and performs persistence separately.
// Without allowSecure the dialog offers only the plain file type.
class SaveAsDialog final {
public:
    static std::optional<SaveAsSelection> choose(
        QWidget *parent, const QString &suggestion,
        DocumentPersistence currentPersistence, bool binaryDocument,
        bool allowSecure);
};
