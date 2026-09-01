#pragma once

class QPlainTextEdit;
class QScrollBar;
class QsciScintilla;
class VirtualFileView;
class QWidget;

void configurePrimaryEditorAccessibility(QsciScintilla *editor);
void configureEditorPresentationAccessibility(QsciScintilla *editor, bool hex);
void configureHexCharacterAccessibility(QPlainTextEdit *ascii, bool readOnly);
void configureByteProviderAccessibility(VirtualFileView *provider);
void configureHexTabOrder(QWidget *hex, QWidget *ascii, QWidget *globalScroll);
void configureEditableHexOverlayAccessibility(QsciScintilla *hex,
                                               QPlainTextEdit *ascii,
                                               QScrollBar *globalScroll);
