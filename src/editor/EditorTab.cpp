#include "EditorTab.h"
#include "EditorSurfaceStyle.h"
#include "ByteClipboard.h"
#include "VirtualFileView.h"
#include "LargeFileVerticalSlice.h"
#include "FileTierPolicy.h"
#include "SearchResultsModel.h"
#include "Pcre2Regex.h"
#include "AsyncTaskManager.h"
#include "DraftStore.h"

#include <QVBoxLayout>
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
#include <QBuffer>
#include <algorithm>
#include <limits>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <ILoader.h>

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

namespace {

QsciScintilla::EolMode detectLineEndingMode(const QString &text)
{
    qsizetype crlf = 0;
    qsizetype lf = 0;
    qsizetype cr = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (text.at(i) == QLatin1Char('\n')) {
            ++lf;
        }
    }
    if (crlf >= lf && crlf >= cr && crlf > 0)
        return QsciScintilla::EolWindows;
    if (cr > lf && cr > 0)
        return QsciScintilla::EolMac;
    if (lf > 0)
        return QsciScintilla::EolUnix;
    return QsciScintilla::EolWindows;
}

constexpr char LargeRecoveryMagic[] = "NotepadSafe.LargeEditJournal.v1";
constexpr quint32 LargeRecoveryVersion = 1;
constexpr qsizetype MaximumLargeRecoveryPayload = 32 * 1024 * 1024;
constexpr char BinaryRecoveryMagic[] = "NotepadSafe.BinaryPatchJournal.v1";
constexpr quint32 BinaryRecoveryVersion = 1;

QString formatHexBytes(const QByteArray &bytes, int bytesPerRow = 16)
{
    bytesPerRow = qMax(1, bytesPerRow);
    static constexpr char Digits[] = "0123456789ABCDEF";
    QString text;
    text.reserve(bytes.size() * 3 + bytes.size() / bytesPerRow + 1);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes.at(i));
        text.append(QLatin1Char(Digits[value >> 4]));
        text.append(QLatin1Char(Digits[value & 0x0f]));
        const bool endOfLine = ((i + 1) % bytesPerRow) == 0;
        if (i + 1 < bytes.size())
            text.append(endOfLine ? QLatin1Char('\n') : QLatin1Char(' '));
    }
    return text;
}

QString formatAsciiBytes(const QByteArray &bytes, int bytesPerRow = 16)
{
    bytesPerRow = qMax(1, bytesPerRow);
    QString text;
    text.reserve(bytes.size() + bytes.size() / bytesPerRow + 1);
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes.at(i));
        text.append(value >= 0x20 && value <= 0x7e
                        ? QChar::fromLatin1(char(value))
                        : QLatin1Char('.'));
        if ((i + 1) % bytesPerRow == 0 && i + 1 < bytes.size())
            text.append(QLatin1Char('\n'));
    }
    return text;
}

int hexNibble(QChar ch)
{
    const ushort value = ch.unicode();
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parseHexBytes(const QString &text, QByteArray *bytes, QString *error)
{
    if (!bytes)
        return false;
    bytes->clear();
    bytes->reserve(text.size() / 3 + 1);
    int highNibble = -1;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace()) {
            if (highNibble >= 0) {
                if (error) {
                    *error = QObject::tr(
                        "第 %1 个字符处的十六进制字节只有一位；每个字节必须是两位。")
                                 .arg(i + 1);
                }
                bytes->clear();
                return false;
            }
            continue;
        }
        const int nibble = hexNibble(ch);
        if (nibble < 0) {
            if (error)
                *error = QObject::tr("第 %1 个字符“%2”不是十六进制数字。")
                             .arg(i + 1).arg(ch);
            bytes->clear();
            return false;
        }
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            bytes->append(char((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    if (highNibble >= 0) {
        if (error)
            *error = QObject::tr("末尾的十六进制字节只有一位；每个字节必须是两位。");
        bytes->clear();
        return false;
    }
    return true;
}

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

using ScintillaSigned = intptr_t;
using ScintillaUnsigned = uintptr_t;
using ScintillaDirectFunction = ScintillaSigned (SCI_METHOD *)(
    ScintillaSigned, unsigned int, ScintillaUnsigned, ScintillaSigned);

ScintillaSigned directScintilla(QsciScintilla *editor, unsigned int message,
                                ScintillaUnsigned wParam = 0,
                                ScintillaSigned lParam = 0)
{
    if (!editor)
        return 0;
    const auto function = reinterpret_cast<ScintillaDirectFunction>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTFUNCTION));
    const auto context = reinterpret_cast<ScintillaSigned>(
        editor->SendScintillaPtrResult(QsciScintillaBase::SCI_GETDIRECTPOINTER));
    return function && context ? function(context, message, wParam, lParam) : 0;
}

class Utf8StreamValidator final {
public:
    bool feed(const char *data, qsizetype size)
    {
        const auto *bytes = reinterpret_cast<const unsigned char *>(data);
        for (qsizetype i = 0; i < size; ++i) {
            const unsigned char byte = bytes[i];
            if (remaining_ == 0) {
                if (byte <= 0x7f)
                    continue;
                if (byte >= 0xc2 && byte <= 0xdf) {
                    begin(byte & 0x1f, 1, 0x80);
                } else if (byte >= 0xe0 && byte <= 0xef) {
                    begin(byte & 0x0f, 2, 0x800);
                } else if (byte >= 0xf0 && byte <= 0xf4) {
                    begin(byte & 0x07, 3, 0x10000);
                } else {
                    return false;
                }
                continue;
            }
            if ((byte & 0xc0) != 0x80)
                return false;
            codePoint_ = (codePoint_ << 6) | (byte & 0x3f);
            if (--remaining_ == 0
                && (codePoint_ < minimum_ || codePoint_ > 0x10ffff
                    || (codePoint_ >= 0xd800 && codePoint_ <= 0xdfff))) {
                return false;
            }
        }
        return true;
    }

    bool complete() const { return remaining_ == 0; }

private:
    void begin(quint32 prefix, int remaining, quint32 minimum)
    {
        codePoint_ = prefix;
        remaining_ = remaining;
        minimum_ = minimum;
    }

    quint32 codePoint_ = 0;
    quint32 minimum_ = 0;
    int remaining_ = 0;
};

TextEncoding sniffLargeTextEncoding(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return TextEncoding::Utf8;
    const QByteArray sample = file.read(1024 * 1024);
    if (sample.size() >= 3
        && static_cast<unsigned char>(sample[0]) == 0xef
        && static_cast<unsigned char>(sample[1]) == 0xbb
        && static_cast<unsigned char>(sample[2]) == 0xbf) {
        return TextEncoding::Utf8Bom;
    }
    if (sample.size() >= 2
        && static_cast<unsigned char>(sample[0]) == 0xff
        && static_cast<unsigned char>(sample[1]) == 0xfe) {
        return TextEncoding::Utf16LE;
    }
    if (sample.size() >= 2
        && static_cast<unsigned char>(sample[0]) == 0xfe
        && static_cast<unsigned char>(sample[1]) == 0xff) {
        return TextEncoding::Utf16BE;
    }
    Utf8StreamValidator validator;
    return validator.feed(sample.constData(), sample.size())
               ? TextEncoding::Utf8 : TextEncoding::GBK;
}

qint64 estimatedUtf8Size(qint64 sourceBytes, TextEncoding encoding)
{
    if (sourceBytes < 0)
        return -1;
    if (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf8Bom)
        return sourceBytes;
    // UTF-16 and the Windows system codepage can expand when represented as UTF-8.
    // 1.5x is the conservative common-language bound used for admission; overflow
    // saturates so the file safely remains in analysis mode.
    if (sourceBytes > (std::numeric_limits<qint64>::max() / 3) * 2)
        return std::numeric_limits<qint64>::max();
    return sourceBytes + sourceBytes / 2;
}

QString admissionFallbackText(ResourceAdmissionReason reason)
{
    switch (reason) {
    case ResourceAdmissionReason::PhysicalMemoryBudgetExceeded:
        return QObject::tr("当前物理内存不足，保持只读分析模式");
    case ResourceAdmissionReason::CommitReserveInsufficient:
        return QObject::tr("当前可提交内存不足，保持只读分析模式");
    case ResourceAdmissionReason::InvalidMemorySnapshot:
        return QObject::tr("无法确认安全内存余量，保持只读分析模式");
    default:
        return QObject::tr("当前文件不能晋升为完整编辑模式");
    }
}

} // namespace

EditorTab::ExternalFileState EditorTab::externalFileState() const
{
    if (isSecureDocument() || m_filePath.isEmpty())
        return ExternalFileState::NotTracked;
    return compareDiskFileVersion(m_filePath, m_diskVersion);
}

void EditorTab::refreshDiskVersionBaseline()
{
    m_diskVersion = {};
    if (isSecureDocument() || m_filePath.isEmpty())
        return;
    DiskFileVersion captured;
    if (captureDiskFileVersion(m_filePath, &captured) == ExternalFileState::Unchanged)
        m_diskVersion = std::move(captured);
}

bool EditorTab::representsDiskFile(const DiskFileVersion &candidate) const
{
    if (isSecureDocument() || m_filePath.isEmpty() || !candidate.valid)
        return false;
    if (sameDiskFile(m_diskVersion, candidate))
        return true;
    return sameDiskFilePath(m_filePath, candidate.absolutePath);
}

EditorTab::EditorTab(QWidget *parent)
    : QWidget(parent)
{
    m_editor = new QsciScintilla(this);
    m_stack  = new QStackedWidget(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_editor);   // index 0 = L1 (QsciScintilla)

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    setupEditor();
    connect(m_editor, &QsciScintilla::selectionChanged,
            this, &EditorTab::commandCapabilitiesChanged);
    connect(m_editor, &QsciScintilla::textChanged,
            this, &EditorTab::commandCapabilitiesChanged);
    connect(&AsyncTaskManager::instance(), &AsyncTaskManager::taskFinished,
            this, [this](qint64 taskId, AsyncTaskState state) {
        // A queued task can be canceled before its worker body ever runs, so
        // worker-side cancellation checks alone cannot complete the UI state.
        if (taskId != m_l2PromotionTaskId || state != AsyncTaskState::Canceled)
            return;
        const QString path = m_l2PromotionPath;
        finishLargeTextPromotion(
            nullptr, taskId, path, TextEncoding::Utf8,
            QObject::tr("用户已取消准备编辑，保持只读分析模式"));
    });
}

EditorTab::~EditorTab() {
    // ⚠️  注意：此处是 EditorTab 析构体内部！虽然 EditorTab 刚进入析构、成员还完整，
    //     但 QObject 事件循环可能已派发指向本对象/子对象的 QueuedConnection。
    //     任何 processEvents 调用都可能触发半析构对象的 method invoke → 0xC0000005。
    //
    //  正确的关闭路径是 MainWindow::closeEvent() 在 Step 2.5 中显式调
    //  shutdownL2BeforeClose()（allowProcessEvents=true，MainWindow 完整，UI 事件循环仍活跃）。
    //  那里会完成 cancel LineIndex build task + 轮询终态 + unmap mmap。
    //
    //  此处析构体里仅做 dtor-safe 兜底：如果 MainWindow 路径没执行到（如异常析构），
    //  手动调 closeFile(false)—— 禁止 processEvents，保守等待 500ms 让 worker 自退出。
    cancelLargeTextPromotion();
    clearLargeRecoveryOperations(&m_largeRecoveryOperations);
    clearLargeRecoveryOperations(&m_pendingRecoveryOperations);
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View && m_l2View->isOpen()) {
        m_l2View->closeFile(/*allowProcessEvents=*/false);
    }
}

void EditorTab::shutdownL2BeforeClose() {
    cancelLargeTextPromotion();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (Q_LIKELY(!m_l2Mode || !m_l2View)) return;
    if (!m_l2View->isOpen()) {
        // 虽然没 open，但 Scintilla 可能 reparent 到了 viewport；先回到 stack 避免下一次
        // loadFile L1 找不到 editor 在 stack page 0
        bridgeReparentEditorToStack();
        m_bridge.syncVfvDebounce.stop();
        clearL2BridgeBaseline();
        m_l2Mode = false;
        return;
    }

    // MainWindow 仍活跃：先 flush 最近窗口修改到 patches（不做就可能丢最后编辑），
    // 再 closeFile，再 reparent editor 回 stack。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        qWarning("[EditorTab::shutdownL2BeforeClose] flush overlay→patches 失败（不阻塞关闭，"
                 "可能丢失最近窗口编辑）：%s", qUtf8Printable(flushErr));
    }

    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 mode: closing mmap + "
           "canceling LineIndex build (allow processEvents because MainWindow is still alive).",
           qUtf8Printable(tabTitle()));
    m_l2View->closeFile(/*allowProcessEvents=*/true);
    bridgeReparentEditorToStack();
    m_bridge.syncVfvDebounce.stop();
    clearL2BridgeBaseline();
    m_l2Mode = false;
    qDebug("[EditorTab::shutdownL2BeforeClose] Tab '%s' L2 shutdown done.",
           qUtf8Printable(tabTitle()));
}

void EditorTab::setupEditor()
{
    m_editor->installEventFilter(this);
    if (m_editor->viewport())
        m_editor->viewport()->installEventFilter(this);
    applySharedEditorSurfaceStyle(m_editor);

    // 行号
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->setMarginWidth(0, "99999");

    // 书签 marker（边距 1）
    m_bookmarkMarker = m_editor->markerDefine(QsciScintilla::Circle);
    m_editor->setMarginType(1, QsciScintilla::SymbolMargin);
    m_editor->setMarginWidth(1, 16);
    m_editor->setMarginMarkerMask(1, 1 << m_bookmarkMarker);
    m_editor->setMarkerBackgroundColor(QColor(255, 180, 0), m_bookmarkMarker);
    m_editor->setMarkerForegroundColor(QColor(255, 255, 255), m_bookmarkMarker);

    // New documents follow the Windows convention. Existing documents replace
    // this with their detected dominant line ending before text is installed.
    m_editor->setEolMode(QsciScintilla::EolWindows);

    // 长行标尺（默认 80 列）
    applyEdgeSettings();

    // 横向滚动条按需显示：Scintilla 默认 scrollWidth=2000px，即使空文档也会显示滚动条。
    // setScrollWidthTracking(true) 让 scrollWidth 跟踪实际最长行，配合初始 scrollWidth=1，
    // 只有当某行实际超出视口宽度时才出现横向滚动条。
    // 注意：QScintilla 的 ReconfigureScrollBars() 会覆盖 setHorizontalScrollBarPolicy，
    //       所以必须用 setScrollWidth/setScrollWidthTracking 而非 Qt 策略。
    // 折叠（代码折叠开关）
    m_editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);

    // 括号匹配
    // 自动补全
    m_editor->setAutoCompletionSource(QsciScintilla::AcsAll);
    m_editor->setAutoCompletionThreshold(3);

    // 信号
    connect(m_editor, &QsciScintilla::SCN_MODIFYATTEMPTRO, this, [this]() {
        emit modificationChanged(true);
    });
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, [this](int line, int index) {
        if (!isL2Mode() || !m_l2View
            || m_l2View->viewMode() != ViewMode::Hex
            || m_bridge.windowStart < 0) {
            emit cursorPositionChanged(line, index);
            return;
        }

        const qint64 local = qBound<qint64>(0,
                                             qint64(line) * m_hexBytesPerRow + index / 3,
                                             qMax<qint64>(0, m_bridge.windowLen));
        m_lastL2Line = line;
        m_lastL2Col = int(local % m_hexBytesPerRow);
        m_lastL2Byte = qMin(m_l2View->virtualFileSize(),
                            m_bridge.windowStart + local);
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    });
    connect(m_editor, &QsciScintilla::modificationChanged,
            this, [this](bool m) {
        emit modificationChanged(m);
        emit titleChanged(tabTitle());
    });
    connect(m_editor, &QsciScintilla::SCN_MODIFIED, this,
            [this](int position, int modificationType, const char *text, int length, int,
                   int, int, int, int, int) {
        constexpr int contentChanges = QsciScintillaBase::SC_MOD_INSERTTEXT
                                       | QsciScintillaBase::SC_MOD_DELETETEXT;
        if (!m_bridge.applying && (modificationType & contentChanges)) {
            advanceContentRevision();
            if (m_promotedLargeDocument && !m_replayingRecoveryJournal && length > 0) {
                if ((modificationType & QsciScintillaBase::SC_MOD_INSERTTEXT) && text) {
                    const QByteArray inserted(text, length);
                    if (!m_largeRecoveryOperations.isEmpty()) {
                        auto &last = m_largeRecoveryOperations.last();
                        if (last.kind == LargeRecoveryOperation::Kind::Insert
                            && last.position + last.insertedBytes.size() == position) {
                            last.insertedBytes.append(inserted);
                            return;
                        }
                    }
                    LargeRecoveryOperation operation;
                    operation.kind = LargeRecoveryOperation::Kind::Insert;
                    operation.position = position;
                    operation.insertedBytes = inserted;
                    m_largeRecoveryOperations.push_back(std::move(operation));
                } else if (modificationType & QsciScintillaBase::SC_MOD_DELETETEXT) {
                    if (!m_largeRecoveryOperations.isEmpty()) {
                        auto &last = m_largeRecoveryOperations.last();
                        if (last.kind == LargeRecoveryOperation::Kind::Delete
                            && last.position == position) {
                            last.deleteLength += length;
                            return;
                        }
                    }
                    LargeRecoveryOperation operation;
                    operation.kind = LargeRecoveryOperation::Kind::Delete;
                    operation.position = position;
                    operation.deleteLength = length;
                    m_largeRecoveryOperations.push_back(std::move(operation));
                }
            }
        }
    });
}

bool EditorTab::loadFile(const QString &path)
{
    m_metadataModified = false;
    m_binaryDocument = false;
    cancelLargeTextPromotion();
    resetLargeRecoveryJournal(QString());
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2TextPreviewReadOnly = false;
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(path).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile] ⚠️  大文件(%lld MB)直接走了 L1 loadFile！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openTabAutoMode → openLargeFileL2 窗口化秒开。"
                  " path='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(path));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadFile]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadFile] readAll 小文件 %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    m_editor->setEolMode(detectLineEndingMode(result.text));
    m_filePath = QFileInfo(path).absoluteFilePath();
    m_autosavePath.clear();  // 加载正式文件，清除 autosave 关联

    // 大文件分块加载：避免 setText() 一次性重建整个文档导致 UI 长时间卡死
    // （Notepad++ / Scintilla 的 ILoader 也是增量加载，此处用 SCI_APPENDTEXT 分块追加）
    const int largeThreshold = 2 * 1024 * 1024; // 2MB 以上走分块路径
    if (data.size() < largeThreshold) {
        // 重置 scrollWidth：tracking 模式只增不减，切换文件时需手动归零
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        // UTF-8 文件可直接使用原始字节，跳过 QString 中间层以节省内存和时间
        // 非 UTF-8 文件需经 QString 转换为 UTF-8 字节
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3); // 跳过 3 字节 BOM
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(false);

    // 成功加载文件：切回 L1 显示，关闭 L2 模式（若之前曾打开 L2）。
    // L2 会把 Scintilla 临时 reparent 到 VirtualFileView 的 viewport；reparent 时
    // QStackedWidget 会自动移除该页，因此不能假设 L1 永远仍是 index 0。
    bridgeReparentEditorToStack();
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    m_l2Mode = false;
    emit l2ModeStatusChanged();
    if (m_stack) m_stack->setCurrentWidget(m_editor);

    // 根据后缀猜测语法
    Syntax s = guessSyntaxFromExtension(path);
    setSyntax(s);

    refreshDiskVersionBaseline();

    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::loadAutosave(const QString &autosavePath)
{
    QElapsedTimer t; t.start();
    const qint64 size = QFileInfo(autosavePath).size();
    const FileTierDecision tier = FileTierPolicy::classify(size);
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave] ⚠️  大 autosave(%lld MB)直接走了 L1 loadAutosave！"
                  " 这会阻塞 UI 线程 %lld ms+，导致启动时'全程静默无弹窗直到加载完'！"
                  " 正确路径应是 openAutosaveAutoMode → 窗口化大文件恢复。"
                  " autosavePath='%s'",
                  size / (1024*1024), qMax(1LL, size / (50 * 1024 * 1024)),
                  qUtf8Printable(autosavePath));
    }
    QFile file(autosavePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray data = file.readAll();
    file.close();
    const qint64 msReadAll = t.elapsed();
    if (tier.usesMappedPreview()) {
        qCritical("[EditorTab::loadAutosave]   ⚠️  readAll(%lld MB) 主线程同步耗时 = %lld ms → "
                  "这就是用户以为'窗口没弹出'的根因（事件循环无法派发 Show/Paint 事件）",
                  size / (1024*1024), msReadAll);
    } else {
        qDebug("[EditorTab::loadAutosave] readAll 小 autosave %lld KB, 耗时=%lld ms (OK)",
               size / 1024, msReadAll);
    }

    return loadAutosaveBytes(data, autosavePath);
}

bool EditorTab::loadAutosaveBytes(const QByteArray &data, const QString &backingPath)
{
    m_metadataModified = false;
    m_binaryDocument = false;
    if (data.size() >= FileTierPolicy::StandardEditLimitBytes)
        return false;
    cancelLargeTextPromotion();
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2TextPreviewReadOnly = false;

    DecodeResult result = decodeBytes(data);
    m_encoding = result.encoding;
    m_editor->setEolMode(detectLineEndingMode(result.text));
    // 关键：不设置 m_filePath，保持未命名状态，对用户透明
    m_autosavePath = backingPath;
    m_diskVersion = {};

    // 设置文本（但不清除 modified 标记，让用户知道内容是上次未保存的）
    const int largeThreshold = 2 * 1024 * 1024;
    if (data.size() < largeThreshold) {
        m_editor->setScrollWidth(1);
        m_editor->setText(result.text);
    } else {
        QByteArray utf8;
        if (m_encoding == TextEncoding::Utf8) {
            utf8 = data;
        } else if (m_encoding == TextEncoding::Utf8Bom) {
            utf8 = data.mid(3);
        } else {
            utf8 = result.text.toUtf8();
        }
        loadUtf8Chunked(utf8);
    }
    m_editor->setModified(true);

    // autosave 不猜语法，保持 Plain 或已设置的语法

    emit encodingChanged(m_encoding);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::saveFile(QString *errMsg)
{
    if (isUntitled()) {
        if (errMsg) *errMsg = QObject::tr("未命名文件，请先指定保存路径。");
        return false;
    }
    // Match native editor semantics: Save on an unchanged named document is a
    // no-op. Binary files are routed through the mapped provider even at 9 MiB;
    // rewriting that entire backing file merely because Ctrl+S was pressed
    // created a visible sibling temporary copy for no user-visible change.
    if (!isModified())
        return true;
    if (isL2Mode())
        return saveL2As(m_filePath, errMsg);
    return saveAsFile(m_filePath, errMsg);
}

bool EditorTab::saveAsFile(const QString &path, QString *errMsg)
{
    if (sameDiskFilePath(m_filePath, path)) {
        const ExternalFileState diskState = externalFileState();
        if (diskState == ExternalFileState::Changed
            || diskState == ExternalFileState::Missing
            || diskState == ExternalFileState::Unreadable) {
            if (errMsg) {
                *errMsg = QObject::tr(
                    "目标仍是已被外部修改的原文件。请选择其他路径，或先重新加载磁盘版本。");
            }
            return false;
        }
    }
    if (isL2Mode())
        return saveL2As(path, errMsg);
    if (m_promotedLargeDocument)
        return savePromotedLargeAs(path, errMsg);

    QByteArray data = encodeText(m_editor->text(), m_encoding);

    // QSaveFile already writes a sibling temporary file and atomically replaces
    // the destination on commit. A second remove/rename layer created a data-loss
    // window when the final rename failed.
    QSaveFile saveFile(path);
    saveFile.setDirectWriteFallback(false);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        if (errMsg) *errMsg = QObject::tr("无法打开文件进行原子写入：%1").arg(path);
        return false;
    }
    qint64 written = saveFile.write(data);
    if (written != data.size()) {
        if (errMsg) *errMsg = QObject::tr("写入临时文件不完整：%1/%2 字节").arg(written).arg(data.size());
        return false;
    }
    if (!saveFile.commit()) {
        if (errMsg) *errMsg = QObject::tr("原子提交临时文件失败：%1").arg(saveFile.errorString());
        return false;
    }

    m_filePath = QFileInfo(path).absoluteFilePath();
    setModified(false);

    // 保存成功：自动清理 autosave 临时文件（自清理机制）
    clearAutosaveFile();
    m_autosavePath.clear();
    refreshDiskVersionBaseline();

    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::savePromotedLargeAs(const QString &path, QString *errMsg)
{
    if (!m_promotedLargeDocument || path.isEmpty()) {
        if (errMsg)
            *errMsg = QObject::tr("大文件保存参数无效。");
        return false;
    }

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (errMsg)
            *errMsg = QObject::tr("无法建立原子保存文件：%1").arg(output.errorString());
        return false;
    }

    m_editor->setReadOnly(true);
    const auto restoreEditable = [this]() { m_editor->setReadOnly(false); };
    if (!streamPromotedDocumentTo(&output, errMsg, true)) {
        output.cancelWriting();
        restoreEditable();
        return false;
    }
    if (!output.commit()) {
        restoreEditable();
        if (errMsg)
            *errMsg = QObject::tr("原子提交大文件失败：%1").arg(output.errorString());
        return false;
    }

    restoreEditable();
    if (auto *window = qobject_cast<QMainWindow *>(this->window()))
        window->statusBar()->showMessage(QObject::tr("大文件已保存"), 3000);
    m_filePath = QFileInfo(path).absoluteFilePath();
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_promotedLargeDirty = false;
    resetLargeRecoveryJournal(m_filePath);
    clearAutosaveFile();
    m_autosavePath.clear();
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::streamPromotedDocumentTo(QIODevice *output, QString *errMsg,
                                         bool yieldUi) const
{
    if (!output || !output->isWritable()) {
        if (errMsg)
            *errMsg = QObject::tr("大文件输出设备不可写。");
        return false;
    }

    QByteArray bom;
    switch (m_encoding) {
    case TextEncoding::Utf8Bom:  bom = QByteArray("\xef\xbb\xbf", 3); break;
    case TextEncoding::Utf16LE:  bom = QByteArray("\xff\xfe", 2); break;
    case TextEncoding::Utf16BE:  bom = QByteArray("\xfe\xff", 2); break;
    default: break;
    }
    if (!bom.isEmpty() && output->write(bom) != bom.size()) {
        if (errMsg)
            *errMsg = QObject::tr("写入文件编码标记失败。");
        return false;
    }

    std::unique_ptr<QStringDecoder> decoder;
    std::unique_ptr<QStringEncoder> encoder;
    if (m_encoding != TextEncoding::Utf8 && m_encoding != TextEncoding::Utf8Bom) {
        decoder = std::make_unique<QStringDecoder>(QStringConverter::Utf8);
        if (m_encoding == TextEncoding::Utf16LE)
            encoder = std::make_unique<QStringEncoder>(QStringConverter::Utf16LE);
        else if (m_encoding == TextEncoding::Utf16BE)
            encoder = std::make_unique<QStringEncoder>(QStringConverter::Utf16BE);
        else
            encoder = std::make_unique<QStringEncoder>(QStringConverter::System);
    }

    const qint64 total = static_cast<qint64>(directScintilla(
        m_editor, QsciScintillaBase::SCI_GETLENGTH));
    constexpr qint64 chunkBytes = 8LL * 1024 * 1024;
    auto *window = yieldUi ? qobject_cast<QMainWindow *>(this->window()) : nullptr;
    for (qint64 position = 0; position < total;) {
        const qint64 length = qMin(chunkBytes, total - position);
        const char *data = reinterpret_cast<const char *>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETRANGEPOINTER,
            static_cast<ScintillaUnsigned>(position),
            static_cast<ScintillaSigned>(length)));
        if (!data) {
            if (errMsg)
                *errMsg = QObject::tr("读取 Scintilla 大文件快照失败。");
            return false;
        }
        QByteArray encoded;
        if (decoder && encoder) {
            const QString decoded = (*decoder)(QByteArrayView(data, length));
            encoded = (*encoder)(QStringView(decoded));
            data = encoded.constData();
            if (decoder->hasError() || encoder->hasError()) {
                if (errMsg)
                    *errMsg = QObject::tr("按原编码转换大文件时遇到无效字符。");
                return false;
            }
        }
        const qint64 outputLength = encoded.isNull() ? length : encoded.size();
        if (output->write(data, outputLength) != outputLength) {
            if (errMsg)
                *errMsg = QObject::tr("流式写入大文件失败。");
            return false;
        }
        position += length;
        if (window && window->statusBar()) {
            window->statusBar()->showMessage(
                QObject::tr("正在保存大文件… %1%").arg(
                    total > 0 ? int((position * 100) / total) : 100));
        }
        if (yieldUi)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    return true;
}

bool EditorTab::isModified() const
{
    if (isL2Mode())
        // Overlay edits become PatchList entries at the next window switch or
        // save. They must still participate in close prompts immediately.
        return m_l2SaveNeeded || m_editor->isModified() || m_metadataModified;
    if (m_promotedLargeDocument)
        return m_promotedLargeDirty || m_editor->isModified() || m_metadataModified;
    return m_editor->isModified() || m_metadataModified;
}

void EditorTab::setModified(bool m)
{
    if (isL2Mode()) {
        const bool before = isModified();
        m_l2SaveNeeded = m;
        if (!m) {
            m_metadataModified = false;
            m_editor->setModified(false);
        }
        if (before != isModified()) {
            emit modificationChanged(isModified());
            emit titleChanged(tabTitle());
        }
        return;
    }
    if (m_promotedLargeDocument) {
        const bool before = isModified();
        m_promotedLargeDirty = m;
        if (!m) {
            m_metadataModified = false;
            m_editor->setModified(false);
        }
        if (before != isModified()) {
            emit modificationChanged(isModified());
            emit titleChanged(tabTitle());
        }
        return;
    }
    const bool before = isModified();
    m_metadataModified = m;
    if (!m)
        m_editor->setModified(false);
    if (before != isModified()) {
        emit modificationChanged(isModified());
        emit titleChanged(tabTitle());
    }
}

bool EditorTab::isUntitled() const
{
    return m_filePath.isEmpty();
}

QString EditorTab::filePath() const
{
    return m_filePath;
}

void EditorTab::setFilePath(const QString &path)
{
    m_filePath = path;
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
}

QString EditorTab::fileName() const
{
    if (isSecureDocument()) {
        if (!m_filePath.isEmpty())
            return QFileInfo(m_filePath).fileName();
        return m_secureDocumentTitle.isEmpty() ? QStringLiteral("未命名")
                                               : m_secureDocumentTitle;
    }
    if (m_filePath.isEmpty())
        return QStringLiteral("未命名");
    return QFileInfo(m_filePath).fileName();
}

QString EditorTab::tabTitle() const
{
    QString title = fileName();
    if (isModified())
        title += QStringLiteral(" ●");
    return title;
}

void EditorTab::initializeSecureDocument(const QUuid &id, const QString &title,
                                         const QString &plainText,
                                         const QString &filePath)
{
    Q_ASSERT(!id.isNull());
    m_metadataModified = false;
    cancelLargeTextPromotion();
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2TextPreviewReadOnly = false;
    m_secureDocumentTransitioning = true;
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    if (m_l2View && m_l2View->isOpen())
        m_l2View->closeFile(true);
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(0);
    m_l2Mode = false;
    m_filePath = filePath;
    m_autosavePath.clear();
    m_secureDocumentId = id;
    m_secureDocumentTitle = title.trimmed();
    m_secureDocumentMasked = false;
    m_secureBinaryDocument = false;
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);
    m_binaryDocument = false;
    m_diskVersion = {};
    m_encoding = TextEncoding::Utf8;
    m_editor->setEolMode(detectLineEndingMode(plainText));
    m_editor->setScrollWidth(1);
    m_editor->setReadOnly(false);
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_CLEARALL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->setText(plainText);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    if (m_stack)
        m_stack->setCurrentWidget(m_editor);
    setSyntax(Syntax::Markdown);
    emit l2ModeStatusChanged();
    emit encodingChanged(m_encoding);
    emit titleChanged(tabTitle());
}

bool EditorTab::initializeSecureBinaryDocument(const QUuid &id,
                                               const QString &title,
                                               QByteArray bytes,
                                               const QString &filePath,
                                               QString *error)
{
    if (id.isNull()) {
        if (error)
            *error = tr("安全二进制文档标识无效。");
        if (!bytes.isEmpty())
            bytes.fill(0);
        return false;
    }
    cancelLargeTextPromotion();
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2TextPreviewReadOnly = false;
    m_secureDocumentTransitioning = true;
    bridgeReparentEditorToStack();
    clearL2BridgeBaseline();
    m_l2Mode = false;

    VirtualFileView *view = ensureL2View();
    QString localError;
    if (!view->openMemory(std::move(bytes), ViewMode::Hex, &localError)) {
        m_secureDocumentTransitioning = false;
        if (error)
            *error = localError;
        return false;
    }
    setSecurePayloadLimitExclusive(32LL * 1024 * 1024);

    m_filePath = filePath;
    m_autosavePath.clear();
    m_secureDocumentId = id;
    m_secureDocumentTitle = title.trimmed();
    m_secureDocumentMasked = false;
    m_secureBinaryDocument = true;
    m_binaryDocument = true;
    m_diskVersion = {};
    m_encoding = TextEncoding::Utf8;
    m_l2Mode = true;
    m_l2ReadOnlyByPolicy = false;
    m_l2TextPreviewReadOnly = false;
    m_l2SaveNeeded = false;
    m_lastL2Line = 0;
    m_lastL2Col = 0;
    m_lastL2Byte = 0;
    m_syntax = Syntax::Plain;
    activateL2Overlay(view);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    emit l2ModeStatusChanged();
    emit encodingChanged(m_encoding);
    emit syntaxChanged(m_syntax);
    emit titleChanged(tabTitle());
    emit cursorPositionChanged(0, 0);
    return true;
}

bool EditorTab::exportBinaryPayloadBytes(QByteArray *bytes, QString *error)
{
    if (bytes)
        bytes->clear();
    if (!bytes || !isBinaryDocument() || isSecureDocumentMasked()
        || !m_l2View || !m_l2View->isOpen()) {
        if (error)
            *error = tr("安全二进制文档当前不可读取。");
        return false;
    }
    if (!bridgeFlushOverlayToPatches(error))
        return false;
    const qint64 size = m_l2View->virtualFileSize();
    if (size < 0 || size >= 32LL * 1024 * 1024) {
        if (error)
            *error = tr("安全文档必须小于 32 MiB。");
        return false;
    }
    *bytes = m_l2View->readByteRange(0, size);
    return bytes->size() == size;
}

void EditorTab::setSecurePayloadLimitExclusive(qint64 limit)
{
    m_securePayloadLimitExclusive = qMax<qint64>(0, limit);
    if (m_l2View)
        m_l2View->setVirtualSizeLimitExclusive(m_securePayloadLimitExclusive);
}

void EditorTab::maskSecureDocument()
{
    if (!isSecureDocument() || m_secureDocumentMasked)
        return;
    m_secureDocumentTransitioning = true;
    m_secureDocumentMasked = true;
    if (m_secureBinaryDocument) {
        bridgeReparentEditorToStack();
        if (m_l2View && m_l2View->isOpen())
            m_l2View->closeFile(true);
        m_l2Mode = false;
        if (m_stack)
            m_stack->setCurrentWidget(m_editor);
        clearL2BridgeBaseline();
        emit l2ModeStatusChanged();
    }
    m_editor->setReadOnly(false);
    // Disable undo before removing plaintext so neither Ctrl+Z nor Scintilla's
    // undo buffer can resurrect the secure document while the tab is masked.
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_CLEARALL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->setText(QStringLiteral(
        "此加密文档已锁定。\n\n解锁加密数据后可继续编辑。"));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setModified(false);
    m_editor->setReadOnly(true);
    m_secureDocumentTransitioning = false;
    emit titleChanged(tabTitle());
}

void EditorTab::setSecureDocumentTitle(const QString &title)
{
    if (!isSecureDocument())
        return;
    const QString normalized = title.trimmed();
    if (m_secureDocumentTitle == normalized)
        return;
    m_secureDocumentTitle = normalized;
    emit titleChanged(tabTitle());
}

void EditorTab::convertToPlainDocument(const QString &filePath)
{
    m_metadataModified = false;
    cancelLargeTextPromotion();
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2TextPreviewReadOnly = false;
    m_secureDocumentTransitioning = true;
    clearL2BridgeBaseline();
    m_secureDocumentId = {};
    m_secureDocumentTitle.clear();
    m_secureDocumentMasked = false;
    m_secureBinaryDocument = false;
    setSecurePayloadLimitExclusive(0);
    m_filePath = filePath;
    m_autosavePath.clear();
    m_editor->setReadOnly(false);
    m_editor->setModified(false);
    m_secureDocumentTransitioning = false;
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
}

TextEncoding EditorTab::encoding() const
{
    return m_encoding;
}

void EditorTab::setEncoding(TextEncoding enc)
{
    if (m_encoding == enc)
        return;
    m_encoding = enc;
    emit encodingChanged(m_encoding);
}

QString EditorTab::lineEndingName() const
{
    switch (m_editor->eolMode()) {
    case QsciScintilla::EolWindows: return QStringLiteral("CRLF");
    case QsciScintilla::EolMac: return QStringLiteral("CR");
    case QsciScintilla::EolUnix: return QStringLiteral("LF");
    }
    return QStringLiteral("LF");
}

Syntax EditorTab::syntax() const
{
    return m_syntax;
}

void EditorTab::setSyntax(Syntax syntax)
{
    if (m_syntax == syntax)
        return;
    m_syntax = syntax;
    applySyntax(syntax);
    emit syntaxChanged(m_syntax);
}

Syntax EditorTab::guessSyntaxFromExtension(const QString &fileName)
{
    QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "hh" || ext == "hxx" || ext == "c" || ext == "inl")
        return Syntax::Cpp;
    if (ext == "py" || ext == "pyw")
        return Syntax::Python;
    if (ext == "json")
        return Syntax::Json;
    if (ext == "html" || ext == "htm" || ext == "xhtml")
        return Syntax::Html;
    if (ext == "xml" || ext == "svg" || ext == "ui" || ext == "pro")
        return Syntax::Xml;
    if (ext == "sql")
        return Syntax::Sql;
    if (ext == "yaml" || ext == "yml")
        return Syntax::Yaml;
    if (ext == "md" || ext == "markdown")
        return Syntax::Markdown;
    return Syntax::Plain;
}

QsciLexer *EditorTab::createLexer(Syntax syntax) const
{
    switch (syntax) {
    case Syntax::Cpp:      return new QsciLexerCPP(m_editor);
    case Syntax::Python:   return new QsciLexerPython(m_editor);
    case Syntax::Json:     return new QsciLexerJSON(m_editor);
    case Syntax::Html:     return new QsciLexerHTML(m_editor);
    case Syntax::Xml:      return new QsciLexerXML(m_editor);
    case Syntax::Sql:      return new QsciLexerSQL(m_editor);
    case Syntax::Yaml:     return new QsciLexerYAML(m_editor);
    case Syntax::Markdown: return new QsciLexerMarkdown(m_editor);
    case Syntax::Plain:    return nullptr;
    }
    return nullptr;
}

void EditorTab::applySyntax(Syntax syntax)
{
    QsciLexer *lexer = createLexer(syntax);
    if (lexer) {
        QFont font = m_editor->font();
        lexer->setDefaultFont(font);
    }
    m_editor->setLexer(lexer);
    // setLexer 接管 ownership，旧 lexer 自动删除

    // 大文件启用 idle styling：只对可见区域着色，剩余部分在空闲时逐步完成，
    // 避免 setLexer 后一次性对全文档做词法分析导致 UI 卡顿
    // （Notepad++ 对大文件也采用类似策略）
    const int docLen = m_editor->length();
    if (docLen > 5 * 1024 * 1024) { // > 5MB
        // SC_IDLESTYLING_TOVISIBLE=1: 只着色到可见区域，其余在 idle 时完成
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 1UL);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETIDLESTYLING, 0UL);
    }
}

// ============ 行操作 ============

void EditorTab::duplicateLine()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    QString lineText = m_editor->text(line);
    m_editor->insertAt(lineText + "\n", line, 0);
    m_editor->setCursorPosition(line + 1, index);
}

void EditorTab::deleteLine()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    m_editor->setSelection(line, 0, line + 1, 0);
    m_editor->removeSelectedText();
}

void EditorTab::moveLineUp()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    if (line == 0)
        return;
    QString current = m_editor->text(line);
    QString above = m_editor->text(line - 1);
    // 替换上一行
    m_editor->setSelection(line - 1, 0, line, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(current + "\n", line - 1, 0);
    m_editor->setSelection(line, 0, line + 1, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(above + "\n", line, 0);
    m_editor->setCursorPosition(line - 1, index);
}

void EditorTab::moveLineDown()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    int lines = m_editor->lines();
    if (line >= lines - 1)
        return;
    QString current = m_editor->text(line);
    QString below = m_editor->text(line + 1);
    m_editor->setSelection(line + 1, 0, line + 2, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(below + "\n", line, 0);
    m_editor->setSelection(line + 1, 0, line + 2, 0);
    m_editor->removeSelectedText();
    m_editor->insertAt(current + "\n", line + 1, 0);
    m_editor->setCursorPosition(line + 1, index);
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

    Pcre2Regex compiled;
    QString compileError;
    if (!compiled.compile(pattern, !regex, caseSensitive, wholeWord, &compileError)) {
        m_lastSearchError = compileError;
        return matches;
    }

    const QByteArray subject = m_editor->text().toUtf8();
    QString scanError;
    const Pcre2Regex::ScanResult scanResult = compiled.scan(
        reinterpret_cast<const uchar *>(subject.constData()), subject.size(),
        {}, {},
        [&](qint64 byteOffset, qint64 byteLength) {
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
            if (matches.size() >= 200000) {
                m_lastFindAllTruncated = true;
                return false;
            }
            return true;
        }, &scanError);
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
    m_l1SearchCachePattern = pattern;
    m_l1SearchCacheRegex = regex;
    m_l1SearchCacheCaseSensitive = caseSensitive;
    m_l1SearchCacheWholeWord = wholeWord;
    m_l1SearchCacheRevision = m_contentRevision;
    m_l1SearchCacheValid = true;
    return matches;
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
    m_editor->getCursorPosition(line, index);
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
    m_editor->setCursorPosition(line, index);
    m_editor->ensureLineVisible(line);
}

// ============ 长行标尺 ============

void EditorTab::applyEdgeSettings()
{
    m_editor->setEdgeColumn(m_edgeColumn);
    if (m_edgeVisible)
        m_editor->setEdgeMode(QsciScintilla::EdgeLine);
    else
        m_editor->setEdgeMode(QsciScintilla::EdgeNone);
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

bool EditorTab::isEdgeVisible() const
{
    return m_edgeVisible;
}

// ============ 编码切换 ============

void EditorTab::reloadWithEncoding(TextEncoding enc)
{
    if (m_encoding == enc)
        return;
    // 重新解释内容：当前 Unicode 文本 → 用旧编码还原字节 → 用新编码解码
    // 但更实用的做法是：如果磁盘上有文件，从磁盘按新编码重读；否则保持文本不变只改编码标记
    if (!isUntitled() && QFileInfo::exists(m_filePath)) {
        // 只有在用户确认后才重载（由上层调用 saveFile 检查），这里简单切换编码标记
        m_encoding = enc;
        emit encodingChanged(m_encoding);
        return;
    }
    // 未命名/内存中：简单切换编码标记（下次保存用新编码）
    m_encoding = enc;
    emit encodingChanged(m_encoding);
}

// ============ autosave 自清理 ============

void EditorTab::clearAutosaveFile() const
{
    if (!m_autosavePath.isEmpty() && QFile::exists(m_autosavePath)) {
        QFile::remove(m_autosavePath);
    }
}

// ============ 大文件分块加载 ============

void EditorTab::loadUtf8Chunked(const QByteArray &utf8)
{
    // 通过 Scintilla 底层 SCI_APPENDTEXT 分块追加文本，
    // 比 setText(整个文本) 快很多倍：setText 会一次性重建行索引、
    // 触发全量重排，而 SCI_APPENDTEXT 是增量追加，Scintilla 内部只做最小化更新。
    //
    // 参考：Notepad++ / Scintilla 的 ILoader 也是增量加载机制，
    //       QScintilla 2.14 未直接暴露 ILoader API，故用 SCI_APPENDTEXT 替代。
    //
    // !! 重要：不能调用 setReadOnly(true) 来防止用户在加载期间编辑 !!
    // 因为 Scintilla 的 Document::InsertString 内部会检查 cb.IsReadOnly()，
    // readOnly 状态下 SCI_APPENDTEXT 会直接返回 0 不插入任何内容，
    // 导致加载完成后编辑框为空（进度条正常显示但内容未写入）。
    // 防干扰改为：每 chunk 间 processEvents(ExcludeUserInputEvents) 仅重绘不接受输入；
    // 且 L1 阈值仅 ~128MB（秒级完成），故不再弹出 WindowModal QProgressDialog。

    // 关闭 undo 收集：加载过程不产生 undo 步骤
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 0UL);

    // 清空已有内容并重置 scrollWidth（tracking 只增不减，需手动归零）
    m_editor->setScrollWidth(1);
    m_editor->clear();

    // 加载期间临时关闭换行：WrapWord 模式下每追加一段文本都会触发
    // 整个可见区的换行重算，大文件时极慢；加载完成后恢复
    QsciScintilla::WrapMode savedWrap = m_editor->wrapMode();
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(QsciScintilla::WrapNone);

    const char *raw = utf8.constData();
    const int total = utf8.size();
    const int chunkSize = 512 * 1024; // 512KB per chunk

    // 状态栏非模态进度（若当前窗口为 QMainWindow）；无则静默。
    auto *mw = qobject_cast<QMainWindow*>(this->window());
    const int reportEvery = qMax(1, (2 * 1024 * 1024) / qMax(1, chunkSize)); // ~每 2MB 报一次
    int chunkNo = 0;

    int pos = 0;
    while (pos < total) {
        int end = qMin(pos + chunkSize, total);
        // 回退到完整 UTF-8 字符边界：不截断多字节字符
        while (end < total &&
               (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80)
            --end;
        // 防御：极端情况下整个 chunk 都是 UTF-8 后续字节，end 会回退到 pos 之前，
        // 此时不再回退，直接使用 chunkSize 范围（可能截断 1 个多字节字符，但避免死循环/负长度）
        if (end <= pos)
            end = qMin(pos + chunkSize, total);

        int len = end - pos;
        m_editor->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT,
                                static_cast<uintptr_t>(len), raw + pos);

        pos = end;
        ++chunkNo;
        if ((chunkNo % reportEvery) == 0 || pos >= total) {
            if (mw && mw->statusBar() && total > 0) {
                int pct = int((qint64(pos) * 100) / qint64(total));
                mw->statusBar()->showMessage(
                    QObject::tr("正在加载文件… %1% (%2 / %3 MB)")
                        .arg(pct)
                        .arg(pos / (1024 * 1024))
                        .arg(total / (1024 * 1024)),
                    0);
            }
        }
        // 仅允许重绘（不接受鼠标/键盘输入），避免模态且防止用户编辑中状态不一致
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mw && mw->statusBar()) mw->statusBar()->clearMessage();

    // 恢复换行模式
    if (savedWrap != QsciScintilla::WrapNone)
        m_editor->setWrapMode(savedWrap);

    // 恢复 undo 收集并清空 undo 缓冲（加载步骤不可 undo）
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
}

// ============ 自动换行 ============

void EditorTab::setWordWrap(bool on)
{
    if (windowedReadOnlyActive()) {
        m_l2View->setWordWrap(on);
        if (m_windowedReadOnlyProvider->presentation()
            == WindowedReadOnlyProvider::Presentation::Text) {
            m_windowedReadOnlyProvider->setWordWrap(on);
        }
        return;
    }
    // L2 模式：委托 VirtualFileView
    if (isL2Mode() && m_l2View) {
        m_l2View->setWordWrap(on);
        if (m_l2View->viewMode() != ViewMode::Hex) {
            m_editor->setWrapMode(on ? QsciScintilla::WrapWord
                                     : QsciScintilla::WrapNone);
            if (on) {
                m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
                m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
            }
        }
        return;
    }
    // L1 模式：QsciScintilla
    if (on) {
        m_editor->setWrapMode(QsciScintilla::WrapWord);
        m_editor->setWrapVisualFlags(QsciScintilla::WrapFlagByText);
        m_editor->setWrapIndentMode(QsciScintilla::WrapIndentSame);
    } else {
        m_editor->setWrapMode(QsciScintilla::WrapNone);
    }
}

bool EditorTab::wordWrap() const
{
    if (isL2Mode() && m_l2View)
        return m_l2View->wordWrap();
    return m_editor->wrapMode() != QsciScintilla::WrapNone;
}

void EditorTab::setDisplayEncoding(TextEncoding enc)
{
    if (isL2Mode() && m_l2View) {
        QString error;
        if (!bridgeFlushOverlayToPatches(&error)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(error, 6000);
            return;
        }
        // L2: preserve the virtual byte stream and rebuild only the visible
        // decoded projection. Pending patches remain attached to byte offsets.
        m_l2View->setDisplayEncoding(enc);
        const qint64 start = m_l2View->activeWindowByteStart();
        const qint64 end = m_l2View->activeWindowByteEnd();
        clearL2BridgeBaseline();
        if (end > start)
            bridgePopulateScintilla(start, end);
        emit encodingChanged(enc);
    } else {
        // Secure Text is a logical UTF-8 payload; m_filePath points to its
        // encrypted KDBX container and must never be decoded as document text.
        if (m_filePath.isEmpty() || isSecureDocument()) return;
        QFile f(m_filePath);
        if (!f.open(QIODevice::ReadOnly)) return;
        QByteArray raw = f.readAll();
        f.close();
        int skip = 0;
        if (enc == TextEncoding::Utf8Bom && raw.size() >= 3 &&
            (uchar)raw[0] == 0xEF && (uchar)raw[1] == 0xBB && (uchar)raw[2] == 0xBF)
            skip = 3;
        else if (enc == TextEncoding::Utf16LE && raw.size() >= 2
                 && uchar(raw[0]) == 0xFF && uchar(raw[1]) == 0xFE)
            skip = 2;
        else if (enc == TextEncoding::Utf16BE && raw.size() >= 2
                 && uchar(raw[0]) == 0xFE && uchar(raw[1]) == 0xFF)
            skip = 2;
        QByteArray body = raw.mid(skip);
        QString text;
        switch (enc) {
        case TextEncoding::Utf8:
        case TextEncoding::Utf8Bom:
            text = QString::fromUtf8(body);
            break;
        case TextEncoding::GBK: {
            auto decoder = QStringDecoder("GB18030");
            text = decoder.isValid() ? decoder(body) : QString::fromLatin1(body);
            break;
        }
        case TextEncoding::Utf16LE:
            text = QString::fromUtf16(reinterpret_cast<const char16_t*>(body.constData()),
                                      body.size() / 2);
            break;
        case TextEncoding::Utf16BE: {
            QByteArray swapped = body;
            for (int i = 0; i + 1 < swapped.size(); i += 2)
                std::swap(swapped[i], swapped[i+1]);
            text = QString::fromUtf16(reinterpret_cast<const char16_t*>(swapped.constData()),
                                      swapped.size() / 2);
            break;
        }
        }
        m_editor->setEolMode(detectLineEndingMode(text));
        m_editor->setText(text);
        m_editor->setModified(false);
        m_metadataModified = false;
        m_encoding = enc;
        refreshDiskVersionBaseline();
        emit encodingChanged(m_encoding);
    }
}

TextEncoding EditorTab::displayEncoding() const
{
    if (isL2Mode() && m_l2View)
        return m_l2View->displayEncoding();
    return m_encoding;
}

// ============================================================================
//  L2 视图模式（Text / Hex）
// ============================================================================
void EditorTab::setViewModeL2(ViewMode mode)
{
    if (!m_l2View || m_l2View->viewMode() == mode)
        return;

    QString error;
    if (!bridgeFlushOverlayToPatches(&error)) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(error, 6000);
        return;
    }

    m_l2View->setViewMode(mode);
    if (windowedReadOnlyActive()) {
        m_windowedReadOnlyProvider->setPresentation(
            mode == ViewMode::Hex ? WindowedReadOnlyProvider::Presentation::Hex
                                  : WindowedReadOnlyProvider::Presentation::Text);
        return;
    }
    // The overlay representation changes between decoded text and a reversible
    // two-digit byte grid. Refill even when the mmap active range did not move.
    const qint64 start = m_l2View->activeWindowByteStart();
    const qint64 end = m_l2View->activeWindowByteEnd();
    clearL2BridgeBaseline();
    if (end > start)
        bridgePopulateScintilla(start, end);
}

ViewMode EditorTab::viewModeL2() const
{
    return m_l2View ? m_l2View->viewMode() : ViewMode::Text;
}

void EditorTab::setDisplayEncodingL2(TextEncoding enc)
{
    if (m_l2View) m_l2View->setDisplayEncoding(enc);
}

TextEncoding EditorTab::displayEncodingL2() const
{
    return m_l2View ? m_l2View->displayEncoding() : TextEncoding::Utf8;
}

bool EditorTab::isL2Active() const
{
    // 不能用 currentIndex()==1：进入 L2 后 Scintilla 会被 reparent 到 VFV 的
    // viewport，QStackedWidget 随即自动移除原 L1 page，VFV 的 index 会从 1 变成 0。
    // 用稳定的页面对象身份判断，避免菜单状态和实际编辑模式永久错位。
    return m_l2Mode && m_stack && m_l2View
        && (m_stack->currentWidget() == m_l2View
            || m_stack->currentWidget() == m_windowedReadOnlyProvider);
}

qint64 EditorTab::currentByteOffset() const
{
    // m_lastL2Byte is a compatibility/search cache. Programmatic navigation
    // and byte selections update VirtualFileView directly, so session state
    // must read the provider's authoritative 64-bit caret position.
    if (windowedReadOnlyActive())
        return m_windowedReadOnlyProvider->globalOffset();
    if (m_l2Mode && m_l2View)
        return m_l2View->cursorByte();
    return m_lastL2Byte;
}

// ============================================================================
//  L2 路径（VirtualFileView 大文件虚拟视图）—— 独立实现，不影响 L1
// ============================================================================

VirtualFileView *EditorTab::ensureL2View()
{
    if (!m_l2View) {
        m_l2View = new VirtualFileView(this);
        // 加进 QStackedWidget（index 1 = L2）
        if (m_stack && m_stack->indexOf(m_l2View) < 0)
            m_stack->addWidget(m_l2View);

        // 连接 L2 信号：缓存数据 + 转发 EditorTab 的统一/专用信号
        connect(m_l2View, &VirtualFileView::cursorPositionChanged,
                this, [this](int line, int col) {
                    m_lastL2Line = line;
                    m_lastL2Col  = col;
                    if (m_l2View && m_l2View->isOpen()) {
                        m_lastL2Byte = m_l2View->viewMode() == ViewMode::Hex
                            ? qMin(m_l2View->virtualFileSize(),
                                   qint64(line) * m_l2View->hexBytesPerRow()
                                       + qint64(col))
                            : m_l2View->lineIndex().lineToByteOffset(line) + col;
                    }
                    // 转发统一 cursorPositionChanged：MainWindow 状态栏更新走同一个
                    emit cursorPositionChanged(line, col);
                    emit l2CursorPositionChanged(line, col);
                });
        connect(m_l2View, &VirtualFileView::fileOpenedChanged,
                this, &EditorTab::l2FileOpenedChanged);
        connect(m_l2View, &VirtualFileView::saveNeededChanged,
                this, [this](bool needed) {
                    if (needed)
                        advanceContentRevision();
                    m_l2SaveNeeded = needed;
                    emit modificationChanged(needed);
                    emit titleChanged(tabTitle());
                    emit l2SaveNeededChanged(needed);
                });
        connect(m_l2View, &VirtualFileView::undoRedoChanged,
                this, &EditorTab::l2UndoRedoChanged);
        connect(m_l2View, &VirtualFileView::undoRedoChanged,
                this, &EditorTab::commandCapabilitiesChanged);
        connect(m_l2View, &VirtualFileView::lineBuildProgress,
                this, &EditorTab::l2LineBuildProgress);
        connect(m_l2View, &VirtualFileView::lineBuildFinished,
                this, &EditorTab::l2LineBuildFinished);
        connect(m_l2View, &VirtualFileView::lineBuildFinished,
                this, [this](bool ok) {
                    if (ok)
                        refreshTextProjectionMargin();
                });
    }
    return m_l2View;
}

WindowedReadOnlyProvider *EditorTab::ensureWindowedReadOnlyProvider()
{
    if (!m_windowedReadOnlyProvider) {
        m_windowedReadOnlyProvider = new WindowedReadOnlyProvider(this);
        m_windowedReadOnlyProvider->setObjectName(
            QStringLiteral("productionWindowedReadOnlyProvider"));
        if (m_stack && m_stack->indexOf(m_windowedReadOnlyProvider) < 0)
            m_stack->addWidget(m_windowedReadOnlyProvider);
        connect(m_windowedReadOnlyProvider->editor(),
                &QsciScintilla::selectionChanged,
                this, &EditorTab::commandCapabilitiesChanged);
        connect(m_windowedReadOnlyProvider, &WindowedReadOnlyProvider::windowReady,
                this, [this](qint64, qint64, qint64 anchor) {
            m_lastL2Byte = anchor;
            if (m_l2View && m_l2View->viewMode() == ViewMode::Hex) {
                const int row = m_windowedReadOnlyProvider->hexBytesPerRow();
                m_lastL2Line = int(qMin<qint64>(anchor / row, INT_MAX));
                m_lastL2Col = int(anchor % row);
            } else {
                m_lastL2Line = 0;
                m_lastL2Col = 0;
            }
            emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
            emit l2CursorPositionChanged(m_lastL2Line, m_lastL2Col);
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::sourceFileChanged,
                this, [this](qint64, qint64) {
            cancelLargeTextPromotion();
            refreshDiskVersionBaseline();
            if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
                window->statusBar()->showMessage(
                    QObject::tr("源文件已在磁盘上变化；已刷新只读视图并取消旧的编辑准备"),
                    8000);
            }
        });
        connect(m_windowedReadOnlyProvider,
                &WindowedReadOnlyProvider::sourceReloadFailed,
                this, [this](const QString &message) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(message, 8000);
        });
    }
    return m_windowedReadOnlyProvider;
}

bool EditorTab::windowedReadOnlyActive() const
{
    return m_l2Mode && m_stack && m_windowedReadOnlyProvider
        && m_stack->currentWidget() == m_windowedReadOnlyProvider;
}

EditorTab::LargeDocumentRuntimeState EditorTab::largeDocumentRuntimeState() const
{
    if (m_promotedLargeDocument && !m_l2Mode)
        return LargeDocumentRuntimeState::Editable;
    if (!m_l2Mode)
        return LargeDocumentRuntimeState::None;
    if (m_l2PromotionTaskId > 0)
        return LargeDocumentRuntimeState::PreparingEditor;
    if (!m_l2FallbackReason.isEmpty())
        return LargeDocumentRuntimeState::ReadOnlyFallback;
    if (m_l2ReadOnlyByPolicy)
        return LargeDocumentRuntimeState::ReadOnlyAnalysis;
    if (m_l2TextPreviewReadOnly)
        return LargeDocumentRuntimeState::PreviewReady;
    // B1 Hex uses the byte provider directly and is already editable; it is
    // not an S2 preview or promotion state.  Keep None so the status bar can
    // expose the more useful per-document INS/OVR mode.
    return LargeDocumentRuntimeState::None;
}

qint64 EditorTab::contentSizeBytes() const
{
    if (windowedReadOnlyActive() && m_windowedReadOnlyProvider)
        return m_windowedReadOnlyProvider->fileSize();
    if (m_l2Mode && m_l2View && m_l2View->isOpen())
        return m_l2View->virtualFileSize();
    return m_editor ? static_cast<qint64>(m_editor->length()) : 0;
}

bool EditorTab::ensureWindowedSearchSource(QString *error)
{
    // Search-source availability is a document capability, not a presentation
    // detail.  QStackedWidget::currentWidget() is appropriate for routing
    // visible-editor commands, but it is not a reliable lifetime marker for
    // the already-open production provider (dialogs and transient view changes
    // can make the provider non-current without closing its source file).
    const bool hasWindowedSource = m_l2Mode && m_windowedReadOnlyProvider
        && m_windowedReadOnlyProvider->fileSize() > 0
        && !m_filePath.isEmpty();
    if (!hasWindowedSource) {
        const bool hasMappedSource = m_l2View && m_l2View->isOpen();
        if (!hasMappedSource && error) {
            *error = QObject::tr(
                "当前大文件搜索源不可用（文档模式=%1，窗口源=%2，文件大小=%3，路径=%4）。")
                .arg(m_l2Mode ? QObject::tr("大文件") : QObject::tr("普通文件"))
                .arg(m_windowedReadOnlyProvider ? QObject::tr("存在")
                                                : QObject::tr("不存在"))
                .arg(m_windowedReadOnlyProvider
                         ? m_windowedReadOnlyProvider->fileSize() : -1)
                .arg(m_filePath.isEmpty() ? QObject::tr("为空")
                                          : QObject::tr("存在"));
        }
        return hasMappedSource;
    }
    if (m_l2View && (m_l2View->isOpen() || m_l2View->mmapSize() > 0))
        return true;
    if (m_filePath.isEmpty()) {
        if (error)
            *error = QObject::tr("当前大文件没有可搜索的磁盘源。");
        return false;
    }
    VirtualFileView *view = ensureL2View();
    view->setFullLineIndexBuildEnabled(false);
    view->setViewMode(m_windowedReadOnlyProvider->presentation()
                          == WindowedReadOnlyProvider::Presentation::Hex
                      ? ViewMode::Hex : ViewMode::Text);
    QString openError;
    if (!view->openFile(m_filePath, &openError, this->window())) {
        if (error)
            *error = openError;
        return false;
    }
    return true;
}

void EditorTab::ensureHexAsciiPreview(QWidget *parent)
{
    if (!parent)
        return;
    if (!m_hexAsciiPreview) {
        m_hexAsciiPreview = new QPlainTextEdit(parent);
        m_hexAsciiPreview->setObjectName(QStringLiteral("HexAsciiPreview"));
        m_hexAsciiPreview->setAccessibleName(QStringLiteral("ASCII 字节预览"));
        m_hexAsciiPreview->setToolTip(
            QStringLiteral("字符栏（不可打印字节显示为 .；可直接编辑原始字节）"));
        m_hexAsciiPreview->setFont(m_editor->font());
        m_hexAsciiPreview->setReadOnly(isReadOnlyByPolicy());
        // A byte pane is not a Unicode text editor. Disable native composition
        // so Chinese/Japanese IMEs cannot inject preedit text into the visual
        // projection; eventFilter still validates any commit event that a
        // platform plugin delivers despite this attribute.
        m_hexAsciiPreview->setAttribute(Qt::WA_InputMethodEnabled, false);
        m_hexAsciiPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_hexAsciiPreview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_hexAsciiPreview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_hexAsciiPreview->document()->setDocumentMargin(0);
        m_hexAsciiPreview->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { border: 0; border-left: 1px solid #c8c8c8;"
            " background: #fafafa; padding-left: 8px; }"));
        m_hexAsciiPreview->installEventFilter(this);
        m_hexAsciiPreview->viewport()->installEventFilter(this);
        connect(m_hexAsciiPreview, &QPlainTextEdit::cursorPositionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
        connect(m_hexAsciiPreview, &QPlainTextEdit::selectionChanged,
                this, &EditorTab::syncHexSelectionFromAscii);
    } else if (m_hexAsciiPreview->parentWidget() != parent) {
        m_hexAsciiPreview->setParent(parent);
    }
}

int EditorTab::preferredHexBytesPerRow() const
{
    if (!m_l2View || !m_l2View->viewport() || !m_editor)
        return 16;
    const int width = m_l2View->viewport()->width();
    const int charWidth = qMax(7, QFontMetrics(m_editor->font()).horizontalAdvance(
                                      QLatin1Char('0')));
    const int addressDigits = qMax(
        8, QString::number(qMax<qint64>(0, m_l2View->virtualFileSize() - 1), 16).size());
    // Do not read Scintilla's current margin width here: on first projection it
    // still belongs to the previous presentation, making the result order-dependent.
    const int addressWidth = charWidth * (addressDigits + 2) + 16;
    for (int candidate = 64; candidate >= 8; candidate -= 4) {
        const int required = addressWidth
            + charWidth * (candidate * 3 + candidate + 12) + 52;
        if (required <= width)
            return candidate;
    }
    return 8;
}

void EditorTab::adaptHexBytesPerRow(bool repopulate)
{
    if (!m_l2View || m_l2View->viewMode() != ViewMode::Hex)
        return;
    const int desired = preferredHexBytesPerRow();
    if (desired == m_hexBytesPerRow)
        return;

    if (m_bridge.windowStart >= 0 && m_editor->isModified()) {
        QString error;
        if (!bridgeFlushOverlayToPatches(&error))
            return;
    }
    m_hexBytesPerRow = desired;
    m_l2View->setHexBytesPerRow(desired);
    if (repopulate && m_bridge.windowStart >= 0 && m_bridge.windowLen > 0) {
        bridgePopulateScintilla(m_bridge.windowStart,
                                m_bridge.windowStart + m_bridge.windowLen);
    }
}

void EditorTab::layoutL2Overlay()
{
    if (!m_l2View || !m_editor)
        return;
    QWidget *viewport = m_l2View->viewport();
    if (!viewport)
        return;

    const bool hex = m_l2View->viewMode() == ViewMode::Hex;
    if (!hex) {
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        m_l2View->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 1UL);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR, 1UL);
        if (m_hexAsciiPreview)
            m_hexAsciiPreview->hide();
        m_editor->setGeometry(0, 0, viewport->width(), viewport->height());
        return;
    }

    ensureHexAsciiPreview(viewport);
    m_editor->setAttribute(Qt::WA_InputMethodEnabled, false);
    // The B1 Hex document has exactly one user-visible position truth: the
    // provider's global vertical bar. The projected Scintilla and the mirrored
    // ASCII pane must not expose independent scrollbars.
    m_l2View->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETVSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETHSCROLLBAR, 0UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 1UL);
    const int charWidth = qMax(7, QFontMetrics(m_editor->font()).horizontalAdvance(
                                      QLatin1Char('0')));
    const int desiredPreviewWidth = charWidth * (m_hexBytesPerRow + 2) + 24;
    const int previewWidth = qMin(qMax(1, viewport->width() - 1),
                                  qMin(desiredPreviewWidth,
                                       qMax(120, viewport->width() / 4)));
    // The two visible byte panes must tile the viewport. The old preferred
    // width left a third, inert strip to the right on wide windows, which made
    // the Hex page look unfinished and provided no hit target or information.
    const int editorWidth = qMax(1, viewport->width() - previewWidth);
    m_editor->setGeometry(0, 0, editorWidth, viewport->height());
    m_hexAsciiPreview->setGeometry(editorWidth, 0, previewWidth, viewport->height());
    m_hexAsciiPreview->show();
    m_hexAsciiPreview->raise();
    m_editor->raise();
}

void EditorTab::alignHexOverlayToGlobalScroll()
{
    if (!m_l2Mode || !m_l2View || !m_editor
        || m_l2View->viewMode() != ViewMode::Hex
        || m_bridge.windowStart < 0 || m_hexBytesPerRow <= 0) {
        return;
    }
    const qint64 globalFirst = m_l2View->verticalScrollBar()->value();
    const qint64 windowFirst = m_bridge.windowStart / m_hexBytesPerRow;
    const qint64 requestedLocal = globalFirst - windowFirst;
    const int linesOnScreen = qMax(1, int(m_editor->SendScintilla(
        QsciScintillaBase::SCI_LINESONSCREEN)));
    const int lastLocalFirst = qMax(0, m_editor->lines() - linesOnScreen);
    if (requestedLocal < 0 || requestedLocal > lastLocalFirst)
        return;
    m_bridge.applying = true;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                            static_cast<unsigned long>(requestedLocal));
    if (m_hexAsciiPreview)
        m_hexAsciiPreview->verticalScrollBar()->setValue(int(requestedLocal));
    m_bridge.applying = false;
}

void EditorTab::refreshTextProjectionMargin()
{
    if (!m_l2Mode || !m_l2View || !m_editor
        || m_l2View->viewMode() != ViewMode::Text
        || m_bridge.windowStart < 0) {
        return;
    }

    const LineIndex &index = m_l2View->lineIndex();
    const qint64 originalStart = m_l2View->patches().isModified()
        ? m_l2View->patches().virtToOrig(m_bridge.windowStart)
        : m_bridge.windowStart;
    bool exactBase = !index.isIncomplete()
        || originalStart <= index.realCoverageBytes();
    int baseLine = 0;
    qint64 column = 0;
    if (exactBase && !m_l2View->patches().isModified()) {
        index.byteToLineCol(originalStart, &baseLine, &column);
    } else {
        // The compatibility B1/secure-binary bridge is capped below the large
        // windowed tiers.  Its background LineIndex may still be incomplete
        // when the user switches Hex -> Text, and patches can change newline
        // counts.  Count the authoritative virtual bytes up to this bounded
        // window instead of showing the false local 1..N gutter.
        constexpr qint64 Chunk = Q_INT64_C(4) * 1024 * 1024;
        qint64 cursor = 0;
        qint64 lineBreaks = 0;
        while (cursor < m_bridge.windowStart) {
            const qint64 end = qMin(m_bridge.windowStart, cursor + Chunk);
            const QByteArray bytes = m_l2View->readByteRange(cursor, end);
            if (bytes.size() != end - cursor) {
                exactBase = false;
                break;
            }
            lineBreaks += bytes.count('\n');
            cursor = end;
        }
        if (cursor == m_bridge.windowStart) {
            baseLine = int(qMin<qint64>(lineBreaks, INT_MAX));
            exactBase = true;
        }
    }

    m_editor->SendScintilla(QsciScintillaBase::SCI_MARGINTEXTCLEARALL);
    m_editor->setMarginLineNumbers(0, false);
    m_editor->setMarginType(0, QsciScintilla::TextMargin);
    const int lines = qMax(1, m_editor->lines());
    const qint64 largest = exactBase ? qint64(baseLine) + lines : lines;
    const int digits = qMax(4, QString::number(qMax<qint64>(1, largest)).size());
    m_editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('9')));
    for (int localLine = 0; localLine < lines; ++localLine) {
        m_editor->setMarginText(
            localLine,
            exactBase ? QString::number(qint64(baseLine) + localLine + 1)
                      : QStringLiteral("?"),
            0);
    }
}

void EditorTab::activateL2Overlay(VirtualFileView *view)
{
    if (!view || !m_stack || !m_editor)
        return;
    m_stack->setCurrentWidget(view);

    QWidget *viewport = view->viewport();
    m_editor->setParent(viewport);
    m_editor->setGeometry(QRect(QPoint(0, 0), viewport->size()));
    m_editor->show();
    m_editor->raise();
    m_editor->setFocus();
    viewport->installEventFilter(this);
    layoutL2Overlay();

    // The provider object is stable for the tab lifetime. Install the bridge
    // once and reuse it for mmap files and decrypted in-memory binary payloads.
    if (!m_l2BridgeConnectionsInstalled) {
        m_bridge.syncVfvDebounce.setSingleShot(true);
        m_bridge.syncVfvDebounce.setInterval(250);
        connect(&m_bridge.syncVfvDebounce, &QTimer::timeout,
                this, &EditorTab::bridgeOnSyncDebounce);
        QObject::connect(m_editor, &QsciScintilla::SCN_UPDATEUI, this, [this]() {
            if (Q_UNLIKELY(!m_l2Mode || m_bridge.applying))
                return;
            if (m_hexAsciiPreview && m_hexAsciiPreview->isVisible()) {
                syncAsciiSelectionFromHex();
                const long first = m_editor->SendScintilla(
                    QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
                m_hexAsciiPreview->verticalScrollBar()->setValue(int(first));
            }
            m_bridge.syncVfvDebounce.start();
        });
        QObject::connect(view, &VirtualFileView::activeWindowChanged,
                         this, [this](qint64 byteStart, qint64 byteEnd) {
            if (m_l2Mode)
                bridgePopulateScintilla(byteStart, byteEnd);
        });
        m_l2BridgeConnectionsInstalled = true;
    }

    view->syncActiveWindowToCursor(0);
    const qint64 start = view->activeWindowByteStart();
    const qint64 end = view->activeWindowByteEnd();
    if (start >= 0 && end > start)
        bridgePopulateScintilla(start, end);

    QTimer::singleShot(0, this, [this, view]() {
        if (!m_l2Mode || !m_l2View)
            return;
        if (view->viewport() && m_editor)
            layoutL2Overlay();
    });
}

void EditorTab::clearL2BridgeBaseline()
{
    if (m_hexAsciiPreview && m_secureBinaryDocument) {
        const int previewLength = qMax(0,
            m_hexAsciiPreview->document()->characterCount() - 1);
        if (previewLength > 0)
            m_hexAsciiPreview->setPlainText(QString(previewLength, QLatin1Char('.')));
        m_hexAsciiPreview->clear();
    }
    if (!m_bridge.baseline.isEmpty() && m_secureBinaryDocument)
        m_bridge.baseline.fill(0);
    m_bridge.baseline.clear();
    m_bridge.baseline.squeeze();
    m_bridge.windowStart = -1;
    m_bridge.windowLen = 0;
}

// ============================================================================
//  L2ScintillaBridge：VFV 不做任何绘制；Scintilla 是唯一 UI。
// ============================================================================

void EditorTab::bridgeReparentEditorToStack()
{
    if (!m_stack) return;
    if (m_hexAsciiPreview)
        m_hexAsciiPreview->hide();
    // The same Scintilla instance is reused by ordinary Text documents after
    // leaving the Hex provider. Never leak the Hex-specific IME policy.
    m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
    // 先卸载 VFV viewport 的 eventFilter（避免 reparent 后仍收到无效 Resize）
    if (m_l2View && m_l2View->viewport())
        m_l2View->viewport()->removeEventFilter(this);
    // 如果 Scintilla 被 reparent 到 VFV viewport（L2 overlay），把它移回 m_stack page 0
    // （父对象不同或 parent 仍在 stack 外的 QAbstractScrollArea viewport）
    const bool stacked = (m_stack->indexOf(m_editor) >= 0);
    if (!stacked) {
        // 从当前父（可能 VFV viewport）拆下来，放回 stack page 0
        m_editor->setParent(m_stack);
        if (m_stack->indexOf(m_editor) < 0)
            m_stack->insertWidget(0, m_editor);
    }
    // 确保 stack page 0 仍指向 m_editor（即使重复 insertWidget 会被 move，再设一次 idx=0）
    if (m_stack->indexOf(m_editor) < 0)
        m_stack->insertWidget(0, m_editor);
}

void EditorTab::bridgePopulateScintilla(qint64 s, qint64 e)
{
    if (!m_l2View || !m_l2View->isOpen()) return;
    if (s < 0 || e <= s) return;

    // 桥 2：如果已有有效 baseline 窗口（用户可能做了编辑），先 flush diff 回 patches 再切窗口
    if (m_bridge.windowStart >= 0 && m_bridge.windowLen > 0
        && !(m_bridge.windowStart == s && m_bridge.windowLen == (e - s))) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(flushError, 6000);
            return;
        }
    }

    adaptHexBytesPerRow(false);
    m_bridge.applying = true;
    QByteArray bytes = m_l2View->readByteRange(s, e);
    // 走 SCI_SETTEXT（setText），对于 W_active 上限 8MB，主线程 setText 约 100ms 内完成，
    // 不会触发"UI 线程卡死"；甩滚时 bridge 不频繁触发（VFV debounce 已节流）。
    m_editor->setReadOnly(false);
    if (m_l2View->viewMode() == ViewMode::Hex) {
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, false);
        m_editor->setUtf8(true);
        m_editor->setText(formatHexBytes(bytes, m_hexBytesPerRow));
        // The row width is chosen to fit the viewport. A reused Scintilla can
        // retain an x-offset from a previous text/Hex projection, which would
        // otherwise hide the first bytes while the absolute offset still says 0.
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, 0);
        ensureHexAsciiPreview(m_l2View->viewport());
        QString ascii = formatAsciiBytes(bytes, m_hexBytesPerRow);
        const int oldPreviewLength = qMax(0,
            m_hexAsciiPreview->document()->characterCount() - 1);
        if (m_secureBinaryDocument && oldPreviewLength > 0) {
            m_hexAsciiPreview->setPlainText(
                QString(oldPreviewLength, QLatin1Char('.')));
        }
        m_hexAsciiPreview->setPlainText(ascii);
        alignPlainTextRowsToScintilla(m_hexAsciiPreview, m_editor);
        m_hexAsciiPreview->setReadOnly(isReadOnlyByPolicy());
        // QPlainTextEdit::setReadOnly(false) re-enables native input methods.
        // Apply the byte-surface policy after the editability policy every
        // time a projected window is repopulated.
        m_hexAsciiPreview->setAttribute(Qt::WA_InputMethodEnabled, false);
        if (m_secureBinaryDocument && !ascii.isEmpty())
            ascii.fill(QChar(0));
        m_editor->setWrapMode(QsciScintilla::WrapNone);
        m_editor->setMarginLineNumbers(0, false);
        // QScintilla implements setMarginLineNumbers(false) by forcing a
        // SymbolMargin, so TextMargin must be applied after that call.
        m_editor->setMarginType(0, QsciScintilla::TextMargin);
        const qint64 lastOffset = s + qMax<qint64>(0, bytes.size() - 1);
        const int addressDigits = qMax(8, QString::number(lastOffset, 16).size());
        m_editor->setMarginWidth(0, QString(addressDigits + 2, QLatin1Char('0')));
        const int lines = (bytes.size() + m_hexBytesPerRow - 1) / m_hexBytesPerRow;
        for (int line = 0; line < lines; ++line) {
            const QString address = QString::number(
                                        s + qint64(line) * m_hexBytesPerRow, 16)
                                        .rightJustified(addressDigits, QLatin1Char('0'))
                                        .toUpper();
            m_editor->setMarginText(line, address, 0);
        }
        m_bridge.windowStart = s;
        m_bridge.windowLen = e - s;
        layoutL2Overlay();
        alignHexOverlayToGlobalScroll();
    } else {
        m_editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        if (m_hexAsciiPreview)
            m_hexAsciiPreview->hide();
        m_editor->setText(QString::fromUtf8(bytes));
        m_editor->setWrapMode(m_l2View->wordWrap() ? QsciScintilla::WrapWord
                                                   : QsciScintilla::WrapNone);
    }
    m_editor->setModified(false);
    m_editor->setReadOnly(isReadOnlyByPolicy());
    // Both Scintilla and QPlainTextEdit may recalculate input-method support
    // when editability changes. This final assignment is authoritative for
    // every initial load, projection remap and Text<->Hex round trip.
    m_editor->setAttribute(
        Qt::WA_InputMethodEnabled,
        m_l2View->viewMode() != ViewMode::Hex);
    if (!m_bridge.baseline.isEmpty() && m_secureBinaryDocument)
        m_bridge.baseline.fill(0);
    m_bridge.baseline    = std::move(bytes);
    m_bridge.windowStart = s;
    m_bridge.windowLen   = e - s;
    m_bridge.applying    = false;
    if (m_l2View->viewMode() == ViewMode::Text)
        refreshTextProjectionMargin();
    applyL2SearchHighlightsToOverlay();
}

bool EditorTab::bridgeFlushOverlayToPatches(QString* errMsg)
{
    if (!m_l2View || !m_l2View->isOpen()) return true;
    if (isReadOnlyByPolicy()) return true;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return true;
    if (!m_editor->isModified()) return true;

    QByteArray now;
    if (m_l2View->viewMode() == ViewMode::Hex) {
        if (!parseHexBytes(m_editor->text(), &now, errMsg))
            return false;
    } else {
        now = m_editor->text().toUtf8();
    }
    if (now == m_bridge.baseline) {
        m_editor->setModified(false);
        return true;
    }

    const qint64 s = m_bridge.windowStart;
    const QByteArray& oldBytes = m_bridge.baseline;
    const qint64 oldLen = oldBytes.size();

    // 最小可用 diff：先找到公共前缀 + 公共后缀（O(n)，8MB 规模毫秒级），
    // 只对中间变化区间做一次 replaceBytes，避免 Myers 复杂度爆炸。
    qint64 commonPrefix = 0;
    const qint64 cmpN = qMin<qint64>(oldLen, now.size());
    while (commonPrefix < cmpN && oldBytes[int(commonPrefix)] == now[int(commonPrefix)])
        ++commonPrefix;
    qint64 commonSuffix = 0;
    while (commonSuffix < cmpN - commonPrefix
           && oldBytes[int(oldLen - 1 - commonSuffix)] == now[int(now.size() - 1 - commonSuffix)])
        ++commonSuffix;

    const qint64 patchOldStart = commonPrefix;
    const qint64 patchOldLen   = oldLen - commonPrefix - commonSuffix;
    const qint64 patchNewStart = commonPrefix;
    const qint64 patchNewLen   = now.size() - commonPrefix - commonSuffix;

    QByteArray newData;
    if (patchNewLen > 0)
        newData = now.mid(int(patchNewStart), int(patchNewLen));

    // 16MB 粘贴硬顶：超出阈值时不允许通过窗口内 flush 写入，避免 PatchList 单条 patch 过大。
    // 用户会被要求走"小范围粘贴分批"，后续可升级为 pasteBytes 分块路径（P1 之后）。
    constexpr qint64 HARD_DELTA_BYTES = Q_INT64_C(16) * 1024 * 1024;
    if (qAbs(qint64(newData.size()) - patchOldLen) > HARD_DELTA_BYTES ||
        qint64(newData.size()) > HARD_DELTA_BYTES) {
        if (errMsg) *errMsg = QObject::tr("窗口内单步修改过大（>16MB），请分批操作或缩小窗口后再修改。");
        return false;
    }

    const qint64 globByte = s + patchOldStart;
    QString localErr;
    const bool ok = m_l2View->replaceBytes(globByte, qint32(patchOldLen), newData, &localErr);
    if (!ok) {
        if (errMsg) *errMsg = localErr.isEmpty() ? QObject::tr("写入 patch 失败") : localErr;
        return false;
    }

    // flush 成功：更新 baseline 为新文本（长度对应 virtualSize）
    // 注意：替换后窗口可能因为前后 patch 变化而长/短，但我们只保存 Scintilla 当前 now 作为
    // baseline，窗口边界以 s~s+virtualChangedLen 为准——下一次切窗口时桥 1 会重新对齐重灌，
    // 所以此处 baseline 按 Scintilla 实际文本即可。
    if (!m_bridge.baseline.isEmpty() && m_secureBinaryDocument)
        m_bridge.baseline.fill(0);
    m_bridge.baseline    = now;
    m_bridge.windowStart = s;
    m_bridge.windowLen   = now.size();
    m_editor->setModified(false);
    return true;
}

void EditorTab::advanceContentRevision()
{
    ++m_contentRevision;
    m_l1SearchCacheValid = false;
    m_l1SearchCache.clear();
    m_lastL1MatchIndex = -1;
    m_l2SearchCacheValid = false;
    clearSearchHighlights();
    cancelPendingL2SearchForContentChange();
    emit contentChanged(m_contentRevision);
}

void EditorTab::cancelPendingL2SearchForContentChange()
{
    if (!m_pendingL2Search.active || !m_l2View)
        return;
    if (LargeFileSearch *search = m_l2View->searchEngine()) {
        search->requestCancel();
        if (search->lastTaskId() > 0)
            AsyncTaskManager::instance().cancel(search->lastTaskId());
    }
}

void EditorTab::applyL2SearchHighlightsToOverlay()
{
    if (!m_editor)
        return;
    constexpr int indicator = 8;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, indicator,
                            QsciScintillaBase::INDIC_ROUNDBOX);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, indicator, 0x00B7FF);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, indicator, 90);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                            0, m_editor->length());
    if (!isL2Mode() || m_bridge.windowStart < 0 || m_l2SearchHighlights.isEmpty())
        return;

    const qint64 windowStart = m_bridge.windowStart;
    const qint64 windowEnd = windowStart + m_bridge.windowLen;
    auto first = std::lower_bound(
        m_l2SearchHighlights.cbegin(), m_l2SearchHighlights.cend(), windowStart,
        [](const QPair<qint64, qint32> &match, qint64 value) {
            return match.first < value;
        });
    if (first != m_l2SearchHighlights.cbegin())
        --first;

    auto overlayPosition = [this](qint64 localByte) -> long {
        if (!m_l2View || m_l2View->viewMode() != ViewMode::Hex)
            return long(qBound<qint64>(qint64(0), localByte, qint64(LONG_MAX)));
        const int line = int(qMin<qint64>(localByte / m_hexBytesPerRow, INT_MAX));
        const int column = int((localByte % m_hexBytesPerRow) * 3);
        return m_editor->positionFromLineIndex(line, column);
    };
    for (auto it = first; it != m_l2SearchHighlights.cend(); ++it) {
        const qint64 matchStart = it->first;
        const qint64 matchEnd = matchStart + qMax<qint32>(0, it->second);
        if (matchStart >= windowEnd)
            break;
        if (matchEnd <= windowStart)
            continue;
        const long start = overlayPosition(qMax(windowStart, matchStart) - windowStart);
        const long end = overlayPosition(qMin(windowEnd, matchEnd) - windowStart);
        if (start >= 0 && end > start) {
            m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                                    start, end - start);
        }
    }
}

void EditorTab::bridgeOnSyncDebounce()
{
    if (!m_l2View || !m_l2View->isOpen() || m_bridge.applying) return;
    if (m_bridge.windowStart < 0 || m_bridge.windowLen <= 0) return;

    // 取 Scintilla 中心位置：
    //   - 有选区：选区中心字节
    //   - 无选区：(FirstVisibleLine + LinesOnScreen/2) → 转字节
    const int lineCount = m_editor->lines();
    if (lineCount <= 0) return;
    int firstLine = m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    int linesOnScreen = m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
    if (linesOnScreen <= 0) linesOnScreen = 1;
    const int mid = qBound(0, firstLine + linesOnScreen / 2, lineCount - 1);
    // SCI_GETLINESELSTARTPOSITION / CURRENTPOS 太粗糙，直接用 mid 行首 + 当前光标列近似：
    qint64 localByte = 0;
    if (m_l2View->viewMode() == ViewMode::Hex) {
        // The byte grid and global provider share one adaptive row width. This keeps
        // navigation independent of the three display characters per byte.
        localByte = qint64(mid) * m_hexBytesPerRow;
        const qint64 globalFirst = m_bridge.windowStart / m_hexBytesPerRow
                                   + qMax(0, firstLine);
        QSignalBlocker blocker(m_l2View->verticalScrollBar());
        m_l2View->verticalScrollBar()->setValue(int(qBound<qint64>(
            qint64(m_l2View->verticalScrollBar()->minimum()), globalFirst,
            qint64(m_l2View->verticalScrollBar()->maximum()))));
    } else {
        const long lineStart = m_editor->positionFromLineIndex(mid, 0);
        localByte = qMax<qint64>(0, lineStart);
    }
    const qint64 globCenter = m_bridge.windowStart + localByte;

    m_l2View->syncActiveWindowToCursor(globCenter);
}

bool EditorTab::eventFilter(QObject* obj, QEvent* ev)
{
    const bool hexOverlayObject = m_l2Mode && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex
        && (obj == m_editor || obj == m_editor->viewport()
            || obj == m_hexAsciiPreview
            || (m_hexAsciiPreview && obj == m_hexAsciiPreview->viewport()));
    if (hexOverlayObject && ev->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(ev)->button() == Qt::LeftButton) {
        m_hexMouseSelecting = true;
    }
    if (hexOverlayObject && ev->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(ev);
        if (wheel->modifiers().testFlag(Qt::ControlModifier)
            || wheel->modifiers().testFlag(Qt::ShiftModifier)
            || qAbs(wheel->angleDelta().x()) > qAbs(wheel->angleDelta().y())) {
            return QWidget::eventFilter(obj, ev);
        }
        int lineDelta = 0;
        if (!wheel->pixelDelta().isNull()) {
            const int lineHeight = qMax(1, m_editor->fontMetrics().lineSpacing());
            m_hexWheelPixelRemainder += wheel->pixelDelta().y();
            lineDelta = -(m_hexWheelPixelRemainder / lineHeight);
            m_hexWheelPixelRemainder %= lineHeight;
        } else {
            m_hexWheelAngleRemainder += wheel->angleDelta().y();
            const int steps = m_hexWheelAngleRemainder / 120;
            m_hexWheelAngleRemainder %= 120;
            lineDelta = -steps * qMax(1, QApplication::wheelScrollLines());
        }
        if (lineDelta != 0) {
            QScrollBar *global = m_l2View->verticalScrollBar();
            global->setValue(qBound(global->minimum(),
                                    global->value() + lineDelta,
                                    global->maximum()));
            // Do not wait for the legacy slider debounce when the requested
            // global row approaches the edge of the 256 KiB projection. Move
            // the active byte window during the wheel gesture so the next row
            // is painted immediately and both Hex/ASCII panes stay continuous.
            const int rowsOnScreen = qMax(1, int(m_editor->SendScintilla(
                QsciScintillaBase::SCI_LINESONSCREEN)));
            const qint64 centerRow = qint64(global->value()) + rowsOnScreen / 2;
            const qint64 targetByte = qBound<qint64>(
                0, centerRow * m_hexBytesPerRow,
                qMax<qint64>(0, m_l2View->virtualFileSize() - 1));
            m_l2View->syncActiveWindowToCursor(targetByte);
            alignHexOverlayToGlobalScroll();
        }
        wheel->accept();
        return true;
    }
    if (hexOverlayObject && ev->type() == QEvent::InputMethod) {
        const bool asciiSurface = m_hexAsciiPreview
            && (obj == m_hexAsciiPreview
                || obj == m_hexAsciiPreview->viewport());
        return handleHexInputMethodEvent(
            static_cast<QInputMethodEvent *>(ev), asciiSurface);
    }
    if (m_hexAsciiPreview
        && (obj == m_hexAsciiPreview || obj == m_hexAsciiPreview->viewport())
        && ev->type() == QEvent::KeyPress && isL2Mode() && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex) {
        if (handleHexAsciiKeyPress(static_cast<QKeyEvent *>(ev)))
            return true;
    }
    if ((obj == m_editor || obj == m_editor->viewport())
        && ev->type() == QEvent::KeyPress
        && isL2Mode() && m_l2View
        && m_l2View->viewMode() == ViewMode::Hex) {
        auto *keyEvent = static_cast<QKeyEvent *>(ev);
        if (handleHexKeyPress(keyEvent))
            return true;
    }
    if (hexOverlayObject && ev->type() == QEvent::MouseButtonRelease) {
        // Do not rewrite Scintilla's selection while its mouse-drag state
        // machine still owns the anchor. Normalize once the release has been
        // fully processed; doing it during MouseMove can repeatedly restart
        // the native drag and stall the GUI event loop.
        const bool asciiSurface = m_hexAsciiPreview
            && (obj == m_hexAsciiPreview
                || obj == m_hexAsciiPreview->viewport());
        QTimer::singleShot(0, this, [this, asciiSurface]() {
            m_hexMouseSelecting = false;
            if (asciiSurface)
                syncHexSelectionFromAscii();
            else
                syncAsciiSelectionFromHex();
        });
    }

    // 监听 VFV viewport 的 Resize：Scintilla overlay 必须同步跟着 resize，
    // 否则 reparent 后 vp->size() 为 0（stack 刚切，布局未完成）→ Scintilla 0x0 不显示。
    if (ev->type() == QEvent::Resize && m_l2Mode && m_l2View) {
        QWidget* vp = m_l2View->viewport();
        if (obj == vp && m_editor) {
            adaptHexBytesPerRow(true);
            layoutL2Overlay();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

bool EditorTab::asciiSelectionByteRange(qint64 *start, qint64 *end) const
{
    if (!start || !end || !m_hexAsciiPreview || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex || m_bridge.windowStart < 0) {
        return false;
    }
    const QTextCursor cursor = m_hexAsciiPreview->textCursor();
    const auto mapPosition = [this](int position) {
        const QTextBlock block = m_hexAsciiPreview->document()->findBlock(position);
        if (!block.isValid())
            return m_bridge.windowLen;
        const qint64 column = qBound<qint64>(
            0LL, qint64(position - block.position()),
            qint64(qMin(m_hexBytesPerRow, block.text().size())));
        return qMin(m_bridge.windowLen,
                    qint64(block.blockNumber()) * m_hexBytesPerRow + column);
    };
    qint64 localStart = mapPosition(cursor.selectionStart());
    qint64 localEnd = mapPosition(cursor.selectionEnd());
    if (localEnd < localStart)
        std::swap(localStart, localEnd);
    *start = m_bridge.windowStart + localStart;
    *end = m_bridge.windowStart + localEnd;
    return true;
}

bool EditorTab::handleHexInsertModeKey(QKeyEvent *event)
{
    if (!event || event->key() != Qt::Key_Insert
        || event->modifiers() != Qt::NoModifier || isReadOnlyByPolicy()) {
        return false;
    }
    m_hexOverwriteMode = !m_hexOverwriteMode;
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
        window->statusBar()->showMessage(
            m_hexOverwriteMode ? QObject::tr("Hex 覆盖模式 (OVR)")
                               : QObject::tr("Hex 插入模式 (INS)"),
            1800);
    }
    return true;
}

void EditorTab::setAsciiCaret(qint64 globalByte)
{
    if (!m_hexAsciiPreview || !m_l2View || m_bridge.windowStart < 0)
        return;
    globalByte = qBound<qint64>(0, globalByte, m_l2View->virtualFileSize());
    m_l2View->syncActiveWindowToCursor(globalByte);
    const qint64 local = qBound<qint64>(0, globalByte - m_bridge.windowStart,
                                        m_bridge.windowLen);
    const int line = int(local / m_hexBytesPerRow);
    const int column = int(local % m_hexBytesPerRow);
    const QTextBlock block = m_hexAsciiPreview->document()->findBlockByNumber(line);
    if (!block.isValid())
        return;
    QTextCursor cursor(m_hexAsciiPreview->document());
    cursor.setPosition(qMin(block.position() + column,
                            block.position() + block.text().size()));
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setTextCursor(cursor);
    m_adjustingAsciiCaret = false;
    setHexCaret(globalByte, 0);
    m_lastL2Byte = globalByte;
}

void EditorTab::syncAsciiSelectionFromHex()
{
    if (m_adjustingHexCaret || m_adjustingAsciiCaret || m_bridge.applying
        || !m_hexAsciiPreview || !m_hexAsciiPreview->isVisible()) {
        return;
    }
    qint64 start = 0;
    qint64 end = 0;
    int nibble = 0;
    if (!hexSelectionByteRange(&start, &end, &nibble))
        return;

    // Scintilla still performs mouse hit testing against the textual
    // projection ("00 01 ...").  A click can therefore leave its visible
    // caret on a separator space even though the source model has already
    // resolved that position to the following byte.  Keep the byte model and
    // the visible caret identical after every native mouse/cursor update.
    if (end == start && !m_hexMouseSelecting) {
        const qint64 local = qBound<qint64>(
            0, start - m_bridge.windowStart, m_bridge.windowLen);
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3
                               + qBound(0, nibble, 1));
        const long expected = m_editor->positionFromLineIndex(line, column);
        const long actual = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        if (expected >= 0 && actual != expected)
            setHexCaret(start, nibble);
    } else if (!m_hexMouseSelecting) {
        const auto boundaryPosition = [this](qint64 globalByte) -> long {
            const qint64 local = qBound<qint64>(
                0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
            if (local >= m_bridge.windowLen)
                return m_editor->length();
            const int line = int(qMin<qint64>(
                local / m_hexBytesPerRow, INT_MAX));
            const int column = int((local % m_hexBytesPerRow) * 3);
            return m_editor->positionFromLineIndex(line, column);
        };
        const long actualAnchor = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETANCHOR);
        const long actualCaret = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        const bool forward = actualCaret >= actualAnchor;
        const long expectedAnchor = boundaryPosition(forward ? start : end);
        const long expectedCaret = boundaryPosition(forward ? end : start);
        // Mouse drag and native Shift movement operate on characters. Snap
        // both ends to source-byte boundaries so the highlighted Hex range,
        // ASCII mirror and the bytes affected by Cut/Delete cannot disagree.
        if (expectedAnchor >= 0 && expectedCaret >= 0
            && (actualAnchor != expectedAnchor || actualCaret != expectedCaret)) {
            m_adjustingHexCaret = true;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                    expectedAnchor, expectedCaret);
            m_adjustingHexCaret = false;
        }
    }

    const auto documentPosition = [this](qint64 globalByte) -> int {
        const qint64 local = qBound<qint64>(
            0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
        if (local >= m_bridge.windowLen)
            return qMax(0, m_hexAsciiPreview->document()->characterCount() - 1);
        const QTextBlock block = m_hexAsciiPreview->document()->findBlockByNumber(
            int(local / m_hexBytesPerRow));
        if (!block.isValid())
            return qMax(0, m_hexAsciiPreview->document()->characterCount() - 1);
        return qMin(block.position() + int(local % m_hexBytesPerRow),
                    block.position() + int(block.text().size()));
    };

    // Do not feed an unselected Hex caret into QPlainTextEdit on every
    // SCN_UPDATEUI. setTextCursor() calls ensureCursorVisible(), which used to
    // pull the ASCII pane back to the caret after each wheel gesture. A real
    // byte selection is still mirrored below; otherwise scrolling owns the
    // viewport and both panes remain locked to the same first row.
    if (end == start && !m_hexAsciiPreview->textCursor().hasSelection()) {
        m_hexAsciiPreview->setExtraSelections({});
        const int first = int(m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        m_hexAsciiPreview->verticalScrollBar()->setValue(first);
        m_l2View->selectBytes(start, end);
        m_lastL2Byte = start;
        return;
    }

    QTextCursor cursor(m_hexAsciiPreview->document());
    cursor.setPosition(documentPosition(start));
    if (end > start)
        cursor.setPosition(documentPosition(end), QTextCursor::KeepAnchor);
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setTextCursor(cursor);
    m_adjustingAsciiCaret = false;
    QList<QTextEdit::ExtraSelection> mirroredSelections;
    if (end > start) {
        QTextEdit::ExtraSelection mirrored;
        mirrored.cursor = cursor;
        mirrored.format.setBackground(QColor(51, 153, 255));
        mirrored.format.setForeground(Qt::white);
        mirroredSelections.append(mirrored);
    }
    m_hexAsciiPreview->setExtraSelections(mirroredSelections);
    const int first = int(m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    m_hexAsciiPreview->verticalScrollBar()->setValue(first);
    m_l2View->selectBytes(start, end);
    m_lastL2Byte = end > start ? end : start;
}

void EditorTab::syncHexSelectionFromAscii()
{
    if (m_adjustingAsciiCaret || m_adjustingHexCaret || m_bridge.applying
        || m_hexMouseSelecting || !m_hexAsciiPreview) {
        return;
    }
    qint64 start = 0;
    qint64 end = 0;
    if (!asciiSelectionByteRange(&start, &end))
        return;

    const auto overlayPosition = [this](qint64 globalByte) -> long {
        const qint64 local = qBound<qint64>(
            0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
        if (local >= m_bridge.windowLen)
            return m_editor->length();
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3);
        return m_editor->positionFromLineIndex(line, column);
    };

    const long first = overlayPosition(start);
    const long last = overlayPosition(end);
    if (first < 0 || last < 0)
        return;
    m_adjustingHexCaret = true;
    if (end > start) {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, first, last);
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, first);
    }
    m_adjustingHexCaret = false;
    m_l2View->selectBytes(start, end);
    m_lastL2Line = int(qMin<qint64>(start / m_hexBytesPerRow, INT_MAX));
    m_lastL2Col = int(start % m_hexBytesPerRow);
    m_lastL2Byte = end > start ? end : start;
    emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
}

void EditorTab::refreshHexAsciiProjectionFromEditor()
{
    if (!m_editor || !m_hexAsciiPreview || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex) {
        return;
    }
    QByteArray previewBytes;
    QString previewError;
    if (!parseHexBytes(m_editor->text(), &previewBytes, &previewError))
        return;
    QString ascii = formatAsciiBytes(previewBytes, m_hexBytesPerRow);
    m_adjustingAsciiCaret = true;
    m_hexAsciiPreview->setPlainText(ascii);
    alignPlainTextRowsToScintilla(m_hexAsciiPreview, m_editor);
    m_adjustingAsciiCaret = false;
    if (m_secureBinaryDocument) {
        if (!previewBytes.isEmpty()) previewBytes.fill(0);
        if (!ascii.isEmpty()) ascii.fill(QChar(0));
    }
    syncAsciiSelectionFromHex();
}

bool EditorTab::handleHexAsciiKeyPress(QKeyEvent *event)
{
    if (!event || !m_l2View || isReadOnlyByPolicy())
        return false;
    if (handleHexInsertModeKey(event))
        return true;
    qint64 start = 0;
    qint64 end = 0;
    if (!asciiSelectionByteRange(&start, &end))
        return false;
    const bool selected = end > start;
    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };
    const auto replace = [this, &showError](qint64 from, qint64 to,
                                            QByteArray bytes) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return;
        }
        QString error;
        const bool ok = m_l2View->replaceBytes(
            from, qint32(qMin<qint64>(to - from, INT_MAX)), bytes, &error);
        const qint64 caret = from + bytes.size();
        if (m_secureBinaryDocument && !bytes.isEmpty())
            bytes.fill(0);
        if (!ok) {
            showError(error);
            return;
        }
        m_l2View->syncActiveWindowToCursor(caret);
        bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                m_l2View->activeWindowByteEnd());
        setAsciiCaret(caret);
        m_hexAsciiPreview->setFocus(Qt::OtherFocusReason);
    };

    if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
        if (!selected)
            return true;
        QByteArray bytes = m_l2View->readByteRange(start, end);
        ByteClipboard::setAsciiSelection(bytes);
        if (m_secureBinaryDocument && !bytes.isEmpty())
            bytes.fill(0);
        if (event->matches(QKeySequence::Cut))
            replace(start, end, {});
        return true;
    }
    if (event->matches(QKeySequence::Paste)) {
        const QString text = QGuiApplication::clipboard()->text();
        for (const QChar ch : text) {
            if (ch.unicode() > 0xff) {
                showError(QObject::tr("字符栏粘贴只接受单字节 Latin-1 字符。"));
                return true;
            }
        }
        QByteArray pasted = text.toLatin1();
        qint64 overwriteEnd = end;
        if (!selected && m_hexOverwriteMode) {
            const qint64 available = m_l2View->virtualFileSize() - start;
            if (available <= 0)
                return true;
            if (pasted.size() > available)
                pasted.truncate(int(available));
            overwriteEnd = start + pasted.size();
        }
        replace(start, overwriteEnd, std::move(pasted));
        return true;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (!selected) {
            if (event->key() == Qt::Key_Backspace) {
                // At byte zero Backspace is a boundary no-op.  Falling
                // through with start==0 used to delete the first byte, making
                // Backspace indistinguishable from Delete at the file start.
                if (start <= 0)
                    return true;
                --start;
            } else if (start >= m_l2View->virtualFileSize()) {
                return true;
            }
            end = qMin(m_l2View->virtualFileSize(), start + 1);
        }
        if (end > start)
            replace(start, end, {});
        return true;
    }
    const QString typed = event->text();
    if (typed.size() == 1
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        const uint value = typed.at(0).unicode();
        if (value > 0xff || value < 0x20
            || (value >= 0x7f && value <= 0x9f)) {
            showError(QObject::tr(
                "字符栏直接输入只接受可打印单字节 Latin-1 字符；控制字节请使用十六进制粘贴。"));
            return true;
        }
        if (!selected && m_hexOverwriteMode
            && start >= m_l2View->virtualFileSize()) {
            return true;
        }
        const qint64 overwriteEnd = selected || !m_hexOverwriteMode
            ? end : qMin(m_l2View->virtualFileSize(), start + 1);
        replace(start, overwriteEnd, typed.toLatin1());
        return true;
    }
    return false;
}

bool EditorTab::handleHexInputMethodEvent(QInputMethodEvent *event,
                                          bool asciiSurface)
{
    if (!event || !m_l2View || m_l2View->viewMode() != ViewMode::Hex)
        return false;

    // Preedit text is presentation state owned by the IME. Showing it inside
    // "00 01 ..." would temporarily (and with some plugins permanently)
    // corrupt the fixed Hex grid, so byte surfaces never expose composition.
    // Only a fully validated commit may enter the source-byte command path.
    event->accept();
    const QString committed = event->commitString();
    if (committed.isEmpty() || isReadOnlyByPolicy())
        return true;

    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };

    // Validate the entire commit before changing one byte. This prevents a
    // mixed commit such as "D8中" from applying D8 and rejecting only the tail.
    for (const QChar ch : committed) {
        if (asciiSurface) {
            const uint value = ch.unicode();
            if (value > 0xff || value < 0x20
                || (value >= 0x7f && value <= 0x9f)) {
                showError(QObject::tr(
                    "字符栏输入法提交只接受可打印单字节 Latin-1 字符。"));
                return true;
            }
        } else if (hexNibble(ch) < 0) {
            showError(QObject::tr(
                "十六进制视图只接受 0–9、A–F；输入法候选文本未写入文件。"));
            return true;
        }
    }

    if (!asciiSurface) {
        // An IME commit is one user transaction. Replaying its characters as
        // independent nibble key presses is incorrect in INS mode because a
        // high-nibble insertion rebuilds the projection and moves the caret
        // between characters. Build one byte-level replacement instead.
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }

        qint64 start = 0;
        qint64 end = 0;
        int nibble = 0;
        if (!hexSelectionByteRange(&start, &end, &nibble)) {
            showError(QObject::tr("无法把输入法提交定位到源字节。"));
            return true;
        }
        const bool selected = end > start;
        QByteArray replacement;
        qint32 oldLength = 0;
        qint64 caretByte = start;
        int caretNibble = 0;

        const auto appendNibblePairs = [&committed](QByteArray *target,
                                                     int firstNibble) {
            for (int i = firstNibble; i < committed.size(); i += 2) {
                const int high = hexNibble(committed.at(i));
                const int low = i + 1 < committed.size()
                    ? hexNibble(committed.at(i + 1)) : 0;
                target->append(char((high << 4) | low));
            }
        };

        if (selected) {
            appendNibblePairs(&replacement, 0);
            oldLength = qint32(qMin<qint64>(end - start, INT_MAX));
            if (committed.size() % 2 == 0) {
                caretByte = start + replacement.size();
            } else {
                caretByte = start + qMax(0, replacement.size() - 1);
                caretNibble = 1;
            }
        } else if (!m_hexOverwriteMode) {
            if (nibble == 0) {
                appendNibblePairs(&replacement, 0);
                caretByte = start + committed.size() / 2;
                caretNibble = committed.size() % 2;
            } else {
                if (start >= m_l2View->virtualFileSize())
                    return true;
                QByteArray current = m_l2View->readByteRange(start, start + 1);
                if (current.isEmpty())
                    return true;
                current[0] = char((quint8(current.at(0)) & 0xf0)
                                  | hexNibble(committed.at(0)));
                replacement.append(current.at(0));
                appendNibblePairs(&replacement, 1);
                oldLength = 1;
                const int remaining = committed.size() - 1;
                caretByte = start + 1 + remaining / 2;
                caretNibble = remaining % 2;
                if (m_secureBinaryDocument)
                    current.fill(0);
            }
        } else {
            const qint64 availableNibbles = qMax<qint64>(
                0, (m_l2View->virtualFileSize() - start) * 2 - nibble);
            const int applied = int(qMin<qint64>(
                committed.size(), qMin<qint64>(availableNibbles, INT_MAX)));
            if (applied <= 0)
                return true;
            const int bytesTouched = (nibble + applied + 1) / 2;
            replacement = m_l2View->readByteRange(
                start, start + bytesTouched);
            for (int i = 0; i < applied; ++i) {
                const int destinationNibble = nibble + i;
                const int byteIndex = destinationNibble / 2;
                const int value = hexNibble(committed.at(i));
                const quint8 old = quint8(replacement.at(byteIndex));
                replacement[byteIndex] = destinationNibble % 2 == 0
                    ? char((value << 4) | (old & 0x0f))
                    : char((old & 0xf0) | value);
            }
            oldLength = qint32(replacement.size());
            const int next = nibble + applied;
            caretByte = start + next / 2;
            caretNibble = next % 2;
        }

        QString replaceError;
        if (!m_l2View->replaceBytes(start, oldLength, replacement,
                                    &replaceError)) {
            showError(replaceError);
            if (m_secureBinaryDocument && !replacement.isEmpty())
                replacement.fill(0);
            return true;
        }
        if (m_secureBinaryDocument && !replacement.isEmpty())
            replacement.fill(0);
        m_l2View->syncActiveWindowToCursor(caretByte);
        bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                m_l2View->activeWindowByteEnd());
        setHexCaret(caretByte, caretNibble);
        return true;
    }

    for (const QChar ch : committed) {
        const int key = asciiSurface ? int(ch.unicode())
                                     : int(ch.toUpper().unicode());
        QKeyEvent typed(QEvent::KeyPress, key, Qt::NoModifier, QString(ch));
        const bool handled = asciiSurface
            ? handleHexAsciiKeyPress(&typed) : handleHexKeyPress(&typed);
        if (!handled) {
            showError(QObject::tr("输入法提交未能转换为原始字节，文件保持不变。"));
            break;
        }
    }
    return true;
}

bool EditorTab::hexSelectionByteRange(qint64 *start, qint64 *end,
                                      int *nibble) const
{
    if (!start || !end || !m_editor || !m_l2View
        || m_l2View->viewMode() != ViewMode::Hex
        || m_bridge.windowStart < 0) {
        return false;
    }

    long selectionStart = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONSTART);
    long selectionEnd = m_editor->SendScintilla(
        QsciScintillaBase::SCI_GETSELECTIONEND);
    if (selectionEnd < selectionStart)
        std::swap(selectionStart, selectionEnd);
    const bool selected = selectionEnd > selectionStart;

    const auto mapPosition = [this](long position, bool endBoundary,
                                    int *outNibble) -> qint64 {
        int line = 0;
        int column = 0;
        m_editor->lineIndexFromPosition(position, &line, &column);
        line = qMax(0, line);
        column = qMax(0, column);
        qint64 byteInLine = endBoundary ? (qint64(column) + 2) / 3
                                        : qint64(column) / 3;
        int localNibble = column % 3;
        if (!endBoundary && localNibble == 2) {
            ++byteInLine;
            localNibble = 0;
        }
        byteInLine = qBound<qint64>(qint64(0), byteInLine,
                                    qint64(m_hexBytesPerRow));
        if (outNibble)
            *outNibble = qBound(0, localNibble, 1);
        return qint64(line) * m_hexBytesPerRow + byteInLine;
    };

    int localNibble = 0;
    qint64 localStart = mapPosition(selectionStart, false, &localNibble);
    qint64 localEnd = selected
        ? mapPosition(selectionEnd, true, nullptr)
        : localStart;
    const qint64 windowEnd = m_bridge.windowStart + m_bridge.windowLen;
    *start = qBound(m_bridge.windowStart,
                    m_bridge.windowStart + localStart, windowEnd);
    *end = qBound(*start, m_bridge.windowStart + localEnd, windowEnd);
    if (nibble)
        *nibble = localNibble;
    return true;
}

void EditorTab::setHexCaret(qint64 globalByte, int nibble)
{
    if (!m_editor || !m_l2View || m_bridge.windowStart < 0)
        return;
    globalByte = qBound<qint64>(0, globalByte, m_l2View->virtualFileSize());
    m_l2View->syncActiveWindowToCursor(globalByte);
    if (globalByte < m_bridge.windowStart
        || globalByte > m_bridge.windowStart + m_bridge.windowLen) {
        return;
    }

    qint64 local = globalByte - m_bridge.windowStart;
    const qint64 size = m_l2View->virtualFileSize();
    if (globalByte == size && size > 0) {
        --globalByte;
        --local;
        nibble = 1;
    }
    const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
    const int column = int((local % m_hexBytesPerRow) * 3
                           + qBound(0, nibble, 1));
    const long position = m_editor->positionFromLineIndex(line, column);
    if (position >= 0) {
        m_adjustingHexCaret = true;
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, position);
        m_adjustingHexCaret = false;
        m_lastL2Line = int(qMin<qint64>(globalByte / m_hexBytesPerRow, INT_MAX));
        m_lastL2Col = int(globalByte % m_hexBytesPerRow);
        m_lastL2Byte = globalByte;
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
    }
}

bool EditorTab::handleHexKeyPress(QKeyEvent *event)
{
    if (!event || !m_l2View)
        return false;
    if (handleHexInsertModeKey(event))
        return true;

    const auto showError = [this](const QString &message) {
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(message, 5000);
        QApplication::beep();
    };
    const auto refresh = [this](qint64 caretByte, int caretNibble) {
        m_l2View->syncActiveWindowToCursor(caretByte);
        bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                m_l2View->activeWindowByteEnd());
        setHexCaret(caretByte, caretNibble);
    };

    qint64 start = 0;
    qint64 end = 0;
    int nibble = 0;
    if (!hexSelectionByteRange(&start, &end, &nibble))
        return false;
    const bool hasSelection = end > start;

    const bool shiftOnly = event->modifiers() == Qt::ShiftModifier;
    const bool controlShift = event->modifiers()
                              == (Qt::ControlModifier | Qt::ShiftModifier);
    if ((shiftOnly || controlShift)
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
            || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown
            || event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        const auto sourceBoundary = [this](long position) -> qint64 {
            int line = 0;
            int column = 0;
            m_editor->lineIndexFromPosition(position, &line, &column);
            const qint64 local = qint64(qMax(0, line)) * m_hexBytesPerRow
                + (qint64(qMax(0, column)) + 2) / 3;
            return qBound<qint64>(
                0, m_bridge.windowStart + local,
                m_l2View->virtualFileSize());
        };
        const auto overlayBoundary = [this](qint64 globalByte) -> long {
            const qint64 local = qBound<qint64>(
                0, globalByte - m_bridge.windowStart, m_bridge.windowLen);
            if (local >= m_bridge.windowLen)
                return m_editor->length();
            return m_editor->positionFromLineIndex(
                int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX)),
                int((local % m_hexBytesPerRow) * 3));
        };
        const long anchorPosition = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETANCHOR);
        const long caretPosition = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCURRENTPOS);
        qint64 anchorByte = hasSelection ? sourceBoundary(anchorPosition) : start;
        qint64 caretByte = hasSelection ? sourceBoundary(caretPosition) : start;
        const qint64 size = m_l2View->virtualFileSize();
        const qint64 pageRows = qMax<qint64>(1, m_editor->SendScintilla(
            QsciScintillaBase::SCI_LINESONSCREEN));
        if (controlShift && event->key() == Qt::Key_Home)
            caretByte = 0;
        else if (controlShift && event->key() == Qt::Key_End)
            caretByte = size;
        else if (event->key() == Qt::Key_Left)
            caretByte = qMax<qint64>(0, caretByte - 1);
        else if (event->key() == Qt::Key_Right)
            caretByte = qMin(size, caretByte + 1);
        else if (event->key() == Qt::Key_Up)
            caretByte = qMax<qint64>(0, caretByte - m_hexBytesPerRow);
        else if (event->key() == Qt::Key_Down)
            caretByte = qMin(size, caretByte + m_hexBytesPerRow);
        else if (event->key() == Qt::Key_PageUp)
            caretByte = qMax<qint64>(
                0, caretByte - pageRows * m_hexBytesPerRow);
        else if (event->key() == Qt::Key_PageDown)
            caretByte = qMin(size,
                caretByte + pageRows * m_hexBytesPerRow);
        else {
            const qint64 rowStart = (caretByte / m_hexBytesPerRow)
                                    * m_hexBytesPerRow;
            caretByte = event->key() == Qt::Key_Home
                ? rowStart : qMin(size, rowStart + m_hexBytesPerRow);
        }
        const long newAnchor = overlayBoundary(anchorByte);
        const long newCaret = overlayBoundary(caretByte);
        if (newAnchor >= 0 && newCaret >= 0) {
            m_adjustingHexCaret = true;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                    newAnchor, newCaret);
            m_adjustingHexCaret = false;
            syncAsciiSelectionFromHex();
        }
        return true;
    }

    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        const qint64 local = qMax<qint64>(0, start - m_bridge.windowStart);
        const qint64 row = local / qMax(1, m_hexBytesPerRow);
        const qint64 rowStart = m_bridge.windowStart + row * m_hexBytesPerRow;
        if (event->key() == Qt::Key_Home) {
            setHexCaret(qMin(rowStart, m_l2View->virtualFileSize()), 0);
        } else {
            const qint64 rowEnd = qMin(
                qMin(rowStart + m_hexBytesPerRow,
                     m_bridge.windowStart + m_bridge.windowLen),
                m_l2View->virtualFileSize());
            if (rowEnd > rowStart)
                setHexCaret(rowEnd - 1, 1);
        }
        syncAsciiSelectionFromHex();
        return true;
    }

    // Scintilla sees the Hex projection as ordinary text ("00 01 ...") and
    // would therefore place the caret on separator spaces.  Horizontal Hex
    // navigation is byte/nibble navigation: high -> low -> next byte high,
    // with the inverse sequence for Left.  Keep application shortcuts and
    // Shift-selection on their dedicated paths.
    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        qint64 targetByte = start;
        int targetNibble = nibble;
        if (event->key() == Qt::Key_Right) {
            if (targetNibble == 0) {
                targetNibble = 1;
            } else if (targetByte < m_l2View->virtualFileSize()) {
                ++targetByte;
                targetNibble = 0;
            }
        } else {
            if (targetNibble == 1) {
                targetNibble = 0;
            } else if (targetByte > 0) {
                --targetByte;
                targetNibble = 1;
            }
        }
        setHexCaret(targetByte, targetNibble);
        syncAsciiSelectionFromHex();
        return true;
    }

    if (!hasSelection && event->modifiers() == Qt::NoModifier
        && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
            || event->key() == Qt::Key_PageUp
            || event->key() == Qt::Key_PageDown)) {
        qint64 rowDelta = event->key() == Qt::Key_Up ? -1
            : event->key() == Qt::Key_Down ? 1
            : event->key() == Qt::Key_PageUp
                ? -qMax<qint64>(1, m_editor->SendScintilla(
                    QsciScintillaBase::SCI_LINESONSCREEN))
                : qMax<qint64>(1, m_editor->SendScintilla(
                    QsciScintillaBase::SCI_LINESONSCREEN));
        qint64 targetByte = start + rowDelta * m_hexBytesPerRow;
        if (m_l2View->virtualFileSize() > 0)
            targetByte = qBound<qint64>(
                0, targetByte, m_l2View->virtualFileSize() - 1);
        else
            targetByte = 0;
        setHexCaret(targetByte, nibble);
        syncAsciiSelectionFromHex();
        return true;
    }

    if (!hasSelection
        && (event->modifiers() == Qt::ControlModifier)
        && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        if (event->key() == Qt::Key_Home || m_l2View->virtualFileSize() <= 0)
            setHexCaret(0, 0);
        else
            setHexCaret(m_l2View->virtualFileSize() - 1, 1);
        syncAsciiSelectionFromHex();
        return true;
    }

    // Read-only Hex projections still require byte-aware navigation above,
    // but no mutating key may fall through to Scintilla's text projection.
    if (isReadOnlyByPolicy()) {
        if (event->matches(QKeySequence::Cut)
            || event->matches(QKeySequence::Paste)
            || event->key() == Qt::Key_Delete
            || event->key() == Qt::Key_Backspace
            || event->key() == Qt::Key_Insert
            || (!event->text().isEmpty()
                && !(event->modifiers()
                     & (Qt::ControlModifier | Qt::AltModifier
                        | Qt::MetaModifier)))) {
            return true;
        }
        return false;
    }

    if (event->matches(QKeySequence::Paste)) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        QByteArray pasted;
        QString parseError;
        if (!parseHexBytes(QGuiApplication::clipboard()->text(), &pasted, &parseError)
            || pasted.isEmpty()) {
            showError(parseError.isEmpty()
                          ? QObject::tr("剪贴板中没有十六进制字节。")
                          : parseError);
            return true;
        }
        QString error;
        qint64 replaced = end - start;
        if (!hasSelection && m_hexOverwriteMode) {
            const qint64 available = m_l2View->virtualFileSize() - start;
            if (available <= 0) {
                if (m_secureBinaryDocument && !pasted.isEmpty())
                    pasted.fill(0);
                return true;
            }
            if (pasted.size() > available)
                pasted.truncate(int(available));
            replaced = pasted.size();
        }
        const qint32 oldLength = qint32(qMin<qint64>(replaced, INT_MAX));
        if (!m_l2View->replaceBytes(start, oldLength, pasted, &error)) {
            showError(error);
            return true;
        }
        if (m_secureBinaryDocument && !pasted.isEmpty())
            pasted.fill(0);
        refresh(start + pasted.size(), 0);
        return true;
    }

    if (event->matches(QKeySequence::Cut)) {
        if (!hasSelection)
            return true;
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        QByteArray selected = m_l2View->readByteRange(start, end);
        QGuiApplication::clipboard()->setText(formatHexBytes(selected));
        if (m_secureBinaryDocument && !selected.isEmpty())
            selected.fill(0);
        QString error;
        if (!m_l2View->replaceBytes(start, qint32(end - start), {}, &error))
            showError(error);
        else
            refresh(start, 0);
        return true;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            showError(flushError);
            return true;
        }
        (void)hexSelectionByteRange(&start, &end, &nibble);
        if (!hasSelection) {
            if (event->key() == Qt::Key_Backspace) {
                if (start <= 0)
                    return true;
                --start;
            } else if (start >= m_l2View->virtualFileSize()) {
                return true;
            }
            end = qMin(m_l2View->virtualFileSize(), start + 1);
        }
        if (end <= start)
            return true;
        QString error;
        if (!m_l2View->replaceBytes(start, qint32(end - start), {}, &error))
            showError(error);
        else
            refresh(start, 0);
        return true;
    }

    const QString typed = event->text();
    if (typed.size() == 1 && hexNibble(typed.at(0)) >= 0
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        if (!m_hexOverwriteMode && !hasSelection && nibble == 0) {
            QString flushError;
            if (!bridgeFlushOverlayToPatches(&flushError)) {
                showError(flushError);
                return true;
            }
            QByteArray inserted(1, char(hexNibble(typed.at(0)) << 4));
            QString insertError;
            if (!m_l2View->replaceBytes(start, 0, inserted, &insertError)) {
                showError(insertError);
                return true;
            }
            if (m_secureBinaryDocument)
                inserted.fill(0);
            refresh(start, 1);
            return true;
        }
        // Edit exactly one nibble in the fixed projection.  Both nibbles of a
        // byte are flushed as one byte-level patch on save/window change,
        // avoiding transient half-byte states and overlapping patches.
        const qint64 local = start - m_bridge.windowStart;
        const int line = int(qMin<qint64>(local / m_hexBytesPerRow, INT_MAX));
        const int column = int((local % m_hexBytesPerRow) * 3 + nibble);
        const long position = m_editor->positionFromLineIndex(line, column);
        if (position < 0)
            return true;
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                position, position + 1);
        const QByteArray digit = typed.toUpper().toLatin1();
        m_editor->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                                digit.constData());

        const bool completedByte = nibble == 1;
        const qint64 nextByte = start + (completedByte ? 1 : 0);
        const int nextNibble = completedByte ? 0 : 1;
        const qint64 nextLocal = nextByte - m_bridge.windowStart;
        const int nextLine = int(qMin<qint64>(nextLocal / m_hexBytesPerRow, INT_MAX));
        const int nextColumn = int((nextLocal % m_hexBytesPerRow) * 3 + nextNibble);
        const long nextPosition = m_editor->positionFromLineIndex(nextLine, nextColumn);
        if (nextPosition >= 0)
            m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, nextPosition);

        refreshHexAsciiProjectionFromEditor();
        m_lastL2Line = int(qMin<qint64>(nextByte / m_hexBytesPerRow, INT_MAX));
        m_lastL2Col = int(nextByte % m_hexBytesPerRow);
        m_lastL2Byte = nextByte;
        emit cursorPositionChanged(m_lastL2Line, m_lastL2Col);
        syncAsciiSelectionFromHex();
        return true;
    }

    // Navigation and application shortcuts remain native. Text-producing keys
    // are rejected so spaces/newlines cannot corrupt the byte projection.
    if (!typed.isEmpty()
        && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                  | Qt::MetaModifier))) {
        showError(QObject::tr("十六进制视图只接受 0–9、A–F；ASCII 栏为同步预览。"));
        return true;
    }
    return false;
}

void EditorTab::clearLargeRecoveryOperations(
    QVector<LargeRecoveryOperation> *operations)
{
    if (!operations)
        return;
    for (LargeRecoveryOperation &operation : *operations) {
        if (!operation.insertedBytes.isEmpty())
            operation.insertedBytes.fill(0);
    }
    operations->clear();
}

void EditorTab::resetLargeRecoveryJournal(const QString &basePath)
{
    clearLargeRecoveryOperations(&m_largeRecoveryOperations);
    clearLargeRecoveryOperations(&m_pendingRecoveryOperations);
    const QFileInfo info(basePath);
    m_recoveryBasePath = info.exists() && info.isFile()
                             ? info.absoluteFilePath() : QString();
    m_recoveryBaseSize = info.exists() && info.isFile() ? info.size() : -1;
    m_recoveryBaseMtimeMs = info.exists() && info.isFile()
                                ? info.lastModified().toMSecsSinceEpoch() : -1;
    m_recoveryRequiresCheckpoint = false;
}

bool EditorTab::applyPendingLargeRecovery(QString *error)
{
    if (m_pendingRecoveryOperations.isEmpty())
        return true;
    m_replayingRecoveryJournal = true;
    for (const LargeRecoveryOperation &operation : m_pendingRecoveryOperations) {
        const qint64 documentLength = static_cast<qint64>(directScintilla(
            m_editor, QsciScintillaBase::SCI_GETLENGTH));
        if (operation.position < 0 || operation.position > documentLength
            || (operation.kind == LargeRecoveryOperation::Kind::Delete
                && operation.deleteLength > documentLength - operation.position)) {
            m_replayingRecoveryJournal = false;
            if (error) *error = QObject::tr("恢复日志中的编辑位置已超出文档范围。");
            clearLargeRecoveryOperations(&m_pendingRecoveryOperations);
            return false;
        }
        directScintilla(m_editor, QsciScintillaBase::SCI_SETTARGETRANGE,
                        static_cast<ScintillaUnsigned>(operation.position),
                        static_cast<ScintillaSigned>(
                            operation.kind == LargeRecoveryOperation::Kind::Delete
                                ? operation.position + operation.deleteLength
                                : operation.position));
        const char *replacement = operation.kind == LargeRecoveryOperation::Kind::Insert
                                      ? operation.insertedBytes.constData() : "";
        const qint64 replacementLength = operation.kind == LargeRecoveryOperation::Kind::Insert
                                             ? operation.insertedBytes.size() : 0;
        const auto replaced = directScintilla(
            m_editor, QsciScintillaBase::SCI_REPLACETARGET,
            static_cast<ScintillaUnsigned>(replacementLength),
            reinterpret_cast<ScintillaSigned>(replacement));
        if (replaced < 0) {
            m_replayingRecoveryJournal = false;
            if (error) *error = QObject::tr("Scintilla 拒绝应用大文件恢复日志。");
            clearLargeRecoveryOperations(&m_pendingRecoveryOperations);
            return false;
        }
    }
    m_replayingRecoveryJournal = false;
    clearLargeRecoveryOperations(&m_largeRecoveryOperations);
    m_largeRecoveryOperations = std::move(m_pendingRecoveryOperations);
    m_pendingRecoveryOperations.clear();
    m_editor->setModified(true);
    m_promotedLargeDirty = true;
    return true;
}

void EditorTab::cancelLargeTextPromotion()
{
    if (m_l2PromotionTaskId > 0)
        AsyncTaskManager::instance().cancel(m_l2PromotionTaskId);
    m_l2PromotionTaskId = 0;
    m_l2PromotionPath.clear();
}

void EditorTab::startLargeTextPromotion(const QString &path, qint64 sourceBytes)
{
    if (!m_l2Mode || !m_l2TextPreviewReadOnly || sourceBytes < 0)
        return;

    m_l2FallbackReason.clear();
    const TextEncoding sourceEncoding = sniffLargeTextEncoding(path);
    const qint64 expectedUtf8Bytes = estimatedUtf8Size(sourceBytes, sourceEncoding);
    ResourceAdmissionInput input;
    input.file = FileTierPolicy::classify(sourceBytes, expectedUtf8Bytes);
    if (input.file.tier == FileTier::HugeReadOnly) {
        m_l2FallbackReason = QObject::tr(
            "文件转换为 UTF-8 后预计超过 2 GiB，保持只读分析模式");
        if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
            window->statusBar()->showMessage(
                QObject::tr("文件转换为 UTF-8 后预计超过 2 GiB，保持只读分析模式"), 8000);
        }
        emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(),
                                               m_l2FallbackReason);
        return;
    }
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        input.totalPhysicalBytes = static_cast<qint64>(memory.ullTotalPhys);
        input.availableCommitBytes = static_cast<qint64>(memory.ullAvailPageFile);
    }
#endif
    const ResourceAdmissionDecision admission = ResourceAdmission::evaluate(input);
    if (!admission.admitted) {
        m_l2FallbackReason = admissionFallbackText(admission.reason);
        if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
            window->statusBar()->showMessage(admissionFallbackText(admission.reason), 8000);
        }
        emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(),
                                               m_l2FallbackReason);
        return;
    }

    constexpr int documentOptions = QsciScintillaBase::SC_DOCUMENTOPTION_STYLES_NONE
                                    | QsciScintillaBase::SC_DOCUMENTOPTION_TEXT_LARGE;
    auto *loader = reinterpret_cast<ILoader *>(directScintilla(
        m_editor, QsciScintillaBase::SCI_CREATELOADER,
        static_cast<ScintillaUnsigned>(expectedUtf8Bytes),
        static_cast<ScintillaSigned>(documentOptions)));
    if (!loader) {
        m_l2FallbackReason = QObject::tr(
            "无法建立后台编辑文档，保持只读分析模式");
        if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
            window->statusBar()->showMessage(
                QObject::tr("无法建立后台编辑文档，保持只读分析模式"), 8000);
        }
        emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(),
                                               m_l2FallbackReason);
        return;
    }

    m_l2PromotionPath = QFileInfo(path).absoluteFilePath();
    QPointer<EditorTab> guard(this);
    QPointer<QObject> application = QCoreApplication::instance();
    const QString absolutePath = m_l2PromotionPath;
    m_l2PromotionTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileLoad, AsyncTaskPriority::Low,
        QObject::tr("准备大文件编辑"),
        QObject::tr("后台构造完整 Scintilla 文档；预计峰值 %1 MiB；当前窗口预览保持可用")
            .arg(admission.estimatedPeakBytes / (1024 * 1024)),
        [guard, application, loader, absolutePath, sourceBytes, sourceEncoding](
            qint64 taskId, AsyncCancelPtr cancel,
            const std::function<void(qint64, qint64)> &reportProgress,
            const std::function<void(const AsyncErrorInfo &)> &) {
            auto postResult = [guard, application, taskId, absolutePath](
                                  ILoader *resultLoader, TextEncoding encoding,
                                  const QString &reason) {
                if (!application) {
                    if (resultLoader)
                        resultLoader->Release();
                    return;
                }
                const bool queued = QMetaObject::invokeMethod(
                    application,
                    [guard, resultLoader, taskId, absolutePath, encoding, reason]() {
                        if (guard) {
                            guard->finishLargeTextPromotion(resultLoader, taskId,
                                                            absolutePath, encoding, reason);
                        } else if (resultLoader) {
                            resultLoader->Release();
                        }
                    },
                    Qt::QueuedConnection);
                if (!queued && resultLoader)
                    resultLoader->Release();
            };

            QFile file(absolutePath);
            if (!file.open(QIODevice::ReadOnly)) {
                loader->Release();
                postResult(nullptr, TextEncoding::Utf8,
                           QObject::tr("后台读取文件失败，保持只读分析模式"));
                return;
            }

            constexpr qsizetype chunkBytes = 4 * 1024 * 1024;
            QByteArray chunk(chunkBytes, Qt::Uninitialized);
            Utf8StreamValidator validator;
            std::unique_ptr<QStringDecoder> decoder;
            if (sourceEncoding == TextEncoding::Utf16LE)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::Utf16LE);
            else if (sourceEncoding == TextEncoding::Utf16BE)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::Utf16BE);
            else if (sourceEncoding == TextEncoding::GBK)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::System);
            bool firstChunk = true;
            qint64 consumed = 0;
            while (!file.atEnd()) {
                if (cancel && cancel->load(std::memory_order_acquire)) {
                    loader->Release();
                    postResult(nullptr, sourceEncoding,
                               QObject::tr("用户已取消准备编辑，保持只读分析模式"));
                    return;
                }
                const qint64 count = file.read(chunk.data(), chunk.size());
                if (count < 0) {
                    loader->Release();
                    postResult(nullptr, sourceEncoding,
                               QObject::tr("后台读取文件失败，保持只读分析模式"));
                    return;
                }
                qsizetype offset = 0;
                if (firstChunk) {
                    firstChunk = false;
                    if (sourceEncoding == TextEncoding::Utf8Bom)
                        offset = 3;
                    else if (sourceEncoding == TextEncoding::Utf16LE
                             || sourceEncoding == TextEncoding::Utf16BE)
                        offset = 2;
                }
                const qsizetype payload = static_cast<qsizetype>(count) - offset;
                QByteArray converted;
                const char *data = chunk.constData() + offset;
                qsizetype convertedSize = payload;
                if (decoder) {
                    const QString decoded = (*decoder)(QByteArrayView(data, payload));
                    converted = decoded.toUtf8();
                    data = converted.constData();
                    convertedSize = converted.size();
                } else if (!validator.feed(data, payload)) {
                    loader->Release();
                    postResult(nullptr, sourceEncoding,
                               QObject::tr("文件并非有效 UTF-8，保持只读分析模式"));
                    return;
                }
                if (convertedSize > 0
                    && loader->AddData(data, static_cast<Sci_Position>(convertedSize)) != 0) {
                    loader->Release();
                    postResult(nullptr, sourceEncoding,
                               QObject::tr("准备完整编辑文档时内存不足，保持只读分析模式"));
                    return;
                }
                consumed += count;
                reportProgress(consumed, sourceBytes);
            }
            if ((!decoder && !validator.complete()) || (decoder && decoder->hasError())) {
                loader->Release();
                postResult(nullptr, sourceEncoding,
                           QObject::tr("文件末尾包含不完整编码字符，保持只读分析模式"));
                return;
            }
            if (cancel && cancel->load(std::memory_order_acquire)) {
                loader->Release();
                postResult(nullptr, sourceEncoding,
                           QObject::tr("用户已取消准备编辑，保持只读分析模式"));
                return;
            }
            postResult(loader, sourceEncoding, {});
        },
        0, QStringLiteral("LargeFileLoad"), sourceBytes);

    emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(), {});

    if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
        window->statusBar()->showMessage(
            QObject::tr("文件已快速打开；正在后台准备完整编辑，当前为只读预览"), 6000);
    }
}

void EditorTab::finishLargeTextPromotion(ILoader *loader, qint64 taskId,
                                         const QString &path, TextEncoding encoding,
                                         const QString &fallbackReason)
{
    const bool stillCurrent = m_l2PromotionTaskId == taskId
                              && m_l2PromotionPath == path
                              && m_l2Mode && m_l2TextPreviewReadOnly;
    if (!stillCurrent) {
        if (loader)
            loader->Release();
        return;
    }
    m_l2PromotionTaskId = 0;
    m_l2PromotionPath.clear();

    if (!loader) {
        m_l2FallbackReason = fallbackReason.isEmpty()
            ? QObject::tr("大文件保持只读分析模式") : fallbackReason;
        if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
            window->statusBar()->showMessage(
                fallbackReason.isEmpty()
                    ? QObject::tr("大文件保持只读分析模式") : fallbackReason,
                8000);
        }
        emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(),
                                               m_l2FallbackReason);
        return;
    }

    bool restoreDirtyState = m_l2PromotionRepresentsDirty;
    m_l2PromotionRepresentsDirty = false;

    void *document = loader->ConvertToDocument();
    if (!document) {
        loader->Release();
        m_l2FallbackReason = QObject::tr(
            "完整编辑文档转换失败，保持只读分析模式");
        if (auto *window = qobject_cast<QMainWindow *>(this->window()))
            window->statusBar()->showMessage(m_l2FallbackReason, 8000);
        emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(),
                                               m_l2FallbackReason);
        return;
    }

    const qint64 restoreByteOffset = windowedReadOnlyActive()
        ? m_windowedReadOnlyProvider->globalOffset() : m_lastL2Byte;
    bridgeReparentEditorToStack();
    m_l2Mode = false;
    m_l2TextPreviewReadOnly = false;
    m_l2ReadOnlyByPolicy = false;
    m_l2FallbackReason.clear();
    m_promotedLargeDocument = true;
    m_promotedLargeDirty = restoreDirtyState;
    m_bridge.syncVfvDebounce.stop();
    clearL2BridgeBaseline();
    if (m_l2View && m_l2View->isOpen())
        m_l2View->closeFile(true);
    if (m_windowedReadOnlyProvider)
        m_windowedReadOnlyProvider->closeFile();

    m_editor->SendScintilla(QsciScintillaBase::SCI_SETDOCPOINTER, 0UL, document);
    loader->Release();
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, 1UL);
    m_editor->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_editor->setReadOnly(false);
    m_editor->setWrapMode(QsciScintilla::WrapNone);
    m_editor->setFolding(QsciScintilla::NoFoldStyle);
    m_editor->setAutoCompletionSource(QsciScintilla::AcsNone);
    m_editor->setLexer(nullptr);
    m_editor->setModified(false);
    m_encoding = encoding;
    if (!m_pendingRecoveryOperations.isEmpty()) {
        QString recoveryError;
        if (!applyPendingLargeRecovery(&recoveryError)) {
            restoreDirtyState = false;
            m_promotedLargeDirty = false;
            if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
                window->statusBar()->showMessage(
                    recoveryError.isEmpty() ? QObject::tr("大文件恢复日志应用失败")
                                            : recoveryError,
                    10000);
            }
        } else {
            restoreDirtyState = true;
        }
    }
    if (m_syntax != Syntax::Plain) {
        m_syntax = Syntax::Plain;
        emit syntaxChanged(m_syntax);
    }
    const int lineDigits = qMax(6, QString::number(qMax(1, m_editor->lines())).size() + 1);
    m_editor->setMarginWidth(0, QString(lineDigits, QLatin1Char('9')));
    if (m_stack)
        m_stack->setCurrentWidget(m_editor);
    directScintilla(m_editor, QsciScintillaBase::SCI_GOTOPOS,
                    static_cast<ScintillaUnsigned>(qMax<qint64>(0, restoreByteOffset)));
    directScintilla(m_editor, QsciScintillaBase::SCI_SCROLLCARET);

    emit l2ModeStatusChanged();
    emit l2FileOpenedChanged(false);
    emit encodingChanged(m_encoding);
    emit modificationChanged(restoreDirtyState);
    emit titleChanged(tabTitle());
    emit largeDocumentRuntimeStateChanged(largeDocumentRuntimeState(), {});
    if (auto *window = qobject_cast<QMainWindow *>(this->window())) {
        window->statusBar()->showMessage(
            QObject::tr("大文件已准备完成，现在可以完整编辑"), 6000);
    }
}

bool EditorTab::openLargeFileL2(const QString &path, QString *errMsg,
                                bool allowFullTextPromotion,
                                L2Presentation presentation,
                                L2ContentKind contentKind)
{
    const bool inheritedWordWrap = m_editor
        && m_editor->wrapMode() != QsciScintilla::WrapNone;
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (errMsg) *errMsg = QObject::tr("文件不存在：%1").arg(path);
        return false;
    }
    cancelLargeTextPromotion();
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2PromotionRepresentsDirty = false;
    m_l2FallbackReason.clear();
    m_secureBinaryDocument = false;
    const bool binaryHex = presentation == L2Presentation::Hex;
    m_binaryDocument = contentKind == L2ContentKind::Binary;
    const FileTierDecision tier = FileTierPolicy::classify(fi.size());
    const bool b2HexReadOnlyFallback = binaryHex && tier.mayPromoteToFullEdit();
    const bool readOnlyByPolicy = tier.isReadOnlyByPolicy() || b2HexReadOnlyFallback;
    const bool textPreviewReadOnly = !binaryHex && tier.mayPromoteToFullEdit();
    const bool useWindowedReadOnly = readOnlyByPolicy || textPreviewReadOnly;

    VirtualFileView *v = ensureL2View();
    v->setFullLineIndexBuildEnabled(!useWindowedReadOnly);
    v->setWordWrap(inheritedWordWrap);
    setSecurePayloadLimitExclusive(0);
    // Select the representation before openFile starts emitting progress.  A
    // binary document never needs the text line-index pipeline, even when it
    // is only a few bytes long and is routed through the mmap/Hex editor.
    v->setViewMode(binaryHex ? ViewMode::Hex : ViewMode::Text);
    // 确保 VirtualFileView 已加入 stack（ensureL2View 会 addWidget 到 index=1）
    QString localErr;
    if (!useWindowedReadOnly && !v->openFile(path, &localErr, this->window())) {
        if (errMsg) *errMsg = localErr;
        return false;
    }
    resetLargeRecoveryJournal(path);
    // 成功：切到 L2 视图并标记模式
    m_filePath = fi.absoluteFilePath();
    refreshDiskVersionBaseline();
    m_l2Mode = true;
    // B2 binary/unknown files still lack a proven fixed-length write-through
    // provider.  Keep them useful and predictable in the same single-scroll
    // mmap view as B3 instead of exposing the legacy double-scroll overlay.
    // Text S2 documents continue to promote to the full Scintilla editor.
    m_l2ReadOnlyByPolicy = readOnlyByPolicy;
    // Every S2 text document starts in the same single-scroll readonly
    // provider. allowFullTextPromotion controls only whether ILoader starts;
    // it must never select the legacy editable overlay as a side effect.
    m_l2TextPreviewReadOnly = textPreviewReadOnly;
    m_l2SaveNeeded = false;
    m_lastL2Line = 0;
    m_lastL2Col  = 0;
    m_lastL2Byte = 0;
    if (useWindowedReadOnly) {
        WindowedReadOnlyProvider *provider = ensureWindowedReadOnlyProvider();
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
        if (!binaryHex)
            provider->setWordWrap(inheritedWordWrap);
        m_stack->setCurrentWidget(provider);
        provider->editor()->setFocus();
    } else {
        // Transitional editable B2/secure-binary path. S2 text preview and all
        // S3/B3 documents never enter the legacy double-scroll overlay.
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
    if (allowFullTextPromotion && !binaryHex && m_l2TextPreviewReadOnly) {
        QTimer::singleShot(0, this, [this, path, bytes = fi.size()]() {
            startLargeTextPromotion(path, bytes);
        });
    }
    return true;
}

bool EditorTab::saveL2As(const QString &path, QString *errMsg)
{
    if (!m_l2Mode || !m_l2View) {
        if (errMsg) *errMsg = QObject::tr("当前非 L2 模式");
        return false;
    }
    if (sameDiskFilePath(m_filePath, path)) {
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
    // P0-5：保存前强制 flush overlay → patches。最近一次编辑没切窗口时就靠这里回写。
    QString flushErr;
    bridgeFlushOverlayToPatches(&flushErr);
    if (!flushErr.isEmpty()) {
        if (errMsg) *errMsg = QObject::tr("保存前回写窗口修改失败：%1").arg(flushErr);
        return false;
    }
    std::atomic<bool> cancel{false};
    QString localErr;

    // 判断是否保存到当前 mmap 映射的同一文件
    const bool sameFile = !m_filePath.isEmpty() &&
        QFileInfo(path).absoluteFilePath() == QFileInfo(m_filePath).absoluteFilePath();

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
                *errMsg = QObject::tr("无法生成完整的 B1 保存快照。");
            if (!snapshot.isEmpty()) snapshot.fill(0);
            return false;
        }
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)
            || output.write(snapshot) != snapshot.size()) {
            if (errMsg)
                *errMsg = QObject::tr("无法写入 B1 原子保存数据：%1")
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
            m_l2View->openFile(path, &reopenError, this->window());
            if (errMsg)
                *errMsg = QObject::tr("B1 原子保存失败：%1").arg(commitError);
            if (!snapshot.isEmpty()) snapshot.fill(0);
            return false;
        }
        if (!snapshot.isEmpty()) snapshot.fill(0);
        QString reopenError;
        m_l2View->setViewMode(presentation);
        if (!m_l2View->openFile(path, &reopenError, this->window())) {
            if (errMsg)
                *errMsg = QObject::tr("保存成功但重新打开失败：%1").arg(reopenError);
            return false;
        }
        m_filePath = QFileInfo(path).absoluteFilePath();
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
        m_filePath = path;  // sameFile 路径也同步（openFile 可能没更新 EditorTab::m_filePath，确保 tab 标题/后续状态正确）
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
        ok = m_l2View->openFile(path, &reopenErr, this->window());
        if (!ok) {
            if (errMsg)
                *errMsg = QObject::tr("文件已另存为，但无法切换到新文件：%1").arg(reopenErr);
            return false;
        }
        m_filePath = QFileInfo(path).absoluteFilePath();
    }

    setModified(false);
    resetLargeRecoveryJournal(m_filePath);
    clearAutosaveFile();
    m_autosavePath.clear();
    refreshDiskVersionBaseline();
    emit titleChanged(tabTitle());
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

    if (m_promotedLargeDocument) {
        QSaveFile output(destPath);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly))
            return false;
        QString error;
        if (!streamPromotedDocumentTo(&output, &error, false)) {
            output.cancelWriting();
            return false;
        }
        return output.commit();
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
    if (!data) {
        if (error) *error = QObject::tr("恢复载荷输出参数无效。");
        return false;
    }
    if (!m_promotedLargeDocument)
        return exportSnapshotBytes(data);
    if (m_recoveryRequiresCheckpoint
        || QFileInfo(m_recoveryBasePath).absoluteFilePath()
               != QFileInfo(m_filePath).absoluteFilePath()
        || m_recoveryBaseSize < 0 || m_recoveryBaseMtimeMs < 0
        || m_largeRecoveryOperations.isEmpty()) {
        if (error) *error = QObject::tr("当前大文件恢复状态需要建立加密检查点。");
        return false;
    }

    data->clear();
    QDataStream stream(data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << QByteArray(LargeRecoveryMagic, sizeof(LargeRecoveryMagic))
           << LargeRecoveryVersion
           << m_recoveryBaseSize << m_recoveryBaseMtimeMs
           << qint32(m_encoding)
           << quint32(m_largeRecoveryOperations.size());
    for (const LargeRecoveryOperation &operation : m_largeRecoveryOperations) {
        stream << quint8(operation.kind) << operation.position;
        if (operation.kind == LargeRecoveryOperation::Kind::Insert)
            stream << operation.insertedBytes;
        else
            stream << operation.deleteLength;
        if (data->size() >= MaximumLargeRecoveryPayload) {
            data->fill(0);
            data->clear();
            if (error) *error = QObject::tr("大文件编辑日志已达到 32 MiB，需要建立加密检查点。");
            return false;
        }
    }
    if (stream.status() != QDataStream::Ok) {
        data->fill(0);
        data->clear();
        if (error) *error = QObject::tr("序列化大文件恢复日志失败。");
        return false;
    }
    return true;
}

bool EditorTab::restoreRecoveryPayload(const QByteArray &data,
                                       const QString &realFilePath,
                                       QString *error)
{
    QDataStream stream(data);
    stream.setByteOrder(QDataStream::LittleEndian);
    QByteArray magic;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != QByteArray(LargeRecoveryMagic, sizeof(LargeRecoveryMagic)))
        return loadAutosaveBytes(data, QString());
    if (version != LargeRecoveryVersion || realFilePath.isEmpty()) {
        if (error) *error = QObject::tr("大文件恢复日志版本或原文件路径无效。");
        return false;
    }

    qint64 baseSize = -1;
    qint64 baseMtime = -1;
    qint32 storedEncoding = 0;
    quint32 operationCount = 0;
    stream >> baseSize >> baseMtime >> storedEncoding >> operationCount;
    const QFileInfo base(realFilePath);
    if (!base.exists() || !base.isFile() || base.size() != baseSize
        || base.lastModified().toMSecsSinceEpoch() != baseMtime) {
        if (error) *error = QObject::tr("原始大文件已移动或发生变化，不能安全套用恢复日志。");
        return false;
    }
    if (storedEncoding < qint32(TextEncoding::Utf8)
        || storedEncoding > qint32(TextEncoding::GBK)
        || operationCount == 0 || operationCount > 1000000) {
        if (error) *error = QObject::tr("大文件恢复日志元数据无效。");
        return false;
    }

    QVector<LargeRecoveryOperation> parsed;
    parsed.reserve(int(operationCount));
    qint64 insertedTotal = 0;
    for (quint32 i = 0; i < operationCount; ++i) {
        quint8 kind = 0;
        LargeRecoveryOperation operation;
        stream >> kind >> operation.position;
        if (kind == quint8(LargeRecoveryOperation::Kind::Insert)) {
            operation.kind = LargeRecoveryOperation::Kind::Insert;
            stream >> operation.insertedBytes;
            insertedTotal += operation.insertedBytes.size();
            if (operation.insertedBytes.isEmpty()
                || insertedTotal >= MaximumLargeRecoveryPayload) {
                clearLargeRecoveryOperations(&parsed);
                if (error) *error = QObject::tr("大文件恢复日志插入数据无效或超限。");
                return false;
            }
        } else if (kind == quint8(LargeRecoveryOperation::Kind::Delete)) {
            operation.kind = LargeRecoveryOperation::Kind::Delete;
            stream >> operation.deleteLength;
            if (operation.deleteLength <= 0) {
                clearLargeRecoveryOperations(&parsed);
                if (error) *error = QObject::tr("大文件恢复日志删除范围无效。");
                return false;
            }
        } else {
            clearLargeRecoveryOperations(&parsed);
            if (error) *error = QObject::tr("大文件恢复日志包含未知操作。");
            return false;
        }
        if (operation.position < 0 || stream.status() != QDataStream::Ok) {
            clearLargeRecoveryOperations(&parsed);
            if (error) *error = QObject::tr("大文件恢复日志已损坏。");
            return false;
        }
        parsed.push_back(std::move(operation));
    }
    if (!stream.atEnd()) {
        clearLargeRecoveryOperations(&parsed);
        if (error) *error = QObject::tr("大文件恢复日志尾部包含无效数据。");
        return false;
    }

    QString openError;
    if (!openLargeFileL2(realFilePath, &openError, true)) {
        clearLargeRecoveryOperations(&parsed);
        if (error) *error = openError;
        return false;
    }
    clearLargeRecoveryOperations(&m_pendingRecoveryOperations);
    m_pendingRecoveryOperations = std::move(parsed);
    m_encoding = TextEncoding(storedEncoding);
    m_l2PromotionRepresentsDirty = true;
    m_l2SaveNeeded = true;
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::exportRecoveryCheckpoint(QIODevice *output, QString *error) const
{
    if (!m_promotedLargeDocument || !output || !output->isWritable()) {
        if (error) *error = QObject::tr("当前文档不能生成流式恢复检查点。");
        return false;
    }
    return streamPromotedDocumentTo(output, error, false);
}

bool EditorTab::restoreRecoveryCheckpoint(const QString &checkpointPath,
                                          const QString &realFilePath,
                                          TextEncoding encoding,
                                          qint64 plaintextBytes,
                                          QString *error)
{
    if (checkpointPath.isEmpty() || plaintextBytes < FileTierPolicy::StandardEditLimitBytes
        || plaintextBytes >= FileTierPolicy::LargeEditLimitBytes) {
        if (error) *error = QObject::tr("恢复检查点大小不属于 S2 可编辑范围。");
        return false;
    }
    const qint64 expectedUtf8Bytes = estimatedUtf8Size(plaintextBytes, encoding);
    ResourceAdmissionInput input;
    input.file = FileTierPolicy::classify(plaintextBytes, expectedUtf8Bytes);
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        input.totalPhysicalBytes = static_cast<qint64>(memory.ullTotalPhys);
        input.availableCommitBytes = static_cast<qint64>(memory.ullAvailPageFile);
    }
#endif
    const ResourceAdmissionDecision admission = ResourceAdmission::evaluate(input);
    if (!admission.admitted) {
        if (error) *error = admissionFallbackText(admission.reason);
        return false;
    }

    constexpr int documentOptions = QsciScintillaBase::SC_DOCUMENTOPTION_STYLES_NONE
                                    | QsciScintillaBase::SC_DOCUMENTOPTION_TEXT_LARGE;
    auto *loader = reinterpret_cast<ILoader *>(directScintilla(
        m_editor, QsciScintillaBase::SCI_CREATELOADER,
        static_cast<ScintillaUnsigned>(expectedUtf8Bytes),
        static_cast<ScintillaSigned>(documentOptions)));
    if (!loader) {
        if (error) *error = QObject::tr("无法建立后台恢复文档。");
        return false;
    }

    cancelLargeTextPromotion();
    resetLargeRecoveryJournal(realFilePath);
    m_recoveryRequiresCheckpoint = true;
    m_filePath = realFilePath;
    refreshDiskVersionBaseline();
    m_l2Mode = true;
    m_l2ReadOnlyByPolicy = false;
    m_l2TextPreviewReadOnly = true;
    m_promotedLargeDocument = false;
    m_promotedLargeDirty = false;
    m_l2SaveNeeded = true;
    m_l2PromotionRepresentsDirty = true;
    m_editor->setReadOnly(true);
    if (m_stack)
        m_stack->setCurrentWidget(m_editor);

    m_l2PromotionPath = QFileInfo(checkpointPath).absoluteFilePath();
    const QString absolutePath = m_l2PromotionPath;
    QPointer<EditorTab> guard(this);
    QPointer<QObject> application = QCoreApplication::instance();
    m_l2PromotionTaskId = AsyncTaskManager::instance().submit(
        AsyncTaskType::LargeFileLoad, AsyncTaskPriority::Low,
        QObject::tr("恢复大文件编辑"),
        QObject::tr("后台解密恢复检查点并构造完整 Scintilla 文档"),
        [guard, application, loader, absolutePath, plaintextBytes, encoding](
            qint64 taskId, AsyncCancelPtr cancel,
            const std::function<void(qint64, qint64)> &reportProgress,
            const std::function<void(const AsyncErrorInfo &)> &) {
            auto postResult = [guard, application, taskId, absolutePath](
                                  ILoader *resultLoader, TextEncoding resultEncoding,
                                  const QString &reason) {
                if (!application) {
                    if (resultLoader) resultLoader->Release();
                    return;
                }
                const bool queued = QMetaObject::invokeMethod(
                    application,
                    [guard, resultLoader, taskId, absolutePath, resultEncoding, reason]() {
                        if (guard) {
                            guard->finishLargeTextPromotion(resultLoader, taskId,
                                                            absolutePath, resultEncoding,
                                                            reason);
                        } else if (resultLoader) {
                            resultLoader->Release();
                        }
                    }, Qt::QueuedConnection);
                if (!queued && resultLoader)
                    resultLoader->Release();
            };

            std::unique_ptr<QStringDecoder> decoder;
            if (encoding == TextEncoding::Utf16LE)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::Utf16LE);
            else if (encoding == TextEncoding::Utf16BE)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::Utf16BE);
            else if (encoding == TextEncoding::GBK)
                decoder = std::make_unique<QStringDecoder>(QStringConverter::System);
            Utf8StreamValidator validator;
            bool firstChunk = true;
            qint64 consumed = 0;
            QString readError;
            const bool readOk = DraftStore::readChunks(
                absolutePath,
                [&](const QByteArray &chunk, QString *consumerError) {
                    if (cancel && cancel->load(std::memory_order_acquire)) {
                        if (consumerError) *consumerError = QObject::tr("恢复已取消。");
                        return false;
                    }
                    qsizetype offset = 0;
                    if (firstChunk) {
                        firstChunk = false;
                        if (encoding == TextEncoding::Utf8Bom)
                            offset = qMin<qsizetype>(3, chunk.size());
                        else if (encoding == TextEncoding::Utf16LE
                                 || encoding == TextEncoding::Utf16BE)
                            offset = qMin<qsizetype>(2, chunk.size());
                    }
                    const char *source = chunk.constData() + offset;
                    qsizetype sourceSize = chunk.size() - offset;
                    QByteArray converted;
                    if (decoder) {
                        const QString decoded = (*decoder)(QByteArrayView(source, sourceSize));
                        converted = decoded.toUtf8();
                        source = converted.constData();
                        sourceSize = converted.size();
                    } else if (!validator.feed(source, sourceSize)) {
                        if (consumerError) *consumerError = QObject::tr("恢复检查点包含无效 UTF-8。");
                        return false;
                    }
                    if (sourceSize > 0
                        && loader->AddData(source, static_cast<Sci_Position>(sourceSize)) != 0) {
                        if (consumerError) *consumerError = QObject::tr("恢复大文件时内存不足。");
                        return false;
                    }
                    consumed += chunk.size();
                    reportProgress(consumed, plaintextBytes);
                    return true;
                }, &readError);
            if (!readOk || (decoder && decoder->hasError())
                || (!decoder && !validator.complete())) {
                loader->Release();
                postResult(nullptr, encoding,
                           readError.isEmpty() ? QObject::tr("恢复检查点解密或编码转换失败。")
                                               : readError);
                return;
            }
            postResult(loader, encoding, {});
        }, 0, QStringLiteral("RecoveryLoad"), plaintextBytes);

    emit l2ModeStatusChanged();
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
    return true;
}

bool EditorTab::exportBinaryRecoveryJournal(QIODevice *output, QString *error)
{
    if (!output || !output->isWritable() || !isBinaryDocument() || isSecureDocument()
        || !isL2Mode() || !m_l2View || m_filePath.isEmpty()) {
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
    if (!openLargeFileL2(realFilePath, &openError, false,
                         L2Presentation::Hex, L2ContentKind::Binary)) {
        if (error) *error = openError;
        journal.fill(0);
        return false;
    }
    // openLargeFileL2 intentionally returns as soon as the asynchronous mmap
    // task is scheduled.  Recovery patches must not be installed before
    // onMmapReady(), because normal mmap initialization resets PatchList.
    // Wait only for this Hex mapping boundary (no line index is required).
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
            m_filePath = realFilePath;
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
    const bool ok = openLargeFileL2(autosavePath, &err, true);
    qDebug("[openAutosaveAutoMode] 统一 mmap 返回 ok=%d err='%s', 耗时=%lld ms",
           ok ? 1 : 0, qUtf8Printable(err), t.elapsed());
    if (!ok) {
        qWarning("[openAutosaveAutoMode] mmap 恢复失败，安全跳过且不做同步全量回退: %s",
                 qUtf8Printable(err));
        return false;
    }

    m_filePath = realFilePath;
    refreshDiskVersionBaseline();
    m_autosavePath = autosavePath;
    m_l2SaveNeeded = true;
    m_l2PromotionRepresentsDirty = true;
    emit modificationChanged(true);
    emit titleChanged(tabTitle());
    emit l2SaveNeededChanged(true);
    return true;
}

// ============================================================================
//  统一分流：编辑/剪贴板/跳转（外部不要直接调 editor()->xxx()，走这里）
// ============================================================================

void EditorTab::selectAll()
{
    if (windowedReadOnlyActive()) {
        m_windowedReadOnlyProvider->selectAllSource();
        return;
    }
    if (isL2Mode() && m_l2View) {
        // Scintilla is the only visible/input surface in L2. Select the active
        // window; selecting multi-GiB virtual content would be unsafe anyway.
        m_editor->selectAll();
        return;
    }
    m_editor->selectAll();
}

bool EditorTab::canModifyCommand() const
{
    if (m_secureDocumentMasked || isSecureDocumentTransitioning()
        || windowedReadOnlyActive() || isReadOnlyByPolicy() || !m_editor) {
        return false;
    }
    return !m_editor->isReadOnly();
}

bool EditorTab::canUndoCommand() const
{
    if (!canModifyCommand())
        return false;
    if (isL2Mode() && m_l2View)
        return m_editor->isUndoAvailable() || m_l2View->canUndo();
    return m_editor->isUndoAvailable();
}

bool EditorTab::canRedoCommand() const
{
    if (!canModifyCommand())
        return false;
    if (isL2Mode() && m_l2View)
        return m_editor->isRedoAvailable() || m_l2View->canRedo();
    return m_editor->isRedoAvailable();
}

bool EditorTab::hasSelectionCommand() const
{
    if (m_secureDocumentMasked)
        return false;
    if (windowedReadOnlyActive()) {
        return m_windowedReadOnlyProvider->selectionEndOffset()
               > m_windowedReadOnlyProvider->selectionStartOffset();
    }
    if (!m_editor)
        return false;
    if (isL2Mode() && m_l2View && m_l2View->viewMode() == ViewMode::Hex) {
        qint64 start = 0;
        qint64 end = 0;
        return hexSelectionByteRange(&start, &end) && end > start;
    }
    return m_editor->hasSelectedText();
}

void EditorTab::undo()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_editor->isUndoAvailable()) {
            m_editor->undo();
            if (m_l2View->viewMode() == ViewMode::Hex)
                refreshHexAsciiProjectionFromEditor();
        } else if (m_l2View->undo(nullptr)) {
            bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                    m_l2View->activeWindowByteEnd());
        }
        return;
    }
    m_editor->undo();
}

void EditorTab::redo()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_editor->isRedoAvailable()) {
            m_editor->redo();
            if (m_l2View->viewMode() == ViewMode::Hex)
                refreshHexAsciiProjectionFromEditor();
        } else if (m_l2View->redo(nullptr)) {
            bridgePopulateScintilla(m_l2View->activeWindowByteStart(),
                                    m_l2View->activeWindowByteEnd());
        }
        return;
    }
    m_editor->redo();
}

void EditorTab::cut()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier);
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused)
                (void)handleHexAsciiKeyPress(&event);
            else
                (void)handleHexKeyPress(&event);
            return;
        }
        m_editor->cut();
        return;
    }
    m_editor->cut();
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
                QByteArray selected = m_l2View->readByteRange(start, end);
                ByteClipboard::setHexSelection(selected);
                if (m_secureBinaryDocument && !selected.isEmpty())
                    selected.fill(0);
            }
            return;
        }
        m_editor->copy();
        return;
    }
    m_editor->copy();
}

void EditorTab::paste()
{
    if (isReadOnlyByPolicy()) return;
    if (isL2Mode() && m_l2View) {
        if (m_l2View->viewMode() == ViewMode::Hex) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
            const bool asciiFocused = m_hexAsciiPreview
                && (m_hexAsciiPreview->hasFocus()
                    || m_hexAsciiPreview->viewport()->hasFocus());
            if (asciiFocused)
                (void)handleHexAsciiKeyPress(&event);
            else
                (void)handleHexKeyPress(&event);
            return;
        }
        m_editor->paste();
        return;
    }
    m_editor->paste();
}

void EditorTab::setFocusEditor()
{
    if (windowedReadOnlyActive()) {
        m_windowedReadOnlyProvider->editor()->setFocus();
        return;
    }
    if (isL2Mode() && m_l2View) {
        m_editor->setFocus();
        return;
    }
    m_editor->setFocus();
}

void EditorTab::gotoByteOffset(qint64 byteOffset, int selectLength)
{
    if (windowedReadOnlyActive()) {
        byteOffset = qBound<qint64>(
            0, byteOffset,
            qMax<qint64>(0, m_windowedReadOnlyProvider->fileSize() - 1));
        m_windowedReadOnlyProvider->goToOffset(byteOffset, selectLength);
        m_lastL2Byte = byteOffset;
        m_windowedReadOnlyProvider->editor()->setFocus();
        return;
    }
    if (isL2Mode() && m_l2View) {
        QString flushError;
        if (!bridgeFlushOverlayToPatches(&flushError)) {
            if (auto *window = qobject_cast<QMainWindow *>(this->window()))
                window->statusBar()->showMessage(flushError, 6000);
            return;
        }
        byteOffset = qBound<qint64>(0, byteOffset, m_l2View->virtualFileSize());
        m_l2View->syncActiveWindowToCursor(byteOffset);
        const qint64 local = qMax<qint64>(0, byteOffset - m_bridge.windowStart);
        auto overlayPosition = [this](qint64 localByte) -> long {
            if (m_l2View->viewMode() != ViewMode::Hex)
                return long(qMin<qint64>(localByte, LONG_MAX));
            const int line = int(qMin<qint64>(localByte / m_hexBytesPerRow, INT_MAX));
            const int column = int((localByte % m_hexBytesPerRow) * 3);
            return m_editor->positionFromLineIndex(line, column);
        };
        const long start = overlayPosition(local);
        if (selectLength > 0) {
            m_l2View->selectBytes(byteOffset, byteOffset + qint64(selectLength));
            const long end = overlayPosition(local + selectLength);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL, start, end);
        } else {
            m_l2View->gotoByteOffset(byteOffset, false);
            m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, start);
        }
        m_editor->ensureCursorVisible();
        m_editor->setFocus();
        return;
    }
    // L1：用 Scintilla SCI_GOTOPOS（字节偏移，UTF-8）定位，然后读取 line/col 更新
    if (byteOffset < 0) byteOffset = 0;
    if (byteOffset > INT_MAX) byteOffset = INT_MAX;
    if (selectLength > 0) {
        const qint64 end = qMin<qint64>(m_editor->length(),
                                        byteOffset + selectLength);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                static_cast<long>(byteOffset),
                                static_cast<long>(end));
    } else {
        m_editor->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                static_cast<long>(byteOffset));
    }
    int line, col;
    m_editor->getCursorPosition(&line, &col);
    emit cursorPositionChanged(line, col);
}

// ============================================================================
//  搜索分流：L1=PCRE2 内存快照；L2=PCRE2 mmap/patch 快照
// ============================================================================

namespace {  // 匿名：L2 findAll 同步等待 helper
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
    const quint64 searchRevision = m_contentRevision;

    // Byte offsets are authoritative. An incomplete LineIndex may postpone the
    // row label, but must never postpone scanning or exact navigation.

    LargeFileSearch *s = m_l2View->searchEngine();  // 懒创建 + setSource 已内部完成
    if (!s) return nullptr;

    LargeFileSearch::Options opts;
    opts.regexMode     = regex;
    opts.hexBytesMode  = hexBytes;
    opts.byteContextMode = isBinaryDocument();
    opts.caseSensitive = hexBytes ? true : caseSensitive;
    opts.wholeWord     = hexBytes ? false : wholeWord;

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

    s->findAll(pattern, opts);
    if (!aw.done)
        aw.loop.exec();

    QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
    if (m_contentRevision != searchRevision)
        return nullptr;
    if (!aw.err.isEmpty() || aw.total <= 0) {
        // 0 匹配也返回空的 model（Dock 可以显示 0 结果）
    }
    auto *model = new SearchResultsModel(parentForModel);
    model->setBytePositionMode(isBinaryDocument());
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
    m_l2SearchCacheRevision = m_contentRevision;
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

void EditorTab::startPendingL2Search()
{
    if (!m_pendingL2Search.active || !m_l2View) {
        resetPendingL2Search();
        return;
    }
    if (m_contentRevision != m_pendingL2Search.contentRevision) {
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
        empty->setBytePositionMode(isBinaryDocument());
        emit findAllL2Finished(empty, m_pendingL2Search.pattern, fileName());
        resetPendingL2Search();
        return;
    }

    LargeFileSearch::Options opts;
    opts.regexMode     = m_pendingL2Search.regex;
    opts.hexBytesMode  = m_pendingL2Search.hexBytes;
    opts.byteContextMode = isBinaryDocument();
    opts.caseSensitive = m_pendingL2Search.hexBytes
        ? true : m_pendingL2Search.caseSensitive;
    opts.wholeWord     = m_pendingL2Search.hexBytes
        ? false : m_pendingL2Search.wholeWord;

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

    m_pendingL2Search.searchProgressConn = QObject::connect(
        s, &LargeFileSearch::progress, this,
        [this, that](qint64 done, qint64 total) {
            if (!that) return;
            int pct = (total > 0) ? int((done * 100LL) / total) : -1;
            emit findAllL2Progress(pct, done, total);
        });

    m_pendingL2Search.searchPartialConn = QObject::connect(
        s, &LargeFileSearch::partialBatch, this,
        [this, that, patternCopy, fileNameCopy](
            const QVector<LargeFileSearchMatch> &batch) {
            if (!that || !that->m_pendingL2Search.active
                || that->m_pendingL2Search.previewMatchesPublished >= 5000) {
                return;
            }
            const int remaining = 5000 - that->m_pendingL2Search.previewMatchesPublished;
            const QVector<LargeFileSearchMatch> preview = batch.size() <= remaining
                ? batch : batch.mid(0, remaining);
            that->m_pendingL2Search.previewMatchesPublished += preview.size();
            emit that->findAllL2Partial(preview, patternCopy, fileNameCopy);
        });

    m_pendingL2Search.searchFinishedConn = QObject::connect(
        s, &LargeFileSearch::finished, this,
        [this, that, s, parentForModel, patternCopy, fileNameCopy,
         revisionCopy](qint64 /*total*/) {
            if (!that) return;
            if (that->m_contentRevision != revisionCopy) {
                that->clearSearchHighlights();
                that->resetPendingL2Search();
                emit that->findAllL2Finished(nullptr, patternCopy, fileNameCopy);
                return;
            }
            auto *model = new SearchResultsModel(parentForModel);
            model->setBytePositionMode(that->isBinaryDocument());
            model->setResults(s->memoryMatches(), s->overrunFilePath(), s->overrunCount(),
                              s->overrunCheckpoints(), s->overrunGroupedCount(), 2);
            that->m_l2SearchCachePattern = patternCopy;
            that->m_l2SearchCacheRegex = that->m_pendingL2Search.regex;
            that->m_l2SearchCacheCaseSensitive = that->m_pendingL2Search.caseSensitive;
            that->m_l2SearchCacheWholeWord = that->m_pendingL2Search.wholeWord;
            that->m_l2SearchCacheHexBytes = that->m_pendingL2Search.hexBytes;
            that->m_l2SearchCacheRevision = revisionCopy;
            that->m_l2SearchCacheValid = true;
            that->m_lastSearchError.clear();
            // 把匹配位置传给 VirtualFileView 用于正文区域染色
            if (that->m_l2View) {
                QVector<QPair<qint64, qint32>> highlights;
                const auto& memMatches = s->memoryMatches();
                highlights.reserve(memMatches.size());
                for (const auto& m : memMatches) {
                    highlights.append(qMakePair(m.byteOffset, m.length));
                }
                that->m_l2SearchHighlights = highlights;
                that->m_l2View->setSearchHighlights(highlights);
                that->applyL2SearchHighlightsToOverlay();
            }
            emit findAllL2Finished(model, patternCopy, fileNameCopy);
            that->resetPendingL2Search();
        });

    m_pendingL2Search.searchFailedConn = QObject::connect(
        s, &LargeFileSearch::failed, this,
        [this, that, s, parentForModel, patternCopy, fileNameCopy,
         revisionCopy](const QString& msg) {
            if (!that) return;
            if (that->m_contentRevision != revisionCopy) {
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
            partial->setBytePositionMode(that->isBinaryDocument());
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
    m_pendingL2Search.contentRevision = m_contentRevision;
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
            partial->setBytePositionMode(isBinaryDocument());
            partial->setIncompleteReason(message);
            resetPendingL2Search();
            emit findAllL2Finished(partial, patternCopy, fileNameCopy);
        });
    return true;
}

void EditorTab::cancelFindAllL2()
{
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
        partial->setBytePositionMode(isBinaryDocument());
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
            && m_l1SearchCacheRevision == m_contentRevision
            && m_l1SearchCachePattern == pattern
            && m_l1SearchCacheRegex == regex
            && m_l1SearchCacheCaseSensitive == caseSensitive
            && m_l1SearchCacheWholeWord == wholeWord;
        if (!cacheMatches)
            findAll(pattern, regex, caseSensitive, wholeWord);
        if (!m_l1SearchCacheValid || !m_lastSearchError.isEmpty()
            || m_l1SearchCache.isEmpty()) {
            return false;
        }

        const qint64 selectionStart = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        const qint64 selectionEnd = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
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
        && m_l2SearchCacheRevision == m_contentRevision
        && m_l2SearchCachePattern == pattern
        && m_l2SearchCacheRegex == regex
        && m_l2SearchCacheCaseSensitive == caseSensitive
        && m_l2SearchCacheWholeWord == wholeWord
        && m_l2SearchCacheHexBytes == hexBytes;
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
    qint64 from = m_lastL2Byte;
    if (windowedReadOnlyActive()
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
        if (hexBytes || !m_l1SearchCacheValid
            || m_l1SearchCacheRevision != m_contentRevision
            || m_l1SearchCachePattern != pattern
            || m_l1SearchCacheRegex != regex
            || m_l1SearchCacheCaseSensitive != caseSensitive
            || m_l1SearchCacheWholeWord != wholeWord) {
            return false;
        }
        qint64 start = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONSTART);
        qint64 end = m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETSELECTIONEND);
        if (end < start)
            std::swap(start, end);
        if (end <= start)
            return false;
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
    if (!m_l2View || (!hasWindowedSelection && !m_l2View->hasSelection())
        || !m_l2SearchCacheValid
        || m_l2SearchCacheRevision != m_contentRevision
        || m_l2SearchCachePattern != pattern
        || m_l2SearchCacheRegex != regex
        || m_l2SearchCacheCaseSensitive != caseSensitive
        || m_l2SearchCacheWholeWord != wholeWord
        || m_l2SearchCacheHexBytes != hexBytes) {
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

    QByteArray bytes = replacement.toUtf8();
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
        if (!subject || !compiled.compile(
                find, false, caseSensitive, wholeWord, &expansionError)
            || !compiled.expandReplacementAt(
                subject, subjectSize, start, end - start,
                replacement, &bytes, &expansionError)) {
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
    const QByteArray newData = replaceText.toUtf8();
    QString err;
    const bool ok = m_l2View->replaceBytes(s, oldLen, newData, &err);
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
