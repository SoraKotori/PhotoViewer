#pragma once

#include "graphics.h"
#include "pipeline_limits.h"
#include "pipeline_model.h"
#include "pipeline_resource_size.h"
#include "pipeline_resources.h"
#include "presentation_controller.h"
#include "upload_ledger.h"

#include <optional>

namespace pv {

class ImageRecord;
class PipelineObserver;
class ReservationTable;
class ViewerWindow;

// Owns the graphics executor, upload queue, GPU budget and presentation
// authorization. Fence completion is the sole path that releases GPU work.
class GraphicsPipeline {
public:
    GraphicsPipeline(const PipelineLimits& limits, const PipelineModel& model,
                     PresentationCompletionAccess presentation_completion,
                     GraphicsFrameAccess frames,
                     const PipelineResources& resources,
                     GraphicsResourceAccess slots, PipelineObserver& observer,
                     ViewerWindow& window);

    void InitializeDevice();
    void Initialize2D();
    void InitializeSwapChain();
    void InitializeBackBufferTarget();
    [[nodiscard]] bool DeviceReady() const noexcept;
    [[nodiscard]] bool UploadReady() const noexcept;
    [[nodiscard]] bool Ready() const noexcept;

    [[nodiscard]] HANDLE FrameWaitEvent() const noexcept;
    [[nodiscard]] HANDLE GpuWaitEvent() const noexcept;
    [[nodiscard]] bool HandleGpuCompletion();
    void GrantFrameCredit() noexcept;
    void Resize(UINT width, UINT height);
    void Paint();

    void PrepareDecodeStaging(DecodeStaging& staging, UINT width, UINT height);
    void MapDecodeStaging(DecodeStaging& staging, UINT width, UINT height,
                          std::size_t decoded_bytes);
    void UnmapDecodeStaging(DecodeStaging& staging);

    void SubmitEligibleUploads();
    void ClearReservation(SlotId id);
    [[nodiscard]] bool TryPresent();
    [[nodiscard]] bool HasReadableTexture(std::size_t frame) const noexcept;
    void RequestRedraw(bool erase) noexcept;
    void DrainForShutdown() noexcept;
    void UnmapAllStagingForShutdown() noexcept;

    void ResetMetrics() noexcept;
    [[nodiscard]] std::size_t UploadCount() const noexcept;
    [[nodiscard]] std::size_t GpuBytes() const noexcept;
    [[nodiscard]] std::uint64_t SubmittedUploadCount() const noexcept;
    [[nodiscard]] std::uint64_t UploadNanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t DrawCount() const noexcept;
    [[nodiscard]] std::uint64_t DrawNanoseconds() const noexcept;

private:
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;
    void ArmOldestFence();
    [[nodiscard]] bool MakeGpuBytesAvailable(SlotId destination,
                                             std::size_t bytes);

    const PipelineLimits& limits_;
    const PipelineModel& model_;
    PresentationCompletionAccess presentation_completion_;
    GraphicsFrameAccess frames_;
    const PipelineResources& resources_;
    GraphicsResourceAccess slots_;
    PipelineObserver& observer_;
    ViewerWindow& window_;
    Graphics graphics_;
    PresentationController presentation_;
    UploadLedger uploads_;
    FixedSlotByteBudget gpu_budget_;
    bool device_ready_ = false;
    bool upload_ready_ = false;
    bool ready_ = false;
};

}  // namespace pv
