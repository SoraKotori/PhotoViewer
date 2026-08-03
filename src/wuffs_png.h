#pragma once

#include "model.h"

namespace pv {

HRESULT DecodePngWuffs(std::span<const std::byte> compressed,
                       CpuSurface& surface) noexcept;

}  // namespace pv
