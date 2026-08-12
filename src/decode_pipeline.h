#pragma once

#include "decode_stage.h"
#include "pipeline_limits.h"
#include "pipeline_model.h"
#include "pipeline_resources.h"

namespace pv {

class GraphicsPipeline;
class ReservationTable;
class RuntimeTelemetry;
class ImageRecord;

// Owns CPU work queues, workers and decode resource authorizations. All
// staging and compressed-slot transitions for a CPU task happen here.
class DecodePipeline {
public:
    DecodePipeline(const PipelineLimits& limits, const PipelineModel& model,
                   DecodeFrameAccess frames,
                   const PipelineResources& resources,
                   DecodeResourceAccess slots, GraphicsPipeline& graphics,
                   RuntimeTelemetry& telemetry);

    void Start(std::size_t worker_count);
    void Stop() noexcept;
    void EnableGraphicsDevice() noexcept;

    [[nodiscard]] HANDLE CompletionEvent() const noexcept;
    [[nodiscard]] bool HandleCompletions();
    void PrepareStaging(std::size_t frame);
    void DispatchEligible();
    [[nodiscard]] bool CancelQueued(std::size_t frame);
    void ReleaseCompressed(std::size_t frame);
    void Reorder(std::span<const std::size_t> priority);
    void Remap(std::size_t from, std::size_t to, std::uint64_t generation);

    void ResetMetrics() noexcept;
    [[nodiscard]] std::uint64_t DecodeCount() const noexcept;
    [[nodiscard]] std::uint64_t DecodeNanoseconds() const noexcept;
    [[nodiscard]] std::size_t QueuedWorkCount() const;

private:
    [[nodiscard]] bool ReservationActive(const ReservationTable& table,
                                         ReservationId id,
                                         std::size_t frame) const noexcept;
    void ReleaseCompressedFrame(std::size_t frame);

    const PipelineLimits& limits_;
    const PipelineModel& model_;
    DecodeFrameAccess frames_;
    const PipelineResources& resources_;
    DecodeResourceAccess slots_;
    GraphicsPipeline& graphics_;
    RuntimeTelemetry& telemetry_;
    DecodeStage workers_;
    bool graphics_device_ready_ = false;
};

}  // namespace pv
