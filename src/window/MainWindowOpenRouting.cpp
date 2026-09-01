#include "MainWindow.h"

#include "AppComposition.h"
#include "DocumentOpenWorkflow.h"
#include "EditorTab.h"
#include "FileContentProbe.h"
#include "FileCredentialDialog.h"
#include "SecureDocumentContract.h"
#include "SecuritySessionPort.h"
#include "VaultDock.h"

#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>

EditorTab *MainWindow::findOpenPlainDocument(const QString &path) const
{
    // Normal opens and clean-session restores share one bounded disk identity
    // lookup. Dirty recovery payloads deliberately do not use this lookup.
    DiskFileVersion candidate;
    const DiskFileState candidateState = captureDiskFileVersion(path, &candidate);
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab || tab->isSecureDocument())
            continue;
        if (sameDiskFilePath(tab->filePath(), path)
            || (candidateState == DiskFileState::Unchanged
                && tab->representsDiskFile(candidate))) {
            return tab;
        }
    }
    return nullptr;
}

bool MainWindow::openPathUnified(const QString &path, EditorTab *reuseTab,
                                  std::optional<TextEncoding> forcedEncoding,
                                  OpenPresentationPolicy presentation,
                                  OpenAuthorizationPolicy authorization,
                                  QString *failureReason,
                                  bool *securityRefused)
{
    if (failureReason)
        failureReason->clear();
    if (securityRefused)
        *securityRefused = false;
    const QFileInfo info(path);
    if (!info.isFile()) {
        if (failureReason) {
            *failureReason = !info.exists()
                ? QStringLiteral("文件不存在。请检查文件的位置和名称后重试。")
                : info.isDir()
                    ? QStringLiteral("这是文件夹，不能作为文件打开。请在文件夹中选择具体文件。")
                    : QStringLiteral("此路径不是可打开的普通文件。请选择具体文件后重试。");
        }
        return false;
    }
    const bool preserveWorkspace =
        presentation == OpenPresentationPolicy::PreserveCurrentWorkspace
        && m_workspaceStack && m_tabs
        && m_workspaceStack->currentWidget() != m_tabs;
    const auto persistenceAdapter =
        m_composition->persistenceAdapters().resolve(path);
    if (persistenceAdapter
        && persistenceAdapter->persistence == DocumentPersistence::SecureFile) {
        // Authorization is independent of presentation and must be checked
        // against this final route, even if an earlier probe saw a plain file.
        if (authorization == OpenAuthorizationPolicy::DenyInteractiveUnlock) {
            if (securityRefused)
                *securityRefused = true;
            if (failureReason) {
                *failureReason = QStringLiteral(
                    "为保护安全文件，外部请求不会解锁；请在窗口内用文件→打开。");
            }
            return false;
        }
        ensureSecurityServicesCreated();
        if (!m_securePersistence || !m_securePersistence->available()) {
            QMessageBox::critical(this, QStringLiteral("安全文件不可用"),
                                  QStringLiteral("当前构建未包含 KDBX 安全后端。"));
            return false;
        }
        QString documentType;
        QUuid documentId;
        SecurityIdentityParameters fileParameters;
        const VaultError probe = m_securePersistence->inspectFile(
            path, &documentType, &documentId, &fileParameters);
        if (!probe.isError() && documentType == QStringLiteral("SecureDocument")) {
            // A copied or hard-linked .nsafe still carries the same document id.
            // Focus its live editor instead of asking the service to open a
            // second competing handle to the same logical document.
            if (!reuseTab && !documentId.isNull()) {
                for (int i = 0; i < m_tabs->count(); ++i) {
                    auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
                    if (!openTab || openTab->secureDocumentId() != documentId)
                        continue;
                    showDocumentWorkspace();
                    m_tabs->setCurrentIndex(i);
                    openTab->setFocusEditor();
                    updateRecentFiles(path);
                    statusBar()->showMessage(QStringLiteral("文件已在当前窗口中打开"), 2000);
                    return true;
                }
            }
            return openSecurePath(path, reuseTab);
        }
        if (probe.isError()
            && probe.diagnosticCode != QStringLiteral("identity.envelope.missing")) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("安全文件已损坏"),
                            probe.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(probe.diagnosticCode);
            box.exec();
            return false;
        }

        const bool managedAccounts = !probe.isError()
                                     && documentType == QStringLiteral("Accounts");
        // Account KDBX routing needs its adapter before credentials are
        // applied. The workspace remains lazy for ordinary startup/documents,
        // but file-open is a legitimate first-use trigger.
        ensureAccountsWorkspaceCreated();
        if (!ensureSecuritySessionInteractive())
            return false;
        if (managedAccounts) {
            SecureBuffer rememberedMaster;
            VaultError sessionOpen = m_securitySession->acquireRememberedMasterKey(
                fileParameters, &rememberedMaster);
            if (!sessionOpen.isError()) {
                sessionOpen = m_vaultDock->unlockManagedStoreWithMasterKey(
                    path, rememberedMaster);
            }
            rememberedMaster.clear();
            if (sessionOpen.isError()) {
                SecureBuffer rememberedRecovery;
                VaultError recoveryOpen =
                    m_securitySession->acquireRememberedRecoverySecret(
                        fileParameters, &rememberedRecovery);
                if (!recoveryOpen.isError()) {
                    recoveryOpen = m_vaultDock->unlockManagedStoreWithRecoverySecret(
                        path, rememberedRecovery);
                }
                rememberedRecovery.clear();
                sessionOpen = recoveryOpen;
            }
            if (!sessionOpen.isError()) {
                updateRecentFiles(path);
                return showAccountsWorkspace();
            }
            if (sessionOpen.code != VaultErrorCode::InvalidCredentials) {
                QMessageBox box(QMessageBox::Critical,
                                QStringLiteral("无法打开账户集合"),
                                sessionOpen.userMessage, QMessageBox::Ok, this);
                box.setDetailedText(sessionOpen.diagnosticCode);
                box.exec();
                return false;
            }
        }

        FileCredentialDialog dialog(
            managedAccounts ? QStringLiteral("打开受其他密码保护的账户集合")
                            : QStringLiteral("打开兼容 KDBX"),
            managedAccounts
                ? QStringLiteral("当前会话不能解锁此账户文件。选择该文件的主密码或恢复密钥。")
                : QStringLiteral("输入此 KDBX 文件的主密码。"),
            managedAccounts, this);
        while (dialog.exec() == QDialog::Accepted) {
            VaultError error;
            if (dialog.kind() == FileCredentialDialog::Kind::RecoveryKey) {
                QString recoveryKey = dialog.takeRecoveryKey();
                if (recoveryKey.isEmpty()) {
                    dialog.showError(QStringLiteral("请输入恢复密钥。"));
                    continue;
                }
                SecureBuffer recoverySecret;
                error = m_securitySession->decodeRecoveryKey(
                    recoveryKey, &recoverySecret);
                if (!error.isError()) {
                    error = m_vaultDock->unlockManagedStoreWithRecoverySecret(
                        path, recoverySecret);
                }
                if (!error.isError()) {
                    error = m_securitySession->rememberRecoverySecret(
                        fileParameters, recoverySecret);
                }
                recoverySecret.clear();
                recoveryKey.fill(QChar(0));
                recoveryKey.clear();
            } else {
                SecureBuffer buffer = dialog.takePassphrase();
                if (buffer.empty()) {
                    dialog.showError(QStringLiteral("请输入文件密码。"));
                    continue;
                }
                if (managedAccounts) {
                    SecureBuffer masterUnlockKey;
                    error = m_securitySession->deriveMasterUnlockKey(
                        buffer, fileParameters, &masterUnlockKey);
                    if (!error.isError()) {
                        error = m_vaultDock->unlockManagedStoreWithMasterKey(
                            path, masterUnlockKey);
                    }
                    if (!error.isError()) {
                        error = m_securitySession->rememberMasterKey(
                            fileParameters, masterUnlockKey);
                    }
                    masterUnlockKey.clear();
                } else {
                    error = m_vaultDock->unlockVault(path, buffer);
                }
                buffer.clear();
            }
            if (!error.isError()) {
                updateRecentFiles(path);
                return showAccountsWorkspace();
            }
            dialog.showError(error.userMessage);
        }
        return false;
    }
    // Normal Open is idempotent.  Probe at most 192 KiB regardless of file
    // size, then compare against the baselines already captured by each tab.
    // Reload deliberately supplies reuseTab and must re-read that tab instead.
    if (!reuseTab) {
        if (EditorTab *openTab = findOpenPlainDocument(path)) {
            if (!preserveWorkspace) {
                showDocumentWorkspace();
                m_tabs->setCurrentWidget(openTab);
                openTab->setFocusEditor();
            }
            updateRecentFiles(path);
            statusBar()->showMessage(
                preserveWorkspace
                    ? QStringLiteral("文件已在文档标签中打开；当前工作区保持不变。")
                    : QStringLiteral("文件已在当前窗口中打开"),
                2000);
            return true;
        }
    }

    // An already open document above does not need to read its source again.
    // Only a new/reloaded provider needs actual read access; a failed read
    // does not by itself establish that permissions are the cause.
    {
        QFile readable(path);
        if (!readable.open(QIODevice::ReadOnly)) {
            if (failureReason) {
                *failureReason = QStringLiteral(
                    "当前无法读取此文件。请检查读取权限，确认文件未被其他程序独占后重试。");
            }
            return false;
        }
    }
    if (!preserveWorkspace)
        showDocumentWorkspace();
    EditorTab *tab = reuseTab ? reuseTab : addNewTab(preserveWorkspace ? OpenPresentationPolicy::PreserveCurrentWorkspace : OpenPresentationPolicy::FocusDocumentWorkspace);
    if (reuseTab && reuseTab->isSecureDocument()) {
        if (m_securePersistence)
            closeSecureDocumentOnSecurityLane(reuseTab->secureDocumentId());
        // External Open may reuse the single empty default secure draft. The
        // reused tab must stop being a secure document before ordinary routing.
        reuseTab->convertToPlainDocument(QString());
    }
    const bool opened = openDocumentByPolicy(
        m_composition->documentProviders(), tab, path, forcedEncoding);
    if (!opened) {
        if (failureReason) {
            *failureReason = QStringLiteral(
                "未能打开此文件。请确认文件可用后重试；若仍失败，请在窗口内用文件→打开检查。");
        }
        if (!reuseTab) {
            const int index = m_tabs->indexOf(tab);
            if (index >= 0)
                m_tabs->removeTab(index);
            delete tab;
        }
        return false;
    }
    updateRecentFiles(path);
    m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    if (tab == currentTab()) {
        syncGlobalMenuCheckStates();
        updateStatusBar();
    }
    if (preserveWorkspace) {
        statusBar()->showMessage(
            QStringLiteral("文件已在后台文档标签中打开；当前工作区保持不变。"),
            3000);
    }
    return true;
}
