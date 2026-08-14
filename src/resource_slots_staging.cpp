#include "resource_slots.h"

#include <stdexcept>

namespace pv {

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

void ResourceSlots::RemapStagingImage(const SlotId id,
                                      const std::size_t image) {
    StagingSlot& slot = MutableStaging(id);
    if (slot.state_ == StagingSlotState::Free) {
        throw std::logic_error("cannot remap a free staging slot");
    }
    slot.image_ = image;
}

StagingSlot& ResourceSlots::MutableStaging(const SlotId id) {
    if (id >= staging_.size()) throw std::out_of_range("staging slot");
    return staging_[id];
}

const StagingSlot& ResourceSlots::Staging(const SlotId id) const {
    if (id >= staging_.size()) throw std::out_of_range("staging slot");
    return staging_[id];
}

DecodeStaging& ResourceSlots::StagingResource(const SlotId id) {
    StagingSlot& slot = MutableStaging(id);
    if (slot.state_ == StagingSlotState::Free) {
        throw std::logic_error("access free staging resource");
    }
    return slot.resource_;
}

std::size_t ResourceSlots::StagingCommittedBytes() const noexcept {
    return staging_committed_bytes_;
}

std::size_t ResourceSlots::FreeStagingCount() const noexcept {
    return free_staging_.size();
}

std::size_t ResourceSlots::StagingCount() const noexcept {
    return staging_.size();
}

void ResourceSlots::RequireState(const StagingSlotState actual,
                                 const StagingSlotState expected,
                                 const char* const operation) {
    if (actual != expected) throw std::logic_error(operation);
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

DecodeSurface& DecodeSlotView::DecodeOutput(const SlotId id) const {
    StagingSlot& slot = slots_.MutableStaging(id);
    ResourceSlots::RequireState(slot.state_,
                                StagingSlotState::DecodeOutputActive,
                                "access decode output");
    return slot.resource_.surface;
}

const PngResourcePlan& DecodeSlotView::ExpectedPng(const SlotId id) const {
    StagingSlot& slot = slots_.MutableStaging(id);
    ResourceSlots::RequireState(slot.state_,
                                StagingSlotState::DecodeOutputActive,
                                "access expected PNG plan");
    return slot.resource_.resource_plan;
}

}  // namespace pv
