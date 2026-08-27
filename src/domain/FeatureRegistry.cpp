#include "FeatureRegistry.h"

#include <initializer_list>

namespace {

bool containsCommand(const QString &commandId,
                     std::initializer_list<const char *> commands)
{
    for (const char *command : commands) {
        if (commandId == QLatin1String(command))
            return true;
    }
    return false;
}

bool hasWhitelistedSuffix(const QString &commandId, const char *prefix,
                          std::initializer_list<const char *> suffixes)
{
    const QLatin1String prefixText(prefix);
    if (!commandId.startsWith(prefixText))
        return false;
    const QStringView suffix = QStringView(commandId).sliced(prefixText.size());
    for (const char *allowed : suffixes) {
        if (suffix == QLatin1String(allowed))
            return true;
    }
    return false;
}

} // namespace

QList<FeatureDescriptor> FeatureRegistry::features()
{
    return {
        {QStringLiteral("NSP-FILE"), QStringLiteral("文件与生命周期"),
         QStringLiteral("新建、打开、保存、关闭与退出")},
        {QStringLiteral("NSP-EDIT"), QStringLiteral("编辑"),
         QStringLiteral("撤销、剪贴板、行操作、缩进与补全")},
        {QStringLiteral("NSP-SEARCH"), QStringLiteral("搜索与书签"),
         QStringLiteral("查找、替换、结果导航与书签")},
        {QStringLiteral("NSP-LANGUAGE"), QStringLiteral("语言"),
         QStringLiteral("语法呈现")},
        {QStringLiteral("NSP-VIEW"), QStringLiteral("视图"),
         QStringLiteral("外观、分屏、Text/Hex、面板与缩放")},
        {QStringLiteral("NSP-ENCODING"), QStringLiteral("编码"),
         QStringLiteral("检测、显示、转换与行尾")},
        {QStringLiteral("NSP-ACCOUNTS"), QStringLiteral("账号隔离"),
         QStringLiteral("账号、身份、锁定与恢复材料")},
        {QStringLiteral("NSP-TASKS"), QStringLiteral("任务中心"),
         QStringLiteral("异步任务呈现与取消")},
        {QStringLiteral("NSP-HELP"), QStringLiteral("帮助"),
         QStringLiteral("版本与产品说明")},
        {QStringLiteral("NSP-SHELL"), QStringLiteral("应用 Shell"),
         QStringLiteral("菜单、启动、会话与工作区")},
    };
}

QString FeatureRegistry::featureIdForCommand(const QString &commandId)
{
    if (containsCommand(commandId, {
            "file.new", "file.open", "file.recent", "file.reload",
            "file.save", "file.saveAs", "file.close", "file.closeAll",
            "file.exit"}))
        return QStringLiteral("NSP-FILE");
    if (containsCommand(commandId, {
            "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste",
            "edit.selectAll", "edit.hexInsertMode", "edit.line.duplicate",
            "edit.line.delete", "edit.line.moveUp", "edit.line.moveDown",
            "edit.line.transpose", "edit.multiSelect.next", "edit.multiSelect.all",
            "edit.selection.upper", "edit.selection.lower", "edit.lines.join",
            "edit.lines.split", "edit.lines.sortAscending",
            "edit.lines.sortDescending", "edit.indentation",
            "edit.indentation.useTabs", "edit.indentation.tabWidth",
            "edit.indentation.backspaceUnindents", "edit.completion.words"})
        || hasWhitelistedSuffix(commandId, "edit.indentation.tabWidth.",
                                {"2", "4", "8"}))
        return QStringLiteral("NSP-EDIT");
    if (containsCommand(commandId, {
            "search.find", "search.replace", "search.next", "search.previous",
            "search.brace.match", "search.brace.select", "search.bookmarks",
            "bookmark.toggle", "bookmark.next", "bookmark.previous",
            "bookmark.clear"}))
        return QStringLiteral("NSP-SEARCH");
    if (hasWhitelistedSuffix(commandId, "language.", {
            "plain", "cpp", "python", "json", "html", "xml", "sql",
            "yaml", "markdown", "javascript", "java", "csharp", "css",
            "bash", "batch", "cmake", "makefile", "lua", "ruby", "perl",
            "diff", "properties", "asm"}))
        return QStringLiteral("NSP-LANGUAGE");
    if (containsCommand(commandId, {
            "view.appearance", "view.appearance.system", "view.appearance.light",
            "view.appearance.dark", "view.wordWrap", "view.split",
            "view.split.horizontal", "view.split.vertical", "view.symbols",
            "view.symbols.whitespace", "view.symbols.eol", "view.folding",
            "view.folding.toggleCurrent", "view.folding.collapseAll",
            "view.folding.expandAll", "view.presentation",
            "view.presentation.text", "view.presentation.hex",
            "view.presentation.toggle", "view.edge", "view.edge.visible",
            "view.zoom", "view.zoom.in", "view.zoom.out", "view.zoom.reset",
            "view.panels", "view.panel.searchResults", "view.toolbar",
            "view.statusBar"})
        || hasWhitelistedSuffix(commandId, "view.edge.column.",
                                {"72", "80", "100", "120", "150", "200"}))
        return QStringLiteral("NSP-VIEW");
    if (containsCommand(commandId, {"encoding.eol", "encoding.convert"})
        || hasWhitelistedSuffix(commandId, "encoding.eol.", {"crlf", "lf", "cr"})
        || hasWhitelistedSuffix(commandId, "encoding.display.", {
            "gbk", "gb18030", "big5", "shiftjis", "windows1252", "utf8",
            "utf8bom", "utf16le", "utf16be"})
        || hasWhitelistedSuffix(commandId, "encoding.convert.", {
            "gbk", "gb18030", "big5", "shiftjis", "windows1252", "utf8",
            "utf8bom", "utf16le", "utf16be"}))
        return QStringLiteral("NSP-ENCODING");
    if (commandId == QLatin1String("tools.accounts")
        || commandId == QLatin1String("tools.securityCenter")
        || commandId == QLatin1String("tools.lockNow"))
        return QStringLiteral("NSP-ACCOUNTS");
    if (commandId == QLatin1String("tools.tasks"))
        return QStringLiteral("NSP-TASKS");
    if (commandId == QLatin1String("help.about"))
        return QStringLiteral("NSP-HELP");
    if (containsCommand(commandId, {
            "menu.file", "menu.edit", "menu.search", "menu.language",
            "menu.view", "menu.encoding", "menu.tools", "menu.help",
            "tabContext.save", "tabContext.saveAs", "tabContext.close",
            "tabContext.closeOthers", "tabContext.closeAll",
            "tabContext.copyPath", "tabContext.reveal",
            "tabContext.presentText", "tabContext.presentHex"}))
        return QStringLiteral("NSP-SHELL");
    return {};
}

bool FeatureRegistry::isKnownFeature(const QString &featureId)
{
    for (const auto &feature : features()) {
        if (feature.id == featureId)
            return true;
    }
    return false;
}
