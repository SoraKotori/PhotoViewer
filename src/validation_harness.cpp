#include "validation_harness.h"

#include "win32_support.h"
#include "pipeline_runtime.h"
#include "viewer_window.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace pv {
namespace {

bool SameRect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

std::uint64_t FileTimeTicks(const FILETIME time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

}  // namespace

void ValidationHarness::MarkWindowReady() noexcept {
    telemetry_.Mark(StartupMilestone::WindowReady);
}

void ValidationHarness::MarkCatalogReady() noexcept {
    telemetry_.Mark(StartupMilestone::CatalogReady);
}

bool ValidationHarness::NavigationTimerActive() const noexcept {
    return session_.navigation_timer_active;
}

void ValidationHarness::BeginFullscreenValidation(ViewerWindow& window_owner) {
    if (session_.fullscreen_phase != FullscreenValidationPhase::Pending) return;
    const HWND window = window_owner.Handle();
    if (!GetWindowRect(window, &session_.windowed_rect)) {
        ThrowLastError("GetWindowRect validation");
    }
    session_.windowed_style = GetWindowLongPtrW(window, GWL_STYLE);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         &info)) {
        ThrowLastError("GetMonitorInfoW validation");
    }
    session_.monitor_rect = info.rcMonitor;
    session_.fullscreen_phase = FullscreenValidationPhase::Entering;
    PostMessageW(window, WM_KEYDOWN, VK_F11, 0);
    PostMessageW(window, WM_KEYUP, VK_F11, 0);
    SetTimer(window, 3, 100, nullptr);
}

std::optional<int> ValidationHarness::OnFullscreenValidationTimer(
    ViewerWindow& window_owner, PipelineRuntime& pipeline) {
    const HWND window = window_owner.Handle();
    RECT rectangle{};
    if (!GetWindowRect(window, &rectangle)) {
        ThrowLastError("GetWindowRect fullscreen");
    }
    if (session_.fullscreen_phase == FullscreenValidationPhase::Entering) {
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        if (!window_owner.IsFullscreen() ||
            (style & static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) != 0 ||
            !SameRect(rectangle, session_.monitor_rect)) {
            KillTimer(window, 3);
            return 3;
        }
        session_.fullscreen_phase = FullscreenValidationPhase::Leaving;
        PostMessageW(window, WM_KEYDOWN, VK_F11, 0);
        PostMessageW(window, WM_KEYUP, VK_F11, 0);
        return std::nullopt;
    }
    if (session_.fullscreen_phase != FullscreenValidationPhase::Leaving) {
        return std::nullopt;
    }

    int exit_code = 0;
    if (window_owner.IsFullscreen()) {
        exit_code = 4;
    } else if ((GetWindowLongPtrW(window, GWL_STYLE) &
                ~static_cast<LONG_PTR>(WS_VISIBLE)) !=
               (session_.windowed_style & ~static_cast<LONG_PTR>(WS_VISIBLE))) {
        exit_code = 6;
    } else if (!SameRect(rectangle, session_.windowed_rect)) {
        exit_code = 5;
    }
    session_.fullscreen_phase = FullscreenValidationPhase::Complete;
    KillTimer(window, 3);
    if (exit_code == 0 && !config_.validation_navigation.empty()) {
        InjectValidationNavigation(pipeline, window_owner);
        return std::nullopt;
    }
    KillTimer(window, 1);
    return exit_code;
}

void ValidationHarness::InjectValidationNavigation(
    PipelineRuntime& pipeline, ViewerWindow& window) {
    if (session_.script_injected || config_.validation_navigation.empty()) return;
    session_.script_injected = true;
    session_.expected_index = pipeline.CurrentIndex();
    session_.navigation_cursor = 0;
    session_.navigation_started = std::chrono::steady_clock::now();
    session_.navigation_injection_finished = {};
    WriteReport(pipeline, "warmup-complete", true);
    telemetry_.BeginNavigation(session_.navigation_started);

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        session_.main_kernel_started = FileTimeTicks(kernel);
        session_.main_user_started = FileTimeTicks(user);
    }
    pipeline.ResetPerformanceCounters();

    if (config_.validation_navigation_interval_ms == 0) {
        for (const wchar_t step : config_.validation_navigation) {
            const int direction = step == L'L' ? -1 : 1;
            if (direction < 0 && session_.expected_index > 0) {
                --session_.expected_index;
            } else if (direction > 0 &&
                       session_.expected_index + 1 < pipeline.FrameCount()) {
                ++session_.expected_index;
            }
            ++session_.navigation_cursor;
        }
        pipeline.ApplyNavigationSequence(config_.validation_navigation);
        session_.navigation_injection_finished = std::chrono::steady_clock::now();
        return;
    }

    InjectValidationNavigationStep(pipeline, window);
    if (session_.navigation_cursor < config_.validation_navigation.size()) {
        if (SetTimer(window.Handle(), 4,
                     config_.validation_navigation_interval_ms, nullptr) == 0) {
            ThrowLastError("SetTimer validation navigation");
        }
        session_.navigation_timer_active = true;
    }
}

void ValidationHarness::InjectValidationNavigationStep(
    PipelineRuntime& pipeline, ViewerWindow& window) {
    if (session_.navigation_cursor >= config_.validation_navigation.size()) {
        StopValidationNavigationTimer(window);
        return;
    }
    const wchar_t step =
        config_.validation_navigation[session_.navigation_cursor];
    const int direction = step == L'L' ? -1 : 1;
    if (direction < 0 && session_.expected_index > 0) {
        --session_.expected_index;
    } else if (direction > 0 &&
               session_.expected_index + 1 < pipeline.FrameCount()) {
        ++session_.expected_index;
    }
    if (config_.validation_short_presses) {
        pipeline.Navigate(direction, false);
        pipeline.ReleaseNavigation(direction);
    } else {
        pipeline.Navigate(direction, session_.navigation_cursor != 0);
    }
    ++session_.navigation_cursor;
    if (session_.navigation_cursor >= config_.validation_navigation.size()) {
        session_.navigation_injection_finished =
            std::chrono::steady_clock::now();
        StopValidationNavigationTimer(window);
    }
    if (session_.navigation_cursor == 1 ||
        session_.navigation_cursor == 10 ||
        session_.navigation_cursor == 30 ||
        session_.navigation_cursor == 60) {
        WriteReport(pipeline,
                    "navigation-step-" +
                        std::to_string(session_.navigation_cursor),
                    false);
    }
}

void ValidationHarness::StopValidationNavigationTimer(ViewerWindow& window) {
    if (!session_.navigation_timer_active) return;
    KillTimer(window.Handle(), 4);
    session_.navigation_timer_active = false;
}

void ValidationHarness::RecordPresentation(const std::size_t index) {
    if (telemetry_.TimelineStarted() ==
            std::chrono::steady_clock::time_point{} ||
        (!session_.presented_samples.empty() &&
         session_.presented_samples.back().image_index == index)) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - telemetry_.TimelineStarted());
    session_.presented_samples.push_back(
        {index, static_cast<std::uint64_t>(elapsed.count())});
}

void ValidationHarness::OnFrameReady(const std::size_t index) {
    if (telemetry_.TimelineStarted() ==
            std::chrono::steady_clock::time_point{} ||
        std::ranges::find(session_.ready_samples, index,
                          &ValidationSample::image_index) !=
            session_.ready_samples.end()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - telemetry_.TimelineStarted());
    session_.ready_samples.push_back(
        {index, static_cast<std::uint64_t>(elapsed.count())});
}

std::optional<int> ValidationHarness::OnFramePresented(
    const std::size_t index, PipelineRuntime& pipeline, ViewerWindow& window) {
    RecordPresentation(index);
    if (!config_.validation_exit_after_present) return std::nullopt;

    if (config_.validation_fullscreen &&
        session_.fullscreen_phase == FullscreenValidationPhase::Pending) {
        BeginFullscreenValidation(window);
        return std::nullopt;
    }
    if (config_.validation_fullscreen &&
        session_.fullscreen_phase != FullscreenValidationPhase::Complete) {
        return std::nullopt;
    }

    if (!config_.validation_navigation.empty() &&
        !session_.script_injected) {
        if (config_.validation_warmup_ms != 0) {
            if (!session_.script_scheduled) {
                session_.script_scheduled = true;
                SetTimer(window.Handle(), 2, config_.validation_warmup_ms,
                         nullptr);
            }
        } else {
            InjectValidationNavigation(pipeline, window);
        }
        return std::nullopt;
    }

    const bool navigation_complete = config_.validation_navigation.empty() ||
        (session_.script_injected &&
         session_.navigation_cursor >= config_.validation_navigation.size() &&
         pipeline.NavigationIdle());
    if (!navigation_complete) return std::nullopt;

    int exit_code = 0;
    if (!config_.validation_navigation.empty() &&
        pipeline.CurrentIndex() != session_.expected_index) {
        exit_code = 2;
    } else if (config_.validation_elapsed_exit_code &&
               !config_.validation_navigation.empty()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - session_.navigation_started);
        exit_code = static_cast<int>(std::clamp<std::int64_t>(
            elapsed.count(), 1, std::numeric_limits<int>::max()));
    }
    WriteReport(pipeline, "navigation-complete", false);
    KillTimer(window.Handle(), 1);
    return exit_code;
}

int ValidationHarness::OnTimeout(PipelineRuntime& pipeline) {
    const auto stage = pipeline.PendingFrameStage();
    const int exit_code = stage ? 100 + static_cast<int>(*stage) : 199;
    WriteReport(pipeline, "timeout", false);
    return exit_code;
}

void ValidationHarness::WriteReport(PipelineRuntime& pipeline,
                                    const std::string_view phase,
                                    const bool truncate) {
    if (config_.validation_report.empty()) return;
    std::ofstream output(config_.validation_report,
                         std::ios::out |
                             (truncate ? std::ios::trunc : std::ios::app));
    if (!output) return;

    const auto startup_nanoseconds = [&](const auto time) {
        if (telemetry_.TimelineStarted() ==
                std::chrono::steady_clock::time_point{} ||
            time == std::chrono::steady_clock::time_point{}) {
            return std::int64_t{0};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   time - telemetry_.TimelineStarted())
            .count();
    };
    output << "phase=" << phase << '\n'
           << "startup_window_ready_nanoseconds="
           << startup_nanoseconds(telemetry_.At(StartupMilestone::WindowReady))
           << '\n'
           << "startup_initial_io_submitted_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::InitialIoSubmitted))
           << '\n'
           << "startup_decoders_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::DecodersReady))
           << '\n'
           << "startup_graphics_device_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::GraphicsDeviceReady))
           << '\n'
           << "startup_graphics_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::GraphicsReady))
           << '\n'
           << "startup_catalog_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::CatalogReady))
           << '\n'
           << "startup_initial_header_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::InitialHeaderReady))
           << '\n'
           << "startup_initial_content_completion_observed_nanoseconds="
           << startup_nanoseconds(telemetry_.At(
                  StartupMilestone::InitialContentCompletionObserved))
           << '\n'
           << "startup_initial_content_ready_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::InitialContentReady))
           << '\n'
           << "startup_initial_decode_submitted_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::InitialDecodeSubmitted))
           << '\n'
           << "startup_initial_decode_completed_nanoseconds="
           << startup_nanoseconds(
                  telemetry_.At(StartupMilestone::InitialDecodeCompleted))
           << '\n';
    pipeline.WriteDiagnostics(output);

    const auto write_samples = [&](const std::string_view prefix,
                                   const auto& samples) {
        output << prefix << "_count=" << samples.size() << '\n'
               << prefix << "_indices=";
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if (index != 0) output << ',';
            output << samples[index].image_index;
        }
        output << '\n' << prefix << "_nanoseconds=";
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if (index != 0) output << ',';
            output << samples[index].nanoseconds;
        }
        output << '\n';
    };
    output << "validation_cursor=" << session_.navigation_cursor << '\n';
    write_samples("validation_ready", session_.ready_samples);
    write_samples("validation_presented", session_.presented_samples);

    const auto report_time = std::chrono::steady_clock::now();
    const auto elapsed_since = [&](const auto start) {
        return start == std::chrono::steady_clock::time_point{}
                   ? std::int64_t{0}
                   : std::chrono::duration_cast<std::chrono::nanoseconds>(
                         report_time - start)
                         .count();
    };
    output << "navigation_injection_nanoseconds=";
    if (session_.navigation_started != std::chrono::steady_clock::time_point{} &&
        session_.navigation_injection_finished !=
            std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      session_.navigation_injection_finished -
                      session_.navigation_started)
                      .count();
    } else {
        output << 0;
    }
    output << '\n'
           << "navigation_completion_nanoseconds="
           << elapsed_since(session_.navigation_started) << '\n'
           << "navigation_pipeline_tail_nanoseconds="
           << elapsed_since(session_.navigation_injection_finished) << '\n';

    constexpr std::array timing_names{
        "pipeline_pump", "reservation_plan", "reservation_reconcile",
        "dispatch_decode", "submit_reads", "acquire_compressed",
        "open_file", "submit_file_reads", "submit_uploads", "try_present"};
    for (std::size_t index = 0; index < timing_names.size(); ++index) {
        const OperationTiming& timing =
            telemetry_.Timing(static_cast<TimedOperation>(index));
        output << timing_names[index] << "_count=" << timing.calls << '\n'
               << timing_names[index] << "_nanoseconds="
               << timing.nanoseconds << '\n';
    }

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    const bool have_main_times = GetThreadTimes(
        GetCurrentThread(), &created, &exited, &kernel, &user);
    output << "main_thread_kernel_nanoseconds="
           << (have_main_times
                   ? (FileTimeTicks(kernel) - session_.main_kernel_started) *
                         100ULL
                   : 0ULL)
           << '\n'
           << "main_thread_user_nanoseconds="
           << (have_main_times
                   ? (FileTimeTicks(user) - session_.main_user_started) * 100ULL
                   : 0ULL)
           << '\n';
}

}  // namespace pv
