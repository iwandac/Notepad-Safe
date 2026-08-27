#include "VaultDock.h"
#include "VaultDockSupport.h"
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

constexpr int RecoveryClipboardLifetimeMs = 60 * 1000;





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
    auto *copy = new QPushButton(QObject::tr("复制（60 秒后清除）"), &dialog);
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
    QObject::connect(copy, &QPushButton::clicked, &dialog, [&recoveryKey]() {
        QByteArray keyBytes = recoveryKey.toUtf8();
        const QByteArray fingerprint = QCryptographicHash::hash(
            keyBytes, QCryptographicHash::Sha256);
        QGuiApplication::clipboard()->setText(recoveryKey, QClipboard::Clipboard);
        scrubByteArray(keyBytes);
        QTimer::singleShot(RecoveryClipboardLifetimeMs, qApp, [fingerprint]() {
            QClipboard *clipboard = QGuiApplication::clipboard();
            if (!clipboard)
                return;
            QByteArray current = clipboard->text(QClipboard::Clipboard).toUtf8();
            const QByteArray currentFingerprint = QCryptographicHash::hash(
                current, QCryptographicHash::Sha256);
            scrubByteArray(current);
            if (currentFingerprint == fingerprint)
                clipboard->clear(QClipboard::Clipboard);
        });
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &recoveryKey]() {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, QObject::tr("保存恢复密钥"), QStringLiteral("NotepadSafe-Recovery-Key.txt"),
            QObject::tr("文本文件 (*.txt)"));
        if (path.isEmpty())
            return;
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        QByteArray bytes = recoveryKey.toUtf8();
        const qint64 expected = bytes.size();
        const bool opened = output.open(QIODevice::WriteOnly);
        const qint64 written = opened ? output.write(bytes) : -1;
        scrubByteArray(bytes);
        if (!opened || written != expected || !output.commit()) {
            QMessageBox::critical(&dialog, QObject::tr("保存失败"),
                                  QObject::tr("无法保存恢复密钥文件。"));
            return;
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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
    connect(filterEdit_, &QLineEdit::textChanged, this, &VaultDock::applyFilter);
    connect(sortCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        refreshEntries(currentEntryId_);
    });
    connect(healthButton_, &QPushButton::clicked,
            this, &VaultDock::showPasswordHealthReport);
    connect(entryTree_, &QTreeWidget::currentItemChanged,
            this, &VaultDock::onCurrentItemChanged);
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
