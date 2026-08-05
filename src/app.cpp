#include "app.h"

#include "common.h"

#include <array>
#include <cmath>
#include <fstream>
#include <numeric>

namespace pv {
namespace {

constexpr wchar_t kWindowClass[] = L"PhotoViewer.Window";

void SetWindowStyleChecked(const HWND window, const LONG_PTR style) {
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(window, GWL_STYLE, style) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        ThrowLastError("SetWindowLongPtrW(GWL_STYLE)");
    }
}

bool SameRect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

bool NavigationInputPending(const HWND window) noexcept {
    MSG message{};
    return PeekMessageW(&message, window, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) ||
           PeekMessageW(&message, window, kMessageValidationStep,
                        kMessageValidationStep, PM_NOREMOVE);
}

}  // namespace

App::App(Config config)
    : config_(std::move(config)) {
    resources_.slots = std::make_unique<ResourceSlots>(
        config_.compressed_slot_count, config_.staging_slot_count,
        config_.gpu_texture_slot_count, config_.compressed_budget_bytes,
        config_.staging_cache_bytes);
}

App::~App() {
    StopValidationNavigationTimer();
    decoders_.reset();
    if (graphics_ready_ && resources_.slots) {
        for (SlotId id = 0; id < resources_.slots->StagingCount(); ++id) {
            DecodeStaging& staging = resources_.slots->StagingAt(id).resource;
            if (staging.mapped) graphics_.UnmapDecodeStaging(staging);
        }
    }
    CancelAllIo();
}

int App::Run(const HINSTANCE instance, const int show_command) {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        ThrowLastError("Set main thread priority");
    }
    InitializeWindow(instance, show_command);
    decoders_ = std::make_unique<DecoderPool>(config_.worker_count, work_queue_,
                                              completion_queue_, *resources_.slots,
                                              window_);
    if (!config_.initial_image.empty()) OpenInitialImage();
    graphics_.Initialize(window_);
    graphics_ready_ = true;
    ShowWindow(window_, config_.validation_exit_after_present ? SW_HIDE : show_command);
    if (!config_.validation_exit_after_present) UpdateWindow(window_);
    if (config_.validation_exit_after_present) {
        SetTimer(window_, 1, config_.validation_timeout_ms, nullptr);
    }
    return EventLoop();
}

void App::InitializeWindow(const HINSTANCE instance, const int show_command) {
    (void)show_command;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &App::WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ThrowLastError("RegisterClassExW");
    }
    window_ = CreateWindowExW(0, kWindowClass, L"PhotoViewer", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              nullptr, nullptr, instance, this);
    if (!window_) ThrowLastError("CreateWindowExW");
}

LRESULT CALLBACK App::WindowProcedure(const HWND window, const UINT message,
                                      const WPARAM wparam, const LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        return app->HandleWindowMessage(message, wparam, lparam);
    } catch (const std::exception& error) {
        MessageBoxA(window, error.what(), "PhotoViewer fatal error", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 0;
    }
}

LRESULT App::HandleWindowMessage(const UINT message, const WPARAM wparam,
                                 const LPARAM lparam) {
    switch (message) {
        case WM_KEYDOWN: {
            const bool repeat = (lparam & (1LL << 30)) != 0;
            const std::size_t repeat_count = std::max<std::size_t>(
                1, LOWORD(static_cast<DWORD_PTR>(lparam)));
            if (wparam == VK_LEFT) OnDirection(-1, repeat, repeat_count);
            else if (wparam == VK_RIGHT) OnDirection(1, repeat, repeat_count);
            else if (wparam == VK_F11 && !repeat) ToggleFullscreen();
            return 0;
        }
        case WM_KEYUP:
            if (wparam == VK_LEFT) OnDirectionReleased(-1);
            else if (wparam == VK_RIGHT) OnDirectionReleased(1);
            return 0;
        case kMessageIoComplete:
            OnIoComplete(reinterpret_cast<IoRequest*>(wparam));
            return 0;
        case kMessageWorkerComplete:
            OnWorkerComplete();
            return 0;
        case kMessageValidationStep:
            InjectValidationNavigationStep();
            return 0;
        case WM_SIZE:
            if (graphics_ready_ && wparam != SIZE_MINIMIZED) {
                OnSurfaceChanged(LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_TIMER:
            if (wparam == 1 && config_.validation_exit_after_present) {
                const auto next = resources_.navigation.NextIndex();
                exit_code_ = next && *next < resources_.images.size()
                                 ? 100 + static_cast<int>(StageOf(resources_.images[*next]))
                                 : 199;
                WriteValidationReport("timeout", false);
                KillTimer(window_, 1);
                DestroyWindow(window_);
            } else if (wparam == 2 && config_.validation_exit_after_present) {
                KillTimer(window_, 2);
                InjectValidationNavigation();
            } else if (wparam == 3 && config_.validation_fullscreen) {
                OnFullscreenValidationTimer();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window_, 1);
            KillTimer(window_, 2);
            KillTimer(window_, 3);
            StopValidationNavigationTimer();
            running_ = false;
            PostQuitMessage(exit_code_);
            return 0;
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
    }
}

int App::EventLoop() {
    int exit_code = 0;
    while (running_) {
        HANDLE handles[2]{};
        enum class Kind { Frame, Fence } kinds[2]{};
        DWORD count = 0;
        if (!resources_.frame_credit && graphics_.FrameWaitableObject()) {
            handles[count] = graphics_.FrameWaitableObject();
            kinds[count++] = Kind::Frame;
        }
        if ((!resources_.uploads.empty() || resources_.reading_source_fence != 0) &&
            graphics_.FenceEvent()) {
            handles[count] = graphics_.FenceEvent();
            kinds[count++] = Kind::Fence;
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(
            count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) ThrowLastError("MsgWaitForMultipleObjectsEx");
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            const DWORD index = result - WAIT_OBJECT_0;
            if (kinds[index] == Kind::Frame) OnFrameCredit();
            else OnGpuComplete();
        }

        MSG message{};
        constexpr int message_batch_limit = 8;
        for (int processed = 0;
             processed < message_batch_limit &&
             PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
             ++processed) {
            if (message.message == WM_QUIT) {
                running_ = false;
                exit_code = static_cast<int>(message.wParam);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return exit_code;
}

void App::OpenInitialImage() {
    resources_.catalog = config_.validation_file_list.empty()
                             ? BuildCatalog(config_.initial_image)
                             : BuildCatalogFromList(config_.validation_file_list,
                                                    config_.initial_image);
    resources_.generation++;
    resources_.images.clear();
    resources_.images.resize(resources_.catalog.items.size());
    for (std::size_t index = 0; index < resources_.images.size(); ++index) {
        auto& image = resources_.images[index];
        image.generation = resources_.generation;
        image.failed = resources_.catalog.items[index].file_bytes == 0;
    }
    resources_.navigation.Reset(resources_.catalog.initial_index,
                                resources_.catalog.items.size());
    InitializeReservations();
    resources_.redraw_pending = true;
    PumpPipeline();
}

void App::OnDirection(const int direction, const bool repeat,
                      const std::size_t repeat_count) {
    if (resources_.images.empty()) return;
    if (!repeat && repeat_count > 1) {
        resources_.navigation.Step(direction, false);
        resources_.navigation.Step(direction, true, repeat_count - 1);
    } else {
        resources_.navigation.Step(direction, repeat, repeat_count);
    }
    PumpPipeline();
}

void App::OnDirectionReleased(const int direction) {
    if (resources_.images.empty()) return;
    resources_.navigation.Release(direction);
    PumpPipeline();
}

void CALLBACK App::IoCompletion(PTP_CALLBACK_INSTANCE, void* context, void*,
                                const ULONG io_result, const ULONG_PTR transferred,
                                PTP_IO) {
    auto* request = static_cast<IoRequest*>(context);
    request->transferred.store(transferred, std::memory_order_relaxed);
    request->result.store(io_result, std::memory_order_release);
    PostMessageW(request->window, kMessageIoComplete,
                 reinterpret_cast<WPARAM>(request), 0);
}

void CALLBACK App::ValidationTimerCallback(PTP_CALLBACK_INSTANCE, void* context,
                                           PTP_TIMER) {
    auto* app = static_cast<App*>(context);
    PostMessageW(app->window_, kMessageValidationStep, 0, 0);
}

void App::OnIoComplete(IoRequest* request) {
    for (;;) {
        CompleteIoRequest(request);
        MSG pending{};
        if (!PeekMessageW(&pending, nullptr, 0, 0, PM_NOREMOVE) ||
            pending.hwnd != window_ || pending.message != kMessageIoComplete) {
            break;
        }
        PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE);
        request = reinterpret_cast<IoRequest*>(pending.wParam);
    }
    PumpPipeline();
}

void App::CompleteIoRequest(IoRequest* const request) {
    if (!request || request->index >= resources_.images.size()) return;
    ImageRecord& image = resources_.images[request->index];
    if (!image.io || image.io.get() != request) return;

    WaitForThreadpoolIoCallbacks(request->threadpool_io, FALSE);
    CloseThreadpoolIo(request->threadpool_io);
    request->threadpool_io = nullptr;
    CloseHandle(request->file);
    request->file = INVALID_HANDLE_VALUE;

    const DWORD io_result = request->result.load(std::memory_order_acquire);
    const std::size_t transferred = request->transferred.load(std::memory_order_relaxed);
    const SlotId compressed_slot = request->compressed_slot;
    CompressedSlot& slot = resources_.slots->Compressed(compressed_slot);
    const std::size_t allocation = slot.resource.size;
    const bool success = io_result == ERROR_SUCCESS && transferred == allocation;
    const bool current = request->generation == image.generation;
    auto completed = std::move(image.io);

    const bool reserved = current && ReservationActive(
        resources_.compressed_reservations, image.compressed_reservation,
        request->index);
    if (success && reserved) {
        CatalogItem& item = resources_.catalog.items[request->index];
        const auto header = ParsePngHeader(std::span<const std::byte>(
            slot.resource.data, slot.resource.size));
        if (header) {
            item.png = *header;
            item.header_valid = true;
            slot.state = CompressedSlotState::CompressedDataAvailable;
        } else {
            if (resources_.compressed_bytes >= allocation) {
                resources_.compressed_bytes -= allocation;
            }
            resources_.slots->ReleaseCompressed(compressed_slot);
            image.compressed_slot = kInvalidSlot;
            image.failed = true;
        }
    } else {
        if (resources_.compressed_bytes >= allocation) resources_.compressed_bytes -= allocation;
        resources_.slots->ReleaseCompressed(compressed_slot);
        image.compressed_slot = kInvalidSlot;
        if (reserved && io_result != ERROR_OPERATION_ABORTED) {
            image.failed = true;
        }
    }
}

void App::OnWorkerComplete() {
    for (ReleasedInput& input : completion_queue_.DrainReleasedInputs()) {
        if (input.compressed_slot == kInvalidSlot) continue;
        CompressedSlot& slot = resources_.slots->Compressed(input.compressed_slot);
        if (resources_.compressed_bytes >= slot.resource.size) {
            resources_.compressed_bytes -= slot.resource.size;
        }
        if (input.index < resources_.images.size()) {
            ImageRecord& image = resources_.images[input.index];
            if (image.generation == input.generation &&
                image.compressed_slot == input.compressed_slot) {
                image.compressed_slot = kInvalidSlot;
            }
        }
        resources_.slots->ReleaseCompressed(input.compressed_slot);
    }
    for (DecodeResult& result : completion_queue_.Drain()) {
        if (result.staging_slot == kInvalidSlot) continue;
        StagingSlot& slot = resources_.slots->StagingAt(result.staging_slot);
        graphics_.UnmapDecodeStaging(slot.resource);
        if (result.index >= resources_.images.size()) {
            resources_.slots->ReleaseStaging(result.staging_slot);
            continue;
        }
        ImageRecord& image = resources_.images[result.index];
        if (result.generation != image.generation) {
            resources_.slots->ReleaseStaging(result.staging_slot);
            continue;
        }
        image.work_token.reset();
        const bool reserved = ReservationActive(
            resources_.staging_reservations, image.staging_reservation,
            result.index);
        if (result.success && reserved) {
            slot.state = StagingSlotState::DecodedPixelsAvailable;
        } else {
            resources_.slots->ReleaseStaging(result.staging_slot);
            image.staging_slot = kInvalidSlot;
            if (!result.cancelled && FAILED(result.error) && reserved) {
                image.failed = true;
            }
        }
    }
    if (completion_queue_.AcknowledgeNotification()) {
        PostMessageW(window_, kMessageWorkerComplete, 0, 0);
    }
    PumpPipeline();
}

void App::OnGpuComplete() {
    const UINT64 completed = graphics_.CompletedFenceValue();
    if (resources_.reading_source_fence != 0 &&
        resources_.reading_source_fence <= completed) {
        if (resources_.reading_source_slot != kInvalidSlot) {
            GpuTextureSlot& source = resources_.slots->GpuTextureAt(
                resources_.reading_source_slot);
            if (source.state == GpuTextureSlotState::Reading) {
                source.state = source.reserved_frame == source.content_frame
                                   ? GpuTextureSlotState::Readable
                                   : GpuTextureSlotState::Writable;
            }
        }
        resources_.reading_source_slot = kInvalidSlot;
        resources_.reading_source_fence = 0;
    }
    while (!resources_.uploads.empty() &&
           resources_.uploads.front().fence_value <= completed) {
        UploadTicket ticket = std::move(resources_.uploads.front());
        resources_.uploads.pop_front();
        if (ticket.index >= resources_.images.size()) {
            resources_.slots->ReleaseStaging(ticket.staging_slot);
            continue;
        }
        ImageRecord& image = resources_.images[ticket.index];
        GpuTextureSlot& source = resources_.slots->GpuTextureAt(
            ticket.source_texture_slot);
        const bool keep_gpu = ticket.generation == image.generation &&
            ReservationActive(resources_.source_reservations,
                              image.source_reservation, ticket.index) &&
            image.source_reservation == ticket.source_texture_slot &&
            source.reserved_frame == ticket.index;
        if (keep_gpu) {
            graphics_.FinishUpload(source.resource);
            source.content_frame = ticket.index;
            source.state = GpuTextureSlotState::Readable;
        } else {
            source.content_frame = ticket.index;
            source.state = GpuTextureSlotState::Writable;
        }
        resources_.slots->ReleaseStaging(ticket.staging_slot);
        if (image.staging_slot == ticket.staging_slot) {
            image.staging_slot = kInvalidSlot;
        }
    }
    resources_.armed_fence = 0;
    ArmOldestFence();
    PumpPipeline();
}

void App::OnFrameCredit() {
    resources_.frame_credit = true;
    PumpPipeline();
}

void App::OnSurfaceChanged(const UINT width, const UINT height) {
    if (width == 0 || height == 0) return;
    graphics_.Resize(width, height);
    resources_.redraw_pending = true;
    resources_.frame_credit = true;
    PumpPipeline();
}

void App::OnPaint() {
    PAINTSTRUCT paint{};
    BeginPaint(window_, &paint);
    EndPaint(window_, &paint);
    resources_.redraw_pending = true;
    if (graphics_ready_) PumpPipeline();
}

void App::ToggleFullscreen() {
    if (!fullscreen_) {
        windowed_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
        windowed_placement_.length = sizeof(windowed_placement_);
        if (!GetWindowPlacement(window_, &windowed_placement_)) {
            ThrowLastError("GetWindowPlacement");
        }
        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) ThrowLastError("GetMonitorInfoW");
        SetWindowStyleChecked(
            window_, windowed_style_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        fullscreen_ = true;
        if (!SetWindowPos(window_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                          info.rcMonitor.right - info.rcMonitor.left,
                          info.rcMonitor.bottom - info.rcMonitor.top,
                          SWP_FRAMECHANGED | SWP_NOOWNERZORDER)) {
            ThrowLastError("SetWindowPos fullscreen");
        }
    } else {
        fullscreen_ = false;
        SetWindowStyleChecked(window_, windowed_style_);
        if (!SetWindowPlacement(window_, &windowed_placement_)) {
            ThrowLastError("SetWindowPlacement");
        }
        if (!SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                              SWP_NOOWNERZORDER | SWP_FRAMECHANGED)) {
            ThrowLastError("SetWindowPos restore");
        }
    }
}

void App::BeginFullscreenValidation() {
    if (validation_fullscreen_phase_ != 0) return;
    if (!GetWindowRect(window_, &validation_windowed_rect_)) {
        ThrowLastError("GetWindowRect validation");
    }
    validation_windowed_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &info)) {
        ThrowLastError("GetMonitorInfoW validation");
    }
    validation_monitor_rect_ = info.rcMonitor;
    validation_fullscreen_phase_ = 1;
    PostMessageW(window_, WM_KEYDOWN, VK_F11, 0);
    PostMessageW(window_, WM_KEYUP, VK_F11, 0);
    SetTimer(window_, 3, 100, nullptr);
}

void App::OnFullscreenValidationTimer() {
    RECT rectangle{};
    if (!GetWindowRect(window_, &rectangle)) ThrowLastError("GetWindowRect fullscreen");
    if (validation_fullscreen_phase_ == 1) {
        const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
        if (!fullscreen_ ||
            (style & static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) != 0 ||
            !SameRect(rectangle, validation_monitor_rect_)) {
            exit_code_ = 3;
            KillTimer(window_, 3);
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        validation_fullscreen_phase_ = 2;
        PostMessageW(window_, WM_KEYDOWN, VK_F11, 0);
        PostMessageW(window_, WM_KEYUP, VK_F11, 0);
        return;
    }
    if (validation_fullscreen_phase_ == 2) {
        if (fullscreen_) {
            exit_code_ = 4;
        } else if ((GetWindowLongPtrW(window_, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_VISIBLE)) !=
                   (validation_windowed_style_ & ~static_cast<LONG_PTR>(WS_VISIBLE))) {
            exit_code_ = 6;
        } else if (!SameRect(rectangle, validation_windowed_rect_)) {
            exit_code_ = 5;
        }
        validation_fullscreen_phase_ = 3;
        KillTimer(window_, 3);
        if (exit_code_ == 0 && !config_.validation_navigation.empty()) {
            config_.validation_fullscreen = false;
            InjectValidationNavigation();
            return;
        }
        KillTimer(window_, 1);
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
}

void App::InjectValidationNavigation() {
    if (validation_script_injected_ || config_.validation_navigation.empty()) return;
    validation_script_injected_ = true;
    validation_expected_index_ = resources_.navigation.CurrentIndex();
    validation_navigation_cursor_ = 0;
    validation_navigation_started_ = std::chrono::steady_clock::now();
    validation_navigation_injection_finished_ = {};
    WriteValidationReport("warmup-complete", true);
    decoders_->ResetMetrics();
    graphics_.ResetMetrics();
    if (config_.validation_navigation_interval_ms == 0) {
        for (const wchar_t step : config_.validation_navigation) {
            const int direction = step == L'L' ? -1 : 1;
            if (direction < 0 && validation_expected_index_ > 0) {
                --validation_expected_index_;
            } else if (direction > 0 &&
                       validation_expected_index_ + 1 < resources_.images.size()) {
                ++validation_expected_index_;
            }
            resources_.navigation.Step(direction, false);
            resources_.navigation.Release(direction);
            ++validation_navigation_cursor_;
        }
        validation_navigation_injection_finished_ = std::chrono::steady_clock::now();
        PumpPipeline();
        return;
    }
    InjectValidationNavigationStep();
    if (validation_navigation_cursor_ < config_.validation_navigation.size()) {
        validation_navigation_timer_ = CreateThreadpoolTimer(
            &App::ValidationTimerCallback, this, nullptr);
        if (!validation_navigation_timer_) {
            ThrowLastError("CreateThreadpoolTimer validation navigation");
        }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(
            config_.validation_navigation_interval_ms) * 10'000LL;
        SetThreadpoolTimer(validation_navigation_timer_,
                           reinterpret_cast<FILETIME*>(&due),
                           config_.validation_navigation_interval_ms, 0);
    }
}

void App::InjectValidationNavigationStep() {
    if (validation_navigation_cursor_ >= config_.validation_navigation.size()) {
        StopValidationNavigationTimer();
        return;
    }
    const wchar_t step = config_.validation_navigation[validation_navigation_cursor_];
    const int direction = step == L'L' ? -1 : 1;
    if (direction < 0 && validation_expected_index_ > 0) {
        --validation_expected_index_;
    } else if (direction > 0 &&
               validation_expected_index_ + 1 < resources_.images.size()) {
        ++validation_expected_index_;
    }
    if (config_.validation_short_presses) {
        resources_.navigation.Step(direction, false);
        resources_.navigation.Release(direction);
    } else {
        resources_.navigation.Step(direction, validation_navigation_cursor_ != 0);
    }
    ++validation_navigation_cursor_;
    if (validation_navigation_cursor_ >= config_.validation_navigation.size()) {
        validation_navigation_injection_finished_ = std::chrono::steady_clock::now();
        StopValidationNavigationTimer();
    }
    PumpPipeline();
    if (validation_navigation_cursor_ == 1 || validation_navigation_cursor_ == 10 ||
        validation_navigation_cursor_ == 30 || validation_navigation_cursor_ == 60) {
        const std::string phase = "navigation-step-" +
                                  std::to_string(validation_navigation_cursor_);
        WriteValidationReport(phase, false);
    }
}

void App::StopValidationNavigationTimer() {
    if (!validation_navigation_timer_) return;
    SetThreadpoolTimer(validation_navigation_timer_, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(validation_navigation_timer_, TRUE);
    CloseThreadpoolTimer(validation_navigation_timer_);
    validation_navigation_timer_ = nullptr;
}

void App::WriteValidationReport(const std::string_view phase, const bool truncate) {
    if (config_.validation_report.empty()) return;
    std::ofstream output(config_.validation_report,
                         std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (!output) return;
    constexpr std::array names{"Outside", "WaitingIo", "IoInFlight", "CompressedReady",
                               "DecodeQueued", "DecodedStagingAvailable", "Uploading",
                               "PresentationTextureAvailable",
                               "CancelPending", "Failed"};
    std::array<std::size_t, names.size()> counts{};
    for (const ImageRecord& image : resources_.images) {
        const std::size_t stage = static_cast<std::size_t>(StageOf(image));
        if (stage < counts.size()) ++counts[stage];
    }
    output << "phase=" << phase << '\n';
    for (std::size_t index = 0; index < names.size(); ++index) {
        output << names[index] << '=' << counts[index] << '\n';
    }
    const auto write_indices = [&](const std::string_view name,
                                   const PipelineStage stage) {
        output << name << '=';
        bool first = true;
        for (std::size_t index = 0; index < resources_.images.size(); ++index) {
            if (StageOf(resources_.images[index]) != stage) continue;
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
    const auto retiring_count = [](const ReservationTable& table) {
        std::size_t count = 0;
        for (ReservationId id = 0; id < table.Capacity(); ++id) {
            if (table.At(id).retiring) ++count;
        }
        return count;
    };
    output << "compressed_bytes=" << resources_.compressed_bytes << '\n'
           << "compressed_committed_bytes="
           << resources_.slots->CompressedCommittedBytes() << '\n'
           << "staging_committed_bytes=" << resources_.slots->StagingCommittedBytes() << '\n'
           << "gpu_bytes=" << resources_.gpu_bytes << '\n'
           << "free_compressed_slots=" << resources_.slots->FreeCompressedCount() << '\n'
           << "free_staging_slots=" << resources_.slots->FreeStagingCount() << '\n'
           << "free_gpu_texture_slots=" << resources_.slots->FreeGpuTextureCount() << '\n'
           << "compressed_reservations="
           << resources_.compressed_reservations.AssignedCount() << '/'
           << resources_.compressed_reservations.Capacity() << '\n'
           << "staging_reservations="
           << resources_.staging_reservations.AssignedCount() << '/'
           << resources_.staging_reservations.Capacity() << '\n'
           << "source_reservations="
           << resources_.source_reservations.AssignedCount() << '/'
           << resources_.source_reservations.Capacity() << '\n'
           << "retiring_reservations="
           << retiring_count(resources_.compressed_reservations) +
                  retiring_count(resources_.staging_reservations) +
                  retiring_count(resources_.source_reservations)
           << '\n'
           << "work_queue=" << work_queue_.Size() << '\n'
           << "uploads=" << resources_.uploads.size() << '\n'
           << "held_direction=" << resources_.navigation.HeldDirection() << '\n'
           << "current_index=" << resources_.navigation.CurrentIndex() << '\n'
           << "next_index=";
    if (const auto next = resources_.navigation.NextIndex()) {
        output << *next;
    } else {
        output << "none";
    }
    output << '\n'
           << "validation_cursor=" << validation_navigation_cursor_ << '\n'
           << "navigation_injection_nanoseconds=";
    if (validation_navigation_started_ != std::chrono::steady_clock::time_point{} &&
        validation_navigation_injection_finished_ !=
            std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      validation_navigation_injection_finished_ -
                      validation_navigation_started_)
                      .count();
    } else {
        output << 0;
    }
    const auto report_time = std::chrono::steady_clock::now();
    output << '\n' << "navigation_completion_nanoseconds=";
    if (validation_navigation_started_ != std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      report_time - validation_navigation_started_)
                      .count();
    } else {
        output << 0;
    }
    output << '\n' << "navigation_pipeline_tail_nanoseconds=";
    if (validation_navigation_injection_finished_ !=
        std::chrono::steady_clock::time_point{}) {
        output << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      report_time - validation_navigation_injection_finished_)
                      .count();
    } else {
        output << 0;
    }
    output << '\n'
           << "decode_count=" << decoders_->DecodeCount() << '\n'
           << "decode_nanoseconds=" << decoders_->DecodeNanoseconds() << '\n'
           << "selected_cpu_set_count=" << decoders_->SelectedCpuSetCount() << '\n'
           << "unthrottled_worker_count=" << decoders_->UnthrottledWorkerCount() << '\n'
           << "elevated_worker_count=" << decoders_->ElevatedWorkerCount() << '\n'
           << "upload_count=" << graphics_.UploadCount() << '\n'
           << "upload_nanoseconds=" << graphics_.UploadNanoseconds() << '\n'
           << "draw_count=" << graphics_.DrawCount() << '\n'
           << "draw_nanoseconds=" << graphics_.DrawNanoseconds() << '\n';
}

void App::PumpPipeline() {
    if (resources_.images.empty()) return;
    for (int pass = 0; pass < 3; ++pass) {
        ReconcileReservations();
        SubmitReads();
        DispatchDecodes();
        SubmitUploads();
        if (!TryPresent()) break;
    }
}

PipelineStage App::StageOf(const ImageRecord& image) const noexcept {
    const std::size_t frame = static_cast<std::size_t>(
        &image - resources_.images.data());
    if (image.failed) return PipelineStage::Failed;
    if (image.source_reservation != kInvalidReservation &&
        image.source_reservation < resources_.slots->GpuTextureCount()) {
        const GpuTextureSlot& source = resources_.slots->GpuTextureAt(
            image.source_reservation);
        if (source.reserved_frame == frame) {
            switch (source.state) {
            case GpuTextureSlotState::Writing:
                return PipelineStage::Uploading;
            case GpuTextureSlotState::Readable:
            case GpuTextureSlotState::Reading:
                return PipelineStage::PresentationTextureAvailable;
            case GpuTextureSlotState::Writable:
            case GpuTextureSlotState::Inactive:
                break;
            }
        }
    }
    if (image.staging_slot != kInvalidSlot) {
        switch (resources_.slots->StagingAt(image.staging_slot).state) {
            case StagingSlotState::DecodeOutputMapped:
                return PipelineStage::DecodeQueued;
            case StagingSlotState::DecodedPixelsAvailable:
                return PipelineStage::DecodedStagingAvailable;
            case StagingSlotState::GpuCopySource:
                return PipelineStage::Uploading;
            case StagingSlotState::CancellationPending:
                return PipelineStage::CancelPending;
            case StagingSlotState::Free:
                break;
        }
    }
    if (image.compressed_slot != kInvalidSlot) {
        switch (resources_.slots->Compressed(image.compressed_slot).state) {
            case CompressedSlotState::FileReadDestination:
                return PipelineStage::IoInFlight;
            case CompressedSlotState::CompressedDataAvailable:
                return PipelineStage::CompressedReady;
            case CompressedSlotState::DecodeInput:
                return PipelineStage::DecodeQueued;
            case CompressedSlotState::CancellationPending:
                return PipelineStage::CancelPending;
            case CompressedSlotState::Free:
                break;
        }
    }
    return ReservationActive(resources_.compressed_reservations,
                             image.compressed_reservation, frame)
               ? PipelineStage::WaitingIo
               : PipelineStage::Outside;
}

void App::InitializeReservations() {
    const std::size_t frame_count = resources_.images.size();
    if (frame_count == 0) return;

    std::uintmax_t largest_file = 1;
    for (const CatalogItem& item : resources_.catalog.items) {
        if (item.file_bytes != 0 && item.file_bytes <= config_.compressed_budget_bytes) {
            largest_file = std::max(largest_file, item.file_bytes);
        }
    }
    constexpr std::size_t decoded_8k_bytes = 7680ULL * 4320ULL * 4ULL;
    constexpr std::size_t staging_8k_bytes =
        decoded_8k_bytes + 4320ULL + 7680ULL * 4ULL;
    const auto fixed_capacity = [frame_count](const std::size_t slots,
                                               const std::size_t budget,
                                               const std::size_t unit) {
        return std::min({frame_count, slots, std::max<std::size_t>(1, budget / unit)});
    };
    const std::size_t compressed_capacity = fixed_capacity(
        config_.compressed_slot_count, config_.compressed_budget_bytes,
        static_cast<std::size_t>(largest_file));
    const std::size_t staging_capacity = fixed_capacity(
        config_.staging_slot_count, config_.staging_cache_bytes,
        staging_8k_bytes);
    const std::size_t source_capacity = fixed_capacity(
        config_.gpu_texture_slot_count, config_.gpu_cache_bytes,
        decoded_8k_bytes);

    resources_.compressed_reservations.Reset(compressed_capacity);
    resources_.staging_reservations.Reset(staging_capacity);
    resources_.source_reservations.Reset(source_capacity);
    for (SlotId id = 0; id < source_capacity; ++id) {
        if (!resources_.slots->ActivateGpuTexture(id)) {
            throw std::logic_error("failed to activate SourceTexture slot");
        }
    }
}

bool App::ReservationActive(const ReservationTable& table,
                            const ReservationId id,
                            const std::size_t frame) const noexcept {
    return table.IsActive(id) && table.At(id).frame == frame;
}

bool App::HasReadableSource(const std::size_t frame) const noexcept {
    if (frame >= resources_.images.size()) return false;
    const ImageRecord& image = resources_.images[frame];
    if (!ReservationActive(resources_.source_reservations,
                           image.source_reservation, frame)) {
        return false;
    }
    const GpuTextureSlot& source = resources_.slots->GpuTextureAt(
        image.source_reservation);
    return source.reserved_frame == frame && source.content_frame == frame &&
           (source.state == GpuTextureSlotState::Readable ||
            source.state == GpuTextureSlotState::Reading);
}

bool App::CancelQueuedDecode(const std::size_t frame) {
    if (frame >= resources_.images.size()) return false;
    ImageRecord& image = resources_.images[frame];
    DecodeWork cancelled;
    if (!work_queue_.TryCancel(image.work_token, cancelled)) return false;

    if (cancelled.staging_slot != kInvalidSlot) {
        StagingSlot& staging = resources_.slots->StagingAt(cancelled.staging_slot);
        graphics_.UnmapDecodeStaging(staging.resource);
        resources_.slots->ReleaseStaging(cancelled.staging_slot);
        if (image.staging_slot == cancelled.staging_slot) {
            image.staging_slot = kInvalidSlot;
        }
    }
    if (cancelled.compressed_slot != kInvalidSlot) {
        CompressedSlot& compressed = resources_.slots->Compressed(
            cancelled.compressed_slot);
        if (ReservationActive(resources_.compressed_reservations,
                              image.compressed_reservation, frame)) {
            compressed.state = CompressedSlotState::CompressedDataAvailable;
        } else {
            ReleaseCompressed(image);
        }
    }
    image.work_token.reset();
    return true;
}

void App::ReconcileReservations() {
    resources_.priority_order = resources_.navigation.PlannedOrder(
        resources_.images.size());

    const auto append_unique = [](std::vector<std::size_t>& frames,
                                  const std::size_t frame,
                                  const std::size_t capacity) {
        if (frames.size() < capacity &&
            std::find(frames.begin(), frames.end(), frame) == frames.end()) {
            frames.push_back(frame);
        }
    };

    std::vector<std::size_t> source_desired;
    const std::size_t source_capacity = resources_.source_reservations.Capacity();
    source_desired.reserve(source_capacity);
    const std::size_t forward_capacity = source_capacity > 2
                                             ? source_capacity - 2
                                             : source_capacity;
    for (const std::size_t frame : resources_.priority_order) {
        if (source_desired.size() >= forward_capacity) break;
        if (!resources_.images[frame].failed) {
            append_unique(source_desired, frame, source_capacity);
        }
    }
    const std::size_t current = resources_.navigation.CurrentIndex();
    append_unique(source_desired, current, source_capacity);
    int direction = resources_.navigation.PreferredDirection();
    if (direction == 0) direction = 1;
    if ((direction > 0 && current > 0) ||
        (direction < 0 && current + 1 < resources_.images.size())) {
        append_unique(source_desired,
                      direction > 0 ? current - 1 : current + 1,
                      source_capacity);
    }
    for (const std::size_t frame : resources_.priority_order) {
        if (!resources_.images[frame].failed) {
            append_unique(source_desired, frame, source_capacity);
        }
    }

    resources_.source_reservations.Reconcile(
        source_desired,
        [&](const ReservationId id, const std::size_t) {
            const GpuTextureSlotState state = resources_.slots->GpuTextureAt(id).state;
            return state == GpuTextureSlotState::Writable ||
                   state == GpuTextureSlotState::Readable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.source_reservation == id) {
                image.source_reservation = kInvalidReservation;
            }
            GpuTextureSlot& source = resources_.slots->GpuTextureAt(id);
            source.reserved_frame = kInvalidFrame;
            source.state = GpuTextureSlotState::Writable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            image.source_reservation = id;
            GpuTextureSlot& source = resources_.slots->GpuTextureAt(id);
            source.reserved_frame = frame;
            source.generation = image.generation;
            source.state = source.content_frame == frame && source.resource.bitmap
                               ? GpuTextureSlotState::Readable
                               : GpuTextureSlotState::Writable;
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            for (ReservationId id = 0; id < entries.size(); ++id) {
                if (entries[id].frame == kInvalidFrame &&
                    resources_.slots->GpuTextureAt(id).content_frame == frame) {
                    return id;
                }
            }
            return ReservationTable::FirstFree(frame, entries);
        });

    std::vector<std::size_t> staging_desired;
    staging_desired.reserve(resources_.staging_reservations.Capacity());
    for (const std::size_t frame : resources_.priority_order) {
        if (staging_desired.size() == resources_.staging_reservations.Capacity()) break;
        const ImageRecord& image = resources_.images[frame];
        if (image.failed) continue;
        bool source_complete = false;
        if (image.source_reservation != kInvalidReservation) {
            const GpuTextureSlot& source = resources_.slots->GpuTextureAt(
                image.source_reservation);
            source_complete = source.reserved_frame == frame &&
                              source.state != GpuTextureSlotState::Writable;
        }
        if (!source_complete) append_unique(
            staging_desired, frame, resources_.staging_reservations.Capacity());
    }
    resources_.staging_reservations.Reconcile(
        staging_desired,
        [&](const ReservationId, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.staging_slot == kInvalidSlot) return true;
            StagingSlot& slot = resources_.slots->StagingAt(image.staging_slot);
            if (slot.state == StagingSlotState::DecodedPixelsAvailable) return true;
            if (slot.state == StagingSlotState::DecodeOutputMapped ||
                slot.state == StagingSlotState::CancellationPending) {
                if (CancelQueuedDecode(frame)) return true;
                if (image.work_token) {
                    image.work_token->claim.store(WorkClaim::Cancelled,
                                                  std::memory_order_release);
                }
                slot.state = StagingSlotState::CancellationPending;
            }
            return false;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.staging_slot != kInvalidSlot) {
                StagingSlot& slot = resources_.slots->StagingAt(image.staging_slot);
                if (slot.state == StagingSlotState::DecodedPixelsAvailable) {
                    resources_.slots->ReleaseStaging(image.staging_slot);
                    image.staging_slot = kInvalidSlot;
                }
            }
            if (image.staging_reservation == id) {
                image.staging_reservation = kInvalidReservation;
            }
        },
        [&](const ReservationId id, const std::size_t frame) {
            resources_.images[frame].staging_reservation = id;
        },
        ReservationTable::FirstFree);

    std::vector<std::size_t> compressed_desired;
    compressed_desired.reserve(resources_.compressed_reservations.Capacity());
    for (const std::size_t frame : resources_.priority_order) {
        if (compressed_desired.size() == resources_.compressed_reservations.Capacity()) break;
        const PipelineStage stage = StageOf(resources_.images[frame]);
        if (stage == PipelineStage::Failed ||
            stage == PipelineStage::DecodeQueued ||
            stage == PipelineStage::DecodedStagingAvailable ||
            stage == PipelineStage::Uploading ||
            stage == PipelineStage::PresentationTextureAvailable) {
            continue;
        }
        append_unique(compressed_desired, frame,
                      resources_.compressed_reservations.Capacity());
    }
    resources_.compressed_reservations.Reconcile(
        compressed_desired,
        [&](const ReservationId, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.io) {
                CancelIoEx(image.io->file, &image.io->overlapped);
                resources_.slots->Compressed(image.compressed_slot).state =
                    CompressedSlotState::CancellationPending;
                return false;
            }
            if (image.compressed_slot == kInvalidSlot) return true;
            return resources_.slots->Compressed(image.compressed_slot).state ==
                   CompressedSlotState::CompressedDataAvailable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.compressed_slot != kInvalidSlot) ReleaseCompressed(image);
            if (image.compressed_reservation == id) {
                image.compressed_reservation = kInvalidReservation;
            }
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            image.compressed_reservation = id;
            image.failed = resources_.catalog.items[frame].file_bytes == 0;
        },
        ReservationTable::FirstFree);
    work_queue_.Reorder(resources_.priority_order);
}

std::vector<std::size_t> App::PrioritizedCandidates(
    const PipelineStage stage) const {
    std::vector<std::size_t> candidates;
    candidates.reserve(resources_.priority_order.size());
    for (const std::size_t index : resources_.priority_order) {
        if (StageOf(resources_.images[index]) == stage) {
            candidates.push_back(index);
        }
    }
    return candidates;
}

void App::SubmitReads() {
    for (const std::size_t index : PrioritizedCandidates(PipelineStage::WaitingIo)) {
        ImageRecord& image = resources_.images[index];
        const CatalogItem& item = resources_.catalog.items[index];
        if (item.file_bytes == 0 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            image.failed = true;
            continue;
        }
        const std::size_t compressed = static_cast<std::size_t>(item.file_bytes);
        if (compressed > config_.compressed_budget_bytes) {
            image.failed = true;
            continue;
        }
        if (resources_.compressed_bytes >
                config_.compressed_budget_bytes - compressed) {
            continue;
        }
        auto request = std::make_unique<IoRequest>();
        request->window = window_;
        request->index = index;
        request->generation = image.generation;
        request->compressed_slot = resources_.slots->AcquireCompressed(
            compressed, index, image.generation);
        if (request->compressed_slot == kInvalidSlot) {
            continue;
        }
        image.compressed_slot = request->compressed_slot;
        request->file = CreateFileW(item.path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                                        FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
        if (request->file == INVALID_HANDLE_VALUE) {
            resources_.slots->ReleaseCompressed(request->compressed_slot);
            image.compressed_slot = kInvalidSlot;
            image.failed = true;
            continue;
        }
        request->threadpool_io = CreateThreadpoolIo(request->file, &App::IoCompletion,
                                                    request.get(), nullptr);
        if (!request->threadpool_io) {
            CloseHandle(request->file);
            resources_.slots->ReleaseCompressed(request->compressed_slot);
            image.compressed_slot = kInvalidSlot;
            image.failed = true;
            continue;
        }

        resources_.compressed_bytes += compressed;
        image.io = std::move(request);
        CompressedBuffer& buffer = resources_.slots->Compressed(
            image.io->compressed_slot).resource;
        StartThreadpoolIo(image.io->threadpool_io);
        const BOOL submitted = ReadFile(image.io->file, buffer.data,
                                        static_cast<DWORD>(buffer.size), nullptr,
                                        &image.io->overlapped);
        if (!submitted && GetLastError() != ERROR_IO_PENDING) {
            CancelThreadpoolIo(image.io->threadpool_io);
            CloseThreadpoolIo(image.io->threadpool_io);
            CloseHandle(image.io->file);
            resources_.slots->ReleaseCompressed(image.io->compressed_slot);
            image.io.reset();
            image.compressed_slot = kInvalidSlot;
            resources_.compressed_bytes -= compressed;
            image.failed = true;
        }
    }
}

void App::DispatchDecodes() {
    for (const std::size_t index : PrioritizedCandidates(
             PipelineStage::CompressedReady)) {
        ImageRecord& image = resources_.images[index];
        if (!ReservationActive(resources_.staging_reservations,
                               image.staging_reservation, index)) {
            continue;
        }
        if (!resources_.catalog.items[index].header_valid) {
            ReleaseCompressed(image);
            image.failed = true;
            continue;
        }
        const CatalogItem& item = resources_.catalog.items[index];
        const std::size_t staging_bytes = item.png.decoded_bytes +
            item.png.height + static_cast<std::size_t>(item.png.width) * 4;
        if (staging_bytes == 0 || staging_bytes > config_.staging_cache_bytes) {
            ReleaseCompressed(image);
            image.failed = true;
            continue;
        }
        SlotId staging_slot = resources_.slots->AcquireStaging(
            staging_bytes, index, image.generation);
        if (staging_slot == kInvalidSlot) continue;
        DecodeStaging& staging = resources_.slots->StagingAt(staging_slot).resource;
        graphics_.MapDecodeStaging(staging, item.png.width, item.png.height,
                                   item.png.decoded_bytes);
        auto token = std::make_shared<WorkToken>();
        DecodeWork work{index, image.generation, token, image.compressed_slot,
                        staging_slot};
        resources_.slots->Compressed(image.compressed_slot).state =
            CompressedSlotState::DecodeInput;
        if (!work_queue_.TryPush(work)) {
            graphics_.UnmapDecodeStaging(staging);
            resources_.slots->ReleaseStaging(staging_slot);
            resources_.slots->Compressed(image.compressed_slot).state =
                CompressedSlotState::CompressedDataAvailable;
            break;
        }
        image.staging_slot = staging_slot;
        image.work_token = std::move(token);
    }
}

void App::SubmitUploads() {
    for (const std::size_t index : PrioritizedCandidates(
             PipelineStage::DecodedStagingAvailable)) {
        ImageRecord& image = resources_.images[index];
        if (!ReservationActive(resources_.source_reservations,
                               image.source_reservation, index)) {
            continue;
        }
        GpuTextureSlot& source = resources_.slots->GpuTextureAt(
            image.source_reservation);
        if (source.state != GpuTextureSlotState::Writable ||
            source.reserved_frame != index) {
            continue;
        }
        StagingSlot& staging_slot = resources_.slots->StagingAt(
            image.staging_slot);
        const std::size_t bytes = staging_slot.resource.surface.ByteSize();
        if (bytes == 0 || bytes > config_.gpu_cache_bytes) {
            resources_.slots->ReleaseStaging(image.staging_slot);
            image.staging_slot = kInvalidSlot;
            image.failed = true;
            continue;
        }
        const std::size_t old_bytes = source.resource.bytes;
        const std::size_t retained_bytes = resources_.gpu_bytes >= old_bytes
                                               ? resources_.gpu_bytes - old_bytes
                                               : 0;
        if (bytes > config_.gpu_cache_bytes -
                        std::min(retained_bytes, config_.gpu_cache_bytes)) {
            continue;
        }
        UploadTicket ticket = graphics_.SubmitUpload(
            index, image.generation, image.staging_slot,
            staging_slot.resource, source.resource);
        ticket.source_texture_slot = image.source_reservation;
        resources_.gpu_bytes = retained_bytes + source.resource.bytes;
        staging_slot.state = StagingSlotState::GpuCopySource;
        source.content_frame = kInvalidFrame;
        source.state = GpuTextureSlotState::Writing;
        resources_.uploads.push_back(std::move(ticket));
        if (NavigationInputPending(window_)) break;
    }
    ArmOldestFence();
}

bool App::TryPresent() {
    if (!resources_.frame_credit || resources_.reading_source_fence != 0) return false;
    const auto next = resources_.navigation.NextIndex();
    if (next) {
        ImageRecord& image = resources_.images[*next];
        if (!HasReadableSource(*next)) return false;
        const SlotId source_id = image.source_reservation;
        GpuTextureSlot& source = resources_.slots->GpuTextureAt(source_id);
        resources_.reading_source_fence = graphics_.Draw(source.resource);
        source.state = GpuTextureSlotState::Reading;
        resources_.reading_source_slot = source_id;
        ArmOldestFence();
        resources_.frame_credit = false;
        resources_.redraw_pending = false;
        resources_.navigation.CompletePresentation(*next);
        SetWindowTextW(window_, resources_.catalog.items[*next].path.filename().c_str());
        if (config_.validation_exit_after_present) {
            if (config_.validation_fullscreen && validation_fullscreen_phase_ == 0) {
                BeginFullscreenValidation();
            } else if (config_.validation_fullscreen) {
                // The fullscreen validation timer owns completion.
            } else if (!config_.validation_navigation.empty() && !validation_script_injected_) {
                if (config_.validation_warmup_ms != 0) {
                    if (!validation_script_scheduled_) {
                        validation_script_scheduled_ = true;
                        SetTimer(window_, 2, config_.validation_warmup_ms, nullptr);
                    }
                } else {
                    InjectValidationNavigation();
                }
            } else if (config_.validation_navigation.empty() ||
                       (validation_script_injected_ &&
                        validation_navigation_cursor_ >= config_.validation_navigation.size() &&
                        resources_.navigation.Empty())) {
                if (!config_.validation_navigation.empty() &&
                    resources_.navigation.CurrentIndex() != validation_expected_index_) {
                    exit_code_ = 2;
                } else if (config_.validation_elapsed_exit_code &&
                           !config_.validation_navigation.empty()) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - validation_navigation_started_);
                    exit_code_ = static_cast<int>(std::clamp<std::int64_t>(
                        elapsed.count(), 1, std::numeric_limits<int>::max()));
                }
                WriteValidationReport("navigation-complete", false);
                KillTimer(window_, 1);
                PostMessageW(window_, WM_CLOSE, 0, 0);
            }
        }
        return true;
    }
    if (resources_.redraw_pending) {
        const std::size_t current_index = resources_.navigation.CurrentIndex();
        ImageRecord& current = resources_.images[current_index];
        if (HasReadableSource(current_index)) {
            const SlotId source_id = current.source_reservation;
            GpuTextureSlot& source = resources_.slots->GpuTextureAt(source_id);
            resources_.reading_source_fence = graphics_.Draw(source.resource);
            source.state = GpuTextureSlotState::Reading;
            resources_.reading_source_slot = source_id;
            ArmOldestFence();
            resources_.frame_credit = false;
            resources_.redraw_pending = false;
            return true;
        }
    }
    return false;
}

void App::ReleaseCompressed(ImageRecord& image) {
    if (image.compressed_slot == kInvalidSlot) return;
    CompressedSlot& slot = resources_.slots->Compressed(image.compressed_slot);
    if (resources_.compressed_bytes >= slot.resource.size) {
        resources_.compressed_bytes -= slot.resource.size;
    }
    resources_.slots->ReleaseCompressed(image.compressed_slot);
    image.compressed_slot = kInvalidSlot;
}

void App::ArmOldestFence() {
    UINT64 value = resources_.reading_source_fence;
    if (!resources_.uploads.empty() &&
        (value == 0 || resources_.uploads.front().fence_value < value)) {
        value = resources_.uploads.front().fence_value;
    }
    if (value == 0) {
        resources_.armed_fence = 0;
        return;
    }
    if (resources_.armed_fence != value) {
        graphics_.ArmFence(value);
        resources_.armed_fence = value;
    }
}

void App::CancelAllIo() {
    for (ImageRecord& image : resources_.images) {
        if (image.io) CancelIoEx(image.io->file, &image.io->overlapped);
    }
    for (ImageRecord& image : resources_.images) {
        if (!image.io) continue;
        WaitForThreadpoolIoCallbacks(image.io->threadpool_io, TRUE);
        CloseThreadpoolIo(image.io->threadpool_io);
        CloseHandle(image.io->file);
        if (image.io->compressed_slot != kInvalidSlot) {
            const std::size_t bytes = resources_.slots->Compressed(
                image.io->compressed_slot).resource.size;
            if (resources_.compressed_bytes >= bytes) resources_.compressed_bytes -= bytes;
            resources_.slots->ReleaseCompressed(image.io->compressed_slot);
            image.compressed_slot = kInvalidSlot;
        }
        image.io.reset();
    }
}

}  // namespace pv
