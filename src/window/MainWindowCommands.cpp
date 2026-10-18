#include "MainWindow.h"
#include "CommandPalette.h"
#include "DocumentTabBar.h"
#include "NoticeBar.h"
#include "StatusSegments.h"
#include "ThemeStyle.h"
#include "TitleBar.h"
#include "RecoveryIssuePage.h"
#include "EditorTab.h"
#include "FileContentProbe.h"
#include "FindReplaceDialog.h"
#include "SearchResultsDock.h"
#include "SearchResultsController.h"
#include "SessionManager.h"
#include "WorkspaceRecoveryCoordinator.h"
#include "TextEncoding.h"
#include "FileTierPolicy.h"
#include "VirtualFileView.h"
#include "StatusBarTaskWidget.h"
#include "AsyncSerialQueue.h"
#include "AsyncUiOperation.h"
#include "VaultDock.h"
#include "SecureDocumentContract.h"
#include "SecuritySessionPort.h"
#include "AppComposition.h"
#include "FileCredentialDialog.h"
#include "SensitiveText.h"
#include "CommandRegistry.h"
#include "DocumentOpenWorkflow.h"
#include "DocumentStatusPresentation.h"
#include "EditorSurfaceStyle.h"
#include "DocumentTailActions.h"
#include "MessageBoxPresentation.h"

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
#include <QScopeGuard>
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

namespace {

bool systemHighContrastEnabled()
{
    // Deterministic GUI coverage without changing the developer machine's OS
    // accessibility settings. This is intentionally process-local.
    if (qEnvironmentVariableIntValue("NOTEPADSAFE_FORCE_HIGH_CONTRAST") == 1)
        return true;
#ifdef Q_OS_WIN
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(state), &state, 0)
           && (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
#else
    return false;
#endif
}


} // namespace

// 前向：同步编码菜单勾选状态
static void syncEncodingMenuTo(QMenu *menu, TextEncoding enc)
{
    if (!menu) return;
    const auto acts = menu->actions();
    for (QAction *a : acts) {
        bool ok = false;
        int val = a->data().toInt(&ok);
        if (ok && static_cast<TextEncoding>(val) == enc) {
            a->setChecked(true);
        } else if (ok) {
            a->setChecked(false);
        }
    }
}


// Main-window command, menu-state and interaction implementation.
void MainWindow::createStatusBar()
{
    m_statusPos = new QLabel(QStringLiteral("行: 1, 列: 1"));
    m_statusPos->setObjectName(QStringLiteral("status.position"));
    m_statusPos->setAccessibleName(QStringLiteral("文档位置"));
    m_statusPos->setMinimumWidth(120);
    statusBar()->addPermanentWidget(m_statusPos);

    // Clickable segments open menus built from the existing command actions.
    m_statusSecurity = new StatusSegment(QStringLiteral("status.security"),
                                         QStringLiteral("文档安全状态"));
    m_statusSecurity->setPopupMode(QToolButton::DelayedPopup);
    m_statusSecurity->hide();
    connect(m_statusSecurity, &QToolButton::clicked,
            m_titleBar, &TitleBar::securityPillClicked);
    statusBar()->addPermanentWidget(m_statusSecurity);
    m_statusEncoding = new StatusSegment(QStringLiteral("status.encoding"),
                                         QStringLiteral("编码"));
    m_statusEncoding->setText(QStringLiteral("UTF-8"));
    statusBar()->addPermanentWidget(m_statusEncoding);
    m_statusEol = new StatusSegment(QStringLiteral("status.eol"), QStringLiteral("换行符"));
    statusBar()->addPermanentWidget(m_statusEol);
    m_statusSyntax = new StatusSegment(QStringLiteral("status.context"),
                                       QStringLiteral("文档类型或当前工作区"));
    m_statusSyntax->setText(QStringLiteral("纯文本"));
    statusBar()->addPermanentWidget(m_statusSyntax);
    m_statusZoom = new StatusSegment(QStringLiteral("status.zoom"), QStringLiteral("缩放"));
    statusBar()->addPermanentWidget(m_statusZoom);
    installStatusMenus(this, m_statusEncoding, m_statusEol, m_statusSyntax, m_statusZoom,
                       [this] { updateStatusBar(); });

    m_statusSize = new QLabel(QStringLiteral("0 B"));
    m_statusSize->setObjectName(QStringLiteral("status.size"));
    m_statusSize->setAccessibleName(QStringLiteral("文档大小"));
    m_statusSize->setMinimumWidth(80);
    statusBar()->addPermanentWidget(m_statusSize);

    m_statusCharacters = new QLabel(QStringLiteral("字符数: 0"));
    m_statusCharacters->setObjectName(QStringLiteral("status.characters"));
    m_statusCharacters->setAccessibleName(QStringLiteral("文档总字符数"));
    m_statusCharacters->setMinimumWidth(100);
    statusBar()->addPermanentWidget(m_statusCharacters);

    m_characterStatusTimer.setSingleShot(true);
    m_characterStatusTimer.setInterval(120);
    connect(&m_characterStatusTimer, &QTimer::timeout,
            this, &MainWindow::updateStatusBar);

    m_hexEditModeButton = new StatusSegment(QStringLiteral("status.hexEditMode"),
                                            QStringLiteral("十六进制编辑模式"));
    m_hexEditModeButton->setPopupMode(QToolButton::DelayedPopup);
    m_hexEditModeButton->setMinimumWidth(44);
    m_hexEditModeButton->hide();
    statusBar()->addPermanentWidget(m_hexEditModeButton);
    connect(m_hexEditModeButton, &QToolButton::clicked, this, [this]() {
        if (auto *tab = currentTab())
            tab->toggleHexOverwriteMode();
    });

    m_statusModified = new QLabel(QStringLiteral(""));
    m_statusModified->setObjectName(QStringLiteral("status.modified"));
    m_statusModified->setAccessibleName(QStringLiteral("保存状态"));
    m_statusModified->setMinimumWidth(40);
    statusBar()->addPermanentWidget(m_statusModified);

    // 活跃任务或失败存在时才显示一个状态 chip；线程占用只放在 tooltip/任务中心，
    // 不让实现细节永久挤占编辑状态栏。
    m_taskWidget = new StatusBarTaskWidget(this);
    statusBar()->addPermanentWidget(m_taskWidget);
    updateStatusBar();
}

// ============ 编辑操作 ============

void MainWindow::duplicateLine()
{
    if (auto *t = currentTab()) t->duplicateLine();
}

void MainWindow::deleteLine()
{
    if (auto *t = currentTab()) t->deleteLine();
}

void MainWindow::moveLineUp()
{
    if (auto *t = currentTab()) t->moveLineUp();
}

void MainWindow::moveLineDown()
{
    if (auto *t = currentTab()) t->moveLineDown();
}

void MainWindow::sortLinesAsc()
{
    if (auto *t = currentTab()) t->sortLinesAsc();
}

void MainWindow::sortLinesDesc()
{
    if (auto *t = currentTab()) t->sortLinesDesc();
}

// ============ 搜索 ============

void MainWindow::showFindDialog()
{
    ensureFindDialogCreated();
    if (m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage) {
        if (m_vaultDock)
            m_vaultDock->focusAccountSearch();
        return;
    }
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showFindMode();
}

void MainWindow::showReplaceDialog()
{
    ensureFindDialogCreated();
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab)
        return;
    m_findDialog->setActiveTab(tab);
    m_findDialog->showReplaceMode();
}

void MainWindow::findNext()
{
    ensureFindDialogCreated();
    if (!currentTab() || m_lastFindPattern.isEmpty())
        return;
    m_findDialog->findNextFromShortcut(true);
}

void MainWindow::findPrev()
{
    ensureFindDialogCreated();
    if (!currentTab() || m_lastFindPattern.isEmpty())
        return;
    m_findDialog->findNextFromShortcut(false);
}

// ============ 语法 ============

void MainWindow::setSyntaxFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act)
        return;
    int syntax = act->data().toInt();
    auto *tab = currentTab();
    if (!tab)
        return;
    tab->setSyntax(static_cast<Syntax>(syntax));
    syncCommandStates(tab);
    updateStatusBar();
}

// ============ 书签 ============

void MainWindow::toggleBookmark()
{
    if (auto *t = currentTab()) t->toggleBookmark();
}

void MainWindow::nextBookmark()
{
    if (auto *t = currentTab()) t->nextBookmark();
}

void MainWindow::prevBookmark()
{
    if (auto *t = currentTab()) t->prevBookmark();
}

void MainWindow::clearBookmarks()
{
    if (auto *t = currentTab()) t->clearBookmarks();
}

// ============ 视图 ============

void MainWindow::setAppearanceModeFromAction(QAction *action)
{
    if (!action)
        return;
    const int value = action->data().toInt();
    if (value < static_cast<int>(AppearanceMode::System)
        || value > static_cast<int>(AppearanceMode::Dark)) {
        return;
    }
    m_appearanceMode = static_cast<AppearanceMode>(value);
    applyAppearanceMode();
    syncGlobalMenuCheckStates();
    saveSettings();
}

void MainWindow::zoomIn()
{
    if (auto *t = currentTab()) t->editor()->zoomIn();
}

void MainWindow::zoomOut()
{
    if (auto *t = currentTab()) t->editor()->zoomOut();
}

void MainWindow::resetZoom()
{
    if (auto *t = currentTab()) t->editor()->zoomTo(0);
}

void MainWindow::toggleEdgeLine()
{
    m_edgeVisible = !m_edgeVisible;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setEdgeVisible(m_edgeVisible);
    }
    // 同步勾选（toggle 是用户直接点菜单，QAction 自己会切；这里兜底防手动调用）
    if (m_actEdgeVisible) m_actEdgeVisible->setChecked(m_edgeVisible);
    saveSettings();
}

void MainWindow::setEdgeColumnFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    int col = act->data().toInt();
    if (col <= 0) return;
    m_edgeColumn = col;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setEdgeColumn(m_edgeColumn);
    }
    // 同步 action group 勾选
    if (m_edgeColGroup) {
        for (QAction *a : m_edgeColGroup->actions()) {
            a->setChecked(a->data().toInt() == m_edgeColumn);
        }
    }
    saveSettings();
}

void MainWindow::toggleWordWrap()
{
    EditorTab *active = currentTab();
    if (active && active->isReadOnlyByPolicy()) {
        syncCommandStates(active);
        syncGlobalMenuCheckStates();
        return;
    }
    m_wordWrap = !m_wordWrap;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (t) t->setWordWrap(m_wordWrap);
    }
    if (m_actWordWrap) m_actWordWrap->setChecked(m_wordWrap);
    saveSettings();
}

// 统一把外观/换行/标尺同步到视图菜单。
// 这是解决"恢复设置后菜单勾选未同步显示"的核心函数
void MainWindow::syncGlobalMenuCheckStates()
{
    if (m_actAppearanceSystem)
        m_actAppearanceSystem->setChecked(m_appearanceMode == AppearanceMode::System);
    if (m_actAppearanceLight)
        m_actAppearanceLight->setChecked(m_appearanceMode == AppearanceMode::Light);
    if (m_actDarkMode)
        m_actDarkMode->setChecked(m_appearanceMode == AppearanceMode::Dark);
    if (m_actWordWrap) {
        EditorTab *tab = currentTab();
        m_actWordWrap->setChecked(tab && tab->isReadOnlyByPolicy()
                                      ? false : m_wordWrap);
    }
    if (m_actWhitespaceVisible)
        m_actWhitespaceVisible->setChecked(m_whitespaceVisible);
    if (m_actEolVisible)
        m_actEolVisible->setChecked(m_eolVisible);
    if (m_actUseTabs)
        m_actUseTabs->setChecked(m_useTabs);
    if (m_actBackspaceUnindents)
        m_actBackspaceUnindents->setChecked(m_backspaceUnindents);
    if (m_actWordCompletion)
        m_actWordCompletion->setChecked(m_wordCompletion);
    if (m_actEdgeVisible) m_actEdgeVisible->setChecked(m_edgeVisible);
    if (m_edgeColGroup) {
        for (QAction *a : m_edgeColGroup->actions()) {
            a->setChecked(a->data().toInt() == m_edgeColumn);
        }
    }
    if (m_tabWidthGroup) {
        for (QAction *action : m_tabWidthGroup->actions())
            action->setChecked(action->data().toInt() == m_tabWidth);
    }
}

// 给 w 及其（当它是 EditorTab 时）内部的 editor/editor->viewport() 关闭 acceptDrops
// 并安装事件过滤器，把 DragEnter/DragMove/Drop 都转发到 MainWindow 处理
void MainWindow::registerDropProxy(QWidget *w)
{
    if (!w) return;
    w->setAcceptDrops(false);
    w->installEventFilter(this);

    // 特殊：EditorTab 内部的 QsciScintilla 才是用户最常拖到的控件
    auto *tab = qobject_cast<EditorTab *>(w);
    if (tab && tab->editor()) {
        QsciScintilla *ed = tab->editor();
        ed->setAcceptDrops(false);
        ed->installEventFilter(this);
        if (ed->viewport()) {
            ed->viewport()->setAcceptDrops(false);
            ed->viewport()->installEventFilter(this);
        }
    }
}

// ============ 编码 ============

void MainWindow::setEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    tab->setEncoding(enc);
    tab->setModified(true);
}

void MainWindow::convertEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    if (tab->isSecureDocument() || tab->isUntitled() || tab->isBinaryDocument())
        return;
    if (tab->encodingHasDecodeErrors()) {
        QMessageBox::information(
            this, QStringLiteral("请先确认源编码"),
            QStringLiteral("源文件包含不符合当前 %1 解释的字节。"
                           "请先用“以 … 编码显示”选择正确源编码，再执行转换。")
                .arg(encodingName(tab->displayEncoding())));
        return;
    }
    if (tab->isL2Active()
        && tab->externalFileState() != EditorTab::ExternalFileState::Unchanged) {
        QMessageBox::warning(
            this, QStringLiteral("源文件已经变化"),
            QStringLiteral("磁盘文件在打开后已被修改、替换或删除。"
                           "请重新打开源文件后再执行编码转换。"));
        return;
    }

    const QFileInfo sourceInfo(tab->filePath());
    QString suffix = sourceInfo.completeSuffix();
    const QString stem = sourceInfo.completeBaseName().isEmpty()
        ? sourceInfo.fileName() : sourceInfo.completeBaseName();
    QString targetSuggestion = sourceInfo.dir().filePath(
        stem + QStringLiteral("-")
        + encodingName(enc).toLower().replace(QLatin1Char(' '), QLatin1Char('-'))
        + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix));
    QFileDialog conversionDialog(
        this, QStringLiteral("转换为 %1 并另存为").arg(encodingName(enc)),
        targetSuggestion, QStringLiteral("所有文件 (*)"));
    conversionDialog.setObjectName(QStringLiteral("encodingConversionSaveAsDialog"));
    conversionDialog.setAcceptMode(QFileDialog::AcceptSave);
    conversionDialog.setFileMode(QFileDialog::AnyFile);
    conversionDialog.setOption(QFileDialog::DontUseNativeDialog, true);
    conversionDialog.selectFile(targetSuggestion);
    if (conversionDialog.exec() != QDialog::Accepted
        || conversionDialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString target = conversionDialog.selectedFiles().constFirst();
    if (sameDiskFilePath(tab->filePath(), target)) {
        QMessageBox::warning(this, QStringLiteral("请选择新文件"),
                             QStringLiteral("编码转换不会覆盖源文件，请选择其他路径。"));
        return;
    }
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *openTab = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (openTab && openTab != tab && !openTab->filePath().isEmpty()
            && sameDiskFilePath(openTab->filePath(), target)) {
            QMessageBox::warning(
                this, QStringLiteral("目标文件已打开"),
                QStringLiteral("为避免写入后仍显示旧内容，请关闭目标文件标签，或选择其他路径。"));
            return;
        }
    }

    struct ConversionResult { bool converted = false; QString error; };
    auto result = QSharedPointer<ConversionResult>::create();
    auto smallText = tab->isL2Active()
        ? QSharedPointer<QString>()
        : QSharedPointer<QString>::create(tab->editor()->text());
    const QString sourcePath = tab->filePath();
    const TextEncoding sourceEncoding = tab->displayEncoding();
    QProgressDialog progress(QStringLiteral("正在转换编码…"),
                             QStringLiteral("取消"), 0, 1000, this);
    progress.setObjectName(QStringLiteral("encodingConversionProgress"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setValue(0);

    QEventLoop loop;
    qint64 conversionTaskId = -1;
    const auto finishedConnection = connect(
        &AsyncTaskManager::instance(), &AsyncTaskManager::taskFinished, &loop,
        [&loop, &conversionTaskId](qint64 id, AsyncTaskState) {
            if (id == conversionTaskId)
                loop.quit();
        });
    const auto progressConnection = connect(
        &AsyncTaskManager::instance(), &AsyncTaskManager::taskProgressChanged,
        &progress, [&progress, &conversionTaskId](qint64 id, qint64 done, qint64 total) {
            if (id == conversionTaskId && total > 0)
                progress.setValue(int(qMin<qint64>(1000, done * 1000 / total)));
        });
    conversionTaskId = AsyncTaskManager::instance().submit(
AsyncTaskType::EncodingConversion, AsyncTaskPriority::Highest,
        QStringLiteral("正在转换文件编码"), QString(),
        [result, smallText, sourcePath, sourceEncoding, target, enc]
        (qint64, AsyncCancelPtr cancel, const auto &reportProgress,
         const auto &reportError) {
            if (smallText) {
                result->converted = writeTextAsEncoding(
                    target, *smallText, enc, &result->error);
                smallText->fill(QChar(0));
                smallText->clear();
            } else {
                result->converted = transcodeFileEncoding(
                    sourcePath, sourceEncoding, target, enc, &result->error,
                    cancel.data(), reportProgress);
            }
            if (!result->converted && result->error != QStringLiteral("转换已取消。")) {
                AsyncErrorInfo asyncError;
                asyncError.message = result->error.isEmpty()
                    ? QStringLiteral("编码转换未完成。") : result->error;
                asyncError.userMessage = QStringLiteral("编码转换未完成。请检查目标位置和所选编码，查看技术详情后重试。");
                reportError(asyncError);
            }
        }, 0, QStringLiteral("Encoding"), 1000, tab->isL2Active());
    connect(&progress, &QProgressDialog::canceled, this,
            [conversionTaskId] {
                AsyncTaskManager::instance().cancel(conversionTaskId);
            });
    progress.show();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    disconnect(finishedConnection);
    disconnect(progressConnection);
    progress.close();

    const bool converted = result->converted;
    const QString error = result->error;
    if (!converted) {
        if (error != QStringLiteral("转换已取消。")) {
            QMessageBox failure(QMessageBox::Warning, QStringLiteral("编码转换失败"),
                QStringLiteral("编码转换未完成。请检查目标位置和所选编码，查看技术详情后重试。"),
                QMessageBox::Ok, this);
            failure.setTextFormat(Qt::PlainText);
            // The reason (e.g. the byte that does not fit the encoding) is
            // what the user needs; keep it visible, not only in the details.
            failure.setInformativeText(error);
            failure.setDetailedText(error);
            localizeProductMessageBox(failure);
            failure.exec();
        }
        return;
    }
    if (!openPathUnified(QFileInfo(target).absoluteFilePath(), nullptr, enc)) {
        QMessageBox::warning(this, QStringLiteral("转换完成但打开失败"),
                             QStringLiteral("新文件已保存：\n%1").arg(target));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已转换为 %1，并打开新文件").arg(encodingName(enc)), 4000);
}

void MainWindow::displayEncodingFromAction()
{
    auto *act = qobject_cast<QAction *>(sender());
    if (!act) return;
    auto enc = static_cast<TextEncoding>(act->data().toInt());
    auto *tab = currentTab();
    if (!tab) return;
    if (tab->isSecureDocument() || tab->isUntitled()) {
        syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
        return;
    }
    if (tab->displayEncoding() == enc)
        return;

    // L1 rereads the saved bytes. A modified small document uses the ordinary
    // Save/Cancel decision; after Save it follows the exact unmodified path.
    if (!tab->isL2Active() && tab->isModified()) {
        const auto decision = QMessageBox::warning(
            this, QStringLiteral("保存修改"),
            QStringLiteral("“%1”有未保存的修改。要先保存再以 %2 编码显示吗？")
                .arg(tab->fileName(), encodingName(enc)),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (decision != QMessageBox::Save) {
            syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
            return;
        }
        if (!saveFile()) {
            syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
            return;
        }
    }

    tab->setDisplayEncoding(enc);
    syncEncodingMenuTo(m_encodingMenu, tab->encoding());
    syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
    updateStatusBar();
}


namespace {
TabVisualState tabVisualState(QWidget *page)
{
    auto *tab = qobject_cast<EditorTab *>(page);
    if (!tab)
        return {TabVisualState::Kind::Recovery, false, false};
    const TabVisualState::Kind kind = tab->isSecureDocument()
        ? TabVisualState::Kind::Secure
        : tab->isReadOnlyByPolicy() ? TabVisualState::Kind::ReadOnly
                                    : TabVisualState::Kind::Plain;
    return {kind, tab->isModified(), tab->isSecureDocumentMasked()};
}

void themeTabEditors(EditorTab *tab, bool highContrast, const ThemeTokens &t)
{
    for (QsciScintilla *surface : tab->themedEditors()) {
        if (highContrast)
            applyEditorSurfacePalette(surface, qApp->palette());
        else
            applyEditorTheme(surface, t, tab->isSecureDocument());
        // Highlights already on screen take the current hit color too.
        surface->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, 8UL,
                               scintillaColor(searchHitColor()));
    }
    tab->setProperty("themedSecure", tab->isSecureDocument());
}

void themeTabBar(QTabWidget *tabs, const ThemeTokens &t)
{
    auto *bar = qobject_cast<DocumentTabBar *>(tabs ? tabs->tabBar() : nullptr);
    if (!bar)
        return;
    bar->setStateProvider([tabs](int index) { return tabVisualState(tabs->widget(index)); });
    bar->setTokens(t);
}
} // namespace

void themeNewEditorTab(EditorTab *tab, bool highContrast, bool dark)
{
    if (tab)
        themeTabEditors(tab, highContrast, dark ? ThemeTokens::dark() : ThemeTokens::light());
}

void refreshTabSecurityCue(EditorTab *tab, bool highContrast, bool dark)
{
    // Only a change of security class re-themes (the secure gutter tint).
    if (tab && tab->property("themedSecure").toBool() != tab->isSecureDocument())
        themeNewEditorTab(tab, highContrast, dark);
}

void MainWindow::applyDarkMode(bool dark)
{
    if (!m_highContrastMode) {
        applyTheme(dark ? ThemeTokens::dark() : ThemeTokens::light());
        return;
    }
    // Let the platform own every color in accessibility mode; never put the
    // token style sheet on top of a user's custom Windows contrast scheme.
    const QPalette pal = QApplication::style()->standardPalette();
    qApp->setPalette(pal);
    qApp->setStyleSheet(QString());
    if (m_titleBar)
        m_titleBar->setTokens(ThemeTokens::light(), false);
    m_noticeBar->setTokens(ThemeTokens::light(), false);
    themeTabBar(m_tabs, tokensFromPalette(pal));
    searchHitColor() = pal.color(QPalette::Highlight);
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab)
            continue;
        themeTabEditors(tab, true, ThemeTokens::light());
    }
}

void MainWindow::showCommandPalette()
{
    if (!m_commandPalette)
        m_commandPalette = new CommandPalette(this);
    m_commandPalette->setTokens(m_highContrastMode ? tokensFromPalette(qApp->palette())
                                : m_darkMode ? ThemeTokens::dark() : ThemeTokens::light(),
                                !m_highContrastMode);
    QVector<PaletteEntry> entries;
    for (int i = 0; i < m_tabs->count(); ++i) {
        QPointer<QWidget> page = m_tabs->widget(i);
        auto *tab = qobject_cast<EditorTab *>(page.data());
        // A locked secure document must not reveal its title or path.
        const bool locked = tab && tab->isSecureDocumentMasked();
        entries.push_back({PaletteEntry::Group::OpenDocument,
                           locked ? QStringLiteral("已锁定文档") : m_tabs->tabText(i),
                           locked || !tab ? QString() : tab->filePath(), {}, true,
                           [this, page] { if (page) m_tabs->setCurrentWidget(page); }});
    }
    entries += commandPaletteEntries(this);
    entries += recentFileEntries(m_recentMenu);
    m_commandPalette->open(std::move(entries));
}

void MainWindow::applyTheme(const ThemeTokens &t)
{
    qApp->setPalette(applicationPalette(t));
    qApp->setStyleSheet(applicationStyleSheet(t));
    if (m_titleBar)
        m_titleBar->setTokens(t);
    if (m_noticeBar)
        m_noticeBar->setTokens(t);
    themeTabBar(m_tabs, t);
    searchHitColor() = t.hit;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(i));
        if (!tab)
            continue;
        themeTabEditors(tab, false, t);
    }
}

void MainWindow::applyAppearanceMode()
{
    m_highContrastMode = systemHighContrastEnabled();
    bool dark = false;
    switch (m_appearanceMode) {
    case AppearanceMode::Dark:
        dark = true;
        break;
    case AppearanceMode::Light:
        dark = false;
        break;
    case AppearanceMode::System: {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        dark = scheme == Qt::ColorScheme::Dark;
        if (scheme == Qt::ColorScheme::Unknown) {
            dark = QApplication::style()->standardPalette()
                       .color(QPalette::Window).lightness() < 128;
        }
        break;
    }
    }
    m_darkMode = dark;
    applyDarkMode(dark);
}

// ============ 标签事件 ============

EditorTab *MainWindow::currentTab() const
{
    if (m_destroying || !m_tabs)
        return nullptr;
    if (m_workspaceStack && m_workspaceStack->currentWidget() != m_tabs)
        return nullptr;
    return qobject_cast<EditorTab *>(m_tabs->currentWidget());
}

void MainWindow::syncCommandStates(EditorTab *tab)
{
    if (m_destroying)
        return;
    const bool documentWorkspace = isDocumentWorkspaceActive();
    if (!tab && documentWorkspace)
        tab = currentTab();
    if (!documentWorkspace)
        tab = nullptr;
    const bool hasDocument = tab != nullptr;
    const bool canModify = hasDocument && tab->canModifyCommand();
    const bool hasSelection = hasDocument && tab->hasSelectionCommand();
    const bool accountsWorkspace = m_workspaceStack && m_accountsPage
        && m_workspaceStack->currentWidget() == m_accountsPage;
    const QMimeData *clipboardData = QGuiApplication::clipboard()->mimeData();
    if (m_commandRegistry) {
        const DocumentState documentState = hasDocument
            ? tab->documentState() : DocumentState{};
        CommandContext context;
        context.document = hasDocument ? &documentState : nullptr;
        context.hasDocument = hasDocument;
        context.canModify = canModify;
        context.hasSelection = hasSelection;
        context.clipboardHasText = clipboardData && clipboardData->hasText()
            && !clipboardData->text().isEmpty();
        context.canUndo = hasDocument && tab->canUndoCommand();
        context.canRedo = hasDocument && tab->canRedoCommand();
        context.masked = hasDocument && tab->isSecureDocumentMasked();
        context.modifiedOrUntitled = hasDocument
            && (tab->isModified() || tab->isUntitled());
        context.accountsWorkspace = accountsWorkspace;
        context.accountCanSave = accountsWorkspace && m_vaultDock
            && m_vaultDock->canSaveActiveEntry();
        context.accountCanSearch = accountsWorkspace && m_vaultDock
            && m_vaultDock->canSearchAccounts();
        context.reloadable = hasDocument && (tab->isSecureDocument()
            ? !tab->filePath().isEmpty() : !tab->isUntitled());
        context.canCloseAll = documentWorkspace && m_tabs
            && m_tabs->count() > 0 && currentTab() != nullptr;
        context.readOnlyByPolicy = hasDocument && tab->isReadOnlyByPolicy();
        context.textMutationSupported = hasDocument
            && tab->supportsTextMutationCommands();
        context.nativeTextSurface = hasDocument && !tab->isL2Active()
            && !tab->isBinaryDocument() && !tab->isSecureDocumentMasked();
        context.lexerSupported = hasDocument && tab->supportsLexerCommands();
        context.editableHex = hasDocument && canModify && tab->isL2Active()
            && tab->viewModeL2() == ViewMode::Hex;
        context.hexInsertMode = context.editableHex && !tab->hexOverwriteMode();
        context.displayEncodingAvailable = hasDocument
            && !tab->isSecureDocument() && !tab->isUntitled();
        context.conversionAvailable = context.displayEncodingAvailable
            && !tab->isBinaryDocument();
        DocumentTailActions::installAndPopulate(m_tailActions, this, *m_commandRegistry, tab, context,
            [this] { return currentTab(); }, [this] { syncCommandStates(currentTab()); });
        m_commandRegistry->applyContext(context);
    }
    if (m_statusModified && accountsWorkspace) {
        m_statusModified->setText(m_vaultDock && m_vaultDock->canSaveActiveEntry()
                                      ? QStringLiteral("未保存")
                                      : QString());
    }
}

void MainWindow::onTabChanged(int index)
{
    EditorTab *tab = nullptr;
    if (index >= 0) tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (m_findDialog) m_findDialog->setActiveTab(tab);
    // 统一同步不适配 L2 的菜单项（含 index<0 无 tab → 全部变灰）
    syncCommandStates(tab);
    syncGlobalMenuCheckStates();
    if (!tab) {
        updateStatusBar();
        return;
    }
    // 同步编码菜单勾选到当前标签
    syncEncodingMenuTo(m_encodingMenu, tab->encoding());
    syncEncodingMenuTo(m_displayEncodingMenu, tab->displayEncoding());
    // 应用全局设置（保持所有标签一致）
    tab->setEdgeVisible(m_edgeVisible);
    tab->setEdgeColumn(m_edgeColumn);
    tab->setWordWrap(m_wordWrap);
    applyEditorPreferences(tab);
    updateStatusBar();
}

void MainWindow::showTabContextMenu(const QPoint &position)
{
    if (!m_tabs || !m_tabs->tabBar() || !isDocumentWorkspaceActive())
        return;

    QTabBar *bar = m_tabs->tabBar();
    int index = bar->tabAt(position);
    QPoint popupPosition = bar->mapToGlobal(position);
    // Qt sends (-1, -1) for the keyboard context-menu key. In that case use
    // the active tab and anchor the menu below its label.
    if (index < 0 && position.x() < 0 && m_tabs->currentIndex() >= 0) {
        index = m_tabs->currentIndex();
        const QRect rect = bar->tabRect(index);
        popupPosition = bar->mapToGlobal(QPoint(rect.left(), rect.bottom()));
    }
    if (index < 0)
        return;

    m_tabs->setCurrentIndex(index);
    auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (!tab)
        return;

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("tabContextMenu"));
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    QAction *save = menu->addAction(QStringLiteral("保存"));
    save->setObjectName(QStringLiteral("tabContext.save"));
    save->setShortcutVisibleInContextMenu(true);
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this]() { saveFile(); });

    QAction *saveAs = menu->addAction(QStringLiteral("另存为…"));
    saveAs->setObjectName(QStringLiteral("tabContext.saveAs"));
    saveAs->setShortcutVisibleInContextMenu(true);
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs, &QAction::triggered, this, [this]() { saveAsFile(); });

    menu->addSeparator();
    QAction *close = menu->addAction(QStringLiteral("关闭"));
    close->setObjectName(QStringLiteral("tabContext.close"));
    close->setShortcutVisibleInContextMenu(true);
    close->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(close, &QAction::triggered, this, [this, index]() {
        onTabCloseRequested(index);
    });

    QAction *closeOthers = menu->addAction(QStringLiteral("关闭其他标签"));
    closeOthers->setObjectName(QStringLiteral("tabContext.closeOthers"));
    closeOthers->setEnabled(m_tabs->count() > 1);
    connect(closeOthers, &QAction::triggered, this, [this, index]() {
        closeOtherTabs(index);
    });

    QAction *closeAll = menu->addAction(QStringLiteral("关闭全部标签"));
    closeAll->setObjectName(QStringLiteral("tabContext.closeAll"));
    connect(closeAll, &QAction::triggered, this, &MainWindow::closeAllTabs);

    // DraftStore paths are private implementation details. Only user-named
    // files expose path actions, including named encrypted .nsafe files.
    const QString path = tab->filePath();
    if (!path.isEmpty()) {
        menu->addSeparator();
        QAction *copyPath = menu->addAction(QStringLiteral("复制完整路径"));
        copyPath->setObjectName(QStringLiteral("tabContext.copyPath"));
        connect(copyPath, &QAction::triggered, this, [this, path]() {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
            statusBar()->showMessage(QStringLiteral("已复制文件路径"), 2500);
        });

        QAction *reveal = menu->addAction(QStringLiteral("在资源管理器中显示"));
        reveal->setObjectName(QStringLiteral("tabContext.reveal"));
        reveal->setEnabled(QFileInfo::exists(path));
        connect(reveal, &QAction::triggered, this, [this, path]() {
            bool opened = false;
#ifdef Q_OS_WIN
            opened = QProcess::startDetached(
                QStringLiteral("explorer.exe"),
                {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
            opened = QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
            if (!opened)
                statusBar()->showMessage(QStringLiteral("无法打开文件所在位置"), 5000);
        });
    }

    menu->addSeparator();
    if (tab->canSwitchWindowedPresentation()) {
        const bool showingHex = tab->viewModeL2() == ViewMode::Hex;
        QAction *switchView = menu->addAction(showingHex
            ? QStringLiteral("以文本查看") : QStringLiteral("以十六进制查看"));
        switchView->setObjectName(QStringLiteral("tabContext.switchPresentation"));
        connect(switchView, &QAction::triggered, this,
                [this, tab = QPointer<EditorTab>(tab), showingHex]() {
            if (!tab || !tab->canSwitchWindowedPresentation())
                return;
            QString error;
            if (!tab->reopenWindowedPresentation(showingHex
                    ? ViewMode::Text : ViewMode::Hex, &error)
                && !error.isEmpty()) {
                statusBar()->showMessage(error, 6000);
            }
            syncCommandStates(tab);
            updateStatusBar();
        });
    }

    menu->popup(popupPosition);
}

void MainWindow::closeTabUnchecked(int index, bool checkSave)
{
    if (!m_tabs || index < 0 || index >= m_tabs->count()
        || m_windowCloseInProgress || m_closingTabs.contains(m_tabs->widget(index))
        || WorkspaceRecoveryCoordinator::forWindow(this).isInFlight(m_tabs->widget(index)))
        return;
    const QPointer<MainWindow> owner(this);
    const QPointer<QWidget> widget(m_tabs->widget(index));
    QWidget *const closingIdentity = widget.data();
    m_closingTabs.insert(closingIdentity);
    const auto releaseClose = qScopeGuard([owner, closingIdentity] {
        if (owner)
            owner->m_closingTabs.remove(closingIdentity);
    });
    // Own the widget before Save can open a modal loop or cancel a task.
    if (checkSave && !maybeSaveTab(index, /*closingApp=*/false))
        return;
    if (!owner || !widget || m_tabs->indexOf(widget) < 0)
        return;
    const QPointer<EditorTab> tab(qobject_cast<EditorTab *>(widget.data()));
    if (tab) {
        if (m_searchDock)
            m_searchDock->closeResultsFor(tab->documentState().id);
        if (!owner || !tab)
            return;
        if (tab->isSecureDocument() && m_securePersistence)
            closeSecureDocumentOnSecurityLane(tab->secureDocumentId());
        else
            m_session->discardRecovery(tab);
        if (!owner || !tab)
            return;
        tab->shutdownL2BeforeClose();
    }
    if (!owner || !widget || m_tabs->indexOf(widget) < 0)
        return;
    WorkspaceRecoveryCoordinator::forWindow(this).noteAcceptedRemoval(widget);
    m_tabs->removeTab(m_tabs->indexOf(widget));
    delete widget.data();
}

void MainWindow::closeOtherTabs(int keepIndex)
{
    if (!m_tabs || keepIndex < 0 || keepIndex >= m_tabs->count())
        return;
    QPointer<EditorTab> keep = qobject_cast<EditorTab *>(m_tabs->widget(keepIndex));
    if (!keep)
        return;

    // Preflight all affected documents before deleting any of them, matching
    // Close All's all-or-nothing cancellation behavior.
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->widget(index) == keep)
            continue;
        if (!maybeSaveTab(index, false)) {
            const int currentKeepIndex = m_tabs->indexOf(keep);
            if (currentKeepIndex >= 0)
                m_tabs->setCurrentIndex(currentKeepIndex);
            return;
        }
    }
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        if (m_tabs->widget(index) != keep)
            closeTabUnchecked(index);
    }
    const int currentKeepIndex = m_tabs->indexOf(keep);
    if (currentKeepIndex >= 0)
        m_tabs->setCurrentIndex(currentKeepIndex);
}

void MainWindow::onTabCloseRequested(int index)
{
    closeTabUnchecked(index, /*checkSave=*/true);
}

bool MainWindow::maybeSaveTab(int index, bool closingApp)
{
    auto *tab = qobject_cast<EditorTab *>(m_tabs->widget(index));
    if (!tab)
        return !qobject_cast<RecoveryIssuePage *>(m_tabs->widget(index)) || closingApp;

    // 加密笔记采用 Note 风格自动保存；绝不进入明文 autosave，也不弹出
    // “另存为”对话框。保存失败时阻止关闭，避免静默丢失。
    if (tab->isSecureDocument()) {
        if (!tab->isModified())
            return true;
        return saveSecureTab(tab, true, true);
    }

    // 无论何种情况，内容未修改都直接放行
    if (!tab->isModified()) {
        // 空内容的未命名文件：直接清 autosave（没内容就没必要缓存）
        if (tab->isUntitled() && tab->editor()->text().trimmed().isEmpty())
            m_session->discardRecovery(tab);
        return true;
    }

    // ===== 关闭整个应用的 Notepad++ 逻辑 =====
    if (closingApp) {
        // ① "未命名+有内容"：不提示用户，由后续 saveSession 写入 autosave（下次恢复）
        if (tab->isUntitled())
            return true;
        // ② 从 autosave 会话恢复的 tab + autosave 文件还存在：
        //    数据已在 autosave 里，closeEvent 紧接着的 saveSessionNow() 会再落盘一次，
        //    下次启动还能完整恢复（等价于 ① 的"未命名 autosave"语义，只是对用户显示原名）。
        //    若每次恢复后关闭都弹「是否保存」，用户会被"我没改过东西也弹窗"困惑。
        const QString &asp = tab->autosavePath();
        if (!asp.isEmpty() && QFileInfo::exists(asp))
            return true;
        // ③ 已存在文件+用户修改过+没有 autosave 兜底：必须提示保存/丢弃/取消
    }

    // ===== 单标签关闭：未命名且有内容也要提示 =====
    // （下面的代码对 closingApp=true 的"已存在文件修改"以及 closingApp=false 的全部情况都提示）

    auto ret = QMessageBox::warning(
        this, QStringLiteral("Notepad Safe"),
        QStringLiteral("标签 \"%1\" 已修改。\n是否保存更改？").arg(tab->fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (ret == QMessageBox::Save) {
        m_tabs->setCurrentIndex(index);
        if (tab->isUntitled()) {
            // 单标签关闭时的另存为
            bool ok = saveAsFile();
            // 如果用户取消另存为对话框，则视为取消操作
            return ok;
        }
        return saveFile();
    }
    if (ret == QMessageBox::Cancel)
        return false;
    // Discard: 丢弃改动
    if (!tab->isSecureDocument())
        m_session->discardRecovery(tab);
    return true;
}

void MainWindow::onTabModifiedChanged(bool modified)
{
    Q_UNUSED(modified)
    // Size is the live document payload, not the backing file on disk. This is
    // especially important for secure documents, whose KDBX container size is
    // unrelated to the text the user is editing.
    updateStatusBar();
}

void MainWindow::onTabCursorPositionChanged(int line, int index)
{
    EditorTab *tab = currentTab();
    if (tab && tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
        // Hex uses cursorPositionChanged for both byte movement and OVR/INS
        // mode changes.  Refresh the complete permanent status contract so
        // the size/tier/mode segment cannot retain a stale INS/OVR value.
        updateStatusBar();
        return;
    }
    QString positionDescription;
    m_statusPos->setText(documentPositionText(tab, line, index, &positionDescription));
    m_statusPos->setAccessibleDescription(positionDescription);
}

void MainWindow::onTabTitleChanged(const QString &title)
{
    Q_UNUSED(title);
}

void MainWindow::onTabEncodingChanged(TextEncoding encoding)
{
    const EditorTab *tab = currentTab();
    const QString origin = tab && tab->encodingHasDecodeErrors()
        ? QStringLiteral(" · 包含非法字节")
        : tab && tab->encodingOrigin() == EncodingOrigin::Fallback
            ? QStringLiteral(" · 默认假设") : QString();
    m_statusEncoding->setText(encodingName(encoding) + origin);
    if (tab)
        m_statusEol->setText(tab->lineEndingName());
    syncEncodingMenuTo(m_encodingMenu, encoding);
}

void MainWindow::onJumpToByteOffset(qint64 byteOffset, int length)
{
    auto *tab = currentTab();
    if (!tab) return;
    tab->gotoByteOffset(byteOffset, length);
    tab->setFocusEditor();
}

void MainWindow::onJumpToDocumentByteOffset(const QUuid &documentId,
                                             quint64 expectedRevision,
                                             qint64 byteOffset, int length)
{
    EditorTab *tab = nullptr;
    for (int index = 0; m_tabs && index < m_tabs->count(); ++index) {
        auto *candidate = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (candidate && candidate->documentState().id == documentId) {
            tab = candidate;
            break;
        }
    }
    if (!tab)
        return;
    if (tab->contentRevision() != expectedRevision) {
        if (m_searchDock)
            m_searchDock->invalidateResultsFor(documentId);
        return;
    }
    const int tabIndex = m_tabs->indexOf(tab);
    if (tabIndex < 0)
        return;
    showDocumentWorkspace();
    m_tabs->setCurrentIndex(tabIndex);
    QPointer<EditorTab> target(tab);
    QTimer::singleShot(0, tab, [target, documentId, expectedRevision, byteOffset, length]() {
        // Revalidate at delivery: edits or document replacement can occur
        // after the request passed the early check and before this queued turn.
        if (!target || target->documentState().id != documentId
            || target->contentRevision() != expectedRevision)
            return;
        target->gotoByteOffset(byteOffset, length);
        target->setFocusEditor();
    });
}

void MainWindow::updateStatusBar()
{
    if (!isDocumentWorkspaceActive())
        return;
    auto *tab = currentTab();
    if (!tab) {
        // An empty document workspace must not retain metadata from the tab
        // that was just closed. The task chip remains independently visible
        // when background work exists.
        if (m_statusPos) m_statusPos->hide();
        if (m_statusEncoding) m_statusEncoding->hide();
        if (m_statusSyntax) m_statusSyntax->hide();
        m_statusEol->hide();
        m_statusZoom->hide();
        m_statusSecurity->hide();
        if (m_statusSize) m_statusSize->hide();
        if (m_statusCharacters) m_statusCharacters->hide();
        if (m_hexEditModeButton) m_hexEditModeButton->hide();
        if (m_statusModified) m_statusModified->hide();
        return;
    }
    if (m_statusPos) m_statusPos->show();
    if (m_statusEncoding) m_statusEncoding->show();
    if (m_statusSyntax) m_statusSyntax->show();
    m_statusSyntax->setEnabled(true);
    m_statusZoom->show();
    if (m_statusSize) m_statusSize->show();
    if (m_statusCharacters) m_statusCharacters->show();
    if (m_statusModified) m_statusModified->show();

    int line, index;
    tab->getCursorPosition(&line, &index);
    const bool hexMode = tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex;
    if (m_hexEditModeButton) {
        m_hexEditModeButton->setVisible(hexMode);
        if (hexMode) {
            const bool overwrite = tab->hexOverwriteMode();
            m_hexEditModeButton->setText(overwrite ? QStringLiteral("OVR")
                                                    : QStringLiteral("INS"));
            m_hexEditModeButton->setEnabled(tab->canModifyCommand());
            m_hexEditModeButton->setToolTip(overwrite
                ? QStringLiteral("覆盖模式：输入替换现有字节。单击或按 Insert 切换为插入模式")
                : QStringLiteral("插入模式：输入新增字节。单击或按 Insert 切换为覆盖模式"));
            m_hexEditModeButton->setAccessibleDescription(
                m_hexEditModeButton->toolTip());
        }
    }
    if (m_hexInsertModeAction) {
        m_hexInsertModeAction->setEnabled(hexMode && tab->canModifyCommand());
        m_hexInsertModeAction->setChecked(hexMode && !tab->hexOverwriteMode());
    }
    QString positionDescription;
    m_statusPos->setText(documentPositionText(tab, line, index, &positionDescription));
    m_statusPos->setAccessibleDescription(positionDescription);
    const QString origin = tab->encodingHasDecodeErrors()
        ? QStringLiteral(" · 包含非法字节")
        : tab->encodingOrigin() == EncodingOrigin::Fallback
            ? QStringLiteral(" · 默认假设") : QString();
    m_statusEncoding->setText(hexMode
                                     ? QStringLiteral("原始字节 · 字符区 %1")
                                           .arg(encodingName(tab->displayEncoding()))
                                     : encodingName(tab->encoding()) + origin);
    m_statusEol->setText(tab->lineEndingName());
    m_statusEol->setVisible(!hexMode);
    m_statusSecurity->setVisible(tab->isSecureDocument());
    m_statusSecurity->setText(tab->isSecureDocumentMasked() ? QStringLiteral("已加密 · 已锁定")
                                                            : QStringLiteral("已加密"));
    const int zoomPoints = int(tab->editor()->SendScintilla(QsciScintillaBase::SCI_GETZOOM));
    m_statusZoom->setText(QString::number(qRound(100.0 * (10 + zoomPoints) / 10.0))
                          + QLatin1Char('%'));

    const QStringList syntaxNames = {
        QStringLiteral("纯文本"), QStringLiteral("C/C++"),
        QStringLiteral("Python"), QStringLiteral("JSON"),
        QStringLiteral("HTML"), QStringLiteral("XML"),
        QStringLiteral("SQL"), QStringLiteral("YAML"),
        QStringLiteral("Markdown"), QStringLiteral("JavaScript"),
        QStringLiteral("Java"), QStringLiteral("C#"),
        QStringLiteral("CSS"), QStringLiteral("Shell"),
        QStringLiteral("Batch"), QStringLiteral("CMake"),
        QStringLiteral("Makefile"), QStringLiteral("Lua"),
        QStringLiteral("Ruby"), QStringLiteral("Perl"),
        QStringLiteral("Diff/Patch"), QStringLiteral("Properties/INI"),
        QStringLiteral("Assembly"),
    };
    int s = static_cast<int>(tab->syntax());
    if (hexMode)
        m_statusSyntax->setText(QStringLiteral("十六进制"));
    else if (s >= 0 && s < syntaxNames.size())
        m_statusSyntax->setText(syntaxNames[s]);

    const auto presentation = documentStatusPresentation(*tab);
    m_statusSize->setText(presentation.sizeText);
    m_statusSize->setToolTip(presentation.sizeDescription);
    m_statusSize->setAccessibleDescription(presentation.sizeDescription);
    m_statusCharacters->setText(presentation.characterText);
    m_statusCharacters->setToolTip(presentation.characterDescription);
    m_statusCharacters->setAccessibleDescription(presentation.characterDescription);

    m_statusModified->setText(tab->isModified() ? QStringLiteral("● 已修改") : QStringLiteral(""));
}

void MainWindow::showWorkspaceStatus(const QString &workspaceName)
{
    if (m_statusPos) m_statusPos->hide();
    if (m_statusEncoding) m_statusEncoding->hide();
    if (m_statusSize) m_statusSize->hide();
    if (m_statusCharacters) m_statusCharacters->hide();
    if (m_hexEditModeButton) m_hexEditModeButton->hide();
    if (m_statusModified) m_statusModified->hide();
    m_statusEol->hide();
    m_statusZoom->hide();
    m_statusSecurity->hide();
    // A workspace name is not a language: no language menu here.
    m_statusSyntax->setEnabled(false);
    if (m_statusSyntax) {
        m_statusSyntax->setText(workspaceName);
        m_statusSyntax->show();
    }
}

// ============ 最近文件 ============

void MainWindow::updateRecentFiles(const QString &path)
{
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    DiskFileVersion candidate;
    const bool candidateValid = captureDiskFileVersion(normalized, &candidate)
                                == DiskFileState::Unchanged;
    for (int i = m_recentFiles.size() - 1; i >= 0; --i) {
        const QString existingPath = m_recentFiles.at(i);
        bool duplicate = sameDiskFilePath(existingPath, normalized);
        if (!duplicate && candidateValid) {
            DiskFileVersion existing;
            duplicate = captureDiskFileVersion(existingPath, &existing)
                            == DiskFileState::Unchanged
                        && sameDiskFile(candidate, existing);
        }
        if (duplicate)
            m_recentFiles.removeAt(i);
    }
    m_recentFiles.prepend(normalized);
    while (m_recentFiles.size() > MAX_RECENT)
        m_recentFiles.removeLast();
    rebuildRecentFilesMenu();
    saveSettings();
}

void MainWindow::rebuildRecentFilesMenu()
{
    m_recentMenu->clear();
    if (m_recentFiles.isEmpty()) {
        auto *act = m_recentMenu->addAction(QStringLiteral("(无)"));
        act->setEnabled(false);
        return;
    }
    for (const QString &file : m_recentFiles) {
        QString name = QFileInfo(file).fileName();
        auto *act = m_recentMenu->addAction(name + "  [" + file + "]");
        act->setData(file);
        connect(act, &QAction::triggered, this, &MainWindow::openRecentFile);
    }
    m_recentMenu->addSeparator();
    auto *clearAct = m_recentMenu->addAction(QStringLiteral("清除列表"));
    connect(clearAct, &QAction::triggered, this, &MainWindow::clearRecentFiles);
}

// ============ 拖放 ============

// 判断 MIME 是否含至少一个可打开的本地文件
static QStringList filePathsFromMime(const QMimeData *mime)
{
    QStringList res;
    if (!mime || !mime->hasUrls()) return res;
    for (const QUrl &u : mime->urls()) {
        QString p = u.toLocalFile();
        if (u.isLocalFile() && !p.isEmpty())
            res << p;
    }
    return res;
}

static bool mimeHasLocalFiles(const QMimeData *mime)
{
    return !filePathsFromMime(mime).isEmpty();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // QScintilla consumes editing shortcuts and its native context menu before
    // the window-level QActions can run. Route every editor copy/cut entry path
    // through the same commands so secure-document clipboard fingerprints are
    // always registered and can be conditionally cleared at lock time.
    EditorTab *sourceTab = nullptr;
    for (int index = 0; m_tabs && index < m_tabs->count(); ++index) {
        auto *candidate = qobject_cast<EditorTab *>(m_tabs->widget(index));
        if (candidate && candidate->ownsUserInputObject(watched)) {
            sourceTab = candidate;
            break;
        }
    }
    if (sourceTab && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->matches(QKeySequence::Copy)) {
            if (sourceTab->hasSelectionCommand()) {
                sourceTab->copy();
                rememberSensitiveDocumentClipboard(sourceTab);
            }
            keyEvent->accept();
            return true;
        }
        if (keyEvent->matches(QKeySequence::Cut)) {
            if (sourceTab->hasSelectionCommand()) {
                sourceTab->cut();
                rememberSensitiveDocumentClipboard(sourceTab);
            }
            keyEvent->accept();
            return true;
        }
    }
    if (sourceTab && event->type() == QEvent::ContextMenu) {
        auto *contextEvent = static_cast<QContextMenuEvent *>(event);
        QMenu menu(this);
        menu.addAction(m_undoAction);
        menu.addAction(m_redoAction);
        menu.addSeparator();
        menu.addAction(m_cutAction);
        menu.addAction(m_copyAction);
        menu.addAction(m_pasteAction);
        menu.addSeparator();
        menu.addAction(m_selectAllAction);
        menu.exec(contextEvent->globalPos());
        contextEvent->accept();
        return true;
    }

    // 统一转发 DragEnter/DragMove/Drop：
    // 经过 registerDropProxy 注册过的控件（m_tabs/每个EditorTab/每个QsciScintilla/其viewport）
    // 都会安装本事件过滤器并关闭 acceptDrops。这里把它们的 drag 事件都转给 MainWindow 处理。
    // 这就是修复"拖放到编辑区有时没反应"的关键。
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *e = static_cast<QDragEnterEvent *>(event);
        if (mimeHasLocalFiles(e->mimeData())) {
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *e = static_cast<QDragMoveEvent *>(event);
        if (mimeHasLocalFiles(e->mimeData())) {
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::Drop: {
        auto *e = static_cast<QDropEvent *>(event);
        QStringList paths = filePathsFromMime(e->mimeData());
        if (!paths.isEmpty()) {
            openFilesFromExternal(paths);
            e->acceptProposedAction();
            return true;
        }
        break;
    }
    default: break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (mimeHasLocalFiles(event->mimeData()))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths = filePathsFromMime(event->mimeData());
    if (paths.isEmpty())
        return;
    openFilesFromExternal(paths);
    event->acceptProposedAction();
}
