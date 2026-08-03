#include "app.h"

#include "common.h"

#include <array>
#include <cmath>
#include <fstream>
#include <numeric>

namespace pv {
namespace {

constexpr wchar_t kWindowClass[] = L"PhotoViewer.Window";

std::size_t Distance(const std::size_t left, const std::size_t right) noexcept {
    return left > right ? left - right : right - left;
}

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

}  // namespace

App::App(Config config)
    : config_(std::move(config)), work_queue_(config_.work_queue_capacity) {}

App::~App() {
    StopValidationNavigationTimer();
    decoders_.reset();
    CancelAllIo();
}

int App::Run(const HINSTANCE instance, const int show_command) {
    InitializeWindow(instance, show_command);
    decoders_ = std::make_unique<DecoderPool>(config_.worker_count, work_queue_,
                                              completion_queue_, window_);
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
            if (wparam == VK_LEFT) OnDirection(-1, repeat);
            else if (wparam == VK_RIGHT) OnDirection(1, repeat);
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
                                 ? 100 + static_cast<int>(resources_.images[*next].stage)
                                 : 199;
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
        if (!resources_.uploads.empty() && graphics_.FenceEvent()) {
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
                             : BuildCatalogFromList(config_.validation_file_list);
    resources_.generation++;
    resources_.images.clear();
    resources_.images.resize(resources_.catalog.items.size());
    for (std::size_t index = 0; index < resources_.images.size(); ++index) {
        auto& image = resources_.images[index];
        image.generation = resources_.generation;
        image.stage = resources_.catalog.items[index].file_bytes != 0
                          ? PipelineStage::Outside
                          : PipelineStage::Failed;
    }
    resources_.navigation.Reset(resources_.catalog.initial_index,
                                resources_.catalog.items.size());
    resources_.redraw_pending = true;
    PumpPipeline();
}

void App::OnDirection(const int direction, const bool repeat) {
    if (resources_.images.empty()) return;
    resources_.navigation.Step(direction, repeat);
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

void App::OnIoComplete(IoRequest* const request) {
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
    const std::size_t allocation = request->compressed ? request->compressed->size : 0;
    const bool success = io_result == ERROR_SUCCESS && transferred == allocation;
    const bool current = request->generation == image.generation;
    auto completed = std::move(image.io);

    if (success && current && InCpuRange(request->index)) {
        CatalogItem& item = resources_.catalog.items[request->index];
        const auto header = ParsePngHeader(std::span<const std::byte>(
            completed->compressed->data, completed->compressed->size));
        if (header) {
            item.png = *header;
            item.header_valid = true;
            image.compressed = std::move(completed->compressed);
            image.stage = PipelineStage::CompressedReady;
        } else {
            if (resources_.compressed_bytes >= allocation) {
                resources_.compressed_bytes -= allocation;
            }
            ReleaseCompressedBuffer(std::move(completed->compressed));
            image.stage = PipelineStage::Failed;
        }
    } else {
        if (resources_.compressed_bytes >= allocation) resources_.compressed_bytes -= allocation;
        ReleaseCompressedBuffer(std::move(completed->compressed));
        ReleaseReservation(image);
        if (current && io_result != ERROR_OPERATION_ABORTED && InCpuRange(request->index)) {
            image.stage = PipelineStage::Failed;
        } else {
            image.stage = current && InCpuRange(request->index)
                              ? PipelineStage::WaitingIo
                              : PipelineStage::Outside;
        }
    }
    PumpPipeline();
}

void App::OnWorkerComplete() {
    for (ReleasedInput& input : completion_queue_.DrainReleasedInputs()) {
        if (input.compressed && resources_.compressed_bytes >= input.compressed->size) {
            resources_.compressed_bytes -= input.compressed->size;
        }
        ReleaseCompressedBuffer(std::move(input.compressed));
    }
    for (DecodeResult& result : completion_queue_.Drain()) {
        if (result.index >= resources_.images.size()) {
            ReleaseSurface(std::move(result.surface));
            continue;
        }
        ImageRecord& image = resources_.images[result.index];
        if (result.generation != image.generation) {
            ReleaseSurface(std::move(result.surface));
            continue;
        }
        image.work_token.reset();
        if (result.success && InCpuRange(result.index)) {
            image.cpu = std::move(result.surface);
            image.stage = PipelineStage::DecodedPixelSurfaceAvailable;
        } else {
            ReleaseSurface(std::move(result.surface));
            if (!result.cancelled && FAILED(result.error) && InCpuRange(result.index)) {
                image.stage = PipelineStage::Failed;
            } else {
                image.stage = InCpuRange(result.index) ? PipelineStage::WaitingIo
                                                       : PipelineStage::Outside;
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
    while (!resources_.uploads.empty() &&
           resources_.uploads.front().fence_value <= completed) {
        UploadTicket ticket = std::move(resources_.uploads.front());
        resources_.uploads.pop_front();
        if (ticket.index >= resources_.images.size()) {
            ReleaseSurface(std::move(ticket.source));
            continue;
        }
        ImageRecord& image = resources_.images[ticket.index];
        const bool keep_gpu = ticket.generation == image.generation &&
                              (InGpuRange(ticket.index) || InRequiredRange(ticket.index) ||
                               ticket.index == resources_.navigation.CurrentIndex());
        if (keep_gpu) {
            image.gpu = graphics_.FinishUpload(ticket);
            image.stage = PipelineStage::PresentationTextureAvailable;
            ReleaseSurface(std::move(ticket.source));
        } else {
            if (resources_.gpu_bytes >= ticket.bytes) resources_.gpu_bytes -= ticket.bytes;
            if (ticket.generation == image.generation && InCpuRange(ticket.index)) {
                image.cpu = std::move(ticket.source);
                image.stage = PipelineStage::DecodedPixelSurfaceAvailable;
            } else {
                ReleaseSurface(std::move(ticket.source));
                image.stage = PipelineStage::Outside;
            }
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
    resources_.navigation.Step(direction, validation_navigation_cursor_ != 0);
    ++validation_navigation_cursor_;
    if (validation_navigation_cursor_ >= config_.validation_navigation.size()) {
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
                               "DecodeQueued", "DecodedPixelSurfaceAvailable", "Uploading",
                               "PresentationTextureAvailable",
                               "CancelPending", "Failed"};
    std::array<std::size_t, names.size()> counts{};
    for (const ImageRecord& image : resources_.images) {
        const std::size_t stage = static_cast<std::size_t>(image.stage);
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
            if (resources_.images[index].stage != stage) continue;
            if (!first) output << ',';
            output << index;
            first = false;
        }
        output << '\n';
    };
    write_indices("DecodedPixelSurfaceAvailable_indices",
                  PipelineStage::DecodedPixelSurfaceAvailable);
    write_indices("Uploading_indices", PipelineStage::Uploading);
    write_indices("PresentationTextureAvailable_indices",
                  PipelineStage::PresentationTextureAvailable);
    output << "compressed_bytes=" << resources_.compressed_bytes << '\n'
           << "cpu_committed_bytes=" << resources_.cpu_committed_bytes << '\n'
           << "gpu_bytes=" << resources_.gpu_bytes << '\n'
           << "free_surfaces=" << resources_.free_surfaces.size() << '\n'
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
           << "decode_count=" << decoders_->DecodeCount() << '\n'
           << "decode_nanoseconds=" << decoders_->DecodeNanoseconds() << '\n'
           << "upload_count=" << graphics_.UploadCount() << '\n'
           << "upload_nanoseconds=" << graphics_.UploadNanoseconds() << '\n'
           << "draw_count=" << graphics_.DrawCount() << '\n'
           << "draw_nanoseconds=" << graphics_.DrawNanoseconds() << '\n';
}

void App::PumpPipeline() {
    if (resources_.images.empty()) return;
    for (int pass = 0; pass < 3; ++pass) {
        RecalculateRanges();
        ReclaimOutsideRanges();
        SubmitReads();
        DispatchDecodes();
        SubmitUploads();
        if (!TryPresent()) break;
    }
}

std::size_t App::CountStage(const PipelineStage stage) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        resources_.images.begin(), resources_.images.end(),
        [stage](const ImageRecord& image) { return image.stage == stage; }));
}

void App::RecalculateRanges() {
    const std::size_t count = resources_.images.size();
    if (count == 0) return;
    const auto [required_low, required_high] = resources_.navigation.RequiredBounds();
    const int direction = resources_.navigation.PreferredDirection();
    const auto subtract = [](const std::size_t value, const std::size_t amount) {
        return value > amount ? value - amount : 0;
    };
    const auto add = [count](const std::size_t value, const std::size_t amount) {
        return std::min(count - 1, value + std::min(amount, count - 1 - value));
    };
    resources_.ranges.required_low = required_low;
    resources_.ranges.required_high = required_high;
    if (resources_.navigation.InitialPending()) {
        const std::size_t initial = resources_.navigation.CurrentIndex();
        resources_.ranges.cpu_low = initial;
        resources_.ranges.cpu_high = initial;
        resources_.ranges.gpu_low = initial;
        resources_.ranges.gpu_high = initial;
    } else if (direction >= 0) {
        resources_.ranges.cpu_low = subtract(required_low, 6);
        resources_.ranges.cpu_high = count - 1;
        resources_.ranges.gpu_low = subtract(required_low, 2);
        resources_.ranges.gpu_high = count - 1;
    } else {
        resources_.ranges.cpu_low = 0;
        resources_.ranges.cpu_high = add(required_high, 6);
        resources_.ranges.gpu_low = 0;
        resources_.ranges.gpu_high = add(required_high, 2);
    }
    for (std::size_t index = resources_.ranges.cpu_low;
         index <= resources_.ranges.cpu_high; ++index) {
        ImageRecord& image = resources_.images[index];
        const bool on_requested_side = direction >= 0
                                           ? index >= required_low
                                           : index <= required_high;
        if (image.stage == PipelineStage::Outside &&
            (InRequiredRange(index) || on_requested_side)) {
            image.stage = resources_.catalog.items[index].file_bytes != 0
                              ? PipelineStage::WaitingIo
                              : PipelineStage::Failed;
        }
    }
}

bool App::InRequiredRange(const std::size_t index) const noexcept {
    return index >= resources_.ranges.required_low && index <= resources_.ranges.required_high;
}

bool App::InCpuRange(const std::size_t index) const noexcept {
    return index >= resources_.ranges.cpu_low && index <= resources_.ranges.cpu_high;
}

bool App::InGpuRange(const std::size_t index) const noexcept {
    return index >= resources_.ranges.gpu_low && index <= resources_.ranges.gpu_high;
}

void App::ReclaimOutsideRanges() {
    const std::size_t current = resources_.navigation.CurrentIndex();
    for (std::size_t index = 0; index < resources_.images.size(); ++index) {
        ImageRecord& image = resources_.images[index];
        if (image.stage == PipelineStage::PresentationTextureAvailable &&
            !InGpuRange(index) &&
            !InRequiredRange(index) && index != current) {
            EvictGpu(index);
        }
        if (InCpuRange(index)) continue;
        switch (image.stage) {
            case PipelineStage::WaitingIo:
                image.stage = PipelineStage::Outside;
                break;
            case PipelineStage::IoInFlight:
                if (image.io) CancelIoEx(image.io->file, &image.io->overlapped);
                image.stage = PipelineStage::CancelPending;
                break;
            case PipelineStage::CompressedReady:
                ReleaseCompressed(image);
                ReleaseReservation(image);
                image.stage = PipelineStage::Outside;
                break;
            case PipelineStage::DecodeQueued:
                if (image.work_token) {
                    image.work_token->claim.store(WorkClaim::Cancelled,
                                                  std::memory_order_release);
                }
                image.stage = PipelineStage::CancelPending;
                break;
            case PipelineStage::DecodedPixelSurfaceAvailable:
                ReleaseSurface(std::move(image.cpu));
                image.stage = PipelineStage::Outside;
                break;
            default:
                break;
        }
    }
}

std::vector<std::size_t> App::PrioritizedCandidates(const PipelineStage stage,
                                                     const bool gpu_range) const {
    std::vector<std::size_t> candidates;
    const std::size_t low = gpu_range ? resources_.ranges.gpu_low : resources_.ranges.cpu_low;
    const std::size_t high = gpu_range ? resources_.ranges.gpu_high : resources_.ranges.cpu_high;
    for (std::size_t index = low; index <= high; ++index) {
        if (resources_.images[index].stage == stage) candidates.push_back(index);
    }
    const std::size_t target = resources_.navigation.NextIndex().value_or(
        resources_.navigation.CurrentIndex());
    std::sort(candidates.begin(), candidates.end(), [&](const std::size_t left,
                                                         const std::size_t right) {
        const auto key = [&](const std::size_t value) {
            return std::tuple{value == target ? 0 : (InRequiredRange(value) ? 1 : 2),
                              Distance(value, target), value};
        };
        return key(left) < key(right);
    });
    return candidates;
}

void App::SubmitReads() {
    for (const std::size_t index : PrioritizedCandidates(PipelineStage::WaitingIo, false)) {
        ImageRecord& image = resources_.images[index];
        const CatalogItem& item = resources_.catalog.items[index];
        if (item.file_bytes == 0 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            image.stage = PipelineStage::Failed;
            continue;
        }
        const std::size_t compressed = static_cast<std::size_t>(item.file_bytes);
        if (compressed > config_.compressed_budget_bytes ||
            resources_.compressed_bytes > config_.compressed_budget_bytes - compressed) {
            continue;
        }
        auto request = std::make_unique<IoRequest>();
        request->window = window_;
        request->index = index;
        request->generation = image.generation;
        request->compressed = AcquireCompressed(compressed);
        if (!request->compressed) {
            continue;
        }
        request->file = CreateFileW(item.path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                                        FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
        if (request->file == INVALID_HANDLE_VALUE) {
            ReleaseCompressedBuffer(std::move(request->compressed));
            image.stage = PipelineStage::Failed;
            continue;
        }
        request->threadpool_io = CreateThreadpoolIo(request->file, &App::IoCompletion,
                                                    request.get(), nullptr);
        if (!request->threadpool_io) {
            CloseHandle(request->file);
            ReleaseCompressedBuffer(std::move(request->compressed));
            image.stage = PipelineStage::Failed;
            continue;
        }

        resources_.compressed_bytes += compressed;
        image.stage = PipelineStage::IoInFlight;
        image.io = std::move(request);
        StartThreadpoolIo(image.io->threadpool_io);
        const BOOL submitted = ReadFile(image.io->file, image.io->compressed->data,
                                        static_cast<DWORD>(image.io->compressed->size), nullptr,
                                        &image.io->overlapped);
        if (!submitted && GetLastError() != ERROR_IO_PENDING) {
            CancelThreadpoolIo(image.io->threadpool_io);
            CloseThreadpoolIo(image.io->threadpool_io);
            CloseHandle(image.io->file);
            ReleaseCompressedBuffer(std::move(image.io->compressed));
            image.io.reset();
            resources_.compressed_bytes -= compressed;
            ReleaseReservation(image);
            image.stage = PipelineStage::Failed;
        }
    }
}

void App::DispatchDecodes() {
    for (const std::size_t index : PrioritizedCandidates(PipelineStage::CompressedReady, false)) {
        if (work_queue_.Size() >= config_.work_queue_capacity) break;
        ImageRecord& image = resources_.images[index];
        if (!resources_.catalog.items[index].header_valid) {
            ReleaseCompressed(image);
            image.stage = PipelineStage::Failed;
            continue;
        }
        if (resources_.navigation.HeldDirection() == 0 && !InRequiredRange(index)) {
            const CatalogItem& item = resources_.catalog.items[index];
            const std::size_t surface_bytes = item.png.decoded_bytes + item.png.height;
            const std::size_t surface_capacity = surface_bytes == 0
                                                     ? 0
                                                     : config_.cpu_cache_bytes / surface_bytes;
            const std::size_t ready_limit = std::max<std::size_t>(1, surface_capacity);
            const std::size_t gpu_capacity = item.png.decoded_bytes == 0
                                                 ? 0
                                                 : config_.gpu_cache_bytes /
                                                       item.png.decoded_bytes;
            const std::size_t resident_or_active =
                CountStage(PipelineStage::PresentationTextureAvailable) +
                CountStage(PipelineStage::Uploading) +
                CountStage(PipelineStage::DecodedPixelSurfaceAvailable) +
                CountStage(PipelineStage::DecodeQueued);
            if (CountStage(PipelineStage::DecodedPixelSurfaceAvailable) >= ready_limit ||
                resident_or_active >= gpu_capacity + ready_limit) {
                continue;
            }
        }
        EvictCpuCopiesForBudget(resources_.catalog.items[index].png.decoded_bytes);
        std::unique_ptr<CpuSurface> surface = AcquireSurface(resources_.catalog.items[index]);
        if (!surface) {
            const std::size_t target = resources_.navigation.NextIndex().value_or(
                resources_.navigation.CurrentIndex());
            const auto priority = [&](const std::size_t value) {
                return std::tuple{value == target ? 0 : (InRequiredRange(value) ? 1 : 2),
                                  Distance(value, target), value};
            };
            std::optional<std::size_t> victim;
            for (std::size_t candidate = resources_.ranges.cpu_low;
                 candidate <= resources_.ranges.cpu_high; ++candidate) {
                if (resources_.images[candidate].stage !=
                        PipelineStage::DecodedPixelSurfaceAvailable ||
                    !resources_.images[candidate].cpu) {
                    continue;
                }
                if (!victim || priority(candidate) > priority(*victim)) {
                    victim = candidate;
                }
            }
            if (victim && priority(index) < priority(*victim)) {
                ReleaseSurface(std::move(resources_.images[*victim].cpu));
                resources_.images[*victim].stage = PipelineStage::WaitingIo;
                surface = AcquireSurface(resources_.catalog.items[index]);
            }
        }
        if (!surface) {
            continue;
        }
        auto token = std::make_shared<WorkToken>();
        DecodeWork work{index, image.generation, token, std::move(image.compressed),
                        std::move(surface)};
        if (!work_queue_.TryPush(work)) {
            ReleaseSurface(std::move(work.surface));
            image.compressed = std::move(work.compressed);
            break;
        }
        image.work_token = std::move(token);
        image.stage = PipelineStage::DecodeQueued;
    }
}

void App::SubmitUploads() {
    const std::size_t target = resources_.navigation.NextIndex().value_or(
        resources_.navigation.CurrentIndex());
    const int direction = resources_.navigation.PreferredDirection();
    const auto priority = [&](const std::size_t value) {
        const bool preferred_side = direction >= 0 ? value >= target : value <= target;
        return std::tuple{value == target ? 0 : (InRequiredRange(value) ? 1 : 2),
                          Distance(value, target), preferred_side ? 0 : 1, value};
    };
    for (const std::size_t index : PrioritizedCandidates(
             PipelineStage::DecodedPixelSurfaceAvailable, true)) {
        ImageRecord& image = resources_.images[index];
        const std::size_t bytes = image.cpu ? image.cpu->ByteSize() : 0;
        if (bytes == 0 || bytes > config_.gpu_cache_bytes) {
            image.stage = PipelineStage::Failed;
            ReleaseSurface(std::move(image.cpu));
            continue;
        }
        std::size_t reserved_higher_priority_bytes = 0;
        for (std::size_t candidate = resources_.ranges.gpu_low;
             candidate <= resources_.ranges.gpu_high; ++candidate) {
            if (candidate == index || !(priority(candidate) < priority(index))) continue;
            const PipelineStage stage = resources_.images[candidate].stage;
            if (stage == PipelineStage::PresentationTextureAvailable ||
                stage == PipelineStage::Uploading ||
                stage == PipelineStage::Failed || stage == PipelineStage::Outside) {
                continue;
            }
            const std::size_t candidate_bytes =
                resources_.catalog.items[candidate].png.decoded_bytes != 0
                    ? resources_.catalog.items[candidate].png.decoded_bytes
                    : bytes;
            if (reserved_higher_priority_bytes >
                config_.gpu_cache_bytes - std::min(candidate_bytes,
                                                   config_.gpu_cache_bytes)) {
                reserved_higher_priority_bytes = config_.gpu_cache_bytes;
                break;
            }
            reserved_higher_priority_bytes += candidate_bytes;
        }
        if (bytes > config_.gpu_cache_bytes -
                        std::min(reserved_higher_priority_bytes,
                                 config_.gpu_cache_bytes)) {
            continue;
        }
        const std::size_t available_for_existing =
            config_.gpu_cache_bytes - bytes - reserved_higher_priority_bytes;
        while (resources_.gpu_bytes > available_for_existing) {
            std::optional<std::size_t> victim;
            for (std::size_t candidate = 0; candidate < resources_.images.size(); ++candidate) {
                if (candidate == resources_.navigation.CurrentIndex() ||
                    candidate == target ||
                    resources_.images[candidate].stage !=
                        PipelineStage::PresentationTextureAvailable) {
                    continue;
                }
                if (!victim || priority(candidate) > priority(*victim)) {
                    victim = candidate;
                }
            }
            if (!victim || !(priority(index) < priority(*victim))) break;
            EvictGpu(*victim);
        }
        if (resources_.gpu_bytes > available_for_existing) continue;
        UploadTicket ticket = graphics_.SubmitUpload(index, image.generation,
                                                     std::move(image.cpu));
        resources_.gpu_bytes += bytes;
        image.stage = PipelineStage::Uploading;
        resources_.uploads.push_back(std::move(ticket));
    }
    ArmOldestFence();
}

bool App::TryPresent() {
    if (!resources_.frame_credit) return false;
    const auto next = resources_.navigation.NextIndex();
    if (next) {
        ImageRecord& image = resources_.images[*next];
        if (image.stage != PipelineStage::PresentationTextureAvailable) return false;
        graphics_.Draw(image.gpu);
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
        ImageRecord& current = resources_.images[resources_.navigation.CurrentIndex()];
        if (current.stage == PipelineStage::PresentationTextureAvailable) {
            graphics_.Draw(current.gpu);
            resources_.frame_credit = false;
            resources_.redraw_pending = false;
            return true;
        }
    }
    return false;
}

std::unique_ptr<CpuSurface> App::AcquireSurface(const CatalogItem& item) {
    const std::size_t required = item.png.decoded_bytes + item.png.height;
    auto best = resources_.free_surfaces.end();
    for (auto iterator = resources_.free_surfaces.begin();
         iterator != resources_.free_surfaces.end(); ++iterator) {
        if ((*iterator)->allocation_bytes >= required &&
            (best == resources_.free_surfaces.end() ||
             (*iterator)->allocation_bytes < (*best)->allocation_bytes)) {
            best = iterator;
        }
    }
    std::unique_ptr<CpuSurface> surface;
    if (best != resources_.free_surfaces.end()) {
        surface = std::move(*best);
        resources_.free_surfaces.erase(best);
        if (!surface->Allocate(required)) return nullptr;
    } else {
        TrimFreeSurfaces(required);
        if (resources_.cpu_committed_bytes > config_.cpu_cache_bytes - required) return nullptr;
        surface = std::make_unique<CpuSurface>();
        if (!surface->Allocate(required)) return nullptr;
        resources_.cpu_committed_bytes += required;
    }
    surface->width = item.png.width;
    surface->height = item.png.height;
    surface->stride = item.png.width * 4U;
    surface->byte_size = item.png.decoded_bytes;
    return surface;
}

std::shared_ptr<CompressedBuffer> App::AcquireCompressed(const std::size_t bytes) {
    auto best = resources_.free_compressed.end();
    for (auto iterator = resources_.free_compressed.begin();
         iterator != resources_.free_compressed.end(); ++iterator) {
        if ((*iterator)->allocation_size >= bytes &&
            (best == resources_.free_compressed.end() ||
             (*iterator)->allocation_size < (*best)->allocation_size)) {
            best = iterator;
        }
    }
    if (best != resources_.free_compressed.end()) {
        auto buffer = std::move(*best);
        resources_.free_compressed.erase(best);
        if (!buffer->Allocate(bytes)) return {};
        return buffer;
    }

    constexpr std::size_t alignment = 4096;
    if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) return {};
    const std::size_t required = (bytes + (alignment - 1)) & ~(alignment - 1);
    if (required > config_.compressed_budget_bytes) return {};
    while (!resources_.free_compressed.empty() &&
           resources_.compressed_committed_bytes >
               config_.compressed_budget_bytes - required) {
        const auto largest = std::max_element(
            resources_.free_compressed.begin(), resources_.free_compressed.end(),
            [](const auto& left, const auto& right) {
                return left->allocation_size < right->allocation_size;
            });
        resources_.compressed_committed_bytes -= (*largest)->allocation_size;
        resources_.free_compressed.erase(largest);
    }
    if (resources_.compressed_committed_bytes > config_.compressed_budget_bytes - required) {
        return {};
    }
    auto buffer = std::make_shared<CompressedBuffer>();
    if (!buffer->Allocate(bytes)) return {};
    resources_.compressed_committed_bytes += buffer->allocation_size;
    return buffer;
}

void App::ReleaseCompressedBuffer(std::shared_ptr<CompressedBuffer> buffer) {
    if (!buffer) return;
    resources_.free_compressed.push_back(std::move(buffer));
}

void App::ReleaseSurface(std::unique_ptr<CpuSurface> surface) {
    if (!surface) return;
    surface->width = 0;
    surface->height = 0;
    surface->stride = 0;
    surface->byte_size = 0;
    resources_.free_surfaces.push_back(std::move(surface));
}

void App::TrimFreeSurfaces(const std::size_t bytes_needed) {
    while (!resources_.free_surfaces.empty() &&
           (bytes_needed > config_.cpu_cache_bytes ||
            resources_.cpu_committed_bytes + resources_.reserved_output_bytes >
                config_.cpu_cache_bytes - bytes_needed)) {
        const auto largest = std::max_element(
            resources_.free_surfaces.begin(), resources_.free_surfaces.end(),
            [](const auto& left, const auto& right) {
                return left->allocation_bytes < right->allocation_bytes;
            });
        resources_.cpu_committed_bytes -= (*largest)->allocation_bytes;
        resources_.free_surfaces.erase(largest);
    }
}

void App::EvictCpuCopiesForBudget(const std::size_t bytes_needed) {
    const std::size_t target = resources_.navigation.NextIndex().value_or(
        resources_.navigation.CurrentIndex());
    while (bytes_needed <= config_.cpu_cache_bytes &&
           resources_.cpu_committed_bytes + resources_.reserved_output_bytes >
               config_.cpu_cache_bytes - bytes_needed) {
        std::optional<std::size_t> victim;
        std::size_t victim_distance = 0;
        for (std::size_t index = 0; index < resources_.images.size(); ++index) {
            const ImageRecord& image = resources_.images[index];
            if (image.stage != PipelineStage::PresentationTextureAvailable || !image.cpu) {
                continue;
            }
            const std::size_t distance = Distance(index, target);
            if (!victim || distance > victim_distance) {
                victim = index;
                victim_distance = distance;
            }
        }
        if (!victim) break;
        ReleaseSurface(std::move(resources_.images[*victim].cpu));
        TrimFreeSurfaces(bytes_needed);
    }
}

void App::ReleaseCompressed(ImageRecord& image) {
    if (!image.compressed) return;
    if (resources_.compressed_bytes >= image.compressed->size) {
        resources_.compressed_bytes -= image.compressed->size;
    }
    ReleaseCompressedBuffer(std::move(image.compressed));
}

void App::ReleaseReservation(ImageRecord& image) {
    if (resources_.reserved_output_bytes >= image.reserved_output_bytes) {
        resources_.reserved_output_bytes -= image.reserved_output_bytes;
    }
    image.reserved_output_bytes = 0;
}

void App::EvictGpu(const std::size_t index) {
    ImageRecord& image = resources_.images[index];
    if (!image.gpu.texture) return;
    if (resources_.gpu_bytes >= image.gpu.bytes) resources_.gpu_bytes -= image.gpu.bytes;
    image.gpu = {};
    if (image.cpu) image.stage = PipelineStage::DecodedPixelSurfaceAvailable;
    else {
        const std::size_t target = resources_.navigation.NextIndex().value_or(
            resources_.navigation.CurrentIndex());
        const int direction = resources_.navigation.PreferredDirection();
        const bool on_requested_side = direction >= 0 ? index >= target : index <= target;
        image.stage = InCpuRange(index) && (InRequiredRange(index) || on_requested_side)
                          ? PipelineStage::WaitingIo
                          : PipelineStage::Outside;
    }
}

void App::ArmOldestFence() {
    if (resources_.uploads.empty()) {
        resources_.armed_fence = 0;
        return;
    }
    const UINT64 value = resources_.uploads.front().fence_value;
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
        image.io.reset();
    }
}

}  // namespace pv
