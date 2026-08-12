#include "pipeline_state.h"

#include "pipeline_resources.h"
#include "reservation_planner.h"

namespace pv {

bool IsReservationActive(const ReservationTable& table,
                         const ReservationId id,
                         const std::size_t frame) noexcept {
    return table.IsActive(id) && table.At(id).frame == frame;
}

PipelineStage DeterminePipelineStage(
    const std::size_t frame, const ImageRecord& image,
    const PipelineResources& resources,
    const ReservationPlanner& reservations) noexcept {
    if (image.Failed()) return PipelineStage::Failed;
    const ResourceSlots& slots = resources.SlotsView();
    if (image.GpuTextureReservation() != kInvalidReservation &&
        image.GpuTextureReservation() < slots.GpuTextureCount()) {
        const GpuTextureSlot& gpu_texture =
            slots.GpuTexture(image.GpuTextureReservation());
        if (gpu_texture.ReservedFrame() == frame) {
            switch (gpu_texture.State()) {
            case GpuTextureSlotState::Writing:
                return PipelineStage::Uploading;
            case GpuTextureSlotState::Readable:
            case GpuTextureSlotState::Reading:
                return PipelineStage::PresentationTextureAvailable;
            case GpuTextureSlotState::Writable:
            case GpuTextureSlotState::Inactive:
                break;
            }
        }
    }
    if (image.StagingSlot() != kInvalidSlot) {
        switch (slots.Staging(image.StagingSlot()).State()) {
        case StagingSlotState::Prepared:
            break;
        case StagingSlotState::DecodeOutputActive:
            return PipelineStage::DecodeQueued;
        case StagingSlotState::DecodedPixelsAvailable:
            return PipelineStage::DecodedStagingAvailable;
        case StagingSlotState::GpuCopySource:
            return PipelineStage::Uploading;
        case StagingSlotState::Free:
            break;
        }
    }
    if (image.CompressedSlot() != kInvalidSlot) {
        switch (slots.Compressed(image.CompressedSlot()).State()) {
        case CompressedSlotState::FileReadDestination:
            return PipelineStage::IoInFlight;
        case CompressedSlotState::CompressedDataAvailable:
            return PipelineStage::CompressedReady;
        case CompressedSlotState::DecodeInput:
            return PipelineStage::DecodeQueued;
        case CompressedSlotState::CancellationPending:
            return PipelineStage::CancelPending;
        case CompressedSlotState::Free:
            break;
        }
    }
    return IsReservationActive(reservations.Compressed(),
                               image.CompressedReservation(), frame)
               ? PipelineStage::WaitingIo
               : PipelineStage::Outside;
}

}  // namespace pv
