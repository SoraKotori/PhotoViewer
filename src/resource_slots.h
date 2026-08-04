#pragma once

#include "model.h"

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
    DecodeOutputMapped,
    DecodedPixelsAvailable,
    GpuCopySource,
    CancellationPending,
};

enum class GpuTextureSlotState : std::uint8_t {
    Free,
    UploadDestination,
    Presentable,
    Retiring,
};

struct CompressedSlot {
    CompressedBuffer resource;
    CompressedSlotState state = CompressedSlotState::Free;
    std::size_t image = 0;
    std::uint64_t generation = 0;
};

struct StagingSlot {
    DecodeStaging resource;
    StagingSlotState state = StagingSlotState::Free;
    std::size_t image = 0;
    std::uint64_t generation = 0;
};

struct GpuTextureSlot {
    GpuImage resource;
    GpuTextureSlotState state = GpuTextureSlotState::Free;
    std::size_t image = 0;
    std::uint64_t generation = 0;
};

class ResourceSlots {
public:
    ResourceSlots(std::size_t compressed_count, std::size_t staging_count,
                  std::size_t gpu_texture_count, std::size_t compressed_budget,
                  std::size_t staging_budget)
        : compressed_budget_(compressed_budget), staging_budget_(staging_budget) {
        Initialize(compressed_, free_compressed_, compressed_count);
        Initialize(staging_, free_staging_, staging_count);
        Initialize(gpu_textures_, free_gpu_textures_, gpu_texture_count);
    }

    ResourceSlots(const ResourceSlots&) = delete;
    ResourceSlots& operator=(const ResourceSlots&) = delete;

    [[nodiscard]] SlotId AcquireCompressed(std::size_t bytes, std::size_t image,
                                           std::uint64_t generation) {
        const auto required = CompressedAllocationSize(bytes);
        if (!required || *required > compressed_budget_ || free_compressed_.empty()) {
            return kInvalidSlot;
        }
        const SlotId id = BestCompressed(bytes);
        CompressedSlot& slot = Compressed(id);
        const std::size_t before = slot.resource.allocation_size;
        if (before < *required) {
            TrimCompressed(*required, id);
            if (compressed_committed_bytes_ - before >
                compressed_budget_ - *required) {
                return kInvalidSlot;
            }
        }
        if (!slot.resource.Allocate(bytes)) {
            compressed_committed_bytes_ -= before;
            return kInvalidSlot;
        }
        compressed_committed_bytes_ = compressed_committed_bytes_ - before +
                                      slot.resource.allocation_size;
        RemoveFree(free_compressed_, id);
        slot.state = CompressedSlotState::FileReadDestination;
        slot.image = image;
        slot.generation = generation;
        return id;
    }

    [[nodiscard]] SlotId AcquireStaging(std::size_t bytes, std::size_t image,
                                        std::uint64_t generation) {
        if (bytes == 0 || bytes > staging_budget_ || free_staging_.empty()) {
            return kInvalidSlot;
        }
        const SlotId id = BestStaging(bytes);
        StagingSlot& slot = StagingAt(id);
        const std::size_t before = slot.resource.committed_bytes;
        if (before < bytes) {
            TrimStaging(bytes, id);
            if (staging_committed_bytes_ - before > staging_budget_ - bytes) {
                return kInvalidSlot;
            }
            slot.resource.ReleaseAllocation();
            slot.resource.committed_bytes = bytes;
            staging_committed_bytes_ = staging_committed_bytes_ - before + bytes;
        }
        RemoveFree(free_staging_, id);
        slot.state = StagingSlotState::DecodeOutputMapped;
        slot.image = image;
        slot.generation = generation;
        return id;
    }

    [[nodiscard]] SlotId AcquireGpuTexture(std::size_t image,
                                           std::uint64_t generation) {
        if (free_gpu_textures_.empty()) return kInvalidSlot;
        const SlotId id = free_gpu_textures_.back();
        free_gpu_textures_.pop_back();
        GpuTextureSlot& slot = GpuTextureAt(id);
        slot.state = GpuTextureSlotState::UploadDestination;
        slot.image = image;
        slot.generation = generation;
        return id;
    }

    void ReleaseCompressed(SlotId id) {
        if (id == kInvalidSlot) return;
        CompressedSlot& slot = Compressed(id);
        if (slot.state == CompressedSlotState::Free) {
            throw std::logic_error("compressed slot released twice");
        }
        slot.resource.size = 0;
        slot.state = CompressedSlotState::Free;
        slot.image = 0;
        slot.generation = 0;
        free_compressed_.push_back(id);
    }

    void ReleaseStaging(SlotId id) {
        if (id == kInvalidSlot) return;
        StagingSlot& slot = StagingAt(id);
        if (slot.state == StagingSlotState::Free) {
            throw std::logic_error("staging slot released twice");
        }
        if (slot.resource.mapped) {
            throw std::logic_error("mapped staging slot released");
        }
        slot.resource.ResetView();
        slot.state = StagingSlotState::Free;
        slot.image = 0;
        slot.generation = 0;
        free_staging_.push_back(id);
    }

    void ReleaseGpuTexture(SlotId id) {
        if (id == kInvalidSlot) return;
        GpuTextureSlot& slot = GpuTextureAt(id);
        if (slot.state == GpuTextureSlotState::Free) {
            throw std::logic_error("GPU texture slot released twice");
        }
        slot.state = GpuTextureSlotState::Retiring;
        slot.resource = {};
        slot.state = GpuTextureSlotState::Free;
        slot.image = 0;
        slot.generation = 0;
        free_gpu_textures_.push_back(id);
    }

    [[nodiscard]] CompressedSlot& Compressed(SlotId id) {
        return *compressed_.at(id);
    }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        return *compressed_.at(id);
    }
    [[nodiscard]] StagingSlot& StagingAt(SlotId id) {
        return *staging_.at(id);
    }
    [[nodiscard]] const StagingSlot& StagingAt(SlotId id) const {
        return *staging_.at(id);
    }
    [[nodiscard]] GpuTextureSlot& GpuTextureAt(SlotId id) {
        return *gpu_textures_.at(id);
    }
    [[nodiscard]] const GpuTextureSlot& GpuTextureAt(SlotId id) const {
        return *gpu_textures_.at(id);
    }

    [[nodiscard]] std::size_t CompressedCommittedBytes() const noexcept {
        return compressed_committed_bytes_;
    }
    [[nodiscard]] std::size_t StagingCommittedBytes() const noexcept {
        return staging_committed_bytes_;
    }
    [[nodiscard]] std::size_t FreeCompressedCount() const noexcept {
        return free_compressed_.size();
    }
    [[nodiscard]] std::size_t FreeStagingCount() const noexcept {
        return free_staging_.size();
    }
    [[nodiscard]] std::size_t FreeGpuTextureCount() const noexcept {
        return free_gpu_textures_.size();
    }
    [[nodiscard]] std::size_t StagingCount() const noexcept {
        return staging_.size();
    }
    [[nodiscard]] std::size_t CompressedCount() const noexcept {
        return compressed_.size();
    }
    [[nodiscard]] std::size_t GpuTextureCount() const noexcept {
        return gpu_textures_.size();
    }

private:
    [[nodiscard]] static std::optional<std::size_t> CompressedAllocationSize(
        const std::size_t bytes) noexcept {
        constexpr std::size_t alignment = 4096;
        if (bytes == 0 ||
            bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            return std::nullopt;
        }
        return (bytes + (alignment - 1)) & ~(alignment - 1);
    }

    template <typename Slot>
    static void Initialize(std::vector<std::unique_ptr<Slot>>& slots,
                           std::vector<SlotId>& free_index,
                           const std::size_t count) {
        slots.reserve(count);
        free_index.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            slots.push_back(std::make_unique<Slot>());
            free_index.push_back(static_cast<SlotId>(count - 1 - index));
        }
    }

    static void RemoveFree(std::vector<SlotId>& free_index, SlotId id) {
        const auto found = std::find(free_index.begin(), free_index.end(), id);
        if (found == free_index.end()) throw std::logic_error("slot missing from free index");
        free_index.erase(found);
    }

    [[nodiscard]] SlotId BestCompressed(const std::size_t bytes) const {
        SlotId best = free_compressed_.front();
        bool adequate = false;
        for (const SlotId id : free_compressed_) {
            const std::size_t capacity = Compressed(id).resource.allocation_size;
            if (capacity >= bytes &&
                (!adequate || capacity < Compressed(best).resource.allocation_size)) {
                best = id;
                adequate = true;
            } else if (!adequate && capacity < Compressed(best).resource.allocation_size) {
                best = id;
            }
        }
        return best;
    }

    [[nodiscard]] SlotId BestStaging(const std::size_t bytes) const {
        SlotId best = free_staging_.front();
        bool adequate = false;
        for (const SlotId id : free_staging_) {
            const std::size_t capacity = StagingAt(id).resource.committed_bytes;
            if (capacity >= bytes &&
                (!adequate || capacity < StagingAt(best).resource.committed_bytes)) {
                best = id;
                adequate = true;
            } else if (!adequate && capacity < StagingAt(best).resource.committed_bytes) {
                best = id;
            }
        }
        return best;
    }

    void TrimCompressed(std::size_t bytes, SlotId protected_id) {
        while (compressed_committed_bytes_ - Compressed(protected_id).resource.allocation_size >
               compressed_budget_ - bytes) {
            SlotId victim = kInvalidSlot;
            for (const SlotId id : free_compressed_) {
                if (id == protected_id || Compressed(id).resource.allocation_size == 0) continue;
                if (victim == kInvalidSlot ||
                    Compressed(id).resource.allocation_size >
                        Compressed(victim).resource.allocation_size) {
                    victim = id;
                }
            }
            if (victim == kInvalidSlot) break;
            compressed_committed_bytes_ -= Compressed(victim).resource.allocation_size;
            Compressed(victim).resource.ReleaseAllocation();
        }
    }

    void TrimStaging(std::size_t bytes, SlotId protected_id) {
        while (staging_committed_bytes_ - StagingAt(protected_id).resource.committed_bytes >
               staging_budget_ - bytes) {
            SlotId victim = kInvalidSlot;
            for (const SlotId id : free_staging_) {
                if (id == protected_id || StagingAt(id).resource.committed_bytes == 0) continue;
                if (victim == kInvalidSlot ||
                    StagingAt(id).resource.committed_bytes >
                        StagingAt(victim).resource.committed_bytes) {
                    victim = id;
                }
            }
            if (victim == kInvalidSlot) break;
            staging_committed_bytes_ -= StagingAt(victim).resource.committed_bytes;
            StagingAt(victim).resource.ReleaseAllocation();
        }
    }

    std::vector<std::unique_ptr<CompressedSlot>> compressed_;
    std::vector<std::unique_ptr<StagingSlot>> staging_;
    std::vector<std::unique_ptr<GpuTextureSlot>> gpu_textures_;
    std::vector<SlotId> free_compressed_;
    std::vector<SlotId> free_staging_;
    std::vector<SlotId> free_gpu_textures_;
    std::size_t compressed_budget_ = 0;
    std::size_t staging_budget_ = 0;
    std::size_t compressed_committed_bytes_ = 0;
    std::size_t staging_committed_bytes_ = 0;
};

}  // namespace pv
