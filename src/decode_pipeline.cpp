#include "decode_pipeline.h"

#include "graphics_pipeline.h"
#include "pipeline_model.h"
#include "pipeline_resources.h"
#include "pipeline_resource_size.h"
#include "pipeline_state.h"
#include "runtime_telemetry.h"
#include "win32_support.h"

#include <utility>

namespace pv {
DecodePipeline::DecodePipeline(const PipelineLimits& limits,
                               const PipelineModel& model,
                               DecodeFrameAccess frames,
                               const PipelineResources& resources,
                               DecodeResourceAccess slots,
                               GraphicsPipeline& graphics,
                               RuntimeTelemetry& telemetry,
                               const PngValidationOptions validation)
    : limits_(limits), model_(model), frames_(std::move(frames)),
      resources_(resources), slots_(std::move(slots)), graphics_(graphics),
      telemetry_(telemetry),
      workers_(limits.staging_slot_count, slots_.WorkerView(), validation) {}

void DecodePipeline::Start(const std::size_t worker_count) {
    workers_.Start(worker_count);
}

void DecodePipeline::Stop() noexcept { workers_.Stop(); }

void DecodePipeline::EnableGraphicsDevice() noexcept {
    graphics_device_ready_ = true;
}

HANDLE DecodePipeline::CompletionEvent() const noexcept {
    return workers_.CompletionEvent();
}

bool DecodePipeline::HandleCompletions() {
    const CompletionQueue::Batch& batch = workers_.Drain();
    if (batch.results.empty() && batch.released_inputs.empty()) return false;
    for (const ReleasedInput& input : batch.released_inputs) {
        if (input.compressed_slot == kInvalidSlot) continue;
        const CompressedSlot& slot =
            slots_.Compressed(input.compressed_slot);
        const std::size_t frame = slot.Image();
        if (frame < model_.FrameCount()) {
            const ImageRecord& image = frames_.View(frame);
            if (image.Generation() == input.generation &&
                image.CompressedSlot() == input.compressed_slot) {
                frames_.ClearCompressedSlot(frame, input.compressed_slot);
            }
        }
        slots_.ReleaseCompressed(input.compressed_slot);
    }
    for (const DecodeResult& result : batch.results) {
        if (result.staging_slot == kInvalidSlot) continue;
        const StagingSlot& slot = slots_.Staging(result.staging_slot);
        DecodeStaging& staging =
            slots_.StagingResource(result.staging_slot);
        const bool cpu_decode = staging.cpu_surface;
        if (!cpu_decode) graphics_.UnmapDecodeStaging(staging);
        const std::size_t frame = slot.Image();
        if (frame >= model_.FrameCount()) {
            slots_.ReleaseStaging(result.staging_slot);
            continue;
        }
        const ImageRecord& image = frames_.View(frame);
        if (frame == model_.NavigationView().CurrentIndex() &&
            model_.NavigationView().InitialPending() &&
            telemetry_.At(StartupMilestone::InitialDecodeCompleted) ==
                std::chrono::steady_clock::time_point{}) {
            telemetry_.Mark(StartupMilestone::InitialDecodeCompleted);
        }
        if (result.generation != image.Generation()) {
            slots_.ReleaseStaging(result.staging_slot);
            if (image.StagingSlot() == result.staging_slot) {
                frames_.ClearStagingSlot(frame, result.staging_slot);
            }
            continue;
        }
        frames_.EndWork(frame);
        const bool reserved = ReservationActive(
            model_.ReservationPlan().Staging(), image.StagingReservation(),
            frame);
        if (result.success && reserved) {
            slots_.CompleteDecodeOutput(result.staging_slot);
        } else {
            slots_.ReleaseStaging(result.staging_slot);
            frames_.ClearStagingSlot(frame, result.staging_slot);
            if (FAILED(result.error) && reserved) {
                frames_.MarkFailed(frame);
            }
        }
    }
    return true;
}

bool DecodePipeline::CancelQueued(const std::size_t frame) {
    if (frame >= model_.FrameCount()) return false;
    const ImageRecord& image = frames_.View(frame);
    if (!image.WorkActive() || image.StagingSlot() == kInvalidSlot) return false;
    DecodeWork cancelled;
    if (!workers_.Cancel(image.StagingSlot(), cancelled)) {
        return false;
    }

    if (cancelled.staging_slot != kInvalidSlot) {
        graphics_.UnmapDecodeStaging(
            slots_.StagingResource(cancelled.staging_slot));
        slots_.ReleaseStaging(cancelled.staging_slot);
        if (image.StagingSlot() == cancelled.staging_slot) {
            frames_.ClearStagingSlot(frame, cancelled.staging_slot);
        }
    }
    if (cancelled.compressed_slot != kInvalidSlot) {
        if (ReservationActive(model_.ReservationPlan().Compressed(),
                              image.CompressedReservation(), frame)) {
            slots_.RestoreDecodeInput(cancelled.compressed_slot);
        } else {
            ReleaseCompressedFrame(frame);
        }
    }
    frames_.EndWork(frame);
    return true;
}

void DecodePipeline::PrepareStaging(const std::size_t index) {
    if (index >= model_.FrameCount()) return;
    const ImageRecord& image = frames_.View(index);
    if (image.Failed() || image.StagingSlot() != kInvalidSlot ||
        !ReservationActive(model_.ReservationPlan().Staging(),
                           image.StagingReservation(), index)) {
        return;
    }
    const CatalogItem& item = model_.CatalogItemAt(index);
    if (!item.header_valid) return;
    const std::optional<std::size_t> staging_bytes =
        StagingReservationBytes(item, limits_.staging_cache_bytes);
    if (!staging_bytes || *staging_bytes == 0 ||
        *staging_bytes > limits_.staging_cache_bytes) {
        return;
    }
    const SlotId staging_slot = slots_.AcquireStaging(
        *staging_bytes, index, image.Generation());
    if (staging_slot == kInvalidSlot) return;
    DecodeStaging& staging = slots_.StagingResource(staging_slot);
    staging.Configure(item.resource_plan);
    if (graphics_device_ready_) {
        graphics_.PrepareDecodeStaging(staging);
    } else if (!staging.PrepareCpuSurface()) {
        slots_.ReleaseStaging(staging_slot);
        return;
    }
    frames_.AttachStagingSlot(index, staging_slot);
}

void DecodePipeline::DispatchEligible() {
    for (const std::size_t index : model_.ReservationPlan().PriorityOrder()) {
        if (DeterminePipelineStage(index, frames_.View(index),
                                   resources_.SlotsView(),
                                   model_.ReservationPlan()) !=
            PipelineStage::CompressedReady) {
            continue;
        }
        const ImageRecord& image = frames_.View(index);
        if (!ReservationActive(model_.ReservationPlan().Staging(),
                               image.StagingReservation(), index)) {
            continue;
        }
        if (!model_.CatalogItemAt(index).header_valid) {
            ReleaseCompressedFrame(index);
            frames_.MarkFailed(index);
            continue;
        }
        const CatalogItem& item = model_.CatalogItemAt(index);
        const std::optional<std::size_t> staging_bytes =
            StagingReservationBytes(item, limits_.staging_cache_bytes);
        if (!staging_bytes || *staging_bytes == 0 ||
            *staging_bytes > limits_.staging_cache_bytes) {
            ReleaseCompressedFrame(index);
            frames_.MarkFailed(index);
            continue;
        }
        PrepareStaging(index);
        const SlotId staging_slot = image.StagingSlot();
        if (staging_slot == kInvalidSlot) continue;
        const StagingSlot& staging_state =
            slots_.Staging(staging_slot);
        DecodeStaging& staging = slots_.StagingResource(staging_slot);
        if (staging_state.State() != StagingSlotState::Prepared ||
            staging_state.Image() != index ||
            staging_state.Generation() != image.Generation()) {
            throw std::logic_error("invalid prepared staging slot");
        }
        if (graphics_device_ready_) {
            graphics_.MapDecodeStaging(staging);
        } else if (!staging.cpu_surface &&
                   !staging.PrepareCpuSurface()) {
            slots_.ReleaseStaging(staging_slot);
            frames_.ClearStagingSlot(index, staging_slot);
            continue;
        }
        slots_.BeginDecodeOutput(staging_slot);
        DecodeWork work{index, image.Generation(), image.CompressedSlot(),
                        staging_slot};
        slots_.BeginDecodeInput(image.CompressedSlot());
        if (!workers_.Submit(work)) {
            graphics_.UnmapDecodeStaging(staging);
            slots_.RestoreDecodeOutput(staging_slot);
            slots_.RestoreDecodeInput(image.CompressedSlot());
            break;
        }
        frames_.BeginWork(index);
        if (model_.NavigationView().InitialPending() &&
            index == model_.NavigationView().CurrentIndex() &&
            telemetry_.At(StartupMilestone::InitialDecodeSubmitted) ==
                std::chrono::steady_clock::time_point{}) {
            telemetry_.Mark(StartupMilestone::InitialDecodeSubmitted);
        }
    }
}

void DecodePipeline::ReleaseCompressedFrame(const std::size_t frame) {
    const ImageRecord& image = frames_.View(frame);
    if (image.CompressedSlot() == kInvalidSlot) return;
    const SlotId compressed_slot = image.CompressedSlot();
    slots_.ReleaseCompressed(compressed_slot);
    frames_.ClearCompressedSlot(frame, compressed_slot);
}

void DecodePipeline::ReleaseCompressed(const std::size_t frame) {
    ReleaseCompressedFrame(frame);
}

void DecodePipeline::Reorder(const std::span<const std::size_t> priority) {
    workers_.Reorder(priority);
}

void DecodePipeline::Remap(const std::size_t from, const std::size_t to,
                           const std::uint64_t generation) {
    workers_.Remap(from, to, generation);
}

void DecodePipeline::ResetMetrics() noexcept { workers_.ResetMetrics(); }

std::uint64_t DecodePipeline::DecodeCount() const noexcept {
    return workers_.DecodeCount();
}

std::uint64_t DecodePipeline::DecodeNanoseconds() const noexcept {
    return workers_.DecodeNanoseconds();
}

std::size_t DecodePipeline::QueuedWorkCount() const {
    return workers_.QueuedWorkCount();
}

bool DecodePipeline::ReservationActive(const ReservationTable& table,
                                       const ReservationId id,
                                       const std::size_t frame) const noexcept {
    return IsReservationActive(table, id, frame);
}

}  // namespace pv
