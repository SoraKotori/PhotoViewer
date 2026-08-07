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
    ReservationId source_reservation = kInvalidReservation;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

struct ResourceContext {
    Catalog catalog;
    NavigationState navigation;
    std::vector<ImageRecord> images;
    std::uint64_t generation = 1;

    ReservationTable compressed_reservations;
    ReservationTable staging_reservations;
    ReservationTable source_reservations;
    std::vector<std::size_t> priority_order;

    std::size_t compressed_bytes = 0;
    std::size_t gpu_bytes = 0;
    std::optional<ResourceSlots> slots;
    std::deque<UploadTicket> uploads;

    bool frame_credit = false;
    bool redraw_pending = true;
    UINT64 armed_fence = 0;
    SlotId reading_source_slot = kInvalidSlot;
    UINT64 reading_source_fence = 0;
};

class App {
public:
    explicit App(Config config);
    ~App();

    int Run(HINSTANCE instance, int show_command);

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);
    static void CALLBACK IoCompletion(PTP_CALLBACK_INSTANCE instance, void* context,
                                      void* overlapped, ULONG io_result,
                                      ULONG_PTR transferred, PTP_IO io);
    static void CALLBACK ValidationTimerCallback(PTP_CALLBACK_INSTANCE instance,
                                                 void* context, PTP_TIMER timer);
    void InitializeWindow(HINSTANCE instance, int show_command);
    void ProcessStartupCatalogCompletion();
    void ProcessStartupIoCompletion();
    int EventLoop();
    LRESULT HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void OpenInitialImage();
    void OnDirection(int direction, bool repeat, std::size_t repeat_count);
    void OnDirectionReleased(int direction);
    void OnCatalogComplete();
    void OnIoHeaderReady(IoRequest* request, LPARAM completion);
    void OnIoComplete(IoRequest* request, LPARAM completion);
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
    void ReconcileReservations();
    void PrepareStagingForImage(std::size_t index);
    void SubmitReads();
    void DispatchDecodes();
    void SubmitUploads();
    bool TryPresent();

    [[nodiscard]] std::vector<std::size_t> PrioritizedCandidates(
        PipelineStage stage) const;
    [[nodiscard]] PipelineStage StageOf(const ImageRecord& image) const noexcept;
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;
    [[nodiscard]] bool HasReadableSource(std::size_t frame) const noexcept;
    [[nodiscard]] bool CancelQueuedDecode(std::size_t frame);
    void ReleaseCompressed(ImageRecord& image);
    void CancelAllIo();
    void ArmOldestFence();

    Config config_;
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
    std::chrono::steady_clock::time_point validation_navigation_started_{};
    std::chrono::steady_clock::time_point validation_navigation_injection_finished_{};
    std::vector<std::size_t> validation_ready_indices_;
    std::vector<std::uint64_t> validation_ready_nanoseconds_;
    std::vector<std::size_t> validation_presented_indices_;
    std::vector<std::uint64_t> validation_presented_nanoseconds_;
    PTP_TIMER validation_navigation_timer_ = nullptr;
    std::vector<IoRequest*> deferred_catalog_io_;
    int exit_code_ = 0;

    Graphics graphics_;
    WorkQueue work_queue_;
    CompletionQueue completion_queue_;
    std::optional<DecoderPool> decoders_;
    std::optional<AsyncCatalog> catalog_io_;
    ResourceContext resources_;
};

}  // namespace pv
