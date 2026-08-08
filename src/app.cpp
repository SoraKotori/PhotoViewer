#include "app.h"

#include "common.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <type_traits>

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

DWORD QueryIoPrefixGranularity(const HANDLE file) noexcept {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    DWORD granularity = std::max<DWORD>(4096, system.dwPageSize);
    FILE_STORAGE_INFO storage{};
    if (GetFileInformationByHandleEx(file, FileStorageInfo, &storage,
                                     sizeof(storage))) {
        granularity = std::max({granularity,
                                storage.LogicalBytesPerSector,
                                storage.PhysicalBytesPerSectorForAtomicity,
                                storage.PhysicalBytesPerSectorForPerformance,
                                storage.FileSystemEffectivePhysicalBytesPerSectorForAtomicity});
    }
    return granularity;
}

std::optional<std::size_t> DecodeStagingBytes(const PngInfo& png) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(png.width) * 4;
    if (png.decoded_bytes > std::numeric_limits<std::size_t>::max() - png.height) {
        return std::nullopt;
    }
    const std::size_t filtered_bytes = png.decoded_bytes + png.height;
    if (filtered_bytes > std::numeric_limits<std::size_t>::max() - row_bytes) {
        return std::nullopt;
    }
    return filtered_bytes + row_bytes;
}

LPARAM PackIoCompletion(const ULONG result,
                        const ULONG_PTR transferred) noexcept {
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(result) << 32) |
        static_cast<std::uint32_t>(transferred);
    return static_cast<LPARAM>(packed);
}

DWORD IoCompletionResult(const LPARAM completion) noexcept {
    return static_cast<DWORD>(static_cast<std::uint64_t>(completion) >> 32);
}

DWORD IoCompletionTransferred(const LPARAM completion) noexcept {
    return static_cast<DWORD>(static_cast<std::uint64_t>(completion));
}

DWORD IoRingResult(const HRESULT result) noexcept {
    if (SUCCEEDED(result)) return ERROR_SUCCESS;
    if (HRESULT_FACILITY(result) == FACILITY_WIN32) {
        return HRESULT_CODE(result);
    }
    return ERROR_GEN_FAILURE;
}

std::uint64_t FileTimeTicks(const FILETIME time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

template <typename Function>
Function LoadKernelFunction(const HMODULE module, const char* name) noexcept {
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

}  // namespace

App::App(Config config)
    : config_(std::move(config)) {
    io_completion_port_ = CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!io_completion_port_) ThrowLastError("CreateIoCompletionPort");
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        io_ring_api_.query_capabilities = LoadKernelFunction<
            decltype(io_ring_api_.query_capabilities)>(
                kernelbase, "QueryIoRingCapabilities");
        io_ring_api_.create = LoadKernelFunction<decltype(io_ring_api_.create)>(
            kernelbase, "CreateIoRing");
        io_ring_api_.submit = LoadKernelFunction<decltype(io_ring_api_.submit)>(
            kernelbase, "SubmitIoRing");
        io_ring_api_.close = LoadKernelFunction<decltype(io_ring_api_.close)>(
            kernelbase, "CloseIoRing");
        io_ring_api_.pop = LoadKernelFunction<decltype(io_ring_api_.pop)>(
            kernelbase, "PopIoRingCompletion");
        io_ring_api_.set_completion_event = LoadKernelFunction<
            decltype(io_ring_api_.set_completion_event)>(
                kernelbase, "SetIoRingCompletionEvent");
        io_ring_api_.build_read = LoadKernelFunction<
            decltype(io_ring_api_.build_read)>(kernelbase, "BuildIoRingReadFile");
        io_ring_api_.build_register_buffers = LoadKernelFunction<
            decltype(io_ring_api_.build_register_buffers)>(
                kernelbase, "BuildIoRingRegisterBuffers");
        const bool complete_api = io_ring_api_.query_capabilities &&
            io_ring_api_.create && io_ring_api_.submit && io_ring_api_.close &&
            io_ring_api_.pop && io_ring_api_.set_completion_event &&
            io_ring_api_.build_read && io_ring_api_.build_register_buffers;
        if (complete_api) {
            IORING_CAPABILITIES capabilities{};
            if (SUCCEEDED(io_ring_api_.query_capabilities(&capabilities))) {
                const UINT32 requested = static_cast<UINT32>(std::min<std::size_t>(
                    std::numeric_limits<UINT32>::max(),
                    config_.compressed_slot_count * 2 + 8));
                const UINT32 queue_size = std::min(
                    requested, capabilities.MaxSubmissionQueueSize);
                IORING_CREATE_FLAGS flags{};
                if (queue_size != 0 && SUCCEEDED(io_ring_api_.create(
                        capabilities.MaxVersion, flags, queue_size,
                        std::min(queue_size, capabilities.MaxCompletionQueueSize),
                        &io_ring_))) {
                    io_ring_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (!io_ring_event_ || FAILED(io_ring_api_.set_completion_event(
                            io_ring_, io_ring_event_))) {
                        if (io_ring_event_) CloseHandle(io_ring_event_);
                        io_ring_event_ = nullptr;
                        io_ring_api_.close(io_ring_);
                        io_ring_ = nullptr;
                    }
                }
            }
        }
    }
    resources_.slots.emplace(
        config_.compressed_slot_count, config_.staging_slot_count,
        config_.GpuSlotCount(), config_.compressed_budget_bytes,
        config_.staging_cache_bytes);
}

App::~App() {
    StopValidationNavigationTimer();
    catalog_io_.reset();
    decoders_.reset();
    if (graphics_device_ready_ && resources_.slots) {
        for (SlotId id = 0; id < resources_.slots->StagingCount(); ++id) {
            DecodeStaging& staging = resources_.slots->StagingAt(id).resource;
            if (staging.mapped) graphics_.UnmapDecodeStaging(staging);
        }
    }
    CancelAllIo();
    if (io_ring_event_) {
        CloseHandle(io_ring_event_);
        io_ring_event_ = nullptr;
    }
    if (io_completion_port_) {
        CloseHandle(io_completion_port_);
        io_completion_port_ = nullptr;
    }
}

int App::Run(const HINSTANCE instance, const int show_command) {
    if (!config_.validation_navigation.empty()) {
        validation_cold_started_ = std::chrono::steady_clock::now();
    }
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        ThrowLastError("Set main thread priority");
    }
    InitializeWindow(instance, show_command);
    validation_window_ready_ = std::chrono::steady_clock::now();
    if (!config_.initial_image.empty()) OpenInitialImage();
    validation_initial_io_submitted_ = std::chrono::steady_clock::now();
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    decoders_.emplace(config_.worker_count, work_queue_, completion_queue_,
                      *resources_.slots, window_);
    validation_decoders_ready_ = std::chrono::steady_clock::now();
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    graphics_.InitializeDirect3D(window_);
    graphics_device_ready_ = true;
    validation_graphics_device_ready_ = std::chrono::steady_clock::now();
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    graphics_.InitializeDirect2D();
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    graphics_.InitializeSwapChain();
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    graphics_.InitializeBackBufferTarget();
    graphics_ready_ = true;
    ProcessStartupCatalogCompletion();
    ProcessStartupIoCompletion();
    validation_graphics_ready_ = std::chrono::steady_clock::now();
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

void App::ProcessStartupIoCompletion() {
    DrainIoCompletions();
}

bool App::TryInitializeIoRing() {
    if (!io_ring_ || io_ring_buffers_registered_) {
        return io_ring_ && io_ring_buffers_registered_;
    }
    if (resources_.compressed_reservations.Capacity() == 0) return false;
    for (const ImageRecord& image : resources_.images) {
        if (image.io) return false;
    }
    std::size_t largest = 0;
    for (const CatalogItem& item : resources_.catalog.items) {
        if (!item.file_size_known || item.file_bytes == 0 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            return false;
        }
        largest = std::max(largest, static_cast<std::size_t>(item.file_bytes));
    }
    constexpr std::size_t registration_alignment = 64 * 1024;
    if (largest > std::numeric_limits<DWORD>::max() -
                      (registration_alignment - 1)) {
        return false;
    }
    const std::size_t registered_bytes =
        ((largest + registration_alignment - 1) / registration_alignment) *
        registration_alignment;
    // Register every physical compressed slot.  Restricting registration to
    // the current reservation capacity would let a previously allocated but
    // unregistered free slot win the best-fit search after a catalog merge.
    const std::size_t count = resources_.slots->CompressedCount();
    if (!resources_.slots->PreallocateCompressed(count, registered_bytes)) {
        return false;
    }
    std::vector<IORING_BUFFER_INFO> buffers;
    buffers.reserve(count);
    io_ring_buffer_slots_.clear();
    io_ring_buffer_slots_.reserve(count);
    for (SlotId id = 0; id < count; ++id) {
        const CompressedBuffer& buffer = resources_.slots->Compressed(id).resource;
        if (!buffer.data || buffer.allocation_size >
                                std::numeric_limits<UINT32>::max()) {
            io_ring_buffer_slots_.clear();
            return false;
        }
        buffers.push_back(IORING_BUFFER_INFO{
            buffer.data, static_cast<UINT32>(buffer.allocation_size)});
        io_ring_buffer_slots_.push_back(id);
    }
    constexpr UINT_PTR registration_user_data = 1;
    HRESULT result = io_ring_api_.build_register_buffers(
        io_ring_, static_cast<UINT32>(buffers.size()), buffers.data(),
        registration_user_data);
    if (SUCCEEDED(result)) {
        result = io_ring_api_.submit(io_ring_, 1, INFINITE, nullptr);
    }
    IORING_CQE completion{};
    if (SUCCEEDED(result)) result = io_ring_api_.pop(io_ring_, &completion);
    if (FAILED(result) || completion.UserData != registration_user_data ||
        FAILED(completion.ResultCode)) {
        io_ring_buffer_slots_.clear();
        io_ring_api_.close(io_ring_);
        io_ring_ = nullptr;
        return false;
    }
    io_ring_buffers_registered_ = true;
    return true;
}

void App::ProcessStartupCatalogCompletion() {
    DrainIoCompletions();
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
            } else if (wparam == 4 && validation_navigation_timer_active_) {
                InjectValidationNavigationStep();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window_, 1);
            KillTimer(window_, 2);
            KillTimer(window_, 3);
            KillTimer(window_, 4);
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
        HANDLE handles[4]{};
        enum class Kind { Frame, Fence, Io } kinds[4]{};
        DWORD count = 0;
        handles[count] = io_completion_port_;
        kinds[count++] = Kind::Io;
        if (io_ring_event_) {
            handles[count] = io_ring_event_;
            kinds[count++] = Kind::Io;
        }
        if (!resources_.frame_credit && graphics_.FrameWaitableObject()) {
            handles[count] = graphics_.FrameWaitableObject();
            kinds[count++] = Kind::Frame;
        }
        if ((!resources_.uploads.empty() || resources_.reading_gpu_texture_fence != 0) &&
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
            else if (kinds[index] == Kind::Fence) OnGpuComplete();
            else DrainIoCompletions();
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
    resources_.catalog = asynchronous_catalog
                             ? BuildInitialCatalog(config_.initial_image)
                             : BuildCatalogFromList(config_.validation_file_list,
                                                    config_.initial_image);
    resources_.generation++;
    resources_.images.clear();
    resources_.images.resize(resources_.catalog.items.size());
    for (std::size_t index = 0; index < resources_.images.size(); ++index) {
        auto& image = resources_.images[index];
        image.generation = resources_.generation;
        const CatalogItem& item = resources_.catalog.items[index];
        image.failed = item.file_size_known && item.file_bytes == 0;
    }
    resources_.navigation.Reset(resources_.catalog.initial_index,
                                resources_.catalog.items.size());
    InitializeReservations();
    resources_.redraw_pending = true;
    PumpPipeline();
    if (asynchronous_catalog) {
        catalog_loading_ = true;
        catalog_io_.emplace(config_.initial_image, io_completion_port_);
    }
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
    resources_.reservation_plan_dirty = true;
    PumpPipeline();
}

void App::OnDirectionReleased(const int direction) {
    if (resources_.images.empty()) return;
    resources_.navigation.Release(direction);
    resources_.reservation_plan_dirty = true;
    PumpPipeline();
}

void App::OnCatalogComplete() {
    if (!catalog_loading_ || !catalog_io_ || !catalog_io_->Advance()) return;
    Catalog catalog = catalog_io_->TakeCatalog();
    catalog_io_.reset();
    catalog_loading_ = false;
    if (catalog.items.empty()) {
        throw std::runtime_error("asynchronous catalog returned no images");
    }

    const std::size_t initial = catalog.initial_index;
    catalog.items[initial] = std::move(resources_.catalog.items[0]);
    ImageRecord initial_image = std::move(resources_.images[0]);
    initial_image.compressed_reservation = kInvalidReservation;
    initial_image.staging_reservation = kInvalidReservation;
    initial_image.gpu_texture_reservation = kInvalidReservation;

    std::vector<ImageRecord> images(catalog.items.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        images[index].generation = resources_.generation;
        images[index].failed = catalog.items[index].file_size_known &&
                               catalog.items[index].file_bytes == 0;
    }
    images[initial] = std::move(initial_image);
    if (images[initial].io) images[initial].io->index = initial;
    if (images[initial].compressed_slot != kInvalidSlot) {
        resources_.slots->Compressed(images[initial].compressed_slot).image = initial;
    }
    if (images[initial].staging_slot != kInvalidSlot) {
        resources_.slots->StagingAt(images[initial].staging_slot).image = initial;
    }

    resources_.catalog = std::move(catalog);
    resources_.images = std::move(images);
    resources_.navigation.Reset(initial, resources_.images.size());
    InitializeReservations();
    resources_.redraw_pending = true;

    for (IoRequest* const io : deferred_catalog_io_) CompleteIoRequest(io);
    deferred_catalog_io_.clear();
    PumpPipeline();
}

void App::DrainIoCompletions() {
    bool drained = false;
    if (io_ring_) {
        IORING_CQE completion{};
        while (io_ring_api_.pop(io_ring_, &completion) == S_OK) {
            drained = true;
            constexpr UINT_PTR tag_mask = 3;
            const UINT_PTR tag = completion.UserData & tag_mask;
            auto* request = reinterpret_cast<IoRequest*>(
                completion.UserData & ~tag_mask);
            const LPARAM packed = PackIoCompletion(
                IoRingResult(completion.ResultCode), completion.Information);
            if (tag == 1) OnIoHeaderReady(request, packed);
            else if (tag == 2) OnIoComplete(request, packed);
        }
    }
    for (;;) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL success = GetQueuedCompletionStatus(
            io_completion_port_, &transferred, &key, &overlapped, 0);
        if (!overlapped) {
            if (!success && GetLastError() == WAIT_TIMEOUT) break;
            if (!success) ThrowLastError("GetQueuedCompletionStatus");
            continue;
        }
        drained = true;
        if (key == kCatalogIoCompletionKey) {
            OnCatalogComplete();
            continue;
        }
        auto* request = reinterpret_cast<IoRequest*>(key);
        const DWORD result = success ? ERROR_SUCCESS : GetLastError();
        const LPARAM completion = PackIoCompletion(result, transferred);
        if (overlapped == &request->header_overlapped) {
            OnIoHeaderReady(request, completion);
        } else {
            OnIoComplete(request, completion);
        }
    }
    if (drained && !catalog_loading_) PumpPipeline();
}

void App::OnIoHeaderReady(IoRequest* request,
                          const LPARAM completion) {
    if (!request || request->index >= resources_.images.size()) return;
    ImageRecord& image = resources_.images[request->index];
    if (!image.io || image.io != request ||
        request->generation != image.generation || request->header_completed) {
        return;
    }
    const DWORD transferred = IoCompletionTransferred(completion);
    request->header_result = IoCompletionResult(completion);
    request->header_transferred = transferred;
    request->header_completed = true;
    if (request->header_result == ERROR_SUCCESS && transferred >= 24) {
        const auto header = ParsePngHeader(std::span<const std::byte>(
            request->destination, 24));
        if (header) {
            CatalogItem& item = resources_.catalog.items[request->index];
            item.png = *header;
            item.header_valid = true;
            PrepareStagingForImage(request->index);
        }
    }
    if (!request->content_submitted || request->content_completed) {
        if (!request->content_submitted) {
            request->result = ERROR_SUCCESS;
            request->transferred = 0;
        }
        if (catalog_loading_) {
            if (std::ranges::find(deferred_catalog_io_, request) ==
                deferred_catalog_io_.end()) {
                deferred_catalog_io_.push_back(request);
            }
        } else {
            CompleteIoRequest(request);
        }
    }
}

void App::OnIoComplete(IoRequest* request, LPARAM completion) {
    if (request && request->index < resources_.images.size()) {
        ImageRecord& image = resources_.images[request->index];
        if (image.io == request && !request->content_completed) {
            request->result = IoCompletionResult(completion);
            request->transferred = IoCompletionTransferred(completion);
            request->content_completed = true;
            if (!request->split_header || request->header_completed) {
                if (catalog_loading_) {
                    if (std::ranges::find(deferred_catalog_io_, request) ==
                        deferred_catalog_io_.end()) {
                        deferred_catalog_io_.push_back(request);
                    }
                } else {
                    CompleteIoRequest(request);
                }
            }
        }
    }
}

void App::CompleteIoRequest(IoRequest* const request) {
    if (!request || request->index >= resources_.images.size()) return;
    ImageRecord& image = resources_.images[request->index];
    if (!image.io || image.io != request) return;

    CloseHandle(request->file);
    request->file = INVALID_HANDLE_VALUE;

    const DWORD io_result = request->result;
    const std::size_t transferred = request->transferred;
    const SlotId compressed_slot = request->compressed_slot;
    CompressedSlot& slot = resources_.slots->Compressed(compressed_slot);
    const std::size_t allocation = slot.resource.size;
    const bool header_success = !request->split_header ||
        (request->header_result == ERROR_SUCCESS &&
         request->header_transferred == request->prefix_bytes);
    const std::size_t expected_content = request->split_header
                                             ? allocation - request->prefix_bytes
                                             : allocation;
    const bool success = header_success && io_result == ERROR_SUCCESS &&
                         transferred == expected_content;
    const bool current = request->generation == image.generation;
    image.io = nullptr;

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
    CompletionQueue::Batch batch = completion_queue_.DrainAll();
    for (ReleasedInput& input : batch.released_inputs) {
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
    for (DecodeResult& result : batch.results) {
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
        image.work_active = false;
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
    PumpPipeline();
}

void App::OnGpuComplete() {
    const UINT64 completed = graphics_.CompletedFenceValue();
    if (resources_.reading_gpu_texture_fence != 0 &&
        resources_.reading_gpu_texture_fence <= completed) {
        if (resources_.reading_gpu_texture_slot != kInvalidSlot) {
            GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
                resources_.reading_gpu_texture_slot);
            if (gpu_texture.state == GpuTextureSlotState::Reading) {
                gpu_texture.state = gpu_texture.reserved_frame == gpu_texture.content_frame
                                   ? GpuTextureSlotState::Readable
                                   : GpuTextureSlotState::Writable;
            }
        }
        resources_.reading_gpu_texture_slot = kInvalidSlot;
        resources_.reading_gpu_texture_fence = 0;
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
        GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
            ticket.gpu_texture_slot);
        const bool keep_gpu = ticket.generation == image.generation &&
            ReservationActive(resources_.gpu_texture_reservations,
                              image.gpu_texture_reservation, ticket.index) &&
            image.gpu_texture_reservation == ticket.gpu_texture_slot &&
            gpu_texture.reserved_frame == ticket.index;
        if (keep_gpu) {
            graphics_.FinishUpload(gpu_texture.resource);
            gpu_texture.content_frame = ticket.index;
            gpu_texture.state = GpuTextureSlotState::Readable;
            RecordValidationReady(ticket.index);
        } else {
            gpu_texture.content_frame = ticket.index;
            gpu_texture.state = GpuTextureSlotState::Writable;
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
    validation_pump_count_ = 0;
    validation_pump_nanoseconds_ = 0;
    validation_plan_count_ = 0;
    validation_plan_nanoseconds_ = 0;
    validation_reconcile_count_ = 0;
    validation_reconcile_nanoseconds_ = 0;
    validation_dispatch_nanoseconds_ = 0;
    validation_submit_reads_nanoseconds_ = 0;
    validation_acquire_compressed_nanoseconds_ = 0;
    validation_open_file_nanoseconds_ = 0;
    validation_associate_iocp_nanoseconds_ = 0;
    validation_read_file_nanoseconds_ = 0;
    validation_read_file_synchronous_count_ = 0;
    validation_read_file_pending_count_ = 0;
    validation_submit_uploads_nanoseconds_ = 0;
    validation_try_present_nanoseconds_ = 0;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        validation_main_kernel_started_ = FileTimeTicks(kernel);
        validation_main_user_started_ = FileTimeTicks(user);
    }
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
        resources_.reservation_plan_dirty = true;
        validation_navigation_injection_finished_ = std::chrono::steady_clock::now();
        PumpPipeline();
        return;
    }
    InjectValidationNavigationStep();
    if (validation_navigation_cursor_ < config_.validation_navigation.size()) {
        if (SetTimer(window_, 4, config_.validation_navigation_interval_ms,
                     nullptr) == 0) {
            ThrowLastError("SetTimer validation navigation");
        }
        validation_navigation_timer_active_ = true;
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
    resources_.reservation_plan_dirty = true;
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
    if (!validation_navigation_timer_active_) return;
    KillTimer(window_, 4);
    validation_navigation_timer_active_ = false;
}

void App::RecordValidationPresentation(const std::size_t index) {
    if (validation_cold_started_ == std::chrono::steady_clock::time_point{} ||
        (!validation_presented_indices_.empty() &&
         validation_presented_indices_.back() == index)) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - validation_cold_started_);
    validation_presented_indices_.push_back(index);
    validation_presented_nanoseconds_.push_back(
        static_cast<std::uint64_t>(elapsed.count()));
}

void App::RecordValidationReady(const std::size_t index) {
    if (validation_cold_started_ == std::chrono::steady_clock::time_point{} ||
        std::ranges::find(validation_ready_indices_, index) !=
            validation_ready_indices_.end()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - validation_cold_started_);
    validation_ready_indices_.push_back(index);
    validation_ready_nanoseconds_.push_back(
        static_cast<std::uint64_t>(elapsed.count()));
}

void App::WriteValidationReport(const std::string_view phase, const bool truncate) {
    if (config_.validation_report.empty()) return;
    std::ofstream output(config_.validation_report,
                         std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (!output) return;
    const auto startup_nanoseconds = [&](const auto time) {
        if (validation_cold_started_ == std::chrono::steady_clock::time_point{} ||
            time == std::chrono::steady_clock::time_point{}) {
            return std::int64_t{0};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   time - validation_cold_started_).count();
    };
    constexpr std::array names{"Outside", "WaitingIo", "IoInFlight", "CompressedReady",
                               "DecodeQueued", "DecodedStagingAvailable", "Uploading",
                               "PresentationTextureAvailable",
                               "CancelPending", "Failed"};
    std::array<std::size_t, names.size()> counts{};
    for (const ImageRecord& image : resources_.images) {
        const std::size_t stage = static_cast<std::size_t>(StageOf(image));
        if (stage < counts.size()) ++counts[stage];
    }
    output << "phase=" << phase << '\n'
           << "startup_window_ready_nanoseconds="
           << startup_nanoseconds(validation_window_ready_) << '\n'
           << "startup_initial_io_submitted_nanoseconds="
           << startup_nanoseconds(validation_initial_io_submitted_) << '\n'
           << "startup_decoders_ready_nanoseconds="
           << startup_nanoseconds(validation_decoders_ready_) << '\n'
           << "startup_graphics_device_ready_nanoseconds="
           << startup_nanoseconds(validation_graphics_device_ready_) << '\n'
           << "startup_graphics_ready_nanoseconds="
           << startup_nanoseconds(validation_graphics_ready_) << '\n'
           << "io_prefix_granularity=" << io_prefix_granularity_ << '\n';
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
    output << "ActiveReadableGpuTexture_indices=";
    bool first_readable_gpu_texture = true;
    for (std::size_t index = 0; index < resources_.images.size(); ++index) {
        if (!HasReadableGpuTexture(index)) continue;
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
           << "gpu_texture_reservations="
           << resources_.gpu_texture_reservations.AssignedCount() << '/'
           << resources_.gpu_texture_reservations.Capacity() << '\n'
           << "compressed_retiring_reservations="
           << retiring_count(resources_.compressed_reservations) << '\n'
           << "staging_retiring_reservations="
           << retiring_count(resources_.staging_reservations) << '\n'
           << "gpu_texture_retiring_reservations="
           << retiring_count(resources_.gpu_texture_reservations) << '\n'
           << "retiring_reservations="
           << retiring_count(resources_.compressed_reservations) +
                  retiring_count(resources_.staging_reservations) +
                  retiring_count(resources_.gpu_texture_reservations)
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
           << "validation_ready_count="
           << validation_ready_indices_.size() << '\n'
           << "validation_ready_indices=";
    for (std::size_t index = 0; index < validation_ready_indices_.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_ready_indices_[index];
    }
    output << '\n' << "validation_ready_nanoseconds=";
    for (std::size_t index = 0; index < validation_ready_nanoseconds_.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_ready_nanoseconds_[index];
    }
    output << '\n'
           << "validation_presented_count="
           << validation_presented_indices_.size() << '\n'
           << "validation_presented_indices=";
    for (std::size_t index = 0; index < validation_presented_indices_.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_presented_indices_[index];
    }
    output << '\n' << "validation_presented_nanoseconds=";
    for (std::size_t index = 0; index < validation_presented_nanoseconds_.size(); ++index) {
        if (index != 0) output << ',';
        output << validation_presented_nanoseconds_[index];
    }
    output << '\n'
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
    FILETIME main_created{};
    FILETIME main_exited{};
    FILETIME main_kernel{};
    FILETIME main_user{};
    const bool have_main_times = GetThreadTimes(
        GetCurrentThread(), &main_created, &main_exited, &main_kernel, &main_user);
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
           << "pipeline_pump_count=" << validation_pump_count_ << '\n'
           << "pipeline_pump_nanoseconds=" << validation_pump_nanoseconds_ << '\n'
           << "reservation_plan_count=" << validation_plan_count_ << '\n'
           << "reservation_plan_nanoseconds=" << validation_plan_nanoseconds_ << '\n'
           << "reservation_reconcile_count=" << validation_reconcile_count_ << '\n'
           << "reservation_reconcile_nanoseconds="
           << validation_reconcile_nanoseconds_ << '\n'
           << "dispatch_decode_nanoseconds="
           << validation_dispatch_nanoseconds_ << '\n'
           << "submit_reads_nanoseconds="
           << validation_submit_reads_nanoseconds_ << '\n'
           << "acquire_compressed_nanoseconds="
           << validation_acquire_compressed_nanoseconds_ << '\n'
           << "open_file_nanoseconds="
           << validation_open_file_nanoseconds_ << '\n'
           << "associate_iocp_nanoseconds="
           << validation_associate_iocp_nanoseconds_ << '\n'
           << "read_file_nanoseconds="
           << validation_read_file_nanoseconds_ << '\n'
           << "read_file_synchronous_count="
           << validation_read_file_synchronous_count_ << '\n'
           << "read_file_pending_count="
           << validation_read_file_pending_count_ << '\n'
           << "submit_uploads_nanoseconds="
           << validation_submit_uploads_nanoseconds_ << '\n'
           << "try_present_nanoseconds="
           << validation_try_present_nanoseconds_ << '\n'
           << "main_thread_kernel_nanoseconds="
           << (have_main_times
                   ? (FileTimeTicks(main_kernel) - validation_main_kernel_started_) * 100ULL
                   : 0ULL)
           << '\n'
           << "main_thread_user_nanoseconds="
           << (have_main_times
                   ? (FileTimeTicks(main_user) - validation_main_user_started_) * 100ULL
                   : 0ULL)
           << '\n'
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
    const bool measure = validation_navigation_started_ !=
                         std::chrono::steady_clock::time_point{};
    const auto begin = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    const auto measured = [&](auto&& operation, std::uint64_t& nanoseconds) {
        if (!measure) return operation();
        const auto operation_begin = std::chrono::steady_clock::now();
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(operation)>>) {
            operation();
            nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - operation_begin).count());
        } else {
            auto result = operation();
            nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - operation_begin).count());
            return result;
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        ReconcileReservations();
        measured([&] { DispatchDecodes(); }, validation_dispatch_nanoseconds_);
        measured([&] { SubmitReads(); }, validation_submit_reads_nanoseconds_);
        measured([&] { SubmitUploads(); }, validation_submit_uploads_nanoseconds_);
        if (!measured([&] { return TryPresent(); },
                      validation_try_present_nanoseconds_)) {
            break;
        }
    }
    if (measure) {
        ++validation_pump_count_;
        validation_pump_nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }
}

PipelineStage App::StageOf(const ImageRecord& image) const noexcept {
    const std::size_t frame = static_cast<std::size_t>(
        &image - resources_.images.data());
    if (image.failed) return PipelineStage::Failed;
    if (image.gpu_texture_reservation != kInvalidReservation &&
        image.gpu_texture_reservation < resources_.slots->GpuTextureCount()) {
        const GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
            image.gpu_texture_reservation);
        if (gpu_texture.reserved_frame == frame) {
            switch (gpu_texture.state) {
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
            case StagingSlotState::Prepared:
                break;
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
    const std::size_t gpu_texture_capacity = fixed_capacity(
        config_.GpuSlotCount(), config_.gpu_cache_bytes,
        decoded_8k_bytes);

    resources_.compressed_reservations.Reset(compressed_capacity);
    resources_.staging_reservations.Reset(staging_capacity);
    resources_.gpu_texture_reservations.Reset(gpu_texture_capacity);
    resources_.priority_order.clear();
    resources_.gpu_texture_desired.clear();
    resources_.reservation_plan_dirty = true;
    for (SlotId id = 0; id < gpu_texture_capacity; ++id) {
        if (!resources_.slots->ActivateGpuTexture(id)) {
            throw std::logic_error("failed to activate GPU Texture slot");
        }
    }
}

bool App::ReservationActive(const ReservationTable& table,
                            const ReservationId id,
                            const std::size_t frame) const noexcept {
    return table.IsActive(id) && table.At(id).frame == frame;
}

bool App::HasReadableGpuTexture(const std::size_t frame) const noexcept {
    if (frame >= resources_.images.size()) return false;
    const ImageRecord& image = resources_.images[frame];
    if (!ReservationActive(resources_.gpu_texture_reservations,
                           image.gpu_texture_reservation, frame)) {
        return false;
    }
    const GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
        image.gpu_texture_reservation);
    return gpu_texture.reserved_frame == frame && gpu_texture.content_frame == frame &&
           (gpu_texture.state == GpuTextureSlotState::Readable ||
            gpu_texture.state == GpuTextureSlotState::Reading);
}

bool App::CancelQueuedDecode(const std::size_t frame) {
    if (frame >= resources_.images.size()) return false;
    ImageRecord& image = resources_.images[frame];
    if (!image.work_active || image.staging_slot == kInvalidSlot) return false;
    WorkToken& work_token = resources_.slots->WorkTokenAt(image.staging_slot);
    DecodeWork cancelled;
    if (!work_queue_.TryCancel(&work_token, cancelled)) {
        return false;
    }

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
    image.work_active = false;
    return true;
}

void App::RebuildReservationPlan() {
    const bool measure = validation_navigation_started_ !=
                         std::chrono::steady_clock::time_point{};
    const auto begin = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
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

    resources_.gpu_texture_desired.clear();
    const std::size_t gpu_texture_capacity = resources_.gpu_texture_reservations.Capacity();
    resources_.gpu_texture_desired.reserve(gpu_texture_capacity);
    const std::size_t forward_capacity = std::min(
        config_.gpu_forward_slot_count, gpu_texture_capacity);
    const std::size_t current = resources_.navigation.CurrentIndex();
    append_unique(resources_.gpu_texture_desired, current, gpu_texture_capacity);
    for (const std::size_t frame : resources_.priority_order) {
        if (resources_.gpu_texture_desired.size() >= forward_capacity) break;
        if (!resources_.images[frame].failed) {
            append_unique(resources_.gpu_texture_desired, frame, gpu_texture_capacity);
        }
    }
    const std::size_t reverse_capacity = std::min(
        config_.gpu_reverse_slot_count,
        gpu_texture_capacity - resources_.gpu_texture_desired.size());
    int direction = resources_.navigation.PreferredDirection();
    if (direction == 0) direction = 1;
    for (std::size_t distance = 1; distance <= reverse_capacity; ++distance) {
        if (direction > 0) {
            if (distance > current) break;
            append_unique(resources_.gpu_texture_desired, current - distance,
                          gpu_texture_capacity);
        } else {
            if (distance >= resources_.images.size() - current) break;
            append_unique(resources_.gpu_texture_desired, current + distance,
                          gpu_texture_capacity);
        }
    }
    for (const std::size_t frame : resources_.priority_order) {
        if (!resources_.images[frame].failed) {
            append_unique(resources_.gpu_texture_desired, frame, gpu_texture_capacity);
        }
    }

    // Every reserved GPU Texture must be backed by the upstream pipeline,
    // including the configured reverse-direction set.  Put those frames ahead
    // of speculative work so a direction change reorders GPU Texture,
    // staging, compressed I/O, and queued decodes as one pipeline operation.
    std::vector<std::size_t> upstream_order;
    upstream_order.reserve(resources_.priority_order.size());
    for (const std::size_t frame : resources_.gpu_texture_desired) {
        append_unique(upstream_order, frame, resources_.images.size());
    }
    for (const std::size_t frame : resources_.priority_order) {
        append_unique(upstream_order, frame, resources_.images.size());
    }
    resources_.priority_order = std::move(upstream_order);
    work_queue_.Reorder(resources_.priority_order);
    resources_.reservation_plan_dirty = false;
    if (measure) {
        ++validation_plan_count_;
        validation_plan_nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }
}

void App::ReconcileReservations() {
    const bool measure = validation_navigation_started_ !=
                         std::chrono::steady_clock::time_point{};
    const auto begin = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    if (!resources_.reservation_plan_dirty) {
        for (const std::size_t frame : resources_.gpu_texture_desired) {
            if (resources_.images[frame].failed) {
                resources_.reservation_plan_dirty = true;
                break;
            }
        }
    }
    if (resources_.reservation_plan_dirty) RebuildReservationPlan();

    const auto append_unique = [](std::vector<std::size_t>& frames,
                                  const std::size_t frame,
                                  const std::size_t capacity) {
        if (frames.size() < capacity &&
            std::find(frames.begin(), frames.end(), frame) == frames.end()) {
            frames.push_back(frame);
        }
    };
    const std::vector<std::size_t>& gpu_texture_desired = resources_.gpu_texture_desired;

    resources_.gpu_texture_reservations.Reconcile(
        gpu_texture_desired,
        [&](const ReservationId id, const std::size_t) {
            const GpuTextureSlotState state = resources_.slots->GpuTextureAt(id).state;
            return state == GpuTextureSlotState::Writable ||
                   state == GpuTextureSlotState::Readable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.gpu_texture_reservation == id) {
                image.gpu_texture_reservation = kInvalidReservation;
            }
            GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(id);
            gpu_texture.reserved_frame = kInvalidFrame;
            gpu_texture.state = GpuTextureSlotState::Writable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            image.gpu_texture_reservation = id;
            GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(id);
            gpu_texture.reserved_frame = frame;
            gpu_texture.generation = image.generation;
            gpu_texture.state = gpu_texture.content_frame == frame && gpu_texture.resource.bitmap
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
        bool gpu_texture_complete = false;
        if (image.gpu_texture_reservation != kInvalidReservation) {
            const GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
                image.gpu_texture_reservation);
            gpu_texture_complete = gpu_texture.reserved_frame == frame &&
                              gpu_texture.state != GpuTextureSlotState::Writable;
        }
        if (!gpu_texture_complete) append_unique(
            staging_desired, frame, resources_.staging_reservations.Capacity());
    }
    resources_.staging_reservations.Reconcile(
        staging_desired,
        [&](const ReservationId, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.staging_slot == kInvalidSlot) return true;
            StagingSlot& slot = resources_.slots->StagingAt(image.staging_slot);
            if (slot.state == StagingSlotState::Prepared ||
                slot.state == StagingSlotState::DecodedPixelsAvailable) {
                return true;
            }
            if (slot.state == StagingSlotState::DecodeOutputMapped ||
                slot.state == StagingSlotState::CancellationPending) {
                if (CancelQueuedDecode(frame)) return true;
                if (image.work_active) {
                    resources_.slots->WorkTokenAt(image.staging_slot).claim.store(
                        WorkClaim::Cancelled, std::memory_order_release);
                }
                slot.state = StagingSlotState::CancellationPending;
            }
            return false;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = resources_.images[frame];
            if (image.staging_slot != kInvalidSlot) {
                StagingSlot& slot = resources_.slots->StagingAt(image.staging_slot);
                if (slot.state == StagingSlotState::Prepared ||
                    slot.state == StagingSlotState::DecodedPixelsAvailable) {
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
                // A submitted kernel I/O owns the compressed slot until its
                // completion packet arrives.  Mark the reservation retiring,
                // but do not cancel and resubmit the same pages when direction
                // input oscillates.  If the frame becomes wanted again before
                // completion, Reconcile clears retiring and the result is kept.
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
            const CatalogItem& item = resources_.catalog.items[frame];
            image.failed = item.file_size_known && item.file_bytes == 0;
        },
        ReservationTable::FirstFree);
    if (measure) {
        ++validation_reconcile_count_;
        validation_reconcile_nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }
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

void App::PrepareStagingForImage(const std::size_t index) {
    if (!graphics_device_ready_ || index >= resources_.images.size()) return;
    ImageRecord& image = resources_.images[index];
    if (image.failed || image.staging_slot != kInvalidSlot ||
        !ReservationActive(resources_.staging_reservations,
                           image.staging_reservation, index)) {
        return;
    }
    const CatalogItem& item = resources_.catalog.items[index];
    if (!item.header_valid) return;
    const std::optional<std::size_t> staging_bytes =
        DecodeStagingBytes(item.png);
    if (!staging_bytes || *staging_bytes == 0 ||
        *staging_bytes > config_.staging_cache_bytes) {
        return;
    }
    const SlotId staging_slot = resources_.slots->AcquireStaging(
        *staging_bytes, index, image.generation);
    if (staging_slot == kInvalidSlot) return;
    try {
        graphics_.PrepareDecodeStaging(
            resources_.slots->StagingAt(staging_slot).resource,
            item.png.width, item.png.height);
    } catch (...) {
        resources_.slots->ReleaseStaging(staging_slot);
        throw;
    }
    image.staging_slot = staging_slot;
}

void App::SubmitReads() {
    const bool measure = validation_navigation_started_ !=
                         std::chrono::steady_clock::time_point{};
    const auto measured = [&](auto&& operation, std::uint64_t& nanoseconds) {
        if (!measure) return operation();
        const auto begin = std::chrono::steady_clock::now();
        auto result = operation();
        nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        return result;
    };
    if (io_ring_ && !io_ring_buffers_registered_) {
        bool active = false;
        for (const ImageRecord& image : resources_.images) {
            active = active || image.io != nullptr;
        }
        if (active) return;
        if (!TryInitializeIoRing() && io_ring_) {
            io_ring_api_.close(io_ring_);
            io_ring_ = nullptr;
        }
    }
    const bool use_io_ring = io_ring_ && io_ring_buffers_registered_;
    UINT32 io_ring_entries = 0;
    for (const std::size_t index : PrioritizedCandidates(PipelineStage::WaitingIo)) {
        ImageRecord& image = resources_.images[index];
        CatalogItem& item = resources_.catalog.items[index];
        HANDLE opened_file = measured(
            [&] {
                return CreateFileW(
                    item.path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
            },
            validation_open_file_nanoseconds_);
        if (opened_file == INVALID_HANDLE_VALUE) {
            image.failed = true;
            continue;
        }
        if (!item.file_size_known) {
            LARGE_INTEGER file_size{};
            if (!GetFileSizeEx(opened_file, &file_size) ||
                file_size.QuadPart <= 0 ||
                static_cast<unsigned long long>(file_size.QuadPart) >
                    std::numeric_limits<DWORD>::max()) {
                CloseHandle(opened_file);
                image.failed = true;
                continue;
            }
            item.file_bytes = static_cast<std::uint64_t>(file_size.QuadPart);
            item.file_size_known = true;
        }
        if (item.file_bytes <= 24 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        const std::size_t compressed = static_cast<std::size_t>(item.file_bytes);
        if (compressed > config_.compressed_budget_bytes) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        if (resources_.compressed_bytes >
                config_.compressed_budget_bytes - compressed) {
            CloseHandle(opened_file);
            continue;
        }
        if (io_prefix_granularity_ == 0) {
            io_prefix_granularity_ = QueryIoPrefixGranularity(opened_file);
        }
        const std::size_t granularity = io_prefix_granularity_;
        if (granularity == 0 ||
            compressed > std::numeric_limits<DWORD>::max() - (granularity - 1)) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        const std::size_t transfer_bytes =
            ((compressed + granularity - 1) / granularity) * granularity;
        const SlotId compressed_slot = measured(
            [&] {
                return resources_.slots->AcquireCompressed(
                    transfer_bytes, index, image.generation);
            },
            validation_acquire_compressed_nanoseconds_);
        if (compressed_slot == kInvalidSlot) {
            CloseHandle(opened_file);
            continue;
        }
        image.compressed_slot = compressed_slot;
        CompressedSlot& compressed_state =
            resources_.slots->Compressed(compressed_slot);
        IoRequest& request = compressed_state.io;
        request.Reset();
        request.window = window_;
        request.index = index;
        request.generation = image.generation;
        request.compressed_slot = compressed_slot;
        request.file = opened_file;
        UINT32 registered_buffer = 0;
        if (use_io_ring) {
            const auto found = std::find(io_ring_buffer_slots_.begin(),
                                         io_ring_buffer_slots_.end(),
                                         compressed_slot);
            if (found == io_ring_buffer_slots_.end()) {
                CloseHandle(request.file);
                request.file = INVALID_HANDLE_VALUE;
                resources_.slots->ReleaseCompressed(compressed_slot);
                image.compressed_slot = kInvalidSlot;
                continue;
            }
            registered_buffer = static_cast<UINT32>(
                std::distance(io_ring_buffer_slots_.begin(), found));
            request.using_io_ring = true;
        } else {
            const HANDLE associated_port = measured(
                [&] {
                    return CreateIoCompletionPort(
                        request.file, io_completion_port_,
                        reinterpret_cast<ULONG_PTR>(&request), 0);
                },
                validation_associate_iocp_nanoseconds_);
            if (associated_port != io_completion_port_) {
                CloseHandle(request.file);
                request.file = INVALID_HANDLE_VALUE;
                resources_.slots->ReleaseCompressed(compressed_slot);
                image.compressed_slot = kInvalidSlot;
                image.failed = true;
                continue;
            }
        }

        resources_.compressed_bytes += compressed;
        image.io = &request;
        CompressedBuffer& buffer = compressed_state.resource;
        buffer.size = compressed;
        image.io->destination = buffer.data;
        image.io->byte_count = static_cast<DWORD>(compressed);
        image.io->transfer_count = static_cast<DWORD>(transfer_bytes);
        image.io->split_header = !item.header_valid;
        const auto submit_read = [&](const DWORD buffer_offset,
                                     const DWORD bytes,
                                     const std::uint64_t file_offset,
                                     const UINT_PTR tag,
                                     OVERLAPPED* overlapped) {
            if (!use_io_ring) {
                return ReadFile(image.io->file, buffer.data + buffer_offset,
                                bytes, nullptr, overlapped);
            }
            const UINT_PTR user_data =
                reinterpret_cast<UINT_PTR>(image.io) | tag;
            const HRESULT result = io_ring_api_.build_read(
                io_ring_, IoRingHandleRefFromHandle(image.io->file),
                IoRingBufferRefFromIndexAndOffset(registered_buffer,
                                                  buffer_offset),
                bytes, file_offset, user_data, IOSQE_FLAGS_NONE);
            if (FAILED(result)) {
                SetLastError(IoRingResult(result));
                return FALSE;
            }
            ++io_ring_entries;
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        };
        bool initial_submission_failed = false;
        if (image.io->split_header) {
            image.io->prefix_bytes = std::min(
                image.io->byte_count, io_prefix_granularity_);
            const DWORD prefix_transfer = std::min(
                image.io->transfer_count, io_prefix_granularity_);
            const BOOL prefix_submitted = measured(
                [&] {
                    return submit_read(0, prefix_transfer, 0, 1,
                                       &image.io->header_overlapped);
                },
                validation_read_file_nanoseconds_);
            ++(prefix_submitted ? validation_read_file_synchronous_count_
                                : validation_read_file_pending_count_);
            if (!prefix_submitted && GetLastError() != ERROR_IO_PENDING) {
                initial_submission_failed = true;
            } else if (prefix_transfer < image.io->transfer_count) {
                image.io->content_submitted = true;
                image.io->content_overlapped.Offset = prefix_transfer;
                const BOOL content_submitted = measured(
                    [&] {
                        return submit_read(
                            prefix_transfer,
                            image.io->transfer_count - prefix_transfer,
                            prefix_transfer, 2,
                            &image.io->content_overlapped);
                    },
                    validation_read_file_nanoseconds_);
                ++(content_submitted ? validation_read_file_synchronous_count_
                                     : validation_read_file_pending_count_);
                if (!content_submitted && GetLastError() != ERROR_IO_PENDING) {
                    const DWORD error = GetLastError();
                    image.io->result = error;
                    image.io->transferred = 0;
                    image.io->content_completed = true;
                }
            }
        } else {
            image.io->content_submitted = true;
            const BOOL content_submitted = measured(
                [&] {
                    return submit_read(0, image.io->transfer_count, 0, 2,
                                       &image.io->content_overlapped);
                },
                validation_read_file_nanoseconds_);
            ++(content_submitted ? validation_read_file_synchronous_count_
                                 : validation_read_file_pending_count_);
            if (!content_submitted && GetLastError() != ERROR_IO_PENDING) {
                initial_submission_failed = true;
            }
        }
        if (initial_submission_failed) {
            CloseHandle(image.io->file);
            resources_.slots->ReleaseCompressed(image.io->compressed_slot);
            image.io = nullptr;
            image.compressed_slot = kInvalidSlot;
            resources_.compressed_bytes -= compressed;
            image.failed = true;
        }
    }
    if (use_io_ring && io_ring_entries != 0) {
        const HRESULT result = measured(
            [&] { return io_ring_api_.submit(io_ring_, 0, 0, nullptr); },
            validation_read_file_nanoseconds_);
        if (FAILED(result)) {
            throw std::runtime_error("SubmitIoRing failed (HRESULT " +
                                     std::to_string(
                                         static_cast<unsigned long>(result)) +
                                     ")");
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
        const std::optional<std::size_t> staging_bytes =
            DecodeStagingBytes(item.png);
        if (!staging_bytes || *staging_bytes == 0 ||
            *staging_bytes > config_.staging_cache_bytes) {
            ReleaseCompressed(image);
            image.failed = true;
            continue;
        }
        PrepareStagingForImage(index);
        const SlotId staging_slot = image.staging_slot;
        if (staging_slot == kInvalidSlot) continue;
        StagingSlot& staging_state = resources_.slots->StagingAt(staging_slot);
        if (staging_state.state != StagingSlotState::Prepared ||
            staging_state.image != index ||
            staging_state.generation != image.generation) {
            throw std::logic_error("invalid prepared staging slot");
        }
        graphics_.MapDecodeStaging(staging_state.resource,
                                   item.png.width, item.png.height,
                                   item.png.decoded_bytes);
        staging_state.state = StagingSlotState::DecodeOutputMapped;
        WorkToken& work_token = resources_.slots->WorkTokenAt(staging_slot);
        work_token.claim.store(WorkClaim::Queued, std::memory_order_relaxed);
        DecodeWork work{index, image.generation, &work_token,
                        image.compressed_slot,
                        staging_slot};
        resources_.slots->Compressed(image.compressed_slot).state =
            CompressedSlotState::DecodeInput;
        if (!work_queue_.TryPush(work)) {
            graphics_.UnmapDecodeStaging(staging_state.resource);
            staging_state.state = StagingSlotState::Prepared;
            resources_.slots->Compressed(image.compressed_slot).state =
                CompressedSlotState::CompressedDataAvailable;
            break;
        }
        image.work_active = true;
    }
}

void App::SubmitUploads() {
    for (const std::size_t index : PrioritizedCandidates(
             PipelineStage::DecodedStagingAvailable)) {
        ImageRecord& image = resources_.images[index];
        if (!ReservationActive(resources_.gpu_texture_reservations,
                               image.gpu_texture_reservation, index)) {
            continue;
        }
        GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(
            image.gpu_texture_reservation);
        if (gpu_texture.state != GpuTextureSlotState::Writable ||
            gpu_texture.reserved_frame != index) {
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
        const std::size_t old_bytes = gpu_texture.resource.bytes;
        const std::size_t retained_bytes = resources_.gpu_bytes >= old_bytes
                                               ? resources_.gpu_bytes - old_bytes
                                               : 0;
        if (bytes > config_.gpu_cache_bytes -
                        std::min(retained_bytes, config_.gpu_cache_bytes)) {
            continue;
        }
        UploadTicket ticket = graphics_.SubmitUpload(
            index, image.generation, image.staging_slot,
            staging_slot.resource, gpu_texture.resource);
        ticket.gpu_texture_slot = image.gpu_texture_reservation;
        resources_.gpu_bytes = retained_bytes + gpu_texture.resource.bytes;
        staging_slot.state = StagingSlotState::GpuCopySource;
        gpu_texture.content_frame = kInvalidFrame;
        gpu_texture.state = GpuTextureSlotState::Writing;
        resources_.uploads.push_back(std::move(ticket));
        if (NavigationInputPending(window_)) break;
    }
    ArmOldestFence();
}

bool App::TryPresent() {
    if (!resources_.frame_credit || resources_.reading_gpu_texture_fence != 0) return false;
    const auto next = resources_.navigation.NextIndex();
    if (next) {
        ImageRecord& image = resources_.images[*next];
        if (!HasReadableGpuTexture(*next)) return false;
        const SlotId gpu_texture_id = image.gpu_texture_reservation;
        GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(gpu_texture_id);
        resources_.reading_gpu_texture_fence = graphics_.Draw(gpu_texture.resource);
        gpu_texture.state = GpuTextureSlotState::Reading;
        resources_.reading_gpu_texture_slot = gpu_texture_id;
        ArmOldestFence();
        resources_.frame_credit = false;
        resources_.redraw_pending = false;
        resources_.navigation.CompletePresentation(*next);
        resources_.reservation_plan_dirty = true;
        RecordValidationPresentation(*next);
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
        if (HasReadableGpuTexture(current_index)) {
            const SlotId gpu_texture_id = current.gpu_texture_reservation;
            GpuTextureSlot& gpu_texture = resources_.slots->GpuTextureAt(gpu_texture_id);
            resources_.reading_gpu_texture_fence = graphics_.Draw(gpu_texture.resource);
            gpu_texture.state = GpuTextureSlotState::Reading;
            resources_.reading_gpu_texture_slot = gpu_texture_id;
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
    UINT64 value = resources_.reading_gpu_texture_fence;
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
    if (io_ring_) {
        io_ring_api_.close(io_ring_);
        io_ring_ = nullptr;
        io_ring_buffers_registered_ = false;
    }
    std::size_t pending_operations = 0;
    for (ImageRecord& image : resources_.images) {
        if (image.io && !image.io->using_io_ring &&
            image.io->file != INVALID_HANDLE_VALUE) {
            CancelIoEx(image.io->file, nullptr);
            if (image.io->split_header && !image.io->header_completed) {
                ++pending_operations;
            }
            if (image.io->content_submitted && !image.io->content_completed) {
                ++pending_operations;
            }
        }
    }
    while (pending_operations != 0) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL success = GetQueuedCompletionStatus(
            io_completion_port_, &transferred, &key, &overlapped, INFINITE);
        if (!overlapped) {
            if (!success) ThrowLastError("GetQueuedCompletionStatus shutdown");
            continue;
        }
        auto* request = reinterpret_cast<IoRequest*>(key);
        if (overlapped == &request->header_overlapped) {
            if (!request->header_completed) {
                request->header_completed = true;
                --pending_operations;
            }
        } else if (overlapped == &request->content_overlapped &&
                   !request->content_completed) {
            request->content_completed = true;
            --pending_operations;
        }
    }
    for (ImageRecord& image : resources_.images) {
        if (!image.io) continue;
        CloseHandle(image.io->file);
        image.io->file = INVALID_HANDLE_VALUE;
        if (image.io->compressed_slot != kInvalidSlot) {
            const std::size_t bytes = resources_.slots->Compressed(
                image.io->compressed_slot).resource.size;
            if (resources_.compressed_bytes >= bytes) resources_.compressed_bytes -= bytes;
            resources_.slots->ReleaseCompressed(image.io->compressed_slot);
            image.compressed_slot = kInvalidSlot;
        }
        image.io = nullptr;
    }
}

}  // namespace pv
