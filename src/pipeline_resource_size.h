#pragma once

#include "catalog.h"

#include <optional>
#include <vector>

namespace pv {

[[nodiscard]] std::optional<std::size_t> CompressedReservationBytes(
    const CatalogItem& item, std::size_t unknown_bytes,
    std::size_t allocation_alignment = 4096) noexcept;
[[nodiscard]] std::optional<std::size_t> StagingReservationBytes(
    const CatalogItem& item, std::size_t unknown_bytes) noexcept;
[[nodiscard]] std::optional<std::size_t> GpuReservationBytes(
    const CatalogItem& item, std::size_t unknown_bytes) noexcept;
[[nodiscard]] bool AddWithinBudget(std::size_t bytes, std::size_t budget,
                                   std::size_t& used) noexcept;

// Fixed-capacity byte ledger for resources whose allocation belongs to a
// stable slot. Replacement and release update the slot and total atomically.
class FixedSlotByteBudget {
public:
    FixedSlotByteBudget(std::size_t slot_count, std::size_t byte_limit);

    [[nodiscard]] bool CanReplace(std::size_t slot,
                                  std::size_t bytes) const noexcept;
    void CommitReplacement(std::size_t slot, std::size_t bytes);
    [[nodiscard]] std::size_t Release(std::size_t slot);
    [[nodiscard]] std::size_t Committed() const noexcept { return committed_; }

private:
    const std::size_t byte_limit_;
    std::vector<std::size_t> slot_bytes_;
    std::size_t committed_ = 0;
};

}  // namespace pv
