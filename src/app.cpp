#include "app.h"

#include "common.h"
#include "processor_topology.h"

#include <array>

namespace pv {
namespace {

}  // namespace

App::ComApartment::ComApartment() {
    CheckHr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");
}

App::ComApartment::~ComApartment() { CoUninitialize(); }

App::App(Config config,
         const std::chrono::steady_clock::time_point process_started)
    : config_(std::move(config)),
      validation_(process_started, !config_.validation_navigation.empty(),
                  config_.validation_navigation.size() + 1),
      pipeline_(*this, config_, validation_, window_, catalog_loading_) {}

App::~App() {
    StopValidationNavigationTimer();
    catalog_io_.reset();
}

int App::Run(const HINSTANCE instance, const int show_command) {
    if (!config_.initial_image.empty()) OpenInitialImage();
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) {
        ThrowLastError("SetProcessDpiAwarenessContext");
    }
    com_apartment_.emplace();
    window_.Create(instance, &App::WindowProcedure, this);
    validation_.Mark(StartupMilestone::WindowReady);
    if (config_.worker_count == 0) config_.worker_count = DefaultWorkerCount();
    pipeline_.decoders_.emplace(config_.worker_count, pipeline_.work_queue_,
                      pipeline_.completion_queue_,
                      pipeline_.state_.slots);
    validation_.Mark(StartupMilestone::DecodersReady);
    while ((!config_.initial_image.empty() &&
            validation_.At(StartupMilestone::InitialContentReady) ==
                std::chrono::steady_clock::time_point{}) ||
           catalog_loading_) {
        enum class Kind { Io, Worker, Catalog };
        std::array<HANDLE, 3> handles{pipeline_.io_completion_event_.Get(),
                                      pipeline_.completion_queue_.CompletionEvent()};
        std::array<Kind, 3> kinds{Kind::Io, Kind::Worker};
        DWORD handle_count = 2;
        if (catalog_loading_ && catalog_io_) {
            handles[handle_count] = catalog_io_->CompletionEvent();
            kinds[handle_count++] = Kind::Catalog;
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(
            handle_count, handles.data(), INFINITE,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) {
            ThrowLastError("MsgWaitForMultipleObjectsEx(startup)");
        }
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handle_count) {
            const Kind kind = kinds[result - WAIT_OBJECT_0];
            if (kind == Kind::Io) {
                if (pipeline_.DrainCompletions()) pipeline_.PumpPipeline();
            } else if (kind == Kind::Worker) {
                pipeline_.OnWorkerComplete();
            } else if (kind == Kind::Catalog) {
                const bool was_loading = catalog_loading_;
                OnCatalogComplete();
                if (was_loading && !catalog_loading_) pipeline_.PumpPipeline();
            }
        }

        MSG queued{};
        constexpr int startup_message_batch_limit = 8;
        for (int processed = 0;
             processed < startup_message_batch_limit &&
             PeekMessageW(&queued, nullptr, 0, 0, PM_REMOVE);
             ++processed) {
            TranslateMessage(&queued);
            DispatchMessageW(&queued);
        }
    }
    pipeline_.graphics_.InitializeDirect3D(window_.Handle());
    pipeline_.graphics_device_ready_ = true;
    validation_.Mark(StartupMilestone::GraphicsDeviceReady);
    (void)pipeline_.DrainCompletions();
    // Storage I/O continues in the kernel while main performs the synchronous
    // D3D device call. Drain queued completions before making the newly
    // available device usable.
    pipeline_.PumpPipeline();
    pipeline_.graphics_.InitializeDirect2D();
    if (pipeline_.DrainCompletions()) pipeline_.PumpPipeline();
    pipeline_.graphics_.InitializeSwapChain();
    if (pipeline_.DrainCompletions()) pipeline_.PumpPipeline();
    pipeline_.graphics_.InitializeBackBufferTarget();
    pipeline_.graphics_ready_ = true;
    (void)pipeline_.DrainCompletions();
    pipeline_.PumpPipeline();
    validation_.Mark(StartupMilestone::GraphicsReady);
    ShowWindow(window_.Handle(),
               config_.validation_exit_after_present ? SW_HIDE : show_command);
    if (!config_.validation_exit_after_present) UpdateWindow(window_.Handle());
    if (config_.validation_exit_after_present) {
        SetTimer(window_.Handle(), 1, config_.validation_timeout_ms, nullptr);
    }
    return EventLoop();
}

LRESULT CALLBACK App::WindowProcedure(const HWND window, const UINT message,
                                      const WPARAM wparam, const LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_.Attach(window);
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
        case kMessageValidationStep:
            InjectValidationNavigationStep();
            return 0;
        case WM_SIZE:
            if (pipeline_.graphics_ready_ && wparam != SIZE_MINIMIZED) {
                pipeline_.OnSurfaceChanged(LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        case WM_PAINT:
            pipeline_.OnPaint();
            return 0;
        case WM_TIMER:
            if (wparam == 1 && config_.validation_exit_after_present) {
                const auto next = pipeline_.state_.navigation.NextIndex();
                exit_code_ = next && *next < pipeline_.state_.images.size()
                                 ? 100 + static_cast<int>(pipeline_.StageOf(pipeline_.state_.images[*next]))
                                 : 199;
                WriteValidationReport("timeout", false);
            KillTimer(window_.Handle(), 1);
            DestroyWindow(window_.Handle());
            } else if (wparam == 2 && config_.validation_exit_after_present) {
                KillTimer(window_.Handle(), 2);
                InjectValidationNavigation();
            } else if (wparam == 3 && config_.validation_fullscreen) {
                OnFullscreenValidationTimer();
            } else if (wparam == 4 && validation_.navigation_timer_active) {
                InjectValidationNavigationStep();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window_.Handle(), 1);
            KillTimer(window_.Handle(), 2);
            KillTimer(window_.Handle(), 3);
            KillTimer(window_.Handle(), 4);
            StopValidationNavigationTimer();
            running_ = false;
            window_.Detach();
            PostQuitMessage(exit_code_);
            return 0;
        default:
            return DefWindowProcW(window_.Handle(), message, wparam, lparam);
    }
}

int App::EventLoop() {
    int exit_code = 0;
    while (running_) {
        std::array<HANDLE, 5> handles{pipeline_.io_completion_event_.Get(),
                                      pipeline_.completion_queue_.CompletionEvent()};
        enum class Kind { Io, Worker, Frame, Fence, Catalog };
        std::array<Kind, 5> kinds{Kind::Io, Kind::Worker};
        DWORD count = 2;
        if (catalog_loading_ && catalog_io_) {
            handles[count] = catalog_io_->CompletionEvent();
            kinds[count++] = Kind::Catalog;
        }
        if (!pipeline_.state_.frame_credit && pipeline_.graphics_.FrameWaitableObject()) {
            handles[count] = pipeline_.graphics_.FrameWaitableObject();
            kinds[count++] = Kind::Frame;
        }
        if ((!pipeline_.state_.uploads.empty() || pipeline_.state_.reading_gpu_texture_fence != 0) &&
            pipeline_.graphics_.FenceEvent()) {
            handles[count] = pipeline_.graphics_.FenceEvent();
            kinds[count++] = Kind::Fence;
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(
            count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) ThrowLastError("MsgWaitForMultipleObjectsEx");
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            const DWORD index = result - WAIT_OBJECT_0;
            if (kinds[index] == Kind::Io) {
                if (pipeline_.DrainCompletions()) pipeline_.PumpPipeline();
            } else if (kinds[index] == Kind::Worker) pipeline_.OnWorkerComplete();
            else if (kinds[index] == Kind::Frame) pipeline_.OnFrameCredit();
            else if (kinds[index] == Kind::Fence) pipeline_.OnGpuComplete();
            else if (kinds[index] == Kind::Catalog) {
                const bool was_loading = catalog_loading_;
                OnCatalogComplete();
                if (was_loading && !catalog_loading_) pipeline_.PumpPipeline();
            }
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
    const bool asynchronous_catalog = config_.validation_file_list.empty();
    pipeline_.state_.catalog = asynchronous_catalog
                             ? BuildInitialCatalog(config_.initial_image)
                             : BuildCatalogFromList(config_.validation_file_list,
                                                    config_.initial_image);
    pipeline_.state_.generation++;
    pipeline_.state_.images.clear();
    pipeline_.state_.images.resize(pipeline_.state_.catalog.items.size());
    for (std::size_t index = 0; index < pipeline_.state_.images.size(); ++index) {
        auto& image = pipeline_.state_.images[index];
        image.generation = pipeline_.state_.generation;
        const CatalogItem& item = pipeline_.state_.catalog.items[index];
        image.failed = item.file_size_known && item.file_bytes == 0;
    }
    pipeline_.state_.navigation.Reset(pipeline_.state_.catalog.initial_index,
                                pipeline_.state_.catalog.items.size());
    pipeline_.InitializeReservations();
    pipeline_.state_.redraw_pending = true;
    pipeline_.PumpPipeline();
    validation_.Mark(StartupMilestone::InitialIoSubmitted);
    if (asynchronous_catalog) {
        catalog_loading_ = true;
        catalog_io_.emplace(config_.initial_image);
    }
}

void App::OnDirection(const int direction, const bool repeat,
                      const std::size_t repeat_count) {
    if (pipeline_.state_.images.empty()) return;
    if (!repeat && repeat_count > 1) {
        pipeline_.state_.navigation.Step(direction, false);
        pipeline_.state_.navigation.Step(direction, true, repeat_count - 1);
    } else {
        pipeline_.state_.navigation.Step(direction, repeat, repeat_count);
    }
    pipeline_.state_.reservations.MarkDirty();
    pipeline_.PumpPipeline();
}

void App::OnDirectionReleased(const int direction) {
    if (pipeline_.state_.images.empty()) return;
    pipeline_.state_.navigation.Release(direction);
    pipeline_.state_.reservations.MarkDirty();
    pipeline_.PumpPipeline();
}

void App::OnCatalogComplete() {
    if (!catalog_loading_ || !catalog_io_ || !catalog_io_->Advance()) return;
    validation_.Mark(StartupMilestone::CatalogReady);
    Catalog catalog = catalog_io_->TakeCatalog();
    catalog_io_.reset();
    catalog_loading_ = false;
    if (catalog.items.empty()) {
        throw std::runtime_error("asynchronous catalog returned no images");
    }

    const std::size_t initial = catalog.initial_index;
    catalog.items[initial] = std::move(pipeline_.state_.catalog.items[0]);
    ImageRecord initial_image = std::move(pipeline_.state_.images[0]);
    initial_image.compressed_reservation = kInvalidReservation;
    initial_image.staging_reservation = kInvalidReservation;
    initial_image.gpu_texture_reservation = kInvalidReservation;

    std::vector<ImageRecord> images(catalog.items.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        images[index].generation = pipeline_.state_.generation;
        images[index].failed = catalog.items[index].file_size_known &&
                               catalog.items[index].file_bytes == 0;
    }
    images[initial] = std::move(initial_image);
    pipeline_.work_queue_.Remap(0, initial, pipeline_.state_.generation);
    if (images[initial].io) images[initial].io->index = initial;
    if (images[initial].compressed_slot != kInvalidSlot) {
        pipeline_.state_.slots.Compressed(images[initial].compressed_slot).image = initial;
    }
    if (images[initial].staging_slot != kInvalidSlot) {
        pipeline_.state_.slots.StagingAt(images[initial].staging_slot).image = initial;
    }

    pipeline_.state_.catalog = std::move(catalog);
    pipeline_.state_.images = std::move(images);
    pipeline_.state_.navigation.Reset(initial, pipeline_.state_.images.size());
    pipeline_.InitializeReservations();
    pipeline_.state_.redraw_pending = true;
}

void App::OnFrameReady(const std::size_t index) {
    RecordValidationReady(index);
}

void App::OnFramePresented(const std::size_t index) {
    RecordValidationPresentation(index);
    window_.SetImageTitle(pipeline_.state_.catalog.items[index].path);
    if (!config_.validation_exit_after_present) return;

    if (config_.validation_fullscreen &&
        validation_.fullscreen_phase == FullscreenValidationPhase::Pending) {
        BeginFullscreenValidation();
        return;
    }
    if (config_.validation_fullscreen) return;

    if (!config_.validation_navigation.empty() &&
        !validation_.script_injected) {
        if (config_.validation_warmup_ms != 0) {
            if (!validation_.script_scheduled) {
                validation_.script_scheduled = true;
                SetTimer(window_.Handle(), 2,
                         config_.validation_warmup_ms, nullptr);
            }
        } else {
            InjectValidationNavigation();
        }
        return;
    }

    const bool navigation_complete = config_.validation_navigation.empty() ||
        (validation_.script_injected &&
         validation_.navigation_cursor >= config_.validation_navigation.size() &&
         pipeline_.state_.navigation.Empty());
    if (!navigation_complete) return;

    if (!config_.validation_navigation.empty() &&
        pipeline_.state_.navigation.CurrentIndex() !=
            validation_.expected_index) {
        exit_code_ = 2;
    } else if (config_.validation_elapsed_exit_code &&
               !config_.validation_navigation.empty()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - validation_.navigation_started);
        exit_code_ = static_cast<int>(std::clamp<std::int64_t>(
            elapsed.count(), 1, std::numeric_limits<int>::max()));
    }
    WriteValidationReport("navigation-complete", false);
    KillTimer(window_.Handle(), 1);
    PostMessageW(window_.Handle(), WM_CLOSE, 0, 0);
}

void App::ToggleFullscreen() {
    window_.ToggleFullscreen();
}

}  // namespace pv
