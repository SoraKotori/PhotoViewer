#pragma once

#include "navigation.h"

#include <optional>

namespace pv {

// Intermediate stages may complete in any frame order. Only the frame named by
// NavigationState may cross the presentation boundary.
template <class Ready>
[[nodiscard]] std::optional<std::size_t> NextPresentableFrame(
    const NavigationState& navigation, Ready&& ready) {
    const auto next = navigation.NextIndex();
    return next && ready(*next) ? next : std::nullopt;
}

}  // namespace pv
