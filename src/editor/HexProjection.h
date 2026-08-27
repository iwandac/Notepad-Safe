#pragma once

#include <QByteArray>
#include <QString>

QString formatHexBytes(const QByteArray &bytes, int bytesPerRow = 16);
int hexNibble(QChar ch);
bool parseHexBytes(const QString &text, QByteArray *bytes, QString *error);
