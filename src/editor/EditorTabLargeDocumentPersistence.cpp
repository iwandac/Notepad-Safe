#include "EditorTab.h"
#include "EditorSurfaceStyle.h"
// Hex character rows use the shared measured Scintilla-authoritative grid.
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"
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
#include <QSaveFile>
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

namespace {

constexpr char LargeRecoveryMagic[] = "NotepadSafe.LargeEditJournal.v1";
constexpr char BinaryRecoveryMagic[] = "NotepadSafe.BinaryPatchJournal.v1";
constexpr quint32 BinaryRecoveryVersion = 1;

bool replaceFileAtomically(const QString &temporaryPath, const QString &targetPath,
                           QString *error)
{
#ifdef Q_OS_WIN
    const std::wstring temporary = QDir::toNativeSeparators(temporaryPath).toStdWString();
    const std::wstring target = QDir::toNativeSeparators(targetPath).toStdWString();
    const BOOL replaced = QFileInfo::exists(targetPath)
        ? ::ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
        : ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    if (replaced)
        return true;
    if (error) {
        *error = QObject::tr("Windows 原子替换失败（错误 %1）；恢复副本保留在 %2")
                     .arg(::GetLastError()).arg(temporaryPath);
    }
    return false;
#else
    QFile input(temporaryPath);
    QSaveFile output(targetPath);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        if (error) *error = QObject::tr("无法打开原子替换文件。");
        return false;
    }
    QByteArray chunk;
    while (!(chunk = input.read(4 * 1024 * 1024)).isEmpty()) {
        if (output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            if (error) *error = QObject::tr("原子替换写入不完整。");
            return false;
        }
    }
    if (input.error() != QFileDevice::NoError || !output.commit()) {
        if (error) *error = QObject::tr("原子替换提交失败。");
        return false;
    }
    QFile::remove(temporaryPath);
    return true;
#endif
}

} // namespace

// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================

// Large-document open, export and recovery persistence routes.
bool EditorTab::openLargeFileL2(const QString &path, QString *errMsg,
                                L2Presentation presentation,
                                L2ContentKind contentKind,
                                std::optional<TextEncoding> forcedEncoding)
{
    setSplitView(false);
    // S2 promotion is deliberately disabled: all mapped documents >= 32 MiB
    // use the same read-only provider as S3.
    const bool inheritedWordWrap = m_editor
        && m_editor->wrapMode() != QsciScintilla::WrapNone;
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (errMsg) *errMsg = QObject::tr("文件不存在：%1").arg(path);
        return false;
    }
    const bool binaryHex = presentation == L2Presentation::Hex;
    m_documentState.contentKind = contentKind == L2ContentKind::Binary
        ? DocumentContentKind::Binary : DocumentContentKind::Text;
    if (isBinaryDocument()) {
        // Binary and mixed-byte documents have one predictable default text
        // projection: UTF-8 multibyte decoding with U+FFFD for malformed
        // units. A previous document's selected codec must never leak into a
        // newly opened binary source.
        m_encoding = forcedEncoding.value_or(TextEncoding::Utf8);
        m_encodingOrigin = forcedEncoding ? EncodingOrigin::UserSelected
                                          : EncodingOrigin::Fallback;
        m_encodingConfidence = forcedEncoding ? EncodingConfidence::Certain
                                               : EncodingConfidence::Low;
        m_encodingDecodeErrors = true;
    } else {
        if (forcedEncoding) {
            m_encoding = *forcedEncoding;
            m_encodingOrigin = EncodingOrigin::UserSelected;
            m_encodingConfidence = EncodingConfidence::Certain;
            // Large files are validated incrementally by conversion. Opening
            // remains bounded and does not pretend that a sample proves the
            // entire byte stream.
            m_encodingDecodeErrors = false;
        } else {
            const EncodingDetection detection = detectFileEncoding(path);
            m_encoding = detection.encoding;
            m_encodingOrigin = detection.origin;
            m_encodingConfidence = detection.confidence;
            m_encodingDecodeErrors = detection.decodeErrors;
        }
    }
    const FileTierDecision tier = FileTierPolicy::classify(fi.size());
    const bool readOnlyByPolicy = tier.isReadOnlyByPolicy();
    const bool useWindowedReadOnly = readOnlyByPolicy;

    VirtualFileView *v = ensureL2View();
    v->setFullLineIndexBuildEnabled(!useWindowedReadOnly && !isBinaryDocument());
    v->setWordWrap(readOnlyByPolicy ? false : inheritedWordWrap);
    setSecurePayloadLimitExclusive(0);
    // Select the representation before openFile starts emitting progress.  A
    // binary document never needs the text line-index pipeline, even when it
    // is only a few bytes long and is routed through the mmap/Hex editor.
    v->setViewMode(binaryHex ? ViewMode::Hex : ViewMode::Text);
    v->setDisplayEncoding(m_encoding);
    // 确保 VirtualFileView 已加入 stack（ensureL2View 会 addWidget 到 index=1）
    QString localErr;
    if (!useWindowedReadOnly && !v->openFile(path, &localErr)) {
        if (errMsg) *errMsg = localErr;
        return false;
    }
    // 成功：切到 L2 视图并标记模式
    m_documentState.location = fi.absoluteFilePath();
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
        WindowedReadOnlyProvider *provider = ensureWindowedReadOnlyProvider();
        provider->setBinaryContent(isBinaryDocument());
        QString providerError;
        const auto providerPresentation = binaryHex
            ? WindowedReadOnlyProvider::Presentation::Hex
            : WindowedReadOnlyProvider::Presentation::Text;
        if (!provider->openFile(path, providerPresentation, &providerError)) {
            if (v->isOpen())
                v->closeFile(true);
            m_l2Mode = false;
            if (errMsg)
                *errMsg = providerError;
            return false;
        }
        provider->setTextEncoding(m_encoding);
        if (!binaryHex) {
            provider->setWordWrap(inheritedWordWrap);
        }
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

bool EditorTab::saveL2As(const QString &path, QString *errMsg)
{
    if (!m_l2Mode || !m_l2View) {
        if (errMsg) *errMsg = QObject::tr("当前非 L2 模式");
        return false;
    }
    if (isReadOnlyByPolicy()) {
        if (errMsg) {
            *errMsg = QObject::tr(
                "32 MiB 以上文件为只读分析模式，不能保存或另存编辑结果。");
        }
        return false;
    }
    if (sameDiskFilePath(m_documentState.location, path)) {
        const ExternalFileState diskState = externalFileState();
        if (diskState == ExternalFileState::Changed
            || diskState == ExternalFileState::Missing
            || diskState == ExternalFileState::Unreadable) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "磁盘文件在打开后已变化或当前不可读取。为避免覆盖外部内容，保存已停止。");
            }
            return false;
        }
    }
    // 以下 patch 保存路径只服务 32 MiB 以下的 B1 兼容 Hex 编辑。
    // 上面的策略门禁保证 S2/S3 永远不能进入这里。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        if (errMsg) *errMsg = QObject::tr("保存前回写窗口修改失败：%1").arg(flushErr);
        return false;
    }
    std::atomic<bool> cancel{false};
    QString localErr;

    // 判断是否保存到当前 mmap 映射的同一文件
    const bool sameFile = !m_documentState.location.isEmpty() &&
        QFileInfo(path).absoluteFilePath() == QFileInfo(m_documentState.location).absoluteFilePath();

    if (sameFile
        && m_l2View->virtualFileSize() < FileTierPolicy::StandardEditLimitBytes) {
        // B1 is deliberately bounded below 32 MiB. Materialize the authoritative
        // byte model once, write through QSaveFile, release the source mapping
        // only for the atomic commit, then reopen. This removes the legacy
        // user-visible ".notepad-safe-save-tmp" copy used by the large-file
        // streaming path while retaining crash-safe replacement semantics.
        QByteArray snapshot = m_l2View->readByteRange(
            0, m_l2View->virtualFileSize());
        if (snapshot.size() != m_l2View->virtualFileSize()) {
            if (errMsg)
                *errMsg = QObject::tr("无法生成完整的 Binary 保存快照。");
            if (!snapshot.isEmpty()) snapshot.fill(0);
            return false;
        }
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)
            || output.write(snapshot) != snapshot.size()) {
            if (errMsg)
                *errMsg = QObject::tr("无法写入 Binary 原子保存数据：%1")
                              .arg(output.errorString());
            output.cancelWriting();
            if (!snapshot.isEmpty()) snapshot.fill(0);
            return false;
        }
        const ViewMode presentation = m_l2View->viewMode();
        m_l2View->closeFile();
        if (!output.commit()) {
            const QString commitError = output.errorString();
            QString reopenError;
            m_l2View->setViewMode(presentation);
            m_l2View->openFile(path, &reopenError);
            if (errMsg)
                *errMsg = QObject::tr("Binary 原子保存失败：%1").arg(commitError);
            if (!snapshot.isEmpty()) snapshot.fill(0);
            return false;
        }
        if (!snapshot.isEmpty()) snapshot.fill(0);
        QString reopenError;
        m_l2View->setViewMode(presentation);
        if (!m_l2View->openFile(path, &reopenError)) {
            if (errMsg)
                *errMsg = QObject::tr("保存成功但重新打开失败：%1").arg(reopenError);
            return false;
        }
        m_documentState.location = QFileInfo(path).absoluteFilePath();
    } else if (sameFile) {
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
        // 原文件已解锁，用平台原子替换，禁止“先删除原文件再复制”的数据丢失窗口。
        QString replaceError;
        if (!replaceFileAtomically(tempPath, path, &replaceError)) {
            if (errMsg) *errMsg = replaceError;
            QString reopenErr;
            m_l2View->openFile(path, &reopenErr);
            return false;
        }
        // 重新映射已保存的文件
        QString reopenErr;
        ok = m_l2View->openFile(path, &reopenErr);
        if (!ok) {
            if (errMsg) *errMsg = QObject::tr("保存成功但重新打开失败：%1").arg(reopenErr);
            return false;
        }
        m_documentState.location = path;  // sameFile 路径也同步（openFile 可能没更新 EditorTab::m_documentState.location，确保 tab 标题/后续状态正确）
    } else {
        // 不同文件：先流式保存，再把新文件切成当前 backing store。否则后续编辑
        // 仍依赖旧文件 mmap，旧文件被移动/修改后会破坏“另存为”文档。
        bool ok = m_l2View->saveAs(path, {}, &cancel, &localErr);
        if (!ok) {
            if (errMsg) *errMsg = localErr;
            return false;
        }
        m_l2View->closeFile();
        QString reopenErr;
        ok = m_l2View->openFile(path, &reopenErr);
        if (!ok) {
            if (errMsg)
                *errMsg = QObject::tr("文件已另存为，但无法切换到新文件：%1").arg(reopenErr);
            return false;
        }
        m_documentState.location = QFileInfo(path).absoluteFilePath();
    }

    setModified(false);
    clearAutosaveFile();
    m_autosavePath.clear();
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
    // Saving a mapped document closes and reopens its provider. During the
    // short closed interval the projection length is not the payload length;
    // refresh permanent UI only after the authoritative provider is open.
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    return true;
}

// ============================================================================
//  会话专用：exportToFile（零副作用写盘）+ openAutosaveAutoMode（L1/L2 恢复分流）
// ============================================================================

bool EditorTab::exportToFile(const QString &destPath) const
{
    if (isL2Mode() && m_l2View && m_l2View->isOpen()) {
        QString error;
        std::atomic<bool> cancel{false};
        auto *view = const_cast<VirtualFileView *>(m_l2View);
        return view->saveAs(destPath, {}, &cancel, &error);
    }


    QSaveFile output(destPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    QString text = m_editor->text();
    QByteArray data = encodeText(text, m_encoding);
    const bool complete = output.write(data) == data.size();
    if (!text.isEmpty())
        text.fill(QChar(0));
    if (!data.isEmpty())
        data.fill(0);
    if (!complete) {
        output.cancelWriting();
        return false;
    }
    return output.commit();
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
        || !isL2Mode() || !m_l2View || m_documentState.location.isEmpty() || isReadOnlyByPolicy()) {
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
    if (externalFileState() != ExternalFileState::Unchanged || !m_diskVersion.valid) {
        if (error) *error = QObject::tr("原始二进制文件已经变化，不能建立可安全重放的恢复日志。");
        return false;
    }

    QDataStream stream(output);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QByteArray(BinaryRecoveryMagic, sizeof(BinaryRecoveryMagic))
           << BinaryRecoveryVersion << m_diskVersion.size << m_diskVersion.modifiedMs
           << m_diskVersion.fileIdentity << m_diskVersion.sampleHash;
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
            m_documentState.location = realFilePath;
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

    m_documentState.location = realFilePath;
    refreshDiskVersionBaseline();
    m_autosavePath = autosavePath;
    m_l2SaveNeeded = true;
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
    emit l2SaveNeededChanged(true);
    return true;
}
