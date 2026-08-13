#pragma once

#include "image_resources.h"
#include "io_request.h"
#include "pipeline_types.h"

#include <optional>
#include <span>
#include <vector>

namespace pv {

enum class CompressedSlotState : std::uint8_t {
    Free,
    FileReadDestination,
    CompressedDataAvailable,
    DecodeInput,
    CancellationPending,
};

enum class StagingSlotState : std::uint8_t {
    Free,
    Prepared,
    DecodeOutputActive,
    DecodedPixelsAvailable,
    GpuCopySource,
};

enum class GpuTextureSlotState : std::uint8_t {
    Inactive,
    Writable,
    Writing,
    Readable,
    Reading,
};

struct CompressedSlot {
    [[nodiscard]] CompressedSlotState State() const noexcept { return state_; }
    [[nodiscard]] const CompressedBuffer& Buffer() const noexcept { return resource_; }
    [[nodiscard]] const IoRequest& Request() const noexcept { return io_; }
    [[nodiscard]] std::size_t Image() const noexcept { return image_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }

private:
    friend class DecodeSlotAccess;
    friend class ResourceSlots;
    CompressedBuffer resource_;
    IoRequest io_;
    std::size_t image_ = 0;
    std::uint64_t generation_ = 0;
    CompressedSlotState state_ = CompressedSlotState::Free;
};

struct StagingSlot {
    [[nodiscard]] StagingSlotState State() const noexcept { return state_; }
    [[nodiscard]] const DecodeStaging& Resource() const noexcept { return resource_; }
    [[nodiscard]] std::size_t Image() const noexcept { return image_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }

private:
    friend class DecodeSlotAccess;
    friend class ResourceSlots;
    DecodeStaging resource_;
    std::size_t image_ = 0;
    std::uint64_t generation_ = 0;
    StagingSlotState state_ = StagingSlotState::Free;
};

struct GpuTextureSlot {
    [[nodiscard]] GpuTextureSlotState State() const noexcept { return state_; }
    [[nodiscard]] const GpuImage& Resource() const noexcept { return resource_; }
    [[nodiscard]] std::size_t ReservedFrame() const noexcept { return reserved_frame_; }
    [[nodiscard]] std::size_t ContentFrame() const noexcept { return content_frame_; }
    [[nodiscard]] std::uint64_t ReservationGeneration() const noexcept {
        return reservation_generation_;
    }
    [[nodiscard]] std::uint64_t ContentGeneration() const noexcept {
        return content_generation_;
    }

private:
    friend class ResourceSlots;
    GpuImage resource_;
    std::size_t reserved_frame_ = kInvalidFrame;
    std::size_t content_frame_ = kInvalidFrame;
    std::uint64_t reservation_generation_ = 0;
    std::uint64_t content_generation_ = 0;
    GpuTextureSlotState state_ = GpuTextureSlotState::Inactive;
};

class ResourceSlots {
public:
    ResourceSlots(std::size_t compressed_count, std::size_t staging_count,
                  std::size_t gpu_texture_count, std::size_t compressed_budget,
                  std::size_t staging_budget);

    ResourceSlots(const ResourceSlots&) = delete;
    ResourceSlots& operator=(const ResourceSlots&) = delete;

    [[nodiscard]] SlotId AcquireCompressed(std::size_t bytes,
                                           std::size_t image,
                                           std::uint64_t generation);
    [[nodiscard]] SlotId AcquireStaging(std::size_t bytes, std::size_t image,
                                        std::uint64_t generation);
    [[nodiscard]] bool ActivateGpuTexture(SlotId id);
    void ReleaseCompressed(SlotId id);
    void ReleaseStaging(SlotId id);
    void CancelFileRead(SlotId id);
    void CompleteFileRead(SlotId id);
    void BeginDecodeInput(SlotId id);
    void RestoreDecodeInput(SlotId id);
    void BeginDecodeOutput(SlotId id);
    void RestoreDecodeOutput(SlotId id);
    void CompleteDecodeOutput(SlotId id);
    void BeginGpuCopy(SlotId id);
    void RemapCompressedImage(SlotId id, std::size_t image);
    void RemapStagingImage(SlotId id, std::size_t image);
    void ReserveGpuTexture(SlotId id, std::size_t frame,
                           std::uint64_t generation);
    void ClearGpuTextureReservation(SlotId id);
    [[nodiscard]] std::size_t ReleaseReplaceableGpuContent(SlotId id);
    void BeginGpuUpload(SlotId id);
    void CompleteGpuUpload(SlotId id, std::size_t frame,
                           std::uint64_t generation, std::size_t bytes,
                           bool keep_readable);
    void BeginGpuRead(SlotId id);
    void CompleteGpuRead(SlotId id);

    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const;
    [[nodiscard]] const StagingSlot& Staging(SlotId id) const;
    [[nodiscard]] const GpuTextureSlot& GpuTexture(SlotId id) const;
    [[nodiscard]] std::size_t CompressedCommittedBytes() const noexcept;
    [[nodiscard]] std::size_t StagingCommittedBytes() const noexcept;
    [[nodiscard]] std::size_t FreeCompressedCount() const noexcept;
    [[nodiscard]] std::size_t FreeStagingCount() const noexcept;
    [[nodiscard]] std::size_t InactiveGpuTextureCount() const noexcept;
    [[nodiscard]] std::size_t StagingCount() const noexcept;
    [[nodiscard]] std::size_t CompressedCount() const noexcept;
    [[nodiscard]] std::size_t GpuTextureCount() const noexcept;

private:
    friend class DecodeSlotAccess;
    friend class StorageResourceAccess;
    friend class DecodeResourceAccess;
    friend class GraphicsResourceAccess;
    [[nodiscard]] IoRequest& FileReadRequest(SlotId id);
    [[nodiscard]] CompressedBuffer& FileReadBuffer(SlotId id);
    [[nodiscard]] DecodeStaging& StagingResource(SlotId id);
    [[nodiscard]] GpuImage& GpuResource(SlotId id);
    [[nodiscard]] CompressedSlot& MutableCompressed(SlotId id);
    [[nodiscard]] StagingSlot& MutableStaging(SlotId id);
    [[nodiscard]] GpuTextureSlot& MutableGpuTexture(SlotId id);
    static void RequireState(CompressedSlotState actual,
                             CompressedSlotState expected,
                             const char* operation);
    static void RequireState(StagingSlotState actual,
                             StagingSlotState expected,
                             const char* operation);
    static void RequireState(GpuTextureSlotState actual,
                             GpuTextureSlotState expected,
                             const char* operation);
    [[nodiscard]] static std::optional<std::size_t> CompressedAllocationSize(
        std::size_t bytes) noexcept;
    static void InitializeFree(std::vector<SlotId>& free_index,
                               std::size_t count);
    static void RemoveFree(std::vector<SlotId>& free_index, SlotId id);
    [[nodiscard]] SlotId BestCompressed(std::size_t bytes) const;
    [[nodiscard]] SlotId BestStaging(std::size_t bytes) const;
    void TrimCompressed(std::size_t bytes, SlotId protected_id);
    void TrimStaging(std::size_t bytes, SlotId protected_id);

    // Constructed at their final sizes and never resized, so embedded
    // OVERLAPPED/completion-key addresses remain stable for the session.
    std::vector<CompressedSlot> compressed_;
    std::vector<StagingSlot> staging_;
    std::vector<GpuTextureSlot> gpu_textures_;
    std::vector<SlotId> free_compressed_;
    std::vector<SlotId> free_staging_;
    std::vector<SlotId> free_gpu_textures_;
    std::size_t compressed_budget_ = 0;
    std::size_t staging_budget_ = 0;
    std::size_t compressed_committed_bytes_ = 0;
    std::size_t staging_committed_bytes_ = 0;
};

// The CPU executor receives only the two resources required by DecodeWork.
// It cannot mutate slot identity/state or access file-I/O and GPU resources.
class DecodeSlotAccess {
public:
    explicit DecodeSlotAccess(ResourceSlots& slots) noexcept : slots_(slots) {}

    [[nodiscard]] std::span<std::byte> CompressedInput(SlotId id) const;
    [[nodiscard]] DecodeSurface& DecodeOutput(SlotId id) const;
    [[nodiscard]] const PngResourcePlan& ExpectedPng(SlotId id) const;

private:
    ResourceSlots& slots_;
};

}  // namespace pv
