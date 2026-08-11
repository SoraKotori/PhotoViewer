#pragma once

#include "common.h"

#include <array>
#include <type_traits>

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

struct ValidationTiming {
    std::uint64_t calls = 0;
    std::uint64_t nanoseconds = 0;
};

struct ValidationSample {
    std::size_t image_index = 0;
    std::uint64_t nanoseconds = 0;
};

enum class FullscreenValidationPhase : std::uint8_t {
    Pending,
    Entering,
    Leaving,
    Complete,
};

struct ValidationState {
    ValidationState(std::chrono::steady_clock::time_point process_started,
                    bool timeline_enabled, std::size_t sample_capacity)
        : timeline_started(timeline_enabled ? process_started
                                            : std::chrono::steady_clock::time_point{}) {
        if (timeline_enabled) {
            ready_samples.reserve(sample_capacity);
            presented_samples.reserve(sample_capacity);
        }
    }

    void Mark(const StartupMilestone milestone) noexcept {
        startup[static_cast<std::size_t>(milestone)] =
            std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point At(
        const StartupMilestone milestone) const noexcept {
        return startup[static_cast<std::size_t>(milestone)];
    }

    [[nodiscard]] bool NavigationActive() const noexcept {
        return navigation_started != std::chrono::steady_clock::time_point{};
    }

    void ResetTimings() noexcept { timings.fill({}); }

    [[nodiscard]] const ValidationTiming& Timing(
        const TimedOperation operation) const noexcept {
        return timings[static_cast<std::size_t>(operation)];
    }

    void Record(const TimedOperation operation,
                const std::chrono::steady_clock::time_point begin) noexcept {
        if (!NavigationActive()) return;
        auto& timing = timings[static_cast<std::size_t>(operation)];
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
        auto& timing = timings[static_cast<std::size_t>(operation)];
        if constexpr (std::is_void_v<std::invoke_result_t<Operation>>) {
            std::forward<Operation>(callable)();
            timing.nanoseconds += ElapsedNanoseconds(begin);
        } else {
            decltype(auto) result = std::forward<Operation>(callable)();
            timing.nanoseconds += ElapsedNanoseconds(begin);
            return result;
        }
    }

    [[nodiscard]] static std::uint64_t ElapsedNanoseconds(
        const std::chrono::steady_clock::time_point begin) noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }

    std::chrono::steady_clock::time_point timeline_started{};
    std::array<std::chrono::steady_clock::time_point,
               static_cast<std::size_t>(StartupMilestone::Count)> startup{};
    std::chrono::steady_clock::time_point navigation_started{};
    std::chrono::steady_clock::time_point navigation_injection_finished{};
    std::array<ValidationTiming,
               static_cast<std::size_t>(TimedOperation::Count)> timings{};

    FullscreenValidationPhase fullscreen_phase =
        FullscreenValidationPhase::Pending;
    RECT windowed_rect{};
    RECT monitor_rect{};
    LONG_PTR windowed_style = 0;

    bool script_injected = false;
    bool script_scheduled = false;
    bool navigation_timer_active = false;
    std::size_t navigation_cursor = 0;
    std::size_t expected_index = 0;
    std::uint64_t main_kernel_started = 0;
    std::uint64_t main_user_started = 0;
    std::vector<ValidationSample> ready_samples;
    std::vector<ValidationSample> presented_samples;
};

}  // namespace pv
