#pragma once

#include <cstddef>

namespace pv {

struct Config;

struct PipelineLimits {
    explicit PipelineLimits(const Config& config);

    [[nodiscard]] std::size_t GpuSlotCount() const noexcept {
        return gpu_forward_slot_count + gpu_reverse_slot_count;
    }

    std::size_t compressed_budget_bytes;
    std::size_t staging_cache_bytes;
    std::size_t gpu_cache_bytes;
    std::size_t compressed_slot_count;
    std::size_t staging_slot_count;
    std::size_t gpu_forward_slot_count;
    std::size_t gpu_reverse_slot_count;
};

}  // namespace pv
