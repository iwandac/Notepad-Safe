#pragma once

#include "VaultService.h"

#include <QDockWidget>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
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
    VaultError createVault(const QString &filePath,
                           const SecureBuffer &masterPasswordUtf8,
                           const QString &keyFilePath = {});
    VaultError unlockVault(const QString &filePath,
                           const SecureBuffer &masterPasswordUtf8,
                           const QString &keyFilePath = {});
    bool prepareToClose();

public slots:
    void newVaultInteractive();
    void openVaultInteractive();
    void lockVault();
    void createLogin();
    void createSecureNote();

private slots:
    void onCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void saveCurrentEntry();
    void removeCurrentEntry();
    void applyFilter(const QString &text);
    void updatePasswordEcho(bool visible);
    void copyPasswordProtected();
    void clearProtectedClipboard();

private:
    enum class EntryKind { Login, SecureNote };

    void buildUi();
    void refreshEntries(const QUuid &selectId = {});
    void loadEntry(const QUuid &id);
    void beginNewEntry(EntryKind kind);
    void clearEditor();
    void setEditorEnabled(bool enabled);
    void setVaultOpenControls(bool open);
    void setEntryKind(EntryKind kind);
    void setDirty(bool dirty = true);
    bool maybeSaveChanges();
    bool saveCurrentEntryImpl(bool showSuccess);
    void showError(const VaultError &error, const QString &title);
    static SecureBuffer secureUtf8From(const QString &text);
    static QString transientText(const SecureBuffer &value);
    static void wipeQString(QString &text);

    bool backendAvailable_ = false;
    VaultService service_;
    QStackedWidget *pages_ = nullptr;
    QWidget *lockedPage_ = nullptr;
    QWidget *openPage_ = nullptr;
    QLabel *lockedMessage_ = nullptr;
    QLineEdit *filterEdit_ = nullptr;
    QTreeWidget *entryTree_ = nullptr;
    QTreeWidgetItem *loginRoot_ = nullptr;
    QTreeWidgetItem *noteRoot_ = nullptr;
    QLabel *kindLabel_ = nullptr;
    QLineEdit *titleEdit_ = nullptr;
    QLineEdit *usernameEdit_ = nullptr;
    QLineEdit *urlEdit_ = nullptr;
    QLineEdit *passwordEdit_ = nullptr;
    QWidget *passwordRow_ = nullptr;
    QLineEdit *totpEdit_ = nullptr;
    QTextEdit *notesEdit_ = nullptr;
    QLabel *usernameLabel_ = nullptr;
    QLabel *urlLabel_ = nullptr;
    QLabel *passwordLabel_ = nullptr;
    QLabel *totpLabel_ = nullptr;
    QCheckBox *showPasswordCheck_ = nullptr;
    QPushButton *copyPasswordButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *lockButton_ = nullptr;
    QPushButton *newLoginButton_ = nullptr;
    QPushButton *newNoteButton_ = nullptr;

    QUuid currentEntryId_;
    QStringList currentTags_;
    EntryKind currentKind_ = EntryKind::Login;
    bool editorDirty_ = false;
    bool loading_ = false;
    QTimer *clipboardClearTimer_ = nullptr;
    QByteArray clipboardFingerprint_;
};
