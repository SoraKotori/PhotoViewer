#include "resource_slots.h"

#include <algorithm>
#include <stdexcept>

namespace pv {

ResourceSlots::ResourceSlots(const std::size_t compressed_count,
                             const std::size_t staging_count,
                             const std::size_t gpu_texture_count,
                             const std::size_t compressed_budget,
                             const std::size_t staging_budget)
    : compressed_(compressed_count),
      staging_(staging_count),
      gpu_textures_(gpu_texture_count),
      compressed_budget_(compressed_budget),
      staging_budget_(staging_budget) {
    InitializeFree(free_compressed_, compressed_count);
    InitializeFree(free_staging_, staging_count);
    InitializeFree(free_gpu_textures_, gpu_texture_count);
}

SlotId ResourceSlots::AcquireCompressed(const std::size_t bytes,
                                        const std::size_t image,
                                        const std::uint64_t generation) {
    const auto required = CompressedAllocationSize(bytes);
    if (!required || *required > compressed_budget_ ||
        free_compressed_.empty()) return kInvalidSlot;
    const SlotId id = BestCompressed(bytes);
    CompressedSlot& slot = MutableCompressed(id);
    const std::size_t before = slot.resource_.allocation_size;
    if (before < *required) {
        TrimCompressed(*required, id);
        if (compressed_committed_bytes_ - before >
            compressed_budget_ - *required) return kInvalidSlot;
    }
    if (!slot.resource_.Allocate(bytes)) return kInvalidSlot;
    compressed_committed_bytes_ = compressed_committed_bytes_ - before +
                                  slot.resource_.allocation_size;
    RemoveFree(free_compressed_, id);
    slot.state_ = CompressedSlotState::FileReadDestination;
    slot.image_ = image;
    slot.generation_ = generation;
    return id;
}

SlotId ResourceSlots::AcquireStaging(const std::size_t bytes,
                                     const std::size_t image,
                                     const std::uint64_t generation) {
    if (bytes == 0 || bytes > staging_budget_ || free_staging_.empty()) {
        return kInvalidSlot;
    }
    const SlotId id = BestStaging(bytes);
    StagingSlot& slot = MutableStaging(id);
    const std::size_t before = slot.resource_.committed_bytes;
    if (before < bytes) {
        TrimStaging(bytes, id);
        if (staging_committed_bytes_ - before > staging_budget_ - bytes) {
            return kInvalidSlot;
        }
        slot.resource_.ReleaseAllocation();
        slot.resource_.committed_bytes = bytes;
        staging_committed_bytes_ = staging_committed_bytes_ - before + bytes;
    }
    RemoveFree(free_staging_, id);
    slot.state_ = StagingSlotState::Prepared;
    slot.image_ = image;
    slot.generation_ = generation;
    return id;
}

bool ResourceSlots::ActivateGpuTexture(const SlotId id) {
    if (id == kInvalidSlot || id >= gpu_textures_.size()) return false;
    GpuTextureSlot& slot = MutableGpuTexture(id);
    if (slot.state_ != GpuTextureSlotState::Inactive) return true;
    RemoveFree(free_gpu_textures_, id);
    slot.state_ = GpuTextureSlotState::Writable;
    slot.reserved_frame_ = kInvalidFrame;
    slot.content_frame_ = kInvalidFrame;
    slot.reservation_generation_ = 0;
    slot.content_generation_ = 0;
    return true;
}

void ResourceSlots::ReleaseCompressed(const SlotId id) {
    if (id == kInvalidSlot) return;
    CompressedSlot& slot = MutableCompressed(id);
    if (slot.state_ == CompressedSlotState::Free) {
        throw std::logic_error("compressed slot released twice");
    }
    slot.resource_.size = 0;
    slot.state_ = CompressedSlotState::Free;
    slot.image_ = 0;
    slot.generation_ = 0;
    free_compressed_.push_back(id);
}

void ResourceSlots::ReleaseStaging(const SlotId id) {
    if (id == kInvalidSlot) return;
    StagingSlot& slot = MutableStaging(id);
    if (slot.state_ == StagingSlotState::Free) {
        throw std::logic_error("staging slot released twice");
    }
    if (slot.resource_.mapped) {
        throw std::logic_error("mapped staging slot released");
    }
    slot.resource_.ResetView();
    slot.state_ = StagingSlotState::Free;
    slot.image_ = 0;
    slot.generation_ = 0;
    free_staging_.push_back(id);
}

void ResourceSlots::CancelFileRead(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    if (slot.state_ != CompressedSlotState::FileReadDestination &&
        slot.state_ != CompressedSlotState::CancellationPending) {
        throw std::logic_error("file read cancellation outside active read");
    }
    slot.state_ = CompressedSlotState::CancellationPending;
}

void ResourceSlots::CompleteFileRead(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    RequireState(slot.state_, CompressedSlotState::FileReadDestination,
                 "complete file read");
    slot.state_ = CompressedSlotState::CompressedDataAvailable;
}

void ResourceSlots::BeginDecodeInput(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    RequireState(slot.state_, CompressedSlotState::CompressedDataAvailable,
                 "begin decode input");
    slot.state_ = CompressedSlotState::DecodeInput;
}

void ResourceSlots::RestoreDecodeInput(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    RequireState(slot.state_, CompressedSlotState::DecodeInput,
                 "restore decode input");
    slot.state_ = CompressedSlotState::CompressedDataAvailable;
}

void ResourceSlots::BeginDecodeOutput(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    RequireState(slot.state_, StagingSlotState::Prepared,
                 "begin decode output");
    slot.state_ = StagingSlotState::DecodeOutputActive;
}

void ResourceSlots::RestoreDecodeOutput(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    RequireState(slot.state_, StagingSlotState::DecodeOutputActive,
                 "restore decode output");
    slot.state_ = StagingSlotState::Prepared;
}

void ResourceSlots::CompleteDecodeOutput(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    RequireState(slot.state_, StagingSlotState::DecodeOutputActive,
                 "complete decode output");
    slot.state_ = StagingSlotState::DecodedPixelsAvailable;
}

void ResourceSlots::BeginGpuCopy(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    RequireState(slot.state_, StagingSlotState::DecodedPixelsAvailable,
                 "begin GPU copy");
    slot.state_ = StagingSlotState::GpuCopySource;
}

void ResourceSlots::RemapCompressedImage(const SlotId id,
                                         const std::size_t image) {
    CompressedSlot& slot = MutableCompressed(id);
    if (slot.state_ == CompressedSlotState::Free) {
        throw std::logic_error("cannot remap a free compressed slot");
    }
    slot.image_ = image;
}

void ResourceSlots::RemapStagingImage(const SlotId id,
                                      const std::size_t image) {
    StagingSlot& slot = MutableStaging(id);
    if (slot.state_ == StagingSlotState::Free) {
        throw std::logic_error("cannot remap a free staging slot");
    }
    slot.image_ = image;
}

void ResourceSlots::ReserveGpuTexture(const SlotId id, const std::size_t frame,
                                      const std::uint64_t generation) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    if (slot.state_ != GpuTextureSlotState::Writable &&
        slot.state_ != GpuTextureSlotState::Readable) {
        throw std::logic_error("cannot reserve a busy GPU texture slot");
    }
    slot.reserved_frame_ = frame;
    slot.reservation_generation_ = generation;
    slot.state_ = slot.content_frame_ == frame &&
                          slot.content_generation_ == generation &&
                          slot.resource_.bitmap
                      ? GpuTextureSlotState::Readable
                      : GpuTextureSlotState::Writable;
}

void ResourceSlots::ClearGpuTextureReservation(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    if (slot.state_ != GpuTextureSlotState::Writable &&
        slot.state_ != GpuTextureSlotState::Readable) {
        throw std::logic_error("cannot clear a busy GPU texture reservation");
    }
    slot.reserved_frame_ = kInvalidFrame;
    slot.reservation_generation_ = 0;
    slot.state_ = GpuTextureSlotState::Writable;
}

std::size_t ResourceSlots::ReleaseReplaceableGpuContent(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    const bool matches_reservation =
        slot.reserved_frame_ != kInvalidFrame &&
        slot.content_frame_ == slot.reserved_frame_ &&
        slot.content_generation_ == slot.reservation_generation_ &&
        slot.resource_.bitmap;
    if (slot.state_ != GpuTextureSlotState::Writable || matches_reservation) {
        throw std::logic_error("cannot evict active or reusable GPU content");
    }
    const std::size_t released_bytes = slot.resource_.bytes;
    slot.resource_ = {};
    slot.content_frame_ = kInvalidFrame;
    slot.content_generation_ = 0;
    return released_bytes;
}

void ResourceSlots::BeginGpuUpload(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    RequireState(slot.state_, GpuTextureSlotState::Writable,
                 "begin GPU upload");
    slot.content_frame_ = kInvalidFrame;
    slot.content_generation_ = 0;
    slot.state_ = GpuTextureSlotState::Writing;
}

void ResourceSlots::CompleteGpuUpload(const SlotId id,
                                      const std::size_t frame,
                                      const std::uint64_t generation,
                                      const std::size_t bytes,
                                      const bool keep_readable) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    RequireState(slot.state_, GpuTextureSlotState::Writing,
                 "complete GPU upload");
    slot.content_frame_ = frame;
    slot.content_generation_ = generation;
    slot.resource_.bytes = bytes;
    slot.state_ = keep_readable ? GpuTextureSlotState::Readable
                                : GpuTextureSlotState::Writable;
}

void ResourceSlots::BeginGpuRead(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    RequireState(slot.state_, GpuTextureSlotState::Readable,
                 "begin GPU read");
    slot.state_ = GpuTextureSlotState::Reading;
}

void ResourceSlots::CompleteGpuRead(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    RequireState(slot.state_, GpuTextureSlotState::Reading,
                 "complete GPU read");
    slot.state_ = slot.reserved_frame_ == slot.content_frame_ &&
                          slot.reservation_generation_ ==
                              slot.content_generation_
                      ? GpuTextureSlotState::Readable
                      : GpuTextureSlotState::Writable;
}

CompressedSlot& ResourceSlots::MutableCompressed(const SlotId id) {
    if (id >= compressed_.size()) throw std::out_of_range("compressed slot");
    return compressed_[id];
}

const CompressedSlot& ResourceSlots::Compressed(const SlotId id) const {
    if (id >= compressed_.size()) throw std::out_of_range("compressed slot");
    return compressed_[id];
}

StagingSlot& ResourceSlots::MutableStaging(const SlotId id) {
    if (id >= staging_.size()) throw std::out_of_range("staging slot");
    return staging_[id];
}

const StagingSlot& ResourceSlots::Staging(const SlotId id) const {
    if (id >= staging_.size()) throw std::out_of_range("staging slot");
    return staging_[id];
}

GpuTextureSlot& ResourceSlots::MutableGpuTexture(const SlotId id) {
    if (id >= gpu_textures_.size()) throw std::out_of_range("GPU Texture slot");
    return gpu_textures_[id];
}

const GpuTextureSlot& ResourceSlots::GpuTexture(const SlotId id) const {
    if (id >= gpu_textures_.size()) throw std::out_of_range("GPU Texture slot");
    return gpu_textures_[id];
}

IoRequest& ResourceSlots::FileReadRequest(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    RequireState(slot.state_, CompressedSlotState::FileReadDestination,
                 "access file read request");
    return slot.io_;
}

CompressedBuffer& ResourceSlots::FileReadBuffer(const SlotId id) {
    CompressedSlot& slot = MutableCompressed(id);
    RequireState(slot.state_, CompressedSlotState::FileReadDestination,
                 "access file read buffer");
    return slot.resource_;
}

DecodeStaging& ResourceSlots::StagingResource(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    if (slot.state_ == StagingSlotState::Free) {
        throw std::logic_error("access free staging resource");
    }
    return slot.resource_;
}

GpuImage& ResourceSlots::GpuResource(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    if (slot.state_ == GpuTextureSlotState::Inactive) {
        throw std::logic_error("access inactive GPU resource");
    }
    return slot.resource_;
}

std::size_t ResourceSlots::CompressedCommittedBytes() const noexcept {
    return compressed_committed_bytes_;
}
std::size_t ResourceSlots::StagingCommittedBytes() const noexcept {
    return staging_committed_bytes_;
}
std::size_t ResourceSlots::FreeCompressedCount() const noexcept {
    return free_compressed_.size();
}
std::size_t ResourceSlots::FreeStagingCount() const noexcept {
    return free_staging_.size();
}
std::size_t ResourceSlots::InactiveGpuTextureCount() const noexcept {
    return free_gpu_textures_.size();
}
std::size_t ResourceSlots::StagingCount() const noexcept { return staging_.size(); }
std::size_t ResourceSlots::CompressedCount() const noexcept { return compressed_.size(); }
std::size_t ResourceSlots::GpuTextureCount() const noexcept { return gpu_textures_.size(); }

void ResourceSlots::RequireState(const CompressedSlotState actual,
                                 const CompressedSlotState expected,
                                 const char* const operation) {
    if (actual != expected) throw std::logic_error(operation);
}

void ResourceSlots::RequireState(const StagingSlotState actual,
                                 const StagingSlotState expected,
                                 const char* const operation) {
    if (actual != expected) throw std::logic_error(operation);
}

void ResourceSlots::RequireState(const GpuTextureSlotState actual,
                                 const GpuTextureSlotState expected,
                                 const char* const operation) {
    if (actual != expected) throw std::logic_error(operation);
}

std::optional<std::size_t> ResourceSlots::CompressedAllocationSize(
    const std::size_t bytes) noexcept {
    constexpr std::size_t alignment = 4096;
    if (bytes == 0 ||
        bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (bytes + (alignment - 1)) & ~(alignment - 1);
}

void ResourceSlots::InitializeFree(std::vector<SlotId>& free_index,
                                   const std::size_t count) {
    free_index.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        free_index.push_back(static_cast<SlotId>(count - 1 - index));
    }
}

void ResourceSlots::RemoveFree(std::vector<SlotId>& free_index,
                               const SlotId id) {
    const auto found = std::find(free_index.begin(), free_index.end(), id);
    if (found == free_index.end()) {
        throw std::logic_error("slot missing from free index");
    }
    *found = free_index.back();
    free_index.pop_back();
}

SlotId ResourceSlots::BestCompressed(const std::size_t bytes) const {
    SlotId best = free_compressed_.front();
    bool adequate = false;
    for (const SlotId id : free_compressed_) {
        const std::size_t capacity = Compressed(id).resource_.allocation_size;
        if (capacity >= bytes &&
            (!adequate || capacity < Compressed(best).resource_.allocation_size)) {
            best = id;
            adequate = true;
        } else if (!adequate &&
                   capacity > Compressed(best).resource_.allocation_size) {
            best = id;
        }
    }
    return best;
}

SlotId ResourceSlots::BestStaging(const std::size_t bytes) const {
    SlotId best = free_staging_.front();
    bool adequate = false;
    for (const SlotId id : free_staging_) {
        const std::size_t capacity = Staging(id).resource_.committed_bytes;
        if (capacity >= bytes &&
            (!adequate || capacity < Staging(best).resource_.committed_bytes)) {
            best = id;
            adequate = true;
        } else if (!adequate &&
                   capacity > Staging(best).resource_.committed_bytes) {
            best = id;
        }
    }
    return best;
}

void ResourceSlots::TrimCompressed(const std::size_t bytes,
                                   const SlotId protected_id) {
    while (compressed_committed_bytes_ -
               Compressed(protected_id).resource_.allocation_size >
           compressed_budget_ - bytes) {
        SlotId victim = kInvalidSlot;
        for (const SlotId id : free_compressed_) {
            if (id == protected_id ||
                Compressed(id).resource_.allocation_size == 0) continue;
            if (victim == kInvalidSlot ||
                Compressed(id).resource_.allocation_size >
                    Compressed(victim).resource_.allocation_size) victim = id;
        }
        if (victim == kInvalidSlot) break;
        compressed_committed_bytes_ -=
            Compressed(victim).resource_.allocation_size;
        MutableCompressed(victim).resource_.ReleaseAllocation();
    }
}

void ResourceSlots::TrimStaging(const std::size_t bytes,
                                const SlotId protected_id) {
    while (staging_committed_bytes_ -
               Staging(protected_id).resource_.committed_bytes >
           staging_budget_ - bytes) {
        SlotId victim = kInvalidSlot;
        for (const SlotId id : free_staging_) {
            if (id == protected_id ||
                Staging(id).resource_.committed_bytes == 0) continue;
            if (victim == kInvalidSlot ||
                Staging(id).resource_.committed_bytes >
                    Staging(victim).resource_.committed_bytes) victim = id;
        }
        if (victim == kInvalidSlot) break;
        staging_committed_bytes_ -=
            Staging(victim).resource_.committed_bytes;
        MutableStaging(victim).resource_.ReleaseAllocation();
    }
}

std::span<std::byte> DecodeSlotAccess::CompressedInput(const SlotId id) const {
    CompressedSlot& slot = slots_.MutableCompressed(id);
    ResourceSlots::RequireState(slot.state_, CompressedSlotState::DecodeInput,
                                "access decode input");
    CompressedBuffer& input = slot.resource_;
    return {input.data, input.size};
}

DecodeSurface& DecodeSlotAccess::DecodeOutput(const SlotId id) const {
    StagingSlot& slot = slots_.MutableStaging(id);
    ResourceSlots::RequireState(slot.state_,
                                StagingSlotState::DecodeOutputActive,
                                "access decode output");
    return slot.resource_.surface;
}

}  // namespace pv
