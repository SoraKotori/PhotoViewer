#include "pipeline_scheduler.h"

#include "decode_pipeline.h"
#include "pipeline_limits.h"
#include "pipeline_resource_size.h"
#include "scheduler_reservation_policy.h"
#include "storage_pipeline.h"

namespace pv {

__forceinline void PipelineScheduler::ReconcileCompressedReservations() {
    std::vector<std::size_t>& desired =
        model_.Reservations().PrepareCompressedDesired();
    std::size_t reserved_bytes = 0;
    for (const std::size_t frame : model_.Reservations().PriorityOrder()) {
        if (desired.size() == model_.Reservations().Compressed().Capacity()) {
            break;
        }
        const PipelineStage stage = StageOf(frame);
        if (stage == PipelineStage::Failed ||
            stage == PipelineStage::DecodeQueued ||
            stage == PipelineStage::DecodedStagingAvailable ||
            stage == PipelineStage::Uploading ||
            stage == PipelineStage::PresentationTextureAvailable) {
            continue;
        }
        const auto bytes = CompressedReservationBytes(
            model_.CatalogItemAt(frame), limits_.compressed_budget_bytes,
            storage_.CompressedAlignment());
        if (!bytes || *bytes > limits_.compressed_budget_bytes) {
            model_.MarkFailed(frame);
            continue;
        }
        if (AddWithinBudget(*bytes, limits_.compressed_budget_bytes,
                            reserved_bytes)) {
            scheduler_policy::AppendUniqueFrame(
                desired, frame, model_.Reservations().Compressed().Capacity());
        }
    }
    model_.Reservations().Compressed().Reconcile(
        desired,
        [&](const ReservationId id, const std::size_t frame) {
            const ImageRecord& image = model_.Frame(frame);
            if (image.IoActive()) {
                // Cancel once when a submitted read enters retirement; the
                // kernel owns its slot until the completion packet arrives.
                if (!model_.Reservations().Compressed().IsRetiring(id)) {
                    storage_.RetireRead(frame);
                }
                return false;
            }
            if (image.CompressedSlot() == kInvalidSlot) return true;
            return resources_.View()
                       .Compressed(image.CompressedSlot())
                       .State() ==
                   CompressedSlotState::CompressedDataAvailable;
        },
        [&](const ReservationId id, const std::size_t frame) {
            const ImageRecord& image = model_.Frame(frame);
            if (image.CompressedSlot() != kInvalidSlot) {
                decode_.ReleaseCompressed(frame);
            }
            model_.ClearCompressedReservation(frame, id);
        },
        [&](const ReservationId id, const std::size_t frame) {
            model_.AssignCompressedReservation(frame, id);
            const CatalogItem& item = model_.CatalogItemAt(frame);
            if (item.file_size_known && item.file_bytes == 0) {
                model_.MarkFailed(frame);
            } else {
                model_.ClearFailure(frame);
            }
        },
        [&](const std::size_t frame,
            const std::vector<ReservationEntry>& entries) {
            if (!scheduler_policy::BudgetAllows(
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
}

}  // namespace pv
