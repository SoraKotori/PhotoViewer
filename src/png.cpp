#include "png.h"

#include <array>
#include <limits>

namespace pv {
namespace {

std::uint32_t ReadBigEndian(const std::byte* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

}  // namespace

std::optional<PngInfo> ParsePngHeader(const std::span<const std::byte> bytes) noexcept {
    constexpr std::array<std::byte, 8> signature{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    if (bytes.size() < 24 || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        return std::nullopt;
    }
    if (ReadBigEndian(bytes.data() + 8) != 13 ||
        bytes[12] != std::byte{'I'} || bytes[13] != std::byte{'H'} ||
        bytes[14] != std::byte{'D'} || bytes[15] != std::byte{'R'}) {
        return std::nullopt;
    }
    const std::uint32_t width = ReadBigEndian(bytes.data() + 16);
    const std::uint32_t height = ReadBigEndian(bytes.data() + 20);
    if (width == 0 || height == 0) return std::nullopt;
    constexpr std::size_t bytes_per_pixel = 4;
    if (width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel) return std::nullopt;
    const std::size_t stride = static_cast<std::size_t>(width) * bytes_per_pixel;
    if (height > std::numeric_limits<std::size_t>::max() / stride) return std::nullopt;
    return PngInfo{width, height, stride * height};
}

}  // namespace pv
