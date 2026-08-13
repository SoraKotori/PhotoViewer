#pragma once

#include <cstdint>
#include <utility>

namespace pv {

// Decides whether kernel-visible catalog backing may be released. Cancel and
// wait operations are injected so the production decision is synchronously
// testable without creating a directory query or another thread.
template <typename Cancel, typename Wait>
bool DrainCatalogQueryForShutdown(const bool query_active,
                                  const bool kernel_pending,
                                  Cancel&& cancel, Wait&& wait,
                                  const std::uint32_t timeout_ms) noexcept {
    if (!query_active || !kernel_pending) return true;
    if (!std::forward<Cancel>(cancel)()) return false;
    return std::forward<Wait>(wait)(timeout_ms);
}

}  // namespace pv
