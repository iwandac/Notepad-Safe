#include "ReadOnlyTailController.h"
#include "WindowedReadOnlyProvider.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QWheelEvent>
#include <Qsci/qsciscintilla.h>
#include <algorithm>
#include <utility>

namespace {
constexpr qint64 kEofOverlapBytes = 64 * 1024;
}

ReadOnlyTailController::ReadOnlyTailController(WindowedReadOnlyProvider *provider)
    : QWidget(provider), m_provider(provider)
{
    setObjectName(QStringLiteral("tailStatusBar"));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 4, 8, 4);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tailStatusLabel"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_resume = new QPushButton(tr("回到末尾"), this);
    m_resume->setObjectName(QStringLiteral("tailResumeButton"));
    m_resume->setAccessibleName(tr("回到末尾"));
    row->addWidget(m_status, 1);
    row->addWidget(m_resume);
    connect(m_resume, &QPushButton::clicked, this, &ReadOnlyTailController::resumeAtEnd);
    connect(provider, &WindowedReadOnlyProvider::characterCountChanged,
            this, &ReadOnlyTailController::updateStatus);
    for (QObject *surface : {static_cast<QObject *>(provider),
                            static_cast<QObject *>(provider->editor()),
                            static_cast<QObject *>(provider->editor()->viewport()),
                            static_cast<QObject *>(provider->asciiPreview()),
                            static_cast<QObject *>(provider->asciiPreview()->viewport()),
                            static_cast<QObject *>(provider->globalScrollBar())})
        surface->installEventFilter(this);
    connect(provider->editor(), &QsciScintilla::selectionChanged, this, [this] {
        if (!internalProjection() && m_provider->editor()->hasSelectedText()) pauseForReading();
    });
    connect(provider->asciiPreview(), &QPlainTextEdit::selectionChanged, this, [this] {
        if (!internalProjection() && m_provider->asciiPreview()->textCursor().hasSelection())
            pauseForReading();
    });
    connect(provider->globalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!internalProjection() && !m_provider->m_updatingScrollBar
            && value < m_provider->globalScrollBar()->maximum()) pauseForReading();
    });
    connect(provider->editor()->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this] {
        if (!internalProjection() && !atEnd()) pauseForReading();
    });
    updateStatus();
}

QString ReadOnlyTailController::statusText() const
{
    switch (m_state) {
    case State::Off: return tr("文件跟随已关闭");
    case State::Following: return tr("正在跟随文件末尾");
    case State::Paused: return tr("已暂停跟随，你可以继续查看上方内容");
    case State::Stopped: return tr("已停止跟随：%1").arg(m_stopReason);
    }
    return {};
}

void ReadOnlyTailController::updateStatus()
{
    QString description = statusText();
    if (m_provider && m_provider->statisticsDeferred() && !m_provider->binaryContent()
        && m_provider->presentation() == WindowedReadOnlyProvider::Presentation::Text)
        description += tr("；文件持续变化，行号和字符统计未更新");
    m_status->setText(description);
    m_status->setAccessibleName(tr("文件跟随状态：%1").arg(description));
    m_status->setAccessibleDescription(description);
    setAccessibleName(tr("文件跟随状态"));
    setAccessibleDescription(description);
    m_resume->setVisible(canResume());
    m_resume->setEnabled(canResume());
}

void ReadOnlyTailController::setState(State state, const QString &reason)
{
    if (m_state == state && m_stopReason == reason) return;
    m_state = state;
    m_stopReason = reason;
    if (state != State::Following && m_provider)
        m_provider->m_scrollToEndAfterPaint = false;
    updateStatus();
    emit stateChanged();
}

void ReadOnlyTailController::reset()
{
    ++m_intent;
    m_checkpoint = {};
    m_eofBytes.clear();
    if (m_provider) m_provider->m_scrollToEndAfterPaint = false;
    setState(State::Off);
}

bool ReadOnlyTailController::internalProjection() const
{
    return !m_provider || m_followingInternally || m_provider->m_applyingSourceSelection
        || m_provider->m_syncingHexSelection || m_provider->m_syncingAsciiSelection;
}

std::optional<quint64> ReadOnlyTailController::projectionIntent() const
{
    // A local receipt for this Tail projection, not an additional generation.
    // Explicit navigation after pausing retains its ordinary positioning rules.
    if (m_followingInternally && m_state == State::Following) return m_intent;
    return std::nullopt;
}

bool ReadOnlyTailController::permitsProjection(const std::optional<quint64> &intent) const
{
    return !intent || (*intent == m_intent && m_state == State::Following);
}

bool ReadOnlyTailController::atEnd() const
{
    if (!m_provider || m_provider->selectionStartOffset() != m_provider->selectionEndOffset())
        return false;
    if (m_provider->fileSize() == 0 || m_provider->visibleBottomOffset() >= m_provider->fileSize())
        return true;
    // Hex's left-edge byte probe names the first byte of the last row, not
    // that row's end. Use the actual projection viewport for this final row.
    auto *editor = m_provider->editor();
    const auto first = editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    const auto rows = editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
    return m_provider->windowEnd() == m_provider->fileSize()
        && first + rows >= m_provider->projectedDisplayLineCount();
}

void ReadOnlyTailController::setTailEnabled(bool enabled)
{
    if (enabled == tailEnabled() || !m_provider) return;
    const QPointer<ReadOnlyTailController> self(this);
    const auto provider = m_provider;
    if (!enabled) {
        const quint64 intent = m_intent + 1;
        reset();
        if (self && provider && m_intent == intent && m_state == State::Off)
            provider->setStatisticsDeferred(false);
        return;
    }
    if (!provider->hasSource()) return;
    ++m_intent;
    const quint64 intent = m_intent;
    const bool follow = atEnd();
    m_checkpoint = {};
    m_eofBytes.clear();
    setState(follow ? State::Following : State::Paused);
    if (!self || !provider || m_intent != intent) return;
    provider->setStatisticsDeferred(true);
    if (!self || !provider || m_intent != intent) return;
    QString error;
    // Refresh admits even an empty file and obtains the checkpoint through the
    // same handle as the accepted version. No path preflight/body reopen here.
    if (!provider->refreshFileMapping(&error) && self && m_intent == intent
        && m_state != State::Stopped && !error.isEmpty())
        setState(State::Stopped, error);
}

void ReadOnlyTailController::resumeAtEnd()
{
    if (!canResume() || !m_provider) return;
    const QPointer<ReadOnlyTailController> self(this);
    const auto provider = m_provider;
    const quint64 intent = m_intent;
    QString error;
    if (!provider->refreshFileMapping(&error) || !self || !provider
        || m_intent != intent || !canResume()) return;
    setState(State::Following);
    if (self && provider && m_intent == intent && m_state == State::Following) followEnd();
}

void ReadOnlyTailController::pauseForReading()
{
    if (m_state != State::Following) return;
    // Pausing revokes the pending EOF paint, not the admitted source evidence.
    // acceptCandidate consults the live state; Off->On/reset changes m_intent.
    setState(State::Paused);
}

bool ReadOnlyTailController::followEnd(QString *error)
{
    if (!m_provider || m_state != State::Following) return false;
    const QPointer<ReadOnlyTailController> self(this);
    const auto provider = m_provider;
    const quint64 intent = m_intent;
    const bool previous = std::exchange(m_followingInternally, true);
    const auto restore = qScopeGuard([self, previous] {
        if (self) self->m_followingInternally = previous;
    });
    if (!provider->validateCurrent(error) || !self || !provider
        || m_intent != intent || m_state != State::Following) return false;
    provider->clearVirtualSelection();
    const bool projected = provider->reloadWindow(
        std::max<qint64>(0, provider->fileSize() - 1), 0, -1, false, error);
    return projected && self && provider && m_intent == intent && m_state == State::Following;
}

QString ReadOnlyTailController::changedReason(const DiskFileStamp &stamp) const
{
    if (!stamp.valid || stamp.fileIdentity.isEmpty()) return tr("无法确认源文件身份，请检查后重新开启。");
    if (!m_checkpoint.valid) return {};
    if (stamp.fileIdentity != m_checkpoint.fileIdentity) return tr("源文件已被替换，请检查后重新开启。");
    if (stamp.size < m_checkpoint.size) return tr("源文件已被截断，请检查后重新开启。");
    if (stamp.size == m_checkpoint.size && stamp.modifiedMs != m_checkpoint.modifiedMs)
        return tr("源文件内容已被重写，请检查后重新开启。");
    return {};
}

void ReadOnlyTailController::observeSource(const DiskFileStamp &stamp, DiskFileState state)
{
    if (!tailEnabled() || m_state == State::Stopped) return;
    QString reason;
    if (state == DiskFileState::Missing) reason = tr("源文件已被删除，请检查后重新开启。");
    else if (state == DiskFileState::Unreadable) reason = tr("源文件暂时无法读取，请检查权限后重新开启。");
    else reason = changedReason(stamp);
    if (!reason.isEmpty()) setState(State::Stopped, reason);
}

void ReadOnlyTailController::sourceFailed(const OrdinaryReadResult &failure)
{
    if (!tailEnabled()) return;
    if (failure.status == OrdinaryReadStatus::SecureType) {
        setState(State::Stopped, tr("源文件已变成安全文件，请从“打开”重新打开。"));
    } else if (failure.status == OrdinaryReadStatus::PolicyUnavailable) {
        setState(State::Stopped, tr("缺少安全读取规则，请重新打开文件。"));
    } else {
        observeSource(failure.stamp, failure.status == OrdinaryReadStatus::Missing
            ? DiskFileState::Missing : failure.status == OrdinaryReadStatus::Unreadable
                ? DiskFileState::Unreadable : DiskFileState::Changed);
    }
}

ReadOnlyTailController::Candidate ReadOnlyTailController::prepareCandidate(
    const DiskFileStamp &stamp,
    const std::function<QByteArray(qint64, qsizetype)> &readAt) const
{
    Candidate candidate;
    candidate.stamp = stamp;
    candidate.intent = m_intent;
    if (!tailEnabled() || m_state == State::Stopped) return candidate;
    candidate.stopReason = changedReason(stamp);
    if (!candidate.stopReason.isEmpty()) return candidate;
    if (m_checkpoint.valid && stamp.size > m_checkpoint.size) {
        const QByteArray previous = readAt(m_checkpoint.size - m_eofBytes.size(), m_eofBytes.size());
        candidate.readable = previous.size() == m_eofBytes.size();
        if (candidate.readable && previous != m_eofBytes)
            candidate.stopReason = tr("源文件原末尾内容已被重写，请检查后重新开启。");
    }
    const qsizetype count = qsizetype(std::min(kEofOverlapBytes, stamp.size));
    candidate.eofBytes = readAt(stamp.size - count, count);
    candidate.readable = candidate.readable && candidate.eofBytes.size() == count;
    return candidate;
}

ReadOnlyTailController::Projection ReadOnlyTailController::acceptCandidate(
    const Candidate &candidate, QString *error)
{
    // A reentrant Off->On starts a different user intent. Do not overwrite its
    // checkpoint or perform an old request's follow operation.
    if (candidate.intent != m_intent) return Projection::Failed;
    const QPointer<ReadOnlyTailController> self(this);
    if (tailEnabled() && m_state != State::Stopped) {
        if (!candidate.stopReason.isEmpty()) setState(State::Stopped, candidate.stopReason);
        else {
            m_checkpoint = candidate.stamp;
            m_eofBytes = candidate.eofBytes;
        }
    }
    if (!self || candidate.intent != m_intent) return Projection::Failed;
    if (m_state == State::Paused) return Projection::Preserved;
    if (m_state != State::Following) return Projection::Refresh;
    return followEnd(error) ? Projection::Preserved : Projection::Failed;
}

bool ReadOnlyTailController::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    // Internal setText/selection changes emit signals, not key/mouse events.
    // Real input delivered reentrantly still revokes an in-flight follow.
    if (m_state != State::Following || !m_provider) return false;
    bool reading = false;
    if (event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        reading = wheel->angleDelta().y() > 0 || wheel->pixelDelta().y() > 0;
    } else if (event->type() == QEvent::MouseButtonPress) {
        reading = static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton;
    } else if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy)) return false;
        reading = key->matches(QKeySequence::SelectAll)
            || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down
            || key->key() == Qt::Key_Left || key->key() == Qt::Key_Right
            || key->key() == Qt::Key_Home || key->key() == Qt::Key_End
            || key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown;
    }
    const QPointer<ReadOnlyTailController> self(this);
    if (reading) pauseForReading();
    return !self;
}
