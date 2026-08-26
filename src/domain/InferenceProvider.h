#pragma once

#include "DocumentTool.h"

#include <QByteArray>
#include <QString>

struct InferenceProviderManifest {
    QString id;
    QString version;
    QString source;
    QString license;
    QByteArray sha256;
    QString modelFormat;
    QString hardwareRequirements;
    bool remote = false;
};

struct InferenceRequest {
    QString prompt;
    ToolCancellation *cancellation = nullptr;
};

struct InferenceResponse {
    bool success = false;
    bool cancelled = false;
    QString text;
    QString error;
};

class IInferenceProvider {
public:
    virtual ~IInferenceProvider() = default;
    virtual InferenceProviderManifest manifest() const = 0;
    virtual InferenceResponse infer(const InferenceRequest &request) = 0;
};

