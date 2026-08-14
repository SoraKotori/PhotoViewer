#include "pipeline_limits.h"

#include "config.h"

#include <limits>
#include <stdexcept>

namespace pv {

PipelineLimits::PipelineLimits(const Config& config)
    : compressed_budget_bytes(config.compressed_budget_bytes),
      staging_cache_bytes(config.staging_cache_bytes),
      gpu_cache_bytes(config.gpu_cache_bytes),
      compressed_slot_count(config.compressed_slot_count),
      staging_slot_count(config.staging_slot_count),
      gpu_forward_slot_count(config.gpu_forward_slot_count),
      gpu_reverse_slot_count(config.gpu_reverse_slot_count) {
    if (compressed_budget_bytes == 0 || staging_cache_bytes == 0 ||
        gpu_cache_bytes == 0 || compressed_slot_count == 0 ||
        staging_slot_count == 0 || gpu_forward_slot_count == 0) {
        throw std::invalid_argument("pipeline limits must be nonzero");
    }
    if (gpu_forward_slot_count >
        std::numeric_limits<std::size_t>::max() - gpu_reverse_slot_count) {
        throw std::invalid_argument("GPU slot count overflow");
    }
}

}  // namespace pv
