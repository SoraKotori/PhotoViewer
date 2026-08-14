#include "pipeline_resource_size.h"

#include <limits>
#include <stdexcept>

namespace pv {

std::optional<std::size_t> CompressedReservationBytes(
    const CatalogItem& item, const std::size_t unknown_bytes,
    const std::size_t allocation_alignment) noexcept {
    if (!item.file_size_known || allocation_alignment == 0) {
        return unknown_bytes;
    }
    if (item.file_bytes == 0 ||
        item.file_bytes > std::numeric_limits<std::size_t>::max() -
                              (allocation_alignment - 1)) {
        return std::nullopt;
    }
    const std::size_t bytes = static_cast<std::size_t>(item.file_bytes);
    return ((bytes + allocation_alignment - 1) / allocation_alignment) *
           allocation_alignment;
}

std::optional<std::size_t> StagingReservationBytes(
    const CatalogItem& item, const std::size_t unknown_bytes) noexcept {
    if (!item.header_valid) return unknown_bytes;
    return item.resource_plan.staging_committed_bytes;
}

std::optional<std::size_t> GpuReservationBytes(
    const CatalogItem& item, const std::size_t unknown_bytes) noexcept {
    if (!item.header_valid) return unknown_bytes;
    return item.resource_plan.gpu_reservation_bytes;
}

bool AddWithinBudget(const std::size_t bytes, const std::size_t budget,
                     std::size_t& used) noexcept {
    if (bytes == 0 || bytes > budget || used > budget - bytes) return false;
    used += bytes;
    return true;
}

FixedSlotByteBudget::FixedSlotByteBudget(const std::size_t slot_count,
                                         const std::size_t byte_limit)
    : byte_limit_(byte_limit), slot_bytes_(slot_count, 0) {}

bool FixedSlotByteBudget::CanReplace(const std::size_t slot,
                                     const std::size_t bytes) const noexcept {
    if (slot >= slot_bytes_.size() || bytes == 0 || bytes > byte_limit_) {
        return false;
    }
    const std::size_t retained = committed_ - slot_bytes_[slot];
    return retained <= byte_limit_ - bytes;
}

void FixedSlotByteBudget::CommitReplacement(const std::size_t slot,
                                            const std::size_t bytes) {
    if (!CanReplace(slot, bytes)) {
        throw std::logic_error("slot byte replacement exceeds budget");
    }
    committed_ = committed_ - slot_bytes_[slot] + bytes;
    slot_bytes_[slot] = bytes;
}

std::size_t FixedSlotByteBudget::Release(const std::size_t slot) {
    if (slot >= slot_bytes_.size()) {
        throw std::out_of_range("slot byte budget");
    }
    const std::size_t released = slot_bytes_[slot];
    committed_ -= released;
    slot_bytes_[slot] = 0;
    return released;
}

}  // namespace pv
