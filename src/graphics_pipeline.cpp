#include "graphics_pipeline.h"

#include "pipeline_model.h"
#include "pipeline_observer.h"
#include "pipeline_resources.h"
#include "presentation_order.h"
#include "pipeline_state.h"
#include "viewer_window.h"
#include "win32_support.h"

namespace pv {
namespace {

bool NavigationInputPending(const HWND window) noexcept {
    MSG message{};
    return PeekMessageW(&message, window, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) ||
           PeekMessageW(&message, window, kMessageValidationStep,
                        kMessageValidationStep, PM_NOREMOVE);
}

}  // namespace

GraphicsPipeline::GraphicsPipeline(const PipelineLimits& limits,
                                   const PipelineModel& model,
                                   PresentationCompletionAccess
                                       presentation_completion,
                                   GraphicsFrameAccess frames,
                                   const PipelineResources& resources,
                                   GraphicsResourceAccess slots,
                                   PipelineObserver& observer,
                                   ViewerWindow& window)
    : limits_(limits), model_(model),
      presentation_completion_(presentation_completion), frames_(frames),
      resources_(resources),
      slots_(slots), observer_(observer), window_(window),
      uploads_(limits.staging_slot_count),
      gpu_budget_(limits.GpuSlotCount(), limits.gpu_cache_bytes) {}

void GraphicsPipeline::InitializeDevice() {
    graphics_.InitializeDirect3D(window_.Handle());
    device_ready_ = true;
}

void GraphicsPipeline::Initialize2D() {
    graphics_.InitializeDirect2D();
    upload_ready_ = true;
}

void GraphicsPipeline::InitializeSwapChain() {
    graphics_.InitializeSwapChain();
}

void GraphicsPipeline::InitializeBackBufferTarget() {
    graphics_.InitializeBackBufferTarget();
    ready_ = true;
}

bool GraphicsPipeline::DeviceReady() const noexcept { return device_ready_; }
bool GraphicsPipeline::UploadReady() const noexcept { return upload_ready_; }
bool GraphicsPipeline::Ready() const noexcept { return ready_; }

HANDLE GraphicsPipeline::FrameWaitEvent() const noexcept {
    return presentation_.NeedsFrameCreditEvent()
               ? graphics_.FrameWaitableObject()
               : nullptr;
}

HANDLE GraphicsPipeline::GpuWaitEvent() const noexcept {
    return (!uploads_.Empty() || presentation_.DrawFence() != 0)
               ? graphics_.FenceEvent()
               : nullptr;
}

bool GraphicsPipeline::HandleGpuCompletion() {
    const UINT64 completed = graphics_.CompletedFenceValue();
    if (const auto completed_slot = presentation_.CompleteDraw(completed)) {
        const GpuTextureSlot& gpu_texture =
            slots_.GpuTexture(*completed_slot);
        if (gpu_texture.State() == GpuTextureSlotState::Reading) {
            slots_.CompleteGpuRead(*completed_slot);
        }
    }
    while (auto completed_ticket = uploads_.TakeCompleted(completed)) {
        UploadTicket ticket = std::move(*completed_ticket);
        if (ticket.index >= model_.FrameCount()) {
            slots_.ReleaseStaging(ticket.staging_slot);
            continue;
        }
        const ImageRecord& image = frames_.View(ticket.index);
        const GpuTextureSlot& gpu_texture = slots_.GpuTexture(
            ticket.gpu_texture_slot);
        const bool keep_gpu = ticket.generation == image.Generation() &&
            ReservationActive(model_.ReservationPlan().GpuTextures(),
                              image.GpuTextureReservation(), ticket.index) &&
            image.GpuTextureReservation() == ticket.gpu_texture_slot &&
            gpu_texture.ReservedFrame() == ticket.index &&
            gpu_texture.ReservationGeneration() == ticket.generation;
        if (keep_gpu) {
            graphics_.FinishUpload(
                slots_.GpuResource(ticket.gpu_texture_slot));
            slots_.CompleteGpuUpload(
                ticket.gpu_texture_slot, ticket.index, ticket.generation,
                ticket.bytes, true);
            observer_.OnFrameReady(ticket.index);
        } else {
            slots_.CompleteGpuUpload(
                ticket.gpu_texture_slot, ticket.index, ticket.generation,
                ticket.bytes, false);
        }
        slots_.ReleaseStaging(ticket.staging_slot);
        if (image.StagingSlot() == ticket.staging_slot) {
            frames_.ClearStagingSlot(ticket.index, ticket.staging_slot);
        }
    }
    presentation_.SetArmedFence(0);
    ArmOldestFence();
    return true;
}

void GraphicsPipeline::GrantFrameCredit() noexcept {
    presentation_.GrantFrameCredit();
}

void GraphicsPipeline::Resize(const UINT width, const UINT height) {
    if (width == 0 || height == 0) return;
    graphics_.Resize(width, height);
    presentation_.RequestRedraw(true);
}

void GraphicsPipeline::Paint() {
    PAINTSTRUCT paint{};
    BeginPaint(window_.Handle(), &paint);
    EndPaint(window_.Handle(), &paint);
    presentation_.RequestRedraw(false);
}

bool GraphicsPipeline::HasReadableTexture(const std::size_t frame) const noexcept {
    if (frame >= model_.FrameCount()) return false;
    const ImageRecord& image = model_.FrameView(frame);
    if (!ReservationActive(model_.ReservationPlan().GpuTextures(),
                           image.GpuTextureReservation(), frame)) {
        return false;
    }
    const GpuTextureSlot& gpu_texture = slots_.GpuTexture(
        image.GpuTextureReservation());
    return gpu_texture.ReservedFrame() == frame &&
           gpu_texture.ReservationGeneration() == image.Generation() &&
           gpu_texture.ContentFrame() == frame &&
           gpu_texture.ContentGeneration() == image.Generation() &&
           (gpu_texture.State() == GpuTextureSlotState::Readable ||
            gpu_texture.State() == GpuTextureSlotState::Reading);
}

void GraphicsPipeline::SubmitEligibleUploads() {
    if (!upload_ready_) return;
    for (const std::size_t index : model_.ReservationPlan().PriorityOrder()) {
        if (DeterminePipelineStage(index, frames_.View(index), resources_,
                                   model_.ReservationPlan()) !=
            PipelineStage::DecodedStagingAvailable) {
            continue;
        }
        const ImageRecord& image = frames_.View(index);
        if (!ReservationActive(model_.ReservationPlan().GpuTextures(),
                               image.GpuTextureReservation(), index)) {
            continue;
        }
        const GpuTextureSlot& gpu_texture = slots_.GpuTexture(
            image.GpuTextureReservation());
        if (gpu_texture.State() != GpuTextureSlotState::Writable ||
            gpu_texture.ReservedFrame() != index) {
            continue;
        }
        DecodeStaging& staging =
            slots_.StagingResource(image.StagingSlot());
        GpuImage& gpu_resource =
            slots_.GpuResource(image.GpuTextureReservation());
        const std::size_t bytes =
            staging.resource_plan.gpu_reservation_bytes;
        const SlotId gpu_slot = image.GpuTextureReservation();
        if (!MakeGpuBytesAvailable(gpu_slot, bytes)) continue;
        if (staging.cpu_surface) {
            graphics_.CopyDecodedToStaging(staging);
        }
        UploadTicket ticket = graphics_.SubmitUpload(
            index, image.Generation(), image.StagingSlot(),
            staging, gpu_resource);
        ticket.gpu_texture_slot = image.GpuTextureReservation();
        gpu_budget_.CommitReplacement(gpu_slot, bytes);
        slots_.BeginGpuCopy(image.StagingSlot());
        slots_.BeginGpuUpload(image.GpuTextureReservation());
        uploads_.Queue(std::move(ticket));
        if (NavigationInputPending(window_.Handle())) break;
    }
    ArmOldestFence();
}

void GraphicsPipeline::ClearReservation(const SlotId id) {
    slots_.ClearGpuTextureReservation(id);
}

bool GraphicsPipeline::MakeGpuBytesAvailable(const SlotId destination,
                                             const std::size_t bytes) {
    if (gpu_budget_.CanReplace(destination, bytes)) return true;
    for (SlotId id = 0; id < slots_.GpuTextureCount(); ++id) {
        if (id == destination) continue;
        const GpuTextureSlot& slot = slots_.GpuTexture(id);
        const bool reusable =
            slot.ReservedFrame() != kInvalidFrame &&
            slot.ContentFrame() == slot.ReservedFrame() &&
            slot.ContentGeneration() == slot.ReservationGeneration() &&
            slot.Resource().bitmap;
        if (slot.State() != GpuTextureSlotState::Writable || reusable ||
            slot.Resource().bytes == 0) {
            continue;
        }
        const std::size_t released =
            slots_.ReleaseReplaceableGpuContent(id);
        if (gpu_budget_.Release(id) != released) {
            throw std::logic_error("GPU slot byte accounting mismatch");
        }
        if (gpu_budget_.CanReplace(destination, bytes)) return true;
    }
    return false;
}

bool GraphicsPipeline::TryPresent() {
    if (!presentation_.CanDraw()) return false;
    const auto pending = model_.NavigationView().NextIndex();
    if (pending) {
        const auto next = NextPresentableFrame(
            model_.NavigationView(),
            [&](const std::size_t frame) { return HasReadableTexture(frame); });
        if (!next) return false;
        const ImageRecord& image = frames_.View(*next);
        const SlotId gpu_texture_id = image.GpuTextureReservation();
        const UINT64 draw_fence =
            graphics_.Draw(slots_.GpuResource(gpu_texture_id));
        slots_.BeginGpuRead(gpu_texture_id);
        presentation_.StartDraw(gpu_texture_id, draw_fence);
        ArmOldestFence();
        presentation_completion_.Complete(*next);
        observer_.OnFramePresented(*next);
        return true;
    }
    if (presentation_.RedrawPending()) {
        const std::size_t current_index = model_.NavigationView().CurrentIndex();
        const ImageRecord& current = frames_.View(current_index);
        if (HasReadableTexture(current_index)) {
            const SlotId gpu_texture_id = current.GpuTextureReservation();
            const UINT64 draw_fence =
                graphics_.Draw(slots_.GpuResource(gpu_texture_id));
            slots_.BeginGpuRead(gpu_texture_id);
            presentation_.StartDraw(gpu_texture_id, draw_fence);
            ArmOldestFence();
            return true;
        }
    }
    return false;
}

void GraphicsPipeline::ArmOldestFence() {
    UINT64 value = presentation_.DrawFence();
    const UINT64 upload_fence = uploads_.OldestFence();
    if (upload_fence != 0 && (value == 0 || upload_fence < value)) {
        value = upload_fence;
    }
    if (value == 0) {
        presentation_.SetArmedFence(0);
        return;
    }
    if (presentation_.ArmedFence() != value) {
        graphics_.ArmFence(value);
        presentation_.SetArmedFence(value);
    }
}

void GraphicsPipeline::PrepareDecodeStaging(DecodeStaging& staging) {
    graphics_.PrepareDecodeStaging(staging);
}

void GraphicsPipeline::MapDecodeStaging(DecodeStaging& staging) {
    graphics_.MapDecodeStaging(staging);
}

void GraphicsPipeline::UnmapDecodeStaging(DecodeStaging& staging) {
    graphics_.UnmapDecodeStaging(staging);
}

void GraphicsPipeline::RequestRedraw(const bool erase) noexcept {
    presentation_.RequestRedraw(erase);
}

bool GraphicsPipeline::ReservationActive(const ReservationTable& table,
                                         const ReservationId id,
                                         const std::size_t frame) const noexcept {
    return IsReservationActive(table, id, frame);
}

void GraphicsPipeline::DrainForShutdown() noexcept {
    if (!upload_ready_) return;
    try {
        constexpr ULONGLONG shutdown_timeout_ms = 5000;
        const ULONGLONG deadline = GetTickCount64() + shutdown_timeout_ms;
        while (!uploads_.Empty() || presentation_.DrawFence() != 0) {
            ArmOldestFence();
            const HANDLE event = graphics_.FenceEvent();
            const ULONGLONG now = GetTickCount64();
            if (!event || now >= deadline ||
                WaitForSingleObject(
                    event, static_cast<DWORD>(deadline - now)) !=
                    WAIT_OBJECT_0) {
                break;
            }
            (void)HandleGpuCompletion();
        }
    } catch (...) {
        // Destruction cannot surface a graphics failure. D3D retains submitted
        // resources until the device and context are destroyed.
    }
}

void GraphicsPipeline::UnmapAllStagingForShutdown() noexcept {
    if (!device_ready_) return;
    try {
        for (SlotId id = 0; id < slots_.StagingCount(); ++id) {
            if (slots_.Staging(id).State() !=
                StagingSlotState::Free) {
                graphics_.UnmapDecodeStaging(
                    slots_.StagingResource(id));
            }
        }
    } catch (...) {
    }
}

void GraphicsPipeline::ResetMetrics() noexcept { graphics_.ResetMetrics(); }
std::size_t GraphicsPipeline::UploadCount() const noexcept {
    return uploads_.Count();
}
std::size_t GraphicsPipeline::GpuBytes() const noexcept {
    return gpu_budget_.Committed();
}
std::uint64_t GraphicsPipeline::SubmittedUploadCount() const noexcept {
    return graphics_.UploadCount();
}
std::uint64_t GraphicsPipeline::UploadNanoseconds() const noexcept {
    return graphics_.UploadNanoseconds();
}
std::uint64_t GraphicsPipeline::DrawCount() const noexcept {
    return graphics_.DrawCount();
}
std::uint64_t GraphicsPipeline::DrawNanoseconds() const noexcept {
    return graphics_.DrawNanoseconds();
}

}  // namespace pv
