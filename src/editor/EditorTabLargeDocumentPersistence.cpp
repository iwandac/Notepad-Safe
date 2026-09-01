#include "EditorTab.h"
#include "EditorSurfaceStyle.h"
// Hex character rows use the shared measured Scintilla-authoritative grid.
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
#include "EditorSearchSource.h"
#include "FileTierPolicy.h"
#include "SearchResultsModel.h"
#include "Pcre2Regex.h"
#include "EditableTextSearch.h"
#include "AsyncTaskManager.h"
#include "DraftStore.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDataStream>
#include <QFont>
#include <QColor>
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
#include <QPointer>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QApplication>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QSplitter>

#include "Sci_Position.h"
#include <QBuffer>
#include <algorithm>
#include <limits>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>


#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerjava.h>

#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexerbatch.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexermakefile.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexerperl.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerproperties.h>
#include <Qsci/qscilexermasm.h>

SessionTab captureEditorSessionTab(const EditorTab *tab)
{
    SessionTab state;
    if (!tab)
        return state;
    state.filePath = tab->filePath();
    state.encoding = encodingName(tab->encoding());
    state.syntax = static_cast<int>(tab->syntax());
    tab->getCursorPosition(&state.cursorLine, &state.cursorIndex);
    if (tab->isL2Mode() && tab->l2View()) {
        state.cursorByteOffset = tab->currentByteOffset();
        state.selectionStartByte = tab->l2View()->selectionStart();
        state.selectionEndByte = tab->l2View()->selectionEnd();
        state.presentation = tab->viewModeL2() == ViewMode::Hex ? QStringLiteral("hex") : QStringLiteral("text");
        state.hexInsertMode = state.presentation == QLatin1String("hex") && !tab->hexOverwriteMode();
    } else if (tab->editor()) {
        state.cursorByteOffset = tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        state.selectionStartByte = tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        state.selectionEndByte = tab->editor()->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        state.presentation = QStringLiteral("text");
    }
    state.modified = tab->isModified();
    state.splitView = tab->splitViewEnabled();
    if (state.splitView) {
        state.splitOrientation = tab->splitOrientation() == Qt::Vertical
            ? QStringLiteral("vertical") : QStringLiteral("horizontal");
    }
    state.secureDocument = tab->isSecureDocument();
    if (state.secureDocument)
        state.documentId = tab->secureDocumentId().toString(QUuid::WithoutBraces);
    state.autosavePath = state.secureDocument
        ? tab->secureBackingPath() : tab->autosavePath();
    return state;
}

bool EditorTab::captureRecoveryPayload(SessionRecoveryPayload *payload,
                                       QString *error)
{
    if (!payload) {
        if (error)
            *error = tr("恢复载荷目标不可用。");
        return false;
    }
    payload->bytes.clear();
    payload->encoding = int(encoding());
    if (isBinaryDocument()) {
        payload->kind = SessionRecoveryKind::BinaryPatchJournal;
        QBuffer buffer(&payload->bytes);
        buffer.open(QIODevice::WriteOnly);
        return exportBinaryRecoveryJournal(&buffer, error);
    }
    payload->kind = SessionRecoveryKind::SmallOrJournal;
    if (exportRecoveryPayload(&payload->bytes, error))
        return true;
    if (!isLargeDocument() || isL2Mode())
        return false;
    payload->bytes.clear();
    payload->kind = SessionRecoveryKind::FullCheckpoint;
    QBuffer buffer(&payload->bytes);
    buffer.open(QIODevice::WriteOnly);
    return exportRecoveryCheckpoint(&buffer, error);
}

bool EditorTab::restoreSessionRecovery(const SessionRestoreRequest &request,
                                       QString *error)
{
    bool loaded = false;
    if (request.legacyPlaintextFile) {
        loaded = openAutosaveAutoMode(request.encryptedPath,
                                      request.realFilePath);
    } else if (request.kind == SessionRecoveryKind::BinaryPatchJournal) {
        loaded = restoreBinaryRecoveryJournal(
            request.encryptedPath, request.realFilePath, error);
    } else if (request.kind == SessionRecoveryKind::FullCheckpoint) {
        loaded = restoreRecoveryCheckpoint(
            request.encryptedPath, request.realFilePath,
            TextEncoding(request.encoding), request.plaintextBytes, error);
    } else {
        loaded = restoreRecoveryPayload(request.plaintext,
                                        request.realFilePath, error);
    }
    if (loaded && !request.realFilePath.isEmpty()) {
        setFilePath(request.realFilePath);
        setModified(true);
    }
    return loaded;
}

namespace {

constexpr char LargeRecoveryMagic[] = "NotepadSafe.LargeEditJournal.v1";
constexpr char BinaryRecoveryMagic[] = "NotepadSafe.BinaryPatchJournal.v1";
constexpr quint32 BinaryRecoveryVersion = 1;


} // namespace

// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================

// Large-document open, export and recovery persistence routes.
bool EditorTab::openLargeFileL2(const QString &path, QString *errMsg,
                                L2Presentation presentation,
                                L2ContentKind contentKind,
                                std::optional<TextEncoding> forcedEncoding,
                                const PersistenceReadPolicy &readPolicy,
                                bool requireReadOnly)
{
    if (!m_searchSource->closeAndOwnerAlive()) return false;
    setSplitView(false);
    const bool inheritedWordWrap = m_editor
        && m_editor->wrapMode() != QsciScintilla::WrapNone;
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (errMsg) *errMsg = QObject::tr("文件不存在：%1").arg(path);
        return false;
    }
    const bool binaryHex = presentation == L2Presentation::Hex;
    const bool binaryContent = contentKind == L2ContentKind::Binary;
    const bool readOnlyByPolicy = requireReadOnly
        || FileTierPolicy::classify(fi.size()).isReadOnlyByPolicy();
    const bool useWindowedReadOnly = readOnlyByPolicy;
    VirtualFileView *v = ensureL2View();
    WindowedReadOnlyProvider *provider = nullptr;
    EncodingDetection admittedDetection;
    if (useWindowedReadOnly) {
        provider = ensureWindowedReadOnlyProvider();
        provider->setBinaryContent(binaryContent);
        const auto providerPresentation = binaryHex
            ? WindowedReadOnlyProvider::Presentation::Hex
            : WindowedReadOnlyProvider::Presentation::Text;
        const QPointer<EditorTab> owner(this);
        const bool opened = provider->openFile(path, providerPresentation, readPolicy,
                                               errMsg, forcedEncoding, &admittedDetection);
        if (!owner) return false;
        if (!opened) {
            m_l2Mode = false;
            if (v->isOpen()) v->closeFile(true);
            return false;
        }
    }
    m_documentSession.setContentKind(contentKind == L2ContentKind::Binary
        ? DocumentContentKind::Binary : DocumentContentKind::Text);
    if (forcedEncoding || isBinaryDocument()) {
        // Binary and mixed-byte documents have one predictable default text
        // projection: UTF-8 multibyte decoding with U+FFFD for malformed
        // units. A previous document's selected codec must never leak into a
        // newly opened binary source.
        m_encoding = forcedEncoding.value_or(TextEncoding::Utf8);
        m_encodingOrigin = forcedEncoding ? EncodingOrigin::UserSelected
                                          : EncodingOrigin::Fallback;
        m_encodingConfidence = forcedEncoding ? EncodingConfidence::Certain
                                               : EncodingConfidence::Low;
        m_encodingDecodeErrors = isBinaryDocument();
    } else {
        const EncodingDetection detection = useWindowedReadOnly
            ? admittedDetection : detectFileEncoding(path);
        m_encoding = detection.encoding;
        m_encodingOrigin = detection.origin;
        m_encodingConfidence = detection.confidence;
        m_encodingDecodeErrors = detection.decodeErrors;
    }
    v->setFullLineIndexBuildEnabled(!useWindowedReadOnly && !isBinaryDocument());
    v->setWordWrap(readOnlyByPolicy ? false : inheritedWordWrap);
    setSecurePayloadLimitExclusive(0);
    // Select representation before opening; binary needs no text line index.
    v->setViewMode(binaryHex ? ViewMode::Hex : ViewMode::Text);
    v->setDisplayEncoding(m_encoding);
    // 确保 VirtualFileView 已加入 stack（ensureL2View 会 addWidget 到 index=1）
    if (!useWindowedReadOnly && !v->openFile(path, errMsg)) {
        return false;
    }
    // 成功：切到 L2 视图并标记模式
    m_documentSession.setLocation(fi.absoluteFilePath());
    if (useWindowedReadOnly)
        m_documentSession.setDiskVersion(provider->sourceVersion());
    else
        refreshDiskVersionBaseline();
    m_l2Mode = true;
    // S2 and S3 intentionally expose the same capability: bounded mmap browse,
    // search and copy, with every mutation command disabled. The size tier is
    // retained only for diagnostics and possible future experiments.
    m_l2ReadOnlyByPolicy = readOnlyByPolicy;
    m_l2SaveNeeded = false;
    m_lastL2Line = 0;
    m_lastL2Col  = 0;
    m_lastL2Byte = 0;
    if (useWindowedReadOnly) {
        m_stack->setCurrentWidget(provider);
        provider->editor()->setFocus();
    } else {
        // Only sub-32-MiB B1/secure-binary compatibility paths can reach the
        // legacy editable overlay. S2/S3 and B2/B3 never do.
        activateL2Overlay(v);
    }

    emit l2ModeStatusChanged();
    emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(), {});

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
    if (isReadOnlyByPolicy())
        emit modificationChanged(false);
    return true;
}


bool EditorTab::exportSnapshotBytes(QByteArray *data) const
{
    if (!data || isLargeDocument())
        return false;
    QString text = m_editor->text();
    *data = encodeText(text, m_encoding);
    if (!text.isEmpty())
        text.fill(QChar(0));
    return data->size() < FileTierPolicy::StandardEditLimitBytes;
}

bool EditorTab::exportRecoveryPayload(QByteArray *data, QString *error) const
{
    if (exportSnapshotBytes(data))
        return true;
    if (error)
        *error = QObject::tr("只读分析文档不生成编辑恢复日志。");
    return false;
}

bool EditorTab::restoreRecoveryPayload(const QByteArray &data,
                                       const QString &realFilePath,
                                       QString *error)
{
    Q_UNUSED(realFilePath)
    QDataStream stream(data);
    stream.setByteOrder(QDataStream::LittleEndian);
    QByteArray magic;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != QByteArray(LargeRecoveryMagic, sizeof(LargeRecoveryMagic)))
        return loadAutosaveBytes(data, QString());
    Q_UNUSED(version)
    if (error) {
        *error = QObject::tr(
            "旧版大文件编辑恢复日志已禁用；当前大文件只能以只读分析方式打开。");
    }
    return false;
}

bool EditorTab::exportRecoveryCheckpoint(QIODevice *output, QString *error) const
{
    Q_UNUSED(output)
    if (error)
        *error = QObject::tr("只读分析文档不生成编辑检查点。");
    return false;
}

bool EditorTab::restoreRecoveryCheckpoint(const QString &checkpointPath,
                                          const QString &realFilePath,
                                          TextEncoding encoding,
                                          qint64 plaintextBytes,
                                          QString *error)
{
    Q_UNUSED(checkpointPath)
    Q_UNUSED(realFilePath)
    Q_UNUSED(encoding)
    Q_UNUSED(plaintextBytes)
    if (error) {
        *error = QObject::tr(
            "旧版大文件编辑与后台完整加载已禁用，不能恢复旧的编辑检查点。");
    }
    return false;
}

bool EditorTab::exportBinaryRecoveryJournal(QIODevice *output, QString *error)
{
    if (!output || !output->isWritable() || !isBinaryDocument() || isSecureDocument()
        || !isL2Mode() || !m_l2View || m_documentSession.state().location.isEmpty() || isReadOnlyByPolicy()) {
        if (error) *error = QObject::tr("当前文档不能生成 Binary/Hex 恢复日志。");
        return false;
    }
    QString flushError;
    if (!bridgeFlushOverlayToPatches(&flushError)) {
        if (error) *error = flushError;
        return false;
    }
    if (!m_l2View->patches().isModified()) {
        if (error) *error = QObject::tr("Binary/Hex 文档没有需要恢复的编辑。");
        return false;
    }
    if (externalFileState() != ExternalFileState::Unchanged || !m_documentSession.diskVersion().valid) {
        if (error) *error = QObject::tr("原始二进制文件已经变化，不能建立可安全重放的恢复日志。");
        return false;
    }

    QDataStream stream(output);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QByteArray(BinaryRecoveryMagic, sizeof(BinaryRecoveryMagic))
           << BinaryRecoveryVersion << m_documentSession.diskVersion().size << m_documentSession.diskVersion().modifiedMs
           << m_documentSession.diskVersion().fileIdentity << m_documentSession.diskVersion().sampleHash;
    if (stream.status() != QDataStream::Ok) {
        if (error) *error = QObject::tr("写入 Binary/Hex 恢复元数据失败。");
        return false;
    }
    return m_l2View->patches().writeRecoveryJournal(output, error);
}

bool EditorTab::restoreBinaryRecoveryJournal(const QString &journalPath,
                                             const QString &realFilePath,
                                             QString *error)
{
    if (journalPath.isEmpty() || realFilePath.isEmpty()) {
        if (error) *error = QObject::tr("Binary/Hex 恢复路径无效。");
        return false;
    }
    QByteArray journal;
    constexpr qsizetype MaximumJournalBytes = 512 * 1024 * 1024;
    QString readError;
    const bool readOk = DraftStore::readChunks(
        journalPath,
        [&journal](const QByteArray &chunk, QString *consumerError) {
            if (journal.size() > MaximumJournalBytes - chunk.size()) {
                if (consumerError)
                    *consumerError = QObject::tr("Binary/Hex 恢复日志超过 512 MiB 安全上限。");
                return false;
            }
            journal.append(chunk);
            return true;
        }, &readError);
    if (!readOk) {
        if (error) *error = readError;
        if (!journal.isEmpty()) journal.fill(0);
        return false;
    }

    QBuffer buffer(&journal);
    buffer.open(QIODevice::ReadOnly);
    QByteArray magic;
    quint32 version = 0;
    qint64 baseSize = -1;
    qint64 baseModifiedMs = -1;
    QByteArray baseIdentity;
    QByteArray baseSampleHash;
    {
        QDataStream stream(&buffer);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream >> magic >> version >> baseSize >> baseModifiedMs
               >> baseIdentity >> baseSampleHash;
        if (stream.status() != QDataStream::Ok
            || magic != QByteArray(BinaryRecoveryMagic, sizeof(BinaryRecoveryMagic))
            || version != BinaryRecoveryVersion || baseSize < 0
            || baseModifiedMs < 0 || baseSampleHash.size() != 32) {
            if (error) *error = QObject::tr("Binary/Hex 恢复日志元数据已损坏。");
            journal.fill(0);
            return false;
        }
    }

    DiskFileVersion baseline;
    baseline.valid = true;
    baseline.absolutePath = QFileInfo(realFilePath).absoluteFilePath();
    baseline.size = baseSize;
    baseline.modifiedMs = baseModifiedMs;
    baseline.fileIdentity = baseIdentity;
    baseline.sampleHash = baseSampleHash;
    if (compareDiskFileVersion(realFilePath, baseline) != ExternalFileState::Unchanged) {
        if (error) *error = QObject::tr("原始二进制文件已移动或变化，不能安全重放恢复日志。");
        journal.fill(0);
        return false;
    }

    QString openError;
    if (!openLargeFileL2(realFilePath, &openError,
                         L2Presentation::Hex, L2ContentKind::Binary)) {
        if (error) *error = openError;
        journal.fill(0);
        return false;
    }
    if (isReadOnlyByPolicy()) {
        if (error) {
            *error = QObject::tr(
                "32 MiB 以上 Binary/Hex 文件为只读分析模式，不能恢复编辑日志。");
        }
        journal.fill(0);
        return false;
    }
    // This recovery branch is reachable only for editable B1 Hex. Its mmap is
    // initialized asynchronously, so wait for that bounded mapping before
    // installing the B1 PatchList. S2/S3/B2/B3 have already returned above.
    if (!m_l2View->isOpen()) {
        bool mapped = false;
        QEventLoop mapLoop;
        QTimer mapTimeout;
        mapTimeout.setSingleShot(true);
        const auto readyConnection = connect(
            m_l2View, &VirtualFileView::lineBuildFinished, &mapLoop,
            [this, &mapped, &mapLoop](bool ok) {
                mapped = ok && m_l2View && m_l2View->isOpen();
                mapLoop.quit();
            });
        connect(&mapTimeout, &QTimer::timeout, &mapLoop, &QEventLoop::quit);
        mapTimeout.start(30000);
        mapLoop.exec(QEventLoop::ExcludeUserInputEvents);
        disconnect(readyConnection);
        if (!mapped) {
            if (error)
                *error = QObject::tr("Binary/Hex 原文件映射超时或失败，恢复日志未应用。");
            journal.fill(0);
            return false;
        }
    }
    QString patchError;
    if (!m_l2View->patches().restoreRecoveryJournal(&buffer, &patchError)) {
        if (error) *error = patchError;
        journal.fill(0);
        return false;
    }
    journal.fill(0);
    journal.clear();

    m_l2SaveNeeded = true;
    clearL2BridgeBaseline();
    const qint64 start = m_l2View->activeWindowByteStart();
    const qint64 end = m_l2View->activeWindowByteEnd();
    if (end > start)
        bridgePopulateScintilla(start, end);
    advanceContentRevision();
    emit l2SaveNeededChanged(true);
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
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
            m_documentSession.setLocation(realFilePath);
            refreshDiskVersionBaseline();
            // 注意：loadAutosave 已经 setModified(true)，所以无需再调
            emit titleChanged(tabTitle());
        }
        return ok;
    }

    // ---- S2/S3 autosave：复用统一 mmap/Scintilla 状态机。 ----
    // mmap 失败时不能回退到主线程 readAll；恢复一个损坏或无法映射的 2 GiB
    // autosave 不应让整个应用在启动阶段失去响应。
    qDebug("[openAutosaveAutoMode] 选 S2/S3: autosaveSize=%lld KB，进入统一 mmap 打开路径",
           size / 1024);
    QString err;
    const bool ok = openLargeFileL2(autosavePath, &err);
    qDebug("[openAutosaveAutoMode] 统一 mmap 返回 ok=%d err='%s', 耗时=%lld ms",
           ok ? 1 : 0, qUtf8Printable(err), t.elapsed());
    if (!ok) {
        qWarning("[openAutosaveAutoMode] mmap 恢复失败，安全跳过且不做同步全量回退: %s",
                 qUtf8Printable(err));
        return false;
    }

    m_documentSession.setLocation(realFilePath);
    refreshDiskVersionBaseline();
    m_documentSession.setRecoveryPath(autosavePath);
    m_l2SaveNeeded = true;
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
    emit l2SaveNeededChanged(true);
    return true;
}
