#pragma once

#include "catalog.h"
#include "config.h"
#include "decoder.h"
#include "graphics.h"
#include "navigation.h"
#include "pipeline_types.h"
#include "reservation_planner.h"
#include "validation.h"
#include "viewer_window.h"
#include "win32_handle.h"

namespace pv {

class App;

class PipelineObserver {
public:
    virtual void OnFrameReady(std::size_t index) = 0;
    virtual void OnFramePresented(std::size_t index) = 0;

protected:
    ~PipelineObserver() = default;
};

struct PipelineState {
    explicit PipelineState(const Config& config)
        : slots(config.compressed_slot_count, config.staging_slot_count,
                config.GpuSlotCount(), config.compressed_budget_bytes,
                config.staging_cache_bytes),
          reservations(config) {}

    Catalog catalog;
    NavigationState navigation;
    std::vector<ImageRecord> images;
    std::uint64_t generation = 1;

    std::size_t compressed_bytes = 0;
    std::size_t gpu_bytes = 0;
    ResourceSlots slots;
    ReservationPlanner reservations;
    std::deque<UploadTicket> uploads;

    bool frame_credit = false;
    bool redraw_pending = true;
    UINT64 armed_fence = 0;
    SlotId reading_gpu_texture_slot = kInvalidSlot;
    UINT64 reading_gpu_texture_fence = 0;
};

// Owns every storage, decode, reservation and GPU resource participating in
// the image pipeline. All methods are called by App on the main thread; worker,
// storage and GPU completions only transfer ownership back through their queues.
class PipelineRuntime {
public:
    PipelineRuntime(PipelineObserver& observer, Config& config,
                    ValidationState& validation,
                    ViewerWindow& window, bool& catalog_loading);
    ~PipelineRuntime();

    PipelineRuntime(const PipelineRuntime&) = delete;
    PipelineRuntime& operator=(const PipelineRuntime&) = delete;

private:
    friend class App;

    [[nodiscard]] bool DrainCompletions();
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

    void PumpPipeline();
    void InitializeReservations();
    void RebuildReservationPlan();
    void ReconcileReservations();
    void PrepareStagingForImage(std::size_t index);
    void SubmitReads();
    void DispatchDecodes();
    void SubmitUploads();
    bool TryPresent();

    [[nodiscard]] PipelineStage StageOf(
        const ImageRecord& image) const noexcept;
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;
    [[nodiscard]] bool HasReadableGpuTexture(
        std::size_t frame) const noexcept;
    [[nodiscard]] bool CancelQueuedDecode(std::size_t frame);
    void ReleaseCompressed(ImageRecord& image);
    void CancelAllIo();
    void ArmOldestFence();

    PipelineObserver& observer_;
    Config& config_;
    ValidationState& validation_;
    ViewerWindow& window_;
    bool& catalog_loading_;

    UniqueHandle io_completion_port_;
    UniqueHandle io_completion_event_;
    bool graphics_device_ready_ = false;
    bool graphics_ready_ = false;
    DWORD io_prefix_granularity_ = 0;
    Graphics graphics_;
    WorkQueue work_queue_;
    CompletionQueue completion_queue_;
    CompletionQueue::Batch completion_batch_;
    std::optional<DecoderPool> decoders_;
    PipelineState state_;
};

}  // namespace pv
