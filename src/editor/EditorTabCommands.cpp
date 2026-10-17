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
#include "EditableTextFindAllTask.h"
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




// Editable-text commands, L1 search and editor presentation settings.
void EditorTab::duplicateLine()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_LINEDUPLICATE);
}

void EditorTab::deleteLine()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_LINEDELETE);
}

void EditorTab::moveLineUp()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_MOVESELECTEDLINESUP);
}

void EditorTab::moveLineDown()
{
    if (!supportsTextMutationCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_MOVESELECTEDLINESDOWN);
}

void EditorTab::selectionToUpper()
{
    QsciScintilla *editor = activeTextEditor();
    if (!supportsTextMutationCommands() || !editor->hasSelectedText())
        return;
    editor->SendScintilla(QsciScintillaBase::SCI_UPPERCASE);
}

void EditorTab::selectionToLower()
{
    QsciScintilla *editor = activeTextEditor();
    if (!supportsTextMutationCommands() || !editor->hasSelectedText())
        return;
    editor->SendScintilla(QsciScintillaBase::SCI_LOWERCASE);
}

void EditorTab::joinSelectedLines()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    const auto start = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
    const auto end = editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
    if (end <= start)
        return;
    editor->beginUndoAction();
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, start);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, end);
    editor->SendScintilla(QsciScintillaBase::SCI_LINESJOIN);
    editor->endUndoAction();
}

void EditorTab::selectNextOccurrence()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    if (!editor->hasSelectedText()) {
        const auto position = editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
        const auto start = editor->SendScintilla(QsciScintillaBase::SCI_WORDSTARTPOSITION,
                                                   position, 1L);
        const auto end = editor->SendScintilla(QsciScintillaBase::SCI_WORDENDPOSITION,
                                                 position, 1L);
        if (end <= start)
            return;
        editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
    }
    const long oldTargetStart = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETSTART);
    const long oldTargetEnd = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETEND);
    const long oldSearchFlags = editor->SendScintilla(
        QsciScintillaBase::SCI_GETSEARCHFLAGS);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, 0UL);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(editor->length()));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          QsciScintillaBase::SCFIND_MATCHCASE);
    editor->SendScintilla(QsciScintillaBase::SCI_MULTIPLESELECTADDNEXT);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                          static_cast<unsigned long>(oldTargetStart));
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(oldTargetEnd));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          static_cast<unsigned long>(oldSearchFlags));


}

void EditorTab::selectAllOccurrences()
{
    if (!supportsTextMutationCommands())
        return;
    QsciScintilla *editor = activeTextEditor();
    if (!editor->hasSelectedText()) {
        const auto position = editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
        const auto start = editor->SendScintilla(QsciScintillaBase::SCI_WORDSTARTPOSITION,
                                                   position, 1L);
        const auto end = editor->SendScintilla(QsciScintillaBase::SCI_WORDENDPOSITION,
                                                 position, 1L);
        if (end <= start)
            return;
        editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
    }
    const long oldTargetStart = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETSTART);
    const long oldTargetEnd = editor->SendScintilla(
        QsciScintillaBase::SCI_GETTARGETEND);
    const long oldSearchFlags = editor->SendScintilla(
        QsciScintillaBase::SCI_GETSEARCHFLAGS);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, 0UL);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(editor->length()));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          QsciScintillaBase::SCFIND_MATCHCASE);
    editor->SendScintilla(QsciScintillaBase::SCI_MULTIPLESELECTADDEACH);
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                          static_cast<unsigned long>(oldTargetStart));
    editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                          static_cast<unsigned long>(oldTargetEnd));
    editor->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                          static_cast<unsigned long>(oldSearchFlags));
}

void EditorTab::moveToMatchingBrace()
{
    if (isBinaryDocument() || isSecureDocumentMasked())
        return;
    activeTextEditor()->moveToMatchingBrace();
}

void EditorTab::selectToMatchingBrace()
{
    if (isBinaryDocument() || isSecureDocumentMasked())
        return;
    activeTextEditor()->selectToMatchingBrace();
}

void EditorTab::foldCurrentLine()
{
    if (!supportsLexerCommands())
        return;
    int line = 0;
    int index = 0;
    QsciScintilla *editor = activeTextEditor();
    editor->getCursorPosition(&line, &index);
    editor->foldLine(line);
}

void EditorTab::foldAll(bool expand)
{
    if (!supportsLexerCommands())
        return;
    activeTextEditor()->SendScintilla(QsciScintillaBase::SCI_FOLDALL,
                            expand ? QsciScintillaBase::SC_FOLDACTION_EXPAND
                                   : QsciScintillaBase::SC_FOLDACTION_CONTRACT);
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

QVector<SearchMatch> EditorTab::findAll(const QString &pattern, bool regex,
                                        bool caseSensitive, bool wholeWord)
{
    QVector<SearchMatch> matches;
    m_lastFindAllTruncated = false;
    m_lastSearchError.clear();
    m_l1SearchCache.clear();
    m_l1SearchCacheValid = false;
    m_lastL1MatchIndex = -1;
    if (pattern.isEmpty()) {
        m_lastSearchError = QObject::tr("查找文本为空。");
        return matches;
    }

    const QByteArray subject = m_editor->text().toUtf8();
    const auto recordMatch = [&](qint64 byteOffset, qint64 byteLength) {
            // Retain at most 200000 rows. Only the next actual hit establishes
            // that this is a prefix rather than a complete exact-capacity set.
            if (matches.size() == 200000) {
                m_lastFindAllTruncated = true;
                return false;
            }
            const int lineNum = int(m_editor->SendScintilla(
                QsciScintillaBase::SCI_LINEFROMPOSITION, long(byteOffset)));
            const qint64 lineByteStart = m_editor->SendScintilla(
                QsciScintillaBase::SCI_POSITIONFROMLINE, lineNum);
            QString lineText = m_editor->text(lineNum);
            while (lineText.endsWith('\n') || lineText.endsWith('\r'))
                lineText.chop(1);

            SearchMatch match;
            match.line = lineNum;
            match.index = QString::fromUtf8(subject.constData() + lineByteStart,
                                             byteOffset - lineByteStart).size();
            match.length = QString::fromUtf8(subject.constData() + byteOffset,
                                              byteLength).size();
            match.text = QString::fromUtf8(subject.constData() + byteOffset, byteLength);
            match.context = lineText;
            match.byteOffset = byteOffset;
            match.byteLength = qint32(byteLength);
            m_l1SearchCache.append({byteOffset, qint32(byteLength)});
            matches.append(std::move(match));
            return true;
        };
    if (regex) {
        Pcre2Regex compiled;
        QString scanError;
        if (!compiled.compile(pattern, false, caseSensitive, wholeWord,
                              &scanError)) {
            m_lastSearchError = scanError;
            return matches;
        }
        const Pcre2Regex::ScanResult scanResult = compiled.scan(
            reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
            {}, {}, recordMatch, &scanError);
        if (scanResult == Pcre2Regex::ScanResult::Failed) {
            m_lastSearchError = scanError;
            matches.clear();
            m_l1SearchCache.clear();
            return matches;
        }
        if (scanResult == Pcre2Regex::ScanResult::Cancelled
            && !m_lastFindAllTruncated) {
            m_lastSearchError = QObject::tr("搜索已取消。");
            matches.clear();
            m_l1SearchCache.clear();
            return matches;
        }
    } else {
        const QVector<EditableTextHit> literalHits = findEditableTextLiteralHits(
            subject, pattern, caseSensitive, wholeWord, {}, {}, false, false,
            -1, 200001);
        for (const EditableTextHit &hit : literalHits) {
            if (!recordMatch(hit.byteOffset, hit.byteLength))
                break;
        }
    }
    if (m_lastFindAllTruncated) {
        // A readable prefix is not a complete directional-navigation cache.
        m_l1SearchCache.clear();
        return matches;
    }
    m_l1SearchCachePattern = pattern;
    m_l1SearchCacheRegex = regex;
    m_l1SearchCacheCaseSensitive = caseSensitive;
    m_l1SearchCacheWholeWord = wholeWord;
    m_l1SearchCacheRevision = m_documentSession.state().revision;
    m_l1SearchCacheValid = true;
    return matches;
}

bool EditorTab::findAllL1Async(
    const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
    QObject *callbackContext,
    std::function<void(QVector<SearchMatch>, bool, QString)> callback)
{
    if (isL2Mode() || pattern.isEmpty() || m_l1FindAllTaskId > 0)
        return false;
    m_lastSearchError.clear();

    const quint64 revision = m_documentSession.state().revision;
    const bool wipeSnapshot = isSecureDocument();
    QByteArray subject = m_editor->text().toUtf8();
    const QString displayName = fileName().isEmpty()
        ? QObject::tr("未命名文档") : fileName();
    QPointer<EditorTab> that(this);
    QPointer<QObject> callbackGuard(callbackContext ? callbackContext : this);

    const qint64 submittedId = startEditableTextFindAllTask(
        std::move(subject), pattern, regex, caseSensitive, wholeWord, displayName,
        wipeSnapshot, this, callbackGuard.data(),
        [that, callbackGuard, revision, pattern, regex, caseSensitive, wholeWord,
         callback = std::move(callback)](qint64 taskId, EditableTextFindAllResult result) mutable {
            const auto notify = [&](QVector<SearchMatch> matches, bool truncated, QString error) {
                if (callbackGuard && callback)
                    callback(std::move(matches), truncated, std::move(error));
            };
            if (!that) {
                notify({}, false, QObject::tr("文档已关闭。"));
                return;
            }
            // A canceled request may still need UI settlement after its ID
            // was cleared. A different active request owns the cache/error.
            if (that->m_l1FindAllTaskId > 0 && that->m_l1FindAllTaskId != taskId) {
                notify({}, false, QObject::tr("查找请求已失效，请重新查找。"));
                return;
            }
            if (that->m_l1FindAllTaskId == 0) {
                notify({}, false, QObject::tr("已取消。"));
                return;
            }
            that->m_l1FindAllTaskId = 0;
            if (that->m_documentSession.state().revision != revision) {
                that->m_lastSearchError = QObject::tr("文档已变更，请重新查找。");
                notify({}, false, that->m_lastSearchError);
                return;
            }
            that->m_lastSearchError = result.ok ? QString() : result.error;
            that->m_lastFindAllTruncated = result.truncated;
            if (result.ok && result.truncated) {
                that->m_l1SearchCache.clear();
                that->m_l1SearchCacheValid = false;
                that->m_lastL1MatchIndex = -1;
            }
            if (result.ok && !result.truncated) {
                that->m_l1SearchCache = std::move(result.hits);
                that->m_l1SearchCachePattern = pattern;
                that->m_l1SearchCacheRegex = regex;
                that->m_l1SearchCacheCaseSensitive = caseSensitive;
                that->m_l1SearchCacheWholeWord = wholeWord;
                that->m_l1SearchCacheRevision = revision;
                that->m_l1SearchCacheValid = true;
                that->m_lastL1MatchIndex = -1;
            }
            notify(result.ok ? std::move(result.matches) : QVector<SearchMatch>{},
                   result.truncated, that->m_lastSearchError);
        });
    // taskAdded observers can close the document or submit a newer task before
    // submit returns. Never dereference this or replace that newer task ID.
    if (that && that->m_l1FindAllTaskId == 0)
        that->m_l1FindAllTaskId = submittedId;
    // Accepted work still owes a terminal notification if the owner closed
    // during submit; returning false would make callers settle it twice.
    return submittedId > 0;
}

void EditorTab::setSearchHighlights(const QVector<SearchMatch> &matches)
{
    if (isL2Mode() || !m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, indicator,
                            QsciScintillaBase::INDIC_ROUNDBOX);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, indicator, 0x00B7FF);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, indicator, 90);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
    for (const SearchMatch &match : matches) {
        const qint64 start = match.byteOffset >= 0
            ? match.byteOffset
            : m_editor->positionFromLineIndex(match.line, match.index);
        const qint64 end = match.byteOffset >= 0 && match.byteLength >= 0
            ? match.byteOffset + match.byteLength
            : m_editor->positionFromLineIndex(match.line,
                                              match.index + match.length);
        if (start >= 0 && end > start) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                                    long(start), long(end - start));
        }
    }
}

void EditorTab::clearSearchHighlights()
{
    m_l2SearchHighlights.clear();
    if (m_l2View)
        m_l2View->clearSearchHighlights();
    if (!m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
}

void EditorTab::selectSearchMatch(int line, int index, int length)
{
    if (isL2Mode()) {
        setCursorPosition(line, index);
        return;
    }
    const int start = m_editor->positionFromLineIndex(line, index);
    const int end = m_editor->positionFromLineIndex(line, index + qMax(0, length));
    if (start >= 0 && end >= start) {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                static_cast<long>(start), static_cast<long>(end));
    }
    m_editor->ensureLineVisible(line);
    m_editor->setFocus();
}

// ============ 光标 ============

void EditorTab::getCursorPosition(int *line, int *index) const
{
    if (isL2Mode()) {
        if (line)  *line  = m_lastL2Line;
        if (index) *index = m_lastL2Col;
        return;
    }
    activeTextEditor()->getCursorPosition(line, index);
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
    QsciScintilla *editor = activeTextEditor();
    editor->setCursorPosition(line, index);
    editor->ensureLineVisible(line);
}

// ============ 长行标尺 ============

void EditorTab::applyEdgeSettings()
{
    for (QsciScintilla *view : {m_editor}) {
        if (!view)
            continue;
        view->setEdgeColumn(m_edgeColumn);
        view->setEdgeMode(m_edgeVisible ? QsciScintilla::EdgeLine
                                        : QsciScintilla::EdgeNone);
    }
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

// ============ 编码切换 ============
