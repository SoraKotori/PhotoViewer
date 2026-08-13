#include "resource_slots.h"

#include <stdexcept>

namespace pv {

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

GpuTextureSlot& ResourceSlots::MutableGpuTexture(const SlotId id) {
    if (id >= gpu_textures_.size()) {
        throw std::out_of_range("GPU Texture slot");
    }
    return gpu_textures_[id];
}

const GpuTextureSlot& ResourceSlots::GpuTexture(const SlotId id) const {
    if (id >= gpu_textures_.size()) {
        throw std::out_of_range("GPU Texture slot");
    }
    return gpu_textures_[id];
}

GpuImage& ResourceSlots::GpuResource(const SlotId id) {
    GpuTextureSlot& slot = MutableGpuTexture(id);
    if (slot.state_ == GpuTextureSlotState::Inactive) {
        throw std::logic_error("access inactive GPU resource");
    }
    return slot.resource_;
}

std::size_t ResourceSlots::InactiveGpuTextureCount() const noexcept {
    return free_gpu_textures_.size();
}

std::size_t ResourceSlots::GpuTextureCount() const noexcept {
    return gpu_textures_.size();
}

void ResourceSlots::RequireState(const GpuTextureSlotState actual,
                                 const GpuTextureSlotState expected,
                                 const char* const operation) {
    if (actual != expected) throw std::logic_error(operation);
}

}  // namespace pv
