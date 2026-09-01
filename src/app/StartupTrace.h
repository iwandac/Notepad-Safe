#pragma once

#include <QByteArray>

#include <array>
#include <cstddef>

// Optional main-thread-only observation. Recording owns no QObject, performs no
// I/O or formatting, and never changes application scheduling or exit semantics.
class StartupTrace final {
public:
    enum class Stage {
        AppReady, CompositionReady, WindowReady, ShowBegin, ShowReturned,
        FirstPaint, PostPaintIdleTurn, RestoreCompleted, EventLoopFinished
    };
    struct PaintCounts {
        int heavy = 0;
        int tools = 0;
        int documents = 0;
        int persistence = 0;
        int inference = 0;
    };
    static constexpr std::size_t Capacity = 16;

    StartupTrace() noexcept;
    bool enabled() const noexcept { return enabled_; }
    void record(Stage stage) noexcept;
    void recordPaintCounts(PaintCounts counts) noexcept;
    // Serialization is explicit and deferred; disabled traces return no data.
    QByteArray toJson(int exitCode) const;
    // Call only after primary event-loop exit and destruction of its callback
    // contexts. Failure leaves business exit unchanged; the harness rejects it.
    bool exportToStdout(int exitCode) const noexcept;

private:
    struct Event { Stage stage = Stage::AppReady; qint64 ticks = 0; };
    static constexpr unsigned StageCount = 9;
    std::array<Event, Capacity> events_{};
    std::size_t size_ = 0;
    unsigned seen_ = 0;
    qint64 frequency_ = 0;
    qint64 pid_ = 0;
    PaintCounts paintCounts_{};
    bool enabled_ = false;
    bool invalidClock_ = false;
    bool overflow_ = false;
    bool duplicateStage_ = false;
    bool invalidStage_ = false;
    bool paintObserved_ = false;
};
