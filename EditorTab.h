#pragma once

#include <QWidget>
#include <Qsci/qsciscintilla.h>
#include "TextEncoding.h"

class QsciLexer;

// 语法类型
enum class Syntax {
    Plain,
    Cpp,
    Python,
    Json,
    Html,
    Xml,
    Sql,
    Yaml,
    Markdown,
};

// 搜索匹配结果
struct SearchMatch {
    int line;       // 0-based 行号
    int index;      // 0-based 列号
    int length;     // 匹配长度
    QString text;   // 匹配文本
    QString context; // 所在行内容
};

class EditorTab : public QWidget {
    Q_OBJECT
public:
    explicit EditorTab(QWidget *parent = nullptr);

    // 文件操作
    bool loadFile(const QString &path);
    bool loadAutosave(const QString &autosavePath);  // 仅加载内容，不设置 m_filePath（对用户透明）
    bool saveFile();
    bool saveAsFile(const QString &path);
    bool isModified() const;
    void setModified(bool m);
    bool isUntitled() const;
    QString filePath() const;
    void setFilePath(const QString &path);
    QString fileName() const;
    QString tabTitle() const;
    TextEncoding encoding() const;
    void setEncoding(TextEncoding enc);
    Syntax syntax() const;

    // 编辑器访问
    QsciScintilla *editor() const { return m_editor; }

    // 语法
    void setSyntax(Syntax syntax);
    static Syntax guessSyntaxFromExtension(const QString &fileName);

    // 行操作
    void duplicateLine();
    void deleteLine();
    void moveLineUp();
    void moveLineDown();
    void sortLinesAsc();
    void sortLinesDesc();

    // 书签
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    void clearBookmarks();

    // 搜索（供 FindReplaceDialog 调用）
    // findNext 返回是否找到，并跳转光标
    bool findNext(const QString &pattern, bool regex, bool caseSensitive,
                  bool wholeWord, bool forward, bool wrap);
    // replace 当前匹配（需先 findNext）
    void replaceCurrent(const QString &replaceText);
    // replaceAll 返回替换次数
    int replaceAll(const QString &find, const QString &replace, bool regex,
                   bool caseSensitive, bool wholeWord);
    // findAll 收集所有匹配（不跳转光标），供 SearchResultsDock
    QVector<SearchMatch> findAll(const QString &pattern, bool regex,
                                 bool caseSensitive, bool wholeWord);

    // 光标位置
    void getCursorPosition(int *line, int *index) const;
    void setCursorPosition(int line, int index);

    // 长行标尺
    void setEdgeColumn(int column);
    void setEdgeVisible(bool visible);
    bool isEdgeVisible() const;

    // 自动换行
    void setWordWrap(bool on);
    bool wordWrap() const;

    // 编码重新加载（保持内容不变，仅改变编码解释方式）
    void reloadWithEncoding(TextEncoding enc);

    // 内部 autosave 路径（用于会话恢复时对用户透明）
    QString autosavePath() const { return m_autosavePath; }
    void setAutosavePath(const QString &path) { m_autosavePath = path; }
    void clearAutosaveFile() const;

signals:
    void modificationChanged(bool modified);
    void cursorPositionChanged(int line, int index);
    void encodingChanged(TextEncoding encoding);
    void syntaxChanged(Syntax syntax);
    void titleChanged(const QString &title);

private:
    QsciScintilla *m_editor;
    QString m_filePath;       // 真实文件路径（空=未命名）
    QString m_autosavePath;   // 内部 autosave 临时路径（对用户透明）
    TextEncoding m_encoding = TextEncoding::Utf8;
    Syntax m_syntax = Syntax::Plain;
    int m_bookmarkMarker = -1;
    bool m_edgeVisible = true;
    int m_edgeColumn = 80;

    void setupEditor();
    void applySyntax(Syntax syntax);
    QsciLexer *createLexer(Syntax syntax) const;
    void clearMarkers();
    void applyEdgeSettings();

    // 大文件分块加载：通过 SCI_APPENDTEXT 分块追加 UTF-8 字节，
    // 避免 setText() 一次性重建整个文档导致 UI 卡死
    void loadUtf8Chunked(const QByteArray &utf8);
};
