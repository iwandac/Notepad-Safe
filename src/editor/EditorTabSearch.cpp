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

// ============================================================================
//  搜索分流：L1=PCRE2 内存快照；L2=PCRE2 mmap/patch 快照
// ============================================================================

namespace {  // 匿名：L2 findAll 同步等待 helper
constexpr qint64 L1InlineNavigationBudgetBytes = 256LL * 1024;

struct L2SearchAwaiter {
    QEventLoop loop;
    bool       done = false;
    qint64     total = 0;
    QString    err;
};

} // namespace

SearchResultsModel* EditorTab::findAllL2(const QString& pattern, bool regex,
                                          bool caseSensitive, bool wholeWord,
                                          QObject* parentForModel,
                                          bool hexBytes)
{
    m_lastSearchError.clear();
    if (!isL2Mode() || pattern.isEmpty())
        return nullptr;
    // Production S2/S3/B2/B3 uses WindowedReadOnlyProvider for presentation
    // and deliberately does not create the hidden mmap search backend while
    // opening. ensureWindowedSearchSource() owns that lazy initialization.
    if (m_pendingL2Search.active)
        return nullptr;

    QString sourceError;
    if (!ensureWindowedSearchSource(&sourceError)) {
        m_lastSearchError = sourceError;
        return nullptr;
    }

    // The production windowed provider opens immediately, while its hidden
    // compatibility search mapping is installed asynchronously.  Find Next
    // still uses this synchronous facade, so keep the GUI event loop alive
    // until the accepted mapping reaches a usable state instead of searching
    // a null subject or returning a false zero result.
    if (!m_l2View || !m_l2View->isOpen()) {
        bool mappingReady = false;
        bool mappingFailed = false;
        QEventLoop mappingLoop;
        QTimer mappingTimeout;
        mappingTimeout.setSingleShot(true);
        const QMetaObject::Connection openedConnection = connect(
            m_l2View, &VirtualFileView::fileOpenedChanged, &mappingLoop,
            [&](bool opened) {
                if (opened && m_l2View && m_l2View->isOpen()) {
                    mappingReady = true;
                    mappingLoop.quit();
                }
            });
        const QMetaObject::Connection failedConnection = connect(
            m_l2View, &VirtualFileView::lineBuildFinished, &mappingLoop,
            [&](bool success) {
                if (!success && (!m_l2View || !m_l2View->isOpen())) {
                    mappingFailed = true;
                    mappingLoop.quit();
                }
            });
        connect(&mappingTimeout, &QTimer::timeout,
                &mappingLoop, &QEventLoop::quit);
        mappingTimeout.start(120000);
        if (!m_l2View || !m_l2View->isOpen())
            mappingLoop.exec();
        QObject::disconnect(openedConnection);
        QObject::disconnect(failedConnection);
        mappingTimeout.stop();
        mappingReady = mappingReady || (m_l2View && m_l2View->isOpen());
        if (!mappingReady) {
            m_lastSearchError = mappingFailed
                ? QObject::tr("无法建立大文件搜索映射。")
                : QObject::tr("准备大文件搜索超时，请重试。");
            return nullptr;
        }
    }

    QString flushError;
    if (!bridgeFlushOverlayToPatches(&flushError)) {
        m_lastSearchError = flushError;
        if (auto *mw = qobject_cast<QMainWindow*>(this->window()))
            mw->statusBar()->showMessage(QObject::tr("无法准备搜索快照：%1").arg(flushError), 5000);
        return nullptr;
    }
    const quint64 searchRevision = m_documentSession.state().revision;
    const TextEncoding searchEncoding = displayEncoding();
    const bool searchByteContext = isHexPresentation();
    const bool searchBytePosition = isBinaryDocument() || searchByteContext;

    // Byte offsets are authoritative. An incomplete LineIndex may postpone the
    // row label, but must never postpone scanning or exact navigation.

    LargeFileSearch *s = m_l2View->searchEngine();  // 懒创建 + setSource 已内部完成
    if (!s) return nullptr;

    LargeFileSearch::Options opts;
    opts.regexMode     = regex;
    opts.hexBytesMode  = hexBytes;
    opts.byteContextMode = searchByteContext;
    opts.tolerateInvalidEncoding = !opts.byteContextMode
        && (isBinaryDocument() || encodingHasDecodeErrors());
    opts.caseSensitive = hexBytes ? true : caseSensitive;
    opts.wholeWord     = hexBytes ? false : wholeWord;
    opts.textEncoding  = searchEncoding;

    // 非模态：进度显示在 MainWindow 状态栏（若有）；不再弹 WindowModal QProgressDialog
    auto *mw = qobject_cast<QMainWindow*>(this->window());
    L2SearchAwaiter aw;
    QMetaObject::Connection c1 = QObject::connect(s, &LargeFileSearch::finished,
        &aw.loop,
        [&](qint64 total){ aw.done = true; aw.total = total;
            if (mw) mw->statusBar()->showMessage(QObject::tr("搜索完成：共 %1 处匹配").arg(total), 3000);
            aw.loop.quit(); });
    QMetaObject::Connection c2 = QObject::connect(s, &LargeFileSearch::failed,
        &aw.loop,
        [&](const QString& msg){ aw.done = true; aw.err = msg;
            if (mw) mw->statusBar()->showMessage(QObject::tr("搜索失败：%1").arg(msg), 5000);
            aw.loop.quit(); });
    QMetaObject::Connection c3 = QObject::connect(s, &LargeFileSearch::progress,
        &aw.loop,
        [&](qint64 done, qint64 total){
            if (!mw || total <= 0) return;
            int pct = int((done * 100LL) / total);
            mw->statusBar()->showMessage(QObject::tr("正在搜索大文件… %1%").arg(pct), 0);
        });
    // 取消入口：暂由 statusBar 提供（后续 AsyncTaskPanel 统一取消按钮）；此处保留接口但不连 QProgressDialog
    Q_UNUSED(mw);

    s->findAllViaTaskManager(pattern, opts, fileName());
    if (!aw.done)
        aw.loop.exec();

    QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
    if (m_documentSession.state().revision != searchRevision)
        return nullptr;
    if (!aw.err.isEmpty() || aw.total <= 0) {
        // 0 匹配也返回空的 model（Dock 可以显示 0 结果）
    }
    auto *model = new SearchResultsModel(parentForModel);
    model->setBytePositionMode(searchBytePosition);
    model->setTextEncoding(searchEncoding);
    model->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount(),
                      s->overrunCheckpoints(), s->overrunGroupedCount(), 2);
    if (!aw.err.isEmpty())
        model->setIncompleteReason(aw.err);
    m_lastSearchError = aw.err;
    m_l2SearchCachePattern = pattern;
    m_l2SearchCacheRegex = regex;
    m_l2SearchCacheCaseSensitive = caseSensitive;
    m_l2SearchCacheWholeWord = wholeWord;
    m_l2SearchCacheHexBytes = hexBytes;
    m_l2SearchCacheEncoding = searchEncoding;
    m_l2SearchCacheByteContext = searchByteContext;
    m_l2SearchCacheRevision = m_documentSession.state().revision;
    m_l2SearchCacheValid = aw.err.isEmpty();
    m_l2SearchHighlights.clear();
    m_l2SearchHighlights.reserve(s->memoryMatches().size());
    for (const auto &match : s->memoryMatches())
        m_l2SearchHighlights.append(qMakePair(match.byteOffset, match.length));
    m_l2View->setSearchHighlights(m_l2SearchHighlights);
    applyL2SearchHighlightsToOverlay();
    return model;
}
// ============================================================================
//  findAllL2Async（纯异步：不阻塞 UI，信号回传结果）
// ============================================================================
void EditorTab::resetPendingL2Search()
{
    QObject::disconnect(m_pendingL2Search.waitLineBuildConn);
    QObject::disconnect(m_pendingL2Search.waitMapFailedConn);
    QObject::disconnect(m_pendingL2Search.searchProgressConn);
    QObject::disconnect(m_pendingL2Search.searchPartialConn);
    QObject::disconnect(m_pendingL2Search.searchFinishedConn);
    QObject::disconnect(m_pendingL2Search.searchFailedConn);
    m_pendingL2Search = PendingL2Search{};
}

void EditorTab::invalidateL2ProjectionSearchState()
{
    m_l2SearchCacheValid = false;
    m_l2NavigationHitValid = false;
    clearSearchHighlights();
}

bool EditorTab::l2ProjectionMatches(TextEncoding encoding,
                                    bool byteContextMode) const
{
    return displayEncoding() == encoding
        && isHexPresentation() == byteContextMode;
}

bool EditorTab::pendingL2SearchSourceIsCurrent() const
{
    if (!m_pendingL2Search.active
        || m_documentSession.state().revision != m_pendingL2Search.contentRevision
        || m_documentSession.state().location != m_pendingL2Search.sourcePathAtStart
        || contentSizeBytes() != m_pendingL2Search.sourceSizeAtStart) {
        return false;
    }
    if (m_pendingL2Search.diskVersionAtStart.valid
        && compareDiskFileVersion(m_pendingL2Search.sourcePathAtStart,
                                  m_pendingL2Search.diskVersionAtStart)
               != ExternalFileState::Unchanged) {
        return false;
    }
    return true;
}

void EditorTab::startPendingL2Search()
{
    if (!m_pendingL2Search.active || !m_l2View) {
        resetPendingL2Search();
        return;
    }
    if (!pendingL2SearchSourceIsCurrent()) {
        const QString pattern = m_pendingL2Search.pattern;
        const QString name = fileName();
        resetPendingL2Search();
        emit findAllL2Finished(nullptr, pattern, name);
        return;
    }
    QObject::disconnect(m_pendingL2Search.waitLineBuildConn);
    QObject::disconnect(m_pendingL2Search.waitMapFailedConn);
    m_pendingL2Search.waitLineBuildConn = QMetaObject::Connection{};
    m_pendingL2Search.waitMapFailedConn = QMetaObject::Connection{};
    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) {
        auto *empty = new SearchResultsModel(m_pendingL2Search.parentForModel);
        empty->setBytePositionMode(m_pendingL2Search.bytePositionModeAtStart);
        empty->setTextEncoding(m_pendingL2Search.encodingAtStart);
        emit findAllL2Finished(empty, m_pendingL2Search.pattern, fileName());
        resetPendingL2Search();
        return;
    }

    LargeFileSearch::Options opts;
    opts.regexMode     = m_pendingL2Search.regex;
    opts.hexBytesMode  = m_pendingL2Search.hexBytes;
    opts.byteContextMode = m_pendingL2Search.byteContextModeAtStart;
    opts.tolerateInvalidEncoding =
        m_pendingL2Search.tolerateInvalidEncodingAtStart;
    opts.caseSensitive = m_pendingL2Search.hexBytes
        ? true : m_pendingL2Search.caseSensitive;
    opts.wholeWord     = m_pendingL2Search.hexBytes
        ? false : m_pendingL2Search.wholeWord;
    opts.textEncoding  = m_pendingL2Search.encodingAtStart;

    // 先把之前可能残留的连接断开
    QObject::disconnect(m_pendingL2Search.searchProgressConn);
    QObject::disconnect(m_pendingL2Search.searchPartialConn);
    QObject::disconnect(m_pendingL2Search.searchFinishedConn);
    QObject::disconnect(m_pendingL2Search.searchFailedConn);

    QPointer<EditorTab> that(this);
    QObject* parentForModel = m_pendingL2Search.parentForModel;
    QString patternCopy    = m_pendingL2Search.pattern;
    QString fileNameCopy   = fileName();
    const quint64 revisionCopy = m_pendingL2Search.contentRevision;
    const TextEncoding encodingCopy = m_pendingL2Search.encodingAtStart;
    const bool bytePositionCopy = m_pendingL2Search.bytePositionModeAtStart;
    const bool byteContextCopy = m_pendingL2Search.byteContextModeAtStart;

    m_pendingL2Search.searchProgressConn = QObject::connect(
        s, &LargeFileSearch::progress, this,
        [this, that](qint64 done, qint64 total) {
            if (!that) return;
            int pct = (total > 0) ? int((done * 100LL) / total) : -1;
            emit findAllL2Progress(pct, done, total);
        });

    m_pendingL2Search.searchPartialConn = QObject::connect(
        s, &LargeFileSearch::partialBatch, this,
        [this, that, patternCopy, fileNameCopy, encodingCopy,
         bytePositionCopy](
            const QVector<LargeFileSearchMatch> &batch) {
            if (!that || !that->m_pendingL2Search.active
                || !that->pendingL2SearchSourceIsCurrent()
                || that->m_pendingL2Search.previewMatchesPublished >= 5000) {
                return;
            }
            const int remaining = 5000 - that->m_pendingL2Search.previewMatchesPublished;
            const QVector<LargeFileSearchMatch> preview = batch.size() <= remaining
                ? batch : batch.mid(0, remaining);
            that->m_pendingL2Search.previewMatchesPublished += preview.size();
            emit that->findAllL2Partial(preview, patternCopy, fileNameCopy,
                                       encodingCopy, bytePositionCopy);
        });

    m_pendingL2Search.searchFinishedConn = QObject::connect(
        s, &LargeFileSearch::finished, this,
        [this, that, s, parentForModel, patternCopy, fileNameCopy,
         revisionCopy, encodingCopy, bytePositionCopy,
         byteContextCopy](qint64 /*total*/) {
            if (!that) return;
            if (!that->pendingL2SearchSourceIsCurrent()
                || that->m_documentSession.state().revision != revisionCopy) {
                that->clearSearchHighlights();
                that->resetPendingL2Search();
                emit that->findAllL2Finished(nullptr, patternCopy, fileNameCopy);
                return;
            }
            auto *model = new SearchResultsModel(parentForModel);
            model->setBytePositionMode(bytePositionCopy);
            model->setTextEncoding(encodingCopy);
            model->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount(),
                              s->overrunCheckpoints(), s->overrunGroupedCount(), 2);
            that->m_l2SearchCachePattern = patternCopy;
            that->m_l2SearchCacheRegex = that->m_pendingL2Search.regex;
            that->m_l2SearchCacheCaseSensitive = that->m_pendingL2Search.caseSensitive;
            that->m_l2SearchCacheWholeWord = that->m_pendingL2Search.wholeWord;
            that->m_l2SearchCacheHexBytes = that->m_pendingL2Search.hexBytes;
            that->m_l2SearchCacheEncoding = encodingCopy;
            that->m_l2SearchCacheByteContext = byteContextCopy;
            that->m_l2SearchCacheRevision = revisionCopy;
            that->m_l2SearchCacheValid = true;
            that->m_lastSearchError.clear();
            // 把匹配位置传给 VirtualFileView 用于正文区域染色
            if (that->m_l2View
                && that->l2ProjectionMatches(encodingCopy,
                                             byteContextCopy)) {
                QVector<QPair<qint64, qint32>> highlights;
                const auto& memMatches = s->memoryMatches();
                highlights.reserve(memMatches.size());
                for (const auto& m : memMatches) {
                    highlights.append(qMakePair(m.byteOffset, m.length));
                }
                that->m_l2SearchHighlights = highlights;
                that->m_l2View->setSearchHighlights(highlights);
                that->applyL2SearchHighlightsToOverlay();
            } else {
                that->clearSearchHighlights();
            }
            emit findAllL2Finished(model, patternCopy, fileNameCopy);
            that->resetPendingL2Search();
        });

    m_pendingL2Search.searchFailedConn = QObject::connect(
        s, &LargeFileSearch::failed, this,
        [this, that, s, parentForModel, patternCopy, fileNameCopy,
         revisionCopy, encodingCopy, bytePositionCopy](const QString& msg) {
            if (!that) return;
            if (!that->pendingL2SearchSourceIsCurrent()
                || that->m_documentSession.state().revision != revisionCopy) {
                that->clearSearchHighlights();
                that->resetPendingL2Search();
                emit that->findAllL2Finished(nullptr, patternCopy, fileNameCopy);
                return;
            }
            if (auto *mw = qobject_cast<QMainWindow*>(that->window());
                mw && mw->statusBar()) {
                mw->statusBar()->showMessage(QObject::tr("搜索失败：%1").arg(msg), 8000);
            }
            that->m_l2SearchCacheValid = false;
            that->m_lastSearchError = msg;
            auto *partial = new SearchResultsModel(parentForModel);
            partial->setBytePositionMode(bytePositionCopy);
            partial->setTextEncoding(encodingCopy);
            partial->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount(),
                                s->overrunCheckpoints(), s->overrunGroupedCount(), 2);
            partial->setIncompleteReason(msg);
            emit findAllL2Finished(partial, patternCopy, fileNameCopy);
            that->resetPendingL2Search();
        });

    // 走 AsyncTaskManager 派发
    s->findAllViaTaskManager(m_pendingL2Search.pattern, opts, fileNameCopy);
}

bool EditorTab::findAllL2Async(const QString& pattern, bool regex,
                               bool caseSensitive, bool wholeWord,
                               QObject* parentForModel,
                               bool hexBytes)
{
    m_lastSearchError.clear();
    if (!isL2Mode() || pattern.isEmpty())
        return false;
    // One search owns the shared mmap/PatchList snapshot until it finishes.
    // The UI disables duplicate starts; refusing a programmatic overlap avoids
    // reading a PatchList while another request flushes edits into it.
    if (m_pendingL2Search.active) {
        return false;
    }

    QString sourceError;
    if (!ensureWindowedSearchSource(&sourceError)) {
        m_lastSearchError = sourceError;
        return false;
    }

    QString flushError;
    if (!bridgeFlushOverlayToPatches(&flushError)) {
        m_lastSearchError = flushError;
        if (auto *mw = qobject_cast<QMainWindow*>(this->window()))
            mw->statusBar()->showMessage(QObject::tr("无法准备搜索快照：%1").arg(flushError), 5000);
        return false;
    }

    m_pendingL2Search.active        = true;
    m_pendingL2Search.pattern       = pattern;
    m_pendingL2Search.regex         = regex;
    m_pendingL2Search.caseSensitive = caseSensitive;
    m_pendingL2Search.wholeWord     = wholeWord;
    m_pendingL2Search.hexBytes      = hexBytes;
    m_pendingL2Search.encodingAtStart = displayEncoding();
    m_pendingL2Search.byteContextModeAtStart = isHexPresentation();
    m_pendingL2Search.bytePositionModeAtStart =
        isBinaryDocument() || m_pendingL2Search.byteContextModeAtStart;
    m_pendingL2Search.tolerateInvalidEncodingAtStart =
        !m_pendingL2Search.byteContextModeAtStart
        && (isBinaryDocument() || encodingHasDecodeErrors());
    m_pendingL2Search.sourcePathAtStart = m_documentSession.state().location;
    m_pendingL2Search.sourceSizeAtStart = contentSizeBytes();
    m_pendingL2Search.diskVersionAtStart = m_documentSession.diskVersion();
    m_pendingL2Search.contentRevision = m_documentSession.state().revision;
    m_pendingL2Search.parentForModel = parentForModel;

    if (m_l2View->isOpen()) {
        // Search starts from the stable byte snapshot immediately. Line/column
        // decoration is optional and may still be building in the background.
        startPendingL2Search();
        return true;
    }

    // VirtualFileView::openFile() accepts mmap work asynchronously. mmapSize()
    // is populated immediately for progress reporting, while isOpen() only
    // becomes true when onMmapReady installs the mapped pointer. Treat that
    // interval as an accepted, cancelable search-preparation state instead of
    // reporting a false zero-result completion.
    m_pendingL2Search.waitLineBuildConn = connect(
        m_l2View, &VirtualFileView::fileOpenedChanged, this,
        [this](bool opened) {
            if (!m_pendingL2Search.active || !opened || !m_l2View
                || !m_l2View->isOpen()) {
                return;
            }
            startPendingL2Search();
        });
    m_pendingL2Search.waitMapFailedConn = connect(
        m_l2View, &VirtualFileView::lineBuildFinished, this,
        [this](bool success) {
            if (!m_pendingL2Search.active || success || !m_l2View
                || m_l2View->isOpen()) {
                return;
            }
            const QString patternCopy = m_pendingL2Search.pattern;
            const QString fileNameCopy = fileName();
            QObject *parentForModel = m_pendingL2Search.parentForModel;
            const QString message = QObject::tr("无法建立大文件搜索映射。");
            m_lastSearchError = message;
            auto *partial = new SearchResultsModel(parentForModel);
            partial->setBytePositionMode(
                m_pendingL2Search.bytePositionModeAtStart);
            partial->setTextEncoding(m_pendingL2Search.encodingAtStart);
            partial->setIncompleteReason(message);
            resetPendingL2Search();
            emit findAllL2Finished(partial, patternCopy, fileNameCopy);
        });
    return true;
}

void EditorTab::cancelFindAllL2()
{
    if (m_l1FindAllTaskId > 0) {
        AsyncTaskManager::instance().cancel(m_l1FindAllTaskId);
        m_l1FindAllTaskId = 0;
    }
    if (!m_pendingL2Search.active) {
        if (m_l2View) {
            if (LargeFileSearch *search = m_l2View->searchEngine()) {
                search->requestCancel();
                if (search->lastTaskId() > 0)
                    AsyncTaskManager::instance().cancel(search->lastTaskId());
            }
        }
        return;
    }
    if (!m_l2View || !m_l2View->isOpen()) {
        const QString patternCopy = m_pendingL2Search.pattern;
        const QString fileNameCopy = fileName();
        QObject *parentForModel = m_pendingL2Search.parentForModel;
        auto *partial = new SearchResultsModel(parentForModel);
        partial->setBytePositionMode(
            m_pendingL2Search.bytePositionModeAtStart);
        partial->setTextEncoding(m_pendingL2Search.encodingAtStart);
        partial->setIncompleteReason(QObject::tr("搜索已取消。"));
        resetPendingL2Search();
        emit findAllL2Finished(partial, patternCopy, fileNameCopy);
        return;
    }
    cancelPendingL2SearchForContentChange();
}

bool EditorTab::findNext(const QString &pattern, bool regex, bool caseSensitive,
                         bool wholeWord, bool forward, bool wrap,
                         bool hexBytes)
{
    if (pattern.isEmpty()) return false;
    if (!isL2Mode()) {
        m_lastSearchError.clear();
        if (hexBytes) {
            m_lastSearchError = QObject::tr("十六进制字节搜索仅适用于 Binary/Hex 文档。");
            return false;
        }
        const bool cacheMatches = m_l1SearchCacheValid
            && m_l1SearchCacheRevision == m_documentSession.state().revision
            && m_l1SearchCachePattern == pattern
            && m_l1SearchCacheRegex == regex
            && m_l1SearchCacheCaseSensitive == caseSensitive
            && m_l1SearchCacheWholeWord == wholeWord;
        const qint64 selectionStart = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 selectionEnd = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        if (!cacheMatches) {
            const QByteArray subject = m_editor->text().toUtf8();
            qint64 boundary = forward
                ? qMax(selectionStart, selectionEnd)
                : qMin(selectionStart, selectionEnd);
            const bool currentZeroLengthHit = m_l1NavigationHitValid
                && m_l1NavigationRevision == m_documentSession.state().revision
                && m_l1NavigationPattern == pattern
                && m_l1NavigationRegex == regex
                && m_l1NavigationCaseSensitive == caseSensitive
                && m_l1NavigationWholeWord == wholeWord
                && m_l1NavigationHit.byteLength == 0
                && m_l1NavigationHit.byteOffset == boundary;
            const bool currentZeroLengthHitAtEnd = currentZeroLengthHit
                && boundary >= subject.size();
            if (forward && currentZeroLengthHit && boundary < subject.size()) {
                // PCRE2 start offsets must remain on a UTF-8 code-point
                // boundary. Advance past the current zero-width hit so F3
                // cannot select it forever.
                ++boundary;
                while (boundary < subject.size()
                       && (uchar(subject.at(boundary)) & 0xC0) == 0x80) {
                    ++boundary;
                }
            }

            QString error;
            qint64 foundOffset = -1;
            qint64 foundLength = 0;
            const auto remember = [&](qint64 offset, qint64 length) {
                foundOffset = offset;
                foundLength = length;
            };
            bool ok = true;
            if (!regex) {
                auto literal = findEditableTextLiteralHits(
                    subject, pattern, caseSensitive, wholeWord, {}, {}, true,
                    !forward, boundary);
                if (literal.isEmpty() && wrap) {
                    literal = findEditableTextLiteralHits(
                        subject, pattern, caseSensitive, wholeWord, {}, {}, true,
                        !forward, forward ? 0 : subject.size() + 1);
                }
                if (!literal.isEmpty()) {
                    foundOffset = literal.first().byteOffset;
                    foundLength = literal.first().byteLength;
                }
            } else {
                Pcre2Regex compiled;
                if (!compiled.compile(pattern, false, caseSensitive, wholeWord,
                                      &error)) {
                    m_lastSearchError = error;
                    return false;
                }
                const auto scanForward = [&](qint64 start) {
                    const auto result = compiled.scan(
                        reinterpret_cast<const uchar *>(subject.constData()),
                        subject.size(), {}, {},
                        [&](qint64 offset, qint64 length) {
                            remember(offset, length);
                            return false;
                        }, &error, {}, {}, start);
                    return result != Pcre2Regex::ScanResult::Failed;
                };
                if (forward) {
                    if (!currentZeroLengthHitAtEnd) {
                        ok = scanForward(qBound<qint64>(0, boundary,
                                                       subject.size()));
                    }
                    if (ok && foundOffset < 0 && wrap && boundary > 0)
                        ok = scanForward(0);
                } else {
                    const auto result = compiled.scan(
                        reinterpret_cast<const uchar *>(subject.constData()),
                        subject.size(), {}, {},
                        [&](qint64 offset, qint64 length) {
                            if (offset >= boundary)
                                return false;
                            remember(offset, length);
                            return true;
                        }, &error);
                    ok = result != Pcre2Regex::ScanResult::Failed;
                    if (ok && foundOffset < 0 && wrap && boundary < subject.size()) {
                        const auto wrapped = compiled.scan(
                            reinterpret_cast<const uchar *>(subject.constData()),
                            subject.size(), {}, {},
                            [&](qint64 offset, qint64 length) {
                                remember(offset, length);
                                return true;
                            }, &error, {}, {}, boundary);
                        ok = wrapped != Pcre2Regex::ScanResult::Failed;
                    }
                }
            }
            if (!ok) {
                m_lastSearchError = error;
                return false;
            }
            if (foundOffset < 0)
                return false;

            m_l1NavigationHit = {foundOffset, qint32(foundLength)};
            m_l1NavigationPattern = pattern;
            m_l1NavigationRegex = regex;
            m_l1NavigationCaseSensitive = caseSensitive;
            m_l1NavigationWholeWord = wholeWord;
            m_l1NavigationRevision = m_documentSession.state().revision;
            m_l1NavigationHitValid = true;
            m_lastL1MatchIndex = -1;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                    long(foundOffset),
                                    long(foundOffset + foundLength));
            const int line = int(m_editor->SendScintilla(
                QsciScintillaBase::SCI_LINEFROMPOSITION, long(foundOffset)));
            m_editor->ensureLineVisible(line);
            m_editor->ensureCursorVisible();
            m_editor->setFocus();
            return true;
        }
        if (m_l1SearchCache.isEmpty())
            return false;

        int chosen = -1;
        const bool lastStillSelected = m_lastL1MatchIndex >= 0
            && m_lastL1MatchIndex < m_l1SearchCache.size()
            && selectionStart == m_l1SearchCache.at(m_lastL1MatchIndex).byteOffset
            && selectionEnd == selectionStart
                               + m_l1SearchCache.at(m_lastL1MatchIndex).byteLength;
        if (lastStillSelected) {
            chosen = m_lastL1MatchIndex + (forward ? 1 : -1);
        } else if (forward) {
            const qint64 from = selectionStart == selectionEnd
                ? selectionEnd : qMax(selectionStart, selectionEnd);
            const auto found = std::lower_bound(
                m_l1SearchCache.cbegin(), m_l1SearchCache.cend(), from,
                [](const L1SearchHit &hit, qint64 offset) {
                    return hit.byteOffset < offset;
                });
            chosen = int(std::distance(m_l1SearchCache.cbegin(), found));
        } else {
            const qint64 from = selectionStart == selectionEnd
                ? selectionStart : qMin(selectionStart, selectionEnd);
            const auto found = std::lower_bound(
                m_l1SearchCache.cbegin(), m_l1SearchCache.cend(), from,
                [](const L1SearchHit &hit, qint64 offset) {
                    return hit.byteOffset < offset;
                });
            chosen = int(std::distance(m_l1SearchCache.cbegin(), found)) - 1;
        }
        if (chosen < 0 || chosen >= m_l1SearchCache.size()) {
            if (!wrap)
                return false;
            chosen = forward ? 0 : m_l1SearchCache.size() - 1;
        }

        const L1SearchHit &hit = m_l1SearchCache.at(chosen);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                long(hit.byteOffset),
                                long(hit.byteOffset + hit.byteLength));
        const int line = int(m_editor->SendScintilla(
            QsciScintillaBase::SCI_LINEFROMPOSITION, long(hit.byteOffset)));
        m_editor->ensureLineVisible(line);
        m_editor->ensureCursorVisible();
        m_editor->setFocus();
        m_lastL1MatchIndex = chosen;
        return true;
    }
    m_lastSearchError.clear();
    // L2 路径：同一查询/同一内容修订复用已有结果。旧实现每按一次 F3
    // 都重新扫描整文件，在 2G+ 日志上等同于功能不可用。
    const bool cacheMatches = m_l2SearchCacheValid
        && m_l2SearchCacheRevision == m_documentSession.state().revision
        && m_l2SearchCachePattern == pattern
        && m_l2SearchCacheRegex == regex
        && m_l2SearchCacheCaseSensitive == caseSensitive
        && m_l2SearchCacheWholeWord == wholeWord
        && m_l2SearchCacheHexBytes == hexBytes
        && l2ProjectionMatches(m_l2SearchCacheEncoding,
                               m_l2SearchCacheByteContext);
    if (!cacheMatches) {
        auto *model = findAllL2(pattern, regex, caseSensitive, wholeWord,
                                nullptr, hexBytes);
        if (!model) return false;
        delete model;  // 搜索引擎保留有界内存结果与磁盘 spool
        if (!m_lastSearchError.isEmpty()) return false;
    }
    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) return false;
    LargeFileSearchMatch m;
    const bool navigationHitMatches = m_l2NavigationHitValid
        && m_l2NavigationRevision == m_documentSession.state().revision
        && m_l2NavigationPattern == pattern
        && m_l2NavigationRegex == regex
        && m_l2NavigationCaseSensitive == caseSensitive
        && m_l2NavigationWholeWord == wholeWord
        && m_l2NavigationHexBytes == hexBytes
        && l2ProjectionMatches(m_l2NavigationEncoding,
                               m_l2NavigationByteContext);
    qint64 from = m_lastL2Byte;
    if (navigationHitMatches) {
        from = forward
            ? m_l2NavigationHit.byteOffset + m_l2NavigationHit.byteLength - 1
            : m_l2NavigationHit.byteOffset;
    } else if (windowedReadOnlyActive()
        && m_windowedReadOnlyProvider->selectionEndOffset()
               > m_windowedReadOnlyProvider->selectionStartOffset()) {
        const qint64 selectionStart =
            m_windowedReadOnlyProvider->selectionStartOffset();
        const qint64 selectionEnd =
            m_windowedReadOnlyProvider->selectionEndOffset();
        LargeFileSearchMatch selectedMatch;
        const bool selectedIsCurrentMatch =
            s->findNext(selectionStart - 1, &selectedMatch) == selectionStart
            && selectedMatch.length == selectionEnd - selectionStart;
        from = selectedIsCurrentMatch
            ? selectionStart
            : (forward ? selectionEnd - 1 : selectionStart);
    } else if (m_l2View->hasSelection()) {
        const qint64 selectionStart = m_l2View->selectionStart();
        const qint64 selectionEnd = m_l2View->selectionEnd();
        LargeFileSearchMatch selectedMatch;
        const bool selectedIsCurrentMatch = selectionEnd > selectionStart
            && s->findNext(selectionStart - 1, &selectedMatch) == selectionStart
            && selectedMatch.length == selectionEnd - selectionStart;
        if (selectedIsCurrentMatch) {
            // The current result is already selected: Next/Previous advances
            // strictly away from it.
            from = selectionStart;
        } else {
            // An arbitrary user selection is a search boundary, not a cached
            // result. Forward starts at/after its end; backward before start.
            from = forward ? selectionEnd - 1 : selectionStart;
        }
    } else if (forward) {
        // LargeFileSearch::findNext is strictly greater-than. Subtract one so
        // a match beginning exactly at an unselected caret (including BOF) is
        // considered, matching the S1 lower_bound contract.
        from = from <= 0 ? -1 : from - 1;
    } else if (from != std::numeric_limits<qint64>::max()) {
        // findPrev is strictly less-than; add one to include a match beginning
        // exactly at an unselected caret without overflowing the EOF sentinel.
        ++from;
    }
    qint64 off = forward ? s->findNext(from, &m) : s->findPrev(from, &m);
    if (off < 0) {
        if (!wrap) return false;
        // wrap：从开头/结尾重新找
        off = forward ? s->findNext(-1, &m) : s->findPrev(std::numeric_limits<qint64>::max(), &m);
    }
    if (off < 0) return false;
    gotoByteOffset(m.byteOffset, m.length);
    m_lastL2Byte = m.byteOffset;
    return true;
}

bool EditorTab::findNextL2Async(
    const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
    bool forward, bool wrap, bool hexBytes, QObject *callbackContext,
    std::function<void(bool found, QString error)> callback)
{
    m_lastSearchError.clear();
    if (pattern.isEmpty())
        return false;

    if (!isL2Mode()) {
        if (hexBytes) {
            m_lastSearchError = QObject::tr(
                "十六进制字节搜索仅适用于 Binary/Hex 文档。");
            if (callback) callback(false, m_lastSearchError);
            return false;
        }
        const bool cacheMatches = m_l1SearchCacheValid
            && m_l1SearchCacheRevision == m_documentSession.state().revision
            && m_l1SearchCachePattern == pattern
            && m_l1SearchCacheRegex == regex
            && m_l1SearchCacheCaseSensitive == caseSensitive
            && m_l1SearchCacheWholeWord == wholeWord;
        if (cacheMatches
            || m_editor->length() <= L1InlineNavigationBudgetBytes) {
            const bool found = findNext(pattern, regex, caseSensitive,
                                        wholeWord, forward, wrap, false);
            if (callback)
                callback(found, m_lastSearchError);
            return true;
        }

        const qint64 selectionStart = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 selectionEnd = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        qint64 boundary = forward
            ? qMax(selectionStart, selectionEnd)
            : qMin(selectionStart, selectionEnd);
        const quint64 revision = m_documentSession.state().revision;
        const bool wipeSnapshot = isSecureDocument();
        QByteArray subject = m_editor->text().toUtf8();
        const bool currentZeroLengthHit = m_l1NavigationHitValid
            && m_l1NavigationRevision == revision
            && m_l1NavigationPattern == pattern
            && m_l1NavigationRegex == regex
            && m_l1NavigationCaseSensitive == caseSensitive
            && m_l1NavigationWholeWord == wholeWord
            && m_l1NavigationHit.byteLength == 0
            && m_l1NavigationHit.byteOffset == boundary;
        const bool currentZeroLengthHitAtEnd = currentZeroLengthHit
            && boundary >= subject.size();
        if (forward && currentZeroLengthHit && boundary < subject.size()) {
            ++boundary;
            while (boundary < subject.size()
                   && (uchar(subject.at(boundary)) & 0xC0) == 0x80) {
                ++boundary;
            }
        }
        const qint64 snapshotSize = subject.size();
        QPointer<EditorTab> that(this);
        QPointer<QObject> guard(callbackContext);
        const QString displayName = fileName().isEmpty()
            ? QObject::tr("未命名文档") : fileName();
        AsyncTaskManager::instance().submit(
            AsyncTaskType::LargeFileSearch, AsyncTaskPriority::Highest,
            forward ? QObject::tr("查找下一个: %1").arg(displayName)
                    : QObject::tr("查找上一个: %1").arg(displayName),
            QObject::tr("找到一处即返回"),
            [that, guard, subject = std::move(subject), pattern, regex,
             caseSensitive, wholeWord, forward, wrap, boundary,
             currentZeroLengthHitAtEnd, revision,
             wipeSnapshot, callback = std::move(callback)]
            (qint64 taskId, AsyncCancelPtr cancel,
             std::function<void(qint64, qint64)> reportProgress,
             std::function<void(const AsyncErrorInfo &)> reportError) mutable {
                const auto wipe = qScopeGuard([&] {
                    if (wipeSnapshot && !subject.isEmpty())
                        subject.fill(0);
                });
                QString error;
                qint64 foundOffset = -1;
                qint64 foundLength = 0;
                const auto cancelled = [&] {
                    return cancel && cancel->load(std::memory_order_acquire);
                };
                AsyncTaskManager::instance().updateTaskPhase(
                    taskId, regex ? QObject::tr("执行正则匹配")
                                  : QObject::tr("定向扫描字面值"),
                    1, 1, 0, subject.size());
                bool ok = true;
                if (!regex) {
                    auto literal = findEditableTextLiteralHits(
                        subject, pattern, caseSensitive, wholeWord, cancelled,
                        [reportProgress, total = qint64(subject.size())](qint64 done) {
                            reportProgress(done, total);
                        }, true, !forward, boundary);
                    if (literal.isEmpty() && !cancelled() && wrap) {
                        literal = findEditableTextLiteralHits(
                            subject, pattern, caseSensitive, wholeWord, cancelled,
                            [reportProgress, total = qint64(subject.size())](qint64 done) {
                                reportProgress(done, total);
                            }, true, !forward,
                            forward ? 0 : subject.size() + 1);
                    }
                    if (!literal.isEmpty()) {
                        foundOffset = literal.first().byteOffset;
                        foundLength = literal.first().byteLength;
                    }
                    ok = !cancelled();
                } else {
                    Pcre2Regex compiled;
                    ok = compiled.compile(pattern, false, caseSensitive,
                                          wholeWord, &error);
                    const auto scanForward = [&](qint64 start) {
                    const auto result = compiled.scan(
                        reinterpret_cast<const uchar *>(subject.constData()),
                        subject.size(), cancelled,
                        [reportProgress, total = qint64(subject.size())](qint64 done) {
                            reportProgress(done, total);
                        },
                        [&](qint64 offset, qint64 length) {
                            foundOffset = offset;
                            foundLength = length;
                            return false;
                        }, &error, {}, {}, start);
                    return result != Pcre2Regex::ScanResult::Failed
                        && !cancelled();
                    };
                    if (ok && forward) {
                    if (!currentZeroLengthHitAtEnd) {
                        ok = scanForward(qBound<qint64>(0, boundary,
                                                       subject.size()));
                    }
                    if (ok && foundOffset < 0 && wrap && boundary > 0)
                        ok = scanForward(0);
                    } else if (ok) {
                    const auto result = compiled.scan(
                        reinterpret_cast<const uchar *>(subject.constData()),
                        subject.size(), cancelled,
                        [reportProgress, total = qint64(subject.size())](qint64 done) {
                            reportProgress(done, total);
                        },
                        [&](qint64 offset, qint64 length) {
                            if (offset >= boundary)
                                return false;
                            foundOffset = offset;
                            foundLength = length;
                            return true;
                        }, &error);
                    ok = result != Pcre2Regex::ScanResult::Failed && !cancelled();
                    if (ok && foundOffset < 0 && wrap && boundary < subject.size()) {
                        const auto wrapped = compiled.scan(
                            reinterpret_cast<const uchar *>(subject.constData()),
                            subject.size(), cancelled, {},
                            [&](qint64 offset, qint64 length) {
                                foundOffset = offset;
                                foundLength = length;
                                return true;
                            }, &error, {}, {}, boundary);
                        ok = wrapped != Pcre2Regex::ScanResult::Failed
                            && !cancelled();
                    }
                    }
                }
                if (!ok && error.isEmpty() && cancelled())
                    error = QObject::tr("已取消。");
                if (!ok && !error.isEmpty() && error != QObject::tr("已取消。")) {
                    AsyncErrorInfo taskError;
                    taskError.message = error;
                    reportError(taskError);
                }
                if (!guard || !callback)
                    return;
                QMetaObject::invokeMethod(
                    guard,
                    [that, callback = std::move(callback), revision,
                     foundOffset, foundLength, ok, pattern, regex,
                     caseSensitive, wholeWord,
                     error = std::move(error)]() mutable {
                        if (!that) {
                            callback(false, QObject::tr("文档已关闭。"));
                            return;
                        }
                        if (that->m_documentSession.state().revision != revision) {
                            that->m_lastSearchError = QObject::tr(
                                "文档已变更，请重新查找。");
                            callback(false, that->m_lastSearchError);
                            return;
                        }
                        const bool found = ok && foundOffset >= 0;
                        that->m_lastSearchError = error;
                        if (found) {
                            that->m_l1NavigationHit = {
                                foundOffset, qint32(foundLength)};
                            that->m_l1NavigationPattern = pattern;
                            that->m_l1NavigationRegex = regex;
                            that->m_l1NavigationCaseSensitive = caseSensitive;
                            that->m_l1NavigationWholeWord = wholeWord;
                            that->m_l1NavigationRevision = revision;
                            that->m_l1NavigationHitValid = true;
                            that->m_editor->SendScintilla(
                                QsciScintillaBase::SCI_SETSEL, long(foundOffset),
                                long(foundOffset + foundLength));
                            const int line = int(that->m_editor->SendScintilla(
                                QsciScintillaBase::SCI_LINEFROMPOSITION,
                                long(foundOffset)));
                            that->m_editor->ensureLineVisible(line);
                            that->m_editor->ensureCursorVisible();
                            that->m_editor->setFocus();
                        }
                        callback(found, error);
                    }, Qt::QueuedConnection);
            }, 0, QLatin1String("SearchNavigation"), snapshotSize);
        return true;
    }

    const bool cacheMatches = m_l2SearchCacheValid
        && m_l2SearchCacheRevision == m_documentSession.state().revision
        && m_l2SearchCachePattern == pattern
        && m_l2SearchCacheRegex == regex
        && m_l2SearchCacheCaseSensitive == caseSensitive
        && m_l2SearchCacheWholeWord == wholeWord
        && m_l2SearchCacheHexBytes == hexBytes
        && l2ProjectionMatches(m_l2SearchCacheEncoding,
                               m_l2SearchCacheByteContext);
    if (cacheMatches) {
        const bool found = findNext(pattern, regex, caseSensitive, wholeWord,
                                    forward, wrap, hexBytes);
        if (callback)
            callback(found, m_lastSearchError);
        return true;
    }
    QString sourceError;
    if (!ensureWindowedSearchSource(&sourceError)) {
        m_lastSearchError = sourceError;
        if (callback) callback(false, sourceError);
        return false;
    }
    if (!m_l2View || !m_l2View->isOpen()) {
        // The production provider is already usable while the lazy mmap search
        // source is still being installed.  Accept the user's navigation once
        // and continue it as soon as the mapping is ready; requiring another
        // click both loses the original command and makes the first-hit path
        // appear synchronous with preparation.
        struct PendingNavigation {
            QMetaObject::Connection opened;
            QMetaObject::Connection failed;
            QPointer<QObject> callbackGuard;
            std::function<void(bool, QString)> callback;
            bool completed = false;
        };
        auto pending = std::make_shared<PendingNavigation>();
        pending->callbackGuard = callbackContext ? callbackContext : this;
        pending->callback = std::move(callback);
        QPointer<EditorTab> tabGuard(this);
        const auto finishPreparation = [pending]() {
            QObject::disconnect(pending->opened);
            QObject::disconnect(pending->failed);
            pending->completed = true;
        };
        pending->opened = connect(
            m_l2View, &VirtualFileView::fileOpenedChanged, this,
            [tabGuard, pending, finishPreparation, pattern, regex,
             caseSensitive, wholeWord, forward, wrap, hexBytes](bool opened) mutable {
                if (pending->completed || !opened || !tabGuard
                    || !tabGuard->m_l2View || !tabGuard->m_l2View->isOpen()) {
                    return;
                }
                finishPreparation();
                if (!pending->callbackGuard)
                    return;
                auto continuation = std::move(pending->callback);
                tabGuard->findNextL2Async(
                    pattern, regex, caseSensitive, wholeWord, forward, wrap,
                    hexBytes, pending->callbackGuard.data(),
                    std::move(continuation));
            });
        pending->failed = connect(
            m_l2View, &VirtualFileView::lineBuildFinished, this,
            [tabGuard, pending, finishPreparation](bool success) mutable {
                if (pending->completed || success || !tabGuard
                    || (tabGuard->m_l2View && tabGuard->m_l2View->isOpen())) {
                    return;
                }
                finishPreparation();
                const QString message = QObject::tr("无法建立大文件搜索映射。");
                tabGuard->m_lastSearchError = message;
                if (pending->callbackGuard && pending->callback)
                    pending->callback(false, message);
            });
        return true;
    }
    QString flushError;
    if (!bridgeFlushOverlayToPatches(&flushError)) {
        m_lastSearchError = flushError;
        if (callback) callback(false, flushError);
        return false;
    }

    // Navigation owns a separate search lane.  A background Find All may keep
    // publishing its index, while this Highest-priority task stops at the
    // first directional match and updates the editor immediately.
    LargeFileSearch *search = m_l2View->navigationSearchEngine();
    if (!search) {
        m_lastSearchError = QObject::tr("搜索引擎未就绪。");
        if (callback) callback(false, m_lastSearchError);
        return false;
    }

    qint64 from = m_lastL2Byte;
    const bool navigationHitMatches = m_l2NavigationHitValid
        && m_l2NavigationRevision == m_documentSession.state().revision
        && m_l2NavigationPattern == pattern
        && m_l2NavigationRegex == regex
        && m_l2NavigationCaseSensitive == caseSensitive
        && m_l2NavigationWholeWord == wholeWord
        && m_l2NavigationHexBytes == hexBytes
        && l2ProjectionMatches(m_l2NavigationEncoding,
                               m_l2NavigationByteContext);
    if (navigationHitMatches) {
        from = forward
            ? m_l2NavigationHit.byteOffset + m_l2NavigationHit.byteLength - 1
            : m_l2NavigationHit.byteOffset;
    } else if (windowedReadOnlyActive()
        && m_windowedReadOnlyProvider->selectionEndOffset()
               > m_windowedReadOnlyProvider->selectionStartOffset()) {
        const qint64 selectionStart =
            m_windowedReadOnlyProvider->selectionStartOffset();
        const qint64 selectionEnd =
            m_windowedReadOnlyProvider->selectionEndOffset();
        from = forward ? selectionEnd - 1 : selectionStart;
    } else if (m_l2View->hasSelection()) {
        const qint64 selectionStart = m_l2View->selectionStart();
        const qint64 selectionEnd = m_l2View->selectionEnd();
        from = forward ? selectionEnd - 1 : selectionStart;
    } else if (forward) {
        from = from <= 0 ? -1 : from - 1;
    } else if (from != std::numeric_limits<qint64>::max()) {
        ++from;
    }

    LargeFileSearch::Options opts;
    opts.regexMode = regex;
    opts.hexBytesMode = hexBytes;
    opts.byteContextMode = isHexPresentation();
    opts.tolerateInvalidEncoding = !opts.byteContextMode
        && (isBinaryDocument() || encodingHasDecodeErrors());
    opts.caseSensitive = hexBytes ? true : caseSensitive;
    opts.wholeWord = hexBytes ? false : wholeWord;
    opts.textEncoding = displayEncoding();
    const quint64 revision = m_documentSession.state().revision;
    const TextEncoding searchEncoding = opts.textEncoding;
    const bool searchByteContext = opts.byteContextMode;
    QPointer<EditorTab> guard(this);
    const qint64 taskId = search->findOneViaTaskManager(
        pattern, opts, from, forward, wrap, fileName(),
        callbackContext ? callbackContext : this,
        [guard, revision, searchEncoding, searchByteContext,
         pattern, regex, caseSensitive, wholeWord, hexBytes,
         callback = std::move(callback)](
            bool found, LargeFileSearchMatch match, QString error) mutable {
            if (!guard)
                return;
            if (guard->m_documentSession.state().revision != revision) {
                error = QObject::tr("文档已变化，请重新查找。");
                found = false;
            } else if (!guard->l2ProjectionMatches(searchEncoding,
                                                   searchByteContext)) {
                error = QObject::tr("显示编码或呈现方式已变化，请重新查找。");
                found = false;
            }
            guard->m_lastSearchError = error;
            if (found) {
                guard->m_l2NavigationHit = {match.byteOffset, match.length};
                guard->m_l2NavigationPattern = pattern;
                guard->m_l2NavigationRegex = regex;
                guard->m_l2NavigationCaseSensitive = caseSensitive;
                guard->m_l2NavigationWholeWord = wholeWord;
                guard->m_l2NavigationHexBytes = hexBytes;
                guard->m_l2NavigationEncoding = searchEncoding;
                guard->m_l2NavigationByteContext = searchByteContext;
                guard->m_l2NavigationRevision = revision;
                guard->m_l2NavigationHitValid = true;
                guard->gotoByteOffset(match.byteOffset, match.length);
                guard->m_lastL2Byte = match.byteOffset;
                guard->m_lastSearchError.clear();
            } else if (error.isEmpty()
                       && guard->m_l2NavigationHitValid
                       && guard->m_l2NavigationRevision == revision
                       && guard->m_l2NavigationPattern == pattern
                       && guard->m_l2NavigationRegex == regex
                       && guard->m_l2NavigationCaseSensitive == caseSensitive
                       && guard->m_l2NavigationWholeWord == wholeWord
                       && guard->m_l2NavigationHexBytes == hexBytes
                       && guard->l2ProjectionMatches(
                              guard->m_l2NavigationEncoding,
                              guard->m_l2NavigationByteContext)) {
                // A boundary miss must leave the current match selected, as
                // mature editors do.  The windowed projection may have been
                // reloaded while the asynchronous scan ran, so restore from
                // revision-bound navigation metadata rather than trusting the
                // transient widget selection.
                guard->gotoByteOffset(
                    guard->m_l2NavigationHit.byteOffset,
                    guard->m_l2NavigationHit.byteLength);
            }
            if (callback)
                callback(found, std::move(error));
        });
    return taskId > 0;
}

void EditorTab::resetSearchPosition(bool forward)
{
    if (windowedReadOnlyActive()) {
        const qint64 boundary = forward
            ? 0 : qMax<qint64>(0, m_windowedReadOnlyProvider->fileSize() - 1);
        m_windowedReadOnlyProvider->goToOffset(boundary);
        m_lastL2Byte = forward ? -1 : std::numeric_limits<qint64>::max();
        return;
    }
    if (isL2Mode() && m_l2View) {
        if (forward)
            m_l2View->gotoByteOffset(0);
        else
            m_l2View->gotoByteOffset(std::numeric_limits<qint64>::max());
        m_l2View->clearSelection();
        // gotoByteOffset emits cursor updates. Assign the search sentinel last
        // so those updates cannot turn forward reset (-1) into byte 0 and skip
        // a real BOF match.
        m_lastL2Byte = forward ? -1 : std::numeric_limits<qint64>::max();
        return;
    }
    if (forward) {
        m_lastL1MatchIndex = -1;
        m_editor->setCursorPosition(0, 0);
    } else {
        m_lastL1MatchIndex = -1;
        const int lastLine = qMax(0, m_editor->lines() - 1);
        m_editor->setCursorPosition(lastLine, m_editor->text(lastLine).size());
    }
}

bool EditorTab::currentSelectionMatchesSearch(
    const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
    bool hexBytes)
{
    if (pattern.isEmpty())
        return false;
    if (!isL2Mode()) {
        if (hexBytes)
            return false;
        qint64 start = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        qint64 end = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        if (end < start)
            std::swap(start, end);
        if (end <= start)
            return false;
        if (m_l1NavigationHitValid
            && m_l1NavigationRevision == m_documentSession.state().revision
            && m_l1NavigationPattern == pattern
            && m_l1NavigationRegex == regex
            && m_l1NavigationCaseSensitive == caseSensitive
            && m_l1NavigationWholeWord == wholeWord
            && m_l1NavigationHit.byteOffset == start
            && m_l1NavigationHit.byteLength == end - start) {
            return true;
        }
        if (!m_l1SearchCacheValid
            || m_l1SearchCacheRevision != m_documentSession.state().revision
            || m_l1SearchCachePattern != pattern
            || m_l1SearchCacheRegex != regex
            || m_l1SearchCacheCaseSensitive != caseSensitive
            || m_l1SearchCacheWholeWord != wholeWord) {
            return false;
        }
        const auto found = std::lower_bound(
            m_l1SearchCache.cbegin(), m_l1SearchCache.cend(), start,
            [](const L1SearchHit &hit, qint64 offset) {
                return hit.byteOffset < offset;
            });
        return found != m_l1SearchCache.cend()
            && found->byteOffset == start
            && found->byteLength == end - start;
    }

    const bool hasWindowedSelection = windowedReadOnlyActive()
        && m_windowedReadOnlyProvider->selectionEndOffset()
               > m_windowedReadOnlyProvider->selectionStartOffset();
    if (!m_l2View || (!hasWindowedSelection && !m_l2View->hasSelection())) {
        return false;
    }
    const qint64 start = hasWindowedSelection
        ? m_windowedReadOnlyProvider->selectionStartOffset()
        : m_l2View->selectionStart();
    const qint64 end = hasWindowedSelection
        ? m_windowedReadOnlyProvider->selectionEndOffset()
        : m_l2View->selectionEnd();
    if (end <= start)
        return false;
    if (m_l2NavigationHitValid
        && m_l2NavigationRevision == m_documentSession.state().revision
        && m_l2NavigationPattern == pattern
        && m_l2NavigationRegex == regex
        && m_l2NavigationCaseSensitive == caseSensitive
        && m_l2NavigationWholeWord == wholeWord
        && m_l2NavigationHexBytes == hexBytes
        && l2ProjectionMatches(m_l2NavigationEncoding,
                               m_l2NavigationByteContext)
        && m_l2NavigationHit.byteOffset == start
        && m_l2NavigationHit.byteLength == end - start) {
        return true;
    }
    if (!m_l2SearchCacheValid
        || m_l2SearchCacheRevision != m_documentSession.state().revision
        || m_l2SearchCachePattern != pattern
        || m_l2SearchCacheRegex != regex
        || m_l2SearchCacheCaseSensitive != caseSensitive
        || m_l2SearchCacheWholeWord != wholeWord
        || m_l2SearchCacheHexBytes != hexBytes
        || !l2ProjectionMatches(m_l2SearchCacheEncoding,
                                m_l2SearchCacheByteContext)) {
        return false;
    }
    LargeFileSearch *search = m_l2View->searchEngine();
    if (!search)

        return false;
    LargeFileSearchMatch match;
    const qint64 offset = search->findNext(start - 1, &match);
    return offset == start && match.byteOffset == start
        && match.length == end - start;
}

bool EditorTab::replaceCurrentSearchMatch(
    const QString &find, const QString &replacement,
    bool regex, bool caseSensitive, bool wholeWord, QString *error)
{
    if (!currentSelectionMatchesSearch(
            find, regex, caseSensitive, wholeWord, false)) {
        if (error)
            *error = QObject::tr("当前选区不是本次查询的匹配。");
        return false;
    }

    bool replacementEncodingOk = true;
    QByteArray bytes = isL2Mode()
        ? encodeTextFragment(replacement, displayEncoding(),
                             &replacementEncodingOk)
        : replacement.toUtf8();
    if (!replacementEncodingOk) {
        if (error) {
            *error = QObject::tr("替换文本包含 %1 无法表示的字符。")
                         .arg(encodingName(displayEncoding()));
        }
        return false;
    }
    const auto wipeReplacement = qScopeGuard([&] {
        if (isSecureDocument() && !bytes.isEmpty())
            bytes.fill(0);
    });
    if (regex) {
        qint64 start = 0;
        qint64 end = 0;
        const uchar *subject = nullptr;
        qint64 subjectSize = 0;
        QByteArray snapshot;
        if (!isL2Mode()) {
            start = m_editor->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONSTART);
            end = m_editor->SendScintilla(
                QsciScintillaBase::SCI_GETSELECTIONEND);
            snapshot = m_editor->text().toUtf8();
            subject = reinterpret_cast<const uchar *>(snapshot.constData());
            subjectSize = snapshot.size();
        } else if (m_l2View) {
            start = m_l2View->selectionStart();
            end = m_l2View->selectionEnd();
            subjectSize = m_l2View->virtualFileSize();
            if (!m_l2View->isModified()) {
                subject = m_l2View->mmapBase();
            } else if (subjectSize <= Q_INT64_C(64) * 1024 * 1024) {
                snapshot = m_l2View->readByteRange(0, subjectSize);
                subject = reinterpret_cast<const uchar *>(snapshot.constData());
            } else {
                if (error) {
                    *error = QObject::tr(
                        "当前大文件已有未保存修改，无法在不复制整个文件的情况下重建正则捕获；请先保存后继续。");
                }
                return false;
            }
        }
        Pcre2Regex compiled;
        QString expansionError;
        const bool encodedByteRegex = isL2Mode()
            && displayEncoding() != TextEncoding::Utf8
            && displayEncoding() != TextEncoding::Utf8Bom;
        if (encodedByteRegex) {
            if (error) {
                *error = QObject::tr(
                    "原始编码文本视图为只读；请转换并另存为 UTF-8 后替换。");
            }
            return false;
        }
        const bool compiledOk = compiled.compile(
            find, false, caseSensitive, wholeWord, &expansionError);
        const bool expandedOk = subject && compiledOk
            && compiled.expandReplacementAt(
                subject, subjectSize, start, end - start,
                replacement, &bytes, &expansionError);
        if (!expandedOk) {
            if (error)
                *error = expansionError;
            if (!snapshot.isEmpty())
                snapshot.fill(0);
            return false;
        }
        if (isSecureDocument() && !snapshot.isEmpty())
            snapshot.fill(0);
    }

    if (!isL2Mode()) {
        const qint64 start = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 end = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                                long(start));
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                                long(end));
        const qint64 inserted = m_editor->SendScintilla(
            QsciScintillaBase::SCI_REPLACETARGET,
            static_cast<uintptr_t>(bytes.size()), bytes.constData());
        if (inserted < 0) {
            if (error)
                *error = QObject::tr("Scintilla 拒绝了当前替换。");
            return false;
        }
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION,
                                long(start + inserted));
    } else if (m_l2View) {
        const qint64 start = m_l2View->selectionStart();
        const qint64 end = m_l2View->selectionEnd();
        if (!m_l2View->replaceBytes(
                start, qint32(qMin<qint64>(end - start, INT_MAX)),
                bytes, error)) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

void EditorTab::replaceCurrent(const QString &replaceText)
{
    if (!isL2Mode()) {
        const qint64 start = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 end = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        if (end <= start)
            return;
        const QByteArray replacement = replaceText.toUtf8();
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                                long(start));
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                                long(end));
        const qint64 inserted = m_editor->SendScintilla(
            QsciScintillaBase::SCI_REPLACETARGET,
            static_cast<uintptr_t>(replacement.size()), replacement.constData());
        if (inserted >= 0) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION,
                                    long(start + inserted));
        }
        return;
    }
    if (!m_l2View) return;
    // 先决条件：findNext 必须已经调用过并且 [selStart, selEnd) 正好覆盖待替换的 old。
    if (!m_l2View->hasSelection()) return;
    const qint64 s = m_l2View->selectionStart();
    const qint64 e = m_l2View->selectionEnd();
    if (e <= s) return;
    const qint64 maxL = m_l2View->patches().virtualSize(m_l2View->mmapSize());
    Q_UNUSED(maxL);
    const qint32 oldLen = qint32(qMin<qint64>(e - s, qint64(INT_MAX)));
    bool encodingOk = true;
    const QByteArray newData = encodeTextFragment(
        replaceText, displayEncoding(), &encodingOk);
    QString err;
    if (!encodingOk) {
        err = QObject::tr("替换文本包含 %1 无法表示的字符。")
                  .arg(encodingName(displayEncoding()));
    }
    const bool ok = encodingOk
        && m_l2View->replaceBytes(s, oldLen, newData, &err);
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
        findAll(find, regex, caseSensitive, wholeWord);
        if (!m_lastSearchError.isEmpty() || !m_l1SearchCacheValid)
            return 0;
        const QVector<L1SearchHit> hits = m_l1SearchCache;
        if (std::any_of(hits.cbegin(), hits.cend(), [](const L1SearchHit &hit) {
                return hit.byteLength == 0;
            })) {
            m_lastSearchError = QObject::tr(
                "批量替换暂不接受零长度匹配；请让表达式至少消费一个字符。");
            return 0;
        }
        QByteArray subject = m_editor->text().toUtf8();
        QVector<QByteArray> replacements;
        const auto wipeSearchMaterial = qScopeGuard([&] {
            if (!isSecureDocument())
                return;
            if (!subject.isEmpty())
                subject.fill(0);
            for (QByteArray &value : replacements) {
                if (!value.isEmpty())
                    value.fill(0);
            }
        });
        replacements.reserve(hits.size());
        if (regex) {
            Pcre2Regex compiled;
            QString expansionError;
            if (!compiled.compile(find, false, caseSensitive, wholeWord,
                                  &expansionError)) {
                m_lastSearchError = expansionError;
                return 0;
            }
            for (const L1SearchHit &hit : hits) {
                QByteArray expanded;
                if (!compiled.expandReplacementAt(
                        reinterpret_cast<const uchar *>(subject.constData()),
                        subject.size(), hit.byteOffset, hit.byteLength,
                        replace, &expanded, &expansionError)) {
                    m_lastSearchError = expansionError;
                    return 0;
                }
                replacements.append(std::move(expanded));
            }
        } else {
            const QByteArray literal = replace.toUtf8();
            for (qsizetype i = 0; i < hits.size(); ++i)
                replacements.append(literal);
        }
        m_editor->SendScintilla(QsciScintillaBase::SCI_BEGINUNDOACTION);
        for (qsizetype index = hits.size(); index > 0; --index) {
            const L1SearchHit &hit = hits.at(index - 1);
            const QByteArray &replacement = replacements.at(index - 1);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                                    long(hit.byteOffset));
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                                    long(hit.byteOffset + hit.byteLength));
            m_editor->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                    static_cast<uintptr_t>(replacement.size()),
                                    replacement.constData());
        }
        m_editor->SendScintilla(QsciScintillaBase::SCI_ENDUNDOACTION);
        m_editor->setCursorPosition(0, 0);
        return hits.size();
    }
    // L2：调 LargeFileSearch::replaceAll 分段原子替换（临时仍同步执行，但移除 WindowModal 进度窗避免阻塞）
    if (!m_l2View) return 0;
    LargeFileSearch *s = m_l2View->searchEngine();
    if (!s) return 0;

    LargeFileSearch::Options opts;
    opts.regexMode     = regex;
    opts.caseSensitive = caseSensitive;
    opts.wholeWord     = wholeWord;
    opts.textEncoding  = displayEncoding();

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
    m_l2View->notifyExternalPatchMutation();
    // 返回总数：replacedCount（精确替换成功数）
    return int(replacedCount);
}
