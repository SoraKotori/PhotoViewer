#include "png_rgba8_decoder.h"

#include <wincodec.h>

#include "../third_party/libdeflate/libdeflate.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace pv {
namespace {
std::uint32_t ReadBigEndian32(const std::uint8_t* const data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

void CopyForward(std::byte* const destination, const std::byte* const source,
                 const std::size_t bytes) noexcept {
    std::size_t offset = 0;
    for (; offset + 128 <= bytes; offset += 128) {
        const __m256i block0 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset));
        const __m256i block1 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset + 32));
        const __m256i block2 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset + 64));
        const __m256i block3 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset + 96));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset), block0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset + 32), block1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset + 64), block2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset + 96), block3);
    }
    for (; offset + 32 <= bytes; offset += 32) {
        const __m256i block = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset), block);
    }
    for (; offset < bytes; ++offset) destination[offset] = source[offset];
}

__m128i LoadPixel(const std::uint8_t* const source) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return _mm_cvtsi32_si128(static_cast<int>(value));
}

void StorePixel(std::uint8_t* const destination, const __m128i value) noexcept {
    const std::uint32_t pixel = static_cast<std::uint32_t>(_mm_cvtsi128_si32(value));
    std::memcpy(destination, &pixel, sizeof(pixel));
}

void AddSub(std::uint8_t* const destination, const std::uint8_t* const source,
            const std::size_t bytes) noexcept {
    __m128i previous = _mm_setzero_si128();
    std::size_t offset = 0;
    for (; offset + 16 <= bytes; offset += 16) {
        __m128i decoded = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(source + offset));
        decoded = _mm_add_epi8(decoded, _mm_slli_si128(decoded, 4));
        decoded = _mm_add_epi8(decoded, _mm_slli_si128(decoded, 8));
        decoded = _mm_add_epi8(decoded, _mm_shuffle_epi32(previous, 0xFF));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + offset), decoded);
        previous = decoded;
    }
    std::uint32_t prior_pixel = static_cast<std::uint32_t>(
        _mm_extract_epi32(previous, 3));
    for (; offset < bytes; offset += 4) {
        std::uint32_t filtered = 0;
        std::memcpy(&filtered, source + offset, sizeof(filtered));
        const __m128i decoded = _mm_add_epi8(
            _mm_cvtsi32_si128(static_cast<int>(filtered)),
            _mm_cvtsi32_si128(static_cast<int>(prior_pixel)));
        prior_pixel = static_cast<std::uint32_t>(_mm_cvtsi128_si32(decoded));
        std::memcpy(destination + offset, &prior_pixel, sizeof(prior_pixel));
    }
}

void AddAverage(std::uint8_t* const destination, const std::uint8_t* const source,
                const std::uint8_t* const previous, const std::size_t bytes) noexcept {
    const __m128i zero = _mm_setzero_si128();
    const __m128i one = _mm_set1_epi8(1);
    __m128i decoded = zero;
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const __m128i left = decoded;
        const __m128i up = previous ? LoadPixel(previous + offset) : zero;
        __m128i average = _mm_avg_epu8(left, up);
        average = _mm_sub_epi8(average,
                               _mm_and_si128(_mm_xor_si128(left, up), one));
        decoded = _mm_add_epi8(LoadPixel(source + offset), average);
        StorePixel(destination + offset, decoded);
    }
}

void AddPaeth(std::uint8_t* const destination, const std::uint8_t* const source,
              const std::uint8_t* const previous, const std::size_t bytes) noexcept {
    if (!previous) {
        AddSub(destination, source, bytes);
        return;
    }
    const __m128i zero = _mm_setzero_si128();
    __m128i up_left = zero;
    __m128i decoded = zero;
    const auto decode_pixel = [](__m128i filtered, const __m128i left,
                                 const __m128i up,
                                 const __m128i upper_left) noexcept {
        const __m128i up_delta = _mm_sub_epi16(up, upper_left);
        const __m128i left_delta = _mm_sub_epi16(left, upper_left);
        const __m128i distance_left = _mm_abs_epi16(up_delta);
        const __m128i distance_up = _mm_abs_epi16(left_delta);
        const __m128i distance_upper_left = _mm_abs_epi16(
            _mm_add_epi16(up_delta, left_delta));
        const __m128i choose_up = _mm_cmpgt_epi16(distance_left, distance_up);
        const __m128i left_or_up = _mm_blendv_epi8(left, up, choose_up);
        const __m128i nearest_left_or_up = _mm_min_epi16(
            distance_left, distance_up);
        const __m128i choose_upper_left = _mm_cmpgt_epi16(
            nearest_left_or_up, distance_upper_left);
        const __m128i prediction = _mm_blendv_epi8(
            left_or_up, upper_left, choose_upper_left);
        return _mm_add_epi8(filtered, prediction);
    };
    std::size_t offset = 0;
    for (; offset + 16 <= bytes; offset += 16) {
        const __m128i filtered_bytes = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(source + offset));
        const __m128i above_bytes = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(previous + offset));
        const __m128i filtered_01 = _mm_unpacklo_epi8(filtered_bytes, zero);
        const __m128i filtered_23 = _mm_unpackhi_epi8(filtered_bytes, zero);
        const __m128i above_01 = _mm_unpacklo_epi8(above_bytes, zero);
        const __m128i above_23 = _mm_unpackhi_epi8(above_bytes, zero);
        const __m128i filtered_0 = filtered_01;
        const __m128i filtered_1 = _mm_srli_si128(filtered_01, 8);
        const __m128i filtered_2 = filtered_23;
        const __m128i filtered_3 = _mm_srli_si128(filtered_23, 8);
        const __m128i above_0 = above_01;
        const __m128i above_1 = _mm_srli_si128(above_01, 8);
        const __m128i above_2 = above_23;
        const __m128i above_3 = _mm_srli_si128(above_23, 8);

        const __m128i decoded_0 = decode_pixel(
            filtered_0, decoded, above_0, up_left);
        const __m128i decoded_1 = decode_pixel(
            filtered_1, decoded_0, above_1, above_0);
        const __m128i decoded_2 = decode_pixel(
            filtered_2, decoded_1, above_2, above_1);
        const __m128i decoded_3 = decode_pixel(
            filtered_3, decoded_2, above_3, above_2);
        const __m128i decoded_01 = _mm_unpacklo_epi64(decoded_0, decoded_1);
        const __m128i decoded_23 = _mm_unpacklo_epi64(decoded_2, decoded_3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + offset),
                         _mm_packus_epi16(decoded_01, decoded_23));
        decoded = decoded_3;
        up_left = above_3;
    }
    for (; offset < bytes; offset += 4) {
        const __m128i up = _mm_unpacklo_epi8(LoadPixel(previous + offset), zero);
        decoded = decode_pixel(
            _mm_unpacklo_epi8(LoadPixel(source + offset), zero), decoded, up,
            up_left);
        StorePixel(destination + offset, _mm_packus_epi16(decoded, decoded));
        up_left = up;
    }
}

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
        AddPaeth(destination0, source0, previous0, row_bytes);
        AddPaeth(destination1, source1, destination0, row_bytes);
        AddPaeth(destination2, source2, destination1, row_bytes);
        AddPaeth(destination3, source3, destination2, row_bytes);
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
            AddPaeth(destinations[row], sources[row],
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
            AddPaeth(destinations[row], sources[row],
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

void AddUp(std::uint8_t* const destination, const std::uint8_t* const source,
           const std::uint8_t* const previous, const std::size_t bytes) noexcept {
    std::size_t offset = 0;
    for (; offset + 32 <= bytes; offset += 32) {
        const __m256i current = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(source + offset));
        const __m256i above = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(previous + offset));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + offset),
                            _mm256_add_epi8(current, above));
    }
    for (; offset < bytes; ++offset) {
        destination[offset] = static_cast<std::uint8_t>(source[offset] + previous[offset]);
    }
}

template <bool TrackTimings>
bool UnfilterRow(std::uint8_t* const destination,
                 const std::uint8_t* const source,
                 const std::uint8_t* const previous,
                 const std::size_t row_bytes,
                 const std::uint8_t filter,
                 PngDecodeTimings* const timings) noexcept {
    if constexpr (TrackTimings) {
        if (filter < timings->filter_rows.size()) {
            ++timings->filter_rows[filter];
        }
    }
    switch (filter) {
        case 0:
            CopyForward(reinterpret_cast<std::byte*>(destination),
                        reinterpret_cast<const std::byte*>(source), row_bytes);
            return true;
        case 1:
            AddSub(destination, source, row_bytes);
            return true;
        case 2:
            if (previous) {
                AddUp(destination, source, previous, row_bytes);
            } else {
                CopyForward(reinterpret_cast<std::byte*>(destination),
                            reinterpret_cast<const std::byte*>(source), row_bytes);
            }
            return true;
        case 3:
            AddAverage(destination, source, previous, row_bytes);
            return true;
        case 4:
            AddPaeth(destination, source, previous, row_bytes);
            return true;
        default:
            return false;
    }
}

constexpr std::uint32_t kMaxWavefrontHeight = 16384;

template <bool TrackTimings>
class FusedUnfilter final {
public:
    FusedUnfilter(std::byte* const bytes, const std::size_t row_bytes,
                   const std::uint32_t height,
                   PngDecodeTimings* const timings,
                   const std::span<std::uint8_t> scratch) noexcept
        : base_(reinterpret_cast<std::uint8_t*>(bytes)),
          row_bytes_(row_bytes),
          scanline_bytes_(row_bytes + 1),
          height_(height),
          timings_(timings),
          scratch_(scratch) {}

    [[nodiscard]] bool ProcessSafePrefix(const std::size_t safe_output_bytes,
                                         const bool during_deflate) noexcept {
        const auto begin = TrackTimings
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const bool success = ProcessSafePrefixImpl(safe_output_bytes,
                                                   during_deflate);
        if constexpr (TrackTimings) {
            timings_->unfilter_nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin).count());
        }
        return success;
    }

    [[nodiscard]] std::uint32_t RowsProcessed() const noexcept {
        return next_row_;
    }

    [[nodiscard]] bool OwnsOutput(const void* const output) const noexcept {
        return output == base_;
    }

private:
    [[nodiscard]] bool ProcessSafePrefixImpl(
        const std::size_t safe_output_bytes,
        const bool during_deflate) noexcept {
        if (failed_ || safe_output_bytes > scanline_bytes_ * height_) {
            failed_ = true;
            return false;
        }
        const std::uint32_t available_rows = std::min<std::uint32_t>(
            height_, static_cast<std::uint32_t>(
                         safe_output_bytes / scanline_bytes_));
        while (next_row_ < available_rows) {
            const std::size_t source_offset =
                static_cast<std::size_t>(next_row_) * scanline_bytes_;
            const std::uint8_t filter = base_[source_offset];
            if (height_ <= kMaxWavefrontHeight && next_row_ != 0 &&
                next_row_ + 7 < available_rows && filter == 4 &&
                base_[source_offset + scanline_bytes_] == 4 &&
                base_[source_offset + 2 * scanline_bytes_] == 4 &&
                base_[source_offset + 3 * scanline_bytes_] == 4 &&
                base_[source_offset + 4 * scanline_bytes_] == 4 &&
                base_[source_offset + 5 * scanline_bytes_] == 4 &&
                base_[source_offset + 6 * scanline_bytes_] == 4 &&
                base_[source_offset + 7 * scanline_bytes_] == 4) {
                AddPaethRows8(
                    base_ + static_cast<std::size_t>(next_row_) * row_bytes_,
                    base_ + source_offset + 1, row_bytes_, scratch_.data());
                RecordRows(8, during_deflate, 4);
                next_row_ += 8;
                continue;
            }
            if (height_ <= kMaxWavefrontHeight && next_row_ != 0 &&
                next_row_ + 3 < available_rows && filter == 4 &&
                base_[source_offset + scanline_bytes_] == 4 &&
                base_[source_offset + 2 * scanline_bytes_] == 4 &&
                base_[source_offset + 3 * scanline_bytes_] == 4) {
                AddPaethRows4(
                    base_ + static_cast<std::size_t>(next_row_) * row_bytes_,
                    base_ + source_offset + 1, row_bytes_, scratch_.data());
                RecordRows(4, during_deflate, 4);
                next_row_ += 4;
                continue;
            }

            std::uint8_t* const destination =
                base_ + static_cast<std::size_t>(next_row_) * row_bytes_;
            const std::uint8_t* const previous = next_row_ == 0
                ? nullptr
                : destination - row_bytes_;
            if (!UnfilterRow<TrackTimings>(
                    destination, base_ + source_offset + 1, previous,
                    row_bytes_, filter, timings_)) {
                failed_ = true;
                return false;
            }
            RecordRows(1, during_deflate);
            ++next_row_;
        }
        return true;
    }

    void RecordRows(const std::uint32_t count, const bool during_deflate,
                    const int batch_filter = -1) noexcept {
        if constexpr (TrackTimings) {
            if (batch_filter >= 0) {
                timings_->filter_rows[static_cast<std::size_t>(batch_filter)] +=
                    count;
            }
            if (during_deflate) {
                timings_->fused_rows += count;
                timings_->fused_output_bytes +=
                    static_cast<std::uint64_t>(row_bytes_) * count;
            } else {
                timings_->deferred_rows += count;
            }
        }
    }

    std::uint8_t* base_ = nullptr;
    std::size_t row_bytes_ = 0;
    std::size_t scanline_bytes_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t next_row_ = 0;
    PngDecodeTimings* timings_ = nullptr;
    std::span<std::uint8_t> scratch_;
    bool failed_ = false;
};

template <bool TrackTimings>
class ChecksummedUnfilter final {
public:
    ChecksummedUnfilter(std::byte* const bytes, const std::size_t row_bytes,
                        const std::uint32_t height,
                        PngDecodeTimings* const timings,
                        const std::span<std::uint8_t> scratch) noexcept
        : unfilter_(bytes, row_bytes, height, timings, scratch),
          base_(reinterpret_cast<std::uint8_t*>(bytes)),
          scanline_bytes_(row_bytes + 1),
          height_(height) {}

    [[nodiscard]] bool ProcessSafePrefix(const std::size_t safe_output_bytes,
                                         const bool during_deflate) noexcept {
        if (safe_output_bytes > scanline_bytes_ * height_) return false;
        const std::uint32_t available_rows = std::min<std::uint32_t>(
            height_, static_cast<std::uint32_t>(
                         safe_output_bytes / scanline_bytes_));
        constexpr std::uint32_t checksum_batch_rows = 8;
        while (checksum_rows_ < available_rows) {
            const std::uint32_t batch_end = std::min(
                available_rows, checksum_rows_ + checksum_batch_rows);
            const std::size_t checksum_end =
                static_cast<std::size_t>(batch_end) * scanline_bytes_;
            adler32_ = libdeflate_adler32(
                adler32_, base_ + checksum_bytes_,
                checksum_end - checksum_bytes_);
            checksum_bytes_ = checksum_end;
            checksum_rows_ = batch_end;
            if (!unfilter_.ProcessSafePrefix(checksum_end, during_deflate)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint32_t RowsProcessed() const noexcept {
        return unfilter_.RowsProcessed();
    }

    [[nodiscard]] bool OwnsOutput(const void* const output) const noexcept {
        return unfilter_.OwnsOutput(output);
    }

    [[nodiscard]] std::uint32_t Adler32() const noexcept {
        return adler32_;
    }

private:
    FusedUnfilter<TrackTimings> unfilter_;
    std::uint8_t* base_ = nullptr;
    std::size_t scanline_bytes_ = 0;
    std::size_t checksum_bytes_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t checksum_rows_ = 0;
    std::uint32_t adler32_ = 1;
};

template <bool TrackTimings, typename Unfilter>
int FusedUnfilterCallback(void* const opaque, void* const output,
                          const std::size_t safe_output_bytes,
                          const int final) noexcept {
    auto* const unfilter = static_cast<Unfilter*>(opaque);
    if (!unfilter || !unfilter->OwnsOutput(output)) return 1;
    return unfilter->ProcessSafePrefix(safe_output_bytes, final == 0) ? 0 : 1;
}

struct FusedDecodeResult {
    libdeflate_result status = LIBDEFLATE_BAD_DATA;
    std::uint32_t rows = 0;
    std::uint32_t adler32 = 1;
    std::size_t consumed_bytes = 0;
    std::size_t produced_bytes = 0;
};

template <bool TrackTimings, bool ValidateAdler>
FusedDecodeResult RunFusedDecode(
    libdeflate_decompressor* const decompressor,
    const std::uint8_t* const deflate, const std::size_t deflate_bytes,
    DecodeSurface& surface, const std::size_t filtered_bytes,
    const std::size_t callback_interval, const std::size_t row_bytes,
    PngDecodeTimings* const timings,
    const std::span<std::uint8_t> scratch) noexcept {
    FusedDecodeResult decoded;
    if constexpr (ValidateAdler) {
        ChecksummedUnfilter<TrackTimings> unfilter(
            surface.pixels, row_bytes, surface.height,
            TrackTimings ? timings : nullptr, scratch);
        decoded.status = libdeflate_deflate_decompress_ex_callback(
            decompressor, deflate, deflate_bytes, surface.pixels,
            filtered_bytes, callback_interval,
            FusedUnfilterCallback<TrackTimings,
                                  ChecksummedUnfilter<TrackTimings>>,
            &unfilter, &decoded.consumed_bytes, &decoded.produced_bytes);
        decoded.rows = unfilter.RowsProcessed();
        decoded.adler32 = unfilter.Adler32();
    } else {
        FusedUnfilter<TrackTimings> unfilter(
            surface.pixels, row_bytes, surface.height,
            TrackTimings ? timings : nullptr, scratch);
        decoded.status = libdeflate_deflate_decompress_ex_callback(
            decompressor, deflate, deflate_bytes, surface.pixels,
            filtered_bytes, callback_interval,
            FusedUnfilterCallback<TrackTimings,
                                  FusedUnfilter<TrackTimings>>,
            &unfilter, &decoded.consumed_bytes, &decoded.produced_bytes);
        decoded.rows = unfilter.RowsProcessed();
    }
    return decoded;
}

bool ExpandRowsToPitch(DecodeSurface& surface) noexcept {
    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * 4;
    if (surface.stride < row_bytes ||
        surface.allocation_bytes < static_cast<std::size_t>(surface.stride) *
                                       surface.height) {
        return false;
    }
    if (surface.stride == row_bytes) return true;
    for (std::uint32_t row = surface.height; row-- > 0;) {
        std::memmove(surface.pixels + static_cast<std::size_t>(row) * surface.stride,
                     surface.pixels + static_cast<std::size_t>(row) * row_bytes,
                     row_bytes);
    }
    return true;
}
}  // namespace

HRESULT DecodeRgba8Scanlines(
    const std::span<const std::byte> zlib_stream, DecodeSurface& surface,
    const bool validate_adler32,
    const InputConsumedCallback input_consumed,
    void* const callback_context,
    PngDecodeTimings* const timings) noexcept {
    if (zlib_stream.size() < 6) return WINCODEC_ERR_BADIMAGE;
    const auto* const zlib = reinterpret_cast<const std::uint8_t*>(
        zlib_stream.data());
    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * 4;
    if (row_bytes == std::numeric_limits<std::size_t>::max() ||
        surface.height > std::numeric_limits<std::size_t>::max() /
                             (row_bytes + 1)) {
        return E_OUTOFMEMORY;
    }
    const std::size_t filtered_bytes = (row_bytes + 1) * surface.height;
    if (surface.allocation_bytes < filtered_bytes) {
        return E_OUTOFMEMORY;
    }

    const std::uint16_t header = static_cast<std::uint16_t>((zlib[0] << 8U) | zlib[1]);
    if ((header % 31U) != 0 || (header & 0x0F00U) != 0x0800U ||
        (header & 0x0020U) != 0) {
        return WINCODEC_ERR_BADIMAGE;
    }
    struct DecompressorDeleter {
        void operator()(libdeflate_decompressor* const value) const noexcept {
            libdeflate_free_decompressor(value);
        }
    };
    thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter>
        decompressor(libdeflate_alloc_decompressor());
    if (!decompressor) return E_OUTOFMEMORY;
    constexpr std::size_t minimum_callback_interval = 32 * 1024;
    constexpr std::size_t checksum_locality_rows = 64;
    constexpr std::size_t cache_local_batch_bytes = 8 * 1024 * 1024;
    const std::size_t scanline_bytes = row_bytes + 1;
    const std::size_t cache_local_batch =
        scanline_bytes <= cache_local_batch_bytes / 256
            ? scanline_bytes * 256
            : cache_local_batch_bytes;
    constexpr std::size_t wide_scanline_bytes = 16 * 1024;
    constexpr std::size_t wide_batch_bytes = 32 * 1024 * 1024;
    const std::size_t wide_batch = scanline_bytes >= wide_scanline_bytes
        ? std::min(wide_batch_bytes, scanline_bytes * 1024)
        : cache_local_batch;
    const std::size_t callback_interval = std::max(
        scanline_bytes,
        std::max(minimum_callback_interval, wide_batch));
    // Checksum and unfilter strict-mode output while it is cache-local.
    const std::size_t decode_callback_interval = validate_adler32
        ? scanline_bytes * checksum_locality_rows
        : callback_interval;
    const auto deflate_begin = timings ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    thread_local std::vector<std::uint8_t> wavefront_scratch;
    if (surface.height <= kMaxWavefrontHeight) {
        const std::size_t required_scratch =
            static_cast<std::size_t>(surface.height) * 7;
        try {
            wavefront_scratch.resize(required_scratch);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }
    const std::span<std::uint8_t> scratch(wavefront_scratch);
    const FusedDecodeResult decoded = timings
        ? (validate_adler32
               ? RunFusedDecode<true, true>(
                     decompressor.get(), zlib + 2, zlib_stream.size() - 6,
                     surface, filtered_bytes, decode_callback_interval, row_bytes,
                     timings, scratch)
               : RunFusedDecode<true, false>(
                     decompressor.get(), zlib + 2, zlib_stream.size() - 6,
                     surface, filtered_bytes, decode_callback_interval, row_bytes,
                     timings, scratch))
        : (validate_adler32
               ? RunFusedDecode<false, true>(
                     decompressor.get(), zlib + 2, zlib_stream.size() - 6,
                     surface, filtered_bytes, decode_callback_interval, row_bytes,
                     nullptr, scratch)
               : RunFusedDecode<false, false>(
                     decompressor.get(), zlib + 2, zlib_stream.size() - 6,
                     surface, filtered_bytes, decode_callback_interval, row_bytes,
                     nullptr, scratch));
    if (timings) {
        timings->deflate_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - deflate_begin).count());
    }
    const std::uint32_t expected_adler = ReadBigEndian32(
        zlib + zlib_stream.size() - 4);
    if (decoded.status != LIBDEFLATE_SUCCESS ||
        decoded.consumed_bytes != zlib_stream.size() - 6 ||
        decoded.produced_bytes != filtered_bytes ||
        decoded.rows != surface.height ||
        (validate_adler32 && decoded.adler32 != expected_adler)) {
        return WINCODEC_ERR_BADIMAGE;
    }

    if (input_consumed) input_consumed(callback_context);
    return ExpandRowsToPitch(surface) ? S_OK : WINCODEC_ERR_BADIMAGE;
}

}  // namespace pv
