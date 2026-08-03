#pragma once

#include "catalog.h"
#include "config.h"
#include "decoder.h"
#include "graphics.h"
#include "navigation.h"

namespace pv {

struct IoRequest {
    OVERLAPPED overlapped{};
    HANDLE file = INVALID_HANDLE_VALUE;
    PTP_IO threadpool_io = nullptr;
    HWND window = nullptr;
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    std::atomic<DWORD> result{ERROR_IO_PENDING};
    std::atomic<ULONG_PTR> transferred{0};
};

enum class ImageDemandState : std::uint8_t { Outside, Requested, Failed };

struct ImageRecord {
    ImageDemandState demand = ImageDemandState::Outside;
    std::uint64_t generation = 0;
    std::size_t reserved_output_bytes = 0;
    std::unique_ptr<IoRequest> io;
    std::shared_ptr<WorkToken> work_token;
    SlotId compressed_slot = kInvalidSlot;
    SlotId cpu_surface_slot = kInvalidSlot;
    SlotId gpu_texture_slot = kInvalidSlot;
};

struct BufferRanges {
    std::size_t required_low = 0;
    std::size_t required_high = 0;
    std::size_t cpu_low = 0;
    std::size_t cpu_high = 0;
    std::size_t gpu_low = 0;
    std::size_t gpu_high = 0;
};

struct ResourceContext {
    Catalog catalog;
    NavigationState navigation;
    std::vector<ImageRecord> images;
    BufferRanges ranges;
    std::uint64_t generation = 1;

    std::size_t compressed_bytes = 0;
    std::size_t reserved_output_bytes = 0;
    std::size_t gpu_bytes = 0;
    std::unique_ptr<ResourceSlots> slots;
    std::deque<UploadTicket> uploads;

    bool frame_credit = false;
    bool redraw_pending = true;
    UINT64 armed_fence = 0;
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
    int EventLoop();
    LRESULT HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void OpenInitialImage();
    void OnDirection(int direction, bool repeat);
    void OnDirectionReleased(int direction);
    void OnIoComplete(IoRequest* request);
    void OnWorkerComplete();
    void OnGpuComplete();
    void OnFrameCredit();
    void OnSurfaceChanged(UINT width, UINT height);
    void OnPaint();
    void ToggleFullscreen();
    void InjectValidationNavigation();
    void InjectValidationNavigationStep();
    void StopValidationNavigationTimer();
    void WriteValidationReport(std::string_view phase, bool truncate);
    void BeginFullscreenValidation();
    void OnFullscreenValidationTimer();

    void PumpPipeline();
    void RecalculateRanges();
    void ReclaimOutsideRanges();
    void SubmitReads();
    void DispatchDecodes();
    void SubmitUploads();
    bool TryPresent();

    [[nodiscard]] bool InRequiredRange(std::size_t index) const noexcept;
    [[nodiscard]] bool InCpuRange(std::size_t index) const noexcept;
    [[nodiscard]] bool InGpuRange(std::size_t index) const noexcept;
    [[nodiscard]] std::vector<std::size_t> PrioritizedCandidates(PipelineStage stage,
                                                                 bool gpu_range) const;
    [[nodiscard]] PipelineStage StageOf(const ImageRecord& image) const noexcept;
    void ReleaseCompressed(ImageRecord& image);
    void ReleaseReservation(ImageRecord& image);
    void EvictGpu(std::size_t index);
    void CancelAllIo();
    void ArmOldestFence();
    [[nodiscard]] std::size_t CountStage(PipelineStage stage) const noexcept;

    Config config_;
    HWND window_ = nullptr;
    bool running_ = true;
    bool graphics_ready_ = false;
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
    std::chrono::steady_clock::time_point validation_navigation_started_{};
    PTP_TIMER validation_navigation_timer_ = nullptr;
    int exit_code_ = 0;

    Graphics graphics_;
    WorkQueue work_queue_;
    CompletionQueue completion_queue_;
    std::unique_ptr<DecoderPool> decoders_;
    ResourceContext resources_;
};

}  // namespace pv
