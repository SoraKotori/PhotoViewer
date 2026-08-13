#pragma once

#include "pipeline_limits.h"
#include "resource_slots.h"

#include <memory>

namespace pv {

class StorageResourceAccess {
public:
    [[nodiscard]] SlotId AcquireCompressed(std::size_t bytes, std::size_t image,
                                           std::uint64_t generation) const {
        return slots_->AcquireCompressed(bytes, image, generation);
    }
    void CancelFileRead(SlotId id) const { slots_->CancelFileRead(id); }
    void CompleteFileRead(SlotId id) const { slots_->CompleteFileRead(id); }
    void ReleaseCompressed(SlotId id) const { slots_->ReleaseCompressed(id); }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        return slots_->Compressed(id);
    }
    [[nodiscard]] IoRequest& FileReadRequest(SlotId id) const {
        return slots_->FileReadRequest(id);
    }
    [[nodiscard]] CompressedBuffer& FileReadBuffer(SlotId id) const {
        return slots_->FileReadBuffer(id);
    }

private:
    friend class PipelineResources;
    explicit StorageResourceAccess(ResourceSlots& slots) : slots_(&slots) {}
    ResourceSlots* slots_;
};

class DecodeResourceAccess {
public:
    [[nodiscard]] SlotId AcquireStaging(std::size_t bytes, std::size_t image,
                                       std::uint64_t generation) const {
        return slots_->AcquireStaging(bytes, image, generation);
    }
    void BeginDecodeInput(SlotId id) const { slots_->BeginDecodeInput(id); }
    void BeginDecodeOutput(SlotId id) const { slots_->BeginDecodeOutput(id); }
    void CompleteDecodeOutput(SlotId id) const { slots_->CompleteDecodeOutput(id); }
    void RestoreDecodeInput(SlotId id) const { slots_->RestoreDecodeInput(id); }
    void RestoreDecodeOutput(SlotId id) const { slots_->RestoreDecodeOutput(id); }
    void ReleaseCompressed(SlotId id) const { slots_->ReleaseCompressed(id); }
    void ReleaseStaging(SlotId id) const { slots_->ReleaseStaging(id); }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        return slots_->Compressed(id);
    }
    [[nodiscard]] const StagingSlot& Staging(SlotId id) const {
        return slots_->Staging(id);
    }
    [[nodiscard]] DecodeStaging& StagingResource(SlotId id) const {
        return slots_->StagingResource(id);
    }
    [[nodiscard]] DecodeSlotAccess WorkerAccess() const noexcept {
        return DecodeSlotAccess(*slots_);
    }

private:
    friend class PipelineResources;
    explicit DecodeResourceAccess(ResourceSlots& slots) : slots_(&slots) {}
    ResourceSlots* slots_;
};

class GraphicsResourceAccess {
public:
    void BeginGpuCopy(SlotId id) const { slots_->BeginGpuCopy(id); }
    void BeginGpuRead(SlotId id) const { slots_->BeginGpuRead(id); }
    void BeginGpuUpload(SlotId id) const { slots_->BeginGpuUpload(id); }
    void ClearGpuTextureReservation(SlotId id) const {
        slots_->ClearGpuTextureReservation(id);
    }
    void CompleteGpuRead(SlotId id) const { slots_->CompleteGpuRead(id); }
    void CompleteGpuUpload(SlotId id, std::size_t frame,
                           std::uint64_t generation, std::size_t bytes,
                           bool keep) const {
        slots_->CompleteGpuUpload(id, frame, generation, bytes, keep);
    }
    void ReleaseStaging(SlotId id) const { slots_->ReleaseStaging(id); }
    [[nodiscard]] std::size_t ReleaseReplaceableGpuContent(SlotId id) const {
        return slots_->ReleaseReplaceableGpuContent(id);
    }
    [[nodiscard]] const GpuTextureSlot& GpuTexture(SlotId id) const {
        return slots_->GpuTexture(id);
    }
    [[nodiscard]] const StagingSlot& Staging(SlotId id) const {
        return slots_->Staging(id);
    }
    [[nodiscard]] GpuImage& GpuResource(SlotId id) const {
        return slots_->GpuResource(id);
    }
    [[nodiscard]] DecodeStaging& StagingResource(SlotId id) const {
        return slots_->StagingResource(id);
    }
    [[nodiscard]] std::size_t GpuTextureCount() const noexcept {
        return slots_->GpuTextureCount();
    }
    [[nodiscard]] std::size_t StagingCount() const noexcept {
        return slots_->StagingCount();
    }

private:
    friend class PipelineResources;
    explicit GraphicsResourceAccess(ResourceSlots& slots) : slots_(&slots) {}
    ResourceSlots* slots_;
};

// Grants only the process-exit escape hatch needed when cancelled kernel I/O
// does not drain within the bounded shutdown interval.
class ResourceBackingAbandonment final {
public:
    void AbandonForProcessExit() noexcept { (void)slots_->release(); }

private:
    friend class PipelineResources;
    explicit ResourceBackingAbandonment(
        std::unique_ptr<ResourceSlots>& slots) noexcept
        : slots_(&slots) {}
    std::unique_ptr<ResourceSlots>* slots_;
};

// Owns fixed slot storage and the compressed-content budget. GPU accounting
// belongs to GraphicsPipeline, the only component that can submit uploads.
class PipelineResources {
public:
    explicit PipelineResources(const PipelineLimits& limits);

    [[nodiscard]] const ResourceSlots& SlotsView() const noexcept {
        return *slots_;
    }

private:
    friend class PipelineRuntime;
    friend class PipelineScheduler;
    [[nodiscard]] StorageResourceAccess StorageAccess() noexcept {
        return StorageResourceAccess(*slots_);
    }
    [[nodiscard]] DecodeResourceAccess DecodeAccess() noexcept {
        return DecodeResourceAccess(*slots_);
    }
    [[nodiscard]] GraphicsResourceAccess GraphicsAccess() noexcept {
        return GraphicsResourceAccess(*slots_);
    }
    [[nodiscard]] ResourceBackingAbandonment BackingAbandonment() noexcept {
        return ResourceBackingAbandonment(slots_);
    }
    [[nodiscard]] ResourceSlots& Slots() noexcept { return *slots_; }
    [[nodiscard]] const ResourceSlots& Slots() const noexcept { return *slots_; }
    std::unique_ptr<ResourceSlots> slots_;
};

}  // namespace pv
