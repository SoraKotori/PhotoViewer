#pragma once

#include <cstddef>
#include <cstdint>

namespace pv {

struct PngDecodeTimings;

namespace png_internal {

// Direct row-filter seams keep SIMD behavior independently testable from
// DEFLATE streaming and callback scheduling.
[[nodiscard]] bool UnfilterRgba8Row(
    std::uint8_t* destination, const std::uint8_t* source,
    const std::uint8_t* previous, std::size_t row_bytes,
    std::uint8_t filter) noexcept;
[[nodiscard]] bool UnfilterRgba8RowWithTimings(
    std::uint8_t* destination, const std::uint8_t* source,
    const std::uint8_t* previous, std::size_t row_bytes,
    std::uint8_t filter, PngDecodeTimings& timings) noexcept;

void UnfilterPaethRow(std::uint8_t* destination,
                      const std::uint8_t* source,
                      const std::uint8_t* previous,
                      std::size_t row_bytes) noexcept;
void UnfilterPaethRows4(std::uint8_t* destination,
                        const std::uint8_t* source,
                        std::size_t row_bytes,
                        std::uint8_t* scratch) noexcept;
void UnfilterPaethRows8(std::uint8_t* destination,
                        const std::uint8_t* source,
                        std::size_t row_bytes,
                        std::uint8_t* scratch) noexcept;

}  // namespace png_internal
}  // namespace pv
