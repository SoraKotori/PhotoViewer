#include "pipeline_scheduler.h"

#include "graphics_pipeline.h"
#include "pipeline_limits.h"
#include "pipeline_resource_size.h"
#include "scheduler_reservation_policy.h"

namespace pv {

__forceinline void PipelineScheduler::ReconcileGpuReservations() {
    std::vector<std::size_t>& desired =
        model_.Reservations().PrepareGpuDesired();
    std::size_t reserved_bytes = 0;
    for (const std::size_t frame :
         model_.Reservations().DesiredGpuTextures()) {
        const ImageRecord& image = model_.Frame(frame);
        if (image.Failed()) continue;
        const auto bytes = GpuReservationBytes(
            model_.CatalogItemAt(frame), limits_.gpu_cache_bytes);
        if (!bytes || *bytes > limits_.gpu_cache_bytes) {
            model_.MarkFailed(frame);
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.gpu_cache_bytes, reserved_bytes)) {
            scheduler_policy::AppendUniqueFrame(
                desired, frame,
                model_.Reservations().GpuTextures().Capacity());
        }
    }

    model_.Reservations().GpuTextures().Reconcile(
        desired,
        [&](const ReservationId id, const std::size_t) {
            const GpuTextureSlotState state =
                resources_.View().GpuTexture(id).State();
            return state == GpuTextureSlotState::Writable ||
                   state == GpuTextureSlotState::Readable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            model_.ClearGpuTextureReservation(frame, id);
            graphics_.ClearReservation(id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            model_.AssignGpuTextureReservation(frame, id);
            resources_.ReserveGpuTexture(id, frame,
                                         model_.Frame(frame).Generation());
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!scheduler_policy::BudgetAllows(
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
                    resources_.View().GpuTexture(id).ContentFrame() == frame) {
                    return id;
                }
            }
            return ReservationTable::FirstFree(frame, entries);
        });
}

}  // namespace pv
