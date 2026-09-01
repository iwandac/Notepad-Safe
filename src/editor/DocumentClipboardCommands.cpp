#include "EditorTab.h"

#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "WindowedReadOnlyProvider.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QStatusBar>

bool EditorTab::ownsUserInputObject(const QObject *object) const
{
    if (!object)
        return false;
    QsciScintilla *visibleEditor = editor();
    if (object == visibleEditor
        || (visibleEditor && object == visibleEditor->viewport())) {
        return true;
    }
    if (object == m_hexAsciiPreview
        || (m_hexAsciiPreview && object == m_hexAsciiPreview->viewport())) {
        return true;
    }
    return ownsWindowedReadOnlyInputObject(m_windowedReadOnlyProvider, object);
}

void EditorTab::copy()
{
    if (windowedReadOnlyActive()) {
        QString error;
        if (!m_windowedReadOnlyProvider->copySelection(&error) && !error.isEmpty()) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(error, 5000);
            QApplication::beep();
        }
        return;
    }
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused) {
                QKeyEvent event(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
                (void)handleHexAsciiKeyPress(&event);
                return;
            }
            qint64 start = 0;
            qint64 end = 0;
            if (hexSelectionByteRange(&start, &end) && end > start) {
                QString limitError;
                if (!ByteClipboard::payloadWithinLimit(end - start,
                                                       &limitError)) {
                    if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                        window->statusBar()->showMessage(limitError, 6000);
                    QApplication::beep();
                    return;
                }
                QByteArray selected = m_l2View->readByteRange(start, end);
                ByteClipboard::setHexSelection(selected);
                if (isSecureBinaryDocument() && !selected.isEmpty())
                    selected.fill(0);
            }
            return;
        }
        m_editor->copy();
        return;
    }
    activeTextEditor()->copy();
}
