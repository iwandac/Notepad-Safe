#include "StartupTrace.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cstdio>
#endif

namespace {

qint64 readCounter() noexcept
{
#ifdef Q_OS_WIN
    LARGE_INTEGER counter{};
    if (::QueryPerformanceCounter(&counter))
        return counter.QuadPart;
#endif
    return 0;
}

const char *stageName(StartupTrace::Stage stage) noexcept
{
    switch (stage) {
    case StartupTrace::Stage::AppReady: return "application_ready";
    case StartupTrace::Stage::CompositionReady: return "composition_ready";
    case StartupTrace::Stage::WindowReady: return "window_ready";
    case StartupTrace::Stage::ShowBegin: return "show_begin";
    case StartupTrace::Stage::ShowReturned: return "show_returned";
    case StartupTrace::Stage::FirstPaint: return "first_paint";
    case StartupTrace::Stage::PostPaintIdleTurn: return "post_paint_idle";
    case StartupTrace::Stage::RestoreCompleted: return "restore_completed";
    case StartupTrace::Stage::EventLoopFinished: return "event_loop_finished";
    }
    return "invalid_stage";
}

} // namespace

StartupTrace::StartupTrace() noexcept
    : enabled_(qgetenv("NOTEPADSAFE_STARTUP_TRACE") == QByteArrayLiteral("1"))
{
    if (!enabled_)
        return;
    pid_ = QCoreApplication::applicationPid();
#ifdef Q_OS_WIN
    LARGE_INTEGER frequency{};
    if (::QueryPerformanceFrequency(&frequency))
        frequency_ = frequency.QuadPart;
#endif
    invalidClock_ = frequency_ <= 0;
}

void StartupTrace::record(Stage stage) noexcept
{
    if (!enabled_)
        return;
    const auto index = static_cast<unsigned>(stage);
    if (index >= StageCount) {
        invalidStage_ = true;
        return;
    }
    const unsigned bit = 1U << index;
    duplicateStage_ = duplicateStage_ || (seen_ & bit) != 0;
    seen_ |= bit;
    if (size_ == Capacity) {
        overflow_ = true;
        return;
    }
    const qint64 ticks = readCounter();
    invalidClock_ = invalidClock_ || ticks <= 0
        || (size_ > 0 && ticks < events_[size_ - 1].ticks);
    events_[size_++] = {stage, ticks};
}

void StartupTrace::recordPaintCounts(PaintCounts counts) noexcept
{
    if (!enabled_)
        return;
    if (paintObserved_) {
        duplicateStage_ = true;
        return;
    }
    paintCounts_ = counts;
    paintObserved_ = true;
}

QByteArray StartupTrace::toJson(int exitCode) const
{
    if (!enabled_)
        return {};
    QJsonArray events;
    for (std::size_t index = 0; index < size_; ++index) {
        const Event &event = events_[index];
        events.append(QJsonObject{
            {QStringLiteral("stage"), QString::fromLatin1(stageName(event.stage))},
            {QStringLiteral("ticks"), QString::number(event.ticks)}});
    }
    const bool complete = seen_ == (1U << StageCount) - 1 && paintObserved_;
    const QJsonValue counts = paintObserved_
        ? QJsonValue(QJsonObject{
            {QStringLiteral("heavy"), paintCounts_.heavy},
            {QStringLiteral("tools"), paintCounts_.tools},
            {QStringLiteral("documents"), paintCounts_.documents},
            {QStringLiteral("persistence"), paintCounts_.persistence},
            {QStringLiteral("inference"), paintCounts_.inference}})
        : QJsonValue(QJsonValue::Null);
    const QJsonObject object{
        {QStringLiteral("schema"), QStringLiteral("startup-trace-v1")},
        {QStringLiteral("pid"), pid_},
        {QStringLiteral("qpc_frequency"), QString::number(frequency_)},
        {QStringLiteral("exit_code"), exitCode},
        {QStringLiteral("invalid_clock"), invalidClock_},
        {QStringLiteral("overflow"), overflow_},
        {QStringLiteral("duplicate_stage"), duplicateStage_},
        {QStringLiteral("invalid_stage"), invalidStage_},
        {QStringLiteral("complete"), complete},
        {QStringLiteral("valid"), complete && !invalidClock_ && !overflow_
            && !duplicateStage_ && !invalidStage_},
        {QStringLiteral("paint_counts"), counts},
        {QStringLiteral("events"), events}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool StartupTrace::exportToStdout(int exitCode) const noexcept
{
    if (!enabled_)
        return true;
    try {
        const QByteArray output = toJson(exitCode) + '\n';
#ifdef Q_OS_WIN
        const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (!handle || handle == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        return ::WriteFile(handle, output.constData(), static_cast<DWORD>(output.size()),
                           &written, nullptr) && written == static_cast<DWORD>(output.size());
#else
        return std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout)
                   == static_cast<std::size_t>(output.size())
               && std::fflush(stdout) == 0;
#endif
    } catch (...) {
        return false;
    }
}
