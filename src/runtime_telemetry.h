#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace pv {

enum class StartupMilestone : std::size_t {
    WindowReady,
    InitialIoSubmitted,
    DecodersReady,
    GraphicsDeviceReady,
    GraphicsReady,
    CatalogReady,
    InitialHeaderReady,
    InitialContentCompletionObserved,
    InitialContentReady,
    InitialDecodeSubmitted,
    InitialDecodeCompleted,
    Count,
};

enum class TimedOperation : std::size_t {
    PipelinePump,
    ReservationPlan,
    ReservationReconcile,
    DispatchDecode,
    SubmitReads,
    AcquireCompressed,
    OpenFile,
    SubmitFileReads,
    SubmitUploads,
    TryPresent,
    Count,
};

struct OperationTiming {
    std::uint64_t calls = 0;
    std::uint64_t nanoseconds = 0;
};

// Optional instrumentation sink owned by the caller. PipelineRuntime uses a
// disabled local instance when no telemetry is supplied.
class RuntimeTelemetry {
public:
    RuntimeTelemetry() = default;
    explicit RuntimeTelemetry(
        const std::chrono::steady_clock::time_point process_started)
        : timeline_started_(process_started) {}

    void Mark(const StartupMilestone milestone) noexcept {
        startup_[static_cast<std::size_t>(milestone)] =
            std::chrono::steady_clock::now();
    }

    void MarkOnce(const StartupMilestone milestone) noexcept {
        auto& value = startup_[static_cast<std::size_t>(milestone)];
        if (value == std::chrono::steady_clock::time_point{}) {
            value = std::chrono::steady_clock::now();
        }
    }

    [[nodiscard]] std::chrono::steady_clock::time_point At(
        const StartupMilestone milestone) const noexcept {
        return startup_[static_cast<std::size_t>(milestone)];
    }

    [[nodiscard]] std::chrono::steady_clock::time_point TimelineStarted()
        const noexcept {
        return timeline_started_;
    }

    void BeginNavigation(
        const std::chrono::steady_clock::time_point started) noexcept {
        navigation_started_ = started;
        timings_.fill({});
    }

    [[nodiscard]] bool NavigationActive() const noexcept {
        return navigation_started_ != std::chrono::steady_clock::time_point{};
    }

    [[nodiscard]] const OperationTiming& Timing(
        const TimedOperation operation) const noexcept {
        return timings_[static_cast<std::size_t>(operation)];
    }

    void Record(const TimedOperation operation,
                const std::chrono::steady_clock::time_point begin) noexcept {
        if (!NavigationActive() ||
            begin == std::chrono::steady_clock::time_point{}) {
            return;
        }
        auto& timing = timings_[static_cast<std::size_t>(operation)];
        timing.nanoseconds += ElapsedNanoseconds(begin);
        ++timing.calls;
    }

    template <typename Operation>
    decltype(auto) Measure(const TimedOperation operation,
                           Operation&& callable) {
        if (!NavigationActive()) {
            return std::forward<Operation>(callable)();
        }
        const auto begin = std::chrono::steady_clock::now();
        auto& timing = timings_[static_cast<std::size_t>(operation)];
        if constexpr (std::is_void_v<std::invoke_result_t<Operation>>) {
            std::forward<Operation>(callable)();
            timing.nanoseconds += ElapsedNanoseconds(begin);
            ++timing.calls;
        } else {
            decltype(auto) result = std::forward<Operation>(callable)();
            timing.nanoseconds += ElapsedNanoseconds(begin);
            ++timing.calls;
            return result;
        }
    }

private:
    [[nodiscard]] static std::uint64_t ElapsedNanoseconds(
        const std::chrono::steady_clock::time_point begin) noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin)
                .count());
    }

    std::chrono::steady_clock::time_point timeline_started_{};
    std::array<std::chrono::steady_clock::time_point,
               static_cast<std::size_t>(StartupMilestone::Count)>
        startup_{};
    std::chrono::steady_clock::time_point navigation_started_{};
    std::array<OperationTiming,
               static_cast<std::size_t>(TimedOperation::Count)>
        timings_{};
};

}  // namespace pv
