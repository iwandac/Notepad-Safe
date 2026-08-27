#include "FileCredentialDialog.h"

#include "SensitiveText.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

FileCredentialDialog::FileCredentialDialog(
    const QString &title, const QString &message, bool allowRecovery,
    QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("fileCredentialDialog"));
    setWindowTitle(title);
    setMinimumWidth(500);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(message, this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("fileCredentialKind"));
    kind_->addItem(QStringLiteral("文件密码"), static_cast<int>(Kind::Passphrase));
    if (allowRecovery)
        kind_->addItem(QStringLiteral("恢复密钥"), static_cast<int>(Kind::RecoveryKey));
    form->addRow(QStringLiteral("解锁方式"), kind_);

    fields_ = new QStackedWidget(this);
    passphrase_ = new QLineEdit(fields_);
    passphrase_->setObjectName(QStringLiteral("fileCredentialPassphrase"));
    passphrase_->setEchoMode(QLineEdit::Password);
    passphrase_->setClearButtonEnabled(true);
    fields_->addWidget(passphrase_);
    recoveryKey_ = new QLineEdit(fields_);
    recoveryKey_->setObjectName(QStringLiteral("fileCredentialRecoveryKey"));
    recoveryKey_->setEchoMode(QLineEdit::Password);
    recoveryKey_->setClearButtonEnabled(true);
    recoveryKey_->setPlaceholderText(QStringLiteral("NSR1-…"));
    fields_->addWidget(recoveryKey_);
    form->addRow(QStringLiteral("凭据"), fields_);
    layout->addLayout(form);

    auto *showInput = new QCheckBox(QStringLiteral("显示输入"), this);
    showInput->setObjectName(QStringLiteral("fileCredentialShowInput"));
    layout->addWidget(showInput);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("fileCredentialError"));
    error_->setStyleSheet(QStringLiteral("color:#b00020"));
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Open | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    buttons->setObjectName(QStringLiteral("fileCredentialButtons"));
    buttons->button(QDialogButtonBox::Open)->setText(QStringLiteral("打开"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(kind_, &QComboBox::currentIndexChanged, this, [this](int) {
        clearSecrets();
        error_->clear();
        fields_->setCurrentIndex(kind() == Kind::RecoveryKey ? 1 : 0);
        focusCredential();
    });
    connect(showInput, &QCheckBox::toggled, this, [this](bool shown) {
        const auto mode = shown ? QLineEdit::Normal : QLineEdit::Password;
        passphrase_->setEchoMode(mode);
        recoveryKey_->setEchoMode(mode);
    });
    passphrase_->setFocus();
}

FileCredentialDialog::~FileCredentialDialog()
{
    clearSecrets();
}

FileCredentialDialog::Kind FileCredentialDialog::kind() const
{
    return static_cast<Kind>(kind_->currentData().toInt());
}

SecureBuffer FileCredentialDialog::takePassphrase()
{
    QString text = takeSensitiveLineEdit(passphrase_);
    QByteArray utf8 = text.toUtf8();
    if (!text.isEmpty())
        text.fill(QChar(0));
    text.clear();
    SecureBuffer result(utf8.constData(), utf8.size());
    scrubByteArray(utf8);
    return result;
}

QString FileCredentialDialog::takeRecoveryKey()
{
    QString raw = takeSensitiveLineEdit(recoveryKey_);
    QString result = raw.trimmed();
    if (!raw.isEmpty())
        raw.fill(QChar(0));
    raw.clear();
    return result;
}

void FileCredentialDialog::showError(const QString &message)
{
    error_->setText(message);
    focusCredential();
}

void FileCredentialDialog::clearSecrets()
{
    clearSensitiveLineEdit(passphrase_);
    clearSensitiveLineEdit(recoveryKey_);
}

void FileCredentialDialog::focusCredential()
{
    if (kind() == Kind::RecoveryKey)
        recoveryKey_->setFocus();
    else
        passphrase_->setFocus();
}
