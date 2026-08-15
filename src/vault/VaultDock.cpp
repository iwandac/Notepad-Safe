#include "VaultDock.h"

#include "VaultBackendFactory.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

constexpr int EntryIdRole = Qt::UserRole + 1;
constexpr int ProtectedClipboardLifetimeMs = 30 * 1000;

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

} // namespace

VaultDock::VaultDock(QWidget *parent)
    : VaultDock(createProductionVaultBackend(), parent)
{
}

VaultDock::VaultDock(std::unique_ptr<IVaultBackend> backend, QWidget *parent)
    : QDockWidget(tr("保险库"), parent)
    , backendAvailable_(backend != nullptr)
    , service_(std::move(backend))
{
    setObjectName(QStringLiteral("VaultDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(360);
    buildUi();
}

VaultDock::~VaultDock()
{
    clearProtectedClipboard();
    service_.lock();
    clearEditor();
}

void VaultDock::buildUi()
{
    auto *container = new QWidget(this);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(6, 6, 6, 6);

    auto *vaultCommandRow = new QHBoxLayout();
    auto *openButton = new QPushButton(tr("打开 KDBX"), container);
    openButton->setObjectName(QStringLiteral("vaultOpenButton"));
    auto *newVaultButton = new QPushButton(tr("新建 KDBX"), container);
    newVaultButton->setObjectName(QStringLiteral("vaultNewButton"));
    lockButton_ = new QPushButton(tr("锁定"), container);
    lockButton_->setObjectName(QStringLiteral("vaultLockButton"));
    vaultCommandRow->addWidget(openButton);
    vaultCommandRow->addWidget(newVaultButton);
    vaultCommandRow->addWidget(lockButton_);
    vaultCommandRow->addStretch();
    containerLayout->addLayout(vaultCommandRow);

    auto *entryCommandRow = new QHBoxLayout();
    newLoginButton_ = new QPushButton(tr("新建账号"), container);
    newLoginButton_->setObjectName(QStringLiteral("vaultNewLoginButton"));
    newNoteButton_ = new QPushButton(tr("新建笔记"), container);
    newNoteButton_->setObjectName(QStringLiteral("vaultNewNoteButton"));
    entryCommandRow->addWidget(newLoginButton_);
    entryCommandRow->addWidget(newNoteButton_);
    entryCommandRow->addStretch();
    containerLayout->addLayout(entryCommandRow);

    pages_ = new QStackedWidget(container);
    pages_->setObjectName(QStringLiteral("vaultPages"));
    containerLayout->addWidget(pages_);

    lockedPage_ = new QWidget(pages_);
    auto *lockedLayout = new QVBoxLayout(lockedPage_);
    lockedLayout->addStretch();
    lockedMessage_ = new QLabel(lockedPage_);
    lockedMessage_->setObjectName(QStringLiteral("vaultLockedMessage"));
    lockedMessage_->setAlignment(Qt::AlignCenter);
    lockedMessage_->setWordWrap(true);
    lockedMessage_->setText(backendAvailable_
                                ? tr("保险库已锁定。\n打开 KeePassXC 兼容的 KDBX 文件以继续。")
                                : tr("当前构建未包含生产 KDBX 后端。\n请启用 NOTEPADSAFE_ENABLE_KDBX_BACKEND 后重新构建。"));
    lockedLayout->addWidget(lockedMessage_);
    auto *lockedOpenButton = new QPushButton(tr("打开保险库…"), lockedPage_);
    lockedOpenButton->setObjectName(QStringLiteral("vaultLockedOpenButton"));
    lockedOpenButton->setEnabled(backendAvailable_);
    lockedLayout->addWidget(lockedOpenButton, 0, Qt::AlignHCenter);
    lockedLayout->addStretch();
    pages_->addWidget(lockedPage_);

    openPage_ = new QWidget(pages_);
    auto *openLayout = new QVBoxLayout(openPage_);
    openLayout->setContentsMargins(0, 0, 0, 0);
    filterEdit_ = new QLineEdit(openPage_);
    filterEdit_->setObjectName(QStringLiteral("vaultFilterEdit"));
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->setPlaceholderText(tr("搜索标题、用户名或网址"));
    openLayout->addWidget(filterEdit_);

    auto *splitter = new QSplitter(Qt::Vertical, openPage_);
    entryTree_ = new QTreeWidget(splitter);
    entryTree_->setObjectName(QStringLiteral("vaultEntryTree"));
    entryTree_->setHeaderLabels({tr("条目"), tr("用户名 / 网址")});
    entryTree_->header()->setStretchLastSection(true);
    entryTree_->setRootIsDecorated(true);
    entryTree_->setAlternatingRowColors(true);
    splitter->addWidget(entryTree_);

    auto *editorScroll = new QScrollArea(splitter);
    editorScroll->setWidgetResizable(true);
    auto *editor = new QWidget(editorScroll);
    auto *editorLayout = new QVBoxLayout(editor);
    kindLabel_ = new QLabel(tr("选择一个条目"), editor);
    kindLabel_->setObjectName(QStringLiteral("vaultKindLabel"));
    QFont kindFont = kindLabel_->font();
    kindFont.setBold(true);
    kindLabel_->setFont(kindFont);
    editorLayout->addWidget(kindLabel_);

    auto *form = new QFormLayout();
    titleEdit_ = new QLineEdit(editor);
    titleEdit_->setObjectName(QStringLiteral("vaultTitleEdit"));
    usernameEdit_ = new QLineEdit(editor);
    usernameEdit_->setObjectName(QStringLiteral("vaultUsernameEdit"));
    urlEdit_ = new QLineEdit(editor);
    urlEdit_->setObjectName(QStringLiteral("vaultUrlEdit"));
    passwordEdit_ = new QLineEdit(editor);
    passwordEdit_->setObjectName(QStringLiteral("vaultPasswordEdit"));
    passwordEdit_->setEchoMode(QLineEdit::Password);
    copyPasswordButton_ = new QPushButton(tr("复制"), editor);
    copyPasswordButton_->setObjectName(QStringLiteral("vaultCopyPasswordButton"));
    copyPasswordButton_->setToolTip(tr("复制密码；30 秒后自动清除剪贴板"));
    totpEdit_ = new QLineEdit(editor);
    totpEdit_->setObjectName(QStringLiteral("vaultTotpEdit"));
    showPasswordCheck_ = new QCheckBox(tr("显示密码"), editor);
    showPasswordCheck_->setObjectName(QStringLiteral("vaultShowPasswordCheck"));
    notesEdit_ = new QTextEdit(editor);
    notesEdit_->setObjectName(QStringLiteral("vaultNotesEdit"));
    notesEdit_->setAcceptRichText(false);
    notesEdit_->setMinimumHeight(110);

    form->addRow(tr("标题"), titleEdit_);
    usernameLabel_ = new QLabel(tr("用户名"), editor);
    form->addRow(usernameLabel_, usernameEdit_);
    urlLabel_ = new QLabel(tr("网址"), editor);
    form->addRow(urlLabel_, urlEdit_);
    passwordLabel_ = new QLabel(tr("密码"), editor);
    passwordRow_ = new QWidget(editor);
    auto *passwordLayout = new QHBoxLayout(passwordRow_);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->addWidget(passwordEdit_);
    passwordLayout->addWidget(showPasswordCheck_);
    passwordLayout->addWidget(copyPasswordButton_);
    form->addRow(passwordLabel_, passwordRow_);
    totpLabel_ = new QLabel(tr("TOTP 密钥"), editor);
    form->addRow(totpLabel_, totpEdit_);
    form->addRow(tr("笔记"), notesEdit_);
    editorLayout->addLayout(form);

    auto *editorButtons = new QHBoxLayout();
    saveButton_ = new QPushButton(tr("保存到保险库"), editor);
    saveButton_->setObjectName(QStringLiteral("vaultSaveButton"));
    deleteButton_ = new QPushButton(tr("删除条目"), editor);
    deleteButton_->setObjectName(QStringLiteral("vaultDeleteButton"));
    editorButtons->addWidget(saveButton_);
    editorButtons->addWidget(deleteButton_);
    editorButtons->addStretch();
    editorLayout->addLayout(editorButtons);
    editorLayout->addStretch();
    editorScroll->setWidget(editor);
    splitter->addWidget(editorScroll);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    openLayout->addWidget(splitter);
    pages_->addWidget(openPage_);
    pages_->setCurrentWidget(lockedPage_);

    clipboardClearTimer_ = new QTimer(this);
    clipboardClearTimer_->setSingleShot(true);

    setWidget(container);
    setEditorEnabled(false);
    setVaultOpenControls(false);

    connect(openButton, &QPushButton::clicked, this, &VaultDock::openVaultInteractive);
    connect(newVaultButton, &QPushButton::clicked, this, &VaultDock::newVaultInteractive);
    connect(lockedOpenButton, &QPushButton::clicked, this, &VaultDock::openVaultInteractive);
    connect(lockButton_, &QPushButton::clicked, this, &VaultDock::lockVault);
    connect(newLoginButton_, &QPushButton::clicked, this, &VaultDock::createLogin);
    connect(newNoteButton_, &QPushButton::clicked, this, &VaultDock::createSecureNote);
    connect(filterEdit_, &QLineEdit::textChanged, this, &VaultDock::applyFilter);
    connect(entryTree_, &QTreeWidget::currentItemChanged,
            this, &VaultDock::onCurrentItemChanged);
    connect(saveButton_, &QPushButton::clicked, this, &VaultDock::saveCurrentEntry);
    connect(deleteButton_, &QPushButton::clicked, this, &VaultDock::removeCurrentEntry);
    connect(showPasswordCheck_, &QCheckBox::toggled,
            this, &VaultDock::updatePasswordEcho);
    connect(copyPasswordButton_, &QPushButton::clicked,
            this, &VaultDock::copyPasswordProtected);
    connect(clipboardClearTimer_, &QTimer::timeout,
            this, &VaultDock::clearProtectedClipboard);

    const auto markDirty = [this]() {
        if (!loading_)
            setDirty();
    };
    connect(titleEdit_, &QLineEdit::textEdited, this, markDirty);
    connect(usernameEdit_, &QLineEdit::textEdited, this, markDirty);
    connect(urlEdit_, &QLineEdit::textEdited, this, markDirty);
    connect(passwordEdit_, &QLineEdit::textEdited, this, markDirty);
    connect(totpEdit_, &QLineEdit::textEdited, this, markDirty);
    connect(notesEdit_, &QTextEdit::textChanged, this, markDirty);
}

VaultError VaultDock::createVault(const QString &filePath,
                                  const SecureBuffer &masterPasswordUtf8,
                                  const QString &keyFilePath)
{
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};

    VaultError error = service_.create(filePath, masterPasswordUtf8, keyFilePath);
    if (error.isError()) {
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }

    clearEditor();
    pages_->setCurrentWidget(openPage_);
    setVaultOpenControls(true);
    refreshEntries();
    return VaultError::success();
}

VaultError VaultDock::unlockVault(const QString &filePath,
                                  const SecureBuffer &masterPasswordUtf8,
                                  const QString &keyFilePath)
{
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};

    VaultError error = service_.unlock(filePath, masterPasswordUtf8, keyFilePath);
    if (error.isError()) {
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }

    clearEditor();
    pages_->setCurrentWidget(openPage_);
    setVaultOpenControls(true);
    refreshEntries();
    return VaultError::success();
}

void VaultDock::newVaultInteractive()
{
    if (!backendAvailable_) {
        showError({VaultErrorCode::BackendUnavailable,
                   tr("当前构建未包含生产 KDBX 后端。"),
                   QStringLiteral("vault.backend.unavailable")},
                  tr("无法新建保险库"));
        return;
    }

    QString filePath = QFileDialog::getSaveFileName(
        this, tr("新建 KDBX 保险库"), QString(), tr("KeePass 数据库 (*.kdbx)"));
    if (filePath.isEmpty())
        return;
    if (!filePath.endsWith(QStringLiteral(".kdbx"), Qt::CaseInsensitive))
        filePath += QStringLiteral(".kdbx");

    QDialog dialog(this);
    dialog.setWindowTitle(tr("设置保险库凭据"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *pathLabel = new QLabel(filePath, &dialog);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("vaultCreatePassword"));
    password->setEchoMode(QLineEdit::Password);
    auto *confirmation = new QLineEdit(&dialog);
    confirmation->setObjectName(QStringLiteral("vaultCreatePasswordConfirm"));
    confirmation->setEchoMode(QLineEdit::Password);
    auto *keyFile = new QLineEdit(&dialog);
    keyFile->setObjectName(QStringLiteral("vaultCreateKeyFile"));
    auto *keyRow = new QWidget(&dialog);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    auto *browseKey = new QPushButton(tr("浏览…"), keyRow);
    keyLayout->addWidget(keyFile);
    keyLayout->addWidget(browseKey);
    form->addRow(tr("保险库"), pathLabel);
    form->addRow(tr("主密码"), password);
    form->addRow(tr("确认密码"), confirmation);
    form->addRow(tr("已有密钥文件（可选）"), keyRow);
    layout->addLayout(form);
    auto *warning = new QLabel(tr("主密码无法找回。请使用足够长且唯一的密码，并妥善备份 KDBX 文件。"),
                               &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("创建"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseKey, &QPushButton::clicked, &dialog, [this, keyFile]() {
        const QString selected = QFileDialog::getOpenFileName(this, tr("选择密钥文件"));
        if (!selected.isEmpty())
            keyFile->setText(selected);
    });

    while (dialog.exec() == QDialog::Accepted) {
        if (password->text().isEmpty() && keyFile->text().isEmpty()) {
            QMessageBox::warning(&dialog, tr("无法创建"), tr("主密码和密钥文件不能同时为空。"));
            password->setFocus();
            continue;
        }
        if (password->text() != confirmation->text()) {
            QMessageBox::warning(&dialog, tr("无法创建"), tr("两次输入的主密码不一致。"));
            confirmation->clear();
            confirmation->setFocus();
            continue;
        }

        QString passwordText = password->text();
        SecureBuffer passwordBuffer = secureUtf8From(passwordText);
        wipeQString(passwordText);
        password->clear();
        confirmation->clear();
        VaultError error = createVault(filePath, passwordBuffer, keyFile->text());
        passwordBuffer.clear();
        if (!error.isError())
            return;
        showError(error, tr("创建失败"));
        password->setFocus();
    }
}

void VaultDock::openVaultInteractive()
{
    if (!backendAvailable_) {
        showError({VaultErrorCode::BackendUnavailable,
                   tr("当前构建未包含生产 KDBX 后端。"),
                   QStringLiteral("vault.backend.unavailable")},
                  tr("无法打开保险库"));
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("打开 KDBX 保险库"), QString(), tr("KeePass 数据库 (*.kdbx);;所有文件 (*)"));
    if (filePath.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("解锁保险库"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *pathLabel = new QLabel(filePath, &dialog);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("vaultUnlockPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setClearButtonEnabled(true);
    auto *keyFile = new QLineEdit(&dialog);
    keyFile->setObjectName(QStringLiteral("vaultUnlockKeyFile"));
    auto *keyRow = new QWidget(&dialog);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    auto *browseKey = new QPushButton(tr("浏览…"), keyRow);
    keyLayout->addWidget(keyFile);
    keyLayout->addWidget(browseKey);
    form->addRow(tr("保险库"), pathLabel);
    form->addRow(tr("主密码"), password);
    form->addRow(tr("密钥文件（可选）"), keyRow);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("解锁"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseKey, &QPushButton::clicked, &dialog, [this, keyFile]() {
        const QString selected = QFileDialog::getOpenFileName(this, tr("选择密钥文件"));
        if (!selected.isEmpty())
            keyFile->setText(selected);
    });

    while (dialog.exec() == QDialog::Accepted) {
        QString passwordText = password->text();
        SecureBuffer passwordBuffer = secureUtf8From(passwordText);
        wipeQString(passwordText);
        password->clear();
        VaultError error = unlockVault(filePath, passwordBuffer, keyFile->text());
        passwordBuffer.clear();
        if (!error.isError())
            return;
        showError(error, tr("解锁失败"));
        password->setFocus();
    }
}

void VaultDock::lockVault()
{
    if (service_.state() == VaultState::Locked)
        return;
    if (!maybeSaveChanges())
        return;
    clearProtectedClipboard();
    service_.lock();
    entryTree_->clear();
    loginRoot_ = nullptr;
    noteRoot_ = nullptr;
    clearEditor();
    pages_->setCurrentWidget(lockedPage_);
    setVaultOpenControls(false);
}

bool VaultDock::prepareToClose()
{
    if (!maybeSaveChanges())
        return false;
    service_.lock();
    clearEditor();
    setVaultOpenControls(false);
    return true;
}

void VaultDock::refreshEntries(const QUuid &selectId)
{
    QSignalBlocker blocker(entryTree_);
    entryTree_->clear();
    loginRoot_ = new QTreeWidgetItem(entryTree_, {tr("账号"), QString()});
    noteRoot_ = new QTreeWidgetItem(entryTree_, {tr("加密笔记"), QString()});
    loginRoot_->setFlags(loginRoot_->flags() & ~Qt::ItemIsSelectable);
    noteRoot_->setFlags(noteRoot_->flags() & ~Qt::ItemIsSelectable);
    loginRoot_->setExpanded(true);
    noteRoot_->setExpanded(true);

    VaultError error;
    const auto entries = service_.listEntries(&error);
    if (error.isError()) {
        showError(error, tr("无法读取保险库"));
        return;
    }

    QTreeWidgetItem *toSelect = nullptr;
    for (const auto &entry : entries) {
        auto *parent = entry.secureNote ? noteRoot_ : loginRoot_;
        QString detail = entry.secureNote ? tr("安全笔记") : entry.username;
        if (detail.isEmpty())
            detail = entry.url;
        auto *item = new QTreeWidgetItem(parent, {entry.title, detail});
        item->setData(0, EntryIdRole, entry.id.toString(QUuid::WithoutBraces));
        item->setToolTip(0, entry.url);
        if (entry.id == selectId)
            toSelect = item;
    }
    entryTree_->sortItems(0, Qt::AscendingOrder);
    applyFilter(filterEdit_->text());
    if (toSelect)
        entryTree_->setCurrentItem(toSelect);
}

void VaultDock::onCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous)
{
    if (loading_ || !current)
        return;
    const QUuid id(current->data(0, EntryIdRole).toString());
    if (id.isNull())
        return;
    if (!maybeSaveChanges()) {
        QSignalBlocker blocker(entryTree_);
        entryTree_->setCurrentItem(previous);
        return;
    }
    loadEntry(id);
}

void VaultDock::loadEntry(const QUuid &id)
{
    VaultError error;
    auto entry = service_.readEntry(id, &error);
    if (!entry) {
        showError(error, tr("无法读取条目"));
        return;
    }

    loading_ = true;
    currentEntryId_ = entry->id;
    currentTags_ = entry->tags;
    setEntryKind(entry->tags.contains(VaultSecureNoteTag) ? EntryKind::SecureNote
                                                          : EntryKind::Login);
    titleEdit_->setText(entry->title);
    usernameEdit_->setText(entry->username);
    urlEdit_->setText(entry->url);
    QString password = transientText(entry->passwordUtf8);
    passwordEdit_->setText(password);
    wipeQString(password);
    QString totp = transientText(entry->totpSeedUtf8);
    totpEdit_->setText(totp);
    wipeQString(totp);
    QString notes = transientText(entry->notesUtf8);
    notesEdit_->setPlainText(notes);
    wipeQString(notes);
    entry->passwordUtf8.clear();
    entry->totpSeedUtf8.clear();
    entry->notesUtf8.clear();
    setEditorEnabled(true);
    deleteButton_->setEnabled(true);
    editorDirty_ = false;
    loading_ = false;
}

void VaultDock::createLogin()
{
    if (service_.state() != VaultState::Open)
        return;
    beginNewEntry(EntryKind::Login);
}

void VaultDock::createSecureNote()
{
    if (service_.state() != VaultState::Open)
        return;
    beginNewEntry(EntryKind::SecureNote);
}

void VaultDock::beginNewEntry(EntryKind kind)
{
    if (!maybeSaveChanges())
        return;
    loading_ = true;
    entryTree_->clearSelection();
    clearEditor();
    currentEntryId_ = QUuid::createUuid();
    currentTags_.clear();
    setEntryKind(kind);
    setEditorEnabled(true);
    deleteButton_->setEnabled(false);
    loading_ = false;
    setDirty(true);
    titleEdit_->setFocus();
}

void VaultDock::saveCurrentEntry()
{
    saveCurrentEntryImpl(true);
}

bool VaultDock::saveCurrentEntryImpl(bool showSuccess)
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull())
        return false;
    if (titleEdit_->text().trimmed().isEmpty()) {
        if (showSuccess)
            QMessageBox::warning(this, tr("无法保存"), tr("标题不能为空。"));
        titleEdit_->setFocus();
        return false;
    }

    VaultEntryData data;
    data.id = currentEntryId_;
    data.title = titleEdit_->text().trimmed();
    data.username = currentKind_ == EntryKind::Login ? usernameEdit_->text() : QString();
    data.url = currentKind_ == EntryKind::Login ? urlEdit_->text() : QString();
    data.tags = currentTags_;
    data.tags.removeAll(VaultSecureNoteTag);
    if (currentKind_ == EntryKind::SecureNote)
        data.tags.push_back(VaultSecureNoteTag);
    if (currentKind_ == EntryKind::Login) {
        data.passwordUtf8 = secureUtf8From(passwordEdit_->text());
        data.totpSeedUtf8 = secureUtf8From(totpEdit_->text().trimmed());
    }
    data.notesUtf8 = secureUtf8From(notesEdit_->toPlainText());

    VaultError error = service_.upsertEntry(std::move(data));
    if (!error.isError())
        error = service_.save();
    if (error.isError()) {
        showError(error, tr("保存失败"));
        return false;
    }

    editorDirty_ = false;
    deleteButton_->setEnabled(true);
    refreshEntries(currentEntryId_);
    if (showSuccess)
        kindLabel_->setText(currentKind_ == EntryKind::SecureNote
                                ? tr("加密笔记 · 已保存")
                                : tr("账号 · 已保存"));
    return true;
}

void VaultDock::removeCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull())
        return;
    if (QMessageBox::question(this, tr("删除条目"),
                              tr("确定要从保险库中永久删除“%1”吗？").arg(titleEdit_->text()),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    VaultError error = service_.removeEntry(currentEntryId_);
    if (!error.isError())
        error = service_.save();
    if (error.isError()) {
        showError(error, tr("删除失败"));
        return;
    }
    clearEditor();
    refreshEntries();
}

void VaultDock::setEntryKind(EntryKind kind)
{
    currentKind_ = kind;
    const bool login = kind == EntryKind::Login;
    kindLabel_->setText(login ? tr("账号") : tr("加密笔记"));
    const QList<QWidget *> loginWidgets = {
        usernameLabel_, usernameEdit_, urlLabel_, urlEdit_, passwordLabel_,
        passwordRow_, totpLabel_, totpEdit_};
    for (QWidget *widget : loginWidgets) {
        widget->setVisible(login);
    }
}

void VaultDock::setEditorEnabled(bool enabled)
{
    const QList<QWidget *> editorWidgets = {
        titleEdit_, usernameEdit_, urlEdit_, passwordEdit_, totpEdit_,
        notesEdit_, showPasswordCheck_, copyPasswordButton_, saveButton_};
    for (QWidget *widget : editorWidgets) {
        widget->setEnabled(enabled);
    }
    deleteButton_->setEnabled(enabled && !currentEntryId_.isNull());
}

void VaultDock::setVaultOpenControls(bool open)
{
    lockButton_->setEnabled(open);
    newLoginButton_->setEnabled(open);
    newNoteButton_->setEnabled(open);
}

void VaultDock::clearEditor()
{
    loading_ = true;
    titleEdit_->clear();
    usernameEdit_->clear();
    urlEdit_->clear();
    passwordEdit_->clear();
    totpEdit_->clear();
    notesEdit_->clear();
    showPasswordCheck_->setChecked(false);
    currentEntryId_ = {};
    currentTags_.clear();
    editorDirty_ = false;
    kindLabel_->setText(tr("选择一个条目"));
    setEditorEnabled(false);
    loading_ = false;
}

void VaultDock::setDirty(bool dirty)
{
    editorDirty_ = dirty;
    if (dirty && !kindLabel_->text().endsWith(QLatin1Char('*')))
        kindLabel_->setText(kindLabel_->text() + QStringLiteral(" *"));
}

bool VaultDock::maybeSaveChanges()
{
    if (!editorDirty_)
        return true;
    const auto result = QMessageBox::warning(
        this, tr("未保存的保险库条目"), tr("当前条目有未保存的修改。"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (result == QMessageBox::Cancel)
        return false;
    if (result == QMessageBox::Discard) {
        editorDirty_ = false;
        return true;
    }
    return saveCurrentEntryImpl(false);
}

void VaultDock::applyFilter(const QString &text)
{
    if (!loginRoot_ || !noteRoot_)
        return;
    const QString needle = text.trimmed();
    for (QTreeWidgetItem *root : {loginRoot_, noteRoot_}) {
        bool anyVisible = false;
        for (int i = 0; i < root->childCount(); ++i) {
            auto *item = root->child(i);
            const bool matches = needle.isEmpty()
                                 || item->text(0).contains(needle, Qt::CaseInsensitive)
                                 || item->text(1).contains(needle, Qt::CaseInsensitive)
                                 || item->toolTip(0).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matches);
            anyVisible |= matches;
        }
        root->setHidden(!needle.isEmpty() && !anyVisible);
    }
}

void VaultDock::updatePasswordEcho(bool visible)
{
    passwordEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
}

void VaultDock::copyPasswordProtected()
{
    if (service_.state() != VaultState::Open || currentKind_ != EntryKind::Login
        || passwordEdit_->text().isEmpty()) {
        return;
    }

    QString password = passwordEdit_->text();
    QByteArray utf8 = password.toUtf8();
    clipboardFingerprint_ = QCryptographicHash::hash(utf8, QCryptographicHash::Sha256);
    QGuiApplication::clipboard()->setText(password, QClipboard::Clipboard);
    scrubByteArray(utf8);
    wipeQString(password);
    clipboardClearTimer_->start(ProtectedClipboardLifetimeMs);
}

void VaultDock::clearProtectedClipboard()
{
    if (clipboardClearTimer_)
        clipboardClearTimer_->stop();
    if (clipboardFingerprint_.isEmpty())
        return;

    QClipboard *clipboard = QGuiApplication::clipboard();
    QByteArray current = clipboard->text(QClipboard::Clipboard).toUtf8();
    const QByteArray fingerprint = QCryptographicHash::hash(current, QCryptographicHash::Sha256);
    scrubByteArray(current);
    if (fingerprint == clipboardFingerprint_)
        clipboard->clear(QClipboard::Clipboard);
    scrubByteArray(clipboardFingerprint_);
}

void VaultDock::showError(const VaultError &error, const QString &title)
{
    QString message = error.userMessage;
    if (message.isEmpty())
        message = tr("发生未知错误。");
    QMessageBox box(QMessageBox::Critical, title, message, QMessageBox::Ok, this);
    if (!error.diagnosticCode.isEmpty())
        box.setDetailedText(error.diagnosticCode);
    box.exec();
}

SecureBuffer VaultDock::secureUtf8From(const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    SecureBuffer result(utf8.constData(), utf8.size());
    scrubByteArray(utf8);
    return result;
}

QString VaultDock::transientText(const SecureBuffer &value)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(value.data()), value.size());
}

void VaultDock::wipeQString(QString &text)
{
    if (!text.isEmpty())
        text.fill(QChar(0));
    text.clear();
}
