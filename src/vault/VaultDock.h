#pragma once

#include "VaultService.h"

#include <QDockWidget>

#include <memory>

struct SecurityIdentityParameters;

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class VaultDock final : public QDockWidget {
    Q_OBJECT
public:
    explicit VaultDock(QWidget *parent = nullptr);
    explicit VaultDock(std::unique_ptr<IVaultBackend> backend, QWidget *parent = nullptr);
    ~VaultDock() override;

    VaultState vaultState() const noexcept { return service_.state(); }
    bool isSecureStoreOpen() const noexcept { return service_.state() == VaultState::Open; }
    QString secureStorePath() const { return service_.filePath(); }
    VaultError createVault(const QString &filePath,
                           const SecureBuffer &masterPasswordUtf8,
                           const QString &keyFilePath = {});
    VaultError unlockVault(const QString &filePath,
                           const SecureBuffer &masterPasswordUtf8,
                           const QString &keyFilePath = {});
    VaultError createManagedStore(const QString &filePath,
                                  const SecureBuffer &securityPassphraseUtf8,
                                  QString *recoveryKey);
    VaultError unlockManagedStore(const QString &filePath,
                                  const SecureBuffer &securityPassphraseUtf8);
    VaultError createManagedStoreForIdentity(
        const QString &filePath,
        const SecurityIdentityParameters &parameters,
        const SecureBuffer &masterUnlockKey,
        const SecureBuffer &recoverySecret);
    VaultError unlockManagedStoreWithMasterKey(
        const QString &filePath, const SecureBuffer &masterUnlockKey);
    VaultError unlockManagedStoreWithRecoverySecret(
        const QString &filePath, const SecureBuffer &recoverySecret);
    VaultError recoverManagedStore(const QString &filePath,
                                   const QString &recoveryKey);
    QString preferredManagedStorePath() const;
    bool ensureSecureStoreReadyInteractive();
    std::unique_ptr<VaultEntryData> readSecureDocument(const QUuid &id,
                                                       VaultError *error = nullptr) const;
    VaultError saveSecureDocument(const QUuid &id, const QString &title,
                                  const QString &plainText);
    bool prepareToClose();
    // Leaving the Accounts workspace seals its KDBX backend without ending
    // the document security session or masking open encrypted documents.
    bool releaseWorkspaceSecrets();
    void discardAndLockForExternalReload();
    void focusAccountSearch();
    bool canSearchAccounts() const;
    bool saveActiveEntry();
    bool canSaveActiveEntry() const noexcept
    {
        return isSecureStoreOpen() && editorEditing_ && editorDirty_;
    }

public slots:
    void newVaultInteractive();
    void openVaultInteractive();
    void lockVault();
    bool prepareToLock();
    bool lockVaultSilently();
    void createLogin();
    void createSecureNote();

signals:
    void secureNoteOpenRequested(const QUuid &id);
    void lockRequested();
    void secureStoreLocked();
    void externalStoreReloadRequested(const QString &filePath);
    void workspaceCommandStateChanged();
    void userNotificationRequested(const QString &message, int timeoutMs);

private slots:
    void requestLock();
    void onCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void saveCurrentEntry();
    void editCurrentEntry();
    void cancelCurrentEdit();
    void removeCurrentEntry();
    void restoreCurrentEntry();
    void showEntryHistory();
    void applyFilter(const QString &text);
    void updatePasswordEcho(bool visible);
    void copyPasswordProtected();
    void generatePassword();
    void updatePasswordStrength();
    void copyUsername();
    void openCurrentUrl();
    void refreshTotpCode();
    void configureTotp();
    void copyTotpProtected();
    void clearProtectedClipboard();
    void cloneCurrentEntry();
    void showPasswordHealthReport();

private:
    enum class EntryKind { Login, SecureNote };

    void buildUi();
    void refreshEntries(const QUuid &selectId = {});
    void loadEntry(const QUuid &id);
    void beginNewEntry(EntryKind kind);
    void clearEditor();
    void setEditorEnabled(bool enabled);
    void setEditingState(bool editing, bool newEntry = false);
    void setVaultOpenControls(bool open);
    void setEntryKind(EntryKind kind);
    void setDirty(bool dirty = true);
    bool maybeSaveChanges();
    bool saveCurrentEntryImpl(bool showSuccess);
    void copyProtectedText(const QString &text);
    void hidePassword();
    void showError(const VaultError &error, const QString &title);
    bool handleExternalChangeError(const VaultError &error);
    void applyOpenState();
    void rememberManagedStore(const QString &filePath);
    static SecureBuffer secureUtf8From(const QString &text);
    static QString transientText(const SecureBuffer &value);
    static void wipeQString(QString &text);

    bool backendAvailable_ = false;
    VaultService service_;
    QStackedWidget *pages_ = nullptr;
    QWidget *lockedPage_ = nullptr;
    QWidget *openPage_ = nullptr;
    QLabel *lockedMessage_ = nullptr;
    QWidget *searchRowWidget_ = nullptr;
    QStackedWidget *contentPages_ = nullptr;
    QPushButton *emptyCreateButton_ = nullptr;
    QStackedWidget *listPages_ = nullptr;
    QLabel *listEmptyLabel_ = nullptr;
    QStackedWidget *detailPages_ = nullptr;
    QWidget *detailEditorPage_ = nullptr;
    QLineEdit *filterEdit_ = nullptr;
    QComboBox *sortCombo_ = nullptr;
    QTreeWidget *entryTree_ = nullptr;
    QTreeWidgetItem *loginRoot_ = nullptr;
    QTreeWidgetItem *noteRoot_ = nullptr;
    QTreeWidgetItem *recycleRoot_ = nullptr;
    QLabel *kindLabel_ = nullptr;
    QFormLayout *formLayout_ = nullptr;
    QLineEdit *titleEdit_ = nullptr;
    QLineEdit *usernameEdit_ = nullptr;
    QWidget *usernameRow_ = nullptr;
    QLineEdit *urlEdit_ = nullptr;
    QWidget *urlRow_ = nullptr;
    QLineEdit *tagsEdit_ = nullptr;
    QLineEdit *groupEdit_ = nullptr;
    QPlainTextEdit *additionalUrlsEdit_ = nullptr;
    QPlainTextEdit *customFieldsEdit_ = nullptr;
    QCheckBox *expiryCheck_ = nullptr;
    QDateTimeEdit *expiryEdit_ = nullptr;
    QWidget *expiryRow_ = nullptr;
    QLineEdit *passwordEdit_ = nullptr;
    QWidget *passwordRow_ = nullptr;
    QLineEdit *totpEdit_ = nullptr;
    QWidget *totpInputRow_ = nullptr;
    QPushButton *totpConfigureButton_ = nullptr;
    QWidget *totpCodeRow_ = nullptr;
    QLabel *totpCodeCaption_ = nullptr;
    QLabel *totpCodeLabel_ = nullptr;
    QTextEdit *notesEdit_ = nullptr;
    QLabel *usernameLabel_ = nullptr;
    QLabel *urlLabel_ = nullptr;
    QLabel *tagsLabel_ = nullptr;
    QLabel *passwordLabel_ = nullptr;
    QLabel *passwordStrengthCaption_ = nullptr;
    QLabel *passwordStrengthLabel_ = nullptr;
    QLabel *totpLabel_ = nullptr;
    QCheckBox *showPasswordCheck_ = nullptr;
    QPushButton *copyPasswordButton_ = nullptr;
    QPushButton *generatePasswordButton_ = nullptr;
    QPushButton *copyTotpButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *editButton_ = nullptr;
    QPushButton *historyButton_ = nullptr;
    QPushButton *cloneButton_ = nullptr;
    QPushButton *healthButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *copyUsernameButton_ = nullptr;
    QPushButton *openUrlButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *restoreButton_ = nullptr;
    QPushButton *lockButton_ = nullptr;
    QPushButton *newLoginButton_ = nullptr;
    QPushButton *newNoteButton_ = nullptr;

    QUuid currentEntryId_;
    QStringList currentTags_;
    EntryKind currentKind_ = EntryKind::Login;
    bool editorDirty_ = false;
    bool editorEditing_ = false;
    bool currentEntryPersisted_ = false;
    bool currentEntryRecycled_ = false;
    bool currentEntryHasPassword_ = false;
    bool currentEntryHasTotp_ = false;
    bool loading_ = false;
    QTimer *clipboardClearTimer_ = nullptr;
    QTimer *passwordRevealTimer_ = nullptr;
    QTimer *totpRefreshTimer_ = nullptr;
    QByteArray clipboardFingerprint_;
    QString currentTotpCode_;
};
