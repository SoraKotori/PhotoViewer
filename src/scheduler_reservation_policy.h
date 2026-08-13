#pragma once

#include "pipeline_resource_size.h"
#include "reservation.h"

#include <algorithm>
#include <vector>

namespace pv::scheduler_policy {

inline void AppendUniqueFrame(std::vector<std::size_t>& frames,
                              const std::size_t frame,
                              const std::size_t capacity) {
    if (frames.size() < capacity &&
        std::find(frames.begin(), frames.end(), frame) == frames.end()) {
        frames.push_back(frame);
    }
}

template <typename BytesFor>
bool BudgetAllows(const std::size_t candidate,
                  const std::vector<ReservationEntry>& entries,
                  const std::size_t budget, BytesFor&& bytes_for) {
    std::size_t used = 0;
    for (const ReservationEntry& entry : entries) {
        if (entry.frame == kInvalidFrame) continue;
        const auto bytes = bytes_for(entry.frame);
        if (!bytes || !AddWithinBudget(*bytes, budget, used)) return false;
    }
    const auto candidate_bytes = bytes_for(candidate);
    return candidate_bytes && AddWithinBudget(*candidate_bytes, budget, used);
}

}  // namespace pv::scheduler_policy
