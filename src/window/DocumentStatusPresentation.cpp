#include "DocumentStatusPresentation.h"

#include "EditorTab.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

#include <QLocale>
#include <QStringList>

namespace {

QString formattedSize(qint64 size)
{
    if (size < 1024) return QStringLiteral("%1 B").arg(size);
    if (size < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(size / 1024.0, 0, 'f', 1);
    if (size < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(size / (1024.0 * 1024), 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(size / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

QString sizeSourceDescription(const EditorTab &tab)
{
    // Match the existing contentSizeBytes() authority order. The runtime
    // identifies the read-only source; an open editable byte view precedes
    // the ordinary disk baseline, which precedes in-memory text.
    if (tab.largeDocumentRuntimeState()
        == EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis)
        return QStringLiteral("当前读取视图的源文件大小。");
    if (tab.isL2Mode() && tab.l2View() && tab.l2View()->isOpen())
        return QStringLiteral("当前字节内容大小，包含尚未保存的字节修改。");
    if (!tab.isSecureDocument() && tab.persistenceBaseline().valid)
        return QStringLiteral("打开或上次保存时的文件大小，不包含未保存修改，也不是磁盘实时大小。");
    return QStringLiteral("当前内存文本按 UTF-8 计算的字节大小，不代表保存后的文件大小。");
}

const WindowedReadOnlyProvider *activeReadOnlyTextProvider(const EditorTab *tab)
{
    if (!tab || !tab->isReadOnlyByPolicy() || !tab->isL2Active()) return nullptr;
    const auto state = tab->documentState();
    if (state.securityClass != DocumentSecurityClass::OrdinaryDocument
        || !state.capabilities.testFlag(DocumentCapability::Read)
        || state.capabilities.testFlag(DocumentCapability::Edit)) return nullptr;
    const auto *provider = tab->windowedReadOnlyProvider();
    if (!provider || !provider->hasSource() || !provider->sourceVersion().valid
        || provider->presentation() != WindowedReadOnlyProvider::Presentation::Text
        || tab->editor() != provider->editor()
        || !sameDiskFilePath(provider->sourceVersion().absolutePath, state.location)) return nullptr;
    return provider;
}

} // namespace

QString documentPositionText(const EditorTab *tab, int line, int index, QString *description)
{
    QString label;
    qint64 offset = 0;
    if (tab && tab->isL2Active() && tab->viewModeL2() == ViewMode::Hex) {
        label = QStringLiteral("偏移");
        offset = tab->currentByteOffset();
        if (description) *description = QStringLiteral("当前十六进制视图的字节偏移。");
    } else if (const auto *provider = activeReadOnlyTextProvider(tab)) {
        label = QStringLiteral("浏览偏移");
        offset = provider->globalOffset();
        if (description)
            *description = QStringLiteral("浏览偏移是当前只读视图已定位的源文件字节锚点，不代表文本光标的全局行列。");
    } else {
        if (description) *description = QStringLiteral("文本光标的行与列，从 1 开始。");
        return QStringLiteral("行: %1, 列: %2").arg(line + 1).arg(index + 1);
    }
    return QStringLiteral("%1: 0x%2").arg(label,
        QString::number(qMax<qint64>(0, offset), 16).rightJustified(8, QLatin1Char('0')).toUpper());
}

DocumentStatusPresentation documentStatusPresentation(const EditorTab &tab)
{
    DocumentStatusPresentation result;
    const bool hex = tab.isHexPresentation();
    const auto capabilities = tab.documentState().capabilities;
    const bool editable = capabilities.testFlag(DocumentCapability::Edit);
    result.sizeText = QStringLiteral("%1 · %2 · %3 · %4")
        .arg(formattedSize(tab.contentSizeBytes()),
             tab.isBinaryDocument() ? QStringLiteral("二进制") : QStringLiteral("文本"),
             hex ? QStringLiteral("十六进制") : QStringLiteral("文本视图"),
             editable ? QStringLiteral("可编辑") : QStringLiteral("只读"));
    result.sizeDescription = sizeSourceDescription(tab);
    if (editable) {
        result.sizeDescription += QStringLiteral(" 可编辑当前内容。");
    } else {
        result.sizeDescription += QStringLiteral(" 当前内容只读。");
        QStringList available;
        if (capabilities.testFlag(DocumentCapability::Read)) available << QStringLiteral("浏览");
        if (capabilities.testFlag(DocumentCapability::Search)) available << QStringLiteral("查找");
        if (capabilities.testFlag(DocumentCapability::Copy)) available << QStringLiteral("复制");
        if (!available.isEmpty())
            result.sizeDescription += QStringLiteral(" 可%1。").arg(available.join(QStringLiteral("、")));
    }

    const qint64 characters = tab.contentCharacterCount();
    if (hex) {
        result.characterText = QStringLiteral("字节数: %1").arg(QLocale().toString(characters));
        result.characterDescription = QStringLiteral("当前十六进制视图的字节总数。");
    } else if (tab.isBinaryDocument()) {
        result.characterText = QStringLiteral("字符数: 不做全文统计");
        result.characterDescription = QStringLiteral("二进制文件的文本视图仅解码当前浏览内容，不提供全文字符总数。");
    } else if (tab.largeDocumentRuntimeState() == EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis
               && tab.windowedReadOnlyProvider() && tab.windowedReadOnlyProvider()->statisticsDeferred()) {
        result.characterText = QStringLiteral("字符数: 未更新");
        result.characterDescription = QStringLiteral("文件持续变化，行号和字符统计未更新；跟随模式不会为每次追加重复扫描全文。");
    } else if (characters >= 0) {
        result.characterText = QStringLiteral("字符数: %1").arg(QLocale().toString(characters));
        result.characterDescription = QStringLiteral("当前文本的 Unicode 字符总数。");
    } else if (tab.largeDocumentRuntimeState()
                   == EditorTab::LargeDocumentRuntimeState::ReadOnlyAnalysis
               && tab.windowedReadOnlyProvider()
               && tab.windowedReadOnlyProvider()->characterCountPending()) {
        result.characterText = QStringLiteral("字符数: 统计中…");
        result.characterDescription = QStringLiteral("正在后台统计只读文本的字符数，不影响浏览。");
    } else {
        result.characterText = QStringLiteral("字符数: 暂不可用");
        result.characterDescription = QStringLiteral("字符总数暂不可用；可查看后台任务详情，检查文件后重试。");
    }
    return result;
}
