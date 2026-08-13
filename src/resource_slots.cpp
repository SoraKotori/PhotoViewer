#include "resource_slots.h"

#include <algorithm>
#include <stdexcept>

namespace pv {

ResourceSlots::ResourceSlots(const std::size_t compressed_count,
                             const std::size_t staging_count,
                             const std::size_t gpu_texture_count,
                             const std::size_t compressed_budget,
                             const std::size_t staging_budget)
    : compressed_(compressed_count),
      staging_(staging_count),
      gpu_textures_(gpu_texture_count),
      compressed_budget_(compressed_budget),
      staging_budget_(staging_budget) {
    InitializeFree(free_compressed_, compressed_count);
    InitializeFree(free_staging_, staging_count);
    InitializeFree(free_gpu_textures_, gpu_texture_count);
}

void ResourceSlots::InitializeFree(std::vector<SlotId>& free_index,
                                   const std::size_t count) {
    free_index.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        free_index.push_back(static_cast<SlotId>(count - 1 - index));
    }
}

void ResourceSlots::RemoveFree(std::vector<SlotId>& free_index,
                               const SlotId id) {
    const auto found = std::find(free_index.begin(), free_index.end(), id);
    if (found == free_index.end()) {
        throw std::logic_error("slot missing from free index");
    }
    *found = free_index.back();
    free_index.pop_back();
}

}  // namespace pv
