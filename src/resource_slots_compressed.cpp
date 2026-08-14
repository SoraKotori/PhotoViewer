#include "resource_slots.h"

#include <stdexcept>

namespace pv {

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

void ResourceSlots::RemapCompressedImage(const SlotId id,
                                         const std::size_t image) {
    CompressedSlot& slot = MutableCompressed(id);
    if (slot.state_ == CompressedSlotState::Free) {
        throw std::logic_error("cannot remap a free compressed slot");
    }
    slot.image_ = image;
}

CompressedSlot& ResourceSlots::MutableCompressed(const SlotId id) {
    if (id >= compressed_.size()) throw std::out_of_range("compressed slot");
    return compressed_[id];
}

const CompressedSlot& ResourceSlots::Compressed(const SlotId id) const {
    if (id >= compressed_.size()) throw std::out_of_range("compressed slot");
    return compressed_[id];
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

std::size_t ResourceSlots::CompressedCommittedBytes() const noexcept {
    return compressed_committed_bytes_;
}

std::size_t ResourceSlots::FreeCompressedCount() const noexcept {
    return free_compressed_.size();
}

std::size_t ResourceSlots::CompressedCount() const noexcept {
    return compressed_.size();
}

void ResourceSlots::RequireState(const CompressedSlotState actual,
                                 const CompressedSlotState expected,
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

std::span<std::byte> DecodeSlotView::CompressedInput(const SlotId id) const {
    CompressedSlot& slot = slots_.MutableCompressed(id);
    ResourceSlots::RequireState(slot.state_, CompressedSlotState::DecodeInput,
                                "access decode input");
    CompressedBuffer& input = slot.resource_;
    return {input.data, input.size};
}

}  // namespace pv
