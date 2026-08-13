#include "png.h"

#include "../third_party/libdeflate/libdeflate.h"

#include <array>
#include <limits>

namespace pv {
namespace {

constexpr std::array<std::byte, 8> kPngSignature{
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};

constexpr std::uint64_t kMaximumRowBytes =
    static_cast<std::uint64_t>(kMaximumPngTextureDimension) * 4;
constexpr std::uint64_t kMaximumDecodedBytes =
    kMaximumRowBytes * kMaximumPngTextureDimension;
constexpr std::uint64_t kMaximumFilterWorkspaceBytes =
    kMaximumRowBytes + kMaximumPngTextureDimension;
constexpr std::uint64_t kMaximumStagingBytes =
    kMaximumDecodedBytes + kMaximumFilterWorkspaceBytes;
static_assert(kMaximumStagingBytes <=
              std::numeric_limits<std::uint32_t>::max());

std::uint32_t ReadBigEndian(const std::byte* const data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

bool IsValidColorDepth(const std::uint8_t color,
                       const std::uint8_t depth) noexcept {
    switch (color) {
        case 0:
            return depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
                   depth == 16;
        case 2:
        case 4:
        case 6:
            return depth == 8 || depth == 16;
        case 3:
            return depth == 1 || depth == 2 || depth == 4 || depth == 8;
        default:
            return false;
    }
}

std::optional<PngResourcePlan> BuildPlan(const std::uint32_t width,
                                         const std::uint32_t height,
                                         const std::uint8_t bit_depth,
                                         const std::uint8_t color_type,
                                         const std::uint8_t interlace) noexcept {
    if (width == 0 || height == 0 ||
        width > kMaximumPngTextureDimension ||
        height > kMaximumPngTextureDimension) {
        return std::nullopt;
    }

    const std::uint32_t row_bytes = width * 4;
    const std::uint32_t decoded_bytes = row_bytes * height;
    const std::uint32_t filter_workspace_bytes = row_bytes + height;
    const std::uint32_t staging_committed_bytes =
        decoded_bytes + filter_workspace_bytes;

    const std::uint32_t extra_rows =
        (height + row_bytes - 1) / row_bytes;
    std::uint32_t texture_width = width;
    std::uint32_t texture_height = height;
    if (extra_rows <= kMaximumPngTextureDimension - height) {
        texture_height += extra_rows;
    } else if (width < kMaximumPngTextureDimension) {
        ++texture_width;
    } else {
        return std::nullopt;
    }

    return PngResourcePlan{
        width,
        height,
        row_bytes,
        decoded_bytes,
        filter_workspace_bytes,
        staging_committed_bytes,
        texture_width,
        texture_height,
        decoded_bytes,
        bit_depth,
        color_type,
        interlace,
    };
}

}  // namespace

std::optional<PngResourcePlan> ParsePngResourcePlan(
    const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kPngHeaderBytes ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin()) ||
        ReadBigEndian(bytes.data() + 8) != 13 ||
        bytes[12] != std::byte{'I'} || bytes[13] != std::byte{'H'} ||
        bytes[14] != std::byte{'D'} || bytes[15] != std::byte{'R'}) {
        return std::nullopt;
    }

    const std::uint32_t expected_crc = ReadBigEndian(bytes.data() + 29);
    if (libdeflate_crc32(0, bytes.data() + 12, 17) != expected_crc) {
        return std::nullopt;
    }

    const std::uint8_t bit_depth = static_cast<std::uint8_t>(bytes[24]);
    const std::uint8_t color_type = static_cast<std::uint8_t>(bytes[25]);
    const std::uint8_t compression = static_cast<std::uint8_t>(bytes[26]);
    const std::uint8_t filter = static_cast<std::uint8_t>(bytes[27]);
    const std::uint8_t interlace = static_cast<std::uint8_t>(bytes[28]);
    if (!IsValidColorDepth(color_type, bit_depth) || compression != 0 ||
        filter != 0 || interlace > 1) {
        return std::nullopt;
    }

    return BuildPlan(ReadBigEndian(bytes.data() + 16),
                     ReadBigEndian(bytes.data() + 20), bit_depth, color_type,
                     interlace);
}

}  // namespace pv
