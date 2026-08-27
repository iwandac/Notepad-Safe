#include "SensitiveText.h"

#include <QLineEdit>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

void scrubByteArray(QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
#ifdef Q_OS_WIN
    ::SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
#else
    volatile char *cursor = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        cursor[i] = 0;
#endif
    bytes.clear();
}

QString takeSensitiveLineEdit(QLineEdit *edit)
{
    if (!edit)
        return {};
    QString sensitive = edit->text();
    if (!sensitive.isEmpty()) {
        edit->setText(QString(sensitive.size(), QChar(0)));
        edit->clear();
    }
    return sensitive;
}

void clearSensitiveLineEdit(QLineEdit *edit)
{
    QString sensitive = takeSensitiveLineEdit(edit);
    if (!sensitive.isEmpty())
        sensitive.fill(QChar(0));
    sensitive.clear();
}
