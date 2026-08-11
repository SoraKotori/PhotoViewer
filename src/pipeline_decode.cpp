#include "app.h"

#include "common.h"

namespace pv {
namespace {

std::optional<std::size_t> DecodeStagingBytes(const PngInfo& png) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(png.width) * 4;
    if (png.decoded_bytes > std::numeric_limits<std::size_t>::max() - png.height) {
        return std::nullopt;
    }
    const std::size_t filtered_bytes = png.decoded_bytes + png.height;
    if (filtered_bytes > std::numeric_limits<std::size_t>::max() - row_bytes) {
        return std::nullopt;
    }
    return filtered_bytes + row_bytes;
}

}  // namespace

void PipelineRuntime::OnWorkerComplete() {
    completion_queue_.DrainAll(completion_batch_);
    CompletionQueue::Batch& batch = completion_batch_;
    if (batch.results.empty() && batch.released_inputs.empty()) return;
    for (ReleasedInput& input : batch.released_inputs) {
        if (input.compressed_slot == kInvalidSlot) continue;
        CompressedSlot& slot = state_.slots.Compressed(input.compressed_slot);
        if (state_.compressed_bytes >= slot.resource.size) {
            state_.compressed_bytes -= slot.resource.size;
        }
        const std::size_t frame = slot.image;
        if (frame < state_.images.size()) {
            ImageRecord& image = state_.images[frame];
            if (image.generation == input.generation &&
                image.compressed_slot == input.compressed_slot) {
                image.compressed_slot = kInvalidSlot;
            }
        }
        state_.slots.ReleaseCompressed(input.compressed_slot);
    }
    for (DecodeResult& result : batch.results) {
        if (result.staging_slot == kInvalidSlot) continue;
        StagingSlot& slot = state_.slots.StagingAt(result.staging_slot);
        const bool cpu_decode = slot.resource.cpu_surface;
        if (!cpu_decode) graphics_.UnmapDecodeStaging(slot.resource);
        const std::size_t frame = slot.image;
        if (frame >= state_.images.size()) {
            state_.slots.ReleaseStaging(result.staging_slot);
            continue;
        }
        ImageRecord& image = state_.images[frame];
        if (frame == state_.navigation.CurrentIndex() &&
            state_.navigation.InitialPending() &&
            validation_.At(StartupMilestone::InitialDecodeCompleted) ==
                std::chrono::steady_clock::time_point{}) {
            validation_.Mark(StartupMilestone::InitialDecodeCompleted);
        }
        if (result.generation != image.generation) {
            state_.slots.ReleaseStaging(result.staging_slot);
            continue;
        }
        image.work_active = false;
        const bool reserved = ReservationActive(
            state_.reservations.Staging(), image.staging_reservation,
            frame);
        if (result.success && reserved) {
            slot.state = StagingSlotState::DecodedPixelsAvailable;
        } else {
            state_.slots.ReleaseStaging(result.staging_slot);
            image.staging_slot = kInvalidSlot;
            if (FAILED(result.error) && reserved) {
                image.failed = true;
            }
        }
    }
    PumpPipeline();
}

bool PipelineRuntime::CancelQueuedDecode(const std::size_t frame) {
    if (frame >= state_.images.size()) return false;
    ImageRecord& image = state_.images[frame];
    if (!image.work_active || image.staging_slot == kInvalidSlot) return false;
    DecodeWork cancelled;
    if (!work_queue_.TryCancel(image.staging_slot, cancelled)) {
        return false;
    }

    if (cancelled.staging_slot != kInvalidSlot) {
        graphics_.UnmapDecodeStaging(
            state_.slots.StagingAt(cancelled.staging_slot).resource);
        state_.slots.ReleaseStaging(cancelled.staging_slot);
        if (image.staging_slot == cancelled.staging_slot) {
            image.staging_slot = kInvalidSlot;
        }
    }
    if (cancelled.compressed_slot != kInvalidSlot) {
        CompressedSlot& compressed = state_.slots.Compressed(
            cancelled.compressed_slot);
        if (ReservationActive(state_.reservations.Compressed(),
                              image.compressed_reservation, frame)) {
            compressed.state = CompressedSlotState::CompressedDataAvailable;
        } else {
            ReleaseCompressed(image);
        }
    }
    image.work_active = false;
    return true;
}

void PipelineRuntime::PrepareStagingForImage(const std::size_t index) {
    if (index >= state_.images.size()) return;
    ImageRecord& image = state_.images[index];
    if (image.failed || image.staging_slot != kInvalidSlot ||
        !ReservationActive(state_.reservations.Staging(),
                           image.staging_reservation, index)) {
        return;
    }
    const CatalogItem& item = state_.catalog.items[index];
    if (!item.header_valid) return;
    const std::optional<std::size_t> staging_bytes =
        DecodeStagingBytes(item.png);
    if (!staging_bytes || *staging_bytes == 0 ||
        *staging_bytes > config_.staging_cache_bytes) {
        return;
    }
    const SlotId staging_slot = state_.slots.AcquireStaging(
        *staging_bytes, index, image.generation);
    if (staging_slot == kInvalidSlot) return;
    DecodeStaging& staging = state_.slots.StagingAt(staging_slot).resource;
    if (graphics_device_ready_) {
        graphics_.PrepareDecodeStaging(staging, item.png.width, item.png.height);
    }
    image.staging_slot = staging_slot;
}

void PipelineRuntime::DispatchDecodes() {
    for (const std::size_t index : state_.reservations.PriorityOrder()) {
        if (StageOf(state_.images[index]) != PipelineStage::CompressedReady) {
            continue;
        }
        ImageRecord& image = state_.images[index];
        if (!ReservationActive(state_.reservations.Staging(),
                               image.staging_reservation, index)) {
            continue;
        }
        if (!state_.catalog.items[index].header_valid) {
            ReleaseCompressed(image);
            image.failed = true;
            continue;
        }
        const CatalogItem& item = state_.catalog.items[index];
        const std::optional<std::size_t> staging_bytes =
            DecodeStagingBytes(item.png);
        if (!staging_bytes || *staging_bytes == 0 ||
            *staging_bytes > config_.staging_cache_bytes) {
            ReleaseCompressed(image);
            image.failed = true;
            continue;
        }
        PrepareStagingForImage(index);
        const SlotId staging_slot = image.staging_slot;
        if (staging_slot == kInvalidSlot) continue;
        StagingSlot& staging_state = state_.slots.StagingAt(staging_slot);
        if (staging_state.state != StagingSlotState::Prepared ||
            staging_state.image != index ||
            staging_state.generation != image.generation) {
            throw std::logic_error("invalid prepared staging slot");
        }
        if (graphics_device_ready_) {
            graphics_.MapDecodeStaging(staging_state.resource,
                                       item.png.width, item.png.height,
                                       item.png.decoded_bytes);
        } else if (!staging_state.resource.cpu_surface &&
                   !staging_state.resource.PrepareCpuSurface(
                       item.png.width, item.png.height,
                       item.png.decoded_bytes)) {
            state_.slots.ReleaseStaging(staging_slot);
            image.staging_slot = kInvalidSlot;
            continue;
        }
        staging_state.state = StagingSlotState::DecodeOutputActive;
        DecodeWork work{index, image.generation, image.compressed_slot,
                        staging_slot};
        state_.slots.Compressed(image.compressed_slot).state =
            CompressedSlotState::DecodeInput;
        if (!work_queue_.TryPush(work)) {
            graphics_.UnmapDecodeStaging(staging_state.resource);
            staging_state.state = StagingSlotState::Prepared;
            state_.slots.Compressed(image.compressed_slot).state =
                CompressedSlotState::CompressedDataAvailable;
            break;
        }
        image.work_active = true;
        if (state_.navigation.InitialPending() &&
            index == state_.navigation.CurrentIndex() &&
            validation_.At(StartupMilestone::InitialDecodeSubmitted) ==
                std::chrono::steady_clock::time_point{}) {
            validation_.Mark(StartupMilestone::InitialDecodeSubmitted);
        }
    }
}

void PipelineRuntime::ReleaseCompressed(ImageRecord& image) {
    if (image.compressed_slot == kInvalidSlot) return;
    CompressedSlot& slot = state_.slots.Compressed(image.compressed_slot);
    if (state_.compressed_bytes >= slot.resource.size) {
        state_.compressed_bytes -= slot.resource.size;
    }
    state_.slots.ReleaseCompressed(image.compressed_slot);
    image.compressed_slot = kInvalidSlot;
}

}  // namespace pv





