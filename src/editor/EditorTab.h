#pragma once

#include <QTimer>
#include <QByteArray>
#include <QVector>
#include <QUuid>
#include <limits>
#include <optional>
#include <Qsci/qsciscintilla.h>
#include "TextEncoding.h"
#include "LargeFileSearch.h"   // 先入 LargeFileSearchMatch 定义
#include "SearchMatch.h"
#include "EditableTextFindAllTask.h"
#include "DiskFileVersion.h"
#include "DocumentState.h"
#include "DocumentSession.h"
#include "DocumentSurface.h"
#include "DocumentContentPort.h"
#include "SessionDocumentPort.h"
#include "DocumentSearchSurface.h"
#include "HexMouseSelectionController.h"

enum class ViewMode : int;  // 前置声明（定义在 VirtualFileView.h）

// Historical L2 names are retained in session/API data for compatibility.
// They select text versus binary presentation only: S2/S3/B2/B3 always use
// WindowedReadOnlyProvider and are never promoted to an editable document.
enum class L2Presentation {
    Text,
    Hex,
};

enum class L2ContentKind {
    Text,
    Binary,
};

class QsciLexer;
class VirtualFileView;
class WindowedReadOnlyProvider;
class EditorSearchSource;
class LargeFileSearch;
bool ownsWindowedReadOnlyInputObject(const WindowedReadOnlyProvider *provider,
                                     const QObject *object);
class QStackedWidget;
class QIODevice;
class QKeyEvent;
class QInputMethodEvent;
class QPlainTextEdit;
class QSplitter;
class EditorTab;
SessionTab captureEditorSessionTab(const EditorTab *tab);

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
    JavaScript,
    Java,
    CSharp,
    Css,
    Bash,
    Batch,
    CMake,
    Makefile,
    Lua,
    Ruby,
    Perl,
    Diff,
    Properties,
    Asm,
};

class EditorTab : public DocumentSearchSurface, public IDocumentSurface,
                  public IDocumentContentPort, public ISessionDocumentPort {
    Q_OBJECT
public:
    using ExternalFileState = DiskFileState;
    enum class LargeDocumentRuntimeState {
        None,
        ReadOnlyAnalysis,
    };
    Q_ENUM(LargeDocumentRuntimeState)

    explicit EditorTab(QWidget *parent = nullptr);
    ~EditorTab() override;
    bool openSurface(const DocumentSurfaceOpenRequest &request,
                     QString *error) override;
    DocumentState persistenceState() const noexcept override { return documentState(); }
    TextEncoding persistenceEncoding() const noexcept override { return m_encoding; }
    DiskFileVersion persistenceBaseline() const override {
        return m_documentSession.diskVersion();
    }
    bool createPersistenceSnapshot(qint64 maximumBytes,
                                   DocumentSnapshot *snapshot,
                                   QString *error) override;
    bool suspendPersistenceBacking(QString *error) override;
    bool resumePersistenceBacking(const QString &absolutePath,
                                  QString *error) override;
    void acceptPersistenceCommit(const QString &absolutePath,
                                 DiskFileVersion version) override;
    void acceptSecurePersistenceCommit(const QUuid &documentId,
                                       const QString &title,
                                       const QString &absolutePath,
                                       const QString &backingPath) override;
    SessionTab captureSessionTab() const override {
        return captureEditorSessionTab(this);
    }
    quint64 sessionRevision() const override { return contentRevision(); }
    bool needsRecoverySnapshot() const override {
        return isUntitled() || isModified();
    }
    bool isEmptyUntitledDocument() const override {
        return isUntitled() && !isModified() && !isLargeDocument()
            && editor() && editor()->length() == 0;
    }
    bool isLargeRecoveryDocument() const override { return isLargeDocument(); }
    QString recoveryPath() const override {
        return m_documentSession.recoveryPath();
    }
    void acceptRecoveryPath(const QString &path) override { setAutosavePath(path); }
    bool captureRecoveryPayload(SessionRecoveryPayload *payload,
                                QString *error) override;
    bool restoreSessionRecovery(const SessionRestoreRequest &request,
                                QString *error) override;

    /** 关闭/退出前主动清理 L2 的异步构建任务 + mmap（MainWindow::closeEvent 开头调用）。
     *  此时 MainWindow 还完整，允许 processEvents，轮询到 AsyncTask 终态才返回，
     *  彻底避免「析构阶段还有 worker 访问 mmap」导致的 0xC0000005 访问违例。
     *  L1 模式：什么都不做，直接返回。 */
    void shutdownL2BeforeClose();

    // ---- L1 路径（QsciScintilla，现有接口完全不变，避免功能回退）----
    // 文件操作
    bool loadFile(const QString &path,
                  std::optional<TextEncoding> forcedEncoding = std::nullopt);
    bool loadAutosave(const QString &autosavePath);  // 兼容旧版明文恢复文件
    // 从已解密的内部恢复快照加载，不把正文再次落入临时文件。
    bool loadAutosaveBytes(const QByteArray &data, const QString &backingPath);
    bool isModified() const;
    void setModified(bool m);
    bool isUntitled() const;
    QString filePath() const;
    void setFilePath(const QString &path);
    // Bounded on-disk version tracking shared by editable S1 and mmap/Hex views.
    // A save must never silently replace a file that changed after it was opened.
    ExternalFileState externalFileState() const;
    void refreshDiskVersionBaseline();
    bool representsDiskFile(const DiskFileVersion &candidate) const;
    QString fileName() const;
    QString tabTitle() const;
    QUuid sessionInstanceId() const { return m_sessionInstanceId; }
    quint64 contentRevision() const { return m_documentSession.state().revision; }
    DocumentState documentState() const noexcept;
    // Produces the immutable, bounded input accepted by document tools. The
    // snapshot never includes a path or account-workspace secret.
    bool createDocumentSnapshot(qint64 maximumBytes, DocumentSnapshot *snapshot,
                                QString *error = nullptr) const;
    // Secure notes use the same QScintilla editor but are persisted through
    // VaultService instead of a plaintext filesystem path.
    bool isSecureDocument() const {
        return m_documentSession.state().securityClass == DocumentSecurityClass::SecureDocument;
    }
    bool isSecureBinaryDocument() const {
        return isSecureDocument()
            && m_documentSession.state().contentKind == DocumentContentKind::Binary;
    }
    bool isBinaryDocument() const {
        return m_documentSession.state().contentKind == DocumentContentKind::Binary;
    }
    // Content classification and the active representation are independent.
    // A Binary document shown as Text participates in decoded text search;
    // byte/hex search belongs only to the Hex representation.
    bool isHexPresentation() const;
    bool hexOverwriteMode() const { return m_hexOverwriteMode; }
    void toggleHexOverwriteMode();
    bool isSecureDocumentMasked() const { return m_documentSession.secureMasked(); }
    bool isSecureDocumentTransitioning() const { return m_documentSession.secureTransitioning(); }
    QUuid secureDocumentId() const {
        return isSecureDocument() ? m_documentSession.state().id : QUuid{};
    }
    QString secureDocumentTitle() const { return m_documentSession.secureTitle(); }
    void initializeSecureDocument(const QUuid &id, const QString &title,
                                  const QString &plainText = {},
                                  const QString &filePath = {});
    bool initializeSecureBinaryDocument(const QUuid &id, const QString &title,
                                        QByteArray bytes,
                                        const QString &filePath = {},
                                        QString *error = nullptr);
    qint64 securePayloadLimitExclusive() const {
        return m_documentSession.securePayloadLimitExclusive();
    }
    void setSecurePayloadLimitExclusive(qint64 limit);
    void maskSecureDocument();
    void beginSecureDocumentLocking();
    void cancelSecureDocumentLocking();
    void setSecureDocumentTitle(const QString &title);
    void convertToPlainDocument(const QString &filePath);
    TextEncoding encoding() const { return m_encoding; }
    void setEncoding(TextEncoding enc);
    EncodingOrigin encodingOrigin() const { return m_encodingOrigin; }
    EncodingConfidence encodingConfidence() const { return m_encodingConfidence; }
    bool encodingHasDecodeErrors() const { return m_encodingDecodeErrors; }
    QString lineEndingName() const;
    Syntax syntax() const;

    // 编辑器访问（L2 模式下仍返回有效 QsciScintilla 指针，但内容为空；
    // 优先使用下方统一分流的 selectAll/copy/paste/... 等接口，避免直接调 editor 方法）
    QsciScintilla *editor() const;
    // Every editor surface this tab owns, for theming.
    QList<QsciScintilla *> themedEditors() const;
    // The search engine of the current large-document source (editable or
    // windowed read-only); null for small documents.
    LargeFileSearch *searchEngine() const;
    qint64 currentByteOffset() const;

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
    void selectionToUpper();
    void selectionToLower();
    void joinSelectedLines();

    // Native Scintilla selection/navigation commands. They are deliberately
    // exposed through EditorTab so callers cannot accidentally apply them to
    // the Hex byte model or a bounded S2/S3 projection.
    void selectNextOccurrence();
    void selectAllOccurrences();
    void moveToMatchingBrace();
    void selectToMatchingBrace();
    void foldCurrentLine();
    void foldAll(bool expand);
    bool supportsTextMutationCommands() const;
    bool supportsLexerCommands() const;

    // 书签
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    void clearBookmarks();

    // 搜索（供 FindOverlay 调用；内部按 isL2Mode 自动分流 L1/L2）
    // findNext 返回是否找到，并跳转光标
    bool findNext(const QString &pattern, bool regex, bool caseSensitive,
                  bool wholeWord, bool forward, bool wrap,
                  bool hexBytes = false);
    // Directional navigation reuses a matching result cache first. Small S1
    // documents use an inline first-hit scan; larger uncached documents run in
    // AsyncTaskManager. It never builds Find All merely to navigate.
    bool findNextL2Async(const QString &pattern, bool regex,
                         bool caseSensitive, bool wholeWord,
                         bool forward, bool wrap, bool hexBytes,
                         QObject *callbackContext,
                         std::function<void(bool found, QString error)> callback);
    void resetSearchPosition(bool forward);
    bool currentSelectionMatchesSearch(const QString &pattern, bool regex,
                                       bool caseSensitive, bool wholeWord,
                                       bool hexBytes = false);
    bool replaceCurrentSearchMatch(const QString &find,
                                   const QString &replacement,
                                   bool regex, bool caseSensitive,
                                   bool wholeWord, QString *error = nullptr);
    // replace 当前匹配（需先 findNext）
    void replaceCurrent(const QString &replaceText);
    // replaceAll 返回替换次数（L2：预算内一次命令对应一个 UndoGroup）
    int replaceAll(const QString &find, const QString &replace, bool regex,
                   bool caseSensitive, bool wholeWord);
    // findAll 收集所有匹配；L1 走 QVector、L2 走 SearchResultsModel
    QVector<SearchMatch> findAll(const QString &pattern, bool regex,
                                 bool caseSensitive, bool wholeWord);
    // S1 Find All uses an immutable UTF-8 snapshot and performs compilation,
    // scanning and result shaping in AsyncTaskManager.  The callback is always
    // delivered on callbackContext's thread and is revision-guarded.
    bool findAllL1Async(
        const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
        QObject *callbackContext,
        std::function<void(QVector<SearchMatch> matches, bool truncated,
                           QString error)> callback);
    bool lastFindAllWasTruncated() const { return m_lastFindAllTruncated; }
    QString lastSearchError() const { return m_lastSearchError; }
    void setSearchHighlights(const QVector<SearchMatch> &matches);
    void clearSearchHighlights();
    void selectSearchMatch(int line, int index, int length);
    DocumentSearchCursor searchCursor() const override;
    void selectSearchHit(qint64 byteOffset, qint32 byteLength) override;
    // L2 专用 findAll（同步阻塞版本；搜索本身不等待 LineIndex）
    // 仅用于 L1 层的 findNext/findPrev 内部（需要先同步拿到结果集）及旧调用方
    // 返回 SearchResultsModel 所有权（调用方 delete 或 setParent）
    class SearchResultsModel* findAllL2(const QString& pattern, bool regex,
                                        bool caseSensitive, bool wholeWord,
                                        QObject* parentForModel = nullptr,
                                        bool hexBytes = false);
    // L2 专用 findAll（纯异步，不阻塞 UI）：
    //   - LineIndex 未构建完：先发布 byte range，完成后原位补齐行号
    //   - 搜索走 AsyncTaskManager::submit（LargeFileSearch）
    //   - 完成后触发 findAllL2Finished(SearchResultsModel*, pattern, fileName)
    //   - 期间触发 findAllL2Progress(int percent, qint64 done, qint64 total)
    //  返回 true = 成功启动；false = L1 模式或参数空未启动
    bool findAllL2Async(const QString& pattern, bool regex,
                        bool caseSensitive, bool wholeWord,
                        QObject* parentForModel = nullptr,
                        bool hexBytes = false);
    void cancelFindAllL2();

    // 光标位置
    void getCursorPosition(int *line, int *index) const;
    void setCursorPosition(int line, int index);

    // 长行标尺
    void setEdgeColumn(int column);
    void setEdgeVisible(bool visible);

    // 自动换行
    void setWordWrap(bool on);
    bool wordWrap() const;

    // Shared text-surface preferences. Display-only preferences may also be
    // applied to an S2/S3 text projection; mutation preferences only affect
    // the editable S1/Secure Text surface.
    void setWhitespaceVisible(bool visible);
    bool whitespaceVisible() const { return m_whitespaceVisible; }
    void setEolVisible(bool visible);
    bool eolVisible() const { return m_eolVisible; }
    bool convertEols(QsciScintilla::EolMode mode);
    void setUseTabs(bool useTabs);
    bool useTabs() const { return m_useTabs; }
    void setTabWidth(int width);
    int tabWidth() const { return m_tabWidth; }
    void setBackspaceUnindents(bool enabled);
    bool backspaceUnindents() const { return m_backspaceUnindents; }
    void setWordCompletionEnabled(bool enabled);
    bool wordCompletionEnabled() const { return m_wordCompletionEnabled; }

    // 以指定编码显示（Notepad++ 风格：不修改文件内容，仅改变显示解码方式）
    // L1 模式：重新以指定编码解码并加载到 QsciScintilla
    // L2 模式：设置 VirtualFileView 的 displayEncoding，重绘
    void setDisplayEncoding(TextEncoding enc);
    TextEncoding displayEncoding() const;

    // 内容决定视图：二进制固定为十六进制；只有窗口化只读文本可以在
    // 文本与十六进制查看之间切换，切换后保持当前位置并关闭跟随。
    ViewMode viewModeL2() const;
    bool canSwitchWindowedPresentation() const;
    bool reopenWindowedPresentation(ViewMode target, QString *error = nullptr);
    // 当前 tab 是否正在显示 L2 VirtualFileView（大文件视图），非 L1/QScintilla
    bool isL2Active() const;

    // 内部 autosave 路径（用于会话恢复时对用户透明）
    // Compatibility-facing path: secure documents historically expose their
    // encrypted backing file as the autosave path. Session recovery uses the
    // distinct recoveryPath() port above, so discard cannot delete ciphertext.
    QString autosavePath() const;
    void setAutosavePath(const QString &path) { m_documentSession.setRecoveryPath(path); }
    QString secureBackingPath() const { return m_documentSession.secureBackingPath(); }
    void setSecureBackingPath(const QString &path) {
        m_documentSession.setSecureBackingPath(path);
    }

    // ---- 统一分流的剪贴板/编辑操作（L1/L2 都正确，推荐外部统一走这些，不要调 editor()->xxx()）----
    void selectAll();
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    bool canUndoCommand() const;
    bool canRedoCommand() const;
    bool hasSelectionCommand() const;
    bool canModifyCommand() const;
    bool ownsUserInputObject(const QObject *object) const;
    // Current user-visible payload size in bytes.  The windowed mmap provider
    // owns the authoritative 64-bit size for S2/S3 read-only analysis;
    // Scintilla owns it only for sub-32-MiB ordinary/secure documents.
    qint64 contentSizeBytes() const;
    // Unicode scalar count for text presentations; Hex presentations return
    // their authoritative byte count. -1 means a large-file count is pending
    // or unavailable.
    qint64 contentCharacterCount() const;

    // 焦点设置（跳转后对用户可见）
    void setFocusEditor();

    // L2 跳转（byteOffset = 虚拟字节偏移，selectLength>0 表示选中 [byteOffset, byteOffset+selectLength)）
    // L1 模式下自动换算成行/列再跳转。
    void gotoByteOffset(qint64 byteOffset, int selectLength = 0);

    // ---- L2 路径（VirtualFileView 大文件虚拟视图，独立接口，不破坏 L1）----
    // 当前是否处于 L2 模式
    bool isL2Mode() const { return m_l2Mode; }
    bool isLargeDocument() const {
        return m_l2Mode && !isSecureBinaryDocument();
    }
    bool isReadOnlyByPolicy() const {
        return m_l2ReadOnlyByPolicy;
    }
    LargeDocumentRuntimeState largeDocumentRuntimeState() const;
    // 获取 L2 视图（未 openLargeFileL2 时返回 nullptr）
    VirtualFileView* l2View() const { return m_l2View; }
    WindowedReadOnlyProvider *windowedReadOnlyProvider() const {
        return m_windowedReadOnlyProvider;
    }
    // 显式进入 mmap 路径。生产路由仅用于 32 MiB 以上文件，并始终只读；
    // presentation/contentKind 只决定文本或 Hex 投影，不改变能力边界。
    bool openLargeFileL2(const QString &path, QString *errMsg = nullptr,
                         L2Presentation presentation = L2Presentation::Text,
                         L2ContentKind contentKind = L2ContentKind::Text,
                         std::optional<TextEncoding> forcedEncoding = std::nullopt,
                         const PersistenceReadPolicy &readPolicy = {},
                         bool requireReadOnly = false);
    // 仅供 S1 兼容 Hex 路径保存；32 MiB 以上由策略门禁拒绝。

    // ==== 会话恢复/保存专用：零副作用 + S1/只读 mmap 自动分流 ====
    // · exportToFile：S1 导出编辑内容；只读 mmap 仅复制原文件，不存在 patch。
    // S1 恢复专用：导出原编码字节到内存，交给 DraftStore 立即加密。
    // 大文件必须走后续分块恢复日志，避免构造 GiB 级 QByteArray。
    bool exportSnapshotBytes(QByteArray *data) const;
    // 加密恢复载荷：仅 S1 生成完整原编码快照；S2/S3 永久只读。
    bool exportRecoveryPayload(QByteArray *data, QString *error = nullptr) const;
    bool restoreRecoveryPayload(const QByteArray &data, const QString &realFilePath,
                                QString *error = nullptr);
    bool exportRecoveryCheckpoint(QIODevice *output, QString *error = nullptr) const;
    bool restoreRecoveryCheckpoint(const QString &checkpointPath,
                                   const QString &realFilePath,
                                   TextEncoding encoding, qint64 plaintextBytes,
                                   QString *error = nullptr);
    // Binary/Hex recovery stores only the normalized patch journal, encrypted
    // by DraftStore, and re-applies it only when the original file version matches.
    bool exportBinaryRecoveryJournal(QIODevice *output, QString *error = nullptr);
    bool restoreBinaryRecoveryJournal(const QString &journalPath,
                                      const QString &realFilePath,
                                      QString *error = nullptr);
    // · openAutosaveAutoMode：恢复 autosave 时按文件尺寸自动选 L1/L2：
    //   S1（< 32 MiB）→ 走原 loadAutosave (Scintilla sync，毫秒级)
    //   尺寸 ≥ 限制 → mmap autosave 文件到 L2 VirtualFileView（秒开），
    //                 然后对用户伪装为 realFilePath（标题栏/保存路径）并标记 modified。
    //   返回 true = 成功加载（L1 或 L2 均可）
    bool openAutosaveAutoMode(const QString &autosavePath,
                              const QString &realFilePath = QString());

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    bool handleHexAccessibleFocusTraversal(QEvent *event);

signals:
    void modificationChanged(bool modified);
    void cursorPositionChanged(int line, int index);
    void encodingChanged(TextEncoding encoding);
    void syntaxChanged(Syntax syntax);
    void titleChanged(const QString &title);
    void contentChanged(quint64 revision);
    void characterCountChanged();
    void commandCapabilitiesChanged();
    // Lazy Hex/L2 providers create their real input surfaces after the tab is
    // inserted into MainWindow.  Publish each surface so external file drops
    // can keep using the one authoritative open path.
    void externalDropSurfaceCreated(QWidget *surface);

    // 当前 EditorTab 内部 L2<->L1 模式切换（从 L1 切换到 L2 或反向）——MainWindow 用它同步"不适配 L2 的菜单项 enabled"
    // 触发点：openFile/L1 打开小文件(222行m_l2Mode=false)/openAutosaveAutoMode/... 等所有设置 m_l2Mode 的位置 emit
    // 极简设计：不带参数，由接收方调用 tab->isL2Active()/isL2Mode() 自行判断（减少参数不一致 bug）
    void l2ModeStatusChanged();

    // L2 专用信号：转发 VirtualFileView 的状态（MainWindow/状态栏同步）
    void l2CursorPositionChanged(int line, int col);
    void l2FileOpenedChanged(bool open);
    void l2SaveNeededChanged(bool needed);
    void l2UndoRedoChanged();
    // 转发 VirtualFileView 后台行索引构建进度（秒开 UI 状态栏）
    void l2LineBuildProgress(qint64 bytesDone, qint64 bytesTotal);
    void l2LineBuildFinished(bool ok);
    void largeDocumentRuntimeStateChanged(EditorTab::LargeDocumentRuntimeState state,
                                          const QString &reason);

private:
    void setViewModeL2(ViewMode mode);
    friend class HexMouseSelectionController;
    bool createDocumentSnapshotImpl(qint64 maximumBytes,
                                    DocumentSnapshot *snapshot,
                                    QString *error,
                                    bool allowSecureTransition) const;

    QsciScintilla *m_editor;
    QStackedWidget *m_stack = nullptr;  // 0: m_editor (L1), 1: m_l2View (L2)
    QWidget *m_secureLockingPage = nullptr;
    QUuid m_sessionInstanceId = QUuid::createUuid();
    DocumentSession m_documentSession;
    mutable quint64 m_characterCountRevision = std::numeric_limits<quint64>::max();
    mutable qint64 m_cachedCharacterCount = 0;
    mutable bool m_cachedCharacterCountHex = false;
    TextEncoding m_encoding = TextEncoding::Utf8;
    EncodingOrigin m_encodingOrigin = EncodingOrigin::Fallback;
    EncodingConfidence m_encodingConfidence = EncodingConfidence::Low;
    bool m_encodingDecodeErrors = false;
    Syntax m_syntax = Syntax::Plain;
    int m_bookmarkMarker = -1;
    bool m_edgeVisible = true;
    int m_edgeColumn = 80;
    bool m_whitespaceVisible = false;
    bool m_eolVisible = false;
    bool m_useTabs = false;
    int m_tabWidth = 4;
    bool m_backspaceUnindents = true;
    bool m_wordCompletionEnabled = true;
    bool m_s1ScrollDragRequestedEnd = false;

    void setupEditor();
    void applySyntax(Syntax syntax);
    QsciLexer *createLexer(Syntax syntax, QObject *owner = nullptr) const;
    QsciScintilla *activeTextEditor() const;
    QWidget *l1Page() const;
    void clearMarkers();
    void applyEdgeSettings();
    void activateL2Overlay(VirtualFileView *view);
    void clearL2BridgeBaseline();
    void ensureHexAsciiPreview(QWidget *parent);
    void layoutL2Overlay();
    void alignHexOverlayToGlobalScroll();
    void refreshTextProjectionMargin();
    bool handleHexKeyPress(QKeyEvent *event);
    bool handleHexAsciiKeyPress(QKeyEvent *event);
    bool handleHexInputMethodEvent(QInputMethodEvent *event, bool asciiSurface);
    bool handleHexInsertModeKey(QKeyEvent *event);
    bool hexSelectionByteRange(qint64 *start, qint64 *end,
                               int *nibble = nullptr) const;
    bool asciiSelectionByteRange(qint64 *start, qint64 *end) const;
    void setHexCaret(qint64 globalByte, int nibble = 0);
    void setAsciiCaret(qint64 globalByte);
    void syncAsciiSelectionFromHex();
    void syncHexSelectionFromAscii();
    void refreshHexAsciiProjectionFromEditor();

    // 大文件分块加载：通过 SCI_APPENDTEXT 分块追加 UTF-8 字节，
    // 避免 setText() 一次性重建整个文档导致 UI 卡死
    void loadUtf8Chunked(const QByteArray &utf8);

    // ---- L2 私有（懒创建 VirtualFileView，避免影响 L1 默认路径）----
    bool               m_l2Mode = false;
    bool               m_l2ReadOnlyByPolicy = false;
    // Scintilla has a save-point model for text edits but cannot be forced
    // dirty by metadata-only changes such as an encoding conversion.
    bool m_metadataModified = false;
    VirtualFileView   *m_l2View = nullptr;  // 懒创建，仅 openLargeFileL2 成功后非空
    EditorSearchSource *m_searchSource = nullptr;
    WindowedReadOnlyProvider *m_windowedReadOnlyProvider = nullptr;
    // QSaveFile commit on Windows requires the source mmap to be closed. A
    // failed commit must remap the old source without erasing dirty patches.
    bool m_persistenceBackingSuspendedWithEdits = false;
    bool               m_l2SaveNeeded = false;  // 对应 L2 saveNeededChanged 信号
    int                m_lastL2Line = 0;         // L2 光标缓存（getCursorPosition 用）
    int                m_lastL2Col  = 0;
    qint64             m_lastL2Byte = -1;        // L2 光标字节缓存（findNext 起始位置）
    using L1SearchHit = EditableTextFindAllHit;
    QVector<L1SearchHit> m_l1SearchCache;
    QString            m_l1SearchCachePattern;
    bool               m_l1SearchCacheRegex = false;
    bool               m_l1SearchCacheCaseSensitive = true;
    bool               m_l1SearchCacheWholeWord = false;
    bool               m_l1SearchCacheValid = false;
    quint64            m_l1SearchCacheRevision = 0;
    int                m_lastL1MatchIndex = -1;
    L1SearchHit        m_l1NavigationHit;
    QString            m_l1NavigationPattern;
    bool               m_l1NavigationRegex = false;
    bool               m_l1NavigationCaseSensitive = true;
    bool               m_l1NavigationWholeWord = false;
    bool               m_l1NavigationHitValid = false;
    quint64            m_l1NavigationRevision = 0;
    L1SearchHit        m_l2NavigationHit;
    QString            m_l2NavigationPattern;
    bool               m_l2NavigationRegex = false;
    bool               m_l2NavigationCaseSensitive = true;
    bool               m_l2NavigationWholeWord = false;
    bool               m_l2NavigationHexBytes = false;
    TextEncoding       m_l2NavigationEncoding = TextEncoding::Utf8;
    bool               m_l2NavigationByteContext = false;
    bool               m_l2NavigationHitValid = false;
    quint64            m_l2NavigationRevision = 0;
    qint64             m_l1FindAllTaskId = 0;
    QString            m_lastSearchError;
    QString            m_l2SearchCachePattern;
    bool               m_l2SearchCacheRegex = false;
    bool               m_l2SearchCacheCaseSensitive = true;
    bool               m_l2SearchCacheWholeWord = false;
    bool               m_l2SearchCacheHexBytes = false;
    TextEncoding       m_l2SearchCacheEncoding = TextEncoding::Utf8;
    bool               m_l2SearchCacheByteContext = false;
    bool               m_l2SearchCacheValid = false;
    quint64            m_l2SearchCacheRevision = 0;
    bool               m_lastFindAllTruncated = false;

    // findAllL2Async 的内部上下文：避免同时启动多个搜索
    struct PendingL2Search {
        bool active = false;
        QString pattern;
        bool regex = false;
        bool caseSensitive = true;
        bool wholeWord = false;
        bool hexBytes = false;
        TextEncoding encodingAtStart = TextEncoding::Utf8;
        bool byteContextModeAtStart = false;
        bool bytePositionModeAtStart = false;
        bool tolerateInvalidEncodingAtStart = false;
        QString sourcePathAtStart;
        qint64 sourceSizeAtStart = -1;
        DiskFileVersion diskVersionAtStart;
        quint64 contentRevision = 0;
        QPointer<QObject> parentForModel;
        QMetaObject::Connection waitLineBuildConn;
        QMetaObject::Connection waitMapFailedConn;
        QMetaObject::Connection searchProgressConn;
        QMetaObject::Connection searchPartialConn;
        QMetaObject::Connection searchFinishedConn;
        QMetaObject::Connection searchFailedConn;
        int previewMatchesPublished = 0;
    };
    PendingL2Search m_pendingL2Search;

    // 确保 VirtualFileView 存在（懒创建）；同时把 m_l2View 加入 m_stack。
    // 不依赖固定索引：Scintilla 在 L2 期间会被 reparent，Qt 会重排 stack 页面。
    VirtualFileView *ensureL2View();
    WindowedReadOnlyProvider *ensureWindowedReadOnlyProvider();
    bool windowedReadOnlyActive() const;
    // findAllL2Async 的内部启动：LineIndex 已完整 → 实际调 LargeFileSearch::findAllViaTaskManager
    void startPendingL2Search();
    bool pendingL2SearchSourceIsCurrent();
    bool validateSearchSource();
    // 清理 m_pendingL2Search 所有连接并重置为 inactive
    void resetPendingL2Search();
    void advanceContentRevision();
    void cancelPendingL2SearchForContentChange();
    void invalidateL2ProjectionSearchState();
    bool l2ProjectionMatches(TextEncoding encoding,
                             bool byteContextMode) const;

    // ============================================================================
    //  L2ScintillaBridge —— VFV 不做任何绘制；Scintilla overlay 盖满 VFV viewport，
    //  是唯一 UI（唯一渲染/输入/IME/行号/高亮/自动换行/拖选）。
    //  这里只做：
    //    (1) 桥 1 下拉：activeWindowChanged(start,end) → 读窗口字节 → 灌 Scintilla；
    //    (2) 桥 2 上推 flush：切窗口/保存前把 Scintilla 当前文本 diff 回 VFV patches；
    //    (3) 桥 3 双向滚动同步 debounce：SCN_UPDATEUI → VFV scrollbars / syncActiveWindowToCursor。
    // ============================================================================
    struct L2ScintillaBridge {
        QByteArray baseline;        // 上次 setText 的窗口 bytes（用于桥 2 diff）
        QVector<qint32> projectedToSource; // UTF-8 projection byte -> source byte
        QVector<qint32> sourceBoundaries;  // source byte boundaries
        QVector<qint32> projectedBoundaries; // matching UTF-8 boundaries
        qint64    windowStart = -1; // 对应窗口的全局起始字节（-1 无）
        qint64    windowLen   = 0;  // 窗口长度（= W_active）
        QTimer    syncVfvDebounce;  // 桥 3 Scintilla→VFV 防抖（250ms single-shot）
        bool      applying = false; // 桥 1/3 灌 Scintilla 时抑制 SCN_UPDATEUI 递归
        bool      installingText = false; // 同步 setText 事务；绝不能算作源字节修改
        bool      viewportNavigationPending = false; // 仅真实编辑器导航可反向更新全局视口
        quint64   projectionGeneration = 0; // 丢弃上一轮 setText 遗留的延迟 UI 通知
    };
    L2ScintillaBridge m_bridge;
    bool m_l2BridgeConnectionsInstalled = false;
    QPlainTextEdit *m_hexAsciiPreview = nullptr;
    int m_hexBytesPerRow = 16;
    bool m_hexOverwriteMode = true;
    bool m_adjustingHexCaret = false;
    bool m_adjustingAsciiCaret = false;
    HexMouseSelectionController m_hexMouseSelection;
    int m_hexWheelAngleRemainder = 0;
    int m_hexWheelPixelRemainder = 0;
    QVector<QPair<qint64, qint32>> m_l2SearchHighlights;


    // ---- L2ScintillaBridge 内部辅助 ----
    // 桥 1：读 VFV [s,e)（应用 patch）→ Scintilla setText；记录 baseline
    void bridgePopulateScintilla(qint64 s, qint64 e);
    // 桥 2：把 Scintilla 当前文本 vs baseline 做 diff → 写 VFV patches()。s/e 同 baseline 窗口。
    bool bridgeFlushOverlayToPatches(QString* errMsg = nullptr);
    qint64 bridgeSourceOffsetForProjectedPosition(qint64 position,
                                                  bool endBoundary = false) const;
    long bridgeProjectedPositionForSourceOffset(qint64 sourceOffset) const;
    int preferredHexBytesPerRow() const;
    void adaptHexBytesPerRow(bool repopulate);
    void alignTextOverlayToGlobalScroll();
    void applyL2SearchHighlightsToOverlay();
    bool updateFixedHexProjection(qint64 globalStart, qint32 oldLength,
                                  const QByteArray &replacement);
    bool replaceHexAsciiBytes(qint64 from, qint64 to, QByteArray bytes,
                              QString *error = nullptr);
    // 桥 3：syncVfvDebounce 超时回调 → 取 Scintilla 中心 → 换算全局字节 → syncActiveWindowToCursor
    void bridgeOnSyncDebounce();
    // 切回 L1 / closeEvent 时，把 overlay Scintilla 从 VFV viewport 上拆回 m_stack
    void bridgeReparentEditorToStack();
};
