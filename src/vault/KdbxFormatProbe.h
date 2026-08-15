#pragma once

#include <QtGlobal>
#include <QString>

struct KdbxHeaderInfo {
    bool recognized = false;
    quint16 majorVersion = 0;
    quint16 minorVersion = 0;
    QString error;
};

class KdbxFormatProbe final {
public:
    static KdbxHeaderInfo inspectFile(const QString &filePath);
};
