#include "MainWindow.h"
#include "EditorTab.h"
#include "FileContentProbe.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SearchResultsController.h"
#include "SessionManager.h"
#include "TextEncoding.h"
#include "FileTierPolicy.h"
#include "VirtualFileView.h"
#include "StatusBarTaskWidget.h"
#include "AsyncSerialQueue.h"
#include "AsyncUiOperation.h"
#include "VaultDock.h"
#include "SecureDocumentService.h"
#include "SecurityServiceFactory.h"
#include "SecuritySessionService.h"
#include "AppComposition.h"
#include "FileCredentialDialog.h"
#include "SensitiveText.h"
#include "CommandRegistry.h"
#include "DocumentOpenWorkflow.h"

#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStyle>
#include <QStatusBar>
#include <QLabel>
#include <QToolButton>
#include <QToolBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QFont>
#include <QPalette>
#include <QStyleHints>
#include <QIcon>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QShortcut>
#include <QSignalBlocker>
#include <QKeySequence>
#include <QKeyEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QUuid>
#include <QPointer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QProcess>
#include <QLocale>
#include <QEventLoop>
#include <QProgressDialog>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#ifndef NOTEPADSAFE_VERSION
#define NOTEPADSAFE_VERSION "0.0.0-dev"
#endif

#ifdef Q_OS_WIN
#include <Windows.h>
#include <WtsApi32.h>
#endif

// ============ 文件操作 ============

void MainWindow::ensureSecurityServicesCreated()
{
    if (m_securitySession && m_secureDocuments)
        return;
    SecurityServiceBundle services = m_composition->createSecurityServices();
    m_securitySession = std::move(services.session);
    m_secureDocuments = std::move(services.documents);
}
void MainWindow::ensureFindDialogCreated()
{
    if (m_findDialog)
        return;
    m_composition->recordHeavyFactoryCall();
    m_findDialog = new FindReplaceDialog(this);
    m_findDialog->setSearchResultsDock(m_searchDock);
    m_findDialog->setTabWidget(m_tabs);
    m_findDialog->setActiveTab(currentTab());
    connect(m_findDialog, &FindReplaceDialog::findParametersChanged,
            this, [this](const QString &pattern, bool regex, bool cs, bool wo) {
        m_lastFindPattern = pattern;
        m_lastFindRegex = regex;
        m_lastFindCase = cs;
        m_lastFindWord = wo;
    });
}

bool MainWindow::ensureSecuritySessionInteractive()
{
    ensureSecurityServicesCreated();
    if (!m_securitySession || !m_securitySession->available()
        || !m_secureDocuments || !m_secureDocuments->available()) {
        QMessageBox::critical(this, QStringLiteral("安全功能不可用"),
                              QStringLiteral("当前构建未包含 KDBX 安全后端。"));
        return false;
    }
    const auto restoreMaskedTabs = [this]() {
        if (m_secureDocuments) {
            const int activeIndex = m_tabs ? m_tabs->currentIndex() : -1;
            for (int i = 0; m_tabs && i < m_tabs->count(); ++i) {
                auto *masked = qobject_cast<EditorTab *>(m_tabs->widget(i));
                if (!masked || !masked->isSecureDocument()
                    || !masked->isSecureDocumentMasked()) {
                    continue;
                }
                const QString backing = masked->filePath().isEmpty()
                                            ? masked->autosavePath() : masked->filePath();
                if (backing.isEmpty() || !QFileInfo::exists(backing))
                    continue;
                SecureDocumentSnapshot restored;
                const VaultError restoreError = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                    QStringLiteral("正在恢复安全文档"),
                    [this, backing, &restored] {
                        return m_secureDocuments->open(backing, &restored);
                    });
                if (!restoreError.isError()) {
                    adoptSecureSnapshot(restored, masked);
                    restored.text.fill(QChar(0));
                }
            }
            if (activeIndex >= 0 && activeIndex < m_tabs->count())
                m_tabs->setCurrentIndex(activeIndex);
        }
    };
    if (m_securitySession && m_securitySession->hasActiveSession()) {
        restoreMaskedTabs();
        return true;
    }
    if (!m_securitySession)
        return false;

    const bool creating = !m_securitySession->hasIdentity();
    QDialog dialog(this);
    dialog.setWindowTitle(creating ? QStringLiteral("建立本地安全身份")
                                   : QStringLiteral("登录 Notepad Safe"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(
        creating
            ? QStringLiteral("设置一个主密码。文件始终离线加密；本设备登录后 7 天内使用无提示。")
            : QStringLiteral("输入主密码以开始新的 7 天本机会话。"),
        &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto *form = new QFormLayout();
    auto *password = new QLineEdit(&dialog);
    password->setObjectName(QStringLiteral("securitySessionPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("主密码"), password);
    QLineEdit *confirmation = nullptr;
    if (creating) {
        confirmation = new QLineEdit(&dialog);
        confirmation->setObjectName(QStringLiteral("securitySessionConfirmation"));
        confirmation->setEchoMode(QLineEdit::Password);
        form->addRow(QStringLiteral("确认主密码"), confirmation);
    }
    layout->addLayout(form);
    auto *errorLabel = new QLabel(&dialog);
    errorLabel->setStyleSheet(QStringLiteral("color:#b00020"));
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(creating ? QStringLiteral("建立身份")
                                                            : QStringLiteral("登录"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    password->setFocus();

    while (dialog.exec() == QDialog::Accepted) {
        QString passwordText = takeSensitiveLineEdit(password);
        QString confirmationText = creating && confirmation
            ? takeSensitiveLineEdit(confirmation) : QString();
        if (passwordText.size() < 12) {
            errorLabel->setText(QStringLiteral("主密码至少需要 12 个字符。"));
            passwordText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            password->setFocus();
            continue;
        }
        if (creating && confirmation && passwordText != confirmationText) {
            errorLabel->setText(QStringLiteral("两次输入的主密码不一致。"));
            passwordText.fill(QChar(0));
            confirmationText.fill(QChar(0));
            password->setFocus();
            continue;
        }
        QByteArray passwordUtf8 = passwordText.toUtf8();
        passwordText.fill(QChar(0));
        confirmationText.fill(QChar(0));
        SecureBuffer passphrase(passwordUtf8.constData(), passwordUtf8.size());
        scrubByteArray(passwordUtf8);
        QString recoveryKey;
        VaultError result = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
            creating ? QStringLiteral("正在建立安全身份")
                     : QStringLiteral("正在解锁安全身份"),
            [this, creating, &passphrase, &recoveryKey] {
                return creating
                    ? m_securitySession->createIdentity(passphrase, &recoveryKey)
                    : m_securitySession->unlockIdentity(passphrase);
            });
        passphrase.clear();
        if (result.isError()) {
            errorLabel->setText(result.userMessage);
            password->setFocus();
            continue;
        }

        if (creating) {
            QDialog recoveryDialog(this);
            recoveryDialog.setWindowTitle(QStringLiteral("保存恢复密钥"));
            recoveryDialog.setMinimumWidth(620);
            auto *recoveryLayout = new QVBoxLayout(&recoveryDialog);
            auto *recoveryMessage = new QLabel(
                QStringLiteral("恢复密钥是主密码丢失后的唯一恢复方式。请保存到本设备之外；"
                               "Notepad Safe 和任何厂商都无法替你找回。"),
                &recoveryDialog);
            recoveryMessage->setWordWrap(true);
            recoveryLayout->addWidget(recoveryMessage);
            auto *keyView = new QPlainTextEdit(recoveryKey, &recoveryDialog);
            keyView->setReadOnly(true);
            keyView->setMaximumHeight(96);
            keyView->setObjectName(QStringLiteral("securityRecoveryKey"));
            recoveryLayout->addWidget(keyView);
            auto *verify = new QLineEdit(&recoveryDialog);
            verify->setPlaceholderText(QStringLiteral("输入恢复密钥最后 8 个字符以确认已保存"));
            recoveryLayout->addWidget(verify);
            auto *finish = new QDialogButtonBox(QDialogButtonBox::Ok,
                                                 Qt::Horizontal, &recoveryDialog);
            finish->button(QDialogButtonBox::Ok)->setText(QStringLiteral("完成"));
            finish->button(QDialogButtonBox::Ok)->setEnabled(false);
            recoveryLayout->addWidget(finish);
            const QString suffix = recoveryKey.right(8);
            connect(verify, &QLineEdit::textChanged, finish->button(QDialogButtonBox::Ok),
                    [button = finish->button(QDialogButtonBox::Ok), suffix](const QString &text) {
                        button->setEnabled(text.trimmed().compare(suffix,
                                                                  Qt::CaseInsensitive) == 0);
                    });
            connect(finish, &QDialogButtonBox::accepted,
                    &recoveryDialog, &QDialog::accept);
            recoveryDialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
            while (recoveryDialog.exec() != QDialog::Accepted) {
                // Identity creation is already durable at this point. Do not let a
                // dismissed dialog silently skip the only recovery-key handoff.
            }
            keyView->clear();
            clearSensitiveLineEdit(verify);
            recoveryKey.fill(QChar(0));
            recoveryKey.clear();
        }
        restoreMaskedTabs();
        return true;
    }
    clearSensitiveLineEdit(password);
    if (confirmation)
        clearSensitiveLineEdit(confirmation);
    return false;
}

void MainWindow::newFile()
{
    ensureSecurityServicesCreated();
    if (!m_secureDocuments || !m_secureDocuments->available()) {
        addNewTab();
        return;
    }
    if (!ensureSecuritySessionInteractive())
        return;
    SecureDocumentSnapshot snapshot;
    const VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
        QStringLiteral("正在建立加密草稿"),
        [this, &snapshot] { return m_secureDocuments->newDraft(&snapshot); });
    if (error.isError()) {
        QMessageBox::critical(this, QStringLiteral("无法新建文档"), error.userMessage);
        return;
    }
    auto *tab = addNewTab();
    tab->initializeSecureDocument(snapshot.documentId, snapshot.title,
                                  snapshot.text, snapshot.filePath);
    tab->setAutosavePath(snapshot.backingPath);
    m_tabs->setTabText(m_tabs->indexOf(tab), tab->tabTitle());
    statusBar()->showMessage(QStringLiteral("已新建文档"), 1800);
}

EditorTab *MainWindow::addNewTab()
{
    showDocumentWorkspace();
    auto *tab = new EditorTab(this);
    int index = m_tabs->addTab(tab, QStringLiteral("未命名"));

    // 注册拖放代理：保证拖到这个 EditorTab/editor/viewport 任意位置都能触发 MainWindow 打开
    registerDropProxy(tab);
    connect(tab, &EditorTab::externalDropSurfaceCreated,
            this, &MainWindow::registerDropProxy);

    // 应用全局设置（标尺 + 换行）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);
    applyEditorPreferences(tab);
    // QScintilla keeps its own colors outside the QWidget palette. Apply the
    // resolved appearance to newly created/restored tabs as well as existing ones.
    applyDarkMode(m_darkMode);

    connect(tab, &EditorTab::modificationChanged, this, [this, tab](bool m) {
        if (m && tab->isSecureDocument() && !tab->isSecureDocumentTransitioning()) {
            tab->setSecureDocumentTitle(secureTitleFrom(tab));
            m_secureAutosaveTimer.start(5000);
        }
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0) {
            onTabModifiedChanged(m);
            m_tabs->setTabText(idx, tab->tabTitle());
        }
        if (currentTab() == tab)
            syncCommandStates(tab);
    });
    connect(tab, &EditorTab::commandCapabilitiesChanged,
            this, [this, tab]() {
        if (currentTab() == tab)
            syncCommandStates(tab);
    });
    connect(tab, &EditorTab::largeDocumentRuntimeStateChanged,
            this, [this, tab](EditorTab::LargeDocumentRuntimeState, const QString &) {
        if (currentTab() != tab)
            return;
        syncCommandStates(tab);
        updateStatusBar();
    });
    connect(tab, &EditorTab::contentChanged, this, [this, tab](quint64) {
        if (currentTab() == tab)
            m_characterStatusTimer.start();
        if (m_searchDock)
            m_searchDock->invalidateResultsFor(tab->documentState().id);
    });
    connect(tab, &EditorTab::characterCountChanged, this, [this, tab]() {
        if (currentTab() == tab)
            updateStatusBar();
    });
    connect(tab, &EditorTab::contentChanged, this, [this, tab](quint64) {
        if (!tab->isSecureDocument() || tab->isBinaryDocument()
            || tab->isSecureDocumentMasked()
            || tab->isSecureDocumentTransitioning()
            || tab->securePayloadLimitExclusive() <= 0
            || tab->editor()->length() < tab->securePayloadLimitExclusive()
            || tab->property("securePayloadLimitUndoPending").toBool()) {
            return;
        }
        tab->setProperty("securePayloadLimitUndoPending", true);
        QPointer<EditorTab> guarded(tab);
        QTimer::singleShot(0, tab, [this, guarded]() {
            if (!guarded)
                return;
            guarded->setProperty("securePayloadLimitUndoPending", false);
            if (!guarded->isSecureDocument() || guarded->isBinaryDocument()
                || guarded->editor()->length()
                       < guarded->securePayloadLimitExclusive()) {
                return;
            }
            guarded->undo();
            const bool restored = guarded->editor()->length()
                                  < guarded->securePayloadLimitExclusive();
            if (!restored)
                guarded->editor()->setReadOnly(true);
            statusBar()->showMessage(
                restored
                    ? QStringLiteral("安全文档必须小于 32 MiB；本次编辑已撤销。请另存为普通文件后继续。")
                    : QStringLiteral("安全文档已达到大小上限并已停止编辑；请另存为普通文件。"),
                8000);
            QApplication::beep();
        });
    });
    connect(tab, &EditorTab::cursorPositionChanged, this, &MainWindow::onTabCursorPositionChanged);
    connect(tab, &EditorTab::titleChanged, this, [this, tab](const QString &) {
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0)
            m_tabs->setTabText(idx, tab->tabTitle());
    });
    connect(tab, &EditorTab::encodingChanged, this, &MainWindow::onTabEncodingChanged);
    connect(tab, &EditorTab::syntaxChanged, this, [this](Syntax) {
        updateStatusBar();
    });
    // 秒开：L2 后台行索引构建进度 → 非模态状态栏显示（只更新当前激活 tab 的进度）
    connect(tab, &EditorTab::l2LineBuildProgress,
            this, [this, tab](qint64 done, qint64 total) {
        if (currentTab() != tab) return;
        if (tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
            statusBar()->clearMessage();
            return;
        }
        if (total <= 0) {
            statusBar()->clearMessage();
            return;
        }
        int pct = qBound(0, int(done * 100 / total), 100);
        QString msg = QObject::tr("正在建立行索引… %1%  (%2 / %3 MB)")
            .arg(pct)
            .arg(static_cast<double>(done) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(static_cast<double>(total) / (1024.0 * 1024.0), 0, 'f', 1);
        statusBar()->showMessage(msg, 0);
    });
    connect(tab, &EditorTab::l2LineBuildFinished, this, [this, tab](bool ok) {
        if (ok && m_searchDock && tab->isL2Mode() && tab->l2View())
            m_searchDock->decorateResultsFor(
                tab->documentState().id, tab->l2View()->lineIndex());
        if (currentTab() != tab) return;
        if (tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
            statusBar()->clearMessage();
            return;
        }
        if (ok) statusBar()->showMessage(QObject::tr("行索引构建完成"), 3000);
        else    statusBar()->clearMessage();
    });
    connect(tab, &EditorTab::l2FileOpenedChanged, this, [this, tab](bool) {
        if (currentTab() == tab)
            updateStatusBar();
    });
    connect(tab, &EditorTab::l2SaveNeededChanged, this, [this, tab](bool) {
        if (currentTab() == tab)
            updateStatusBar();
    });

    // ============== L2 禁用不适配菜单：同一 tab 内部 L1↔L2 切换时立即同步（关键：addTab/currentChanged 不触发）==============
    // 最小侵入：直接用 sender() 转 EditorTab* 当 tab 参数（精准指向信号发射源，避免无参
    // syncCommandStates() → currentTab() 在 currentIndex 未切好时查到旧 tab → 新 tab 的
    // L2 状态被旧 tab 覆盖（enabled=true 不变灰的 D001/D003/D007 根因）。
    connect(tab, &EditorTab::l2ModeStatusChanged, this, [this]() {
        auto* src = qobject_cast<EditorTab*>(sender());
        syncCommandStates(src ? src : currentTab());
        if (!src || src == currentTab())
            syncGlobalMenuCheckStates();
    });

    m_tabs->setCurrentIndex(index);
    if (m_findDialog) {
        m_findDialog->setActiveTab(tab);
        m_findDialog->notifyTabOpened(tab);
    }
    // 新建后立刻同步一次 L2 敏感动作（覆盖新 tab 还没收到任何信号的早期空状态）
    syncCommandStates(tab);
    return tab;
}

void MainWindow::applyEditorPreferences(EditorTab *tab)
{
    if (!tab)
        return;
    tab->setWhitespaceVisible(m_whitespaceVisible);
    tab->setEolVisible(m_eolVisible);
    tab->setUseTabs(m_useTabs);
    tab->setTabWidth(m_tabWidth);
    tab->setBackspaceUnindents(m_backspaceUnindents);
    tab->setWordCompletionEnabled(m_wordCompletion);
}

QString MainWindow::secureTitleFrom(EditorTab *tab)
{
    if (!tab || !tab->editor())
        return QStringLiteral("未命名");
    if (tab->isBinaryDocument()) {
        if (tab->isSecureDocument() && !tab->secureDocumentTitle().trimmed().isEmpty()
            && tab->secureDocumentTitle() != QStringLiteral("未命名")) {
            return tab->secureDocumentTitle();
        }
        if (!tab->filePath().isEmpty())
            return QFileInfo(tab->filePath()).completeBaseName().left(80);
        return QStringLiteral("未命名");
    }
    const int lines = qMin(tab->editor()->lines(), 8);
    for (int i = 0; i < lines; ++i) {
        QString line = tab->editor()->text(i).trimmed();
        while (line.startsWith(QLatin1Char('#')))
            line.remove(0, 1);
        line = line.trimmed();
        if (!line.isEmpty())
            return line.left(80);
    }
    return QStringLiteral("未命名");
}

bool MainWindow::saveSecureTab(EditorTab *tab, bool allowInteractiveUnlock, bool showErrors)
{
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments || !m_securitySession)
        return false;
    if (tab->isSecureDocumentMasked()) {
        if (showErrors)
            statusBar()->showMessage(QStringLiteral("加密文档已锁定，请先解锁后再保存。"), 5000);
        return false;
    }
    if (!m_securitySession->hasActiveSession()) {
        if (!allowInteractiveUnlock || !ensureSecuritySessionInteractive()) {
            QString recoveryError;
            checkpointSecureTab(tab, &recoveryError);
            if (!showErrors && !recoveryError.isEmpty()) {
                statusBar()->showMessage(
                    QStringLiteral("安全文档未保存，且无法建立加密恢复点：%1")
                        .arg(recoveryError), 8000);
            }
            return false;
        }
    }

    const DiskFileState diskState = m_secureDocuments->externalFileState(
        tab->secureDocumentId());
    if (diskState == DiskFileState::Changed
        || diskState == DiskFileState::Missing
        || diskState == DiskFileState::Unreadable) {
        if (!allowInteractiveUnlock) {
            QString recoveryError;
            checkpointSecureTab(tab, &recoveryError);
            statusBar()->showMessage(
                recoveryError.isEmpty()
                    ? QStringLiteral("安全文档已在磁盘上变化；最新内容已写入本机加密恢复点。")
                    : QStringLiteral("安全文档已在磁盘上变化，且恢复点写入失败。"),
                8000);
            return false;
        }
        const ExternalSaveDisposition disposition = resolveSecureExternalSaveConflict(tab);
        return disposition == ExternalSaveDisposition::HandledSuccess;
    }

    const QString title = secureTitleFrom(tab);
    // An unnamed secure note already owns an encrypted .nsafe-draft backing
    // file. Ordinary Save commits that same persistence kind silently; only
    // Save As chooses a portable path or converts the document format.
    const QString targetPath;
    VaultError error;
    if (tab->isSecureBinaryDocument()) {
        QByteArray body;
        QString exportError;
        if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
            if (showErrors)
                QMessageBox::critical(this, QStringLiteral("加密保存失败"), exportError);
            return false;
        }
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave,
            showErrors ? AsyncTaskPriority::High : AsyncTaskPriority::Low,
            showErrors ? QStringLiteral("正在安全保存")
                       : QStringLiteral("正在保存加密恢复点"),
            [this, tab, &body, title, targetPath, showErrors] {
                return m_secureDocuments->saveBinary(
                    tab->secureDocumentId(), body, title, targetPath, showErrors);
            }, showErrors);
        scrubByteArray(body);
    } else {
        QString body = tab->editor()->text();
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave,
            showErrors ? AsyncTaskPriority::High : AsyncTaskPriority::Low,
            showErrors ? QStringLiteral("正在安全保存")
                       : QStringLiteral("正在保存加密恢复点"),
            [this, tab, &body, title, targetPath, showErrors] {
                return m_secureDocuments->save(
                    tab->secureDocumentId(), body, title, targetPath, showErrors);
            }, showErrors);
        if (!body.isEmpty())
            body.fill(QChar(0));
        body.clear();
    }
    if (error.isError()) {
        QString recoveryError;
        const bool recovered = checkpointSecureTab(tab, &recoveryError);
        if (error.diagnosticCode.startsWith(
                QStringLiteral("secure-document.external-change"))) {
            if (allowInteractiveUnlock) {
                const ExternalSaveDisposition disposition =
                    resolveSecureExternalSaveConflict(tab);
                return disposition == ExternalSaveDisposition::HandledSuccess;
            }
            statusBar()->showMessage(
                recovered
                    ? QStringLiteral("安全文档已在磁盘上变化；最新内容已写入本机加密恢复点。")
                    : QStringLiteral("安全文档已在磁盘上变化，且恢复点写入失败。"),
                8000);
            return false;
        }
        if (showErrors) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("加密保存失败"),
                            error.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(error.diagnosticCode);
            box.exec();
        }
        if (!showErrors) {
            statusBar()->showMessage(
                recovered
                    ? QStringLiteral("自动保存失败；最新内容已加密恢复，将在稍后重试。")
                    : QStringLiteral("自动保存及本机加密恢复点均失败，请立即手动保存。"),
                8000);
        }
        return false;
    }
    tab->setSecureDocumentTitle(title);
    tab->setAutosavePath(m_secureDocuments->backingPath(tab->secureDocumentId()));
    tab->setModified(false);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabText(index, tab->tabTitle());
    statusBar()->showMessage(showErrors ? QStringLiteral("已保存")
                                        : QStringLiteral("已自动保存"), 2000);
    return true;
}

bool MainWindow::checkpointSecureTab(EditorTab *tab, QString *errorMessage)
{
    if (errorMessage)
        errorMessage->clear();
    if (!tab || !tab->isSecureDocument() || tab->isSecureDocumentMasked()
        || !m_secureDocuments) {
        if (errorMessage)
            *errorMessage = QStringLiteral("文档当前不可恢复。");
        return false;
    }

    VaultError error;
    if (tab->isSecureBinaryDocument()) {
        QByteArray body;
        QString exportError;
        if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
            if (errorMessage)
                *errorMessage = exportError;
            return false;
        }
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
            QStringLiteral("正在建立本机加密恢复点"),
            [this, tab, &body] {
                return m_secureDocuments->checkpointBinary(
                    tab->secureDocumentId(), body);
            }, false);
        scrubByteArray(body);
    } else {
        QString body = tab->editor()->text();
        error = runSerializedSecurityOperation(
            this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
            QStringLiteral("正在建立本机加密恢复点"),
            [this, tab, &body] {
                return m_secureDocuments->checkpointText(
                    tab->secureDocumentId(), body);
            }, false);
        if (!body.isEmpty())
            body.fill(QChar(0));
        body.clear();
    }
    if (error.isError() && errorMessage)
        *errorMessage = error.userMessage;
    return !error.isError();
}

void MainWindow::closeSecureDocumentOnSecurityLane(const QUuid &documentId)
{
    if (!m_secureDocuments || documentId.isNull())
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在关闭安全文档"),
        [this, documentId] {
            m_secureDocuments->close(documentId);
            return VaultError{};
        },
        false);
}

void MainWindow::closeAllSecureDocumentsOnSecurityLane()
{
    if (!m_secureDocuments)
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在清理安全文档"),
        [this] {
            m_secureDocuments->closeAll();
            return VaultError{};
        },
        false);
}

void MainWindow::endSecuritySessionOnSecurityLane(bool removeDeviceSession)
{
    if (!m_securitySession)
        return;
    (void)runSerializedSecurityOperation(
        this, AsyncTaskType::SecureIdentity, AsyncTaskPriority::Highest,
        QStringLiteral("正在结束安全会话"),
        [this, removeDeviceSession] {
            m_securitySession->endSession(removeDeviceSession);
            return VaultError{};
        },
        false);
}

void MainWindow::lockSecureDocuments()
{
    lockSecureDocumentsImpl(false);
}

void MainWindow::lockSecureDocumentsImpl(bool sessionExpired)
{
    m_secureAutosaveTimer.stop();
    bool accountChangesDiscarded = false;
    bool secureChangesDiscarded = false;
    QVector<EditorTab *> lockingTabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()) {
            lockingTabs.append(tab);
            tab->beginSecureDocumentLocking();
        }
    }
    purgeSensitiveSearchState();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()
            && tab->isModified() && !saveSecureTab(tab, false, false)) {
            // Confirm that this exact in-memory revision is recoverable. An
            // older recovery file must never authorize discarding newer text.
            QString recoveryError;
            if (!checkpointSecureTab(tab, &recoveryError)) {
                if (sessionExpired) {
                    secureChangesDiscarded = true;
                    continue;
                }
                for (EditorTab *lockingTab : lockingTabs)
                    lockingTab->cancelSecureDocumentLocking();
                statusBar()->showMessage(
                    QStringLiteral("保存及本机加密恢复均失败，已取消锁定。"),
                    8000);
                return;
            }
        }
    }
    if (m_vaultDock && !m_vaultDock->prepareToLock()) {
        if (sessionExpired) {
            // Expired credentials must not leave account fields visible. The
            // silent save has already failed, so conceal and clear the editor.
            m_vaultDock->discardAndLockForExternalReload();
            accountChangesDiscarded = true;
        } else {
            for (EditorTab *lockingTab : lockingTabs)
                lockingTab->cancelSecureDocumentLocking();
            statusBar()->showMessage(QStringLiteral("账户修改尚未处理，已取消锁定。"), 5000);
            return;
        }
    }
    // Lock is also a recovery boundary for ordinary documents. The operation is
    // explicit, so establish the latest encrypted S1/S2 recovery point first.
    saveSessionNow(true);
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(true);
    if (m_vaultDock)
        m_vaultDock->lockVault();
    maskSecureDocuments();
    if (secureChangesDiscarded || accountChangesDiscarded) {
        QString warning;
        if (secureChangesDiscarded && accountChangesDiscarded) {
            warning = QStringLiteral(
                "会话到期时安全文档与账户均无法保存；为清除明文，未保存内容已丢弃。");
        } else if (secureChangesDiscarded) {
            warning = QStringLiteral(
                "会话到期时安全文档保存及加密恢复均失败；为清除明文，未保存内容已丢弃。");
        } else {
            warning = QStringLiteral(
                "会话到期时账户保存失败；为清除明文，未保存字段已丢弃。");
        }
        statusBar()->showMessage(
            warning, 10000);
    }
}

void MainWindow::purgeSensitiveSearchState()
{
    clearSensitiveDocumentClipboard();
    m_lastFindPattern.fill(QChar(0));
    m_lastFindPattern.clear();
    m_lastFindRegex = false;
    m_lastFindCase = false;
    m_lastFindWord = false;
    if (m_findDialog)
        m_findDialog->clearSensitiveState();
    if (m_searchDock)
        m_searchDock->clearResults();
}

void MainWindow::rememberSensitiveDocumentClipboard(EditorTab *sourceTab)
{
    scrubByteArray(m_sensitiveDocumentClipboardFingerprint);
    if (!sourceTab || !sourceTab->isSecureDocument()
        || sourceTab->isSecureDocumentMasked()) {
        return;
    }

    QString clipboardText = QGuiApplication::clipboard()->text();
    QByteArray clipboardUtf8 = clipboardText.toUtf8();
    if (!clipboardUtf8.isEmpty()) {
        m_sensitiveDocumentClipboardFingerprint = QCryptographicHash::hash(
            clipboardUtf8, QCryptographicHash::Sha256);
    }
    scrubByteArray(clipboardUtf8);
    if (!clipboardText.isEmpty())
        clipboardText.fill(QChar(0));
}

void MainWindow::clearSensitiveDocumentClipboard()
{
    if (m_sensitiveDocumentClipboardFingerprint.isEmpty())
        return;

    QString clipboardText = QGuiApplication::clipboard()->text();
    QByteArray clipboardUtf8 = clipboardText.toUtf8();
    QByteArray currentFingerprint = QCryptographicHash::hash(
        clipboardUtf8, QCryptographicHash::Sha256);
    if (currentFingerprint == m_sensitiveDocumentClipboardFingerprint)
        QGuiApplication::clipboard()->clear();

    scrubByteArray(clipboardUtf8);
    if (!clipboardText.isEmpty())
        clipboardText.fill(QChar(0));
    scrubByteArray(currentFingerprint);
    scrubByteArray(m_sensitiveDocumentClipboardFingerprint);
}

void MainWindow::protectForSystemLock()
{
    if (m_systemProtectionActive)
        return;
    m_secureAutosaveTimer.stop();
    m_systemLockRecoveryFailed = false;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (tab && tab->isSecureDocument() && !tab->isSecureDocumentMasked()
            && tab->isModified() && !saveSecureTab(tab, false, false)) {
            QString recoveryError;
            if (!checkpointSecureTab(tab, &recoveryError))
                m_systemLockRecoveryFailed = true;
        }
    }
    m_restoreAccountsAfterSystemUnlock = m_workspaceStack && m_accountsPage
                                         && m_workspaceStack->currentWidget() == m_accountsPage;
    if (m_vaultDock && !m_vaultDock->lockVaultSilently()) {
        // A system lock is a confidentiality boundary. If the account editor
        // cannot save, it is still cleared instead of keeping secrets visible.
        m_vaultDock->discardAndLockForExternalReload();
        m_systemLockRecoveryFailed = true;
    }
    saveSessionNow(true);
    purgeSensitiveSearchState();
    closeAllSecureDocumentsOnSecurityLane();
    endSecuritySessionOnSecurityLane(false);
    maskSecureDocuments();
    m_systemProtectionActive = true;
}

void MainWindow::restoreAfterSystemUnlock()
{
    if (!m_systemProtectionActive)
        return;
    bool restored = false;
    if (m_securitySession && m_securitySession->restoreSealedSession()) {
        // This call is non-interactive because restoreSealedSession has already
        // rehydrated a valid DPAPI session; it also restores masked tabs.
        restored = ensureSecuritySessionInteractive();
    }
    m_systemProtectionActive = false;
    if (restored && m_restoreAccountsAfterSystemUnlock)
        showAccountsWorkspace();
    m_restoreAccountsAfterSystemUnlock = false;
    if (!restored)
        statusBar()->showMessage(QStringLiteral("安全会话需要重新登录。"), 5000);
    else if (m_systemLockRecoveryFailed)
        statusBar()->showMessage(
            QStringLiteral("锁屏前有安全文档未能保存或建立恢复点，请检查最近编辑。"), 10000);
    m_systemLockRecoveryFailed = false;
}

void MainWindow::maskSecureDocuments()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab || !tab->isSecureDocument())
            continue;
        tab->maskSecureDocument();
        m_tabs->setTabText(i, tab->tabTitle());
    }
    updateStatusBar();
}

EditorTab *MainWindow::adoptSecureSnapshot(SecureDocumentSnapshot &snapshot,
                                          EditorTab *reuseTab)
{
    if (!m_secureDocuments || !m_secureDocuments->available())
        return nullptr;
    showDocumentWorkspace();
    EditorTab *tab = reuseTab ? reuseTab : addNewTab();
    if (reuseTab && reuseTab->isSecureDocument()
        && reuseTab->secureDocumentId() != snapshot.documentId
        && m_secureDocuments) {
        closeSecureDocumentOnSecurityLane(reuseTab->secureDocumentId());
    }
    if (snapshot.payloadKind == SecureDocumentPayloadKind::Binary) {
        QString error;
        if (!tab->initializeSecureBinaryDocument(
                snapshot.documentId, snapshot.title, std::move(snapshot.binary),
                snapshot.filePath, &error)) {
            closeSecureDocumentOnSecurityLane(snapshot.documentId);
            if (!reuseTab) {
                const int failedIndex = m_tabs->indexOf(tab);
                if (failedIndex >= 0)
                    m_tabs->removeTab(failedIndex);
                delete tab;
            }
            QMessageBox::critical(this, QStringLiteral("无法打开安全文件"), error);
            return nullptr;
        }
    } else {
        tab->initializeSecureDocument(snapshot.documentId, snapshot.title,
                                      snapshot.text, snapshot.filePath);
        if (!snapshot.text.isEmpty())
            snapshot.text.fill(QChar(0));
        snapshot.text.clear();
    }
    tab->setAutosavePath(snapshot.backingPath);
    tab->setModified(snapshot.recoveredUnsaved);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0) {
        m_tabs->setCurrentIndex(index);
        m_tabs->setTabText(index, tab->tabTitle());
    }
    tab->setFocusEditor();
    if (snapshot.recoveredUnsaved) {
        statusBar()->showMessage(
            QStringLiteral("已恢复上次未写入 KDBX 的本机加密内容，请确认后保存。"),
            10000);
        m_secureAutosaveTimer.start(5000);
    }
    return tab;
}


bool MainWindow::openSecurePath(const QString &path, EditorTab *reuseTab)
{
    ensureSecurityServicesCreated();
    if (!m_secureDocuments || !m_secureDocuments->available())
        return false;
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!openTab || !openTab->isSecureDocument())
            continue;

        const QString backing = openTab->filePath().isEmpty()
                                    ? openTab->autosavePath() : openTab->filePath();
        if (QFileInfo(backing).absoluteFilePath().compare(absolutePath,
                                                          Qt::CaseInsensitive) == 0) {
            showDocumentWorkspace();
            m_tabs->setCurrentIndex(i);
            openTab->setFocusEditor();
            return true;
        }
    }
    if (!m_securitySession->hasActiveSession() && !ensureSecuritySessionInteractive())
        return false;

    SecureDocumentSnapshot snapshot;
    VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在打开安全文档"),
        [this, path, &snapshot] { return m_secureDocuments->open(path, &snapshot); });
    if (!error.isError()) {
        if (!adoptSecureSnapshot(snapshot, reuseTab))
            return false;
        if (!snapshot.draft)
            updateRecentFiles(path);
        return true;
    }
    if (error.code != VaultErrorCode::InvalidCredentials) {
        QMessageBox box(QMessageBox::Critical, QStringLiteral("无法打开安全文件"),
                        error.userMessage, QMessageBox::Ok, this);
        box.setDetailedText(error.diagnosticCode);
        box.exec();
        return false;
    }

    FileCredentialDialog dialog(
        QStringLiteral("打开受其他密码保护的文件"),
        QStringLiteral("当前会话不能解锁此文件。选择该文件的主密码或恢复密钥；成功后本次会话内不再询问。"),
        true, this);
    while (dialog.exec() == QDialog::Accepted) {
        if (dialog.kind() == FileCredentialDialog::Kind::RecoveryKey) {
            QString recoveryText = dialog.takeRecoveryKey();
            if (recoveryText.isEmpty()) {
                dialog.showError(QStringLiteral("请输入恢复密钥。"));
                continue;
            }
            error = runSerializedSecurityOperation(
                this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                QStringLiteral("正在恢复安全文档"),
                [this, path, &recoveryText, &snapshot] {
                    return m_secureDocuments->openWithRecovery(path, recoveryText,
                                                               &snapshot);
                });
            recoveryText.fill(QChar(0));
            recoveryText.clear();
        } else {
            SecureBuffer buffer = dialog.takePassphrase();
            if (buffer.empty()) {
                dialog.showError(QStringLiteral("请输入文件密码。"));
                continue;
            }
            error = runSerializedSecurityOperation(
                this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
                QStringLiteral("正在解锁安全文档"),
                [this, path, &buffer, &snapshot] {
                    return m_secureDocuments->openWithPassphrase(path, buffer, &snapshot);
                });
            buffer.clear();
        }
        if (!error.isError()) {
            if (!adoptSecureSnapshot(snapshot, reuseTab))
                return false;
            if (!snapshot.draft)
                updateRecentFiles(path);
            return true;
        }
        dialog.showError(error.userMessage);
    }
    return false;
}

bool MainWindow::openPathUnified(const QString &path, EditorTab *reuseTab,
                                 std::optional<TextEncoding> forcedEncoding)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    const auto persistenceAdapter =
        m_composition->persistenceAdapters().resolve(path);
    if (persistenceAdapter
        && persistenceAdapter->persistence == DocumentPersistence::SecureFile) {
        ensureSecurityServicesCreated();
        if (!m_secureDocuments || !m_secureDocuments->available()) {
            QMessageBox::critical(this, QStringLiteral("安全文件不可用"),
                                  QStringLiteral("当前构建未包含 KDBX 安全后端。"));
            return false;
        }
        QString documentType;
        QUuid documentId;
        SecurityIdentityParameters fileParameters;
        const VaultError probe = m_secureDocuments->inspectFile(
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
        DiskFileVersion candidate;
        const DiskFileState candidateState = captureDiskFileVersion(path, &candidate);
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(i));
            if (!openTab || openTab->isSecureDocument())
                continue;
            const bool samePath = sameDiskFilePath(openTab->filePath(), path);
            const bool sameIdentity = candidateState == DiskFileState::Unchanged
                                      && openTab->representsDiskFile(candidate);
            if (!samePath && !sameIdentity)
                continue;
            showDocumentWorkspace();
            m_tabs->setCurrentIndex(i);
            openTab->setFocusEditor();
            updateRecentFiles(path);
            statusBar()->showMessage(QStringLiteral("文件已在当前窗口中打开"), 2000);
            return true;
        }
    }

    showDocumentWorkspace();
    EditorTab *tab = reuseTab ? reuseTab : addNewTab();
    if (reuseTab && reuseTab->isSecureDocument()) {
        if (m_secureDocuments)
            closeSecureDocumentOnSecurityLane(reuseTab->secureDocumentId());
        // External Open may reuse the single empty default secure draft. The
        // reused tab must stop being a secure document before ordinary routing.
        reuseTab->convertToPlainDocument(QString());
    }
    const bool opened = openDocumentByPolicy(
        m_composition->documentProviders(), tab, path, forcedEncoding);
    if (!opened) {
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
    return true;
}

void MainWindow::openFile()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("打开文件"), QString(),
        QStringLiteral("所有文件 (*);;"
                       "C/C++ 文件 (*.cpp *.cc *.cxx *.c *.h *.hpp *.hh *.hxx);;"
                       "Python 文件 (*.py *.pyw);;"
                       "JSON 文件 (*.json);;"
                       "HTML 文件 (*.html *.htm);;"
                       "XML 文件 (*.xml);;"
                       "SQL 文件 (*.sql);;"
                       "YAML 文件 (*.yaml *.yml);;"
                       "Markdown 文件 (*.md *.markdown);;"
                       "文本文件 (*.txt)"));
    for (const QString &file : files) {
        if (file.isEmpty())
            continue;
        if (!openPathUnified(file)) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                                 QStringLiteral("无法打开文件: %1").arg(file));
        }
    }
}

bool MainWindow::saveFile()
{
    if (m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage) {
        const bool saved = m_vaultDock && m_vaultDock->saveActiveEntry();
        if (saved)
            statusBar()->showMessage(QStringLiteral("账户已保存"), 2500);
        return saved;
    }
    if (!isDocumentWorkspaceActive())
        return false;
    auto *tab = currentTab();
    if (!tab)
        return false;
    if (tab->isSecureDocument())
        return saveSecureTab(tab, true);
    if (tab->isUntitled())
        return saveAsFile();
    // Binary documents are saved from their authoritative byte/patch model.
    // U+FFFD in a Hex character pane or Binary Text projection is display
    // state and does not imply that Save will re-encode untouched bytes.
    // Keep this destructive re-encoding confirmation only for ordinary text.
    if (tab->encodingHasDecodeErrors() && !tab->isBinaryDocument()) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("编码需要确认"),
                        QStringLiteral("源文件包含不符合当前 %1 解释的字节。"
                                       "继续保存会用当前显示文本覆盖这些原始字节。")
                            .arg(encodingName(tab->displayEncoding())),
                        QMessageBox::NoButton, this);
        auto *overwrite = box.addButton(
            QStringLiteral("仍以 %1 覆盖").arg(encodingName(tab->displayEncoding())),
                                        QMessageBox::AcceptRole);
        auto *cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != overwrite)
            return false;
        // The explicit decision turns the fallback into a user-selected
        // encoding so subsequent saves do not ask the same question again.
        tab->setEncoding(tab->displayEncoding());
    }
    switch (resolveExternalSaveConflict(tab)) {
    case ExternalSaveDisposition::HandledSuccess:
        return true;
    case ExternalSaveDisposition::Cancelled:
        return false;
    case ExternalSaveDisposition::Proceed:
        break;
    }
    QString err;
    if (!tab->saveFile(&err)) {
        QString detail = err.isEmpty() ? tab->filePath() : QStringLiteral("%1\n\n详细信息：%2").arg(tab->filePath(), err);
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法保存文件：\n%1").arg(detail));
        return false;
    }
    return true;
}

MainWindow::ExternalSaveDisposition MainWindow::resolveExternalSaveConflict(EditorTab *tab)
{
    if (!tab || tab->isSecureDocument())
        return ExternalSaveDisposition::Proceed;
    const EditorTab::ExternalFileState state = tab->externalFileState();
    if (state == EditorTab::ExternalFileState::NotTracked
        || state == EditorTab::ExternalFileState::Unchanged) {
        return ExternalSaveDisposition::Proceed;
    }

    QMessageBox box(QMessageBox::Warning, QStringLiteral("文件已在磁盘上更改"),
                    QStringLiteral("为避免覆盖其他程序的修改，本次保存已暂停。"),
                    QMessageBox::NoButton, this);
    if (state == EditorTab::ExternalFileState::Changed) {
        box.setInformativeText(
            QStringLiteral("“%1”在打开后发生了变化。\n"
                           "可以重新加载磁盘版本，或将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    } else if (state == EditorTab::ExternalFileState::Missing) {
        box.setInformativeText(
            QStringLiteral("“%1”已被移动或删除。\n"
                           "可以将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    } else {
        box.setInformativeText(
            QStringLiteral("当前无法安全读取“%1”以确认它是否变化。\n"
                           "可以将当前编辑内容另存为新文件。")
                .arg(tab->fileName()));
    }

    QPushButton *reloadButton = nullptr;
    if (state == EditorTab::ExternalFileState::Changed) {
        reloadButton = box.addButton(QStringLiteral("重新加载磁盘版本"),
                                     QMessageBox::AcceptRole);
    }
    auto *saveAsButton = box.addButton(QStringLiteral("另存为…"), QMessageBox::ActionRole);
    auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();

    if (reloadButton && box.clickedButton() == reloadButton)
        return reloadTabFromDisk(tab) ? ExternalSaveDisposition::HandledSuccess
                                      : ExternalSaveDisposition::Cancelled;
    if (box.clickedButton() == saveAsButton)
        return saveAsFile() ? ExternalSaveDisposition::HandledSuccess
                            : ExternalSaveDisposition::Cancelled;
    return ExternalSaveDisposition::Cancelled;
}

MainWindow::ExternalSaveDisposition MainWindow::resolveSecureExternalSaveConflict(
    EditorTab *tab)
{
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments
        || !m_secureDocuments->available())
        return ExternalSaveDisposition::Cancelled;
    const DiskFileState state = m_secureDocuments->externalFileState(
        tab->secureDocumentId());

    QMessageBox box(QMessageBox::Warning,
                    QStringLiteral("安全文档已在磁盘上更改"),
                    QStringLiteral("为避免覆盖其他程序写入的密文，本次保存已暂停。"),
                    QMessageBox::NoButton, this);
    if (state == DiskFileState::Changed) {
        box.setInformativeText(
            QStringLiteral("“%1”在打开后发生了变化。\n"
                           "可以重新解密并加载磁盘版本，或将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    } else if (state == DiskFileState::Missing) {
        box.setInformativeText(
            QStringLiteral("“%1”已被移动或删除。\n"
                           "可以将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    } else {
        box.setInformativeText(
            QStringLiteral("当前无法安全验证“%1”的磁盘版本。\n"
                           "可以将当前内容另存为新的安全文档。")
                .arg(tab->fileName()));
    }

    QPushButton *reloadButton = nullptr;
    if (state == DiskFileState::Changed) {
        reloadButton = box.addButton(QStringLiteral("重新加载磁盘版本"),
                                     QMessageBox::AcceptRole);
    }
    auto *saveAsButton = box.addButton(QStringLiteral("另存为…"), QMessageBox::ActionRole);
    auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();

    if (reloadButton && box.clickedButton() == reloadButton)
        return reloadSecureTabFromDisk(tab) ? ExternalSaveDisposition::HandledSuccess
                                            : ExternalSaveDisposition::Cancelled;
    if (box.clickedButton() == saveAsButton)
        return saveAsFile() ? ExternalSaveDisposition::HandledSuccess
                            : ExternalSaveDisposition::Cancelled;
    return ExternalSaveDisposition::Cancelled;
}

bool MainWindow::reloadSecureTabFromDisk(EditorTab *tab)
{
    if (!tab || !tab->isSecureDocument() || !m_secureDocuments
        || !m_secureDocuments->available())
        return false;
    SecureDocumentSnapshot snapshot;
    const VaultError error = runSerializedSecurityOperation(
        this, AsyncTaskType::SecureOpen, AsyncTaskPriority::Highest,
        QStringLiteral("正在重新加载安全文档"),
        [this, tab, &snapshot] {
            return m_secureDocuments->reload(tab->secureDocumentId(), &snapshot);
        });
    if (error.isError()) {
        QMessageBox box(QMessageBox::Critical, QStringLiteral("无法重新加载安全文档"),
                        error.userMessage, QMessageBox::Ok, this);
        box.setDetailedText(error.diagnosticCode);
        box.exec();
        return false;
    }
    if (!adoptSecureSnapshot(snapshot, tab))
        return false;
    if (m_searchDock)
        m_searchDock->clearResults();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已重新解密并加载磁盘版本"), 2500);
    return true;
}

bool MainWindow::reloadTabFromDisk(EditorTab *tab)
{
    if (!tab || tab->isSecureDocument() || tab->filePath().isEmpty())
        return false;
    const QString path = tab->filePath();
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法重新加载"),
                             QStringLiteral("无法读取文件：\n%1\n\n%2")
                                 .arg(path, probe.errorString()));
        return false;
    }
    probe.close();

    // Discarding edits is already explicit at the call site. Tear down mmap and
    // route through the complete Open path (KDBX header, binary and size tier).
    tab->shutdownL2BeforeClose();
    if (!openPathUnified(path, tab)) {
        QMessageBox::warning(this, QStringLiteral("无法重新加载"),
                             QStringLiteral("文件无法按当前格式重新打开：\n%1")
                                 .arg(path));
        return false;
    }
    if (m_searchDock)
        m_searchDock->clearResults();
    updateRecentFiles(path);
    const int index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabText(index, tab->tabTitle());
    syncGlobalMenuCheckStates();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已从磁盘重新加载"), 2500);
    return true;
}

void MainWindow::reloadFile()
{
    EditorTab *tab = currentTab();
    if (!tab || (tab->isSecureDocument() ? tab->filePath().isEmpty()
                                         : tab->isUntitled()))
        return;
    if (tab->isModified()) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("重新加载文件"),
                        QStringLiteral("重新加载将丢弃“%1”的未保存修改。")
                            .arg(tab->fileName()),
                        QMessageBox::NoButton, this);
        auto *reloadButton = box.addButton(QStringLiteral("重新加载"),
                                           QMessageBox::DestructiveRole);
        auto *cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(cancelButton);
        box.exec();
        if (box.clickedButton() != reloadButton)
            return;
    }
    if (tab->isSecureDocument())
        reloadSecureTabFromDisk(tab);
    else
        reloadTabFromDisk(tab);
}

bool MainWindow::saveAsFile()
{
    auto *tab = currentTab();
    if (!tab)
        return false;
    const QString secureFilter = QStringLiteral("Notepad Safe 安全文档 (*.nsafe)");
    const QString plainFilter = QStringLiteral("未加密文件 (*)");
    QString selectedFilter = tab->isSecureDocument() ? secureFilter
                                                      : plainFilter;
    QString suggestion;
    if (!tab->filePath().isEmpty())
        suggestion = tab->filePath();
    else
        suggestion = secureTitleFrom(tab) + (tab->isSecureDocument()
                                                  ? QStringLiteral(".nsafe")
                                                  : QStringLiteral(".txt"));
    QDialog saveAsDialog(this);
    saveAsDialog.setObjectName(QStringLiteral("saveAsDialog"));
    saveAsDialog.setWindowTitle(QStringLiteral("另存为"));
    saveAsDialog.resize(940, 650);
    auto *saveAsLayout = new QVBoxLayout(&saveAsDialog);
    saveAsLayout->setContentsMargins(10, 10, 10, 10);
    saveAsLayout->setSpacing(8);

    auto *fileDialog = new QFileDialog(
        &saveAsDialog, QString(), suggestion,
        secureFilter + QStringLiteral(";;") + plainFilter);
    fileDialog->setObjectName(QStringLiteral("saveAsFileDialog"));
    fileDialog->setWindowFlags(Qt::Widget);
    fileDialog->setAcceptMode(QFileDialog::AcceptSave);
    fileDialog->setFileMode(QFileDialog::AnyFile);
    // The product requires one dialog with an explicit, always-visible
    // encryption consequence. Native dialogs do not allow that content.
    fileDialog->setOption(QFileDialog::DontUseNativeDialog, true);
    fileDialog->setLabelText(QFileDialog::LookIn, QStringLiteral("位置："));
    fileDialog->setLabelText(QFileDialog::FileName, QStringLiteral("文件名："));
    fileDialog->setLabelText(QFileDialog::FileType, QStringLiteral("保存类型："));
    fileDialog->setLabelText(QFileDialog::Accept, QStringLiteral("保存"));
    fileDialog->setLabelText(QFileDialog::Reject, QStringLiteral("取消"));
    fileDialog->selectNameFilter(selectedFilter);
    saveAsLayout->addWidget(fileDialog, 1);

    auto *securityNotice = new QLabel(&saveAsDialog);
    securityNotice->setObjectName(QStringLiteral("saveAsSecurityNotice"));
    securityNotice->setWordWrap(true);
    securityNotice->setContentsMargins(8, 6, 8, 6);
    securityNotice->setMinimumHeight(42);
    saveAsLayout->addWidget(securityNotice);
    const QString plainSuffix = tab->isBinaryDocument()
                                    ? QStringLiteral("bin") : QStringLiteral("txt");
    const auto updateSaveAsNotice = [fileDialog, securityNotice, secureFilter,
                                     plainSuffix](const QString &filter) {
        if (filter == secureFilter) {
            fileDialog->setDefaultSuffix(QStringLiteral("nsafe"));
            securityNotice->setText(QStringLiteral(
                "将保存为离线加密的 Notepad Safe 文档。普通保存会继续保持此格式。"));
            securityNotice->setStyleSheet(QStringLiteral(
                "QLabel { color: #246b45; background: #eaf7ef; border: 1px solid #b9dfc8; "
                "border-radius: 4px; }"));
            const QStringList selected = fileDialog->selectedFiles();
            if (!selected.isEmpty()
                && !selected.constFirst().endsWith(QStringLiteral(".nsafe"),
                                                    Qt::CaseInsensitive)) {
                fileDialog->selectFile(selected.constFirst()
                                       + QStringLiteral(".nsafe"));
            }
        } else {
            fileDialog->setDefaultSuffix(plainSuffix);
            securityNotice->setText(QStringLiteral(
                "将创建未加密文件，其他程序可以直接读取其内容。普通保存会继续保持此格式。"));
            securityNotice->setStyleSheet(QStringLiteral(
                "QLabel { color: #7a4b00; background: #fff5dd; border: 1px solid #e8c77a; "
                "border-radius: 4px; }"));

            // .nsafe is reserved for KDBX-backed encrypted documents. When a
            // user changes the explicit type selector, update the inherited
            // suggestion immediately instead of silently writing plaintext
            // behind an encrypted-looking extension.
            const QStringList selected = fileDialog->selectedFiles();
            if (!selected.isEmpty()
                && selected.constFirst().endsWith(QStringLiteral(".nsafe"),
                                                   Qt::CaseInsensitive)) {
                QString plainName = selected.constFirst();
                plainName.chop(QStringLiteral(".nsafe").size());
                plainName += QLatin1Char('.') + plainSuffix;
                fileDialog->selectFile(plainName);
            }
        }
    };
    connect(fileDialog, &QFileDialog::filterSelected,
            &saveAsDialog, updateSaveAsNotice);
    connect(fileDialog, &QFileDialog::accepted,
            &saveAsDialog, &QDialog::accept);
    connect(fileDialog, &QFileDialog::rejected,
            &saveAsDialog, &QDialog::reject);
    updateSaveAsNotice(selectedFilter);

    if (saveAsDialog.exec() != QDialog::Accepted
        || fileDialog->selectedFiles().isEmpty()) {
        return false;
    }
    QString fileName = fileDialog->selectedFiles().constFirst();
    selectedFilter = fileDialog->selectedNameFilter();

    // The explicit type selector is authoritative. An extension is a display
    // hint only and must never silently change the security boundary.
    const bool saveSecure = selectedFilter == secureFilter;
    if (!saveSecure
        && fileName.endsWith(QStringLiteral(".nsafe"), Qt::CaseInsensitive)) {
        fileName.chop(QStringLiteral(".nsafe").size());
        fileName += QLatin1Char('.') + plainSuffix;
    }
    if (saveSecure) {
        const bool binaryPayload = tab->isBinaryDocument();
        if (tab->isLargeDocument() && !binaryPayload) {
            QMessageBox::information(
                this, QStringLiteral("不能转换为安全文档"),
                QStringLiteral("安全文档只接受小于 32 MiB 的内容。"
                               "当前文本处于大文件模式，未写入任何目标文件。"));
            return false;
        }
        if (!ensureSecuritySessionInteractive() || !m_secureDocuments)
            return false;
        if (!fileName.endsWith(QStringLiteral(".nsafe"), Qt::CaseInsensitive))
            fileName += QStringLiteral(".nsafe");
        const QString title = secureTitleFrom(tab);
        VaultError error;
        if (binaryPayload) {
            QByteArray body;
            QString exportError;
            if (!tab->exportBinaryPayloadBytes(&body, &exportError)) {
                QMessageBox::critical(this, QStringLiteral("另存为失败"), exportError);
                return false;
            }
            if (tab->isSecureDocument()) {
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在另存安全文档"),
                    [this, tab, &body, title, fileName] {
                        return m_secureDocuments->saveBinary(
                            tab->secureDocumentId(), body, title, fileName);
                    });
                if (!error.isError()) {
                    tab->setFilePath(QFileInfo(fileName).absoluteFilePath());
                    tab->setAutosavePath(
                        m_secureDocuments->backingPath(tab->secureDocumentId()));
                    tab->setSecureDocumentTitle(title);
                    tab->setModified(false);
                }
            } else {
                SecureDocumentSnapshot snapshot;
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在创建安全文档"),
                    [this, &body, title, fileName, &snapshot] {
                        return m_secureDocuments->saveAsSecureBinary(
                            body, title, fileName, &snapshot);
                    });
                if (!error.isError() && !adoptSecureSnapshot(snapshot, tab)) {
                    scrubByteArray(body);
                    return false;
                }
            }
            scrubByteArray(body);
        } else {
            QString body = tab->editor()->text();
            if (tab->isSecureDocument()) {
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在另存安全文档"),
                    [this, tab, &body, title, fileName] {
                        return m_secureDocuments->save(
                            tab->secureDocumentId(), body, title, fileName);
                    });
                if (!error.isError()) {
                    tab->setFilePath(QFileInfo(fileName).absoluteFilePath());
                    tab->setAutosavePath(
                        m_secureDocuments->backingPath(tab->secureDocumentId()));
                    tab->setSecureDocumentTitle(title);
                    tab->setModified(false);
                }
            } else {
                SecureDocumentSnapshot snapshot;
                error = runSerializedSecurityOperation(
                    this, AsyncTaskType::SecureSave, AsyncTaskPriority::Highest,
                    QStringLiteral("正在创建安全文档"),
                    [this, &body, title, fileName, &snapshot] {
                        return m_secureDocuments->saveAsSecure(
                            body, title, fileName, &snapshot);
                    });
                if (!error.isError() && !adoptSecureSnapshot(snapshot, tab)) {
                    body.fill(QChar(0));
                    return false;
                }
            }
            if (!body.isEmpty())
                body.fill(QChar(0));
            body.clear();
        }
        if (error.isError()) {
            QMessageBox box(QMessageBox::Critical, QStringLiteral("另存为失败"),
                            error.userMessage, QMessageBox::Ok, this);
            box.setDetailedText(error.diagnosticCode);
            box.exec();
            return false;
        }
    } else {
        const QUuid previousSecureId = tab->isSecureDocument()
                                           ? tab->secureDocumentId() : QUuid();
        QString err;
        if (tab->isSecureDocument()) {
            if (tab->isSecureBinaryDocument()) {
                QByteArray raw;
                if (!tab->exportBinaryPayloadBytes(&raw, &err)) {
                    // export supplied the actionable validation message.
                } else {
                    QSaveFile output(fileName);
                    output.setDirectWriteFallback(false);
                    const bool written = output.open(QIODevice::WriteOnly)
                                         && output.write(raw) == raw.size()
                                         && output.commit();
                    scrubByteArray(raw);
                    if (!written)
                        err = output.errorString().isEmpty()
                                  ? QStringLiteral("无法原子写入文件。")
                                  : output.errorString();
                }
                if (err.isEmpty() && !openPathUnified(fileName, tab))
                    err = QStringLiteral("文件已写入，但无法重新打开目标文件。");
                // Re-enter through the unified opener so the newly created raw
                // file receives its mmap/Hex backing store and disk baseline.
            } else if (!tab->exportToFile(fileName)) {
                err = QStringLiteral("无法写入文件。");
            } else {
                if (m_secureDocuments)
                    closeSecureDocumentOnSecurityLane(previousSecureId);
                tab->convertToPlainDocument(QFileInfo(fileName).absoluteFilePath());
            }
        } else if (!tab->saveAsFile(fileName, &err)) {
            // err is populated by the text writer.
        }
        if (!err.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("错误"),
                                 QStringLiteral("无法保存文件：\n%1\n\n%2")
                                     .arg(fileName, err));
            return false;
        }
    }
    updateRecentFiles(fileName);
    int idx = m_tabs->indexOf(tab);
    if (idx >= 0)
        m_tabs->setTabText(idx, tab->tabTitle());
    syncCommandStates(tab);
    syncGlobalMenuCheckStates();
    updateStatusBar();
    statusBar()->showMessage(QStringLiteral("已另存为"), 2500);
    return true;
}

void MainWindow::closeCurrentTab()
{
    if (!m_tabs)
        return;
    const int index = m_tabs->currentIndex();
    if (index >= 0)
        onTabCloseRequested(index);
}

void MainWindow::closeAllTabs()
{
    if (!m_tabs || m_tabs->count() == 0)
        return;

    // Preflight every document before removing any tab. If the user cancels a
    // later prompt, the workspace remains intact instead of being half closed.
    QVector<int> order;
    order.reserve(m_tabs->count());
    const int current = m_tabs->currentIndex();
    if (current >= 0)
        order.push_back(current);
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (index != current)
            order.push_back(index);
    }
    for (const int index : order) {
        if (!maybeSaveTab(index, false))
            return;
    }

    for (int index = m_tabs->count() - 1; index >= 0; --index)
        closeTabUnchecked(index);
}

void MainWindow::openRecentFile()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act)
        return;
    QString path = act->data().toString();
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        for (int i = m_recentFiles.size() - 1; i >= 0; --i) {
            if (sameDiskFilePath(m_recentFiles.at(i), path))
                m_recentFiles.removeAt(i);
        }
        rebuildRecentFilesMenu();
        saveSettings();
        statusBar()->showMessage(
            QStringLiteral("文件已不存在，已从最近打开中移除：%1").arg(path), 5000);
        return;
    }
    if (!openPathUnified(path)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件: %1").arg(path));
    }
}

void MainWindow::clearRecentFiles()
{
    m_recentFiles.clear();
    rebuildRecentFilesMenu();
    saveSettings();
}
