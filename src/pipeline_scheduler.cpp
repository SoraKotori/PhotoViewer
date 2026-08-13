#include "pipeline_scheduler.h"

#include "decode_pipeline.h"
#include "graphics_pipeline.h"
#include "pipeline_limits.h"
#include "pipeline_state.h"
#include "runtime_telemetry.h"
#include "storage_pipeline.h"

#include <algorithm>
#include <utility>

namespace pv {

PipelineScheduler::PipelineScheduler(
    const PipelineLimits& limits, SchedulerModelAccess model,
    SchedulerResourceAccess resources, StoragePipeline& storage,
    DecodePipeline& decode, GraphicsPipeline& graphics,
    RuntimeTelemetry& telemetry)
    : limits_(limits), model_(std::move(model)),
      resources_(std::move(resources)), storage_(storage),
      decode_(decode), graphics_(graphics), telemetry_(telemetry) {}

void PipelineScheduler::Pump() {
    if (model_.FrameCount() == 0) return;
    const auto begin = telemetry_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    for (int pass = 0; pass < 3; ++pass) {
        ReconcileReservations();
        telemetry_.Measure(TimedOperation::DispatchDecode,
                           [&] { decode_.DispatchEligible(); });
        const bool storage_progress = telemetry_.Measure(
            TimedOperation::SubmitReads,
            [&] { return SubmitStorageReads(); });
        if (catalog_complete_) {
            telemetry_.Measure(TimedOperation::SubmitUploads,
                               [&] { graphics_.SubmitEligibleUploads(); });
        }
        const bool presented = telemetry_.Measure(
            TimedOperation::TryPresent,
            [&] { return graphics_.TryPresent(); });
        if (!ShouldContinuePipelinePass(storage_progress, presented)) break;
    }
    telemetry_.Record(TimedOperation::PipelinePump, begin);
}

bool PipelineScheduler::DrainStorage() {
    const bool drained = storage_.DrainCompletions();
    PrepareStorageHeaders();
    return drained;
}

void PipelineScheduler::PrepareStorageHeaders() {
    for (const std::size_t frame : storage_.HeaderReadyFrames()) {
        decode_.PrepareStaging(frame);
    }
    storage_.ClearHeaderReadyFrames();
}

bool PipelineScheduler::SubmitStorageReads() {
    const bool synchronous_progress = storage_.SubmitEligibleReads();
    PrepareStorageHeaders();
    return synchronous_progress;
}

PipelineStage PipelineScheduler::StageOf(
    const std::size_t frame) const noexcept {
    return DeterminePipelineStage(frame, model_.Frame(frame), resources_.View(),
                                  model_.Reservations());
}

void PipelineScheduler::InitializeReservations() {
    const std::size_t frame_count = model_.FrameCount();
    if (frame_count == 0) return;

    const std::size_t compressed_capacity =
        std::min(frame_count, limits_.compressed_slot_count);
    const std::size_t staging_capacity =
        std::min(frame_count, limits_.staging_slot_count);
    const std::size_t gpu_texture_capacity =
        std::min(frame_count, limits_.GpuSlotCount());

    model_.Reservations().Reset(compressed_capacity, staging_capacity,
                                gpu_texture_capacity);
    for (SlotId id = 0; id < gpu_texture_capacity; ++id) {
        if (!resources_.ActivateGpuTexture(id)) {
            throw std::logic_error("failed to activate GPU Texture slot");
        }
    }
}

bool PipelineScheduler::ReservationActive(const ReservationTable& table,
                                          const ReservationId id,
                                          const std::size_t frame) const
    noexcept {
    return IsReservationActive(table, id, frame);
}

void PipelineScheduler::RebuildReservationPlan() {
    const auto begin = telemetry_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    model_.Reservations().Rebuild(model_.Navigation(), model_.Frames());
    decode_.Reorder(model_.Reservations().PriorityOrder());
    telemetry_.Record(TimedOperation::ReservationPlan, begin);
}

void PipelineScheduler::ReconcileReservations() {
    const auto begin = telemetry_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    if (model_.Reservations().NeedsRebuild(model_.Frames())) {
        RebuildReservationPlan();
    }
    ReconcileGpuReservations();
    ReconcileStagingReservations();
    ReconcileCompressedReservations();
    telemetry_.Record(TimedOperation::ReservationReconcile, begin);
}

}  // namespace pv

// Reservation reconciliation runs on every pump pass. Keep each resource
// policy in its own readable file while compiling them into this translation
// unit so the release build can inline the hot boundaries.
#include "pipeline_scheduler_gpu.inl"
#include "pipeline_scheduler_staging.inl"
#include "pipeline_scheduler_compressed.inl"
