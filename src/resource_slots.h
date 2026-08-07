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
    Prepared,
    DecodeOutputMapped,
    DecodedPixelsAvailable,
    GpuCopySource,
    CancellationPending,
};

enum class GpuTextureSlotState : std::uint8_t {
    Inactive,
    Writable,
    Writing,
    Readable,
    Reading,
};

struct IoRequest {
    void Reset() noexcept {
        header_overlapped = {};
        content_overlapped = {};
        file = INVALID_HANDLE_VALUE;
        threadpool_io = nullptr;
        window = nullptr;
        index = 0;
        generation = 0;
        compressed_slot = kInvalidSlot;
        destination = nullptr;
        byte_count = 0;
        prefix_bytes = 0;
        split_header = false;
        header_completed = false;
        content_submitted = false;
        content_completed = false;
        header_result = ERROR_IO_PENDING;
        header_transferred = 0;
        result = ERROR_IO_PENDING;
        transferred = 0;
    }

    OVERLAPPED header_overlapped{};
    OVERLAPPED content_overlapped{};
    HANDLE file = INVALID_HANDLE_VALUE;
    PTP_IO threadpool_io = nullptr;
    HWND window = nullptr;
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    std::byte* destination = nullptr;
    DWORD byte_count = 0;
    DWORD prefix_bytes = 0;
    bool split_header = false;
    bool header_completed = false;
    bool content_submitted = false;
    bool content_completed = false;
    DWORD header_result = ERROR_IO_PENDING;
    ULONG_PTR header_transferred = 0;
    DWORD result = ERROR_IO_PENDING;
    ULONG_PTR transferred = 0;
};

struct CompressedSlot {
    CompressedBuffer resource;
    IoRequest io;
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
    GpuTextureSlotState state = GpuTextureSlotState::Inactive;
    std::size_t reserved_frame = kInvalidFrame;
    std::size_t content_frame = kInvalidFrame;
    std::uint64_t generation = 0;
};

class ResourceSlots {
public:
    ResourceSlots(std::size_t compressed_count, std::size_t staging_count,
                  std::size_t gpu_texture_count, std::size_t compressed_budget,
                  std::size_t staging_budget)
        : compressed_(std::make_unique<CompressedSlot[]>(compressed_count)),
          staging_(std::make_unique<StagingSlot[]>(staging_count)),
          work_tokens_(std::make_unique<WorkToken[]>(staging_count)),
          gpu_textures_(std::make_unique<GpuTextureSlot[]>(gpu_texture_count)),
          compressed_count_(compressed_count),
          staging_count_(staging_count),
          gpu_texture_count_(gpu_texture_count),
          compressed_budget_(compressed_budget), staging_budget_(staging_budget) {
        InitializeFree(free_compressed_, compressed_count);
        InitializeFree(free_staging_, staging_count);
        InitializeFree(free_gpu_textures_, gpu_texture_count);
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
        slot.state = StagingSlotState::Prepared;
        slot.image = image;
        slot.generation = generation;
        return id;
    }

    [[nodiscard]] bool ActivateGpuTexture(const SlotId id) {
        if (id == kInvalidSlot || id >= gpu_texture_count_) return false;
        GpuTextureSlot& slot = GpuTextureAt(id);
        if (slot.state != GpuTextureSlotState::Inactive) return true;
        RemoveFree(free_gpu_textures_, id);
        slot.state = GpuTextureSlotState::Writable;
        slot.reserved_frame = kInvalidFrame;
        slot.content_frame = kInvalidFrame;
        slot.generation = 0;
        return true;
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

    [[nodiscard]] CompressedSlot& Compressed(SlotId id) {
        if (id >= compressed_count_) throw std::out_of_range("compressed slot");
        return compressed_[id];
    }
    [[nodiscard]] const CompressedSlot& Compressed(SlotId id) const {
        if (id >= compressed_count_) throw std::out_of_range("compressed slot");
        return compressed_[id];
    }
    [[nodiscard]] StagingSlot& StagingAt(SlotId id) {
        if (id >= staging_count_) throw std::out_of_range("staging slot");
        return staging_[id];
    }
    [[nodiscard]] const StagingSlot& StagingAt(SlotId id) const {
        if (id >= staging_count_) throw std::out_of_range("staging slot");
        return staging_[id];
    }
    [[nodiscard]] WorkToken& WorkTokenAt(SlotId id) {
        if (id >= staging_count_) throw std::out_of_range("work token");
        return work_tokens_[id];
    }
    [[nodiscard]] GpuTextureSlot& GpuTextureAt(SlotId id) {
        if (id >= gpu_texture_count_) throw std::out_of_range("GPU texture slot");
        return gpu_textures_[id];
    }
    [[nodiscard]] const GpuTextureSlot& GpuTextureAt(SlotId id) const {
        if (id >= gpu_texture_count_) throw std::out_of_range("GPU texture slot");
        return gpu_textures_[id];
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
        return staging_count_;
    }
    [[nodiscard]] std::size_t CompressedCount() const noexcept {
        return compressed_count_;
    }
    [[nodiscard]] std::size_t GpuTextureCount() const noexcept {
        return gpu_texture_count_;
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

    static void InitializeFree(std::vector<SlotId>& free_index,
                               const std::size_t count) {
        free_index.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
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

    std::unique_ptr<CompressedSlot[]> compressed_;
    std::unique_ptr<StagingSlot[]> staging_;
    std::unique_ptr<WorkToken[]> work_tokens_;
    std::unique_ptr<GpuTextureSlot[]> gpu_textures_;
    std::size_t compressed_count_ = 0;
    std::size_t staging_count_ = 0;
    std::size_t gpu_texture_count_ = 0;
    std::vector<SlotId> free_compressed_;
    std::vector<SlotId> free_staging_;
    std::vector<SlotId> free_gpu_textures_;
    std::size_t compressed_budget_ = 0;
    std::size_t staging_budget_ = 0;
    std::size_t compressed_committed_bytes_ = 0;
    std::size_t staging_committed_bytes_ = 0;
};

}  // namespace pv
