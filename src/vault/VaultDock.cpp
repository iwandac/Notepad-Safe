#include "VaultDock.h"
#include "AsyncUiOperation.h"

#include "VaultBackendFactory.h"
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
#include "SecurityIdentity.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QApplication>
#include <QCryptographicHash>
#include <QColor>
#include <QDesktopServices>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QSettings>
#include <QStandardPaths>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {

constexpr int EntryIdRole = Qt::UserRole + 1;
constexpr int EntryRecycledRole = Qt::UserRole + 2;
constexpr int ProtectedClipboardLifetimeMs = 30 * 1000;
constexpr int PasswordRevealLifetimeMs = 30 * 1000;
constexpr qsizetype SecureNoteMaximumBytes = 32 * 1024 * 1024;

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
    // Hold the old implicitly-shared buffer while replacing the widget value;
    // once the widget releases it, this copy is the remaining owner and can be
    // overwritten before it is freed. This is best-effort (Qt/OS may keep
    // internal copies) but avoids intentionally retaining the clear text.
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

std::unique_ptr<QSettings> createSecuritySettings()
{
    const QString dataDir = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("NOTEPADSAFE_DATADIR")).trimmed();
    if (!dataDir.isEmpty() && QFileInfo(dataDir).isAbsolute()) {
        QDir directory(dataDir);
        directory.mkpath(QStringLiteral("."));
        return std::make_unique<QSettings>(directory.filePath(QStringLiteral("settings.ini")),
                                           QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(QStringLiteral("NotepadSafe"),
                                       QStringLiteral("NotepadSafe"));
}

QString defaultSecureStorePath()
{
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty())
        documents = QDir::homePath();
    return QDir(documents).filePath(QStringLiteral("Notepad Safe/accounts.kdbx"));
}

void showRecoveryKey(QWidget *parent, const QString &recoveryKey)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("保存恢复密钥"));
    dialog.resize(680, 340);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        QObject::tr("这是解锁全部加密数据的最后恢复手段。请打印或保存到其他设备；"
                    "不要只保存在当前电脑的应用目录中。"), &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *keyText = new QPlainTextEdit(recoveryKey, &dialog);
    keyText->setObjectName(QStringLiteral("securityRecoveryKey"));
    keyText->setReadOnly(true);
    keyText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(keyText);
    auto *actions = new QHBoxLayout();
    auto *copy = new QPushButton(QObject::tr("复制恢复密钥"), &dialog);
    auto *save = new QPushButton(QObject::tr("保存到文件…"), &dialog);
    actions->addWidget(copy);
    actions->addWidget(save);
    actions->addStretch();
    layout->addLayout(actions);
    auto *confirmed = new QCheckBox(QObject::tr("我已将恢复密钥保存到独立且安全的位置"), &dialog);
    confirmed->setObjectName(QStringLiteral("securityRecoveryConfirmed"));
    layout->addWidget(confirmed);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("完成"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [recoveryKey]() {
        QGuiApplication::clipboard()->setText(recoveryKey, QClipboard::Clipboard);
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, recoveryKey]() {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QObject::tr("保存恢复密钥"), QStringLiteral("NotepadSafe-Recovery-Key.txt"),
            QObject::tr("文本文件 (*.txt)"));
        if (path.isEmpty())
            return;
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)
            || output.write(recoveryKey.toUtf8()) < 0 || !output.commit()) {
            QMessageBox::critical(&dialog, QObject::tr("保存失败"),
                                  QObject::tr("无法保存恢复密钥文件。"));
        }
    });
    QObject::connect(confirmed, &QCheckBox::toggled,
                     buttons->button(QDialogButtonBox::Ok), &QPushButton::setEnabled);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    dialog.exec();
}

} // namespace

VaultDock::VaultDock(QWidget *parent)
    : VaultDock(createProductionVaultBackend(), parent)
{
}

VaultDock::VaultDock(std::unique_ptr<IVaultBackend> backend, QWidget *parent)
    : QDockWidget(tr("账户"), parent)
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
    // clearEditor() updates command state during normal operation. During
    // QObject child teardown, however, its owner may already have destroyed
    // sibling widgets used by those callbacks. Destructors must never emit
    // workspace state into a partially destroyed MainWindow.
    blockSignals(true);
    clearProtectedClipboard();
    lockServiceOnSecurityLane();
    clearEditor();
}

void VaultDock::lockServiceOnSecurityLane()
{
    if (service_.state() == VaultState::Locked)
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在锁定账户数据库"),
        [this] {
            service_.lock();
            return VaultError{};
        },
        false);
}

void VaultDock::buildUi()
{
    auto *container = new QWidget(this);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(6, 6, 6, 6);

    auto *vaultCommandRow = new QHBoxLayout();
    auto *openButton = new QPushButton(tr("打开账户文件…"), container);
    openButton->setObjectName(QStringLiteral("vaultOpenButton"));
    auto *newVaultButton = new QPushButton(tr("新建账户文件…"), container);
    newVaultButton->setObjectName(QStringLiteral("vaultNewButton"));
    lockButton_ = new QPushButton(tr("锁定"), container);
    lockButton_->setObjectName(QStringLiteral("vaultLockButton"));
    vaultCommandRow->addWidget(openButton);
    vaultCommandRow->addWidget(newVaultButton);
    vaultCommandRow->addWidget(lockButton_);
    vaultCommandRow->addStretch();
    containerLayout->addLayout(vaultCommandRow);
    openButton->hide();
    newVaultButton->hide();
    lockButton_->hide();

    newLoginButton_ = new QPushButton(tr("新建账号"), container);
    newLoginButton_->setObjectName(QStringLiteral("vaultNewLoginButton"));
    newNoteButton_ = new QPushButton(tr("导入旧版安全笔记"), container);
    newNoteButton_->setObjectName(QStringLiteral("vaultNewNoteButton"));
    newNoteButton_->hide();

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
                                ? tr("账户文件尚未打开。\n日常使用由当前安全会话自动解锁。")
                                : tr("当前构建未包含生产 KDBX 后端。\n请启用 NOTEPADSAFE_ENABLE_KDBX_BACKEND 后重新构建。"));
    lockedLayout->addWidget(lockedMessage_);
    auto *lockedOpenButton = new QPushButton(tr("解锁或打开…"), lockedPage_);
    lockedOpenButton->setObjectName(QStringLiteral("vaultLockedOpenButton"));
    lockedOpenButton->setEnabled(backendAvailable_);
    lockedLayout->addWidget(lockedOpenButton, 0, Qt::AlignHCenter);
    lockedOpenButton->hide();
    lockedLayout->addStretch();
    pages_->addWidget(lockedPage_);

    openPage_ = new QWidget(pages_);
    auto *openLayout = new QVBoxLayout(openPage_);
    openLayout->setContentsMargins(0, 0, 0, 0);
    openLayout->setSpacing(10);
    searchRowWidget_ = new QWidget(openPage_);
    auto *searchRow = new QHBoxLayout(searchRowWidget_);
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(10);
    filterEdit_ = new QLineEdit(openPage_);
    filterEdit_->setObjectName(QStringLiteral("vaultFilterEdit"));
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->setPlaceholderText(tr("搜索标题、用户名、网址、标签或备注"));
    filterEdit_->setMinimumHeight(34);
    sortCombo_ = new QComboBox(openPage_);
    sortCombo_->setObjectName(QStringLiteral("vaultSortCombo"));
    sortCombo_->addItem(tr("标题排序"), QStringLiteral("title"));
    sortCombo_->addItem(tr("用户名排序"), QStringLiteral("username"));
    sortCombo_->addItem(tr("分组排序"), QStringLiteral("group"));
    healthButton_ = new QPushButton(tr("健康检查"), openPage_);
    healthButton_->setObjectName(QStringLiteral("vaultHealthButton"));
    healthButton_->setMinimumHeight(34);
    newLoginButton_->setParent(openPage_);
    newLoginButton_->setText(tr("+ 新建账号"));
    newLoginButton_->setMinimumHeight(34);
    searchRow->addWidget(filterEdit_, 1);
    searchRow->addWidget(sortCombo_);
    searchRow->addWidget(healthButton_);
    searchRow->addWidget(newLoginButton_);
    openLayout->addWidget(searchRowWidget_);

    contentPages_ = new QStackedWidget(openPage_);
    contentPages_->setObjectName(QStringLiteral("accountsContentPages"));

    auto *firstAccountPage = new QWidget(contentPages_);
    auto *firstAccountLayout = new QVBoxLayout(firstAccountPage);
    firstAccountLayout->setContentsMargins(32, 32, 32, 32);
    firstAccountLayout->addStretch(2);
    auto *firstAccountTitle = new QLabel(tr("安全保存你的第一个账号"), firstAccountPage);
    firstAccountTitle->setObjectName(QStringLiteral("accountsFirstAccountTitle"));
    firstAccountTitle->setAlignment(Qt::AlignCenter);
    QFont firstAccountTitleFont = firstAccountTitle->font();
    firstAccountTitleFont.setPointSize(firstAccountTitleFont.pointSize() + 5);
    firstAccountTitleFont.setBold(true);
    firstAccountTitle->setFont(firstAccountTitleFont);
    auto *firstAccountHint = new QLabel(
        tr("账号密码保存在本机加密账户文件中。\n登录安全会话后，查看、编辑和复制都不再重复询问密码。"),
        firstAccountPage);
    firstAccountHint->setObjectName(QStringLiteral("accountsFirstAccountHint"));
    firstAccountHint->setAlignment(Qt::AlignCenter);
    firstAccountHint->setWordWrap(true);
    emptyCreateButton_ = new QPushButton(tr("新建账号"), firstAccountPage);
    emptyCreateButton_->setObjectName(QStringLiteral("accountsEmptyCreateButton"));
    emptyCreateButton_->setMinimumHeight(36);
    emptyCreateButton_->setMinimumWidth(128);
    firstAccountLayout->addWidget(firstAccountTitle);
    firstAccountLayout->addSpacing(8);
    firstAccountLayout->addWidget(firstAccountHint);
    firstAccountLayout->addSpacing(18);
    firstAccountLayout->addWidget(emptyCreateButton_, 0, Qt::AlignHCenter);
    firstAccountLayout->addStretch(3);
    contentPages_->addWidget(firstAccountPage);

    auto *splitter = new QSplitter(Qt::Horizontal, contentPages_);
    splitter->setObjectName(QStringLiteral("accountsContentSplitter"));
    splitter->setChildrenCollapsible(false);

    listPages_ = new QStackedWidget(splitter);
    listPages_->setObjectName(QStringLiteral("accountsListPages"));
    listPages_->setFrameShape(QFrame::StyledPanel);
    entryTree_ = new QTreeWidget(listPages_);
    entryTree_->setObjectName(QStringLiteral("vaultEntryTree"));
    entryTree_->setHeaderLabels({tr("条目"), tr("用户名 / 网址")});
    entryTree_->header()->setStretchLastSection(false);
    entryTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    entryTree_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    // Keep the account title readable when the two-pane workspace narrows.
    // The second column remains user-resizable, while its default no longer
    // consumes two thirds of the minimum-width list.
    entryTree_->header()->resizeSection(1, 175);
    entryTree_->setRootIsDecorated(true);
    entryTree_->setAlternatingRowColors(true);
    entryTree_->setMinimumWidth(330);
    listPages_->addWidget(entryTree_);

    auto *listEmptyPage = new QWidget(listPages_);
    auto *listEmptyLayout = new QVBoxLayout(listEmptyPage);
    listEmptyLayout->setContentsMargins(24, 24, 24, 24);
    listEmptyLayout->addStretch();
    listEmptyLabel_ = new QLabel(tr("还没有账号\n使用右上角“+ 新建账号”开始"),
                                 listEmptyPage);
    listEmptyLabel_->setObjectName(QStringLiteral("accountsListEmptyLabel"));
    listEmptyLabel_->setAlignment(Qt::AlignCenter);
    listEmptyLabel_->setWordWrap(true);
    QFont emptyListFont = listEmptyLabel_->font();
    emptyListFont.setPointSize(emptyListFont.pointSize() + 1);
    listEmptyLabel_->setFont(emptyListFont);
    listEmptyLayout->addWidget(listEmptyLabel_);
    listEmptyLayout->addStretch();
    listPages_->addWidget(listEmptyPage);
    splitter->addWidget(listPages_);

    detailPages_ = new QStackedWidget(splitter);
    detailPages_->setObjectName(QStringLiteral("accountsDetailPages"));
    detailPages_->setFrameShape(QFrame::StyledPanel);
    auto *detailEmptyPage = new QWidget(detailPages_);
    auto *detailEmptyLayout = new QVBoxLayout(detailEmptyPage);
    detailEmptyLayout->setContentsMargins(40, 40, 40, 40);
    detailEmptyLayout->addStretch();
    auto *detailEmptyTitle = new QLabel(tr("选择一个账号"), detailEmptyPage);
    detailEmptyTitle->setObjectName(QStringLiteral("accountsDetailEmptyTitle"));
    detailEmptyTitle->setAlignment(Qt::AlignCenter);
    QFont emptyTitleFont = detailEmptyTitle->font();
    emptyTitleFont.setPointSize(emptyTitleFont.pointSize() + 4);
    emptyTitleFont.setBold(true);
    detailEmptyTitle->setFont(emptyTitleFont);
    auto *detailEmptyHint = new QLabel(
        tr("从左侧选择账号以查看详情，或创建一个新账号。\n密码默认隐藏，复制后会自动清理剪贴板。"),
        detailEmptyPage);
    detailEmptyHint->setObjectName(QStringLiteral("accountsDetailEmptyHint"));
    detailEmptyHint->setAlignment(Qt::AlignCenter);
    detailEmptyHint->setWordWrap(true);
    detailEmptyLayout->addWidget(detailEmptyTitle);
    detailEmptyLayout->addSpacing(8);
    detailEmptyLayout->addWidget(detailEmptyHint);
    detailEmptyLayout->addStretch();
    detailPages_->addWidget(detailEmptyPage);

    auto *editorScroll = new QScrollArea(detailPages_);
    editorScroll->setWidgetResizable(true);
    editorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *editor = new QWidget(editorScroll);
    detailEditorPage_ = editorScroll;
    auto *editorLayout = new QVBoxLayout(editor);
    kindLabel_ = new QLabel(tr("选择一个条目"), editor);
    kindLabel_->setObjectName(QStringLiteral("vaultKindLabel"));
    QFont kindFont = kindLabel_->font();
    kindFont.setBold(true);
    kindLabel_->setFont(kindFont);
    editorLayout->addWidget(kindLabel_);

    auto *detailHeader = new QWidget(editor);
    detailHeader->setObjectName(QStringLiteral("vaultDetailHeader"));
    auto *detailHeaderLayout = new QHBoxLayout(detailHeader);
    detailHeaderLayout->setContentsMargins(0, 0, 0, 0);
    detailHeaderLayout->setSpacing(8);
    titleEdit_ = new QLineEdit(editor);
    titleEdit_->setObjectName(QStringLiteral("vaultTitleEdit"));
    titleEdit_->setPlaceholderText(tr("标题"));
    detailHeaderLayout->addWidget(titleEdit_, 1);

    editButton_ = new QPushButton(tr("编辑"), detailHeader);
    editButton_->setObjectName(QStringLiteral("vaultEditButton"));
    historyButton_ = new QPushButton(tr("历史"), detailHeader);
    historyButton_->setObjectName(QStringLiteral("vaultHistoryButton"));
    historyButton_->setToolTip(tr("查看并恢复 KeePassXC 条目历史"));
    cloneButton_ = new QPushButton(tr("克隆"), detailHeader);
    cloneButton_->setObjectName(QStringLiteral("vaultCloneButton"));
    cloneButton_->setToolTip(tr("复制此账号及其扩展字段，在当前分组中创建新条目"));
    saveButton_ = new QPushButton(tr("保存"), detailHeader);
    saveButton_->setObjectName(QStringLiteral("vaultSaveButton"));
    cancelButton_ = new QPushButton(tr("取消"), detailHeader);
    cancelButton_->setObjectName(QStringLiteral("vaultCancelButton"));
    restoreButton_ = new QPushButton(tr("恢复"), detailHeader);
    restoreButton_->setObjectName(QStringLiteral("vaultRestoreButton"));
    deleteButton_ = new QPushButton(tr("移到回收站"), detailHeader);
    deleteButton_->setObjectName(QStringLiteral("vaultDeleteButton"));
    deleteButton_->setToolTip(tr("先移到回收站；可在永久删除前恢复"));
    detailHeaderLayout->addWidget(saveButton_);
    detailHeaderLayout->addWidget(cancelButton_);
    detailHeaderLayout->addWidget(editButton_);
    detailHeaderLayout->addWidget(historyButton_);
    detailHeaderLayout->addWidget(cloneButton_);
    detailHeaderLayout->addWidget(restoreButton_);
    detailHeaderLayout->addWidget(deleteButton_);
    editorLayout->addWidget(detailHeader);

    formLayout_ = new QFormLayout();
    formLayout_->setHorizontalSpacing(18);
    formLayout_->setVerticalSpacing(10);
    usernameEdit_ = new QLineEdit(editor);
    usernameEdit_->setObjectName(QStringLiteral("vaultUsernameEdit"));
    copyUsernameButton_ = new QPushButton(tr("复制"), editor);
    copyUsernameButton_->setObjectName(QStringLiteral("vaultCopyUsernameButton"));
    copyUsernameButton_->setToolTip(tr("复制用户名；30 秒后自动清除剪贴板"));
    usernameRow_ = new QWidget(editor);
    auto *usernameLayout = new QHBoxLayout(usernameRow_);
    usernameLayout->setContentsMargins(0, 0, 0, 0);
    usernameLayout->addWidget(usernameEdit_, 1);
    usernameLayout->addWidget(copyUsernameButton_);
    urlEdit_ = new QLineEdit(editor);
    urlEdit_->setObjectName(QStringLiteral("vaultUrlEdit"));
    openUrlButton_ = new QPushButton(tr("打开"), editor);
    openUrlButton_->setObjectName(QStringLiteral("vaultOpenUrlButton"));
    urlRow_ = new QWidget(editor);
    auto *urlLayout = new QHBoxLayout(urlRow_);
    urlLayout->setContentsMargins(0, 0, 0, 0);
    urlLayout->addWidget(urlEdit_, 1);
    urlLayout->addWidget(openUrlButton_);
    tagsEdit_ = new QLineEdit(editor);
    tagsEdit_->setObjectName(QStringLiteral("vaultTagsEdit"));
    tagsEdit_->setPlaceholderText(tr("多个标签用逗号分隔"));
    groupEdit_ = new QLineEdit(editor);
    groupEdit_->setObjectName(QStringLiteral("vaultGroupEdit"));
    groupEdit_->setPlaceholderText(tr("例如：工作/生产环境"));
    additionalUrlsEdit_ = new QPlainTextEdit(editor);
    additionalUrlsEdit_->setObjectName(QStringLiteral("vaultAdditionalUrlsEdit"));
    additionalUrlsEdit_->setPlaceholderText(tr("每行一个备用网址"));
    additionalUrlsEdit_->setMaximumHeight(72);
    customFieldsEdit_ = new QPlainTextEdit(editor);
    customFieldsEdit_->setObjectName(QStringLiteral("vaultCustomFieldsEdit"));
    customFieldsEdit_->setPlaceholderText(
        tr("每行 name=value；默认加密保护，public:name=value 表示公开字段"));
    customFieldsEdit_->setMaximumHeight(92);
    expiryCheck_ = new QCheckBox(tr("此账号会过期"), editor);
    expiryCheck_->setObjectName(QStringLiteral("vaultExpiryCheck"));
    expiryEdit_ = new QDateTimeEdit(editor);
    expiryEdit_->setObjectName(QStringLiteral("vaultExpiryEdit"));
    expiryEdit_->setCalendarPopup(true);
    expiryEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    expiryEdit_->setDateTime(QDateTime::currentDateTime().addMonths(3));
    expiryRow_ = new QWidget(editor);
    auto *expiryLayout = new QHBoxLayout(expiryRow_);
    expiryLayout->setContentsMargins(0, 0, 0, 0);
    expiryLayout->addWidget(expiryCheck_);
    expiryLayout->addWidget(expiryEdit_, 1);
    passwordEdit_ = new QLineEdit(editor);
    passwordEdit_->setObjectName(QStringLiteral("vaultPasswordEdit"));
    passwordEdit_->setEchoMode(QLineEdit::Password);
    copyPasswordButton_ = new QPushButton(tr("复制"), editor);
    copyPasswordButton_->setObjectName(QStringLiteral("vaultCopyPasswordButton"));
    copyPasswordButton_->setToolTip(tr("复制密码；30 秒后自动清除剪贴板"));
    generatePasswordButton_ = new QPushButton(tr("生成"), editor);
    generatePasswordButton_->setObjectName(QStringLiteral("vaultGeneratePasswordButton"));
    generatePasswordButton_->setToolTip(tr("配置并使用 KeePassXC 密码生成器"));
    totpEdit_ = new QLineEdit(editor);
    totpEdit_->setObjectName(QStringLiteral("vaultTotpEdit"));
    totpEdit_->setEchoMode(QLineEdit::Password);
    totpInputRow_ = new QWidget(editor);
    auto *totpInputLayout = new QHBoxLayout(totpInputRow_);
    totpInputLayout->setContentsMargins(0, 0, 0, 0);
    totpInputLayout->addWidget(totpEdit_, 1);
    totpConfigureButton_ = new QPushButton(tr("设置…"), totpInputRow_);
    totpConfigureButton_->setObjectName(QStringLiteral("vaultTotpConfigureButton"));
    totpInputLayout->addWidget(totpConfigureButton_);
    showPasswordCheck_ = new QCheckBox(tr("显示密码"), editor);
    showPasswordCheck_->setObjectName(QStringLiteral("vaultShowPasswordCheck"));
    notesEdit_ = new QTextEdit(editor);
    notesEdit_->setObjectName(QStringLiteral("vaultNotesEdit"));
    notesEdit_->setAcceptRichText(false);
    notesEdit_->setMinimumHeight(110);

    usernameLabel_ = new QLabel(tr("用户名"), editor);
    formLayout_->addRow(usernameLabel_, usernameRow_);
    urlLabel_ = new QLabel(tr("网址"), editor);
    formLayout_->addRow(urlLabel_, urlRow_);
    tagsLabel_ = new QLabel(tr("标签"), editor);
    formLayout_->addRow(tagsLabel_, tagsEdit_);
    formLayout_->addRow(tr("分组"), groupEdit_);
    formLayout_->addRow(tr("备用网址"), additionalUrlsEdit_);
    formLayout_->addRow(tr("自定义字段"), customFieldsEdit_);
    formLayout_->addRow(tr("到期"), expiryRow_);
    passwordLabel_ = new QLabel(tr("密码"), editor);
    passwordRow_ = new QWidget(editor);
    auto *passwordLayout = new QHBoxLayout(passwordRow_);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->addWidget(passwordEdit_);
    passwordLayout->addWidget(showPasswordCheck_);
    passwordLayout->addWidget(generatePasswordButton_);
    passwordLayout->addWidget(copyPasswordButton_);
    formLayout_->addRow(passwordLabel_, passwordRow_);
    passwordStrengthCaption_ = new QLabel(tr("密码强度"), editor);
    passwordStrengthLabel_ = new QLabel(tr("未输入"), editor);
    passwordStrengthLabel_->setObjectName(QStringLiteral("vaultPasswordStrengthLabel"));
    formLayout_->addRow(passwordStrengthCaption_, passwordStrengthLabel_);
    totpLabel_ = new QLabel(tr("验证码密钥"), editor);
    formLayout_->addRow(totpLabel_, totpInputRow_);
    totpCodeRow_ = new QWidget(editor);
    auto *totpCodeLayout = new QHBoxLayout(totpCodeRow_);
    totpCodeLayout->setContentsMargins(0, 0, 0, 0);
    totpCodeLabel_ = new QLabel(tr("未设置"), totpCodeRow_);
    totpCodeLabel_->setObjectName(QStringLiteral("vaultTotpCodeLabel"));
    QFont totpCodeFont = totpCodeLabel_->font();
    totpCodeFont.setBold(true);
    totpCodeLabel_->setFont(totpCodeFont);
    totpCodeLabel_->setMinimumWidth(
        totpCodeLabel_->fontMetrics().horizontalAdvance(tr("888 888  ·  30 秒")) + 12);
    copyTotpButton_ = new QPushButton(tr("复制验证码"), totpCodeRow_);
    copyTotpButton_->setObjectName(QStringLiteral("vaultCopyTotpButton"));
    copyTotpButton_->setToolTip(tr("复制当前验证码；30 秒后自动清除剪贴板"));
    totpCodeLayout->addWidget(totpCodeLabel_);
    totpCodeLayout->addStretch();
    totpCodeLayout->addWidget(copyTotpButton_);
    totpCodeCaption_ = new QLabel(tr("当前验证码"), editor);
    formLayout_->addRow(totpCodeCaption_, totpCodeRow_);
    // Keep account metadata terminology distinct from document notes. This is
    // the conventional password-manager "Notes" field, rendered as 备注 in UI.
    formLayout_->addRow(tr("备注"), notesEdit_);
    editorLayout->addLayout(formLayout_);

    editorLayout->addStretch();
    editorScroll->setWidget(editor);
    detailPages_->addWidget(editorScroll);
    detailPages_->setCurrentWidget(detailEmptyPage);
    splitter->addWidget(detailPages_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({420, 700});
    contentPages_->addWidget(splitter);
    contentPages_->setCurrentWidget(firstAccountPage);
    openLayout->addWidget(contentPages_);
    pages_->addWidget(openPage_);
    pages_->setCurrentWidget(lockedPage_);

    clipboardClearTimer_ = new QTimer(this);
    clipboardClearTimer_->setSingleShot(true);
    passwordRevealTimer_ = new QTimer(this);
    passwordRevealTimer_->setObjectName(QStringLiteral("vaultPasswordRevealTimer"));
    passwordRevealTimer_->setSingleShot(true);
    totpRefreshTimer_ = new QTimer(this);
    totpRefreshTimer_->setObjectName(QStringLiteral("vaultTotpRefreshTimer"));
    totpRefreshTimer_->setInterval(1000);

    setWidget(container);
    setEditorEnabled(false);
    setVaultOpenControls(false);

    connect(openButton, &QPushButton::clicked, this, &VaultDock::openVaultInteractive);
    connect(newVaultButton, &QPushButton::clicked, this, &VaultDock::newVaultInteractive);
    connect(lockedOpenButton, &QPushButton::clicked, this, &VaultDock::openVaultInteractive);
    connect(lockButton_, &QPushButton::clicked, this, &VaultDock::requestLock);
    connect(newLoginButton_, &QPushButton::clicked, this, &VaultDock::createLogin);
    connect(emptyCreateButton_, &QPushButton::clicked, this, &VaultDock::createLogin);
    connect(newNoteButton_, &QPushButton::clicked, this, &VaultDock::createSecureNote);
    connect(filterEdit_, &QLineEdit::textChanged, this, &VaultDock::applyFilter);
    connect(sortCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshEntries(currentEntryId_);
    });
    connect(healthButton_, &QPushButton::clicked,
            this, &VaultDock::showPasswordHealthReport);
    connect(entryTree_, &QTreeWidget::currentItemChanged,
            this, &VaultDock::onCurrentItemChanged);
    connect(entryTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        const QUuid id(item->data(0, EntryIdRole).toString());
        if (id.isNull() || item->data(0, EntryRecycledRole).toBool())
            return;
        VaultError error;
        auto entry = service_.readEntry(id, &error);
        if (entry && entry->tags.contains(VaultSecureNoteTag))
            emit secureNoteOpenRequested(id);
    });
    connect(saveButton_, &QPushButton::clicked, this, &VaultDock::saveCurrentEntry);
    connect(editButton_, &QPushButton::clicked, this, &VaultDock::editCurrentEntry);
    connect(historyButton_, &QPushButton::clicked,
            this, &VaultDock::showEntryHistory);
    connect(cloneButton_, &QPushButton::clicked,
            this, &VaultDock::cloneCurrentEntry);
    connect(cancelButton_, &QPushButton::clicked, this, &VaultDock::cancelCurrentEdit);
    connect(copyUsernameButton_, &QPushButton::clicked, this, &VaultDock::copyUsername);
    connect(openUrlButton_, &QPushButton::clicked, this, &VaultDock::openCurrentUrl);
    connect(restoreButton_, &QPushButton::clicked, this, &VaultDock::restoreCurrentEntry);
    connect(deleteButton_, &QPushButton::clicked, this, &VaultDock::removeCurrentEntry);
    connect(showPasswordCheck_, &QCheckBox::toggled,
            this, &VaultDock::updatePasswordEcho);
    connect(copyPasswordButton_, &QPushButton::clicked,
            this, &VaultDock::copyPasswordProtected);
    connect(generatePasswordButton_, &QPushButton::clicked,
            this, &VaultDock::generatePassword);
    connect(copyTotpButton_, &QPushButton::clicked,
            this, &VaultDock::copyTotpProtected);
    connect(totpConfigureButton_, &QPushButton::clicked,
            this, &VaultDock::configureTotp);
    connect(clipboardClearTimer_, &QTimer::timeout,
            this, &VaultDock::clearProtectedClipboard);
    connect(passwordRevealTimer_, &QTimer::timeout, this, &VaultDock::hidePassword);
    connect(totpRefreshTimer_, &QTimer::timeout, this, &VaultDock::refreshTotpCode);
    connect(passwordEdit_, &QLineEdit::editingFinished, this, &VaultDock::hidePassword);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
            hidePassword();
    });

    const auto markDirty = [this]() {
        if (!loading_)
            setDirty();
    };
    // loading_ suppresses model-to-view population, so textChanged also covers
    // edits made by paste, password generation and future form commands.
    connect(titleEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(usernameEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(urlEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(tagsEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(groupEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(additionalUrlsEdit_, &QPlainTextEdit::textChanged, this, markDirty);
    connect(customFieldsEdit_, &QPlainTextEdit::textChanged, this, markDirty);
    connect(expiryCheck_, &QCheckBox::toggled, this, [this, markDirty](bool enabled) {
        expiryEdit_->setEnabled(enabled && editorEditing_);
        markDirty();
    });
    connect(expiryEdit_, &QDateTimeEdit::dateTimeChanged, this,
            [markDirty](const QDateTime &) { markDirty(); });
    connect(passwordEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(passwordEdit_, &QLineEdit::textEdited, this, [this] {
        updatePasswordStrength();
        if (editorEditing_)
            copyPasswordButton_->setEnabled(!passwordEdit_->text().isEmpty());
    });
    connect(totpEdit_, &QLineEdit::textChanged, this, markDirty);
    connect(totpEdit_, &QLineEdit::textEdited, this, [this] {
        currentTotpCode_.clear();
        totpCodeLabel_->setText(totpEdit_->text().isEmpty() ? tr("未设置")
                                                            : tr("保存后显示"));
        copyTotpButton_->setEnabled(false);
        totpRefreshTimer_->stop();
    });
    connect(notesEdit_, &QTextEdit::textChanged, this, markDirty);
}

VaultError VaultDock::createVault(const QString &filePath,
                                  const SecureBuffer &masterPasswordUtf8,
                                  const QString &keyFilePath)
{
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};

    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在创建账户数据库"),
        [this, filePath, &masterPasswordUtf8, keyFilePath] {
            return service_.create(filePath, masterPasswordUtf8, keyFilePath);
        });
    if (error.isError()) {
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }

    applyOpenState();
    return VaultError::success();
}

VaultError VaultDock::unlockVault(const QString &filePath,
                                  const SecureBuffer &masterPasswordUtf8,
                                  const QString &keyFilePath)
{
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};

    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在解锁账户数据库"),
        [this, filePath, &masterPasswordUtf8, keyFilePath] {
            return service_.unlock(filePath, masterPasswordUtf8, keyFilePath);
        });
    if (error.isError()) {
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }

    applyOpenState();
    return VaultError::success();
}

VaultError VaultDock::createManagedStore(const QString &filePath,
                                         const SecureBuffer &securityPassphraseUtf8,
                                         QString *recoveryKey)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(securityPassphraseUtf8)
    Q_UNUSED(recoveryKey)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    if (!recoveryKey)
        return {VaultErrorCode::InternalError, tr("恢复密钥输出参数无效。"),
                QStringLiteral("identity.create.output")};
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};

    SecureBuffer databaseCredential;
    QVariantMap publicCustomData;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在创建受保护账户数据库"),
        [this, filePath, &securityPassphraseUtf8, &databaseCredential,
         recoveryKey, &publicCustomData] {
            VaultError result = SecurityIdentity::create(
                securityPassphraseUtf8, &databaseCredential, recoveryKey,
                &publicCustomData);
            if (!result.isError())
                result = service_.create(filePath, databaseCredential, {}, publicCustomData);
            return result;
        });
    databaseCredential.clear();
    if (error.isError()) {
        recoveryKey->fill(QChar(0));
        recoveryKey->clear();
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }
    rememberManagedStore(filePath);
    applyOpenState();
    return VaultError::success();
#endif
}

VaultError VaultDock::unlockManagedStore(const QString &filePath,
                                         const SecureBuffer &securityPassphraseUtf8)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(securityPassphraseUtf8)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    QVariantMap publicCustomData;
    SecureBuffer databaseCredential;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在解析账户数据库密钥"),
        [this, filePath, &publicCustomData, &securityPassphraseUtf8,
         &databaseCredential] {
            VaultError result = service_.readPublicCustomData(filePath, &publicCustomData);
            if (!result.isError())
                result = SecurityIdentity::unlock(publicCustomData,
                                                   securityPassphraseUtf8,
                                                   &databaseCredential);
            return result;
        });
    if (!error.isError())
        error = unlockVault(filePath, databaseCredential);
    databaseCredential.clear();
    if (!error.isError())
        rememberManagedStore(filePath);
    return error;
#endif
}

VaultError VaultDock::createManagedStoreForIdentity(
    const QString &filePath,
    const SecurityIdentityParameters &parameters,
    const SecureBuffer &masterUnlockKey,
    const SecureBuffer &recoverySecret)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(parameters)
    Q_UNUSED(masterUnlockKey)
    Q_UNUSED(recoverySecret)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    if (!maybeSaveChanges())
        return {VaultErrorCode::Cancelled, tr("操作已取消。"), QStringLiteral("vault.ui.cancelled")};
    SecureBuffer databaseCredential;
    QVariantMap publicCustomData;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在创建账户数据库"),
        [this, filePath, &parameters, &masterUnlockKey, &recoverySecret,
         &databaseCredential, &publicCustomData] {
            VaultError result = SecurityIdentity::createForIdentity(
                parameters, masterUnlockKey, recoverySecret, &databaseCredential,
                &publicCustomData, QStringLiteral("Accounts"));
            if (!result.isError())
                result = service_.create(filePath, databaseCredential, {}, publicCustomData);
            return result;
        });
    databaseCredential.clear();
    if (error.isError()) {
        pages_->setCurrentWidget(lockedPage_);
        setVaultOpenControls(false);
        return error;
    }
    rememberManagedStore(filePath);
    applyOpenState();
    return VaultError::success();
#endif
}

VaultError VaultDock::unlockManagedStoreWithMasterKey(
    const QString &filePath, const SecureBuffer &masterUnlockKey)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(masterUnlockKey)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    QVariantMap publicCustomData;
    SecureBuffer databaseCredential;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在解析账户数据库密钥"),
        [this, filePath, &publicCustomData, &masterUnlockKey,
         &databaseCredential] {
            VaultError result = service_.readPublicCustomData(filePath, &publicCustomData);
            if (!result.isError())
                result = SecurityIdentity::unlockWithMasterKey(
                    publicCustomData, masterUnlockKey, &databaseCredential);
            return result;
        });
    if (!error.isError())
        error = unlockVault(filePath, databaseCredential);
    databaseCredential.clear();
    if (!error.isError())
        rememberManagedStore(filePath);
    return error;
#endif
}

VaultError VaultDock::unlockManagedStoreWithRecoverySecret(
    const QString &filePath, const SecureBuffer &recoverySecret)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(recoverySecret)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    QVariantMap publicCustomData;
    SecureBuffer databaseCredential;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在恢复账户数据库密钥"),
        [this, filePath, &publicCustomData, &recoverySecret,
         &databaseCredential] {
            VaultError result = service_.readPublicCustomData(filePath, &publicCustomData);
            if (!result.isError())
                result = SecurityIdentity::recoverWithSecret(
                    publicCustomData, recoverySecret, &databaseCredential);
            return result;
        });
    if (!error.isError())
        error = unlockVault(filePath, databaseCredential);
    databaseCredential.clear();
    if (!error.isError())
        rememberManagedStore(filePath);
    return error;
#endif
}

QString VaultDock::preferredManagedStorePath() const
{
    const auto settings = createSecuritySettings();
    const QString remembered = settings->value(QStringLiteral("security/managedStorePath"))
                                   .toString().trimmed();
    if (!remembered.isEmpty())
        return remembered;
    const QString isolatedDataDir = QProcessEnvironment::systemEnvironment()
                                        .value(QStringLiteral("NOTEPADSAFE_DATADIR")).trimmed();
    if (!isolatedDataDir.isEmpty() && QFileInfo(isolatedDataDir).isAbsolute())
        return QDir(isolatedDataDir).filePath(QStringLiteral("accounts.kdbx"));
    return defaultSecureStorePath();
}

VaultError VaultDock::recoverManagedStore(const QString &filePath,
                                          const QString &recoveryKey)
{
#ifndef NOTEPADSAFE_HAS_KDBX_BACKEND
    Q_UNUSED(filePath)
    Q_UNUSED(recoveryKey)
    return {VaultErrorCode::BackendUnavailable, tr("当前构建未包含生产 KDBX 后端。"),
            QStringLiteral("identity.backend.unavailable")};
#else
    QVariantMap publicCustomData;
    SecureBuffer databaseCredential;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在恢复账户数据库"),
        [this, filePath, &publicCustomData, &recoveryKey, &databaseCredential] {
            VaultError result = service_.readPublicCustomData(filePath, &publicCustomData);
            if (!result.isError())
                result = SecurityIdentity::recover(publicCustomData, recoveryKey,
                                                   &databaseCredential);
            return result;
        });
    if (!error.isError())
        error = unlockVault(filePath, databaseCredential);
    databaseCredential.clear();
    if (!error.isError())
        rememberManagedStore(filePath);
    return error;
#endif
}

std::unique_ptr<VaultEntryData> VaultDock::readSecureDocument(const QUuid &id,
                                                              VaultError *error) const
{
    return service_.readEntry(id, error);
}

VaultError VaultDock::saveSecureDocument(const QUuid &id, const QString &title,
                                         const QString &plainText)
{
    if (service_.state() != VaultState::Open)
        return {VaultErrorCode::Locked, tr("加密文档当前已锁定。"),
                QStringLiteral("secure-document.locked")};
    QByteArray body = plainText.toUtf8();
    if (body.size() > SecureNoteMaximumBytes) {
        scrubByteArray(body);
        return {VaultErrorCode::IoError,
                tr("加密笔记不能超过 32 MiB。请将大文件保存为明文文件，或只保存敏感摘要。"),
                QStringLiteral("secure-document.size-limit")};
    }
    VaultEntryData data;
    data.id = id;
    data.title = title.trimmed().isEmpty() ? tr("未命名加密笔记") : title.trimmed();
    data.tags = {VaultSecureNoteTag};
    data.notesUtf8.assign(body.constData(), body.size());
    scrubByteArray(body);
    auto pendingData = std::make_shared<VaultEntryData>(std::move(data));
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
        tr("正在保存加密笔记"),
        [this, pendingData]() mutable {
            VaultError result = service_.upsertEntry(std::move(*pendingData));
            if (!result.isError())
                result = service_.save();
            return result;
        });
    if (!error.isError())
        refreshEntries(id);
    return error;
}

void VaultDock::applyOpenState()
{
    clearEditor();
    pages_->setCurrentWidget(openPage_);
    setVaultOpenControls(true);
    refreshEntries();
}

void VaultDock::rememberManagedStore(const QString &filePath)
{
    auto settings = createSecuritySettings();
    settings->setValue(QStringLiteral("security/managedStorePath"), filePath);
}

void VaultDock::newVaultInteractive()
{
    if (!backendAvailable_) {
        showError({VaultErrorCode::BackendUnavailable,
                   tr("当前构建未包含生产 KDBX 后端。"),
                   QStringLiteral("vault.backend.unavailable")},
                  tr("无法建立加密数据"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("建立本地安全身份"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        tr("新建笔记将默认加密保存。安全口令只负责解锁随机数据密钥；"
           "应用不会为每篇笔记重复询问密码。"), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *form = new QFormLayout();
    auto *pathEdit = new QLineEdit(defaultSecureStorePath(), &dialog);
    pathEdit->setObjectName(QStringLiteral("securityStorePath"));
    auto *pathRow = new QWidget(&dialog);
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    auto *browsePath = new QPushButton(tr("更改…"), pathRow);
    pathLayout->addWidget(pathEdit);
    pathLayout->addWidget(browsePath);
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("vaultCreatePassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(tr("建议使用多个随机词组成的长口令"));
    auto *confirmation = new QLineEdit(&dialog);
    confirmation->setObjectName(QStringLiteral("vaultCreatePasswordConfirm"));
    confirmation->setEchoMode(QLineEdit::Password);
    form->addRow(tr("加密数据位置"), pathRow);
    form->addRow(tr("安全口令"), password);
    form->addRow(tr("确认安全口令"), confirmation);
    layout->addLayout(form);
    auto *warning = new QLabel(
        tr("下一步会生成独立恢复密钥。即使应用配置或 Windows Hello 数据损坏，"
           "它仍可直接恢复加密数据。"), &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("创建"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browsePath, &QPushButton::clicked, &dialog, [this, pathEdit]() {
        const QString selected = QFileDialog::getSaveFileName(
            this, tr("选择加密数据位置"), pathEdit->text(), tr("KeePass 数据库 (*.kdbx)"));
        if (!selected.isEmpty())
            pathEdit->setText(selected);
    });

    while (dialog.exec() == QDialog::Accepted) {
        QString filePath = pathEdit->text().trimmed();
        if (filePath.isEmpty()) {
            QMessageBox::warning(&dialog, tr("无法创建"), tr("请选择加密数据位置。"));
            pathEdit->setFocus();
            continue;
        }
        if (!filePath.endsWith(QStringLiteral(".kdbx"), Qt::CaseInsensitive))
            filePath += QStringLiteral(".kdbx");
        if (QFileInfo::exists(filePath)) {
            QMessageBox::warning(&dialog, tr("无法创建"),
                                 tr("请选择一个尚不存在的 KDBX 文件。"));
            pathEdit->setFocus();
            continue;
        }
        if (password->text().size() < 12) {
            QMessageBox::warning(&dialog, tr("安全口令过短"),
                                 tr("安全口令至少需要 12 个字符；推荐使用多个随机词组成的长口令。"));
            password->setFocus();
            continue;
        }
        if (password->text() != confirmation->text()) {
            QMessageBox::warning(&dialog, tr("无法创建"), tr("两次输入的主密码不一致。"));
            clearSensitiveLineEdit(confirmation);
            confirmation->setFocus();
            continue;
        }

        const QFileInfo target(filePath);
        if (!QDir().mkpath(target.absolutePath())) {
            QMessageBox::warning(&dialog, tr("无法创建"), tr("无法创建目标目录。"));
            continue;
        }
        QString passwordText = takeSensitiveLineEdit(password);
        clearSensitiveLineEdit(confirmation);
        SecureBuffer passwordBuffer = secureUtf8From(passwordText);
        wipeQString(passwordText);
        QString recoveryKey;
        VaultError error = createManagedStore(filePath, passwordBuffer, &recoveryKey);
        passwordBuffer.clear();
        if (!error.isError()) {
            dialog.accept();
            showRecoveryKey(this, recoveryKey);
            wipeQString(recoveryKey);
            return;
        }
        showError(error, tr("创建失败"));
        password->setFocus();
    }
    clearSensitiveLineEdit(password);
    clearSensitiveLineEdit(confirmation);
}

void VaultDock::openVaultInteractive()
{
    if (!backendAvailable_) {
        showError({VaultErrorCode::BackendUnavailable,
                   tr("当前构建未包含生产 KDBX 后端。"),
                   QStringLiteral("vault.backend.unavailable")},
                  tr("无法打开加密数据"));
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("打开加密数据"), QString(), tr("KeePass 数据库 (*.kdbx);;所有文件 (*)"));
    if (filePath.isEmpty())
        return;

#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    QVariantMap publicCustomData;
    const VaultError publicDataError = service_.readPublicCustomData(filePath, &publicCustomData);
    if (!publicDataError.isError()
        && publicCustomData.contains(SecurityIdentity::publicDataKey())) {
        QDialog identityDialog(this);
        identityDialog.setWindowTitle(tr("解锁加密文档"));
        auto *identityLayout = new QVBoxLayout(&identityDialog);
        auto *identityForm = new QFormLayout();
        auto *pathLabel = new QLabel(filePath, &identityDialog);
        pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *passphrase = new QLineEdit(&identityDialog);
        passphrase->setObjectName(QStringLiteral("securityUnlockPassphrase"));
        passphrase->setEchoMode(QLineEdit::Password);
        auto *recovery = new QLineEdit(&identityDialog);
        recovery->setObjectName(QStringLiteral("securityUnlockRecovery"));
        recovery->setPlaceholderText(tr("仅在忘记口令或文件密钥信息损坏时使用"));
        identityForm->addRow(tr("加密数据"), pathLabel);
        identityForm->addRow(tr("安全口令"), passphrase);
        identityForm->addRow(tr("恢复密钥（可选）"), recovery);
        identityLayout->addLayout(identityForm);
        auto *hint = new QLabel(
            tr("正常使用只需输入安全口令。填写恢复密钥时将忽略安全口令。"),
            &identityDialog);
        hint->setWordWrap(true);
        identityLayout->addWidget(hint);
        auto *identityButtons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &identityDialog);
        identityButtons->button(QDialogButtonBox::Ok)->setText(tr("解锁"));
        identityButtons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
        identityLayout->addWidget(identityButtons);
        connect(identityButtons, &QDialogButtonBox::accepted,
                &identityDialog, &QDialog::accept);
        connect(identityButtons, &QDialogButtonBox::rejected,
                &identityDialog, &QDialog::reject);

        while (identityDialog.exec() == QDialog::Accepted) {
            VaultError error;
            if (!recovery->text().trimmed().isEmpty()) {
                QString rawRecovery = takeSensitiveLineEdit(recovery);
                QString recoveryText = rawRecovery.trimmed();
                wipeQString(rawRecovery);
                clearSensitiveLineEdit(passphrase);
                error = recoverManagedStore(filePath, recoveryText);
                wipeQString(recoveryText);
            } else {
                QString passphraseText = takeSensitiveLineEdit(passphrase);
                clearSensitiveLineEdit(recovery);
                SecureBuffer passphraseBuffer = secureUtf8From(passphraseText);
                wipeQString(passphraseText);
                error = unlockManagedStore(filePath, passphraseBuffer);
                passphraseBuffer.clear();
            }
            if (!error.isError())
                return;
            showError(error, tr("解锁失败"));
            passphrase->setFocus();
        }
        clearSensitiveLineEdit(passphrase);
        clearSensitiveLineEdit(recovery);
        return;
    }
#endif

    QDialog dialog(this);
    dialog.setWindowTitle(tr("打开外部 KDBX"));
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
    form->addRow(tr("KDBX 文件"), pathLabel);
    form->addRow(tr("主密码"), password);
    form->addRow(tr("密钥文件（可选）"), keyRow);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("解锁"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseKey, &QPushButton::clicked, &dialog, [this, keyFile]() {
        const QString selected = QFileDialog::getOpenFileName(this, tr("选择密钥文件"));
        if (!selected.isEmpty())
            keyFile->setText(selected);
    });

    while (dialog.exec() == QDialog::Accepted) {
        QString passwordText = takeSensitiveLineEdit(password);
        SecureBuffer passwordBuffer = secureUtf8From(passwordText);
        wipeQString(passwordText);
        VaultError error = unlockVault(filePath, passwordBuffer, keyFile->text());
        passwordBuffer.clear();
        if (!error.isError())
            return;
        showError(error, tr("解锁失败"));
        password->setFocus();
    }
    clearSensitiveLineEdit(password);
}

bool VaultDock::ensureSecureStoreReadyInteractive()
{
    if (service_.state() == VaultState::Open)
        return true;
    if (!backendAvailable_) {
        showError({VaultErrorCode::BackendUnavailable,
                   tr("当前构建未包含生产 KDBX 后端。"),
                   QStringLiteral("vault.backend.unavailable")},
                  tr("无法使用加密文档"));
        return false;
    }

    auto settings = createSecuritySettings();
    const QString filePath = settings->value(
        QStringLiteral("security/managedStorePath")).toString();
#ifdef NOTEPADSAFE_HAS_KDBX_BACKEND
    QVariantMap publicCustomData;
    const VaultError publicDataError = filePath.isEmpty()
                                           ? VaultError{VaultErrorCode::IoError, {}, {}}
                                           : service_.readPublicCustomData(filePath,
                                                                           &publicCustomData);
    if (!filePath.isEmpty() && QFileInfo::exists(filePath)
        && !publicDataError.isError()
        && publicCustomData.contains(SecurityIdentity::publicDataKey())) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("解锁加密文档"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *path = new QLabel(filePath, &dialog);
        path->setWordWrap(true);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(path);
        auto *passphrase = new QLineEdit(&dialog);
        passphrase->setObjectName(QStringLiteral("securityDefaultPassphrase"));
        passphrase->setEchoMode(QLineEdit::Password);
        passphrase->setPlaceholderText(tr("安全口令"));
        layout->addWidget(passphrase);
        auto *recovery = new QLineEdit(&dialog);
        recovery->setObjectName(QStringLiteral("securityDefaultRecovery"));
        recovery->setPlaceholderText(tr("恢复密钥（仅在恢复时填写）"));
        layout->addWidget(recovery);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("解锁"));
        buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        while (dialog.exec() == QDialog::Accepted) {
            VaultError error;
            if (!recovery->text().trimmed().isEmpty()) {
                QString rawRecovery = takeSensitiveLineEdit(recovery);
                QString recoveryText = rawRecovery.trimmed();
                wipeQString(rawRecovery);
                clearSensitiveLineEdit(passphrase);
                error = recoverManagedStore(filePath, recoveryText);
                wipeQString(recoveryText);
            } else {
                QString text = takeSensitiveLineEdit(passphrase);
                clearSensitiveLineEdit(recovery);
                SecureBuffer buffer = secureUtf8From(text);
                wipeQString(text);
                error = unlockManagedStore(filePath, buffer);
                buffer.clear();
            }
            if (!error.isError())
                return true;
            showError(error, tr("解锁失败"));
            passphrase->setFocus();
        }
        clearSensitiveLineEdit(passphrase);
        clearSensitiveLineEdit(recovery);
        return false;
    }
#endif

    const auto choice = QMessageBox::question(
        this, tr("建立加密文档存储"),
        tr("尚未建立默认加密存储。现在创建本地安全身份吗？\n\n"
           "加密数据默认保存在“文档\\Notepad Safe”中，不依赖应用配置目录。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes)
        return false;
    newVaultInteractive();
    return service_.state() == VaultState::Open;
}

void VaultDock::lockVault()
{
    if (service_.state() == VaultState::Locked)
        return;
    if (!maybeSaveChanges())
        return;
    clearProtectedClipboard();
    lockServiceOnSecurityLane();
    entryTree_->clear();
    loginRoot_ = nullptr;
    noteRoot_ = nullptr;
    recycleRoot_ = nullptr;
    clearEditor();
    pages_->setCurrentWidget(lockedPage_);
    setVaultOpenControls(false);
    emit secureStoreLocked();
}

bool VaultDock::prepareToLock()
{
    return maybeSaveChanges();
}

bool VaultDock::lockVaultSilently()
{
    if (service_.state() == VaultState::Locked) {
        clearProtectedClipboard();
        return true;
    }
    if (editorDirty_ && !saveCurrentEntryImpl(false))
        return false;
    clearProtectedClipboard();
    lockServiceOnSecurityLane();
    entryTree_->clear();
    loginRoot_ = nullptr;
    noteRoot_ = nullptr;
    recycleRoot_ = nullptr;
    clearEditor();
    pages_->setCurrentWidget(lockedPage_);
    setVaultOpenControls(false);
    emit secureStoreLocked();
    return true;
}

bool VaultDock::releaseWorkspaceSecrets()
{
    if (service_.state() == VaultState::Locked) {
        clearProtectedClipboard();
        return true;
    }
    if (editorDirty_ && !saveCurrentEntryImpl(false))
        return false;
    clearProtectedClipboard();
    lockServiceOnSecurityLane();
    entryTree_->clear();
    loginRoot_ = nullptr;
    noteRoot_ = nullptr;
    recycleRoot_ = nullptr;
    clearEditor();
    pages_->setCurrentWidget(lockedPage_);
    setVaultOpenControls(false);
    return true;
}

void VaultDock::discardAndLockForExternalReload()
{
    editorDirty_ = false;
    editorEditing_ = false;
    clearProtectedClipboard();
    lockServiceOnSecurityLane();
    entryTree_->clear();
    loginRoot_ = nullptr;
    noteRoot_ = nullptr;
    recycleRoot_ = nullptr;
    clearEditor();
    pages_->setCurrentWidget(lockedPage_);
    setVaultOpenControls(false);
}

void VaultDock::requestLock()
{
    if (isSignalConnected(QMetaMethod::fromSignal(&VaultDock::lockRequested)))
        emit lockRequested();
    else
        lockVault();
}

bool VaultDock::prepareToClose()
{
    if (!maybeSaveChanges())
        return false;
    lockServiceOnSecurityLane();
    clearEditor();
    setVaultOpenControls(false);
    return true;
}

void VaultDock::refreshEntries(const QUuid &selectId)
{
    QSignalBlocker blocker(entryTree_);
    entryTree_->clear();
    loginRoot_ = new QTreeWidgetItem(entryTree_, {tr("账号"), QString()});
    noteRoot_ = new QTreeWidgetItem(entryTree_, {tr("旧版笔记（迁移）"), QString()});
    recycleRoot_ = new QTreeWidgetItem(entryTree_, {tr("回收站"), QString()});
    loginRoot_->setFlags(loginRoot_->flags() & ~Qt::ItemIsSelectable);
    noteRoot_->setFlags(noteRoot_->flags() & ~Qt::ItemIsSelectable);
    recycleRoot_->setFlags(recycleRoot_->flags() & ~Qt::ItemIsSelectable);
    loginRoot_->setExpanded(true);
    noteRoot_->setExpanded(true);
    recycleRoot_->setExpanded(false);

    VaultError error;
    auto entries = service_.listEntries(&error);
    if (error.isError()) {
        showError(error, tr("无法读取账户文件"));
        return;
    }

    const QString sortMode = sortCombo_ ? sortCombo_->currentData().toString()
                                        : QStringLiteral("title");
    const auto compareEntries = [sortMode](const VaultEntrySummary &left,
                                           const VaultEntrySummary &right) {
        QString leftKey = left.title;
        QString rightKey = right.title;
        if (sortMode == QStringLiteral("username")) {
            leftKey = left.username;
            rightKey = right.username;
        } else if (sortMode == QStringLiteral("group")) {
            leftKey = left.groupPath;
            rightKey = right.groupPath;
        }
        const int primary = QString::localeAwareCompare(leftKey, rightKey);
        return primary == 0
            ? QString::localeAwareCompare(left.title, right.title) < 0
            : primary < 0;
    };
    std::sort(entries.begin(), entries.end(), compareEntries);
    QHash<QString, QTreeWidgetItem *> groupItems;
    const auto loginParentFor = [&](const QString &groupPath) {
        QTreeWidgetItem *parent = loginRoot_;
        QString cumulative;
        const QStringList parts = groupPath.split(
            QRegularExpression(QStringLiteral("[/\\\\]+")), Qt::SkipEmptyParts);
        for (const QString &rawPart : parts) {
            const QString part = rawPart.trimmed();
            if (part.isEmpty())
                continue;
            cumulative += (cumulative.isEmpty() ? QString() : QStringLiteral("/")) + part;
            QTreeWidgetItem *groupItem = groupItems.value(cumulative, nullptr);
            if (!groupItem) {
                groupItem = new QTreeWidgetItem(parent, {part, tr("分组")});
                groupItem->setFlags(groupItem->flags() & ~Qt::ItemIsSelectable);
                groupItem->setExpanded(true);
                groupItem->setToolTip(0, cumulative);
                groupItems.insert(cumulative, groupItem);
            }
            parent = groupItem;
        }
        return parent;
    };
    QTreeWidgetItem *toSelect = nullptr;
    int expiredCount = 0;
    for (const auto &entry : entries) {
        auto *parent = entry.secureNote ? noteRoot_ : loginParentFor(entry.groupPath);
        QString detail = entry.secureNote ? tr("安全笔记") : entry.username;
        if (detail.isEmpty())
            detail = entry.url;
        const bool expired = entry.expires && entry.expiryUtc.isValid()
                             && entry.expiryUtc <= QDateTime::currentDateTimeUtc();
        if (expired) {
            detail = detail.isEmpty() ? tr("已到期") : tr("已到期 · %1").arg(detail);
            ++expiredCount;
        }
        auto *item = new QTreeWidgetItem(parent, {entry.title, detail});
        item->setData(0, EntryIdRole, entry.id.toString(QUuid::WithoutBraces));
        item->setData(0, EntryRecycledRole, false);
        item->setToolTip(0, entry.title);
        item->setToolTip(1, expired
            ? tr("已于 %1 到期").arg(entry.expiryUtc.toLocalTime().toString(
                  QStringLiteral("yyyy-MM-dd HH:mm")))
            : detail);
        if (expired) {
            item->setForeground(0, QColor(Qt::darkRed));
            item->setForeground(1, QColor(Qt::darkRed));
        }
        if (entry.id == selectId)
            toSelect = item;
    }
    auto recycledEntries = service_.listRecycledEntries(&error);
    if (error.isError()) {
        showError(error, tr("无法读取回收站"));
        return;
    }
    std::sort(recycledEntries.begin(), recycledEntries.end(), compareEntries);
    for (const auto &entry : recycledEntries) {
        QString detail = entry.secureNote ? tr("安全笔记") : entry.username;
        if (detail.isEmpty())
            detail = entry.url;
        auto *item = new QTreeWidgetItem(recycleRoot_, {entry.title, detail});
        item->setData(0, EntryIdRole, entry.id.toString(QUuid::WithoutBraces));
        item->setData(0, EntryRecycledRole, true);
        item->setToolTip(0, entry.title);
        item->setToolTip(1, detail);
        if (entry.id == selectId)
            toSelect = item;
    }
    noteRoot_->setHidden(noteRoot_->childCount() == 0);
    recycleRoot_->setHidden(recycleRoot_->childCount() == 0);
    if (healthButton_) {
        healthButton_->setText(expiredCount > 0
            ? tr("健康检查（%1 到期）").arg(expiredCount)
            : tr("健康检查"));
        healthButton_->setToolTip(expiredCount > 0
            ? tr("有 %1 个账号已到期；点击查看完整离线检查报告").arg(expiredCount)
            : tr("离线检查弱密码、重复密码、空密码和到期账号"));
    }
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
    currentEntryRecycled_ = current->data(0, EntryRecycledRole).toBool();
    loadEntry(id);
}

void VaultDock::loadEntry(const QUuid &id)
{
    VaultError error;
    auto entry = service_.readEntryMetadata(id, &error);
    if (!entry) {
        showError(error, tr("无法读取条目"));
        return;
    }

    if (detailPages_ && detailEditorPage_)
        detailPages_->setCurrentWidget(detailEditorPage_);
    loading_ = true;
    currentEntryId_ = entry->id;
    currentTags_ = entry->tags;
    setEntryKind(entry->tags.contains(VaultSecureNoteTag) ? EntryKind::SecureNote
                                                          : EntryKind::Login);
    titleEdit_->setText(entry->title);
    usernameEdit_->setText(entry->username);
    urlEdit_->setText(entry->url);
    QStringList visibleTags = entry->tags;
    visibleTags.removeAll(VaultSecureNoteTag);
    tagsEdit_->setText(visibleTags.join(QStringLiteral(", ")));
    groupEdit_->setText(entry->groupPath);
    additionalUrlsEdit_->setPlainText(entry->additionalUrls.join(QLatin1Char('\n')));
    QStringList customLines;
    for (VaultCustomField &field : entry->customFields) {
        QString value = field.protectedValue
            ? QStringLiteral("••••••••")
            : transientText(field.valueUtf8);
        customLines.append((field.protectedValue ? QString() : QStringLiteral("public:"))
                           + field.name + QLatin1Char('=') + value);
        wipeQString(value);
        field.valueUtf8.clear();
    }
    customFieldsEdit_->setPlainText(customLines.join(QLatin1Char('\n')));
    for (QString &line : customLines)
        wipeQString(line);
    customLines.clear();
    expiryCheck_->setChecked(entry->expires);
    expiryEdit_->setDateTime(entry->expiryUtc.isValid()
                                 ? entry->expiryUtc.toLocalTime()
                                 : QDateTime::currentDateTime().addMonths(3));
    clearSensitiveLineEdit(passwordEdit_);
    clearSensitiveLineEdit(totpEdit_);
    currentEntryHasPassword_ = entry->hasPassword;
    currentEntryHasTotp_ = entry->hasTotp;
    QString notes = transientText(entry->notesUtf8);
    notesEdit_->setPlainText(notes);
    wipeQString(notes);
    entry->passwordUtf8.clear();
    entry->totpSeedUtf8.clear();
    entry->notesUtf8.clear();
    currentEntryPersisted_ = true;
    setEditorEnabled(!currentEntryRecycled_);
    setEditingState(false);
    refreshTotpCode();
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
    if (contentPages_)
        contentPages_->setCurrentIndex(1);
    if (searchRowWidget_)
        searchRowWidget_->show();
    if (newLoginButton_)
        newLoginButton_->show();
    entryTree_->clearSelection();
    clearEditor();
    if (detailPages_ && detailEditorPage_)
        detailPages_->setCurrentWidget(detailEditorPage_);
    currentEntryId_ = QUuid::createUuid();
    currentEntryRecycled_ = false;
    currentEntryPersisted_ = false;
    currentEntryHasPassword_ = false;
    currentEntryHasTotp_ = false;
    currentTags_.clear();
    setEntryKind(kind);
    setEditorEnabled(true);
    setEditingState(true, true);
    loading_ = false;
    setDirty(true);
    refreshTotpCode();
    titleEdit_->setFocus();
}

void VaultDock::saveCurrentEntry()
{
    saveCurrentEntryImpl(true);
}

void VaultDock::editCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || currentEntryRecycled_ || !currentEntryPersisted_) {
        return;
    }
    VaultError error;
    auto entry = service_.readEntry(currentEntryId_, &error);
    if (!entry) {
        showError(error, tr("无法读取账号秘密"));
        return;
    }
    loading_ = true;
    QString password = transientText(entry->passwordUtf8);
    entry->passwordUtf8.clear();
    passwordEdit_->setText(password);
    wipeQString(password);
    QString totpSeed = transientText(entry->totpSeedUtf8);
    entry->totpSeedUtf8.clear();
    totpEdit_->setText(totpSeed);
    wipeQString(totpSeed);
    QStringList customLines;
    for (VaultCustomField &field : entry->customFields) {
        QString value = transientText(field.valueUtf8);
        customLines.append((field.protectedValue ? QString() : QStringLiteral("public:"))
                           + field.name + QLatin1Char('=') + value);
        wipeQString(value);
        field.valueUtf8.clear();
    }
    customFieldsEdit_->setPlainText(customLines.join(QLatin1Char('\n')));
    for (QString &line : customLines)
        wipeQString(line);
    customLines.clear();
    loading_ = false;
    setEditingState(true);
    titleEdit_->setFocus();
    titleEdit_->selectAll();
}

void VaultDock::cancelCurrentEdit()
{
    if (!editorEditing_)
        return;
    editorDirty_ = false;
    if (!currentEntryPersisted_) {
        clearEditor();
        refreshEntries();
        return;
    }
    const QUuid id = currentEntryId_;
    loadEntry(id);
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
    data.tags.clear();
    const QStringList requestedTags = tagsEdit_->text().split(
        QRegularExpression(QStringLiteral("[,，;；]")), Qt::SkipEmptyParts);
    for (const QString &rawTag : requestedTags) {
        const QString tag = rawTag.trimmed();
        if (!tag.isEmpty() && !data.tags.contains(tag, Qt::CaseInsensitive))
            data.tags.push_back(tag);
    }
    if (currentKind_ == EntryKind::SecureNote)
        data.tags.push_back(VaultSecureNoteTag);
    if (currentKind_ == EntryKind::Login) {
        data.synchronizeExtendedFields = true;
        data.groupPath = groupEdit_->text().trimmed();
        const QStringList rawUrls = additionalUrlsEdit_->toPlainText().split(
            QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString &rawUrl : rawUrls) {
            const QString url = rawUrl.trimmed();
            if (!url.isEmpty() && !data.additionalUrls.contains(url, Qt::CaseInsensitive))
                data.additionalUrls.append(url);
        }
        const QStringList rawFields = customFieldsEdit_->toPlainText().split(
            QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        QSet<QString> fieldNames;
        for (const QString &rawLine : rawFields) {
            QString line = rawLine.trimmed();
            bool protectedValue = true;
            if (line.startsWith(QStringLiteral("public:"), Qt::CaseInsensitive)) {
                line.remove(0, 7);
                protectedValue = false;
            }
            const int separator = line.indexOf(QLatin1Char('='));
            const QString name = separator > 0 ? line.left(separator).trimmed() : QString();
            if (name.isEmpty() || fieldNames.contains(name.toCaseFolded())) {
                if (showSuccess) {
                    QMessageBox::warning(
                        this, tr("自定义字段无效"),
                        tr("每个自定义字段必须使用唯一的 name=value 格式。"));
                }
                customFieldsEdit_->setFocus();
                return false;
            }
            fieldNames.insert(name.toCaseFolded());
            VaultCustomField field;
            field.name = name;
            field.protectedValue = protectedValue;
            field.valueUtf8 = secureUtf8From(line.mid(separator + 1));
            data.customFields.push_back(std::move(field));
        }
        data.expires = expiryCheck_->isChecked();
        data.expiryUtc = expiryEdit_->dateTime().toUTC();
        data.passwordUtf8 = secureUtf8From(passwordEdit_->text());
        data.totpSeedUtf8 = secureUtf8From(totpEdit_->text().trimmed());
        currentEntryHasPassword_ = !data.passwordUtf8.empty();
        currentEntryHasTotp_ = !data.totpSeedUtf8.empty();
        data.hasPassword = currentEntryHasPassword_;
        data.hasTotp = currentEntryHasTotp_;
    }
    data.notesUtf8 = secureUtf8From(notesEdit_->toPlainText());

    auto pendingData = std::make_shared<VaultEntryData>(std::move(data));
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在保存账户"),
        [this, pendingData]() mutable {
            VaultError result = service_.upsertEntry(std::move(*pendingData));
            if (!result.isError())
                result = service_.save();
            return result;
        });
    if (error.isError()) {
        if (!handleExternalChangeError(error) && showSuccess)
            showError(error, tr("保存失败"));
        return false;
    }

    editorDirty_ = false;
    currentEntryPersisted_ = true;
    deleteButton_->setEnabled(true);
    refreshEntries(currentEntryId_);
    // Re-read presentation metadata after saving so password/TOTP and protected
    // custom-field values do not remain in ordinary view widgets.
    loadEntry(currentEntryId_);
    return true;
}

void VaultDock::removeCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull())
        return;
    const QString title = titleEdit_->text();
    const QString prompt = currentEntryRecycled_
        ? tr("永久删除“%1”？此操作无法撤销。").arg(title)
        : tr("将“%1”移到回收站？之后可以恢复。").arg(title);
    if (QMessageBox::question(this,
                              currentEntryRecycled_ ? tr("永久删除") : tr("移到回收站"),
                              prompt,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const bool permanentDelete = currentEntryRecycled_;
    const QUuid entryId = currentEntryId_;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        permanentDelete ? tr("正在永久删除账户") : tr("正在移动账户到回收站"),
        [this, permanentDelete, entryId] {
            VaultError result = permanentDelete ? service_.removeEntry(entryId)
                                                : service_.recycleEntry(entryId);
            if (!result.isError())
                result = service_.save();
            return result;
        });
    if (error.isError()) {
        if (!handleExternalChangeError(error))
            showError(error, currentEntryRecycled_ ? tr("永久删除失败") : tr("移到回收站失败"));
        return;
    }
    clearEditor();
    refreshEntries();
    emit userNotificationRequested(permanentDelete ? tr("账号已永久删除")
                                                    : tr("账号已移到回收站"),
                                   4000);
}

void VaultDock::restoreCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || !currentEntryRecycled_) {
        return;
    }
    const QUuid restoredId = currentEntryId_;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在恢复账户"),
        [this, restoredId] {
            VaultError result = service_.restoreEntry(restoredId);
            if (!result.isError())
                result = service_.save();
            return result;
        });
    if (error.isError()) {
        if (!handleExternalChangeError(error))
            showError(error, tr("恢复失败"));
        return;
    }
    currentEntryRecycled_ = false;
    refreshEntries(restoredId);
    loadEntry(restoredId);
    emit userNotificationRequested(tr("账号已从回收站恢复"), 4000);
}

void VaultDock::showEntryHistory()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || editorEditing_ || currentEntryRecycled_) {
        return;
    }
    VaultError error;
    const QList<VaultEntryHistorySummary> history =
        service_.entryHistory(currentEntryId_, &error);
    if (error.isError()) {
        showError(error, tr("无法读取账号历史"));
        return;
    }
    if (history.isEmpty()) {
        QMessageBox::information(this, tr("账号历史"),
                                 tr("此账号还没有可恢复的历史版本。"));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("vaultHistoryDialog"));
    dialog.setWindowTitle(tr("账号历史 — %1").arg(titleEdit_->text()));
    dialog.resize(620, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(
        tr("历史由 KeePassXC 在每次已保存账号变更前建立。列表不显示密码；"
           "恢复后当前版本仍会保留在历史中。"), &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("vaultHistoryList"));
    for (const VaultEntryHistorySummary &item : history) {
        const QString when = item.modifiedAtUtc.isValid()
            ? item.modifiedAtUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : tr("时间未知");
        const QString summary = item.username.trimmed().isEmpty()
            ? QStringLiteral("%1  ·  %2").arg(when, item.title)
            : QStringLiteral("%1  ·  %2  ·  %3").arg(when, item.title, item.username);
        const QString changed = item.changedFields.isEmpty()
            ? tr("与当前版本差异未知")
            : tr("变更：%1").arg(item.changedFields.join(tr("、")));
        auto *row = new QListWidgetItem(summary + QLatin1Char('\n') + changed, list);
        row->setData(Qt::UserRole, item.index);
        row->setToolTip((item.hasPassword ? tr("此版本包含密码（不会在列表中显示）")
                                         : tr("此版本没有密码"))
                        + QStringLiteral("\n") + changed);
    }
    list->setCurrentRow(0);
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *restore = buttons->addButton(tr("恢复所选版本"),
                                       QDialogButtonBox::AcceptRole);
    restore->setObjectName(QStringLiteral("vaultHistoryRestoreButton"));
    auto *remove = buttons->addButton(tr("删除所选版本"),
                                      QDialogButtonBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("vaultHistoryRemoveButton"));
    auto *clear = buttons->addButton(tr("清空历史"),
                                     QDialogButtonBox::DestructiveRole);
    clear->setObjectName(QStringLiteral("vaultHistoryClearButton"));
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked,
            &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, restore,
            [restore](QListWidgetItem *) { restore->click(); });
    bool restored = false;
    connect(restore, &QPushButton::clicked, &dialog, [&]() {
        QListWidgetItem *selected = list->currentItem();
        if (!selected)
            return;
        if (QMessageBox::question(
                &dialog, tr("恢复账号历史"),
                tr("恢复所选版本？当前版本会自动保留在历史中。"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        const QUuid entryId = currentEntryId_;
        const int historyIndex = selected->data(Qt::UserRole).toInt();
        VaultError restoreError = runSerializedSecurityOperation(
            &dialog, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
            tr("正在恢复账户历史"),
            [this, entryId, historyIndex] {
                VaultError result = service_.restoreEntryHistory(entryId, historyIndex);
                if (!result.isError())
                    result = service_.save();
                return result;
            });
        if (restoreError.isError()) {
            if (!handleExternalChangeError(restoreError))
                showError(restoreError, tr("恢复账号历史失败"));
            return;
        }
        restored = true;
        dialog.accept();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&]() {
        QListWidgetItem *selected = list->currentItem();
        if (!selected)
            return;
        if (QMessageBox::question(
                &dialog, tr("删除历史版本"),
                tr("永久删除所选历史版本？此操作无法撤销。"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        const QUuid entryId = currentEntryId_;
        const int historyIndex = selected->data(Qt::UserRole).toInt();
        VaultError removeError = runSerializedSecurityOperation(
            &dialog, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
            tr("正在删除账户历史"),
            [this, entryId, historyIndex] {
                VaultError result = service_.removeEntryHistory(entryId, historyIndex);
                if (!result.isError())
                    result = service_.save();
                return result;
            });
        if (removeError.isError()) {
            if (!handleExternalChangeError(removeError))
                showError(removeError, tr("删除历史版本失败"));
            return;
        }
        dialog.accept();
    });
    connect(clear, &QPushButton::clicked, &dialog, [&]() {
        if (QMessageBox::question(
                &dialog, tr("清空账号历史"),
                tr("永久删除此账号的全部历史版本？当前版本不会被删除。"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        const QUuid entryId = currentEntryId_;
        VaultError clearError = runSerializedSecurityOperation(
            &dialog, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
            tr("正在清理账户历史"),
            [this, entryId] {
                VaultError result = service_.clearEntryHistory(entryId);
                if (!result.isError())
                    result = service_.save();
                return result;
            });
        if (clearError.isError()) {
            if (!handleExternalChangeError(clearError))
                showError(clearError, tr("清空账号历史失败"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QUuid id = currentEntryId_;
    refreshEntries(id);
    loadEntry(id);
    emit userNotificationRequested(restored ? tr("已恢复账号历史版本")
                                             : tr("已清理账号历史"), 4000);
}

void VaultDock::cloneCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || editorEditing_ || currentEntryRecycled_ || !currentEntryPersisted_
        || currentKind_ != EntryKind::Login) {
        return;
    }
    VaultError error;
    auto copy = service_.readEntry(currentEntryId_, &error);
    if (!copy) {
        showError(error, tr("无法读取待克隆账号"));
        return;
    }
    const QUuid cloneId = QUuid::createUuid();
    copy->id = cloneId;
    copy->title = tr("%1 - 副本").arg(copy->title);
    copy->synchronizeExtendedFields = true;
    copy->recordHistory = false;
    auto pendingCopy = std::shared_ptr<VaultEntryData>(std::move(copy));
    error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在克隆账户"),
        [this, pendingCopy]() mutable {
            VaultError result = service_.upsertEntry(std::move(*pendingCopy));
            if (!result.isError())
                result = service_.save();
            return result;
        });
    if (error.isError()) {
        if (!handleExternalChangeError(error))
            showError(error, tr("克隆账号失败"));
        return;
    }
    refreshEntries(cloneId);
    loadEntry(cloneId);
    emit userNotificationRequested(tr("已克隆账号；可继续编辑副本"), 4000);
}

void VaultDock::showPasswordHealthReport()
{
    if (service_.state() != VaultState::Open)
        return;
    VaultError error;
    const QList<VaultHealthFinding> findings = service_.passwordHealthReport(&error);
    if (error.isError()) {
        showError(error, tr("无法执行密码健康检查"));
        return;
    }
    if (findings.isEmpty()) {
        QMessageBox::information(
            this, tr("密码健康检查"),
            tr("未发现空密码、弱密码、重复密码或已到期账号。检查完全在本机进行。"));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("vaultHealthDialog"));
    dialog.setWindowTitle(tr("密码健康检查"));
    dialog.resize(720, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(
        tr("发现 %1 项需要处理的问题。检查完全在本机完成，不上传密码；列表也不显示密码。")
            .arg(findings.size()),
        &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("vaultHealthList"));
    for (const VaultHealthFinding &finding : findings) {
        QString issueLabel = finding.issue;
        if (finding.issue == QLatin1String("expired"))
            issueLabel = tr("已到期");
        else if (finding.issue == QLatin1String("missing"))
            issueLabel = tr("未设置密码");
        else if (finding.issue == QLatin1String("weak"))
            issueLabel = tr("弱密码");
        else if (finding.issue == QLatin1String("reused"))
            issueLabel = tr("重复使用");
        auto *row = new QListWidgetItem(
            QStringLiteral("%1  ·  %2\n%3")
                .arg(finding.title, issueLabel, finding.detail), list);
        row->setData(Qt::UserRole, finding.entryId.toString(QUuid::WithoutBraces));
        row->setToolTip(tr("双击打开此账号"));
    }
    list->setCurrentRow(0);
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *open = buttons->addButton(tr("打开所选账号"), QDialogButtonBox::AcceptRole);
    open->setObjectName(QStringLiteral("vaultHealthOpenButton"));
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked,
            &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, open,
            [open](QListWidgetItem *) { open->click(); });
    connect(open, &QPushButton::clicked, &dialog, [&dialog, list]() {
        if (list->currentItem())
            dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return;
    const QUuid id(list->currentItem()->data(Qt::UserRole).toString());
    if (id.isNull())
        return;
    refreshEntries(id);
    loadEntry(id);
}

void VaultDock::setEntryKind(EntryKind kind)
{
    currentKind_ = kind;
    const bool login = kind == EntryKind::Login;
    kindLabel_->setText(login ? tr("账号") : tr("加密笔记"));
    const QList<QWidget *> loginWidgets = {
        usernameLabel_, usernameEdit_, urlLabel_, urlEdit_, passwordLabel_,
        passwordRow_, passwordStrengthCaption_, passwordStrengthLabel_,
        totpLabel_, totpInputRow_, totpCodeCaption_, totpCodeRow_};
    for (QWidget *widget : loginWidgets) {
        widget->setVisible(login);
    }
}

void VaultDock::setEditingState(bool editing, bool newEntry)
{
    editorEditing_ = editing && !currentEntryRecycled_;
    if (newEntry)
        currentEntryPersisted_ = false;

    const bool hasEntry = !currentEntryId_.isNull();
    const bool available = service_.state() == VaultState::Open && hasEntry;
    const bool editable = available && editorEditing_;
    if (!editable) {
        hidePassword();
        clearSensitiveLineEdit(totpEdit_);
    }
    for (QLineEdit *field : {titleEdit_, usernameEdit_, urlEdit_, tagsEdit_, groupEdit_,
                             passwordEdit_, totpEdit_})
        field->setReadOnly(!editable);
    notesEdit_->setReadOnly(!editable);
    additionalUrlsEdit_->setReadOnly(!editable);
    customFieldsEdit_->setReadOnly(!editable);
    expiryCheck_->setEnabled(editable);
    expiryEdit_->setEnabled(editable && expiryCheck_->isChecked());
    const QString detailLineStyle = editable
        ? QString()
        : QStringLiteral("QLineEdit { border: none; background: transparent; padding-left: 0px; }");
    for (QLineEdit *field : {titleEdit_, usernameEdit_, urlEdit_, tagsEdit_, groupEdit_,
                             passwordEdit_})
        field->setStyleSheet(detailLineStyle);
    QFont titleFont = QApplication::font();
    if (!editable) {
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
    }
    titleEdit_->setFont(titleFont);
    notesEdit_->setStyleSheet(editable
        ? QString()
        : QStringLiteral("QTextEdit { border: none; background: transparent; padding: 0px; }"));

    saveButton_->setVisible(editable);
    saveButton_->setEnabled(editable);
    cancelButton_->setVisible(editable);
    cancelButton_->setEnabled(editable);
    editButton_->setVisible(available && !currentEntryRecycled_ && currentEntryPersisted_
                            && !editorEditing_);
    editButton_->setEnabled(editButton_->isVisible());
    historyButton_->setVisible(available && !currentEntryRecycled_
                               && currentEntryPersisted_ && !editorEditing_);
    historyButton_->setEnabled(historyButton_->isVisible());
    cloneButton_->setVisible(available && currentKind_ == EntryKind::Login
                             && !currentEntryRecycled_ && currentEntryPersisted_
                             && !editorEditing_);
    cloneButton_->setEnabled(cloneButton_->isVisible());
    restoreButton_->setVisible(available && currentEntryRecycled_ && !editorEditing_);
    restoreButton_->setEnabled(restoreButton_->isVisible());
    deleteButton_->setVisible(available && !editorEditing_);
    deleteButton_->setEnabled(deleteButton_->isVisible());
    deleteButton_->setText(currentEntryRecycled_ ? tr("永久删除") : tr("移到回收站"));
    deleteButton_->setToolTip(currentEntryRecycled_
                                  ? tr("永久删除此账号；此操作无法撤销")
                                  : tr("先移到回收站；可在永久删除前恢复"));

    const bool login = currentKind_ == EntryKind::Login;
    titleEdit_->setVisible(available);
    formLayout_->setRowVisible(usernameRow_, available && login);
    formLayout_->setRowVisible(urlRow_, available && login);
    formLayout_->setRowVisible(passwordRow_, available && login);
    formLayout_->setRowVisible(groupEdit_, available && login);
    formLayout_->setRowVisible(additionalUrlsEdit_, available && login);
    formLayout_->setRowVisible(customFieldsEdit_, available && login);
    formLayout_->setRowVisible(expiryRow_, available && login);
    formLayout_->setRowVisible(totpCodeRow_, available && login);
    formLayout_->setRowVisible(notesEdit_, available);
    // The seed is configuration, not daily account information.
    formLayout_->setRowVisible(totpInputRow_, login && editable);
    formLayout_->setRowVisible(passwordStrengthLabel_, login && editable);
    generatePasswordButton_->setVisible(login && editable);
    totpConfigureButton_->setVisible(login && editable);
    totpConfigureButton_->setEnabled(login && editable);
    passwordEdit_->setPlaceholderText(login && !editable && currentEntryHasPassword_
                                          ? QStringLiteral("••••••••••••")
                                          : QString());
    showPasswordCheck_->setVisible(login && available
                                   && (editable || currentEntryHasPassword_));
    showPasswordCheck_->setEnabled(showPasswordCheck_->isVisible());
    copyPasswordButton_->setVisible(login && available
                                    && (editable || currentEntryHasPassword_));
    copyPasswordButton_->setEnabled(copyPasswordButton_->isVisible()
                                    && (editable ? !passwordEdit_->text().isEmpty()
                                                 : currentEntryHasPassword_));
    formLayout_->setRowVisible(tagsEdit_, editable || !tagsEdit_->text().trimmed().isEmpty());
    copyUsernameButton_->setVisible(login && available && !currentEntryRecycled_
                                    && !editorEditing_);
    copyUsernameButton_->setEnabled(copyUsernameButton_->isVisible()
                                    && !usernameEdit_->text().isEmpty());
    openUrlButton_->setVisible(login && available && !currentEntryRecycled_
                               && !editorEditing_);
    openUrlButton_->setEnabled(openUrlButton_->isVisible() && !urlEdit_->text().isEmpty());
    if (editable && login)
        updatePasswordStrength();

    if (currentEntryRecycled_) {
        kindLabel_->setText(currentKind_ == EntryKind::SecureNote
                                ? tr("回收站 · 旧版笔记（只读）")
                                : tr("回收站 · 账号（只读）"));
    } else if (editorEditing_) {
        kindLabel_->setText(currentKind_ == EntryKind::SecureNote
                                ? (currentEntryPersisted_ ? tr("编辑旧版笔记")
                                                          : tr("导入旧版笔记"))
                                : (currentEntryPersisted_ ? tr("编辑账号") : tr("新建账号")));
    } else {
        kindLabel_->setText(currentKind_ == EntryKind::SecureNote ? tr("旧版笔记")
                                                                  : tr("账号"));
    }
    emit workspaceCommandStateChanged();
}

void VaultDock::setEditorEnabled(bool enabled)
{
    const QList<QWidget *> editorWidgets = {
        titleEdit_, usernameEdit_, urlEdit_, tagsEdit_, groupEdit_, passwordEdit_, totpEdit_,
        additionalUrlsEdit_, customFieldsEdit_, expiryCheck_, expiryEdit_, notesEdit_,
        showPasswordCheck_, copyPasswordButton_, copyTotpButton_, saveButton_,
        generatePasswordButton_, totpConfigureButton_, editButton_, historyButton_, cancelButton_,
        cloneButton_, copyUsernameButton_, openUrlButton_};
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
    if (emptyCreateButton_)
        emptyCreateButton_->setEnabled(open);
    emit workspaceCommandStateChanged();
}

void VaultDock::clearEditor()
{
    loading_ = true;
    titleEdit_->clear();
    usernameEdit_->clear();
    urlEdit_->clear();
    tagsEdit_->clear();
    groupEdit_->clear();
    additionalUrlsEdit_->clear();
    customFieldsEdit_->clear();
    expiryCheck_->setChecked(false);
    expiryEdit_->setDateTime(QDateTime::currentDateTime().addMonths(3));
    clearSensitiveLineEdit(passwordEdit_);
    passwordStrengthLabel_->setText(tr("未输入"));
    passwordStrengthLabel_->setStyleSheet(QString());
    clearSensitiveLineEdit(totpEdit_);
    notesEdit_->clear();
    showPasswordCheck_->setChecked(false);
    wipeQString(currentTotpCode_);
    totpCodeLabel_->setText(tr("未设置"));
    if (totpRefreshTimer_)
        totpRefreshTimer_->stop();
    currentEntryId_ = {};
    currentEntryRecycled_ = false;
    currentEntryPersisted_ = false;
    currentEntryHasPassword_ = false;
    currentEntryHasTotp_ = false;
    currentTags_.clear();
    editorDirty_ = false;
    kindLabel_->setText(tr("选择一个条目"));
    setEditorEnabled(false);
    editorEditing_ = false;
    saveButton_->setVisible(false);
    editButton_->setVisible(false);
    historyButton_->setVisible(false);
    cloneButton_->setVisible(false);
    cancelButton_->setVisible(false);
    copyUsernameButton_->setVisible(false);
    openUrlButton_->setVisible(false);
    restoreButton_->setVisible(false);
    deleteButton_->setVisible(false);
    deleteButton_->setText(tr("移到回收站"));
    deleteButton_->setToolTip(tr("先移到回收站；可在永久删除前恢复"));
    titleEdit_->hide();
    const QList<QWidget *> formFields = {
        usernameRow_, urlRow_, tagsEdit_, groupEdit_, additionalUrlsEdit_,
        customFieldsEdit_, expiryRow_, passwordRow_,
        passwordStrengthLabel_, totpInputRow_, totpCodeRow_, notesEdit_};
    for (QWidget *field : formFields) {
        formLayout_->setRowVisible(field, false);
    }
    if (detailPages_)
        detailPages_->setCurrentIndex(0);
    loading_ = false;
    emit workspaceCommandStateChanged();
}

void VaultDock::setDirty(bool dirty)
{
    if (!editorEditing_)
        return;
    if (editorDirty_ == dirty)
        return;
    editorDirty_ = dirty;
    if (dirty && !kindLabel_->text().endsWith(QLatin1Char('*')))
        kindLabel_->setText(kindLabel_->text() + QStringLiteral(" *"));
    emit workspaceCommandStateChanged();
}

void VaultDock::focusAccountSearch()
{
    if (!canSearchAccounts())
        return;
    filterEdit_->setFocus(Qt::ShortcutFocusReason);
    filterEdit_->selectAll();
}

bool VaultDock::canSearchAccounts() const
{
    return isSecureStoreOpen() && filterEdit_ && filterEdit_->isVisible();
}

bool VaultDock::saveActiveEntry()
{
    if (!canSaveActiveEntry())
        return false;
    return saveCurrentEntryImpl(true);
}

bool VaultDock::maybeSaveChanges()
{
    if (!editorDirty_)
        return true;
    const auto result = QMessageBox::warning(
        this, tr("未保存的更改"), tr("当前条目有未保存的修改。"),
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
    if (!loginRoot_ || !noteRoot_ || !recycleRoot_)
        return;
    const QString needle = text.trimmed();
    QSet<QUuid> matches;
    if (!needle.isEmpty()) {
        VaultError error;
        matches = service_.searchEntryIds(needle, true, &error);
        if (error.isError())
            return;
    }
    int visibleEntries = 0;
    const std::function<bool(QTreeWidgetItem *)> updateBranch =
        [&](QTreeWidgetItem *item) -> bool {
        const QUuid id(item->data(0, EntryIdRole).toString());
        if (!id.isNull()) {
            const bool visible = needle.isEmpty() || matches.contains(id);
            item->setHidden(!visible);
            if (visible)
                ++visibleEntries;
            return visible;
        }
        bool anyVisible = false;
        for (int i = 0; i < item->childCount(); ++i)
            anyVisible |= updateBranch(item->child(i));
        item->setHidden(!anyVisible);
        if (!needle.isEmpty() && anyVisible)
            item->setExpanded(true);
        return anyVisible;
    };
    for (QTreeWidgetItem *root : {loginRoot_, noteRoot_, recycleRoot_}) {
        const bool anyVisible = updateBranch(root);
        root->setHidden(!anyVisible);
    }
    if (listPages_) {
        if (listEmptyLabel_) {
            listEmptyLabel_->setText(
                needle.isEmpty()
                    ? tr("还没有账号\n使用右上角“+ 新建账号”开始")
                    : tr("没有匹配的账号\n请尝试其他关键词"));
        }
        listPages_->setCurrentIndex(visibleEntries > 0 ? 0 : 1);
    }
    const int totalEntries = visibleEntries;
    const bool firstUse = totalEntries == 0 && needle.isEmpty();
    if (contentPages_)
        contentPages_->setCurrentIndex(firstUse ? 0 : 1);
    if (searchRowWidget_)
        searchRowWidget_->setVisible(!firstUse);
    if (newLoginButton_)
        newLoginButton_->setVisible(!firstUse);
    if (emptyCreateButton_)
        emptyCreateButton_->setEnabled(service_.state() == VaultState::Open);
}

void VaultDock::updatePasswordEcho(bool visible)
{
    if (visible && !editorEditing_) {
        VaultEntrySecrets secrets;
        const VaultError error = service_.readEntrySecrets(currentEntryId_, &secrets);
        if (error.isError() || secrets.passwordUtf8.empty()) {
            QSignalBlocker blocker(showPasswordCheck_);
            showPasswordCheck_->setChecked(false);
            clearSensitiveLineEdit(passwordEdit_);
            passwordEdit_->setEchoMode(QLineEdit::Password);
            passwordRevealTimer_->stop();
            if (error.isError())
                showError(error, tr("无法显示密码"));
            return;
        }
        QString password = transientText(secrets.passwordUtf8);
        secrets.passwordUtf8.clear();
        passwordEdit_->setText(password);
        wipeQString(password);
    }
    passwordEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    if (visible)
        passwordRevealTimer_->start(PasswordRevealLifetimeMs);
    else {
        passwordRevealTimer_->stop();
        if (!editorEditing_)
            clearSensitiveLineEdit(passwordEdit_);
    }
}

void VaultDock::hidePassword()
{
    if (showPasswordCheck_->isChecked())
        showPasswordCheck_->setChecked(false);
    else
        updatePasswordEcho(false);
}

void VaultDock::copyPasswordProtected()
{
    if (service_.state() != VaultState::Open || currentKind_ != EntryKind::Login) {
        return;
    }
    if (editorEditing_) {
        if (passwordEdit_->text().isEmpty())
            return;
        copyProtectedText(passwordEdit_->text());
        emit userNotificationRequested(
            tr("密码已复制，将在 30 秒后从剪贴板清除"), 5000);
        return;
    }
    VaultEntrySecrets secrets;
    const VaultError error = service_.readEntrySecrets(currentEntryId_, &secrets);
    if (error.isError()) {
        showError(error, tr("无法复制密码"));
        return;
    }
    QString password = transientText(secrets.passwordUtf8);
    secrets.passwordUtf8.clear();
    copyProtectedText(password);
    wipeQString(password);
    emit userNotificationRequested(
        tr("密码已复制，将在 30 秒后从剪贴板清除"), 5000);
}

void VaultDock::generatePassword()
{
    if (!editorEditing_ || currentKind_ != EntryKind::Login || currentEntryRecycled_)
        return;

    const auto settings = createSecuritySettings();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("vaultPasswordGeneratorDialog"));
    dialog.setWindowTitle(tr("生成密码"));
    auto *layout = new QFormLayout(&dialog);
    auto *length = new QSpinBox(&dialog);
    length->setObjectName(QStringLiteral("vaultPasswordLength"));
    length->setRange(8, 128);
    length->setValue(settings->value("passwordGenerator/length", 24).toInt());
    layout->addRow(tr("长度"), length);
    auto *lower = new QCheckBox(tr("小写字母"), &dialog);
    auto *upper = new QCheckBox(tr("大写字母"), &dialog);
    auto *numbers = new QCheckBox(tr("数字"), &dialog);
    auto *special = new QCheckBox(tr("特殊字符"), &dialog);
    lower->setObjectName(QStringLiteral("vaultPasswordLower"));
    upper->setObjectName(QStringLiteral("vaultPasswordUpper"));
    numbers->setObjectName(QStringLiteral("vaultPasswordNumbers"));
    special->setObjectName(QStringLiteral("vaultPasswordSpecial"));
    lower->setChecked(settings->value("passwordGenerator/lower", true).toBool());
    upper->setChecked(settings->value("passwordGenerator/upper", true).toBool());
    numbers->setChecked(settings->value("passwordGenerator/numbers", true).toBool());
    special->setChecked(settings->value("passwordGenerator/special", true).toBool());
    auto *classes = new QWidget(&dialog);
    auto *classesLayout = new QHBoxLayout(classes);
    classesLayout->setContentsMargins(0, 0, 0, 0);
    classesLayout->addWidget(lower);
    classesLayout->addWidget(upper);
    classesLayout->addWidget(numbers);
    classesLayout->addWidget(special);
    layout->addRow(tr("字符集"), classes);
    auto *excludeLookAlike = new QCheckBox(tr("排除易混淆字符"), &dialog);
    excludeLookAlike->setObjectName(QStringLiteral("vaultPasswordExcludeLookAlike"));
    excludeLookAlike->setChecked(
        settings->value("passwordGenerator/excludeLookAlike", true).toBool());
    layout->addRow(QString(), excludeLookAlike);
    auto *everyGroup = new QCheckBox(tr("每组选中的字符至少出现一次"), &dialog);
    everyGroup->setObjectName(QStringLiteral("vaultPasswordEveryGroup"));
    everyGroup->setChecked(
        settings->value("passwordGenerator/everyGroup", true).toBool());
    layout->addRow(QString(), everyGroup);
    auto *excluded = new QLineEdit(&dialog);
    excluded->setObjectName(QStringLiteral("vaultPasswordExcludedCharacters"));
    excluded->setText(settings->value("passwordGenerator/excluded").toString());
    layout->addRow(tr("额外排除"), excluded);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("生成"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    VaultPasswordGenerationOptions options;
    options.length = length->value();
    options.lower = lower->isChecked();
    options.upper = upper->isChecked();
    options.numbers = numbers->isChecked();
    options.special = special->isChecked();
    options.excludeLookAlike = excludeLookAlike->isChecked();
    options.requireEverySelectedGroup = everyGroup->isChecked();
    options.excludedCharacters = excluded->text();
    if (!options.lower && !options.upper && !options.numbers && !options.special) {
        QMessageBox::warning(this, tr("无法生成密码"), tr("请至少选择一个字符集。"));
        return;
    }
    settings->setValue("passwordGenerator/length", options.length);
    settings->setValue("passwordGenerator/lower", options.lower);
    settings->setValue("passwordGenerator/upper", options.upper);
    settings->setValue("passwordGenerator/numbers", options.numbers);
    settings->setValue("passwordGenerator/special", options.special);
    settings->setValue("passwordGenerator/excludeLookAlike", options.excludeLookAlike);
    settings->setValue("passwordGenerator/everyGroup", options.requireEverySelectedGroup);
    settings->setValue("passwordGenerator/excluded", options.excludedCharacters);
    SecureBuffer generated;
    const VaultError error = service_.generatePassword(options, &generated);
    options.excludedCharacters.fill(QChar(0));
    options.excludedCharacters.clear();
    if (error.isError()) {
        showError(error, tr("无法生成密码"));
        return;
    }
    QString password = transientText(generated);
    generated.clear();
    passwordEdit_->setText(password);
    wipeQString(password);
    setDirty(true);
    updatePasswordStrength();
    showPasswordCheck_->setChecked(true);
    passwordEdit_->setFocus();
    passwordEdit_->selectAll();
}

void VaultDock::configureTotp()
{
    if (!editorEditing_ || currentKind_ != EntryKind::Login || currentEntryRecycled_)
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("vaultTotpSetupDialog"));
    dialog.setWindowTitle(tr("设置动态验证码 (TOTP)"));
    auto *layout = new QFormLayout(&dialog);
    auto *seed = new QLineEdit(&dialog);
    seed->setObjectName(QStringLiteral("vaultTotpSeed"));
    seed->setEchoMode(QLineEdit::Password);
    seed->setPlaceholderText(tr("Base32 密钥，或粘贴 otpauth:// URI"));
    auto *issuer = new QLineEdit(&dialog);
    issuer->setObjectName(QStringLiteral("vaultTotpIssuer"));
    issuer->setText(titleEdit_->text().trimmed());
    auto *account = new QLineEdit(&dialog);
    account->setObjectName(QStringLiteral("vaultTotpAccount"));
    account->setText(usernameEdit_->text().trimmed());
    auto *algorithm = new QComboBox(&dialog);
    algorithm->setObjectName(QStringLiteral("vaultTotpAlgorithm"));
    algorithm->addItems({QStringLiteral("SHA1"), QStringLiteral("SHA256"),
                         QStringLiteral("SHA512")});
    auto *digits = new QSpinBox(&dialog);
    digits->setObjectName(QStringLiteral("vaultTotpDigits"));
    digits->setRange(6, 8);
    digits->setValue(6);
    auto *period = new QSpinBox(&dialog);
    period->setObjectName(QStringLiteral("vaultTotpPeriod"));
    period->setRange(5, 300);
    period->setSuffix(tr(" 秒"));
    period->setValue(30);

    const QString current = totpEdit_->text().trimmed();
    if (current.startsWith(QStringLiteral("otpauth://"), Qt::CaseInsensitive)) {
        const QUrl uri(current);
        const QUrlQuery query(uri);
        seed->setText(query.queryItemValue(QStringLiteral("secret")));
        issuer->setText(query.queryItemValue(QStringLiteral("issuer")));
        const QString label = QUrl::fromPercentEncoding(uri.path().mid(1).toUtf8());
        const int colon = label.indexOf(QLatin1Char(':'));
        account->setText(colon >= 0 ? label.mid(colon + 1) : label);
        const QString alg = query.queryItemValue(QStringLiteral("algorithm")).toUpper();
        const int algIndex = algorithm->findText(alg);
        if (algIndex >= 0) algorithm->setCurrentIndex(algIndex);
        bool ok = false;
        const int parsedDigits = query.queryItemValue(QStringLiteral("digits")).toInt(&ok);
        if (ok) digits->setValue(parsedDigits);
        const int parsedPeriod = query.queryItemValue(QStringLiteral("period")).toInt(&ok);
        if (ok) period->setValue(parsedPeriod);
    } else {
        seed->setText(current);
    }
    layout->addRow(tr("密钥"), seed);
    layout->addRow(tr("服务商"), issuer);
    layout->addRow(tr("账户名"), account);
    layout->addRow(tr("算法"), algorithm);
    layout->addRow(tr("位数"), digits);
    layout->addRow(tr("周期"), period);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    auto *remove = buttons->addButton(tr("移除验证码"), QDialogButtonBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("vaultTotpRemoveButton"));
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(2); });
    const int result = dialog.exec();
    if (result == 2) {
        clearSensitiveLineEdit(totpEdit_);
        setDirty(true);
        return;
    }
    if (result != QDialog::Accepted)
        return;
    QString rawSecret = takeSensitiveLineEdit(seed);
    QString secret = rawSecret.trimmed();
    wipeQString(rawSecret);
    if (secret.isEmpty()) {
        wipeQString(secret);
        QMessageBox::warning(this, tr("验证码配置无效"), tr("密钥不能为空。"));
        return;
    }
    if (secret.startsWith(QStringLiteral("otpauth://"), Qt::CaseInsensitive)) {
        totpEdit_->setText(secret);
    } else {
        QUrl uri;
        uri.setScheme(QStringLiteral("otpauth"));
        uri.setHost(QStringLiteral("totp"));
        QString label = account->text().trimmed();
        if (!issuer->text().trimmed().isEmpty())
            label.prepend(issuer->text().trimmed() + QLatin1Char(':'));
        uri.setPath(QLatin1Char('/') + label);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("secret"), secret);
        if (!issuer->text().trimmed().isEmpty())
            query.addQueryItem(QStringLiteral("issuer"), issuer->text().trimmed());
        query.addQueryItem(QStringLiteral("algorithm"), algorithm->currentText());
        query.addQueryItem(QStringLiteral("digits"), QString::number(digits->value()));
        query.addQueryItem(QStringLiteral("period"), QString::number(period->value()));
        uri.setQuery(query);
        totpEdit_->setText(uri.toString(QUrl::FullyEncoded));
    }
    wipeQString(secret);
    setDirty(true);
}

void VaultDock::updatePasswordStrength()
{
    if (!editorEditing_ || currentKind_ != EntryKind::Login) {
        passwordStrengthLabel_->clear();
        return;
    }
    SecureBuffer password = secureUtf8From(passwordEdit_->text());
    VaultPasswordAssessment assessment;
    const VaultError error = service_.assessPassword(password, &assessment);
    password.clear();
    if (error.isError()) {
        passwordStrengthLabel_->setText(tr("无法评估"));
        passwordStrengthLabel_->setStyleSheet(QString());
        return;
    }
    QString quality;
    QString color;
    switch (assessment.quality) {
    case VaultPasswordQuality::Empty:
        quality = tr("未输入");
        break;
    case VaultPasswordQuality::Bad:
        quality = tr("很弱");
        color = QStringLiteral("#c62828");
        break;
    case VaultPasswordQuality::Poor:
        quality = tr("较弱");
        color = QStringLiteral("#d84315");
        break;
    case VaultPasswordQuality::Weak:
        quality = tr("一般");
        color = QStringLiteral("#ad6800");
        break;
    case VaultPasswordQuality::Good:
        quality = tr("强");
        color = QStringLiteral("#2e7d32");
        break;
    case VaultPasswordQuality::Excellent:
        quality = tr("很强");
        color = QStringLiteral("#1b5e20");
        break;
    }
    passwordStrengthLabel_->setText(
        assessment.quality == VaultPasswordQuality::Empty
            ? quality
            : tr("%1 · 约 %2 bit").arg(quality).arg(qRound(assessment.entropyBits)));
    passwordStrengthLabel_->setStyleSheet(
        color.isEmpty() ? QString() : QStringLiteral("QLabel { color: %1; font-weight: 600; }").arg(color));
}

void VaultDock::copyUsername()
{
    if (service_.state() != VaultState::Open || currentKind_ != EntryKind::Login
        || currentEntryRecycled_ || usernameEdit_->text().isEmpty()) {
        return;
    }
    // A username is not treated as a secret, but replacing a previously
    // protected clipboard value must also cancel its pending cleanup token.
    clearProtectedClipboard();
    QGuiApplication::clipboard()->setText(usernameEdit_->text(), QClipboard::Clipboard);
    emit userNotificationRequested(tr("用户名已复制"), 2500);
}

void VaultDock::openCurrentUrl()
{
    if (service_.state() != VaultState::Open || currentKind_ != EntryKind::Login
        || currentEntryRecycled_) {
        return;
    }
    const QUrl url = QUrl::fromUserInput(urlEdit_->text().trimmed());
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https"))) {
        QMessageBox::warning(this, tr("无法打开网站"),
                             tr("网址必须是有效的 HTTP 或 HTTPS 地址。"));
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        QMessageBox::warning(this, tr("无法打开网站"), tr("系统未能打开默认浏览器。"));
    }
}

void VaultDock::refreshTotpCode()
{
    // Security operations mutate the non-thread-safe KDBX backend on a worker.
    // A one-second UI timer must never read it concurrently; the next tick is
    // soon enough and avoids adding a second synchronization mechanism.
    if (AsyncSerialQueue::security().busy()) {
        totpRefreshTimer_->start();
        return;
    }
    wipeQString(currentTotpCode_);
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || currentKind_ != EntryKind::Login || currentEntryRecycled_) {
        totpCodeLabel_->setText(currentEntryRecycled_ ? tr("回收站中停用") : tr("未设置"));
        copyTotpButton_->setEnabled(false);
        totpRefreshTimer_->stop();
        return;
    }
    VaultTotpSnapshot snapshot;
    const VaultError error = service_.currentTotp(currentEntryId_, &snapshot);
    if (error.isError()) {
        totpCodeLabel_->setText(editorDirty_ ? tr("保存后显示") : tr("密钥无效"));
        copyTotpButton_->setEnabled(false);
        if (editorDirty_)
            totpRefreshTimer_->stop();
        else
            totpRefreshTimer_->start();
        return;
    }
    if (snapshot.code.isEmpty()) {
        totpCodeLabel_->setText(editorDirty_ && !totpEdit_->text().isEmpty()
                                    ? tr("保存后显示") : tr("未设置"));
        copyTotpButton_->setEnabled(false);
    } else {
        currentTotpCode_ = snapshot.code;
        QString display = snapshot.code;
        if (display.size() >= 6 && display.size() % 2 == 0)
            display.insert(display.size() / 2, QLatin1Char(' '));
        totpCodeLabel_->setText(tr("%1  ·  %2 秒").arg(display).arg(snapshot.remainingSeconds));
        copyTotpButton_->setEnabled(true);
    }
    totpRefreshTimer_->start();
}

void VaultDock::copyTotpProtected()
{
    if (!currentTotpCode_.isEmpty()) {
        copyProtectedText(currentTotpCode_);
        emit userNotificationRequested(
            tr("验证码已复制，将在 30 秒后从剪贴板清除"), 5000);
    }
}

void VaultDock::copyProtectedText(const QString &text)
{
    if (text.isEmpty())
        return;
    QString protectedText = text;
    QByteArray utf8 = protectedText.toUtf8();
    clipboardFingerprint_ = QCryptographicHash::hash(utf8, QCryptographicHash::Sha256);
    QGuiApplication::clipboard()->setText(protectedText, QClipboard::Clipboard);
    scrubByteArray(utf8);
    wipeQString(protectedText);
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

bool VaultDock::handleExternalChangeError(const VaultError &error)
{
    if (!error.diagnosticCode.startsWith(QStringLiteral("vault.external-change.")))
        return false;

    QMessageBox box(QMessageBox::Warning, tr("账户文件已在磁盘上更改"),
                    tr("为避免覆盖其他程序的修改，本次操作已停止。"),
                    QMessageBox::NoButton, this);
    box.setInformativeText(
        tr("可以放弃当前未保存的表单内容并重新打开磁盘版本，"
           "或取消后手动复制需要保留的字段。"));
    auto *reload = box.addButton(tr("放弃当前编辑并重新打开"),
                                 QMessageBox::DestructiveRole);
    auto *cancel = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() == reload) {
        emit externalStoreReloadRequested(service_.filePath());
    }
    return true;
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
