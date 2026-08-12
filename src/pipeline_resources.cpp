#include "pipeline_resources.h"

namespace pv {

PipelineResources::PipelineResources(const PipelineLimits& limits)
    : slots_(limits.compressed_slot_count, limits.staging_slot_count,
             limits.GpuSlotCount(), limits.compressed_budget_bytes,
             limits.staging_cache_bytes) {}

}  // namespace pv
