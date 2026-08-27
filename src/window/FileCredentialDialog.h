#pragma once

#include "SecureBuffer.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;

class FileCredentialDialog final : public QDialog {
public:
    enum class Kind { Passphrase, RecoveryKey };

    FileCredentialDialog(const QString &title, const QString &message,
                         bool allowRecovery, QWidget *parent);
    ~FileCredentialDialog() override;

    Kind kind() const;
    SecureBuffer takePassphrase();
    QString takeRecoveryKey();
    void showError(const QString &message);
    void clearSecrets();

private:
    void focusCredential();

    QComboBox *kind_ = nullptr;
    QStackedWidget *fields_ = nullptr;
    QLineEdit *passphrase_ = nullptr;
    QLineEdit *recoveryKey_ = nullptr;
    QLabel *error_ = nullptr;
};
