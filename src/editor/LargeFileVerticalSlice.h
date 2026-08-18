#pragma once

#include <QAtomicInteger>
#include <QDateTime>
#include <QFutureWatcher>
#include <QWidget>

#include "SharedReadOnlyFile.h"

class QLabel;
class QFileSystemWatcher;
class QProgressBar;
class QPlainTextEdit;
class QScrollBar;
class QStackedWidget;
class QTimer;
class QsciScintilla;
class ILoader;

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
    void setWordWrap(bool enabled);
    bool wordWrap() const { return m_wordWrap; }
    Presentation presentation() const { return m_presentation; }

    qint64 fileSize() const { return m_fileSize; }
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
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

    SharedReadOnlyFile m_file;
    QString m_path;
    qint64 m_fileSize = 0;
    qint64 m_globalOffset = 0;
    qint64 m_windowStart = 0;
    qint64 m_windowEnd = 0;
    qint64 m_windowSpan = 2LL * 1024 * 1024;
    Presentation m_presentation = Presentation::Text;
    QsciScintilla *m_editor = nullptr;
    QPlainTextEdit *m_asciiPreview = nullptr;
    QScrollBar *m_globalScrollBar = nullptr;
    bool m_updatingScrollBar = false;
    bool m_wordWrap = false;
    int m_hexBytesPerRow = 16;
    int m_wheelAngleRemainder = 0;
    int m_wheelPixelRemainder = 0;
    bool m_hexRelayoutPending = false;
    bool m_syncingHexSelection = false;
    bool m_syncingAsciiSelection = false;
    qint64 m_selectionAnchor = -1;
    qint64 m_selectionCaret = -1;
    QFileSystemWatcher *m_fileWatcher = nullptr;
    QTimer *m_remapDebounce = nullptr;
    QTimer *m_filePoll = nullptr;
    QDateTime m_observedModified;
    qint64 m_observedSize = -1;

    QFutureWatcher<qint64> m_searchWatcher;
    QAtomicInteger<bool> m_cancelSearch = false;
    bool m_searchResultPending = false;
    qint64 m_lastSearchResult = -1;
    QByteArray m_searchNeedle;
};

class LargeFileVerticalSlice final : public QWidget
{
    Q_OBJECT

public:
    enum class State {
        Closed,
        Mapped,
        PreviewReady,
        Reading,
        BuildingDocument,
        Attaching,
        Editable,
        ReadOnlyFallback
    };

    explicit LargeFileVerticalSlice(QWidget *parent = nullptr);
    ~LargeFileVerticalSlice() override;

    bool openS2Text(const QString &path, QString *error = nullptr, bool beginPromotion = true);
    bool openS3Hex(const QString &path, QString *error = nullptr);
    void startPromotion();
    void cancelPromotion();

    State state() const { return m_state; }
    QString stateText() const;
    QString fallbackReason() const { return m_fallbackReason; }
    WindowedReadOnlyProvider *preview() const { return m_preview; }
    QsciScintilla *fullEditor() const { return m_fullEditor; }
    qint64 fullDocumentLength() const;
    qint64 fullEditorPosition() const;
    int visibleVerticalScrollBarCount() const;

signals:
    void stateChanged(LargeFileVerticalSlice::State state);
    void promotionProgress(qint64 completedBytes, qint64 totalBytes);

private:
    void setState(State state);
    void enterFallback(const QString &reason);
    void finishPromotion();

    WindowedReadOnlyProvider *m_preview = nullptr;
    QsciScintilla *m_fullEditor = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_phaseLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QString m_path;
    QString m_fallbackReason;
    State m_state = State::Closed;
    ILoader *m_loader = nullptr;
    QFutureWatcher<bool> m_promotionWatcher;
    QAtomicInteger<bool> m_cancelPromotion = false;
};

Q_DECLARE_METATYPE(LargeFileVerticalSlice::State)
