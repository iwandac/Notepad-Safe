#pragma once

#include <QByteArray>
#include <QString>

class QLineEdit;

// Best-effort lifetime minimization for credentials and decrypted byte buffers.
void scrubByteArray(QByteArray &bytes);
QString takeSensitiveLineEdit(QLineEdit *edit);
void clearSensitiveLineEdit(QLineEdit *edit);
