#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pv {

inline constexpr std::size_t kPngHeaderBytes = 33;
inline constexpr std::uint32_t kMaximumPngTextureDimension = 16384;

// A value-only resource contract produced by the main thread from signature
// and IHDR. Later stages consume it; they do not derive allocation sizes or
// texture shapes again.
struct PngResourcePlan {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_bytes = 0;
    std::uint32_t decoded_bytes = 0;
    std::uint32_t filter_workspace_bytes = 0;
    std::uint32_t staging_committed_bytes = 0;
    std::uint32_t texture_width = 0;
    std::uint32_t texture_height = 0;
    std::uint32_t gpu_reservation_bytes = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    std::uint8_t interlace_method = 0;
};

std::optional<PngResourcePlan> ParsePngResourcePlan(
    std::span<const std::byte> bytes) noexcept;

}  // namespace pv
