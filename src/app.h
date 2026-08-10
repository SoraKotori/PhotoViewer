#pragma once

#include "catalog.h"
#include "config.h"
#include "decoder.h"
#include "graphics.h"
#include "navigation.h"
#include "reservation.h"

namespace pv {

struct ImageRecord {
    bool failed = false;
    std::uint64_t generation = 0;
    IoRequest* io = nullptr;
    bool work_active = false;
    ReservationId compressed_reservation = kInvalidReservation;
    ReservationId staging_reservation = kInvalidReservation;
    ReservationId gpu_texture_reservation = kInvalidReservation;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

struct ResourceContext {
    explicit ResourceContext(const Config& config)
        : slots(config.compressed_slot_count, config.staging_slot_count,
                config.GpuSlotCount(), config.compressed_budget_bytes,
                config.staging_cache_bytes) {}

    Catalog catalog;
    NavigationState navigation;
    std::vector<ImageRecord> images;
    std::uint64_t generation = 1;

    ReservationTable compressed_reservations;
    ReservationTable staging_reservations;
    ReservationTable gpu_texture_reservations;
    std::vector<std::size_t> priority_order;
    std::vector<std::size_t> gpu_texture_desired;
    bool reservation_plan_dirty = true;

    std::size_t compressed_bytes = 0;
    std::size_t gpu_bytes = 0;
    ResourceSlots slots;
    std::deque<UploadTicket> uploads;

    bool frame_credit = false;
    bool redraw_pending = true;
    UINT64 armed_fence = 0;
    SlotId reading_gpu_texture_slot = kInvalidSlot;
    UINT64 reading_gpu_texture_fence = 0;
};

class App {
public:
    App(Config config, std::chrono::steady_clock::time_point process_started);
    ~App();

    int Run(HINSTANCE instance, int show_command);

private:
    struct ValidationSample {
        std::size_t image_index = 0;
        std::uint64_t nanoseconds = 0;
    };

    struct ComApartment {
        ComApartment();
        ~ComApartment();

        ComApartment(const ComApartment&) = delete;
        ComApartment& operator=(const ComApartment&) = delete;
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);
    void InitializeWindow(HINSTANCE instance, int show_command);
    [[nodiscard]] bool DrainCompletions(bool drain_catalog = true);
    int EventLoop();
    LRESULT HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void OpenInitialImage();
    void OnDirection(int direction, bool repeat, std::size_t repeat_count);
    void OnDirectionReleased(int direction);
    void OnCatalogComplete();
    void OnIoCompletion(IoRequest* request, OVERLAPPED* overlapped,
                        DWORD result, ULONG_PTR transferred);
    void OnIoHeaderReady(IoRequest* request, DWORD result,
                         ULONG_PTR transferred);
    void OnIoComplete(IoRequest* request, DWORD result,
                      ULONG_PTR transferred);
    void CompleteIoRequest(IoRequest* request);
    void OnWorkerComplete();
    void OnGpuComplete();
    void OnFrameCredit();
    void OnSurfaceChanged(UINT width, UINT height);
    void OnPaint();
    void ToggleFullscreen();
    void InjectValidationNavigation();
    void InjectValidationNavigationStep();
    void StopValidationNavigationTimer();
    void RecordValidationReady(std::size_t index);
    void RecordValidationPresentation(std::size_t index);
    void WriteValidationReport(std::string_view phase, bool truncate);
    void BeginFullscreenValidation();
    void OnFullscreenValidationTimer();

    void PumpPipeline();
    void InitializeReservations();
    void RebuildReservationPlan();
    void ReconcileReservations();
    void PrepareStagingForImage(std::size_t index);
    void SubmitReads();
    void DispatchDecodes();
    void SubmitUploads();
    bool TryPresent();

    [[nodiscard]] PipelineStage StageOf(const ImageRecord& image) const noexcept;
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;
    [[nodiscard]] bool HasReadableGpuTexture(std::size_t frame) const noexcept;
    [[nodiscard]] bool CancelQueuedDecode(std::size_t frame);
    void ReleaseCompressed(ImageRecord& image);
    void CancelAllIo();
    void ArmOldestFence();

    Config config_;
    // All App state and IOCP consumption remain main-thread-owned.
    HANDLE io_completion_port_ = nullptr;
    HANDLE io_completion_event_ = nullptr;
    HWND window_ = nullptr;
    bool running_ = true;
    bool graphics_device_ready_ = false;
    bool graphics_ready_ = false;
    bool catalog_loading_ = false;
    DWORD io_prefix_granularity_ = 0;
    bool fullscreen_ = false;
    WINDOWPLACEMENT windowed_placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowed_style_ = 0;
    int validation_fullscreen_phase_ = 0;
    RECT validation_windowed_rect_{};
    RECT validation_monitor_rect_{};
    LONG_PTR validation_windowed_style_ = 0;
    bool validation_script_injected_ = false;
    bool validation_script_scheduled_ = false;
    std::size_t validation_navigation_cursor_ = 0;
    std::size_t validation_expected_index_ = 0;
    std::chrono::steady_clock::time_point validation_cold_started_{};
    std::chrono::steady_clock::time_point validation_window_ready_{};
    std::chrono::steady_clock::time_point validation_initial_io_submitted_{};
    std::chrono::steady_clock::time_point validation_decoders_ready_{};
    std::chrono::steady_clock::time_point validation_graphics_device_ready_{};
    std::chrono::steady_clock::time_point validation_graphics_ready_{};
    std::chrono::steady_clock::time_point validation_catalog_ready_{};
    std::chrono::steady_clock::time_point validation_initial_header_ready_{};
    std::chrono::steady_clock::time_point validation_initial_content_completion_observed_{};
    std::chrono::steady_clock::time_point validation_initial_content_ready_{};
    std::chrono::steady_clock::time_point validation_initial_decode_submitted_{};
    std::chrono::steady_clock::time_point validation_initial_decode_completed_{};
    std::chrono::steady_clock::time_point validation_navigation_started_{};
    std::chrono::steady_clock::time_point validation_navigation_injection_finished_{};
    std::uint64_t validation_pump_count_ = 0;
    std::uint64_t validation_pump_nanoseconds_ = 0;
    std::uint64_t validation_plan_count_ = 0;
    std::uint64_t validation_plan_nanoseconds_ = 0;
    std::uint64_t validation_reconcile_count_ = 0;
    std::uint64_t validation_reconcile_nanoseconds_ = 0;
    std::uint64_t validation_dispatch_nanoseconds_ = 0;
    std::uint64_t validation_submit_reads_nanoseconds_ = 0;
    std::uint64_t validation_acquire_compressed_nanoseconds_ = 0;
    std::uint64_t validation_open_file_nanoseconds_ = 0;
    std::uint64_t validation_read_file_nanoseconds_ = 0;
    std::uint64_t validation_submit_uploads_nanoseconds_ = 0;
    std::uint64_t validation_try_present_nanoseconds_ = 0;
    std::uint64_t validation_main_kernel_started_ = 0;
    std::uint64_t validation_main_user_started_ = 0;
    std::vector<ValidationSample> validation_ready_samples_;
    std::vector<ValidationSample> validation_presented_samples_;
    bool validation_navigation_timer_active_ = false;
    int exit_code_ = 0;

    std::optional<ComApartment> com_apartment_;
    Graphics graphics_;
    WorkQueue work_queue_;
    CompletionQueue completion_queue_;
    CompletionQueue::Batch completion_batch_;
    std::optional<DecoderPool> decoders_;
    std::optional<AsyncCatalog> catalog_io_;
    ResourceContext resources_;
};

}  // namespace pv
