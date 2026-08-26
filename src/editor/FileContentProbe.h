#pragma once

#include "FileTierPolicy.h"
#include "TextEncoding.h"
#include "DocumentState.h"

#include <QString>

struct FileContentProbeResult {
    bool exists = false;
    bool readable = false;
    qint64 sourceBytes = -1;
    DocumentContentKind contentKind = DocumentContentKind::Text;
    EncodingDetection encoding;
    FileTierDecision tier;
};

// Bounded file probe shared by every Open entry point and its contract tests.
// It never loads the complete document: encoding detection uses its fixed
// sampling budget and binary classification inspects at most the first 64 KiB.
FileContentProbeResult probeFileContent(const QString &path);
