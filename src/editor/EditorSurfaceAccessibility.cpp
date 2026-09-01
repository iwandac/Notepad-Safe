#include "EditorSurfaceAccessibility.h"

#include "VirtualFileView.h"

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QWidget>

#include <Qsci/qsciscintilla.h>

void configurePrimaryEditorAccessibility(QsciScintilla *editor)
{
    if (!editor)
        return;
    editor->setObjectName(QStringLiteral("primaryEditorSurface"));
    editor->setAccessibleName(QStringLiteral("文本编辑区"));
    editor->setAccessibleDescription(QStringLiteral("文档的主要文本编辑表面"));
}

void configureEditorPresentationAccessibility(QsciScintilla *editor, bool hex)
{
    if (!editor)
        return;
    editor->setAccessibleName(hex ? QStringLiteral("十六进制字节编辑区")
                                  : QStringLiteral("文本编辑区"));
    editor->setAccessibleDescription(
        hex ? QStringLiteral("以两位十六进制数编辑原始字节")
            : QStringLiteral("按当前编码显示的文本内容"));
}

void configureHexCharacterAccessibility(QPlainTextEdit *ascii, bool readOnly)
{
    if (!ascii)
        return;
    ascii->setAccessibleName(QStringLiteral("编码字符预览"));
    ascii->setAccessibleDescription(
        readOnly ? QStringLiteral("与十六进制字节同步的只读字符表面")
                 : QStringLiteral("与十六进制字节同步的字符编辑表面"));
    ascii->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { border: 0; border-left: 1px solid palette(mid);"
        " padding-left: 8px; }"));
}

void configureByteProviderAccessibility(VirtualFileView *provider)
{
    if (!provider)
        return;
    provider->setObjectName(QStringLiteral("byteDocumentProvider"));
    provider->setAccessibleName(QStringLiteral("字节文档视图"));
    provider->setAccessibleDescription(QStringLiteral("以全局字节偏移浏览文档"));
    QScrollBar *vertical = provider->verticalScrollBar();
    vertical->setObjectName(QStringLiteral("globalByteVerticalScrollBar"));
    vertical->setAccessibleName(QStringLiteral("文档全局垂直位置"));
    vertical->setAccessibleDescription(QStringLiteral("在整个字节文档中上下移动"));
    vertical->setFocusPolicy(Qt::StrongFocus);
    QScrollBar *horizontal = provider->horizontalScrollBar();
    horizontal->setObjectName(QStringLiteral("globalByteHorizontalScrollBar"));
    horizontal->setAccessibleName(QStringLiteral("文档水平位置"));
    horizontal->setAccessibleDescription(QStringLiteral("在文档行内左右移动"));
}

void configureHexTabOrder(QWidget *hex, QWidget *ascii, QWidget *globalScroll)
{
    if (!hex || !ascii || !globalScroll)
        return;
    QWidget::setTabOrder(hex, ascii);
    QWidget::setTabOrder(ascii, globalScroll);
}

void configureEditableHexOverlayAccessibility(
    QsciScintilla *hex, QPlainTextEdit *ascii, QScrollBar *globalScroll)
{
    configureHexCharacterAccessibility(ascii, false);
    configureHexTabOrder(hex, ascii, globalScroll);
}
