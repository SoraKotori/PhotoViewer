#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pv {

struct Config {
    std::filesystem::path initial_image;
    std::size_t cpu_cache_bytes = 2048ULL * 1024ULL * 1024ULL;
    std::size_t gpu_cache_bytes = 768ULL * 1024ULL * 1024ULL;
    std::size_t compressed_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::size_t worker_count = 12;
    std::size_t work_queue_capacity = 64;
    bool validation_exit_after_present = false;
    bool validation_elapsed_exit_code = false;
    bool validation_fullscreen = false;
    std::wstring validation_navigation;
    std::filesystem::path validation_file_list;
    std::filesystem::path validation_report;
    std::uint32_t validation_timeout_ms = 10000;
    std::uint32_t validation_warmup_ms = 0;
    std::uint32_t validation_navigation_interval_ms = 0;
};

Config ParseConfig();

}  // namespace pv
