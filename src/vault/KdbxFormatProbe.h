#pragma once

#include <QtGlobal>
#include <QByteArrayView>
#include <QString>

struct KdbxHeaderInfo {
    bool recognized = false;
    quint16 majorVersion = 0;
    quint16 minorVersion = 0;
    QString error;
};

class KdbxFormatProbe final {
public:
    static KdbxHeaderInfo inspectHeader(QByteArrayView header);
    static KdbxHeaderInfo inspectFile(const QString &filePath);
};
