#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pv {

struct PngInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t decoded_bytes = 0;
};

std::optional<PngInfo> ParsePngHeader(std::span<const std::byte> bytes) noexcept;

}  // namespace pv
