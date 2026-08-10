#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace pv {

struct Config {
    std::filesystem::path initial_image;
    std::size_t staging_cache_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t gpu_cache_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t compressed_budget_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t staging_slot_count = 12;
    std::size_t gpu_forward_slot_count = 3;
    std::size_t gpu_reverse_slot_count = 1;
    std::size_t compressed_slot_count = 10;
    std::size_t worker_count = 0;
    bool validation_exit_after_present = false;
    bool validation_elapsed_exit_code = false;
    bool validation_fullscreen = false;
    bool validation_short_presses = false;
    std::wstring validation_navigation;
    std::filesystem::path validation_file_list;
    std::filesystem::path validation_report;
    std::uint32_t validation_timeout_ms = 10000;
    std::uint32_t validation_warmup_ms = 0;
    std::uint32_t validation_navigation_interval_ms = 0;

    [[nodiscard]] std::size_t GpuSlotCount() const noexcept {
        return gpu_forward_slot_count + gpu_reverse_slot_count;
    }
};

Config ParseConfig();

}  // namespace pv
