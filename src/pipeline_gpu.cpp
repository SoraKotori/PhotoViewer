#include "app.h"

#include "common.h"

namespace pv {
namespace {

bool NavigationInputPending(const HWND window) noexcept {
    MSG message{};
    return PeekMessageW(&message, window, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) ||
           PeekMessageW(&message, window, kMessageValidationStep,
                        kMessageValidationStep, PM_NOREMOVE);
}

}  // namespace

void PipelineRuntime::OnGpuComplete() {
    const UINT64 completed = graphics_.CompletedFenceValue();
    if (state_.reading_gpu_texture_fence != 0 &&
        state_.reading_gpu_texture_fence <= completed) {
        if (state_.reading_gpu_texture_slot != kInvalidSlot) {
            GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
                state_.reading_gpu_texture_slot);
            if (gpu_texture.state == GpuTextureSlotState::Reading) {
                gpu_texture.state = gpu_texture.reserved_frame == gpu_texture.content_frame
                                   ? GpuTextureSlotState::Readable
                                   : GpuTextureSlotState::Writable;
            }
        }
        state_.reading_gpu_texture_slot = kInvalidSlot;
        state_.reading_gpu_texture_fence = 0;
    }
    while (!state_.uploads.empty() &&
           state_.uploads.front().fence_value <= completed) {
        UploadTicket ticket = std::move(state_.uploads.front());
        state_.uploads.pop_front();
        if (ticket.index >= state_.images.size()) {
            state_.slots.ReleaseStaging(ticket.staging_slot);
            continue;
        }
        ImageRecord& image = state_.images[ticket.index];
        GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
            ticket.gpu_texture_slot);
        const bool keep_gpu = ticket.generation == image.generation &&
            ReservationActive(state_.reservations.GpuTextures(),
                              image.gpu_texture_reservation, ticket.index) &&
            image.gpu_texture_reservation == ticket.gpu_texture_slot &&
            gpu_texture.reserved_frame == ticket.index;
        if (keep_gpu) {
            graphics_.FinishUpload(gpu_texture.resource);
            gpu_texture.content_frame = ticket.index;
            gpu_texture.state = GpuTextureSlotState::Readable;
            observer_.OnFrameReady(ticket.index);
        } else {
            gpu_texture.content_frame = ticket.index;
            gpu_texture.state = GpuTextureSlotState::Writable;
        }
        state_.slots.ReleaseStaging(ticket.staging_slot);
        if (image.staging_slot == ticket.staging_slot) {
            image.staging_slot = kInvalidSlot;
        }
    }
    state_.armed_fence = 0;
    ArmOldestFence();
    PumpPipeline();
}

void PipelineRuntime::OnFrameCredit() {
    state_.frame_credit = true;
    PumpPipeline();
}

void PipelineRuntime::OnSurfaceChanged(const UINT width, const UINT height) {
    if (width == 0 || height == 0) return;
    graphics_.Resize(width, height);
    state_.redraw_pending = true;
    state_.frame_credit = true;
    PumpPipeline();
}

void PipelineRuntime::OnPaint() {
    PAINTSTRUCT paint{};
    BeginPaint(window_.Handle(), &paint);
    EndPaint(window_.Handle(), &paint);
    state_.redraw_pending = true;
    if (graphics_ready_) PumpPipeline();
}

bool PipelineRuntime::HasReadableGpuTexture(const std::size_t frame) const noexcept {
    if (frame >= state_.images.size()) return false;
    const ImageRecord& image = state_.images[frame];
    if (!ReservationActive(state_.reservations.GpuTextures(),
                           image.gpu_texture_reservation, frame)) {
        return false;
    }
    const GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
        image.gpu_texture_reservation);
    return gpu_texture.reserved_frame == frame && gpu_texture.content_frame == frame &&
           (gpu_texture.state == GpuTextureSlotState::Readable ||
            gpu_texture.state == GpuTextureSlotState::Reading);
}

void PipelineRuntime::SubmitUploads() {
    if (catalog_loading_ || !graphics_device_ready_) return;
    for (const std::size_t index : state_.reservations.PriorityOrder()) {
        if (StageOf(state_.images[index]) !=
            PipelineStage::DecodedStagingAvailable) {
            continue;
        }
        ImageRecord& image = state_.images[index];
        if (!ReservationActive(state_.reservations.GpuTextures(),
                               image.gpu_texture_reservation, index)) {
            continue;
        }
        GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(
            image.gpu_texture_reservation);
        if (gpu_texture.state != GpuTextureSlotState::Writable ||
            gpu_texture.reserved_frame != index) {
            continue;
        }
        StagingSlot& staging_slot = state_.slots.StagingAt(
            image.staging_slot);
        const std::size_t bytes = staging_slot.resource.surface.ByteSize();
        if (bytes == 0 || bytes > config_.gpu_cache_bytes) {
            state_.slots.ReleaseStaging(image.staging_slot);
            image.staging_slot = kInvalidSlot;
            image.failed = true;
            continue;
        }
        const std::size_t old_bytes = gpu_texture.resource.bytes;
        const std::size_t retained_bytes = state_.gpu_bytes >= old_bytes
                                               ? state_.gpu_bytes - old_bytes
                                               : 0;
        if (bytes > config_.gpu_cache_bytes -
                        std::min(retained_bytes, config_.gpu_cache_bytes)) {
            continue;
        }
        if (staging_slot.resource.cpu_surface) {
            graphics_.CopyDecodedToStaging(staging_slot.resource);
        }
        UploadTicket ticket = graphics_.SubmitUpload(
            index, image.generation, image.staging_slot,
            staging_slot.resource, gpu_texture.resource);
        ticket.gpu_texture_slot = image.gpu_texture_reservation;
        state_.gpu_bytes = retained_bytes + gpu_texture.resource.bytes;
        staging_slot.state = StagingSlotState::GpuCopySource;
        gpu_texture.content_frame = kInvalidFrame;
        gpu_texture.state = GpuTextureSlotState::Writing;
        state_.uploads.push_back(std::move(ticket));
        if (NavigationInputPending(window_.Handle())) break;
    }
    ArmOldestFence();
}

bool PipelineRuntime::TryPresent() {
    if (!state_.frame_credit || state_.reading_gpu_texture_fence != 0) return false;
    const auto next = state_.navigation.NextIndex();
    if (next) {
        ImageRecord& image = state_.images[*next];
        if (!HasReadableGpuTexture(*next)) return false;
        const SlotId gpu_texture_id = image.gpu_texture_reservation;
        GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(gpu_texture_id);
        state_.reading_gpu_texture_fence = graphics_.Draw(gpu_texture.resource);
        gpu_texture.state = GpuTextureSlotState::Reading;
        state_.reading_gpu_texture_slot = gpu_texture_id;
        ArmOldestFence();
        state_.frame_credit = false;
        state_.redraw_pending = false;
        state_.navigation.CompletePresentation(*next);
        state_.reservations.MarkDirty();
        observer_.OnFramePresented(*next);
        return true;
    }
    if (state_.redraw_pending) {
        const std::size_t current_index = state_.navigation.CurrentIndex();
        ImageRecord& current = state_.images[current_index];
        if (HasReadableGpuTexture(current_index)) {
            const SlotId gpu_texture_id = current.gpu_texture_reservation;
            GpuTextureSlot& gpu_texture = state_.slots.GpuTextureAt(gpu_texture_id);
            state_.reading_gpu_texture_fence = graphics_.Draw(gpu_texture.resource);
            gpu_texture.state = GpuTextureSlotState::Reading;
            state_.reading_gpu_texture_slot = gpu_texture_id;
            ArmOldestFence();
            state_.frame_credit = false;
            state_.redraw_pending = false;
            return true;
        }
    }
    return false;
}

void PipelineRuntime::ArmOldestFence() {
    UINT64 value = state_.reading_gpu_texture_fence;
    if (!state_.uploads.empty() &&
        (value == 0 || state_.uploads.front().fence_value < value)) {
        value = state_.uploads.front().fence_value;
    }
    if (value == 0) {
        state_.armed_fence = 0;
        return;
    }
    if (state_.armed_fence != value) {
        graphics_.ArmFence(value);
        state_.armed_fence = value;
    }
}

}  // namespace pv




