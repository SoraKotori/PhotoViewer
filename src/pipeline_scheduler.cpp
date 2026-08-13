#include "pipeline_scheduler.h"

#include "decode_pipeline.h"
#include "graphics_pipeline.h"
#include "pipeline_limits.h"
#include "pipeline_model.h"
#include "pipeline_resources.h"
#include "pipeline_state.h"
#include "pipeline_resource_size.h"
#include "runtime_telemetry.h"
#include "storage_pipeline.h"
#include "win32_support.h"

namespace pv {

PipelineScheduler::PipelineScheduler(
    const PipelineLimits& limits, PipelineModel& model,
    PipelineResources& resources, StoragePipeline& storage,
    DecodePipeline& decode, GraphicsPipeline& graphics,
    RuntimeTelemetry& telemetry)
    : limits_(limits), model_(model), resources_(resources), storage_(storage),
      decode_(decode), graphics_(graphics), telemetry_(telemetry) {}

void PipelineScheduler::Pump() {
    if (model_.Frames().empty()) return;
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
        if (!ShouldContinuePipelinePass(storage_progress, presented)) {
            break;
        }
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
    return DeterminePipelineStage(frame, model_.Frames()[frame], resources_,
                                  model_.Reservations());
}

void PipelineScheduler::InitializeReservations() {
    const std::size_t frame_count = model_.Frames().size();
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
        if (!resources_.Slots().ActivateGpuTexture(id)) {
            throw std::logic_error("failed to activate GPU Texture slot");
        }
    }
}

bool PipelineScheduler::ReservationActive(const ReservationTable& table,
                            const ReservationId id,
                            const std::size_t frame) const noexcept {
    return IsReservationActive(table, id, frame);
}

void PipelineScheduler::RebuildReservationPlan() {
    const auto begin = telemetry_.NavigationActive()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    model_.Reservations().Rebuild(model_.NavigationView(), model_.Frames());
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

    const auto append_unique = [](std::vector<std::size_t>& frames,
                                  const std::size_t frame,
                                  const std::size_t capacity) {
        if (frames.size() < capacity &&
            std::find(frames.begin(), frames.end(), frame) == frames.end()) {
            frames.push_back(frame);
        }
    };
    const auto reservation_budget_allows =
        [&](const std::size_t candidate,
            const std::vector<ReservationEntry>& entries,
            const std::size_t budget, auto&& bytes_for) {
            std::size_t used = 0;
            for (const ReservationEntry& entry : entries) {
                if (entry.frame == kInvalidFrame) continue;
                const auto bytes = bytes_for(entry.frame);
                if (!bytes || !AddWithinBudget(*bytes, budget, used)) {
                    return false;
                }
            }
            const auto candidate_bytes = bytes_for(candidate);
            return candidate_bytes &&
                   AddWithinBudget(*candidate_bytes, budget, used);
        };
    std::vector<std::size_t>& gpu_texture_desired =
        model_.Reservations().PrepareGpuDesired();
    std::size_t gpu_bytes = 0;
    for (const std::size_t frame :
         model_.Reservations().DesiredGpuTextures()) {
        ImageRecord& image = model_.Frames()[frame];
        if (image.Failed()) continue;
        const auto bytes = GpuReservationBytes(
            model_.CatalogItemAt(frame), limits_.gpu_cache_bytes);
        if (!bytes || *bytes > limits_.gpu_cache_bytes) {
            image.MarkFailed();
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.gpu_cache_bytes, gpu_bytes)) {
            append_unique(gpu_texture_desired, frame,
                          model_.Reservations().GpuTextures().Capacity());
        }
    }

    model_.Reservations().GpuTextures().Reconcile(
        gpu_texture_desired,
        [&](const ReservationId id, const std::size_t) {
            const GpuTextureSlotState state =
                resources_.Slots().GpuTexture(id).State();
            return state == GpuTextureSlotState::Writable ||
                   state == GpuTextureSlotState::Readable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            image.ClearGpuTextureReservation(id);
            graphics_.ClearReservation(id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            image.AssignGpuTextureReservation(id);
            resources_.Slots().ReserveGpuTexture(id, frame,
                                                 image.Generation());
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!reservation_budget_allows(
                    frame, entries, limits_.gpu_cache_bytes,
                    [&](const std::size_t reserved_frame) {
                        return GpuReservationBytes(
                            model_.CatalogItemAt(reserved_frame),
                            limits_.gpu_cache_bytes);
                    })) {
                return kInvalidReservation;
            }
            for (ReservationId id = 0; id < entries.size(); ++id) {
                if (entries[id].frame == kInvalidFrame &&
                    resources_.Slots().GpuTexture(id).ContentFrame() == frame) {
                    return id;
                }
            }
            return ReservationTable::FirstFree(frame, entries);
        });

    std::vector<std::size_t>& staging_desired =
        model_.Reservations().PrepareStagingDesired();
    std::size_t staging_bytes = 0;
    for (const std::size_t frame : model_.Reservations().PriorityOrder()) {
        if (staging_desired.size() ==
            model_.Reservations().Staging().Capacity()) break;
        const ImageRecord& image = model_.Frames()[frame];
        if (image.Failed()) continue;
        bool gpu_texture_complete = false;
        if (image.GpuTextureReservation() != kInvalidReservation) {
            const GpuTextureSlot& gpu_texture = resources_.Slots().GpuTexture(
                image.GpuTextureReservation());
            gpu_texture_complete = gpu_texture.ReservedFrame() == frame &&
                              gpu_texture.State() != GpuTextureSlotState::Writable;
        }
        if (gpu_texture_complete) continue;
        const auto bytes = StagingReservationBytes(
            model_.CatalogItemAt(frame), limits_.staging_cache_bytes);
        if (!bytes || *bytes > limits_.staging_cache_bytes) {
            model_.Frames()[frame].MarkFailed();
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.staging_cache_bytes,
                            staging_bytes)) {
            append_unique(staging_desired, frame,
                          model_.Reservations().Staging().Capacity());
        }
    }
    model_.Reservations().Staging().Reconcile(
        staging_desired,
        [&](const ReservationId, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            if (image.StagingSlot() == kInvalidSlot) return true;
            const StagingSlot& slot =
                resources_.Slots().Staging(image.StagingSlot());
            if (slot.State() == StagingSlotState::Prepared ||
                slot.State() == StagingSlotState::DecodedPixelsAvailable) {
                return true;
            }
            if (slot.State() == StagingSlotState::DecodeOutputActive) {
                if (decode_.CancelQueued(frame)) return true;
                // The worker already owns this slot. Keep it alive and discard
                // or retain the result according to the reservation at completion.
            }
            return false;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            if (image.StagingSlot() != kInvalidSlot) {
                const StagingSlot& slot =
                    resources_.Slots().Staging(image.StagingSlot());
                if (slot.State() == StagingSlotState::Prepared ||
                    slot.State() == StagingSlotState::DecodedPixelsAvailable) {
                    const SlotId slot_id = image.StagingSlot();
                    resources_.Slots().ReleaseStaging(slot_id);
                    image.ClearStagingSlot(slot_id);
                }
            }
            image.ClearStagingReservation(id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            model_.Frames()[frame].AssignStagingReservation(id);
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!reservation_budget_allows(
                    frame, entries, limits_.staging_cache_bytes,
                    [&](const std::size_t reserved_frame) {
                        return StagingReservationBytes(
                            model_.CatalogItemAt(reserved_frame),
                            limits_.staging_cache_bytes);
                    })) {
                return kInvalidReservation;
            }
            return ReservationTable::FirstFree(frame, entries);
        });

    std::vector<std::size_t>& compressed_desired =
        model_.Reservations().PrepareCompressedDesired();
    std::size_t compressed_bytes = 0;
    for (const std::size_t frame : model_.Reservations().PriorityOrder()) {
        if (compressed_desired.size() ==
            model_.Reservations().Compressed().Capacity()) break;
        const PipelineStage stage = StageOf(frame);
        if (stage == PipelineStage::Failed ||
            stage == PipelineStage::DecodeQueued ||
            stage == PipelineStage::DecodedStagingAvailable ||
            stage == PipelineStage::Uploading ||
            stage == PipelineStage::PresentationTextureAvailable) {
            continue;
        }
        const auto bytes = CompressedReservationBytes(
            model_.CatalogItemAt(frame),
            limits_.compressed_budget_bytes, storage_.CompressedAlignment());
        if (!bytes || *bytes > limits_.compressed_budget_bytes) {
            model_.Frames()[frame].MarkFailed();
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.compressed_budget_bytes,
                            compressed_bytes)) {
            append_unique(compressed_desired, frame,
                          model_.Reservations().Compressed().Capacity());
        }
    }
    model_.Reservations().Compressed().Reconcile(
        compressed_desired,
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            if (image.IoActive()) {
                // A submitted kernel I/O owns the compressed slot until its
                // completion packet arrives. Cancel only on the transition to
                // retiring; repeated pipeline passes keep waiting for the same
                // completion packet without reissuing the cancellation.
                if (!model_.Reservations().Compressed().IsRetiring(id)) {
                    storage_.RetireRead(frame);
                }
                return false;
            }
            if (image.CompressedSlot() == kInvalidSlot) return true;
            return resources_.Slots().Compressed(image.CompressedSlot()).State() ==
                   CompressedSlotState::CompressedDataAvailable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            if (image.CompressedSlot() != kInvalidSlot) {
                decode_.ReleaseCompressed(frame);
            }
            image.ClearCompressedReservation(id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            ImageRecord& image = model_.Frames()[frame];
            image.AssignCompressedReservation(id);
            const CatalogItem& item = model_.CatalogItemAt(frame);
            if (item.file_size_known && item.file_bytes == 0) {
                image.MarkFailed();
            } else {
                image.ClearFailure();
            }
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!reservation_budget_allows(
                    frame, entries, limits_.compressed_budget_bytes,
                    [&](const std::size_t reserved_frame) {
                        return CompressedReservationBytes(
                            model_.CatalogItemAt(reserved_frame),
                            limits_.compressed_budget_bytes,
                            storage_.CompressedAlignment());
                    })) {
                return kInvalidReservation;
            }
            return ReservationTable::FirstFree(frame, entries);
        });
    telemetry_.Record(TimedOperation::ReservationReconcile, begin);
}

}  // namespace pv





