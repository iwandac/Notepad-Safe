#include "SaveAsDialog.h"

#include <QDialog>
#include <QFileDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <utility>

std::optional<SaveAsSelection> SaveAsDialog::choose(
    QWidget *parent, const QString &suggestion,
    DocumentPersistence currentPersistence, bool binaryDocument,
    bool allowSecure)
{
    const QString secureFilter = QStringLiteral(
        "Notepad Safe 安全文档 (*.nsafe)");
    const QString plainFilter = QStringLiteral("未加密文件 (*)");
    QString selectedFilter = allowSecure
            && currentPersistence == DocumentPersistence::SecureFile
        ? secureFilter : plainFilter;

    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("saveAsDialog"));
    dialog.setWindowTitle(QStringLiteral("另存为"));
    dialog.resize(940, 650);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto *fileDialog = new QFileDialog(
        &dialog, QString(), suggestion,
        allowSecure ? secureFilter + QStringLiteral(";;") + plainFilter
                    : plainFilter);
    fileDialog->setObjectName(QStringLiteral("saveAsFileDialog"));
    fileDialog->setWindowFlags(Qt::Widget);
    fileDialog->setAcceptMode(QFileDialog::AcceptSave);
    fileDialog->setFileMode(QFileDialog::AnyFile);
    fileDialog->setOption(QFileDialog::DontUseNativeDialog, true);
    fileDialog->setLabelText(QFileDialog::LookIn, QStringLiteral("位置："));
    fileDialog->setLabelText(QFileDialog::FileName, QStringLiteral("文件名："));
    fileDialog->setLabelText(QFileDialog::FileType, QStringLiteral("保存类型："));
    fileDialog->setLabelText(QFileDialog::Accept, QStringLiteral("保存"));
    fileDialog->setLabelText(QFileDialog::Reject, QStringLiteral("取消"));
    fileDialog->selectNameFilter(selectedFilter);
    layout->addWidget(fileDialog, 1);

    auto *notice = new QLabel(&dialog);
    notice->setObjectName(QStringLiteral("saveAsSecurityNotice"));
    notice->setWordWrap(true);
    notice->setContentsMargins(8, 6, 8, 6);
    notice->setMinimumHeight(42);
    layout->addWidget(notice);
    const QString plainSuffix = binaryDocument ? QStringLiteral("bin")
                                               : QStringLiteral("txt");
    const auto updateNotice = [fileDialog, notice, secureFilter,
                               plainSuffix](const QString &filter) {
        if (filter == secureFilter) {
            fileDialog->setDefaultSuffix(QStringLiteral("nsafe"));
            notice->setText(QStringLiteral(
                "将保存为离线加密的 Notepad Safe 文档。普通保存会继续保持此格式。"));
            notice->setStyleSheet(QStringLiteral(
                "QLabel { color: #246b45; background: #eaf7ef; border: 1px solid #b9dfc8; "
                "border-radius: 4px; }"));
            const QStringList selected = fileDialog->selectedFiles();
            if (!selected.isEmpty()
                && !selected.constFirst().endsWith(
                    QStringLiteral(".nsafe"), Qt::CaseInsensitive)) {
                fileDialog->selectFile(selected.constFirst()
                                       + QStringLiteral(".nsafe"));
            }
            return;
        }

        fileDialog->setDefaultSuffix(plainSuffix);
        notice->setText(QStringLiteral(
            "将创建未加密文件，其他程序可以直接读取其内容。普通保存会继续保持此格式。"));
        notice->setStyleSheet(QStringLiteral(
            "QLabel { color: #7a4b00; background: #fff5dd; border: 1px solid #e8c77a; "
            "border-radius: 4px; }"));
        const QStringList selected = fileDialog->selectedFiles();
        if (!selected.isEmpty()
            && selected.constFirst().endsWith(
                QStringLiteral(".nsafe"), Qt::CaseInsensitive)) {
            QString plainName = selected.constFirst();
            plainName.chop(QStringLiteral(".nsafe").size());
            plainName += QLatin1Char('.') + plainSuffix;
            fileDialog->selectFile(plainName);
        }
    };
    QObject::connect(fileDialog, &QFileDialog::filterSelected,
                     &dialog, updateNotice);
    QObject::connect(fileDialog, &QFileDialog::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(fileDialog, &QFileDialog::rejected,
                     &dialog, &QDialog::reject);
    updateNotice(selectedFilter);

    if (dialog.exec() != QDialog::Accepted
        || fileDialog->selectedFiles().isEmpty()) {
        return std::nullopt;
    }
    QString path = fileDialog->selectedFiles().constFirst();
    selectedFilter = fileDialog->selectedNameFilter();
    const DocumentPersistence persistence = allowSecure
            && selectedFilter == secureFilter
        ? DocumentPersistence::SecureFile : DocumentPersistence::PlainFile;
    if (persistence == DocumentPersistence::PlainFile
        && path.endsWith(QStringLiteral(".nsafe"), Qt::CaseInsensitive)) {
        path.chop(QStringLiteral(".nsafe").size());
        path += QLatin1Char('.') + plainSuffix;
    }
    return SaveAsSelection{std::move(path), persistence};
}
