#pragma once

#include "pipeline_limits.h"
#include "linear_capability.h"
#include "resource_slots.h"

#include <memory>

namespace pv {

class StorageResourceAccess final : private LinearCapability<ResourceSlots> {
public:
    StorageResourceAccess(const StorageResourceAccess&) = delete;
    StorageResourceAccess& operator=(const StorageResourceAccess&) = delete;
    StorageResourceAccess(StorageResourceAccess&&) noexcept = default;
    StorageResourceAccess& operator=(StorageResourceAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] SlotId AcquireCompressed(std::size_t bytes, std::size_t image,
                                           std::uint64_t generation) {
        return Target().AcquireCompressed(bytes, image, generation);
    }
    void CancelFileRead(SlotId id) { Target().CancelFileRead(id); }
    void CompleteFileRead(SlotId id) { Target().CompleteFileRead(id); }
    void ReleaseCompressed(SlotId id) { Target().ReleaseCompressed(id); }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        return Target().Compressed(id);
    }
    [[nodiscard]] IoRequest& FileReadRequest(SlotId id) {
        return Target().FileReadRequest(id);
    }
    [[nodiscard]] CompressedBuffer& FileReadBuffer(SlotId id) {
        return Target().FileReadBuffer(id);
    }

private:
    friend class PipelineResources;
    explicit StorageResourceAccess(ResourceSlots& slots)
        : LinearCapability(slots) {}
};

class DecodeResourceAccess final : private LinearCapability<ResourceSlots> {
public:
    DecodeResourceAccess(const DecodeResourceAccess&) = delete;
    DecodeResourceAccess& operator=(const DecodeResourceAccess&) = delete;
    DecodeResourceAccess(DecodeResourceAccess&&) noexcept = default;
    DecodeResourceAccess& operator=(DecodeResourceAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] SlotId AcquireStaging(std::size_t bytes, std::size_t image,
                                       std::uint64_t generation) {
        return Target().AcquireStaging(bytes, image, generation);
    }
    void BeginDecodeInput(SlotId id) { Target().BeginDecodeInput(id); }
    void BeginDecodeOutput(SlotId id) { Target().BeginDecodeOutput(id); }
    void CompleteDecodeOutput(SlotId id) { Target().CompleteDecodeOutput(id); }
    void RestoreDecodeInput(SlotId id) { Target().RestoreDecodeInput(id); }
    void RestoreDecodeOutput(SlotId id) { Target().RestoreDecodeOutput(id); }
    void ReleaseCompressed(SlotId id) { Target().ReleaseCompressed(id); }
    void ReleaseStaging(SlotId id) { Target().ReleaseStaging(id); }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        return Target().Compressed(id);
    }
    [[nodiscard]] const StagingSlot& Staging(SlotId id) const {
        return Target().Staging(id);
    }
    [[nodiscard]] DecodeStaging& StagingResource(SlotId id) {
        return Target().StagingResource(id);
    }
    [[nodiscard]] DecodeSlotView WorkerView() {
        return DecodeSlotView(Target());
    }

private:
    friend class PipelineResources;
    explicit DecodeResourceAccess(ResourceSlots& slots)
        : LinearCapability(slots) {}
};

class GraphicsResourceAccess final : private LinearCapability<ResourceSlots> {
public:
    GraphicsResourceAccess(const GraphicsResourceAccess&) = delete;
    GraphicsResourceAccess& operator=(const GraphicsResourceAccess&) = delete;
    GraphicsResourceAccess(GraphicsResourceAccess&&) noexcept = default;
    GraphicsResourceAccess& operator=(GraphicsResourceAccess&&) = delete;
    using LinearCapability::IsValid;

    void BeginGpuCopy(SlotId id) { Target().BeginGpuCopy(id); }
    void BeginGpuRead(SlotId id) { Target().BeginGpuRead(id); }
    void BeginGpuUpload(SlotId id) { Target().BeginGpuUpload(id); }
    void ClearGpuTextureReservation(SlotId id) {
        Target().ClearGpuTextureReservation(id);
    }
    void CompleteGpuRead(SlotId id) { Target().CompleteGpuRead(id); }
    void CompleteGpuUpload(SlotId id, std::size_t frame,
                           std::uint64_t generation, std::size_t bytes,
                           bool keep) {
        Target().CompleteGpuUpload(id, frame, generation, bytes, keep);
    }
    void ReleaseStaging(SlotId id) { Target().ReleaseStaging(id); }
    [[nodiscard]] std::size_t ReleaseReplaceableGpuContent(SlotId id) {
        return Target().ReleaseReplaceableGpuContent(id);
    }
    [[nodiscard]] const GpuTextureSlot& GpuTexture(SlotId id) const {
        return Target().GpuTexture(id);
    }
    [[nodiscard]] const StagingSlot& Staging(SlotId id) const {
        return Target().Staging(id);
    }
    [[nodiscard]] GpuImage& GpuResource(SlotId id) {
        return Target().GpuResource(id);
    }
    [[nodiscard]] DecodeStaging& StagingResource(SlotId id) {
        return Target().StagingResource(id);
    }
    [[nodiscard]] std::size_t GpuTextureCount() const noexcept {
        return Target().GpuTextureCount();
    }
    [[nodiscard]] std::size_t StagingCount() const noexcept {
        return Target().StagingCount();
    }

private:
    friend class PipelineResources;
    explicit GraphicsResourceAccess(ResourceSlots& slots)
        : LinearCapability(slots) {}
};

// Grants the scheduler read-only state inspection plus the three resource
// transitions needed while reconciling reservations.
class SchedulerResourceAccess final
    : private LinearCapability<ResourceSlots> {
public:
    SchedulerResourceAccess(const SchedulerResourceAccess&) = delete;
    SchedulerResourceAccess& operator=(const SchedulerResourceAccess&) = delete;
    SchedulerResourceAccess(SchedulerResourceAccess&&) noexcept = default;
    SchedulerResourceAccess& operator=(SchedulerResourceAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] const ResourceSlots& View() const { return Target(); }
    [[nodiscard]] bool ActivateGpuTexture(const SlotId id) {
        return Target().ActivateGpuTexture(id);
    }
    void ReserveGpuTexture(const SlotId id, const std::size_t frame,
                           const std::uint64_t generation) {
        Target().ReserveGpuTexture(id, frame, generation);
    }
    void ReleaseStaging(const SlotId id) { Target().ReleaseStaging(id); }

private:
    friend class PipelineResources;
    explicit SchedulerResourceAccess(ResourceSlots& slots)
        : LinearCapability(slots) {}
};

// Grants only the process-exit escape hatch needed when cancelled kernel I/O
// does not drain within the bounded shutdown interval.
class ResourceBackingAbandonment final
    : private LinearCapability<std::unique_ptr<ResourceSlots>> {
public:
    ResourceBackingAbandonment(const ResourceBackingAbandonment&) = delete;
    ResourceBackingAbandonment& operator=(
        const ResourceBackingAbandonment&) = delete;
    ResourceBackingAbandonment(ResourceBackingAbandonment&&) noexcept =
        default;
    ResourceBackingAbandonment& operator=(ResourceBackingAbandonment&&) =
        delete;
    using LinearCapability::IsValid;

    void AbandonForProcessExit() noexcept {
        if (IsValid()) (void)Target().release();
    }

private:
    friend class PipelineResources;
    explicit ResourceBackingAbandonment(
        std::unique_ptr<ResourceSlots>& slots) noexcept
        : LinearCapability(slots) {}
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
    [[nodiscard]] StorageResourceAccess StorageAccess() noexcept {
        return StorageResourceAccess(*slots_);
    }
    [[nodiscard]] DecodeResourceAccess DecodeAccess() noexcept {
        return DecodeResourceAccess(*slots_);
    }
    [[nodiscard]] GraphicsResourceAccess GraphicsAccess() noexcept {
        return GraphicsResourceAccess(*slots_);
    }
    [[nodiscard]] SchedulerResourceAccess SchedulerAccess() noexcept {
        return SchedulerResourceAccess(*slots_);
    }
    [[nodiscard]] ResourceBackingAbandonment BackingAbandonment() noexcept {
        return ResourceBackingAbandonment(slots_);
    }
    [[nodiscard]] ResourceSlots& Slots() noexcept { return *slots_; }
    [[nodiscard]] const ResourceSlots& Slots() const noexcept { return *slots_; }
    std::unique_ptr<ResourceSlots> slots_;
};

}  // namespace pv
