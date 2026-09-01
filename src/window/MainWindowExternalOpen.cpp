#include "MainWindow.h"

#include "AppComposition.h"
#include "EditorTab.h"
#include "MessageBoxPresentation.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolButton>

// ============ 外部文件打开（命令行/拖放通用） ============

namespace {

void showExternalOpenFailures(QWidget *parent, const QStringList &failures)
{
    if (failures.isEmpty()) return;
    QMessageBox box(QMessageBox::Warning, QStringLiteral("错误"),
                   QStringLiteral("以下文件未打开；已打开的其他文件不受影响。"),
                   QMessageBox::Ok, parent);
    box.setObjectName(QStringLiteral("externalOpenFailuresDialog"));
    box.setTextFormat(Qt::PlainText);
    const QString fullDetails = failures.join(QStringLiteral("\n\n"));
    if (failures.size() > 3 || fullDetails.size() > 1200) {
        QStringList preview;
        for (const QString &failure : failures.mid(0, 3))
            preview << failure.left(320);
        box.setInformativeText(QStringLiteral("共 %1 个文件未打开。以下为部分预览；请展开详细信息查看全部路径、原因和下一步。\n\n%2")
                                   .arg(failures.size()).arg(preview.join(QStringLiteral("\n\n"))));
        box.setDetailedText(fullDetails);
    } else {
        box.setInformativeText(fullDetails);
    }
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    localizeProductMessageBox(box);
    box.exec();
}

} // namespace

void MainWindow::openFilesFromExternal(const QStringList &paths)
{
    if (paths.isEmpty()) return;
    QStringList failures;
    QString firstFailurePath;
    const auto recordFailure = [&](const QString &path, const QString &reason) {
        if (failures.isEmpty()) firstFailurePath = path;
        failures << path + QLatin1Char('\n') + reason;
    };
    bool ordinaryFailure = false;
    int anyEmptyUntitledIdx = -1;
    // 只有当只有一个"未命名 且 空 且 未修改"的标签时，第一个文件才覆盖该标签
    if (m_tabs->count() == 1) {
        auto *t0 = qobject_cast<EditorTab *>(m_tabs->widget(0));
        if (t0 && !t0->isSecureDocument()
            && t0->isUntitled() && !t0->isModified()
            && t0->editor()->text().isEmpty()) {
            anyEmptyUntitledIdx = 0;
        }
    }
    int idx = 0;
    for (const QString &p : paths) {
        if (p.isEmpty()) continue;
        // This early check provides fast feedback, not authorization. The
        // final route below independently enforces DenyInteractiveUnlock.
        const auto persistenceAdapter = QFileInfo(p).isFile()
            ? m_composition->persistenceAdapters().resolve(p) : std::nullopt;
        if (persistenceAdapter
            && persistenceAdapter->persistence
                == DocumentPersistence::SecureFile) {
            recordFailure(p, QStringLiteral("为保护安全文件，外部请求不会解锁；请在窗口内用文件→打开。"));
            ++idx;
            continue;
        }
        EditorTab *reuseTab = nullptr;
        if (idx == 0 && anyEmptyUntitledIdx >= 0)
            reuseTab = qobject_cast<EditorTab *>(m_tabs->widget(anyEmptyUntitledIdx));
        // openPathUnified owns tab creation for every non-placeholder file.
        // This keeps drag/drop and command-line opens on the same duplicate-
        // detection path as File > Open and Recent Files.
        QString failureReason;
        bool securityRefused = false;
        if (!openPathUnified(
                p, reuseTab, std::nullopt,
                OpenPresentationPolicy::PreserveCurrentWorkspace,
                OpenAuthorizationPolicy::DenyInteractiveUnlock,
                &failureReason, &securityRefused)) {
            recordFailure(p, failureReason);
            ordinaryFailure = ordinaryFailure || !securityRefused;
        }
        ++idx;
    }
    // Keep exactly one completed batch. A new successful batch clears the
    // old record; an empty call above leaves it alone. No history is persisted.
    m_externalOpenFailures = failures;
    if (!m_externalOpenFailuresButton && !failures.isEmpty()) {
        m_externalOpenFailuresButton = new QToolButton(statusBar());
        m_externalOpenFailuresButton->setObjectName(QStringLiteral("externalOpenFailuresButton"));
        m_externalOpenFailuresButton->setFocusPolicy(Qt::StrongFocus);
        m_externalOpenFailuresButton->setToolTip(QStringLiteral("查看最近一次外部打开中未打开的文件、原因和下一步；不会重新打开文件。"));
        m_externalOpenFailuresButton->setAccessibleDescription(QStringLiteral("查看最近一次外部打开的失败原因和下一步；不会重新打开文件或解锁。"));
        connect(m_externalOpenFailuresButton, &QToolButton::clicked,
                this, [this]() { showExternalOpenFailures(this, m_externalOpenFailures); });
        statusBar()->addPermanentWidget(m_externalOpenFailuresButton);
    }
    if (m_externalOpenFailuresButton) {
        m_externalOpenFailuresButton->setText(QStringLiteral("未打开文件（%1）").arg(failures.size()));
        m_externalOpenFailuresButton->setAccessibleName(m_externalOpenFailuresButton->text());
        m_externalOpenFailuresButton->setVisible(!failures.isEmpty());
    }
    if (failures.isEmpty()) return;
    const QString summary = ordinaryFailure
        ? QStringLiteral("%1 未打开。请查看“未打开文件”中的原因和下一步。").arg(QFileInfo(firstFailurePath).fileName())
        : QStringLiteral("%1 未打开。为保护安全文件，外部请求不会解锁；请在窗口内用文件→打开。").arg(QFileInfo(firstFailurePath).fileName());
    statusBar()->showMessage(summary, 8000);
    if (ordinaryFailure)
        showExternalOpenFailures(this, m_externalOpenFailures);
}
