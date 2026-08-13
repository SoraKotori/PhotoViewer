#include "png_unfilter.h"

#include "spng_decoder.h"

#include <cstring>
#include <immintrin.h>

namespace pv::png_internal {
namespace {
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

}  // namespace

bool UnfilterRgba8Row(
    std::uint8_t* const destination, const std::uint8_t* const source,
    const std::uint8_t* const previous, const std::size_t row_bytes,
    const std::uint8_t filter) noexcept {
    return UnfilterRow<false>(destination, source, previous, row_bytes,
                              filter, nullptr);
}

bool UnfilterRgba8RowWithTimings(
    std::uint8_t* const destination, const std::uint8_t* const source,
    const std::uint8_t* const previous, const std::size_t row_bytes,
    const std::uint8_t filter, PngDecodeTimings& timings) noexcept {
    return UnfilterRow<true>(destination, source, previous, row_bytes,
                             filter, &timings);
}

void UnfilterPaethRow(
    std::uint8_t* const destination, const std::uint8_t* const source,
    const std::uint8_t* const previous,
    const std::size_t row_bytes) noexcept {
    AddPaeth(destination, source, previous, row_bytes);
}

}  // namespace pv::png_internal
