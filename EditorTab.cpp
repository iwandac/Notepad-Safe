#include "EditorTab.h"

#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFont>
#include <QColor>
#include <QRegularExpression>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProgressDialog>
#include <QCoreApplication>

#include <Qsci/qscilexer.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerxml.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexeryaml.h>
#include <Qsci/qscilexermarkdown.h>

EditorTab::EditorTab(QWidget *parent)
    : QWidget(parent)
{
    m_editor = new QsciScintilla(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);

    setupEditor();
}

void EditorTab::setupEditor()
{
    // 等宽字体
    QFont font("Consolas", 10);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    m_editor->setFont(font);

    // 行号
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->setMarginWidth(0, "99999");

    // 行号边距颜色
    m_editor->setMarginsBackgroundColor(QColor(240, 240, 240));
    m_editor->setMarginsForegroundColor(QColor(120, 120, 120));

    // 书签 marker（边距 1）
    m_bookmarkMarker = m_editor->markerDefine(QsciScintilla::Circle);
    m_editor->setMarginType(1, QsciScintilla::SymbolMargin);
    m_editor->setMarginWidth(1, 16);
    m_editor->setMarginMarkerMask(1, 1 << m_bookmarkMarker);
    m_editor->setMarkerBackgroundColor(QColor(255, 180, 0), m_bookmarkMarker);
    m_editor->setMarkerForegroundColor(QColor(255, 255, 255), m_bookmarkMarker);

    // 缩进
    m_editor->setAutoIndent(true);
    m_editor->setIndentationsUseTabs(false);
    m_editor->setTabWidth(4);
    m_editor->setIndentationGuides(true);

    // 当前行高亮
    m_editor->setCaretLineVisible(true);
    m_editor->setCaretLineBackgroundColor(QColor(245, 245, 220));
    m_editor->setCaretWidth(2);

    // 长行标尺（默认 80 列）
    applyEdgeSettings();

    // 横向滚动条按需显示：Scintilla 默认 scrollWidth=2000px，即使空文档也会显示滚动条。
    // setScrollWidthTracking(true) 让 scrollWidth 跟踪实际最长行，配合初始 scrollWidth=1，
    // 只有当某行实际超出视口宽度时才出现横向滚动条。
    // 注意：QScintilla 的 ReconfigureScrollBars() 会覆盖 setHorizontalScrollBarPolicy，
    //       所以必须用 setScrollWidth/setScrollWidthTracking 而非 Qt 策略。
    m_editor->setScrollWidth(1);
    m_editor->setScrollWidthTracking(true);

    // 选中色
    m_editor->setSelectionBackgroundColor(QColor(51, 153, 255));
    m_editor->setSelectionForegroundColor(QColor(255, 255, 255));

    // 折叠（代码折叠开关）
    m_editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);

    // 括号匹配
    m_editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);

    // 自动补全
    m_editor->setAutoCompletionSource(QsciScintilla::AcsAll);
    m_editor->setAutoCompletionThreshold(3);

    // 信号
    connect(m_editor, &QsciScintilla::SCN_MODIFYATTEMPTRO, this, [this]() {
        emit modificationChanged(true);
    });
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, &EditorTab::cursorPositionChanged);
    connect(m_editor, &QsciScintilla::modificationChanged,
            this, [this](bool m) {
        emit modificationChanged(m);
        emit titleChanged(tabTitle());
    });
}

bool EditorTab::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    m_filePath = path;
    m_autosavePath.clear();  // 加载正式文件，清除 autosave 关联

    // 大文件分块加载：避免 setText() 一次性重建整个文档导致 UI 长时间卡死
    // （Notepad++ / Scintilla 的 ILoader 也是增量加载，此处用 SCI_APPENDTEXT 分块追加）
    const int largeThreshold = 2 * 1024 * 1024; // 2MB 以上走分块路径
    if (data.size() < largeThreshold) {
        // 重置 scrollWidth：tracking 模式只增不减，切换文件时需手动归零
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        // UTF-8 文件可直接使用原始字节，跳过 QString 中间层以节省内存和时间
        // 非 UTF-8 文件需经 QString 转换为 UTF-8 字节
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3); // 跳过 3 字节 BOM
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(false);

    // 根据后缀猜测语法
    Syntax s = guessSyntaxFromExtension(path);
    setSyntax(s);

    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::loadAutosave(const QString &autosavePath)
{
    QFile file(autosavePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    // 关键：不设置 m_filePath，保持未命名状态，对用户透明
    m_autosavePath = autosavePath;

    // 设置文本（但不清除 modified 标记，让用户知道内容是上次未保存的）
    const int largeThreshold = 2 * 1024 * 1024;
    if (data.size() < largeThreshold) {
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3);
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(true);

    // autosave 不猜语法，保持 Plain 或已设置的语法

    emit encodingChanged(m_encoding);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::saveFile()
{
    if (isUntitled())
        return false;
    return saveAsFile(m_filePath);
}

bool EditorTab::saveAsFile(const QString &path)
{
    QByteArray data = encodeText(m_editor->text(), m_encoding);

    // 原子写入：先写 .tmp，再 rename
    QString tmpPath = path + ".tmp";
    QSaveFile saveFile(tmpPath);
    saveFile.setDirectWriteFallback(false);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        return false;
    }
    qint64 written = saveFile.write(data);
    if (written != data.size()) {
        return false;
    }
    if (!saveFile.commit()) {
        return false;
    }

    // rename
    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        return false;
    }

    m_filePath = path;
    m_editor->setModified(false);

    // 保存成功：自动清理 autosave 临时文件（自清理机制）
    clearAutosaveFile();
    m_autosavePath.clear();

    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::isModified() const
{
    return m_editor->isModified();
}

void EditorTab::setModified(bool m)
{
    m_editor->setModified(m);
}

bool EditorTab::isUntitled() const
{
    return m_filePath.isEmpty();
}

QString EditorTab::filePath() const
{
    return m_filePath;
}

void EditorTab::setFilePath(const QString &path)
{
    m_filePath = path;
    emit titleChanged(tabTitle());
}

QString EditorTab::fileName() const
{
    if (m_filePath.isEmpty())
        return QStringLiteral("未命名");
    return QFileInfo(m_filePath).fileName();
}

QString EditorTab::tabTitle() const
{
    QString title = fileName();
    if (m_editor->isModified())
        title += QStringLiteral(" ●");
    return title;
}

TextEncoding EditorTab::encoding() const
{
    return m_encoding;
}

void EditorTab::setEncoding(TextEncoding enc)
{
    if (m_encoding == enc)
        return;
    m_encoding = enc;
    emit encodingChanged(m_encoding);
}

Syntax EditorTab::syntax() const
{
    return m_syntax;
}

void EditorTab::setSyntax(Syntax syntax)
{
    if (m_syntax == syntax)
        return;
    m_syntax = syntax;
    applySyntax(syntax);
    emit syntaxChanged(m_syntax);
}

Syntax EditorTab::guessSyntaxFromExtension(const QString &fileName)
{
    QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "hh" || ext == "hxx" || ext == "c" || ext == "inl")
        return Syntax::Cpp;
    if (ext == "py" || ext == "pyw")
        return Syntax::Python;
    if (ext == "json")
        return Syntax::Json;
    if (ext == "html" || ext == "htm" || ext == "xhtml")
        return Syntax::Html;
    if (ext == "xml" || ext == "svg" || ext == "ui" || ext == "pro")
        return Syntax::Xml;
    if (ext == "sql")
        return Syntax::Sql;
    if (ext == "yaml" || ext == "yml")
        return Syntax::Yaml;
    if (ext == "md" || ext == "markdown")
        return Syntax::Markdown;
    return Syntax::Plain;
}

QsciLexer *EditorTab::createLexer(Syntax syntax) const
{
    switch (syntax) {
    case Syntax::Cpp:      return new QsciLexerCPP(m_editor);
    case Syntax::Python:   return new QsciLexerPython(m_editor);
    case Syntax::Json:     return new QsciLexerJSON(m_editor);
    case Syntax::Html:     return new QsciLexerHTML(m_editor);
    case Syntax::Xml:      return new QsciLexerXML(m_editor);
    case Syntax::Sql:      return new QsciLexerSQL(m_editor);
    case Syntax::Yaml:     return new QsciLexerYAML(m_editor);
    case Syntax::Markdown: return new QsciLexerMarkdown(m_editor);
    case Syntax::Plain:    return nullptr;
    }
    return nullptr;
}

void EditorTab::applySyntax(Syntax syntax)
{
    QsciLexer *lexer = createLexer(syntax);
    if (lexer) {
        QFont font = m_editor->font();
        lexer->setDefaultFont(font);
    }
    m_editor->setLexer(lexer);
    // setLexer 接管 ownership，旧 lexer 自动删除

    // 大文件启用 idle styling：只对可见区域着色，剩余部分在空闲时逐步完成，
    // 避免 setLexer 后一次性对全文档做词法分析导致 UI 卡顿
    // （Notepad++ 对大文件也采用类似策略）
    const int docLen = m_editor->length();
    if (docLen > 5 * 1024 * 1024) { // > 5MB
        // SC_IDLESTYLING_TOVISIBLE=1: 只着色到可见区域，其余在 idle 时完成
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 1UL);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 0UL);
    }
}

// ============ 行操作 ============

void EditorTab::duplicateLine()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    QString lineText = m_editor->text(line);
    m_editor->insertAt(lineText + "\n", line, 0);
    m_editor->setCursorPosition(line + 1, index);
}

void EditorTab::deleteLine()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    m_editor->setSelection(line, 0, line + 1, 0);
    m_editor->removeSelectedText();
}

void EditorTab::moveLineUp()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    if (line == 0)
        return;
    QString current = m_editor->text(line);
    QString above = m_editor->text(line - 1);
    // 替换上一行
    m_editor->setSelection(line - 1, 0, line, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(current + "\n", line - 1, 0);
    m_editor->setSelection(line, 0, line + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(above + "\n", line, 0);
    m_editor->setCursorPosition(line - 1, index);
}

void EditorTab::moveLineDown()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int lines = m_editor->lines();
    if (line >= lines - 1)
        return;
    QString current = m_editor->text(line);
    QString below = m_editor->text(line + 1);
    m_editor->setSelection(line + 1, 0, line + 2, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(below + "\n", line, 0);
    m_editor->setSelection(line + 1, 0, line + 2, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(current + "\n", line + 1, 0);
    m_editor->setCursorPosition(line + 1, index);
}

void EditorTab::sortLinesAsc()
{
    int lineFrom, indexFrom, lineTo, indexTo;
    m_editor->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0 || lineTo <= lineFrom)
        return;
    QStringList selected;
    for (int i = lineFrom; i <= lineTo; ++i)
        selected << m_editor->text(i);
    selected.sort();
    QString replacement = selected.join("\n") + "\n";
    m_editor->setSelection(lineFrom, 0, lineTo + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(replacement, lineFrom, 0);
}

void EditorTab::sortLinesDesc()
{
    int lineFrom, indexFrom, lineTo, indexTo;
    m_editor->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0 || lineTo <= lineFrom)
        return;
    QStringList selected;
    for (int i = lineFrom; i <= lineTo; ++i)
        selected << m_editor->text(i);
    std::sort(selected.begin(), selected.end(), std::greater<QString>());
    QString replacement = selected.join("\n") + "\n";
    m_editor->setSelection(lineFrom, 0, lineTo + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(replacement, lineFrom, 0);
}

// ============ 书签 ============

void EditorTab::toggleBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    unsigned mask = m_editor->markersAtLine(line);
    if (mask & (1 << m_bookmarkMarker)) {
        m_editor->markerDelete(line, m_bookmarkMarker);
    } else {
        m_editor->markerAdd(line, m_bookmarkMarker);
    }
}

void EditorTab::nextBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int next = m_editor->markerFindNext(line + 1, 1 << m_bookmarkMarker);
    if (next < 0) {
        // wrap
        next = m_editor->markerFindNext(0, 1 << m_bookmarkMarker);
    }
    if (next >= 0) {
        m_editor->setCursorPosition(next, 0);
        m_editor->ensureLineVisible(next);
    }
}

void EditorTab::prevBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int prev = m_editor->markerFindPrevious(line - 1, 1 << m_bookmarkMarker);
    if (prev < 0) {
        int lines = m_editor->lines();
        prev = m_editor->markerFindPrevious(lines - 1, 1 << m_bookmarkMarker);
    }
    if (prev >= 0) {
        m_editor->setCursorPosition(prev, 0);
        m_editor->ensureLineVisible(prev);
    }
}

void EditorTab::clearBookmarks()
{
    m_editor->markerDeleteAll(m_bookmarkMarker);
}

void EditorTab::clearMarkers()
{
    m_editor->markerDeleteAll(-1);
}

// ============ 搜索 ============

static QRegularExpression buildPattern(const QString &pattern, bool regex,
                                       bool caseSensitive, bool wholeWord)
{
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;

    QString pat;
    if (regex) {
        pat = pattern;
    } else {
        pat = QRegularExpression::escape(pattern);
    }
    if (wholeWord) {
        pat = QStringLiteral("(?:^|\\W)") + pat + QStringLiteral("(?:$|\\W)");
    }
    QRegularExpression re(pat, options);
    return re;
}

bool EditorTab::findNext(const QString &pattern, bool regex, bool caseSensitive,
                         bool wholeWord, bool forward, bool wrap)
{
    if (pattern.isEmpty())
        return false;
    // 使用 QsciScintilla 内置搜索（会高亮并跳转）
    bool re = regex;
    bool cs = caseSensitive;
    bool wo = wholeWord;
    bool found = false;
    if (forward) {
        int line, index;
        m_editor->getCursorPosition(&line, &index);
        found = m_editor->findFirst(pattern, re, cs, wo, wrap, true, line, index, true);
    } else {
        // findFirst forward=false 从当前位置向前搜索
        int line, index;
        m_editor->getCursorPosition(&line, &index);
        found = m_editor->findFirst(pattern, re, cs, wo, wrap, false, line, index, true);
    }
    return found;
}

void EditorTab::replaceCurrent(const QString &replaceText)
{
    m_editor->replace(replaceText);
}

int EditorTab::replaceAll(const QString &find, const QString &replace, bool regex,
                          bool caseSensitive, bool wholeWord)
{
    if (find.isEmpty())
        return 0;

    // 先回到文档开头
    m_editor->setCursorPosition(0, 0);

    int count = 0;
    // 使用 QsciScintilla 的 findFirst + replace 循环
    bool found = m_editor->findFirst(find, regex, caseSensitive, wholeWord,
                                     false, true, 0, 0, false);
    while (found) {
        m_editor->replace(replace);
        ++count;
        found = m_editor->findNext();
    }
    return count;
}

QVector<SearchMatch> EditorTab::findAll(const QString &pattern, bool regex,
                                        bool caseSensitive, bool wholeWord)
{
    QVector<SearchMatch> matches;
    if (pattern.isEmpty())
        return matches;

    QRegularExpression re = buildPattern(pattern, regex, caseSensitive, wholeWord);
    if (!re.isValid())
        return matches;

    // 按行搜索
    int lines = m_editor->lines();
    for (int lineNum = 0; lineNum < lines; ++lineNum) {
        QString lineText = m_editor->text(lineNum);
        // 移除末尾换行符
        if (lineText.endsWith('\n'))
            lineText.chop(1);
        if (lineText.endsWith('\r'))
            lineText.chop(1);

        auto it = re.globalMatch(lineText);
        while (it.hasNext()) {
            auto m = it.next();
            SearchMatch match;
            match.line = lineNum;
            match.index = m.capturedStart();
            match.length = m.capturedLength();
            match.text = m.captured(0);
            match.context = lineText;
            matches.append(match);
        }
    }
    return matches;
}

// ============ 光标 ============

void EditorTab::getCursorPosition(int *line, int *index) const
{
    m_editor->getCursorPosition(line, index);
}

void EditorTab::setCursorPosition(int line, int index)
{
    m_editor->setCursorPosition(line, index);
    m_editor->ensureLineVisible(line);
}

// ============ 长行标尺 ============

void EditorTab::applyEdgeSettings()
{
    m_editor->setEdgeColumn(m_edgeColumn);
    if (m_edgeVisible)
        m_editor->setEdgeMode(QsciScintilla::EdgeLine);
    else
        m_editor->setEdgeMode(QsciScintilla::EdgeNone);
}

void EditorTab::setEdgeColumn(int column)
{
    if (column < 1) column = 1;
    if (column > 1000) column = 1000;
    m_edgeColumn = column;
    applyEdgeSettings();
}

void EditorTab::setEdgeVisible(bool visible)
{
    m_edgeVisible = visible;
    applyEdgeSettings();
}

bool EditorTab::isEdgeVisible() const
{
    return m_edgeVisible;
}

// ============ 编码切换 ============

void EditorTab::reloadWithEncoding(TextEncoding enc)
{
    if (m_encoding == enc)
        return;
    // 重新解释内容：当前 Unicode 文本 → 用旧编码还原字节 → 用新编码解码
    // 但更实用的做法是：如果磁盘上有文件，从磁盘按新编码重读；否则保持文本不变只改编码标记
    if (!isUntitled() && QFileInfo::exists(m_filePath)) {
        // 只有在用户确认后才重载（由上层调用 saveFile 检查），这里简单切换编码标记
        m_encoding = enc;
        emit encodingChanged(m_encoding);
        return;
    }
    // 未命名/内存中：简单切换编码标记（下次保存用新编码）
    m_encoding = enc;
    emit encodingChanged(m_encoding);
}

// ============ autosave 自清理 ============

void EditorTab::clearAutosaveFile() const
{
    if (!m_autosavePath.isEmpty() && QFile::exists(m_autosavePath)) {
        QFile::remove(m_autosavePath);
    }
}

// ============ 大文件分块加载 ============

void EditorTab::loadUtf8Chunked(const QByteArray &utf8)
{
    // 通过 Scintilla 底层 SCI_APPENDTEXT 分块追加文本，
    // 比 setText(整个文本) 快很多倍：setText 会一次性重建行索引、
    // 触发全量重排，而 SCI_APPENDTEXT 是增量追加，Scintilla 内部只做最小化更新。
    //
    // 参考：Notepad++ / Scintilla 的 ILoader 也是增量加载机制，
    //       QScintilla 2.14 未直接暴露 ILoader API，故用 SCI_APPENDTEXT 替代。
    //
    // !! 重要：不能调用 setReadOnly(true) 来防止用户在加载期间编辑 !!
    // 因为 Scintilla 的 Document::InsertString 内部会检查 cb.IsReadOnly()，
    // readOnly 状态下 SCI_APPENDTEXT 会直接返回 0 不插入任何内容，
    // 导致加载完成后编辑框为空（进度条正常显示但内容未写入）。
    // 防止用户干扰加载由 QProgressDialog 的 WindowModal 模态负责。

    // 关闭 undo 收集：加载过程不产生 undo 步骤
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);

    // 清空已有内容并重置 scrollWidth（tracking 只增不减，需手动归零）
    m_editor->setScrollWidth(1);
    m_editor->clear();

    // 加载期间临时关闭换行：WrapWord 模式下每追加一段文本都会触发
    // 整个可见区的换行重算，大文件时极慢；加载完成后恢复
    QsciScintilla::WrapMode savedWrap = m_editor->wrapMode();
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(QsciScintilla::WrapNone);

    const char *raw = utf8.constData();
    const int total = utf8.size();
    const int chunkSize = 512 * 1024; // 512KB per chunk

    QProgressDialog progress(
        QStringLiteral("正在加载大文件 (%1 MB)…").arg(total / (1024 * 1024)),
        QStringLiteral("取消"), 0, total, window());
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    int pos = 0;
    while (pos < total) {
        int end = qMin(pos + chunkSize, total);
        // 回退到完整 UTF-8 字符边界：不截断多字节字符
        while (end < total &&
               (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
            --end;
        // 防御：极端情况下整个 chunk 都是 UTF-8 后续字节，end 会回退到 pos 之前，
        // 此时不再回退，直接使用 chunkSize 范围（可能截断 1 个多字节字符，但避免死循环/负长度）
        if (end <= pos)
            end = qMin(pos + chunkSize, total);

        int len = end - pos;
        m_editor->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                                static_cast<uintptr_t>(len), raw + pos);

        pos = end;
        progress.setValue(pos);
        QCoreApplication::processEvents();
        if (progress.wasCanceled())
            break;
    }

    progress.close();

    // 恢复换行模式
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(savedWrap);

    // 恢复 undo 收集并清空 undo 缓冲（加载步骤不可 undo）
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
}

// ============ 自动换行 ============

void EditorTab::setWordWrap(bool on)
{
    if (on) {
        // WrapWord：在边界（单词分隔处）换行，不修改内容，仅视觉换行
        m_editor->setWrapMode(QsciScintilla::WrapWord);
        // 换行缩进：与原行对齐（Notepad++ 风格）
        m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
        m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    } else {
        // WrapNone：不主动换行，只有遇到 \n 才换行，超出会显示水平滚动条
        m_editor->setWrapMode(QsciScintilla::WrapNone);
    }
}

bool EditorTab::wordWrap() const
{
    return m_editor->wrapMode() != QsciScintilla::WrapNone;
}
