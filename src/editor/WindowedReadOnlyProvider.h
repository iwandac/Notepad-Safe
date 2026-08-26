#pragma once

// Bounded mmap-backed read-only provider shared by S2 and S3.

#include <QDateTime>
#include <QVector>
#include <QWidget>

#include "SharedReadOnlyFile.h"
#include "TextEncoding.h"
#include "ReadOnlySelectionModel.h"

class QFileSystemWatcher;
class QPlainTextEdit;
class QScrollBar;
class QTimer;
class QsciScintilla;

class WindowedReadOnlyProvider final : public QWidget
{
    Q_OBJECT

public:
    enum class Presentation { Text, Hex };

    explicit WindowedReadOnlyProvider(QWidget *parent = nullptr);
    ~WindowedReadOnlyProvider() override;

    bool openFile(const QString &path, Presentation presentation, QString *error = nullptr);
    void closeFile();
    void setPresentation(Presentation presentation);
    void setBinaryContent(bool binary);
    bool binaryContent() const { return m_binaryContent; }
    void setTextEncoding(TextEncoding encoding);
    TextEncoding textEncoding() const { return m_textEncoding; }
    void setWordWrap(bool enabled);
    bool wordWrap() const { return m_wordWrap; }
    Presentation presentation() const { return m_presentation; }

    qint64 fileSize() const { return m_fileSize; }
    qint64 characterCount() const { return m_characterCount; }
    bool characterCountPending() const { return m_characterCountPending; }
    qint64 globalOffset() const { return m_globalOffset; }
    qint64 windowStart() const { return m_windowStart; }
    qint64 windowEnd() const { return m_windowEnd; }
    qint64 visibleTopOffset() const;
    qint64 visibleBottomOffset() const;
    int hexBytesPerRow() const { return m_hexBytesPerRow; }
    QByteArray sourceBytes(qint64 offset, qsizetype length) const;
    qint64 selectionStartOffset() const;
    qint64 selectionEndOffset() const;
    bool copySelection(QString *error = nullptr) const;
    bool refreshFileMapping(QString *error = nullptr);

    QsciScintilla *editor() const { return m_editor; }
    QPlainTextEdit *asciiPreview() const { return m_asciiPreview; }
    QScrollBar *globalScrollBar() const { return m_globalScrollBar; }
    int visibleVerticalScrollBarCount() const;

    void goToOffset(qint64 offset, qint64 selectionBytes = 0);
    void selectAllSource();
    void findBytesAsync(const QByteArray &needle, qint64 fromOffset = 0);
    bool searchRunning() const;
    qint64 lastSearchResult() const { return m_lastSearchResult; }

signals:
    void windowReady(qint64 startByte, qint64 endByte, qint64 anchorByte);
    void searchProgress(qint64 scannedBytes, qint64 totalBytes);
    void searchFinished(qint64 matchOffset);
    void sourceFileChanged(qint64 previousSize, qint64 currentSize);
    void sourceReloadFailed(const QString &message);
    void characterCountChanged(qint64 count, bool pending);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class SelectionAuthority { NativeProjection, SourceBytes };

    void reloadWindow(qint64 anchorOffset, qint64 selectionBytes,
                      qint64 requestedTopOffset = -1, bool preserveHorizontal = false);
    void scrollByLines(int lineDelta);
    void beginVirtualSelection();
    void updateVirtualSelection(qint64 caretOffset);
    void clearVirtualSelection();
    void applyVirtualSelection();
    qint64 sourceOffsetForProjectedLine(int line) const;
    qint64 sourceOffsetAtViewportY(int y) const;
    int projectedDisplayLineCount() const;
    void scrollProjectionToEnd();
    void configureProjectionMargin();
    void populateAsciiProjection(const QByteArray &source);
    void syncAsciiSelectionFromHex();
    void syncHexSelectionFromAscii();
    void syncAsciiScrollFromHex();
    qint64 sourceOffsetForAsciiPosition(int position, bool endBoundary) const;
    int asciiPositionForSourceOffset(qint64 offset) const;
    qint64 sourceBoundaryForHexPosition(qint64 position) const;
    void setHexCaretAtSource(qint64 offset, int nibble = 0);
    void setHexSourceSelection(qint64 anchor, qint64 caret);
    qint64 sourceOffsetForProjectedPosition(qint64 position, bool endBoundary) const;
    int projectedLineForSourceOffset(qint64 offset) const;
    qint64 projectedPositionForSourceOffset(qint64 offset) const;
    int calculateHexBytesPerRow() const;
    qint64 offsetForSlider(int value) const;
    int sliderForOffset(qint64 offset) const;
    QByteArray formatHexWindow(const uchar *data, qsizetype length, qint64 absoluteStart) const;
    void selectProjectedBytes(qint64 absoluteOffset, qint64 selectionBytes);
    void startLineNumberIndex();
    void cancelLineNumberIndex();
    bool lineNumberBaseForOffset(qint64 offset, qint64 *baseLine) const;

    SharedReadOnlyFile m_file;
    QString m_path;
    qint64 m_fileSize = 0;
    qint64 m_globalOffset = 0;
    qint64 m_windowStart = 0;
    qint64 m_windowEnd = 0;
    TextEncoding m_textEncoding = TextEncoding::Utf8;
    QVector<qint32> m_projectedToSource;
    QVector<qint32> m_sourceBoundaries;
    QVector<qint32> m_projectedBoundaries;
    qint64 m_windowSpan = 2LL * 1024 * 1024;
    Presentation m_presentation = Presentation::Text;
    bool m_binaryContent = false;
    QsciScintilla *m_editor = nullptr;
    QPlainTextEdit *m_asciiPreview = nullptr;
    QScrollBar *m_globalScrollBar = nullptr;
    bool m_updatingScrollBar = false;
    bool m_wordWrap = false;
    bool m_scrollToEndAfterPaint = false;
    int m_hexBytesPerRow = 16;
    int m_wheelAngleRemainder = 0;
    int m_wheelPixelRemainder = 0;
    bool m_hexRelayoutPending = false;
    bool m_syncingHexSelection = false;
    bool m_syncingAsciiSelection = false;
    bool m_applyingSourceSelection = false;
    bool m_asciiMouseSelecting = false;
    SelectionAuthority m_selectionAuthority = SelectionAuthority::NativeProjection;
    ReadOnlySelectionModel m_sourceSelection;
    QFileSystemWatcher *m_fileWatcher = nullptr;
    QTimer *m_remapDebounce = nullptr;
    QTimer *m_filePoll = nullptr;
    QDateTime m_observedModified;
    qint64 m_observedSize = -1;

    qint64 m_searchTaskId = -1;
    quint64 m_searchGeneration = 0;
    bool m_searchResultPending = false;
    qint64 m_lastSearchResult = -1;
    QByteArray m_searchNeedle;
    qint64 m_characterCount = -1;
    bool m_characterCountPending = false;
    qint64 m_lineNumberTaskId = -1;
    quint64 m_lineNumberGeneration = 0;
    bool m_lineNumberPending = false;
    QVector<qint64> m_lineBreakPrefix;
    QVector<quint8> m_lineBreakPrefixEndsWithCr;
    TextEncoding m_lineBreakPrefixEncoding = TextEncoding::Utf8;
};
