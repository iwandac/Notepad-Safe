#include "FeatureRegistry.h"

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
    if (commandId.startsWith(QLatin1String("file.")))
        return QStringLiteral("NSP-FILE");
    if (commandId.startsWith(QLatin1String("edit.")))
        return QStringLiteral("NSP-EDIT");
    if (commandId.startsWith(QLatin1String("search."))
        || commandId.startsWith(QLatin1String("bookmark.")))
        return QStringLiteral("NSP-SEARCH");
    if (commandId.startsWith(QLatin1String("language.")))
        return QStringLiteral("NSP-LANGUAGE");
    if (commandId.startsWith(QLatin1String("view.")))
        return QStringLiteral("NSP-VIEW");
    if (commandId.startsWith(QLatin1String("encoding.")))
        return QStringLiteral("NSP-ENCODING");
    if (commandId == QLatin1String("tools.accounts")
        || commandId == QLatin1String("tools.securityCenter")
        || commandId == QLatin1String("tools.lockNow"))
        return QStringLiteral("NSP-ACCOUNTS");
    if (commandId == QLatin1String("tools.tasks"))
        return QStringLiteral("NSP-TASKS");
    if (commandId.startsWith(QLatin1String("help.")))
        return QStringLiteral("NSP-HELP");
    if (commandId.startsWith(QLatin1String("menu."))
        || commandId.startsWith(QLatin1String("tabContext.")))
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

