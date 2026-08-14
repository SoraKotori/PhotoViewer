#include "pipeline_scheduler.h"

#include "decode_pipeline.h"
#include "pipeline_limits.h"
#include "pipeline_resource_size.h"
#include "scheduler_reservation_policy.h"

namespace pv {

__forceinline void PipelineScheduler::ReconcileStagingReservations() {
    std::vector<std::size_t>& desired =
        model_.Reservations().PrepareStagingDesired();
    std::size_t reserved_bytes = 0;
    for (const std::size_t frame : model_.Reservations().PriorityOrder()) {
        if (desired.size() == model_.Reservations().Staging().Capacity()) break;
        const ImageRecord& image = model_.Frame(frame);
        if (image.Failed()) continue;
        bool gpu_texture_complete = false;
        if (image.GpuTextureReservation() != kInvalidReservation) {
            const GpuTextureSlot& gpu_texture = resources_.View().GpuTexture(
                image.GpuTextureReservation());
            gpu_texture_complete =
                gpu_texture.ReservedFrame() == frame &&
                gpu_texture.State() != GpuTextureSlotState::Writable;
        }
        if (gpu_texture_complete) continue;
        const auto bytes = StagingReservationBytes(
            model_.CatalogItemAt(frame), limits_.staging_cache_bytes);
        if (!bytes || *bytes > limits_.staging_cache_bytes) {
            model_.MarkFailed(frame);
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.staging_cache_bytes,
                            reserved_bytes)) {
            scheduler_policy::AppendUniqueFrame(
                desired, frame, model_.Reservations().Staging().Capacity());
        }
    }
    model_.Reservations().Staging().Reconcile(
        desired,
        [&](const ReservationId, const std::size_t frame) {
            const ImageRecord& image = model_.Frame(frame);
            if (image.StagingSlot() == kInvalidSlot) return true;
            const StagingSlot& slot =
                resources_.View().Staging(image.StagingSlot());
            if (slot.State() == StagingSlotState::Prepared ||
                slot.State() == StagingSlotState::DecodedPixelsAvailable) {
                return true;
            }
            if (slot.State() == StagingSlotState::DecodeOutputActive) {
                if (decode_.CancelQueued(frame)) return true;
                // A running worker owns the slot until its completion arrives.
            }
            return false;
        },
        [&](const ReservationId id, const std::size_t frame) {
            const ImageRecord& image = model_.Frame(frame);
            if (image.StagingSlot() != kInvalidSlot) {
                const StagingSlot& slot =
                    resources_.View().Staging(image.StagingSlot());
                if (slot.State() == StagingSlotState::Prepared ||
                    slot.State() == StagingSlotState::DecodedPixelsAvailable) {
                    const SlotId slot_id = image.StagingSlot();
                    resources_.ReleaseStaging(slot_id);
                    model_.ClearStagingSlot(frame, slot_id);
                }
            }
            model_.ClearStagingReservation(frame, id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            model_.AssignStagingReservation(frame, id);
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!scheduler_policy::BudgetAllows(
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
}

}  // namespace pv
