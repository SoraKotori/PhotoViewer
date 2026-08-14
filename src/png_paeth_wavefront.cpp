#include "png_unfilter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>

namespace pv::png_internal {
namespace {
std::uint8_t PaethByte(const std::uint8_t left, const std::uint8_t up,
                       const std::uint8_t upper_left) noexcept {
    const int distance_left = std::abs(static_cast<int>(up) - upper_left);
    const int distance_up = std::abs(static_cast<int>(left) - upper_left);
    const int distance_upper_left = std::abs(
        static_cast<int>(left) + up - 2 * static_cast<int>(upper_left));
    if (distance_left <= distance_up &&
        distance_left <= distance_upper_left) {
        return left;
    }
    return distance_up <= distance_upper_left ? up : upper_left;
}

std::uint32_t LoadPixelValue(const std::uint8_t* const source) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

__m256i ShiftPaethRows(const __m256i rows,
                       const std::uint32_t first) noexcept {
    const __m256i shifted = _mm256_permute4x64_epi64(
        rows, _MM_SHUFFLE(2, 1, 0, 0));
    const __m256i expanded_first = _mm256_cvtepu8_epi16(
        _mm_cvtsi32_si128(static_cast<int>(first)));
    return _mm256_blend_epi32(shifted, expanded_first, 0x03);
}

void StorePixelValue(std::uint8_t* const destination,
                     const std::uint32_t value) noexcept {
    std::memcpy(destination, &value, sizeof(value));
}

void DecodePaethPixelValue(std::uint8_t* const destination,
                           const std::uint8_t* const previous,
                           const std::size_t pixel,
                           const std::uint32_t filtered) noexcept {
    const std::size_t offset = pixel * 4;
    const std::uint32_t left = pixel == 0
        ? 0
        : LoadPixelValue(destination + offset - 4);
    const std::uint32_t up = previous
        ? LoadPixelValue(previous + offset)
        : 0;
    const std::uint32_t upper_left = previous && pixel != 0
        ? LoadPixelValue(previous + offset - 4)
        : 0;
    std::uint32_t decoded = 0;
    for (unsigned int channel = 0; channel < 4; ++channel) {
        const unsigned int shift = channel * 8;
        const auto byte = [](const std::uint32_t value,
                             const unsigned int bit) noexcept {
            return static_cast<std::uint8_t>(value >> bit);
        };
        const std::uint8_t prediction = PaethByte(
            byte(left, shift), byte(up, shift), byte(upper_left, shift));
        const std::uint8_t decoded_byte = static_cast<std::uint8_t>(
            byte(filtered, shift) + prediction);
        decoded |= static_cast<std::uint32_t>(decoded_byte) << shift;
    }
    StorePixelValue(destination + offset, decoded);
}

void DecodePaethPixel(std::uint8_t* const destination,
                      const std::uint8_t* const source,
                      const std::uint8_t* const previous,
                      const std::size_t pixel) noexcept {
    DecodePaethPixelValue(destination, previous, pixel,
                          LoadPixelValue(source + pixel * 4));
}

std::uint32_t LoadFilteredPixel(const std::uint8_t* const source,
                                const std::size_t row_bytes,
                                const std::uint8_t* const preserved_tail,
                                const std::size_t preserved_bytes,
                                const std::size_t pixel) noexcept {
    const std::size_t offset = pixel * 4;
    const std::size_t preserved_begin = row_bytes - preserved_bytes;
    if (offset + 4 <= preserved_begin) {
        return LoadPixelValue(source + offset);
    }
    if (offset >= preserved_begin) {
        return LoadPixelValue(preserved_tail + offset - preserved_begin);
    }
    std::uint32_t value = 0;
    for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
        const std::size_t position = offset + byte_index;
        const std::uint8_t byte = position < preserved_begin
            ? source[position]
            : preserved_tail[position - preserved_begin];
        value |= static_cast<std::uint32_t>(byte) << (byte_index * 8);
    }
    return value;
}

void AddPaethRows4(std::uint8_t* const destination,
                   const std::uint8_t* const source,
                   const std::size_t row_bytes,
                   std::uint8_t* const scratch) noexcept {
    const std::size_t pixels = row_bytes / 4;
    std::uint8_t* const destination0 = destination;
    std::uint8_t* const destination1 = destination0 + row_bytes;
    std::uint8_t* const destination2 = destination1 + row_bytes;
    std::uint8_t* const destination3 = destination2 + row_bytes;
    const std::uint8_t* const source0 = source;
    const std::uint8_t* const source1 = source0 + row_bytes + 1;
    const std::uint8_t* const source2 = source1 + row_bytes + 1;
    const std::uint8_t* const source3 = source2 + row_bytes + 1;
    const std::uint8_t* const previous0 = destination0 - row_bytes;

    const std::size_t preserved0 = static_cast<std::size_t>(source0 - destination0);
    const std::size_t preserved1 = static_cast<std::size_t>(source1 - destination1);
    const std::size_t preserved2 = static_cast<std::size_t>(source2 - destination2);
    if (pixels < 4 || preserved2 >= row_bytes) {
        UnfilterPaethRow(destination0, source0, previous0, row_bytes);
        UnfilterPaethRow(destination1, source1, destination0, row_bytes);
        UnfilterPaethRow(destination2, source2, destination1, row_bytes);
        UnfilterPaethRow(destination3, source3, destination2, row_bytes);
        return;
    }
    std::uint8_t* const tail0 = scratch;
    std::uint8_t* const tail1 = tail0 + preserved0;
    std::uint8_t* const tail2 = tail1 + preserved1;
    std::memcpy(tail0, source0 + row_bytes - preserved0, preserved0);
    std::memcpy(tail1, source1 + row_bytes - preserved1, preserved1);
    std::memcpy(tail2, source2 + row_bytes - preserved2, preserved2);

    for (std::size_t pixel = 0; pixel < 3; ++pixel) {
        DecodePaethPixel(destination0, source0, previous0, pixel);
    }
    for (std::size_t pixel = 0; pixel < 2; ++pixel) {
        DecodePaethPixel(destination1, source1, destination0, pixel);
    }
    DecodePaethPixel(destination2, source2, destination1, 0);

    __m256i left = _mm256_cvtepu8_epi16(_mm_setr_epi32(
        static_cast<int>(LoadPixelValue(destination0 + 8)),
        static_cast<int>(LoadPixelValue(destination1 + 4)),
        static_cast<int>(LoadPixelValue(destination2)), 0));
    __m256i upper_left = _mm256_cvtepu8_epi16(_mm_setr_epi32(
        static_cast<int>(LoadPixelValue(previous0 + 8)),
        static_cast<int>(LoadPixelValue(destination0 + 4)),
        static_cast<int>(LoadPixelValue(destination1)), 0));
    const auto decode_wavefront = [&](const std::size_t pixel,
                                      const __m256i filtered) noexcept {
        const __m256i up = ShiftPaethRows(
            left, LoadPixelValue(previous0 + (pixel + 3) * 4));
        const __m256i up_delta = _mm256_sub_epi16(up, upper_left);
        const __m256i left_delta = _mm256_sub_epi16(left, upper_left);
        const __m256i distance_left = _mm256_abs_epi16(up_delta);
        const __m256i distance_up = _mm256_abs_epi16(left_delta);
        const __m256i distance_upper_left = _mm256_abs_epi16(
            _mm256_add_epi16(up_delta, left_delta));
        const __m256i choose_up = _mm256_cmpgt_epi16(
            distance_left, distance_up);
        const __m256i left_or_up = _mm256_blendv_epi8(left, up, choose_up);
        const __m256i choose_upper_left = _mm256_cmpgt_epi16(
            _mm256_min_epi16(distance_left, distance_up),
            distance_upper_left);
        const __m256i prediction = _mm256_blendv_epi8(
            left_or_up, upper_left, choose_upper_left);
        const __m256i decoded = _mm256_add_epi8(filtered, prediction);
        const __m256i packed = _mm256_packus_epi16(decoded, decoded);
        const __m128i pixels01 = _mm256_castsi256_si128(packed);
        const __m128i pixels23 = _mm256_extracti128_si256(packed, 1);
        const __m128i values = _mm_unpacklo_epi64(pixels01, pixels23);
        // This diagonal's up pixels are the next diagonal's upper-left pixels.
        upper_left = up;
        left = decoded;
        return values;
    };
    const auto store_wavefront = [&](const std::size_t pixel,
                                     const __m128i values) noexcept {
        StorePixelValue(destination0 + (pixel + 3) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 0)));
        StorePixelValue(destination1 + (pixel + 2) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 1)));
        StorePixelValue(destination2 + (pixel + 1) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 2)));
        StorePixelValue(destination3 + pixel * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 3)));
    };
    const auto load_wavefront = [&](const std::size_t pixel) noexcept {
        return _mm256_cvtepu8_epi16(_mm_setr_epi32(
            static_cast<int>(LoadPixelValue(source0 + (pixel + 3) * 4)),
            static_cast<int>(LoadPixelValue(source1 + (pixel + 2) * 4)),
            static_cast<int>(LoadPixelValue(source2 + (pixel + 1) * 4)),
            static_cast<int>(LoadPixelValue(source3 + pixel * 4))));
    };

    const auto direct_diagonal_end = [](const std::size_t full_pixels,
                                        const std::size_t row_offset) noexcept {
        return full_pixels > row_offset ? full_pixels - row_offset : 0;
    };
    std::size_t direct_end = pixels - 3;
    direct_end = std::min(
        direct_end,
        direct_diagonal_end((row_bytes - preserved0) / 4, 3));
    direct_end = std::min(
        direct_end,
        direct_diagonal_end((row_bytes - preserved1) / 4, 2));
    direct_end = std::min(
        direct_end,
        direct_diagonal_end((row_bytes - preserved2) / 4, 1));

    std::size_t pixel = 0;
    // Batch four diagonals while every source pixel is still before its
    // preserved tail. This keeps overlap checks out of the hot region without
    // reading bytes overwritten by the following compacted rows.
    for (; pixel + 4 <= direct_end; pixel += 4) {
        const __m128i diagonal0 = decode_wavefront(
            pixel, load_wavefront(pixel));
        const __m128i diagonal1 = decode_wavefront(
            pixel + 1, load_wavefront(pixel + 1));
        const __m128i diagonal2 = decode_wavefront(
            pixel + 2, load_wavefront(pixel + 2));
        const __m128i diagonal3 = decode_wavefront(
            pixel + 3, load_wavefront(pixel + 3));

        const __m128i rows01_low = _mm_unpacklo_epi32(diagonal0, diagonal1);
        const __m128i rows23_low = _mm_unpacklo_epi32(diagonal2, diagonal3);
        const __m128i rows01_high = _mm_unpackhi_epi32(diagonal0, diagonal1);
        const __m128i rows23_high = _mm_unpackhi_epi32(diagonal2, diagonal3);
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destination0 + (pixel + 3) * 4),
            _mm_unpacklo_epi64(rows01_low, rows23_low));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destination1 + (pixel + 2) * 4),
            _mm_unpackhi_epi64(rows01_low, rows23_low));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destination2 + (pixel + 1) * 4),
            _mm_unpacklo_epi64(rows01_high, rows23_high));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destination3 + pixel * 4),
            _mm_unpackhi_epi64(rows01_high, rows23_high));
    }
    for (; pixel < direct_end; ++pixel) {
        store_wavefront(pixel, decode_wavefront(pixel, load_wavefront(pixel)));
    }
    for (; pixel + 3 < pixels; ++pixel) {
        const __m256i filtered = _mm256_cvtepu8_epi16(_mm_setr_epi32(
            static_cast<int>(LoadFilteredPixel(
                source0, row_bytes, tail0, preserved0, pixel + 3)),
            static_cast<int>(LoadFilteredPixel(
                source1, row_bytes, tail1, preserved1, pixel + 2)),
            static_cast<int>(LoadFilteredPixel(
                source2, row_bytes, tail2, preserved2, pixel + 1)),
            static_cast<int>(LoadPixelValue(source3 + pixel * 4))));
        store_wavefront(pixel, decode_wavefront(pixel, filtered));
    }

    DecodePaethPixelValue(destination1, destination0, pixels - 1,
                          LoadFilteredPixel(source1, row_bytes, tail1,
                                            preserved1, pixels - 1));
    for (std::size_t tail_pixel = pixels - 2; tail_pixel < pixels; ++tail_pixel) {
        DecodePaethPixelValue(destination2, destination1, tail_pixel,
                              LoadFilteredPixel(source2, row_bytes, tail2,
                                                preserved2, tail_pixel));
    }
    for (std::size_t tail_pixel = pixels - 3; tail_pixel < pixels; ++tail_pixel) {
        DecodePaethPixel(destination3, source3, destination2, tail_pixel);
    }
}

void AddPaethRows8(std::uint8_t* const destination,
                   const std::uint8_t* const source,
                   const std::size_t row_bytes,
                   std::uint8_t* const scratch) noexcept {
    constexpr std::size_t row_count = 8;
    constexpr std::size_t preserved_row_count = row_count - 1;
    const std::size_t pixels = row_bytes / 4;
    std::array<std::uint8_t*, row_count> destinations{};
    std::array<const std::uint8_t*, row_count> sources{};
    destinations[0] = destination;
    sources[0] = source;
    for (std::size_t row = 1; row < row_count; ++row) {
        destinations[row] = destinations[row - 1] + row_bytes;
        sources[row] = sources[row - 1] + row_bytes + 1;
    }
    const std::uint8_t* const previous0 = destinations[0] - row_bytes;

    if (row_bytes >
        (static_cast<std::size_t>(std::numeric_limits<int>::max()) - 28) / 7) {
        for (std::size_t row = 0; row < row_count; ++row) {
            UnfilterPaethRow(
                destinations[row], sources[row],
                row == 0 ? previous0 : destinations[row - 1], row_bytes);
        }
        return;
    }

    std::array<std::size_t, preserved_row_count> preserved{};
    for (std::size_t row = 0; row < preserved.size(); ++row) {
        preserved[row] = static_cast<std::size_t>(
            sources[row] - destinations[row]);
    }
    if (pixels < row_count || preserved.back() >= row_bytes) {
        for (std::size_t row = 0; row < row_count; ++row) {
            UnfilterPaethRow(
                destinations[row], sources[row],
                row == 0 ? previous0 : destinations[row - 1], row_bytes);
        }
        return;
    }

    std::array<std::uint8_t*, preserved_row_count> tails{};
    std::uint8_t* tail = scratch;
    for (std::size_t row = 0; row < tails.size(); ++row) {
        tails[row] = tail;
        std::memcpy(tail, sources[row] + row_bytes - preserved[row],
                    preserved[row]);
        tail += preserved[row];
    }

    for (std::size_t row = 0; row < preserved_row_count; ++row) {
        const std::uint8_t* const previous =
            row == 0 ? previous0 : destinations[row - 1];
        for (std::size_t pixel = 0;
             pixel < preserved_row_count - row;
             ++pixel) {
            DecodePaethPixel(destinations[row], sources[row], previous, pixel);
        }
    }

    __m256i left = _mm256_setr_epi32(
        static_cast<int>(LoadPixelValue(destinations[0] + 6 * 4)),
        static_cast<int>(LoadPixelValue(destinations[1] + 5 * 4)),
        static_cast<int>(LoadPixelValue(destinations[2] + 4 * 4)),
        static_cast<int>(LoadPixelValue(destinations[3] + 3 * 4)),
        static_cast<int>(LoadPixelValue(destinations[4] + 2 * 4)),
        static_cast<int>(LoadPixelValue(destinations[5] + 1 * 4)),
        static_cast<int>(LoadPixelValue(destinations[6])), 0);
    __m256i upper_left = _mm256_setr_epi32(
        static_cast<int>(LoadPixelValue(previous0 + 6 * 4)),
        static_cast<int>(LoadPixelValue(destinations[0] + 5 * 4)),
        static_cast<int>(LoadPixelValue(destinations[1] + 4 * 4)),
        static_cast<int>(LoadPixelValue(destinations[2] + 3 * 4)),
        static_cast<int>(LoadPixelValue(destinations[3] + 2 * 4)),
        static_cast<int>(LoadPixelValue(destinations[4] + 1 * 4)),
        static_cast<int>(LoadPixelValue(destinations[5])), 0);

    const auto absolute_difference = [](
        const __m256i a, const __m256i b) noexcept {
        return _mm256_sub_epi8(_mm256_max_epu8(a, b),
                               _mm256_min_epu8(a, b));
    };
    const auto decode_diagonal = [&](const std::size_t pixel,
                                     const __m256i filtered) noexcept {
        const __m256i shift = _mm256_permutevar8x32_epi32(
            left, _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6));
        const __m256i first_up = _mm256_castsi128_si256(
            _mm_cvtsi32_si128(static_cast<int>(
                LoadPixelValue(previous0 + (pixel + 7) * 4))));
        const __m256i up = _mm256_blend_epi32(shift, first_up, 0x01);
        const __m256i distance_left = absolute_difference(up, upper_left);
        const __m256i distance_up = absolute_difference(left, upper_left);
        const __m256i left_above = _mm256_cmpeq_epi8(
            _mm256_max_epu8(left, upper_left), left);
        const __m256i up_above = _mm256_cmpeq_epi8(
            _mm256_max_epu8(up, upper_left), up);
        const __m256i same_side = _mm256_cmpeq_epi8(left_above, up_above);
        const __m256i zero = _mm256_setzero_si256();
        const __m256i opposite_side_distance = absolute_difference(
            distance_left, distance_up);
        const __m256i nearest_left_or_up = _mm256_min_epu8(
            distance_left, distance_up);
        const __m256i choose_up = _mm256_cmpeq_epi8(
            nearest_left_or_up, distance_up);
        const __m256i left_or_up = _mm256_blendv_epi8(left, up, choose_up);
        const __m256i reject_upper_left = _mm256_or_si256(
            same_side,
            _mm256_cmpeq_epi8(
                _mm256_subs_epu8(
                    nearest_left_or_up, opposite_side_distance),
                zero));
        const __m256i prediction = _mm256_blendv_epi8(
            upper_left, left_or_up, reject_upper_left);
        const __m256i decoded = _mm256_add_epi8(filtered, prediction);
        upper_left = up;
        left = decoded;
        return decoded;
    };
    const __m256i gather_offsets = _mm256_setr_epi32(
        7 * 4, static_cast<int>((row_bytes + 1) + 6 * 4),
        static_cast<int>(2 * (row_bytes + 1) + 5 * 4),
        static_cast<int>(3 * (row_bytes + 1) + 4 * 4),
        static_cast<int>(4 * (row_bytes + 1) + 3 * 4),
        static_cast<int>(5 * (row_bytes + 1) + 2 * 4),
        static_cast<int>(6 * (row_bytes + 1) + 1 * 4),
        static_cast<int>(7 * (row_bytes + 1)));
    const auto load_diagonal = [&](const std::size_t pixel) noexcept {
        return _mm256_i32gather_epi32(
            reinterpret_cast<const int*>(sources[0] + pixel * 4),
            gather_offsets, 1);
    };
    const auto store_diagonal = [&](const std::size_t first_row,
                                    const std::size_t pixel,
                                    const __m128i values) noexcept {
        alignas(16) std::array<std::uint32_t, 4> lanes{};
        _mm_store_si128(reinterpret_cast<__m128i*>(lanes.data()), values);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            StorePixelValue(
                destinations[first_row + lane] +
                    (pixel + 7 - first_row - lane) * 4,
                lanes[lane]);
        }
    };
    const auto store_four_diagonals = [&](const std::size_t first_row,
                                          const std::size_t pixel,
                                          const std::array<__m128i, 4>& values) noexcept {
        const __m128i rows01_low = _mm_unpacklo_epi32(values[0], values[1]);
        const __m128i rows23_low = _mm_unpacklo_epi32(values[2], values[3]);
        const __m128i rows01_high = _mm_unpackhi_epi32(values[0], values[1]);
        const __m128i rows23_high = _mm_unpackhi_epi32(values[2], values[3]);
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destinations[first_row] +
                                       (pixel + 7 - first_row) * 4),
            _mm_unpacklo_epi64(rows01_low, rows23_low));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destinations[first_row + 1] +
                                       (pixel + 6 - first_row) * 4),
            _mm_unpackhi_epi64(rows01_low, rows23_low));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destinations[first_row + 2] +
                                       (pixel + 5 - first_row) * 4),
            _mm_unpacklo_epi64(rows01_high, rows23_high));
        _mm_storeu_si128(
            reinterpret_cast<__m128i*>(destinations[first_row + 3] +
                                       (pixel + 4 - first_row) * 4),
            _mm_unpackhi_epi64(rows01_high, rows23_high));
    };
    const auto direct_diagonal_end = [](const std::size_t full_pixels,
                                         const std::size_t row_offset) noexcept {
        return full_pixels > row_offset ? full_pixels - row_offset : 0;
    };
    std::size_t direct_end = pixels - preserved_row_count;
    for (std::size_t row = 0; row < preserved.size(); ++row) {
        direct_end = std::min(
            direct_end,
            direct_diagonal_end((row_bytes - preserved[row]) / 4,
                                preserved_row_count - row));
    }

    std::size_t pixel = 0;
    for (; pixel + 4 <= direct_end; pixel += 4) {
        std::array<__m128i, 4> values0{};
        std::array<__m128i, 4> values1{};
        for (std::size_t diagonal = 0; diagonal < 4; ++diagonal) {
            const __m256i values = decode_diagonal(
                pixel + diagonal, load_diagonal(pixel + diagonal));
            values0[diagonal] = _mm256_castsi256_si128(values);
            values1[diagonal] = _mm256_extracti128_si256(values, 1);
        }
        store_four_diagonals(0, pixel, values0);
        store_four_diagonals(4, pixel, values1);
    }
    for (; pixel < direct_end; ++pixel) {
        const __m256i values = decode_diagonal(pixel, load_diagonal(pixel));
        store_diagonal(0, pixel, _mm256_castsi256_si128(values));
        store_diagonal(4, pixel, _mm256_extracti128_si256(values, 1));
    }
    for (; pixel + preserved_row_count < pixels; ++pixel) {
        const __m256i filtered = _mm256_setr_epi32(
            static_cast<int>(LoadFilteredPixel(
                sources[0], row_bytes, tails[0], preserved[0], pixel + 7)),
            static_cast<int>(LoadFilteredPixel(
                sources[1], row_bytes, tails[1], preserved[1], pixel + 6)),
            static_cast<int>(LoadFilteredPixel(
                sources[2], row_bytes, tails[2], preserved[2], pixel + 5)),
            static_cast<int>(LoadFilteredPixel(
                sources[3], row_bytes, tails[3], preserved[3], pixel + 4)),
            static_cast<int>(LoadFilteredPixel(
                sources[4], row_bytes, tails[4], preserved[4], pixel + 3)),
            static_cast<int>(LoadFilteredPixel(
                sources[5], row_bytes, tails[5], preserved[5], pixel + 2)),
            static_cast<int>(LoadFilteredPixel(
                sources[6], row_bytes, tails[6], preserved[6], pixel + 1)),
            static_cast<int>(LoadPixelValue(sources[7] + pixel * 4)));
        const __m256i values = decode_diagonal(pixel, filtered);
        store_diagonal(0, pixel, _mm256_castsi256_si128(values));
        store_diagonal(4, pixel, _mm256_extracti128_si256(values, 1));
    }

    for (std::size_t row = 1; row < preserved_row_count; ++row) {
        for (std::size_t tail_pixel = pixels - row;
             tail_pixel < pixels;
             ++tail_pixel) {
            DecodePaethPixelValue(
                destinations[row], destinations[row - 1], tail_pixel,
                LoadFilteredPixel(sources[row], row_bytes, tails[row],
                                  preserved[row], tail_pixel));
        }
    }
    for (std::size_t tail_pixel = pixels - preserved_row_count;
         tail_pixel < pixels;
         ++tail_pixel) {
        DecodePaethPixel(destinations.back(), sources.back(),
                         destinations[preserved_row_count - 1], tail_pixel);
    }
}

}  // namespace

void UnfilterPaethRows4(
    std::uint8_t* const destination, const std::uint8_t* const source,
    const std::size_t row_bytes, std::uint8_t* const scratch) noexcept {
    AddPaethRows4(destination, source, row_bytes, scratch);
}

void UnfilterPaethRows8(
    std::uint8_t* const destination, const std::uint8_t* const source,
    const std::size_t row_bytes, std::uint8_t* const scratch) noexcept {
    AddPaethRows8(destination, source, row_bytes, scratch);
}

}  // namespace pv::png_internal
