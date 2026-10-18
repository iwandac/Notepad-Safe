#pragma once

#include "DocumentState.h"
#include "LargeFileSearch.h"
#include "SearchMatch.h"
#include "TextEncoding.h"

#include <QWidget>
#include <functional>

class SearchResultsModel;

// Where the user is in a document, for the find panel. Byte positions are
// UTF-8 offsets of editable text; other documents report -1.
struct DocumentSearchCursor {
    qint64 selectionStart = -1;
    qint64 selectionEnd = -1;
    qint64 documentBytes = -1;
    QString prefill;   // selected single-line text, else the word at the caret
};

// UI-facing search port implemented by every document surface.  Application
// search coordination depends on this contract, never on EditorTab or a
// concrete text/hex widget.
class DocumentSearchSurface : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    ~DocumentSearchSurface() override = default;

    virtual DocumentState documentState() const noexcept = 0;
    virtual QString fileName() const = 0;
    virtual quint64 contentRevision() const = 0;
    virtual bool isSecureDocumentMasked() const = 0;
    virtual bool isSecureDocumentTransitioning() const = 0;
    virtual bool isBinaryDocument() const = 0;
    virtual bool isHexPresentation() const = 0;
    virtual bool isL2Mode() const = 0;
    virtual bool isReadOnlyByPolicy() const = 0;
    virtual TextEncoding displayEncoding() const = 0;
    virtual void setFocusEditor() = 0;
    virtual void undo() = 0;

    virtual bool findNext(const QString &pattern, bool regex,
                          bool caseSensitive, bool wholeWord, bool forward,
                          bool wrap, bool hexBytes = false) = 0;
    virtual bool findNextL2Async(
        const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
        bool forward, bool wrap, bool hexBytes, QObject *callbackContext,
        std::function<void(bool found, QString error)> callback) = 0;
    virtual void resetSearchPosition(bool forward) = 0;
    virtual bool currentSelectionMatchesSearch(
        const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
        bool hexBytes = false) = 0;
    virtual bool replaceCurrentSearchMatch(
        const QString &find, const QString &replacement, bool regex,
        bool caseSensitive, bool wholeWord, QString *error = nullptr) = 0;
    virtual int replaceAll(const QString &find, const QString &replace,
                           bool regex, bool caseSensitive,
                           bool wholeWord) = 0;
    virtual QVector<SearchMatch> findAll(const QString &pattern, bool regex,
                                         bool caseSensitive,
                                         bool wholeWord) = 0;
    virtual bool findAllL1Async(
        const QString &pattern, bool regex, bool caseSensitive, bool wholeWord,
        QObject *callbackContext,
        std::function<void(QVector<SearchMatch> matches, bool truncated,
                           QString error)> callback) = 0;
    virtual bool lastFindAllWasTruncated() const = 0;
    virtual QString lastSearchError() const = 0;
    virtual void setSearchHighlights(const QVector<SearchMatch> &matches) = 0;
    virtual DocumentSearchCursor searchCursor() const = 0;
    // Selects and reveals a match without moving keyboard focus.
    virtual void selectSearchHit(qint64 byteOffset, qint32 byteLength) = 0;
    virtual bool findAllL2Async(const QString &pattern, bool regex,
                                bool caseSensitive, bool wholeWord,
                                QObject *parentForModel = nullptr,
                                bool hexBytes = false) = 0;
    virtual void cancelFindAllL2() = 0;

signals:
    void findAllL2Progress(int percent, qint64 done, qint64 total);
    void findAllL2Partial(const QVector<LargeFileSearchMatch> &batch,
                          const QString &pattern, const QString &fileName,
                          TextEncoding encoding, bool bytePositionMode);
    void findAllL2Finished(SearchResultsModel *model, const QString &pattern,
                           const QString &fileName);
};
