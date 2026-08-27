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
        || currentKind_ != EntryKind::Login || currentEntryRecycled_
        || !currentEntryPersisted_) {
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
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || currentKind_ != EntryKind::Login) {
        return false;
    }
    if (titleEdit_->text().trimmed().isEmpty()) {
        if (showSuccess)
            QMessageBox::warning(this, tr("无法保存"), tr("标题不能为空。"));
        titleEdit_->setFocus();
        return false;
    }

    VaultEntryData data;
    data.id = currentEntryId_;
    data.title = titleEdit_->text().trimmed();
    data.username = usernameEdit_->text();
    data.url = urlEdit_->text();
    data.tags.clear();
    const QStringList requestedTags = tagsEdit_->text().split(
        QRegularExpression(QStringLiteral("[,，;；]")), Qt::SkipEmptyParts);
    for (const QString &rawTag : requestedTags) {
        const QString tag = rawTag.trimmed();
        if (!tag.isEmpty() && !data.tags.contains(tag, Qt::CaseInsensitive))
            data.tags.push_back(tag);
    }
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
    const QString entryKind = currentKind_ == EntryKind::SecureNote
        ? tr("旧版笔记") : tr("账号");
    const QUuid entryId = currentEntryId_;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        permanentDelete ? tr("正在永久删除%1").arg(entryKind)
                        : tr("正在移动%1到回收站").arg(entryKind),
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
    emit userNotificationRequested(permanentDelete ? tr("%1已永久删除").arg(entryKind)
                                                    : tr("%1已移到回收站").arg(entryKind),
                                   4000);
}

void VaultDock::restoreCurrentEntry()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || !currentEntryRecycled_) {
        return;
    }
    const QUuid restoredId = currentEntryId_;
    const QString entryKind = currentKind_ == EntryKind::SecureNote
        ? tr("旧版笔记") : tr("账号");
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::VaultOperation, AsyncTaskPriority::Highest,
        tr("正在恢复%1").arg(entryKind),
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
    emit userNotificationRequested(tr("%1已从回收站恢复").arg(entryKind), 4000);
}

void VaultDock::showEntryHistory()
{
    if (service_.state() != VaultState::Open || currentEntryId_.isNull()
        || currentKind_ != EntryKind::Login || editorEditing_
        || currentEntryRecycled_) {
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
    editorEditing_ = editing && currentKind_ == EntryKind::Login
                     && !currentEntryRecycled_;
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
    editButton_->setVisible(available && currentKind_ == EntryKind::Login
                            && !currentEntryRecycled_ && currentEntryPersisted_
                            && !editorEditing_);
    editButton_->setEnabled(editButton_->isVisible());
    historyButton_->setVisible(available && currentKind_ == EntryKind::Login
                               && !currentEntryRecycled_
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
        kindLabel_->setText(currentEntryPersisted_ ? tr("编辑账号") : tr("新建账号"));
    } else {
        kindLabel_->setText(currentKind_ == EntryKind::SecureNote ? tr("旧版笔记（只读）")
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
