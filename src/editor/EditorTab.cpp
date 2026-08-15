#include "EditorTab.h"
#include "VirtualFileView.h"
#include "FileTierPolicy.h"
#include "SearchResultsModel.h"
#include "AsyncTaskManager.h"

#include <QVBoxLayout>
#include <QStackedWidget>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFont>
#include <QColor>
#include <QRegularExpression>
#include <QMimeDatabase>
#include <QStringConverter>
#include <QStringDecoder>
#include <QMimeType>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QStatusBar>
#include <limits>
#include <atomic>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

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
    m_stack  = new QStackedWidget(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_editor);   // index 0 = L1 (QsciScintilla)

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    setupEditor();
}

EditorTab::~EditorTab() {
    // ⚠️  注意：此处是 EditorTab 析构体内部！虽然 EditorTab 刚进入析构、成员还完整，
    //     但 QObject 事件循环可能已派发指向本对象/子对象的 QueuedConnection。
    //     任何 processEvents 调用都可能触发半析构对象的 method invoke → 0xC0000005。
    //
    //  正确的关闭路径是 MainWindow::closeEvent() 在 Step 2.5 中显式调
    //  shutdownL2BeforeClose()（allowProcessEvents=true，MainWindow 完整，UI 事件循环仍活跃）。
    //  那里会完成 cancel LineIndex build task + 轮询终态 + unmap mmap。
    //
    //  此处析构体里仅做 dtor-safe 兜底：如果 MainWindow 路径没执行到（如异常析构），
    //  手动调 closeFile(false)—— 禁止 processEvents，保守等待 500ms 让 worker 自退出。
    bridgeReparentEditorToStack();
    if (m_l2View && m_l2View->isOpen()) {
        m_l2View->closeFile(/*allowProcessEvents=*/false);
    }
}

void EditorTab::shutdownL2BeforeClose() {
    if (Q_LIKELY(!m_l2Mode || !m_l2View)) return;
    if (!m_l2View->isOpen()) {
        // 虽然没 open，但 Scintilla 可能 reparent 到了 viewport；先回到 stack 避免下一次
        // loadFile L1 找不到 editor 在 stack page 0
        bridgeReparentEditorToStack();
        return;
    }

    // MainWindow 仍活跃：先 flush 最近窗口修改到 patches（不做就可能丢最后编辑），
    // 再 closeFile，再 reparent editor 回 stack。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        qWarning("[EditorTab::shutdownL2BeforeClose] flush overlay→patches 失败（不阻塞关闭，"
                 "可能丢失最近窗口编辑）：%s", qUtf8Printable(flushErr));
    }

    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 mode: closing mmap + "
           "canceling LineIndex build (allow processEvents because MainWindow is still alive).",
           qUtf8Printable(tabTitle()));
    m_l2View->closeFile(/*allowProcessEvents=*/true);
    bridgeReparentEditorToStack();
    m_l2Mode = false;
    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 shutdown done.",
           qUtf8Printable(tabTitle()));
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
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(path).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile] ⚠️  大文件(%lld MB)直接走了 L1 loadFile！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openTabAutoMode → openLargeFileL2 mmap 秒开。"
                  " path='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(path));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadFile] readAll 小文件 %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

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

    // 成功加载文件：切回 L1 显示，关闭 L2 模式（若之前曾打开 L2）。
    // L2 会把 Scintilla 临时 reparent 到 VirtualFileView 的 viewport；reparent 时
    // QStackedWidget 会自动移除该页，因此不能假设 L1 永远仍是 index 0。
    bridgeReparentEditorToStack();
    m_l2Mode = false;
    emit l2ModeStatusChanged();
    if (m_stack) m_stack->setCurrentWidget(m_editor);

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
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(autosavePath).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave] ⚠️  大 autosave(%lld MB)直接走了 L1 loadAutosave！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openAutosaveAutoMode → ensureL2View+openFile mmap 秒开。"
                  " autosavePath='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(autosavePath));
    }
    QFile file(autosavePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadAutosave] readAll 小 autosave %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

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

bool EditorTab::saveFile(QString *errMsg)
{
    if (isUntitled()) {
        if (errMsg) *errMsg = QObject::tr("未命名文件，请先指定保存路径。");
        return false;
    }
    if (isL2Mode())
        return saveL2As(m_filePath, errMsg);
    return saveAsFile(m_filePath, errMsg);
}

bool EditorTab::saveAsFile(const QString &path, QString *errMsg)
{
    if (isL2Mode())
        return saveL2As(path, errMsg);

    QByteArray data = encodeText(m_editor->text(), m_encoding);

    // 原子写入：先写 .tmp，再 rename
    QString tmpPath = path + ".tmp";
    QSaveFile saveFile(tmpPath);
    saveFile.setDirectWriteFallback(false);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        if (errMsg) *errMsg = QObject::tr("无法打开临时文件进行写入：%1").arg(tmpPath);
        return false;
    }
    qint64 written = saveFile.write(data);
    if (written != data.size()) {
        if (errMsg) *errMsg = QObject::tr("写入临时文件不完整：%1/%2 字节").arg(written).arg(data.size());
        return false;
    }
    if (!saveFile.commit()) {
        if (errMsg) *errMsg = QObject::tr("原子提交临时文件失败：%1").arg(saveFile.errorString());
        return false;
    }

    // rename
    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        if (errMsg) *errMsg = QObject::tr("重命名临时文件到目标路径失败：%1").arg(path);
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
    if (isL2Mode())
        return m_l2SaveNeeded;
    return m_editor->isModified();
}

void EditorTab::setModified(bool m)
{
    if (isL2Mode()) {
        if (m_l2SaveNeeded != m) {
            m_l2SaveNeeded = m;
            emit modificationChanged(m);
            emit titleChanged(tabTitle());
        }
        return;
    }
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
    if (isModified())
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
    if (isL2Mode()) {
        if (line)  *line  = m_lastL2Line;
        if (index) *index = m_lastL2Col;
        return;
    }
    m_editor->getCursorPosition(line, index);
}

void EditorTab::setCursorPosition(int line, int index)
{
    if (isL2Mode() && m_l2View) {
        m_l2View->gotoLine(line);
        // L2 没有列跳转（字节偏移跳转用 gotoByteOffset），列置 0
        if (m_lastL2Col != 0) {
            m_lastL2Col = 0;
            emit cursorPositionChanged(m_lastL2Line, 0);
        }
        return;
    }
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
    // 防干扰改为：每 chunk 间 processEvents(ExcludeUserInputEvents) 仅重绘不接受输入；
    // 且 L1 阈值仅 ~128MB（秒级完成），故不再弹出 WindowModal QProgressDialog。

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

    // 状态栏非模态进度（若当前窗口为 QMainWindow）；无则静默。
    auto *mw = qobject_cast<QMainWindow*>(this->window());
    const int reportEvery = qMax(1, (2 * 1024 * 1024) / qMax(1, chunkSize)); // ~每 2MB 报一次
    int chunkNo = 0;

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
        ++chunkNo;
        if ((chunkNo % reportEvery) == 0 || pos >= total) {
            if (mw && mw->statusBar() && total > 0) {
                int pct = int((qint64(pos) * 100) / qint64(total));
                mw->statusBar()->showMessage(
                    QObject::tr("正在加载文件… %1% (%2 / %3 MB)")
                        .arg(pct)
                        .arg(pos / (1024 * 1024))
                        .arg(total / (1024 * 1024)),
                    0);
            }
        }
        // 仅允许重绘（不接受鼠标/键盘输入），避免模态且防止用户编辑中状态不一致
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mw && mw->statusBar()) mw->statusBar()->clearMessage();

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
    // L2 模式：委托 VirtualFileView
    if (isL2Mode() && m_l2View) {
        m_l2View->setWordWrap(on);
        return;
    }
    // L1 模式：QsciScintilla
    if (on) {
        m_editor->setWrapMode(QsciScintilla::WrapWord);
        m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
        m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    } else {
        m_editor->setWrapMode(QsciScintilla::WrapNone);
    }
}

bool EditorTab::wordWrap() const
{
    if (isL2Mode() && m_l2View)
        return m_l2View->wordWrap();
    return m_editor->wrapMode() != QsciScintilla::WrapNone;
}

void EditorTab::setDisplayEncoding(TextEncoding enc)
{
    if (isL2Mode() && m_l2View) {
        // L2：仅改变显示解码方式，不修改文件内容
        m_l2View->setDisplayEncoding(enc);
    } else {
        // L1：重新以指定编码解码文件字节并加载到编辑器
        // 读取原始文件字节
        if (m_filePath.isEmpty()) return;
        QFile f(m_filePath);
        if (!f.open(QIODevice::ReadOnly)) return;
        QByteArray raw = f.readAll();
        f.close();
        // 跳过 BOM
        int skip = 0;
        if (enc == TextEncoding::Utf8Bom && raw.size() >= 3 &&
            (uchar)raw[0] == 0xEF && (uchar)raw[1] == 0xBB && (uchar)raw[2] == 0xBF)
            skip = 3;
        else if ((enc == TextEncoding::Utf16LE || enc == TextEncoding::Utf16BE) &&
                 raw.size() >= 2)
            skip = 2;
        QByteArray body = raw.mid(skip);
        QString text;
        switch (enc) {
        case TextEncoding::Utf8:
        case TextEncoding::Utf8Bom:
            text = QString::fromUtf8(body);
            break;
        case TextEncoding::GBK:
#ifdef Q_OS_WIN
            {   int wlen = MultiByteToWideChar(CP_ACP, 0, body.constData(), body.size(), nullptr, 0);
                if (wlen > 0) {
                    QVector<wchar_t> wstr(wlen);
                    MultiByteToWideChar(CP_ACP, 0, body.constData(), body.size(), wstr.data(), wlen);
                    text = QString::fromWCharArray(wstr.data(), wlen);
                } }
#else
            {   auto dec = QStringDecoder("GB18030");
                text = dec.isValid() ? dec(body) : QString::fromLatin1(body); }
#endif
            break;
        case TextEncoding::Utf16LE:
            text = QString::fromUtf16(reinterpret_cast<const char16_t*>(body.constData()),
                                      body.size() / 2);
            break;
        case TextEncoding::Utf16BE: {
            QByteArray swapped = body;
            for (int i = 0; i + 1 < swapped.size(); i += 2)
                std::swap(swapped[i], swapped[i+1]);
            text = QString::fromUtf16(reinterpret_cast<const char16_t*>(swapped.constData()),
                                      swapped.size() / 2);
            break;
        }
        }
        m_editor->setText(text);
        m_editor->setModified(false);
    }
}

TextEncoding EditorTab::displayEncoding() const
{
    if (isL2Mode() && m_l2View)
        return m_l2View->displayEncoding();
    return m_encoding;
}

// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================
void EditorTab::setViewModeL2(ViewMode mode)
{
    if (m_l2View) m_l2View->setViewMode(mode);
}

ViewMode EditorTab::viewModeL2() const
{
    return m_l2View ? m_l2View->viewMode() : ViewMode::Text;
}

void EditorTab::setDisplayEncodingL2(TextEncoding enc)
{
    if (m_l2View) m_l2View->setDisplayEncoding(enc);
}

TextEncoding EditorTab::displayEncodingL2() const
{
    return m_l2View ? m_l2View->displayEncoding() : TextEncoding::Utf8;
}

bool EditorTab::isL2Active() const
{
    // 不能用 currentIndex()==1：进入 L2 后 Scintilla 会被 reparent 到 VFV 的
    // viewport，QStackedWidget 随即自动移除原 L1 page，VFV 的 index 会从 1 变成 0。
    // 用稳定的页面对象身份判断，避免菜单状态和实际编辑模式永久错位。
    return m_l2Mode && m_stack && m_l2View
        && m_stack->currentWidget() == m_l2View;
}

// ============================================================================
//  L2 路径（VirtualFileView 大文件虚拟视图）—— 独立实现，不影响 L1
// ============================================================================

VirtualFileView *EditorTab::ensureL2View()
{
    if (!m_l2View) {
        m_l2View = new VirtualFileView(this);
        // 加进 QStackedWidget（index 1 = L2）
        if (m_stack && m_stack->indexOf(m_l2View) < 0)
            m_stack->addWidget(m_l2View);

        // 连接 L2 信号：缓存数据 + 转发 EditorTab 的统一/专用信号
        connect(m_l2View, &VirtualFileView::cursorPositionChanged,
                this, [this](int line, int col) {
                    m_lastL2Line = line;
                    m_lastL2Col  = col;
                    // byteOffset 粗略缓存：用 lineIndex→lineToByteOffset（不准确但用于 findNext 起始足够）
                    if (m_l2View && m_l2View->isOpen()) {
                        m_lastL2Byte = m_l2View->lineIndex().lineToByteOffset(line) + col;
                    }
                    // 转发统一 cursorPositionChanged：MainWindow 状态栏更新走同一个
                    emit cursorPositionChanged(line, col);
                    emit l2CursorPositionChanged(line, col);
                });
        connect(m_l2View, &VirtualFileView::fileOpenedChanged,
                this, &EditorTab::l2FileOpenedChanged);
        connect(m_l2View, &VirtualFileView::saveNeededChanged,
                this, [this](bool needed) {
                    m_l2SaveNeeded = needed;
                    emit modificationChanged(needed);
                    emit titleChanged(tabTitle());
                    emit l2SaveNeededChanged(needed);
                });
        connect(m_l2View, &VirtualFileView::undoRedoChanged,
                this, &EditorTab::l2UndoRedoChanged);
        connect(m_l2View, &VirtualFileView::lineBuildProgress,
                this, &EditorTab::l2LineBuildProgress);
        connect(m_l2View, &VirtualFileView::lineBuildFinished,
                this, &EditorTab::l2LineBuildFinished);
    }
    return m_l2View;
}

// ============================================================================
//  L2ScintillaBridge：VFV 不做任何绘制；Scintilla 是唯一 UI。
// ============================================================================

void EditorTab::bridgeReparentEditorToStack()
{
    if (!m_stack) return;
    // 先卸载 VFV viewport 的 eventFilter（避免 reparent 后仍收到无效 Resize）
    if (m_l2View && m_l2View->viewport())
        m_l2View->viewport()->removeEventFilter(this);
    // 如果 Scintilla 被 reparent 到 VFV viewport（L2 overlay），把它移回 m_stack page 0
    // （父对象不同或 parent 仍在 stack 外的 QAbstractScrollArea viewport）
    const bool stacked = (m_stack->indexOf(m_editor) >= 0);
    if (!stacked) {
        // 从当前父（可能 VFV viewport）拆下来，放回 stack page 0
        m_editor->setParent(m_stack);
        if (m_stack->indexOf(m_editor) < 0)
            m_stack->insertWidget(0, m_editor);
    }
    // 确保 stack page 0 仍指向 m_editor（即使重复 insertWidget 会被 move，再设一次 idx=0）
    if (m_stack->indexOf(m_editor) < 0)
        m_stack->insertWidget(0, m_editor);
}

void EditorTab::bridgePopulateScintilla(qint64 s, qint64 e)
{
    if (!m_l2View || !m_l2View->isOpen()) return;
    if (s < 0 || e <= s) return;

    // 桥 2：如果已有有效 baseline 窗口（用户可能做了编辑），先 flush diff 回 patches 再切窗口
    if (m_bridge.windowStart >= 0 && m_bridge.windowLen > 0
        && !(m_bridge.windowStart == s && m_bridge.windowLen == (e - s))) {
        bridgeFlushOverlayToPatches();
    }

    m_bridge.applying = true;
    QByteArray bytes = m_l2View->readByteRange(s, e);
    // 走 SCI_SETTEXT（setText），对于 W_active 上限 8MB，主线程 setText 约 100ms 内完成，
    // 不会触发"UI 线程卡死"；甩滚时 bridge 不频繁触发（VFV debounce 已节流）。
    m_editor->setText(QString::fromUtf8(bytes));
    m_bridge.baseline    = std::move(bytes);
    m_bridge.windowStart = s;
    m_bridge.windowLen   = e - s;
    m_bridge.applying    = false;
}

void EditorTab::bridgeFlushOverlayToPatches(QString* errMsg)
{
    if (!m_l2View || !m_l2View->isOpen()) return;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return;

    QByteArray now = m_editor->text().toUtf8();
    if (now == m_bridge.baseline) return;  // 无修改，快出

    const qint64 s = m_bridge.windowStart;
    const QByteArray& oldBytes = m_bridge.baseline;
    const qint64 oldLen = oldBytes.size();

    // 最小可用 diff：先找到公共前缀 + 公共后缀（O(n)，8MB 规模毫秒级），
    // 只对中间变化区间做一次 replaceBytes，避免 Myers 复杂度爆炸。
    qint64 commonPrefix = 0;
    const qint64 cmpN = qMin<qint64>(oldLen, now.size());
    while (commonPrefix < cmpN && oldBytes[int(commonPrefix)] == now[int(commonPrefix)])
        ++commonPrefix;
    qint64 commonSuffix = 0;
    while (commonSuffix < cmpN - commonPrefix
           && oldBytes[int(oldLen - 1 - commonSuffix)] == now[int(now.size() - 1 - commonSuffix)])
        ++commonSuffix;

    const qint64 patchOldStart = commonPrefix;
    const qint64 patchOldLen   = oldLen - commonPrefix - commonSuffix;
    const qint64 patchNewStart = commonPrefix;
    const qint64 patchNewLen   = now.size() - commonPrefix - commonSuffix;

    QByteArray newData;
    if (patchNewLen > 0)
        newData = now.mid(int(patchNewStart), int(patchNewLen));

    // 16MB 粘贴硬顶：超出阈值时不允许通过窗口内 flush 写入，避免 PatchList 单条 patch 过大。
    // 用户会被要求走"小范围粘贴分批"，后续可升级为 pasteBytes 分块路径（P1 之后）。
    constexpr qint64 HARD_DELTA_BYTES = Q_INT64_C(16) * 1024 * 1024;
    if (qAbs(qint64(newData.size()) - patchOldLen) > HARD_DELTA_BYTES ||
        qint64(newData.size()) > HARD_DELTA_BYTES) {
        if (errMsg) *errMsg = QObject::tr("窗口内单步修改过大（>16MB），请分批操作或缩小窗口后再修改。");
        return;
    }

    const qint64 globByte = s + patchOldStart;
    QString localErr;
    const bool ok = m_l2View->replaceBytes(globByte, qint32(patchOldLen), newData, &localErr);
    if (!ok) {
        if (errMsg) *errMsg = localErr.isEmpty() ? QObject::tr("写入 patch 失败") : localErr;
        return;
    }

    // flush 成功：更新 baseline 为新文本（长度对应 virtualSize）
    // 注意：替换后窗口可能因为前后 patch 变化而长/短，但我们只保存 Scintilla 当前 now 作为
    // baseline，窗口边界以 s~s+virtualChangedLen 为准——下一次切窗口时桥 1 会重新对齐重灌，
    // 所以此处 baseline 按 Scintilla 实际文本即可。
    m_bridge.baseline    = now;
    m_bridge.windowStart = s;
    m_bridge.windowLen   = now.size();
}

void EditorTab::bridgeOnSyncDebounce()
{
    if (!m_l2View || !m_l2View->isOpen() || m_bridge.applying) return;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return;

    // 取 Scintilla 中心位置：
    //   - 有选区：选区中心字节
    //   - 无选区：(FirstVisibleLine + LinesOnScreen/2) → 转字节
    const int lineCount = m_editor->lines();
    if (lineCount <= 0) return;
    int firstLine = m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    int linesOnScreen = m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
    if (linesOnScreen <= 0) linesOnScreen = 1;
    const int mid = qBound(0, firstLine + linesOnScreen / 2, lineCount - 1);
    // SCI_GETLINESELSTARTPOSITION / CURRENTPOS 太粗糙，直接用 mid 行首 + 当前光标列近似：
    const long lineStart = m_editor->positionFromLineIndex(mid, 0);
    const long cursorPos = m_editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    const long centerWinPos = qBound(long(lineStart), cursorPos, long(lineStart));
    const qint64 globCenter = m_bridge.windowStart + qMax<qint64>(0, centerWinPos);

    m_l2View->syncActiveWindowToCursor(globCenter);
}

bool EditorTab::eventFilter(QObject* obj, QEvent* ev)
{
    // 监听 VFV viewport 的 Resize：Scintilla overlay 必须同步跟着 resize，
    // 否则 reparent 后 vp->size() 为 0（stack 刚切，布局未完成）→ Scintilla 0x0 不显示。
    if (ev->type() == QEvent::Resize && m_l2Mode && m_l2View) {
        QWidget* vp = m_l2View->viewport();
        if (obj == vp && m_editor) {
            m_editor->setGeometry(0, 0, vp->width(), vp->height());
        }
    }
    return QWidget::eventFilter(obj, ev);
}

bool EditorTab::openLargeFileL2(const QString &path, QString *errMsg)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (errMsg) *errMsg = QObject::tr("文件不存在：%1").arg(path);
        return false;
    }
    VirtualFileView *v = ensureL2View();
    // 确保 VirtualFileView 已加入 stack（ensureL2View 会 addWidget 到 index=1）
    QString localErr;
    bool ok = v->openFile(path, &localErr, this->window());
    if (!ok) {
        if (errMsg) *errMsg = localErr;
        // 失败：保持 L1 显示（stack 当前仍=0）
        return false;
    }
    // 成功：切到 L2 视图并标记模式
    m_filePath = path;
    m_l2Mode = true;
    m_l2SaveNeeded = false;
    m_lastL2Line = 0;
    m_lastL2Col  = 0;
    m_lastL2Byte = 0;
    // 重要：先切到 VFV 页面，再 emit l2ModeStatusChanged。页面索引会在后续
    // Scintilla reparent 时变化，所以必须按对象选择，不能硬编码 index=1。
    if (m_stack) m_stack->setCurrentWidget(v);

    // ---- VFV 不绘制方案：Scintilla reparent 到 VFV viewport，占满可视区 ----
    //   唯一 UI = Scintilla overlay。VFV viewport 仅作为父容器（scrollbars 仍由 VFV 提供，
    //   用于全局范围虚拟控制 + 3 档节流；VFV paintEvent 已被改成空壳，不画一个像素）。
    {
        QWidget* vp = v->viewport();
        m_editor->setParent(vp);
        m_editor->setGeometry(QRect(QPoint(0, 0), vp->size()));
        m_editor->show();
        m_editor->raise();
        m_editor->setFocus();
        // 安装 eventFilter：VFV viewport resize 时同步 Scintilla geometry
        vp->installEventFilter(this);
    }

    // 桥 3：Scintilla SCN_UPDATEUI → debounce 250ms → syncActiveWindowToCursor（切到 prev/next chunk）
    m_bridge.syncVfvDebounce.setSingleShot(true);
    m_bridge.syncVfvDebounce.setInterval(250);
    // 注意：不直接连 modificationChanged（太频繁），用 SCN_UPDATEUI（仅滚/光标/选区变化时触发）
    //
    // 关键：不要用 Qt::UniqueConnection 接 lambda。Qt6 对 lambda 作为"接收方成员函数指针"
    //       的校验会报错："unique connections require a pointer to member function of
    //       a QObject subclass"，结果是 connect 失败，桥接永不触发，Scintilla 0x0 不显示。
    //       openLargeFileL2 在 Tab 生命周期里只被调用一次（→shutdownL2BeforeClose），
    //       不需要 "Unique" guard 防重连。
    connect(&m_bridge.syncVfvDebounce, &QTimer::timeout,
            this, &EditorTab::bridgeOnSyncDebounce);
    QObject::connect(m_editor, &QsciScintilla::SCN_UPDATEUI, this, [this]() {
        if (Q_UNLIKELY(!m_l2Mode || m_bridge.applying)) return;
        m_bridge.syncVfvDebounce.start();
    });

    // 桥 1：VFV activeWindowChanged（3 档节流 emit）→ 灌 Scintilla 新窗口
    QObject::connect(v, &VirtualFileView::activeWindowChanged,
            this, [this](qint64 byteStart, qint64 byteEnd) {
                if (!m_l2Mode) return;
                bridgePopulateScintilla(byteStart, byteEnd);
            });

    // 首次对齐：以文件 0 字节为中心，计算 W_active 并切到首个窗口 → 灌 Scintilla
    // 注意：openFile 是异步的（mmap 在 worker 线程），此时 isOpen() 可能仍为 false，
    //       syncActiveWindowToCursor 会提前返回。真正的首次灌入由 onMmapReady →
    //       emit activeWindowChanged → bridgePopulateScintilla 完成（上面已 connect）。
    v->syncActiveWindowToCursor(0);
    {
        const qint64 s = v->activeWindowByteStart();
        const qint64 e = v->activeWindowByteEnd();
        if (s >= 0 && e > s)
            bridgePopulateScintilla(s, e);
    }

    // stack 刚切换，viewport 实际尺寸要等事件循环 layout 后才确定。
    // 用 singleShot(0) 在 layout 完成后补一次 resize，确保 Scintilla 不是 0x0。
    QTimer::singleShot(0, this, [this, v]() {
        if (!m_l2Mode || !m_l2View) return;
        QWidget* vp = v->viewport();
        if (vp && m_editor) {
            m_editor->setGeometry(0, 0, vp->width(), vp->height());
        }
    });

    emit l2ModeStatusChanged();

    // 根据后缀猜语法（仅用于信号触发；L2 语法高亮 P1 暂不做，P3 补齐轻量词法器）
    Syntax syn = guessSyntaxFromExtension(fi.fileName());
    if (syn != m_syntax) {
        m_syntax = syn;
        emit syntaxChanged(syn);
    }
    emit titleChanged(tabTitle());
    emit encodingChanged(m_encoding);
    emit cursorPositionChanged(0, 0);
    emit l2FileOpenedChanged(true);
    return true;
}

bool EditorTab::saveL2As(const QString &path, QString *errMsg)
{
    if (!m_l2Mode || !m_l2View) {
        if (errMsg) *errMsg = QObject::tr("当前非 L2 模式");
        return false;
    }
    // P0-5：保存前强制 flush overlay → patches。最近一次编辑没切窗口时就靠这里回写。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        if (errMsg) *errMsg = QObject::tr("保存前回写窗口修改失败：%1").arg(flushErr);
        return false;
    }
    std::atomic<bool> cancel{false};
    QString localErr;

    // 判断是否保存到当前 mmap 映射的同一文件
    const bool sameFile = !m_filePath.isEmpty() &&
        QFileInfo(path).absoluteFilePath() == QFileInfo(m_filePath).absoluteFilePath();

    if (sameFile) {
        // ---- Windows 关键修复：mmap 活跃时 QSaveFile::commit() 无法替换文件 ----
        // 方案：先写到同目录临时文件（mmapBase 仍有效，applyAndSave 可读原文），
        //       再 closeFile 释放 mmap（含取消后台 LineIndex 构建），
        //       然后 rename 临时文件 → 目标路径（失败 fallback copy+remove），最后 openFile 重新映射。
        const QString tempPath = path + QStringLiteral(".notepad-safe-save-tmp");
        QFile::remove(tempPath);

        bool ok = m_l2View->saveAs(tempPath, {}, &cancel, &localErr);
        if (!ok) {
            if (errMsg) *errMsg = localErr;
            QFile::remove(tempPath);
            return false;
        }
        // 释放 mmap（closeFile 内部取消后台构建 + unmap + resetAfterSave）
        m_l2View->closeFile();
        // 原文件已解锁，可以替换
        if (!QFile::rename(tempPath, path)) {
            // Windows fallback：rename 失败（目标文件可能还被 Explorer/杀软锁），改为 copy + remove
            QString renameErr = QObject::tr("重命名失败");
            bool copied = false;
            if (QFile::exists(path)) {
                if (QFile::remove(path))
                    copied = QFile::copy(tempPath, path);
            } else {
                copied = QFile::copy(tempPath, path);
            }
            if (copied) {
                QFile::remove(tempPath);
            } else {
                if (errMsg) {
                    *errMsg = QObject::tr("保存后写入目标文件失败（rename 且 copy 都不行）：%1。%2")
                        .arg(path, renameErr);
                }
                // 尝试恢复：重新打开原文件（原 mmap 的文件还在，只是内容是旧的——用户可再点保存重试）
                QString reopenErr;
                m_l2View->openFile(path, &reopenErr);
                return false;
            }
        }
        // 重新映射已保存的文件
        QString reopenErr;
        ok = m_l2View->openFile(path, &reopenErr);
        if (!ok) {
            if (errMsg) *errMsg = QObject::tr("保存成功但重新打开失败：%1").arg(reopenErr);
            return false;
        }
        m_filePath = path;  // sameFile 路径也同步（openFile 可能没更新 EditorTab::m_filePath，确保 tab 标题/后续状态正确）
    } else {
        // 不同文件：直接保存（目标文件未被 mmap，QSaveFile 可正常 commit）
        bool ok = m_l2View->saveAs(path, {}, &cancel, &localErr);
        if (!ok) {
            if (errMsg) *errMsg = localErr;
            return false;
        }
        m_filePath = path;
    }

    setModified(false);
    clearAutosaveFile();
    m_autosavePath.clear();
    emit titleChanged(tabTitle());
    return true;
}

// ============================================================================
//  会话专用：exportToFile（零副作用写盘）+ openAutosaveAutoMode（L1/L2 恢复分流）
// ============================================================================

bool EditorTab::exportToFile(const QString &destPath) const
{
    // 原子写入：destPath.tmp → rename 到 destPath（与 saveAsFile 同逻辑）
    const QString tmpPath = destPath + ".tmp";
    {
        QSaveFile sf(tmpPath);
        sf.setDirectWriteFallback(false);
        if (!sf.open(QIODevice::WriteOnly))
            return false;

        if (isL2Mode() && m_l2View && m_l2View->isOpen()) {
            // ---- L2：流式 applyAndSave（VirtualFileView::saveAs 不会调 resetAfterSave / 无副作用）----
            //   saveAs 内部用 PatchList::applyAndSave 走 4MB/块 + QSaveFile 原子 commit，
            //   不触碰 patches_ 状态、不 remap、不触发 saveNeededChanged。
            QString err;
            std::atomic<bool> cancel{false};
            // 通过非 const 指针调 saveAs（但我们不修改任何 EditorTab 状态）；
            // saveAs 本身是 non-const VirtualFileView 方法，但语义上对"未保存状态"不做改动。
            auto *nonConstView = const_cast<VirtualFileView *>(m_l2View);
            bool ok = nonConstView->saveAs(tmpPath, {}, &cancel, &err);
            if (!ok) {
                sf.cancelWriting();
                return false;
            }
            // VirtualFileView::saveAs 没走 QSaveFile，它直接写 tmpPath 参数；
            // 但我们把 tmpPath 传给它了。如果 saveAs 成功，它已经把内容写到 tmpPath
            // （applyAndSave 内部用 QSaveFile + commit），所以 sf 上不要 commitWriting（否则会覆盖为空）。
            // 这里 sf 只是占位，cancel 掉它（applyAndSave 已做过原子写）。
            sf.cancelWriting();
            // applyAndSave 的原子写：实际写的是 tmpPath → QSaveFile(tmpPath).commit()
            // 但此时 destPath 还没被替换。所以我们仍需要 tmpPath → destPath 的 rename：
            // applyAndSave 会直接 commit 到 outPath（即 tmpPath），所以 tmpPath 现在是完整文件。
            // 最后再 move tmpPath → destPath。
        } else {
            // ---- L1：用当前编码把 m_editor 文档写出去（不改变 modified/路径）----
            QString txt = m_editor->text();
            QByteArray data = encodeText(txt, m_encoding);
            qint64 written = sf.write(data);
            if (written != data.size()) {
                sf.cancelWriting();
                return false;
            }
            if (!sf.commit())
                return false;
        }
    }

    // 如果上面走的是 L1：sf.commit() 已经把 tmpPath 提交为最终的 tmpPath（不是 destPath），
    //   因为 QSaveFile 写入的是 tmpPath 本身 → commit 时直接更名到 destPath？
    //   实际上 QSaveFile 打开的文件名就是"最终名"；它内部自动用"最终名.tmp"写再 rename。
    //   所以上面 L1 路径：QSaveFile sf(tmpPath) → sf.commit() → tmpPath 存在。
    //   L2 路径：applyAndSave(outPath=tmpPath) 内部也用 QSaveFile+commit → tmpPath 存在。
    //   因此无论哪个路径，tmpPath 现在都是完整内容，我们还需把它 → destPath。
    // 注意：如果 destPath == tmpPath（不会发生，因为 tmpPath = destPath + ".tmp"），跳过。
    if (QFile::exists(destPath))
        QFile::remove(destPath);
    if (!QFile::rename(tmpPath, destPath)) {
        // rename 失败：回退 copy + remove
        if (!QFile::copy(tmpPath, destPath)) {
            QFile::remove(tmpPath);
            return false;
        }
        QFile::remove(tmpPath);
    }
    return true;
}

bool EditorTab::openAutosaveAutoMode(const QString &autosavePath, const QString &realFilePath)
{
    if (autosavePath.isEmpty() || !QFileInfo::exists(autosavePath))
        return false;

    const qint64 size = QFileInfo(autosavePath).size();
    const qint64 limit = FileTierPolicy::StandardEditLimitBytes;
    const FileTierDecision tier = FileTierPolicy::classify(size);
    QElapsedTimer t; t.start();

    if (tier.tier == FileTier::StandardEdit) {
        // ---- 小文件 autosave：走原 loadAutosave（同步 L1，毫秒级）----
        qDebug("[openAutosaveAutoMode] 选 L1: autosaveSize=%lld KB < limit=%lld KB. 调用 loadAutosave...",
               size / 1024, limit / 1024);
        const bool ok = loadAutosave(autosavePath);
        qDebug("[openAutosaveAutoMode] L1 loadAutosave 返回 ok=%d, 耗时=%lld ms",
               ok ? 1 : 0, t.elapsed());
        if (ok && !realFilePath.isEmpty()) {
            // 已有真实路径：对用户显示原名 + 标记"有未保存修改"
            m_filePath = realFilePath;
            // 注意：loadAutosave 已经 setModified(true)，所以无需再调
            emit titleChanged(tabTitle());
        }
        return ok;
    }

    // ---- S2/S3 autosave：mmap 到 VirtualFileView，秒开 ----
    //   不再主线程 readAll(100MB) → 不再阻塞首帧渲染。
    qDebug("[openAutosaveAutoMode] 选 L2: autosaveSize=%lld KB >= limit=%lld KB. 调 ensureL2View+openFile（mmap 秒开）",
           size / 1024, limit / 1024);
    VirtualFileView *v = ensureL2View();
    QString err;
    bool ok = v->openFile(autosavePath, &err, this->window());
    qDebug("[openAutosaveAutoMode] L2 VirtualFileView::openFile 返回 ok=%d err='%s', 耗时=%lld ms（应 < 50ms；若 > 500ms 说明 mmap/quickBuildFirstRange 意外阻塞）",
           ok ? 1 : 0, qUtf8Printable(err), t.elapsed());
    if (!ok) {
        // L2 失败（mmap 不支持/极端编码情况）：回退到同步 loadAutosave。
        //   虽然仍会阻塞，但至少 L2 成功率极高（≥99%），失败才兜底。
        qWarning("[openAutosaveAutoMode] L2 open 失败，回退 L1 同步 loadAutosave（⚠️ 此时启动会被阻塞!）: %s",
                 qUtf8Printable(err));
        t.restart();
        const bool okL1 = loadAutosave(autosavePath);
        qWarning("[openAutosaveAutoMode] L1 回退 loadAutosave 耗时=%lld ms, ok=%d",
                 t.elapsed(), okL1 ? 1 : 0);
        if (okL1 && !realFilePath.isEmpty()) {
            m_filePath = realFilePath;
            emit titleChanged(tabTitle());
        }
        return okL1;
    }

    // L2 成功：设置元数据（标题栏显示"原名*"，modified 状态，autosave 关联）
    m_filePath = realFilePath;         // 保存走 realFilePath；若空则"未命名"由 isUntitled() 判定
    m_autosavePath = autosavePath;     // 保持 autosave 关联（会话下次恢复、saveAsFile 清空等）
    m_l2Mode = true;
    m_l2SaveNeeded = true;             // 有未保存内容
    m_lastL2Line = 0;
    m_lastL2Col  = 0;
    m_lastL2Byte = 0;
    // 同 openLargeFileL2：先切 stack→1，再 emit（保证 isL2Active()=true 时再通知 MainWindow）
    if (m_stack) m_stack->setCurrentWidget(v);
    emit l2ModeStatusChanged();

    // 根据 realFilePath 或 autosavePath 猜语法（L2 暂不做词法，但信号保持统一）
    const QString &nameForSyntax = !realFilePath.isEmpty() ? realFilePath : autosavePath;
    Syntax syn = guessSyntaxFromExtension(nameForSyntax);
    if (syn != m_syntax) {
        m_syntax = syn;
        emit syntaxChanged(syn);
    }

    emit encodingChanged(m_encoding);
    emit modificationChanged(true);   // autosave 内容 = 有未保存修改
    emit titleChanged(tabTitle());
    emit cursorPositionChanged(0, 0);
    emit l2FileOpenedChanged(true);
    emit l2SaveNeededChanged(true);
    return true;
}

// ============================================================================
//  统一分流：编辑/剪贴板/跳转（外部不要直接调 editor()->xxx()，走这里）
// ============================================================================

void EditorTab::selectAll()
{
    if (isL2Mode() && m_l2View) {
        // L2：从 0 字节跳到文件末尾（长度即 fileSize+patch 字节差。简化：不精确选中但把光标移到末尾）
        gotoByteOffset(std::numeric_limits<qint64>::max());
        return;
    }
    m_editor->selectAll();
}

void EditorTab::undo()
{
    if (isL2Mode() && m_l2View) {
        m_l2View->undo(nullptr);
        return;
    }
    m_editor->undo();
}

void EditorTab::redo()
{
    if (isL2Mode() && m_l2View) {
        m_l2View->redo(nullptr);
        return;
    }
    m_editor->redo();
}

void EditorTab::cut()
{
    if (isL2Mode() && m_l2View) {
        // L2：简化为 copy → delete 选中字节区间（P5 先不做剪贴版深度对接，此处给个最小：copy 即可）
        m_l2View->copy();
        return;
    }
    m_editor->cut();
}

void EditorTab::copy()
{
    if (isL2Mode() && m_l2View) {
        m_l2View->copy();
        return;
    }
    m_editor->copy();
}

void EditorTab::paste()
{
    if (isL2Mode() && m_l2View) {
        // 从系统剪贴板取文本 → 转 UTF-8 字节 → pasteBytes
        QClipboard *cb = QGuiApplication::clipboard();
        if (!cb) return;
        QString txt = cb->text();
        if (txt.isEmpty()) return;
        QByteArray bytes = txt.toUtf8();
        m_l2View->pasteBytes(bytes, nullptr);
        return;
    }
    m_editor->paste();
}

void EditorTab::setFocusEditor()
{
    if (isL2Mode() && m_l2View) {
        m_l2View->setFocus();
        return;
    }
    m_editor->setFocus();
}

void EditorTab::gotoByteOffset(qint64 byteOffset, int selectLength)
{
    if (isL2Mode() && m_l2View) {
        if (selectLength > 0) {
            // 真·选中 [byteOffset, byteOffset+selectLength)（findNext/findPrev / replace 路径用）
            m_l2View->selectBytes(byteOffset, byteOffset + qint64(selectLength));
        } else {
            m_l2View->gotoByteOffset(byteOffset, false);
        }
        return;
    }
    // L1：用 Scintilla SCI_GOTOPOS（字节偏移，UTF-8）定位，然后读取 line/col 更新
    if (byteOffset < 0) byteOffset = 0;
    if (byteOffset > INT_MAX) byteOffset = INT_MAX;
    m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                            static_cast<long>(byteOffset));
    int line, col;
    m_editor->getCursorPosition(&line, &col);
    emit cursorPositionChanged(line, col);
}

// ============================================================================
//  搜索分流：L1=Qsci/逐行扫描；L2=LargeFileSearch 异步转同步
// ============================================================================

namespace {  // 匿名：L2 findAll 同步等待 helper
struct L2SearchAwaiter {
    QEventLoop loop;
    bool       done = false;
    qint64     total = 0;
    QString    err;
};

// 秒开：LineIndex 未全量构建完之前，阻塞 UI 线程但允许重绘/交互的等待器。
// 仅当 LineIndex 真的 incomplete 才等待；若构建已完成（lineBuildFinished 已发过）则瞬间返回。
// 返回 true 表示等完且 LineIndex 完整；false 表示 view 中途失效
bool waitForL2LineIndex(VirtualFileView *view, QWidget *progressParentForContext) {
    if (!view) return false;
    if (!view->isOpen()) return false;
    if (!view->lineIndex().isIncomplete()) return true;

    // 状态栏同步显示（仅当 progressParent 存在时提示，避免测试环境）
    if (auto *mw = qobject_cast<QMainWindow*>(progressParentForContext ? progressParentForContext->window() : nullptr)) {
        mw->statusBar()->showMessage(QObject::tr("等待行索引构建完成…"), 0);
    }
    QEventLoop loop;
    auto conn = QObject::connect(view, &VirtualFileView::lineBuildFinished, &loop,
                                 [&](bool){ loop.quit(); });
    loop.exec(QEventLoop::AllEvents);
    QObject::disconnect(conn);
    if (!view->isOpen()) return false;
    return !view->lineIndex().isIncomplete();
}

} // namespace

SearchResultsModel* EditorTab::findAllL2(const QString& pattern, bool regex,
                                          bool caseSensitive, bool wholeWord,
                                          QObject* parentForModel)
{
    if (!isL2Mode() || !m_l2View || pattern.isEmpty())
        return nullptr;

    // 秒开：搜索必须拿到准确的行号，所以等 LineIndex 完整构建（QEventLoop 本身 NOT modal，
    // 允许用户继续操作/滚动/切换 tab；注意：这里已没有 WindowModal QProgressDialog block 交互）
    if (!waitForL2LineIndex(m_l2View, this->window()))
        return nullptr;

    LargeFileSearch *s = m_l2View->searchEngine();  // 懒创建 + setSource 已内部完成
    if (!s) return nullptr;

    LargeFileSearch::Options opts;
    opts.caseSensitive = caseSensitive;
    opts.wholeWord     = wholeWord;
    if (regex) {
        QRegularExpression::PatternOptions ropts;
        if (!caseSensitive) ropts |= QRegularExpression::CaseInsensitiveOption;
        opts.regex = QRegularExpression(pattern, ropts);
        if (!opts.regex.isValid()) return nullptr;
    }

    // 非模态：进度显示在 MainWindow 状态栏（若有）；不再弹 WindowModal QProgressDialog
    auto *mw = qobject_cast<QMainWindow*>(this->window());
    L2SearchAwaiter aw;
    QMetaObject::Connection c1 = QObject::connect(s, &LargeFileSearch::finished,
        [&](qint64 total){ aw.done = true; aw.total = total;
            if (mw) mw->statusBar()->showMessage(QObject::tr("搜索完成：共 %1 处匹配").arg(total), 3000);
            aw.loop.quit(); });
    QMetaObject::Connection c2 = QObject::connect(s, &LargeFileSearch::failed,
        [&](const QString& msg){ aw.done = true; aw.err = msg;
            if (mw) mw->statusBar()->showMessage(QObject::tr("搜索失败：%1").arg(msg), 5000);
            aw.loop.quit(); });
    QMetaObject::Connection c3 = QObject::connect(s, &LargeFileSearch::progress,
        [&](qint64 done, qint64 total){
            if (!mw || total <= 0) return;
            int pct = int((done * 100LL) / total);
            mw->statusBar()->showMessage(QObject::tr("正在搜索大文件… %1%").arg(pct), 0);
        });
    // 取消入口：暂由 statusBar 提供（后续 AsyncTaskPanel 统一取消按钮）；此处保留接口但不连 QProgressDialog
    Q_UNUSED(mw);

    s->findAll(pattern, opts);
    if (!aw.done)
        QTimer::singleShot(0, &aw.loop, [&]{ /* ensure loop runs even if already done */ });
    aw.loop.exec();

    QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
    if (!aw.err.isEmpty() || aw.total <= 0) {
        // 0 匹配也返回空的 model（Dock 可以显示 0 结果）
    }
    auto *model = new SearchResultsModel(parentForModel);
    model->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount());
    return model;
}

// ============================================================================
//  findAllL2Async（纯异步：不阻塞 UI，信号回传结果）
// ============================================================================
void EditorTab::resetPendingL2Search()
{
    QObject::disconnect(m_pendingL2Search.waitLineBuildConn);
    QObject::disconnect(m_pendingL2Search.searchProgressConn);
    QObject::disconnect(m_pendingL2Search.searchFinishedConn);
    QObject::disconnect(m_pendingL2Search.searchFailedConn);
    m_pendingL2Search = PendingL2Search{};
}

void EditorTab::startPendingL2Search()
{
    if (!m_pendingL2Search.active || !m_l2View) {
        resetPendingL2Search();
        return;
    }
    // LineIndex 仍未完整：不应进入此处（应由 findAllL2Async 在 incomplete 时先等信号）
    if (m_l2View->lineIndex().isIncomplete()) {
        emit findAllL2Progress(-1, 0, 0);  // -1 表示仍在等 LineIndex
        return;
    }

    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) {
        auto *empty = new SearchResultsModel(m_pendingL2Search.parentForModel);
        emit findAllL2Finished(empty, m_pendingL2Search.pattern, fileName());
        resetPendingL2Search();
        return;
    }

    LargeFileSearch::Options opts;
    opts.caseSensitive = m_pendingL2Search.caseSensitive;
    opts.wholeWord     = m_pendingL2Search.wholeWord;
    if (m_pendingL2Search.regex) {
        QRegularExpression::PatternOptions ropts;
        if (!m_pendingL2Search.caseSensitive) ropts |= QRegularExpression::CaseInsensitiveOption;
        opts.regex = QRegularExpression(m_pendingL2Search.pattern, ropts);
        if (!opts.regex.isValid()) {
            auto *empty = new SearchResultsModel(m_pendingL2Search.parentForModel);
            emit findAllL2Finished(empty, m_pendingL2Search.pattern, fileName());
            resetPendingL2Search();
            return;
        }
    }

    // 先把之前可能残留的连接断开
    QObject::disconnect(m_pendingL2Search.searchProgressConn);
    QObject::disconnect(m_pendingL2Search.searchFinishedConn);
    QObject::disconnect(m_pendingL2Search.searchFailedConn);

    QPointer<EditorTab> that(this);
    QObject* parentForModel = m_pendingL2Search.parentForModel;
    QString patternCopy    = m_pendingL2Search.pattern;
    QString fileNameCopy   = fileName();

    m_pendingL2Search.searchProgressConn = QObject::connect(
        s, &LargeFileSearch::progress, this,
        [this, that](qint64 done, qint64 total) {
            if (!that) return;
            int pct = (total > 0) ? int((done * 100LL) / total) : -1;
            emit findAllL2Progress(pct, done, total);
        });

    m_pendingL2Search.searchFinishedConn = QObject::connect(
        s, &LargeFileSearch::finished, this,
        [this, that, s, parentForModel, patternCopy, fileNameCopy](qint64 /*total*/) {
            if (!that) return;
            auto *model = new SearchResultsModel(parentForModel);
            model->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount());
            // 把匹配位置传给 VirtualFileView 用于正文区域染色
            if (that->m_l2View) {
                QVector<QPair<qint64, qint32>> highlights;
                const auto& memMatches = s->memoryMatches();
                highlights.reserve(memMatches.size());
                for (const auto& m : memMatches) {
                    highlights.append(qMakePair(m.byteOffset, m.length));
                }
                that->m_l2View->setSearchHighlights(highlights);
            }
            emit findAllL2Finished(model, patternCopy, fileNameCopy);
            that->resetPendingL2Search();
        });

    m_pendingL2Search.searchFailedConn = QObject::connect(
        s, &LargeFileSearch::failed, this,
        [this, that, parentForModel, patternCopy, fileNameCopy](const QString& /*msg*/) {
            if (!that) return;
            // 失败也返回空 model（让 Dock 显示 0 结果，避免 UI 不一致）
            auto *empty = new SearchResultsModel(parentForModel);
            emit findAllL2Finished(empty, patternCopy, fileNameCopy);
            that->resetPendingL2Search();
        });

    // 走 AsyncTaskManager 派发
    s->findAllViaTaskManager(m_pendingL2Search.pattern, opts, fileNameCopy);
}

bool EditorTab::findAllL2Async(const QString& pattern, bool regex,
                               bool caseSensitive, bool wholeWord,
                               QObject* parentForModel)
{
    if (!isL2Mode() || !m_l2View || pattern.isEmpty())
        return false;

    // 若已有进行中的搜索：先重置（disconnect + 取消 AsyncTaskManager 的上一次搜索任务）
    if (m_pendingL2Search.active) {
        LargeFileSearch *oldS = m_l2View->searchEngine();
        if (oldS && oldS->lastTaskId() > 0) {
            AsyncTaskManager::instance().cancel(oldS->lastTaskId());
        }
        resetPendingL2Search();
    }

    m_pendingL2Search.active        = true;
    m_pendingL2Search.pattern       = pattern;
    m_pendingL2Search.regex         = regex;
    m_pendingL2Search.caseSensitive = caseSensitive;
    m_pendingL2Search.wholeWord     = wholeWord;
    m_pendingL2Search.parentForModel = parentForModel;

    if (!m_l2View->lineIndex().isIncomplete()) {
        // LineIndex 已完整：立即启动
        startPendingL2Search();
        return true;
    }

    // LineIndex 还在后台构建：先发 -1 进度（UI 显示"等待行索引构建"），
    // 等 l2LineBuildFinished 信号再启动搜索。
    emit findAllL2Progress(-1, 0, 0);
    QPointer<EditorTab> that(this);
    m_pendingL2Search.waitLineBuildConn = QObject::connect(
        this, &EditorTab::l2LineBuildFinished, this,
        [this, that](bool ok) {
            if (!that) return;
            QObject::disconnect(that->m_pendingL2Search.waitLineBuildConn);
            that->m_pendingL2Search.waitLineBuildConn = QMetaObject::Connection{};
            if (!ok || !that->m_l2View || that->m_l2View->lineIndex().isIncomplete()) {
                // LineIndex 构建失败：返回空 model
                auto *empty = new SearchResultsModel(that->m_pendingL2Search.parentForModel);
                emit findAllL2Finished(empty,
                                       that->m_pendingL2Search.pattern,
                                       that->fileName());
                that->resetPendingL2Search();
                return;
            }
            that->startPendingL2Search();
        });
    return true;
}

bool EditorTab::findNext(const QString &pattern, bool regex, bool caseSensitive,
                         bool wholeWord, bool forward, bool wrap)
{
    if (pattern.isEmpty()) return false;
    if (!isL2Mode()) {
        // L1 原逻辑
        bool re = regex;
        bool cs = caseSensitive;
        bool wo = wholeWord;
        bool found = false;
        if (forward) {
            int line, index;
            m_editor->getCursorPosition(&line, &index);
            found = m_editor->findFirst(pattern, re, cs, wo, wrap, true, line, index, true);
        } else {
            int line, index;
            m_editor->getCursorPosition(&line, &index);
            found = m_editor->findFirst(pattern, re, cs, wo, wrap, false, line, index, true);
        }
        return found;
    }
    // L2 路径：先做 findAll 同步（缓存到内存+溢出），然后 findNext/findPrev
    auto *model = findAllL2(pattern, regex, caseSensitive, wholeWord, nullptr);
    if (!model) return false;
    delete model;  // 不需要 model 了，LargeFileSearch 内部已保留结果集
    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) return false;
    LargeFileSearchMatch m;
    qint64 from = m_lastL2Byte;
    qint64 off = forward ? s->findNext(from, &m) : s->findPrev(from, &m);
    if (off < 0) {
        if (!wrap) return false;
        // wrap：从开头/结尾重新找
        off = forward ? s->findNext(-1, &m) : s->findPrev(std::numeric_limits<qint64>::max(), &m);
    }
    if (off < 0) return false;
    gotoByteOffset(m.byteOffset, m.length);
    // 从下一次查找的「从 off+1 继续」语义（对齐 notepad++：findNext 从匹配末尾之后的字节开始）
    m_lastL2Byte = m.byteOffset + qint64(m.length);
    return true;
}

void EditorTab::replaceCurrent(const QString &replaceText)
{
    if (!isL2Mode()) { m_editor->replace(replaceText); return; }
    if (!m_l2View) return;
    // 先决条件：findNext 必须已经调用过并且 [selStart, selEnd) 正好覆盖待替换的 old。
    if (!m_l2View->hasSelection()) return;
    const qint64 s = m_l2View->selectionStart();
    const qint64 e = m_l2View->selectionEnd();
    if (e <= s) return;
    const qint64 maxL = m_l2View->patches().virtualSize(m_l2View->mmapSize());
    Q_UNUSED(maxL);
    const qint32 oldLen = qint32(qMin<qint64>(e - s, qint64(INT_MAX)));
    const QByteArray newData = replaceText.toUtf8();
    QString err;
    const bool ok = m_l2View->replaceBytes(s, oldLen, newData, &err);
    if (!ok) {
        if (auto* mw = qobject_cast<QMainWindow*>(window()); mw && mw->statusBar() && !err.isEmpty()) {
            mw->statusBar()->showMessage(QObject::tr("替换失败：%1").arg(err), 5000);
        }
    }
}

int EditorTab::replaceAll(const QString &find, const QString &replace, bool regex,
                          bool caseSensitive, bool wholeWord)
{
    if (find.isEmpty()) return 0;
    if (!isL2Mode()) {
        m_editor->setCursorPosition(0, 0);
        int count = 0;
        bool found = m_editor->findFirst(find, regex, caseSensitive, wholeWord,
                                         false, true, 0, 0, false);
        while (found) {
            m_editor->replace(replace);
            ++count;
            found = m_editor->findNext();
        }
        return count;
    }
    // L2：调 LargeFileSearch::replaceAll 分段原子替换（临时仍同步执行，但移除 WindowModal 进度窗避免阻塞）
    if (!m_l2View) return 0;
    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) return 0;

    LargeFileSearch::Options opts;
    opts.caseSensitive = caseSensitive;
    opts.wholeWord     = wholeWord;
    if (regex) {
        QRegularExpression::PatternOptions ropts;
        if (!caseSensitive) ropts |= QRegularExpression::CaseInsensitiveOption;
        opts.regex = QRegularExpression(find, ropts);
        if (!opts.regex.isValid()) return 0;
    }

    auto *mw = qobject_cast<QMainWindow*>(this->window());
    LargeFileSearch::ReplaceCallbacks cb;
    cb.progressBatch = [&mw, &s](qint64 done, qint64 total) {
        if (mw && mw->statusBar() && total > 0) {
            int pct = int((done * 100LL) / total);
            mw->statusBar()->showMessage(
                QObject::tr("正在批量替换大文件… %1%").arg(pct), 0);
        }
        // 允许重绘，不接受用户输入避免编辑态冲突
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    };
    cb.onPressureSoftWarn = [&](const QString&) {
        // P4 简化：默认继续执行，不弹中间保存
        return false;
    };
    cb.needSaveAsPath = []() { return QString(); };

    // 取消入口：P6 统一迁移到 AsyncTaskManager 任务面板；此处暂不提供 UI 取消按钮
    Q_UNUSED(s);

    QString err;
    qint64 replacedCount = 0;
    bool ok = s->replaceAll(find, replace, opts, m_l2View->patches(), cb, &err, &replacedCount);
    if (mw && mw->statusBar()) {
        if (ok) mw->statusBar()->showMessage(QObject::tr("替换完成：共 %1 处").arg(replacedCount), 3000);
        else    mw->statusBar()->showMessage(QObject::tr("替换失败：%1").arg(err), 5000);
    }
    if (!ok) return 0;
    // 返回总数：replacedCount（精确替换成功数）
    return int(replacedCount);
}
