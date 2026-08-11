#include "app.h"

#include "common.h"

#include <array>
#include <fstream>

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

void App::BeginFullscreenValidation() {
    if (validation_.fullscreen_phase != FullscreenValidationPhase::Pending) return;
    const HWND window = window_.Handle();
    if (!GetWindowRect(window, &validation_.windowed_rect)) {
        ThrowLastError("GetWindowRect validation");
    }
    validation_.windowed_style = GetWindowLongPtrW(window, GWL_STYLE);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
        ThrowLastError("GetMonitorInfoW validation");
    }
    validation_.monitor_rect = info.rcMonitor;
    validation_.fullscreen_phase = FullscreenValidationPhase::Entering;
    PostMessageW(window, WM_KEYDOWN, VK_F11, 0);
    PostMessageW(window, WM_KEYUP, VK_F11, 0);
    SetTimer(window, 3, 100, nullptr);
}

void App::OnFullscreenValidationTimer() {
    const HWND window = window_.Handle();
    RECT rectangle{};
    if (!GetWindowRect(window, &rectangle)) ThrowLastError("GetWindowRect fullscreen");
    if (validation_.fullscreen_phase == FullscreenValidationPhase::Entering) {
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        if (!window_.IsFullscreen() ||
            (style & static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) != 0 ||
            !SameRect(rectangle, validation_.monitor_rect)) {
            exit_code_ = 3;
            KillTimer(window, 3);
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        validation_.fullscreen_phase = FullscreenValidationPhase::Leaving;
        PostMessageW(window, WM_KEYDOWN, VK_F11, 0);
        PostMessageW(window, WM_KEYUP, VK_F11, 0);
        return;
    }
    if (validation_.fullscreen_phase == FullscreenValidationPhase::Leaving) {
        if (window_.IsFullscreen()) {
            exit_code_ = 4;
        } else if ((GetWindowLongPtrW(window, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_VISIBLE)) !=
                   (validation_.windowed_style & ~static_cast<LONG_PTR>(WS_VISIBLE))) {
            exit_code_ = 6;
        } else if (!SameRect(rectangle, validation_.windowed_rect)) {
            exit_code_ = 5;
        }
        validation_.fullscreen_phase = FullscreenValidationPhase::Complete;
        KillTimer(window, 3);
        if (exit_code_ == 0 && !config_.validation_navigation.empty()) {
            config_.validation_fullscreen = false;
            InjectValidationNavigation();
            return;
        }
        KillTimer(window, 1);
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

void App::InjectValidationNavigation() {
    if (validation_.script_injected || config_.validation_navigation.empty()) return;
    validation_.script_injected = true;
    validation_.expected_index = pipeline_.state_.navigation.CurrentIndex();
    validation_.navigation_cursor = 0;
    validation_.navigation_started = std::chrono::steady_clock::now();
    validation_.navigation_injection_finished = {};
    WriteValidationReport("warmup-complete", true);
    validation_.ResetTimings();
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        validation_.main_kernel_started = FileTimeTicks(kernel);
        validation_.main_user_started = FileTimeTicks(user);
    }
    pipeline_.decoders_->ResetMetrics();
    pipeline_.graphics_.ResetMetrics();
    if (config_.validation_navigation_interval_ms == 0) {
        for (const wchar_t step : config_.validation_navigation) {
            const int direction = step == L'L' ? -1 : 1;
            if (direction < 0 && validation_.expected_index > 0) {
                --validation_.expected_index;
            } else if (direction > 0 &&
                       validation_.expected_index + 1 < pipeline_.state_.images.size()) {
                ++validation_.expected_index;
            }
            pipeline_.state_.navigation.Step(direction, false);
            pipeline_.state_.navigation.Release(direction);
            ++validation_.navigation_cursor;
        }
        pipeline_.state_.reservations.MarkDirty();
        validation_.navigation_injection_finished = std::chrono::steady_clock::now();
        pipeline_.PumpPipeline();
        return;
    }
    InjectValidationNavigationStep();
    if (validation_.navigation_cursor < config_.validation_navigation.size()) {
        if (SetTimer(window_.Handle(), 4,
                     config_.validation_navigation_interval_ms,
                     nullptr) == 0) {
            ThrowLastError("SetTimer validation navigation");
        }
        validation_.navigation_timer_active = true;
    }
}

void App::InjectValidationNavigationStep() {
    if (validation_.navigation_cursor >= config_.validation_navigation.size()) {
        StopValidationNavigationTimer();
        return;
    }
    const wchar_t step = config_.validation_navigation[validation_.navigation_cursor];
    const int direction = step == L'L' ? -1 : 1;
    if (direction < 0 && validation_.expected_index > 0) {
        --validation_.expected_index;
    } else if (direction > 0 &&
               validation_.expected_index + 1 < pipeline_.state_.images.size()) {
        ++validation_.expected_index;
    }
    if (config_.validation_short_presses) {
        pipeline_.state_.navigation.Step(direction, false);
        pipeline_.state_.navigation.Release(direction);
    } else {
        pipeline_.state_.navigation.Step(direction, validation_.navigation_cursor != 0);
    }
    pipeline_.state_.reservations.MarkDirty();
    ++validation_.navigation_cursor;
    if (validation_.navigation_cursor >= config_.validation_navigation.size()) {
        validation_.navigation_injection_finished = std::chrono::steady_clock::now();
        StopValidationNavigationTimer();
    }
    pipeline_.PumpPipeline();
    if (validation_.navigation_cursor == 1 || validation_.navigation_cursor == 10 ||
        validation_.navigation_cursor == 30 || validation_.navigation_cursor == 60) {
        const std::string phase = "navigation-step-" +
                                  std::to_string(validation_.navigation_cursor);
        WriteValidationReport(phase, false);
    }
}

void App::StopValidationNavigationTimer() {
    if (!validation_.navigation_timer_active) return;
    KillTimer(window_.Handle(), 4);
    validation_.navigation_timer_active = false;
}

void App::RecordValidationPresentation(const std::size_t index) {
    if (validation_.timeline_started == std::chrono::steady_clock::time_point{} ||
        (!validation_.presented_samples.empty() &&
         validation_.presented_samples.back().image_index == index)) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - validation_.timeline_started);
    validation_.presented_samples.push_back(
        {index, static_cast<std::uint64_t>(elapsed.count())});
}

void App::RecordValidationReady(const std::size_t index) {
    if (validation_.timeline_started == std::chrono::steady_clock::time_point{} ||
        std::ranges::find(validation_.ready_samples, index,
                          &ValidationSample::image_index) !=
            validation_.ready_samples.end()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - validation_.timeline_started);
    validation_.ready_samples.push_back(
        {index, static_cast<std::uint64_t>(elapsed.count())});
}

void App::WriteValidationReport(const std::string_view phase, const bool truncate) {
    if (config_.validation_report.empty()) return;
    std::ofstream output(config_.validation_report,
                         std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (!output) return;
    const auto startup_nanoseconds = [&](const auto time) {
        if (validation_.timeline_started == std::chrono::steady_clock::time_point{} ||
            time == std::chrono::steady_clock::time_point{}) {
            return std::int64_t{0};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   time - validation_.timeline_started).count();
    };
    constexpr std::array names{"Outside", "WaitingIo", "IoInFlight", "CompressedReady",
                               "DecodeQueued", "DecodedStagingAvailable", "Uploading",
                               "PresentationTextureAvailable",
                               "CancelPending", "Failed"};
    std::array<std::size_t, names.size()> counts{};
    for (const ImageRecord& image : pipeline_.state_.images) {
        const std::size_t stage = static_cast<std::size_t>(pipeline_.StageOf(image));
        if (stage < counts.size()) ++counts[stage];
    }
    output << "phase=" << phase << '\n'
           << "startup_window_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::WindowReady)) << '\n'
           << "startup_initial_io_submitted_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::InitialIoSubmitted)) << '\n'
           << "startup_decoders_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::DecodersReady)) << '\n'
           << "startup_graphics_device_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::GraphicsDeviceReady)) << '\n'
           << "startup_graphics_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::GraphicsReady)) << '\n'
           << "startup_catalog_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::CatalogReady)) << '\n'
           << "startup_initial_header_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::InitialHeaderReady)) << '\n'
           << "startup_initial_content_completion_observed_nanoseconds="
           << startup_nanoseconds(
                   validation_.At(StartupMilestone::InitialContentCompletionObserved)) << '\n'
           << "startup_initial_content_ready_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::InitialContentReady)) << '\n'
           << "startup_initial_decode_submitted_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::InitialDecodeSubmitted)) << '\n'
           << "startup_initial_decode_completed_nanoseconds="
           << startup_nanoseconds(validation_.At(StartupMilestone::InitialDecodeCompleted)) << '\n'
           << "iocp_enabled=" << (pipeline_.io_completion_port_ ? 1 : 0) << '\n'
           << "io_prefix_granularity="
           << pipeline_.io_prefix_granularity_ << '\n';
    for (std::size_t index = 0; index < names.size(); ++index) {
        output << names[index] << '=' << counts[index] << '\n';
    }
    const auto write_indices = [&](const std::string_view name,
                                   const PipelineStage stage) {
        output << name << '=';
        bool first = true;
        for (std::size_t index = 0; index < pipeline_.state_.images.size(); ++index) {
            if (pipeline_.StageOf(pipeline_.state_.images[index]) != stage) continue;
            if (!first) output << ',';
            output << index;
            first = false;
        }
        output << '\n';
    };
    write_indices("DecodedStagingAvailable_indices",
                  PipelineStage::DecodedStagingAvailable);
    write_indices("Uploading_indices", PipelineStage::Uploading);
    write_indices("PresentationTextureAvailable_indices",
                  PipelineStage::PresentationTextureAvailable);
    output << "ActiveReadableGpuTexture_indices=";
    bool first_readable_gpu_texture = true;
    for (std::size_t index = 0; index < pipeline_.state_.images.size(); ++index) {
        if (!pipeline_.HasReadableGpuTexture(index)) continue;
        if (!first_readable_gpu_texture) output << ',';
        output << index;
        first_readable_gpu_texture = false;
    }
    output << '\n';
    const auto retiring_count = [](const ReservationTable& table) {
        std::size_t count = 0;
        for (ReservationId id = 0; id < table.Capacity(); ++id) {
            if (table.At(id).retiring) ++count;
        }
        return count;
    };
    output << "compressed_bytes=" << pipeline_.state_.compressed_bytes << '\n'
           << "compressed_committed_bytes="
           << pipeline_.state_.slots.CompressedCommittedBytes() << '\n'
           << "staging_committed_bytes=" << pipeline_.state_.slots.StagingCommittedBytes() << '\n'
           << "gpu_bytes=" << pipeline_.state_.gpu_bytes << '\n'
           << "free_compressed_slots=" << pipeline_.state_.slots.FreeCompressedCount() << '\n'
           << "free_staging_slots=" << pipeline_.state_.slots.FreeStagingCount() << '\n'
           << "free_gpu_texture_slots=" << pipeline_.state_.slots.FreeGpuTextureCount() << '\n'
           << "compressed_reservations="
           << pipeline_.state_.reservations.Compressed().AssignedCount() << '/'
           << pipeline_.state_.reservations.Compressed().Capacity() << '\n'
           << "staging_reservations="
           << pipeline_.state_.reservations.Staging().AssignedCount() << '/'
           << pipeline_.state_.reservations.Staging().Capacity() << '\n'
           << "gpu_texture_reservations="
           << pipeline_.state_.reservations.GpuTextures().AssignedCount() << '/'
           << pipeline_.state_.reservations.GpuTextures().Capacity() << '\n'
           << "compressed_retiring_reservations="
           << retiring_count(pipeline_.state_.reservations.Compressed()) << '\n'
           << "staging_retiring_reservations="
           << retiring_count(pipeline_.state_.reservations.Staging()) << '\n'
           << "gpu_texture_retiring_reservations="
           << retiring_count(pipeline_.state_.reservations.GpuTextures()) << '\n'
           << "retiring_reservations="
           << retiring_count(pipeline_.state_.reservations.Compressed()) +
                  retiring_count(pipeline_.state_.reservations.Staging()) +
                  retiring_count(pipeline_.state_.reservations.GpuTextures())
           << '\n'
           << "work_queue=" << pipeline_.work_queue_.Size() << '\n'
           << "uploads=" << pipeline_.state_.uploads.size() << '\n'
           << "held_direction=" << pipeline_.state_.navigation.HeldDirection() << '\n'
           << "current_index=" << pipeline_.state_.navigation.CurrentIndex() << '\n'
           << "title_matches_current=";
    std::array<wchar_t, 512> window_title{};
    GetWindowTextW(window_.Handle(), window_title.data(),
                   static_cast<int>(window_title.size()));
    const std::filesystem::path current_filename =
        pipeline_.state_.catalog.items[
            pipeline_.state_.navigation.CurrentIndex()].path.filename();
    output << (current_filename.native() == window_title.data() ? 1 : 0) << '\n'
           << "next_index=";
    if (const auto next = pipeline_.state_.navigation.NextIndex()) {
        output << *next;
    } else {
        output << "none";
    }
    output << '\n'
           << "validation_cursor=" << validation_.navigation_cursor << '\n'
           << "validation_ready_count="
           << validation_.ready_samples.size() << '\n'
           << "validation_ready_indices=";
    for (std::size_t index = 0; index < validation_.ready_samples.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_.ready_samples[index].image_index;
    }
    output << '\n' << "validation_ready_nanoseconds=";
    for (std::size_t index = 0; index < validation_.ready_samples.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_.ready_samples[index].nanoseconds;
    }
    output << '\n'
           << "validation_presented_count="
           << validation_.presented_samples.size() << '\n'
           << "validation_presented_indices=";
    for (std::size_t index = 0; index < validation_.presented_samples.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_.presented_samples[index].image_index;
    }
    output << '\n' << "validation_presented_nanoseconds=";
    for (std::size_t index = 0; index < validation_.presented_samples.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_.presented_samples[index].nanoseconds;
    }
    output << '\n'
           << "navigation_injection_nanoseconds=";
    if (validation_.navigation_started != std::chrono::steady_clock::time_point{} &&
        validation_.navigation_injection_finished !=
            std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      validation_.navigation_injection_finished -
                      validation_.navigation_started)
                      .count();
    } else {
        output << 0;
    }
    const auto report_time = std::chrono::steady_clock::now();
    FILETIME main_created{};
    FILETIME main_exited{};
    FILETIME main_kernel{};
    FILETIME main_user{};
    const bool have_main_times = GetThreadTimes(
        GetCurrentThread(), &main_created, &main_exited, &main_kernel, &main_user);
    const auto timing = [&](const TimedOperation operation)
        -> const ValidationTiming& { return validation_.Timing(operation); };
    output << '\n' << "navigation_completion_nanoseconds=";
    if (validation_.navigation_started != std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      report_time - validation_.navigation_started)
                      .count();
    } else {
        output << 0;
    }
    output << '\n' << "navigation_pipeline_tail_nanoseconds=";
    if (validation_.navigation_injection_finished !=
        std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      report_time - validation_.navigation_injection_finished)
                      .count();
    } else {
        output << 0;
    }
    output << '\n'
           << "decode_count=" << pipeline_.decoders_->DecodeCount() << '\n'
           << "decode_nanoseconds=" << pipeline_.decoders_->DecodeNanoseconds() << '\n'
           << "pipeline_pump_count="
           << timing(TimedOperation::PipelinePump).calls << '\n'
           << "pipeline_pump_nanoseconds="
           << timing(TimedOperation::PipelinePump).nanoseconds << '\n'
           << "reservation_plan_count="
           << timing(TimedOperation::ReservationPlan).calls << '\n'
           << "reservation_plan_nanoseconds="
           << timing(TimedOperation::ReservationPlan).nanoseconds << '\n'
           << "reservation_reconcile_count="
           << timing(TimedOperation::ReservationReconcile).calls << '\n'
           << "reservation_reconcile_nanoseconds="
           << timing(TimedOperation::ReservationReconcile).nanoseconds << '\n'
           << "dispatch_decode_nanoseconds="
           << timing(TimedOperation::DispatchDecode).nanoseconds << '\n'
           << "submit_reads_nanoseconds="
           << timing(TimedOperation::SubmitReads).nanoseconds << '\n'
           << "acquire_compressed_nanoseconds="
           << timing(TimedOperation::AcquireCompressed).nanoseconds << '\n'
           << "open_file_nanoseconds="
           << timing(TimedOperation::OpenFile).nanoseconds << '\n'
           << "submit_file_reads_nanoseconds="
           << timing(TimedOperation::SubmitFileReads).nanoseconds << '\n'
           << "submit_uploads_nanoseconds="
           << timing(TimedOperation::SubmitUploads).nanoseconds << '\n'
           << "try_present_nanoseconds="
           << timing(TimedOperation::TryPresent).nanoseconds << '\n'
           << "main_thread_kernel_nanoseconds="
           << (have_main_times
                    ? (FileTimeTicks(main_kernel) - validation_.main_kernel_started) * 100ULL
                    : 0ULL)
           << '\n'
           << "main_thread_user_nanoseconds="
           << (have_main_times
                    ? (FileTimeTicks(main_user) - validation_.main_user_started) * 100ULL
                    : 0ULL)
           << '\n'
           << "upload_count=" << pipeline_.graphics_.UploadCount() << '\n'
           << "upload_nanoseconds=" << pipeline_.graphics_.UploadNanoseconds() << '\n'
           << "draw_count=" << pipeline_.graphics_.DrawCount() << '\n'
           << "draw_nanoseconds=" << pipeline_.graphics_.DrawNanoseconds() << '\n';
}

}  // namespace pv
