#pragma once

#include "win32_support.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pv {

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

struct ValidationSession {
    ValidationSession(const bool timeline_enabled,
                      const std::size_t sample_capacity) {
        if (timeline_enabled) {
            ready_samples.reserve(sample_capacity);
            presented_samples.reserve(sample_capacity);
        }
    }

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
    std::chrono::steady_clock::time_point navigation_started{};
    std::chrono::steady_clock::time_point navigation_injection_finished{};
    std::vector<ValidationSample> ready_samples;
    std::vector<ValidationSample> presented_samples;
};

}  // namespace pv
