#pragma once

#include <cstddef>

namespace pv {

inline constexpr std::size_t kMaximumDefaultWorkerCount = 16;

constexpr std::size_t DefaultWorkerCountForPhysicalCores(
    const std::size_t physical_core_count) noexcept {
    if (physical_core_count == 0) return 1;
    return physical_core_count < kMaximumDefaultWorkerCount
               ? physical_core_count
               : kMaximumDefaultWorkerCount;
}

std::size_t DetectPhysicalCoreCount() noexcept;
std::size_t DefaultWorkerCount() noexcept;

}  // namespace pv
