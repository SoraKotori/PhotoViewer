#include "app.h"

#include "win32_support.h"
#include "processor_topology.h"

#include <shobjidl.h>

#include <array>

namespace pv {
namespace {

std::optional<std::filesystem::path> SelectInitialImage() {
    ComPtr<IFileOpenDialog> dialog;
    CheckHr(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                             CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)),
            "CoCreateInstance(CLSID_FileOpenDialog)");

    constexpr COMDLG_FILTERSPEC filters[] = {
        {L"PNG images (*.png)", L"*.png"},
    };
    CheckHr(dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters),
            "IFileOpenDialog::SetFileTypes");
    CheckHr(dialog->SetFileTypeIndex(1),
            "IFileOpenDialog::SetFileTypeIndex");
    CheckHr(dialog->SetTitle(L"Open PNG image"),
            "IFileOpenDialog::SetTitle");

    FILEOPENDIALOGOPTIONS options{};
    CheckHr(dialog->GetOptions(&options), "IFileOpenDialog::GetOptions");
    options = static_cast<FILEOPENDIALOGOPTIONS>(
        options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
        FOS_PATHMUSTEXIST);
    CheckHr(dialog->SetOptions(options), "IFileOpenDialog::SetOptions");

    const HRESULT shown = dialog->Show(nullptr);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    CheckHr(shown, "IFileOpenDialog::Show");

    ComPtr<IShellItem> item;
    CheckHr(dialog->GetResult(&item), "IFileOpenDialog::GetResult");
    PWSTR raw_path = nullptr;
    CheckHr(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path),
            "IShellItem::GetDisplayName");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(
        raw_path, &CoTaskMemFree);
    return std::filesystem::path(path.get());
}

}  // namespace

App::ComApartment::ComApartment() {
    CheckHr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");
}

App::ComApartment::~ComApartment() { CoUninitialize(); }

App::App(Config config,
         const std::chrono::steady_clock::time_point process_started)
    : config_(std::move(config)),
      validation_(config_, process_started),
      pipeline_(*this, config_, window_, &validation_.Telemetry()) {}

App::~App() {
    validation_.StopValidationNavigationTimer(window_);
    catalog_io_.reset();
}

int App::Run(const HINSTANCE instance, const int show_command) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        GetLastError() != ERROR_ACCESS_DENIED) {
        ThrowLastError("SetProcessDpiAwarenessContext");
    }
    com_apartment_.emplace();
    if (config_.prompt_for_initial_image) {
        const auto selected = SelectInitialImage();
        if (!selected) return 0;
        config_.initial_image = *selected;
    }
    if (!config_.initial_image.empty()) OpenInitialImage();
    window_.Create(instance, &App::WindowProcedure, this);
    validation_.MarkWindowReady();
    if (config_.worker_count == 0) config_.worker_count = DefaultWorkerCount();
    pipeline_.StartWorkers(config_.worker_count);
    while ((!config_.initial_image.empty() &&
            pipeline_.InitialContentPending()) ||
           catalog_loading_) {
        enum class Kind { Io, Worker, Catalog };
        std::array<HANDLE, 3> handles{pipeline_.IoCompletionEvent(),
                                     pipeline_.WorkerCompletionEvent()};
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
                pipeline_.OnIoReady();
            } else if (kind == Kind::Worker) {
                pipeline_.OnWorkerNotification();
            } else if (kind == Kind::Catalog) {
                OnCatalogComplete();
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
    if (pipeline_.InitialContentFailed()) {
        if (config_.validation_exit_after_present) {
            return validation_.OnTimeout(pipeline_);
        }
        throw std::runtime_error("initial image could not be decoded");
    }
    pipeline_.InitializeGraphics();
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
            if (wparam == VK_LEFT) OnDirection(-1, repeat);
            else if (wparam == VK_RIGHT) OnDirection(1, repeat);
            else if (wparam == VK_F11 && !repeat) ToggleFullscreen();
            return 0;
        }
        case WM_KEYUP:
            if (wparam == VK_LEFT) OnDirectionReleased(-1);
            else if (wparam == VK_RIGHT) OnDirectionReleased(1);
            return 0;
        case kMessageValidationStep:
            validation_.InjectValidationNavigationStep(pipeline_, window_);
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                pipeline_.ResizeSurface(LOWORD(lparam), HIWORD(lparam));
            }
            return 0;
        case WM_PAINT:
            pipeline_.Paint();
            return 0;
        case WM_TIMER:
            if (wparam == 1 && config_.validation_exit_after_present) {
                exit_code_ = validation_.OnTimeout(pipeline_);
                KillTimer(window_.Handle(), 1);
                DestroyWindow(window_.Handle());
            } else if (wparam == 2 && config_.validation_exit_after_present) {
                KillTimer(window_.Handle(), 2);
                validation_.InjectValidationNavigation(pipeline_, window_);
            } else if (wparam == 3 && config_.validation_fullscreen) {
                if (const auto result =
                        validation_.OnFullscreenValidationTimer(
                            window_, pipeline_)) {
                    exit_code_ = *result;
                    PostMessageW(window_.Handle(), WM_CLOSE, 0, 0);
                }
            } else if (wparam == 4 &&
                       validation_.NavigationTimerActive()) {
                validation_.InjectValidationNavigationStep(pipeline_, window_);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window_.Handle(), 1);
            KillTimer(window_.Handle(), 2);
            KillTimer(window_.Handle(), 3);
            KillTimer(window_.Handle(), 4);
            validation_.StopValidationNavigationTimer(window_);
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
        std::array<HANDLE, 5> handles{pipeline_.IoCompletionEvent(),
                                     pipeline_.WorkerCompletionEvent()};
        enum class Kind { Io, Worker, Frame, Fence, Catalog };
        std::array<Kind, 5> kinds{Kind::Io, Kind::Worker};
        DWORD count = 2;
        if (catalog_loading_ && catalog_io_) {
            handles[count] = catalog_io_->CompletionEvent();
            kinds[count++] = Kind::Catalog;
        }
        if (const HANDLE frame = pipeline_.FrameWaitEvent()) {
            handles[count] = frame;
            kinds[count++] = Kind::Frame;
        }
        if (const HANDLE fence = pipeline_.GpuWaitEvent()) {
            handles[count] = fence;
            kinds[count++] = Kind::Fence;
        }
        const DWORD result = MsgWaitForMultipleObjectsEx(
            count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) ThrowLastError("MsgWaitForMultipleObjectsEx");
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            const DWORD index = result - WAIT_OBJECT_0;
            if (kinds[index] == Kind::Io) {
                pipeline_.OnIoReady();
            } else if (kinds[index] == Kind::Worker) {
                pipeline_.OnWorkerNotification();
            }
            else if (kinds[index] == Kind::Frame) {
                pipeline_.OnFrameCreditAvailable();
            }
            else if (kinds[index] == Kind::Fence) pipeline_.OnGpuReady();
            else if (kinds[index] == Kind::Catalog) {
                OnCatalogComplete();
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
    Catalog catalog = asynchronous_catalog
                          ? BuildInitialCatalog(config_.initial_image)
                          : BuildCatalogFromList(config_.validation_file_list,
                                                 config_.initial_image);
    pipeline_.LoadInitialCatalog(std::move(catalog), !asynchronous_catalog);
    if (asynchronous_catalog) {
        catalog_loading_ = true;
        catalog_io_.emplace(config_.initial_image);
    }
}

void App::OnDirection(const int direction, const bool repeat) {
    pipeline_.Navigate(direction, repeat);
}

void App::OnDirectionReleased(const int direction) {
    pipeline_.ReleaseNavigation(direction);
}

void App::OnCatalogComplete() {
    if (!catalog_loading_ || !catalog_io_ || !catalog_io_->Advance()) return;
    validation_.MarkCatalogReady();
    Catalog catalog = catalog_io_->TakeCatalog();
    catalog_io_.reset();
    catalog_loading_ = false;
    pipeline_.CompleteCatalog(std::move(catalog));
}

void App::OnFrameReady(const std::size_t index) {
    validation_.OnFrameReady(index);
}

void App::OnFramePresented(const std::size_t index) {
    window_.SetImageTitle(pipeline_.PathFor(index));
    if (const auto result =
            validation_.OnFramePresented(index, pipeline_, window_)) {
        exit_code_ = *result;
        PostMessageW(window_.Handle(), WM_CLOSE, 0, 0);
    }
}

void App::ToggleFullscreen() {
    window_.ToggleFullscreen();
}

}  // namespace pv
