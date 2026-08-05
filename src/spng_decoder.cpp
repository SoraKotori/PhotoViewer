#include "spng_decoder.h"

#define SPNG_STATIC
#include "../third_party/libspng/spng.h"
#include "../third_party/libdeflate/libdeflate.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>

namespace pv {
namespace {

struct SpngContextDeleter {
    void operator()(spng_ctx* const context) const noexcept {
        spng_ctx_free(context);
    }
};

HRESULT SpngErrorToHresult(const int error) noexcept {
    if (error == SPNG_OK) return S_OK;
    if (error == SPNG_EMEM || error == SPNG_EOVERFLOW) return E_OUTOFMEMORY;
    if (error == SPNG_EINVAL || error == SPNG_EBUFSIZ) return E_INVALIDARG;
    return WINCODEC_ERR_BADIMAGE;
}

std::uint32_t ReadBigEndian(const std::byte* const data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

bool IsType(const std::byte* const type, const char* const expected) noexcept {
    return type[0] == std::byte{static_cast<unsigned char>(expected[0])} &&
           type[1] == std::byte{static_cast<unsigned char>(expected[1])} &&
           type[2] == std::byte{static_cast<unsigned char>(expected[2])} &&
           type[3] == std::byte{static_cast<unsigned char>(expected[3])};
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
    for (std::size_t pixel = 0; pixel + 3 < pixels; ++pixel) {
        const __m256i filtered = _mm256_cvtepu8_epi16(_mm_setr_epi32(
            static_cast<int>(LoadFilteredPixel(
                source0, row_bytes, tail0, preserved0, pixel + 3)),
            static_cast<int>(LoadFilteredPixel(
                source1, row_bytes, tail1, preserved1, pixel + 2)),
            static_cast<int>(LoadFilteredPixel(
                source2, row_bytes, tail2, preserved2, pixel + 1)),
            static_cast<int>(LoadPixelValue(source3 + pixel * 4))));
        const __m256i up = _mm256_cvtepu8_epi16(_mm_setr_epi32(
            static_cast<int>(LoadPixelValue(previous0 + (pixel + 3) * 4)),
            static_cast<int>(LoadPixelValue(destination0 + (pixel + 2) * 4)),
            static_cast<int>(LoadPixelValue(destination1 + (pixel + 1) * 4)),
            static_cast<int>(LoadPixelValue(destination2 + pixel * 4))));
        const __m256i upper_left = _mm256_cvtepu8_epi16(_mm_setr_epi32(
            static_cast<int>(LoadPixelValue(previous0 + (pixel + 2) * 4)),
            static_cast<int>(LoadPixelValue(destination0 + (pixel + 1) * 4)),
            static_cast<int>(LoadPixelValue(destination1 + pixel * 4)),
            pixel == 0 ? 0 : static_cast<int>(
                LoadPixelValue(destination2 + (pixel - 1) * 4))));
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
        StorePixelValue(destination0 + (pixel + 3) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 0)));
        StorePixelValue(destination1 + (pixel + 2) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 1)));
        StorePixelValue(destination2 + (pixel + 1) * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 2)));
        StorePixelValue(destination3 + pixel * 4,
                        static_cast<std::uint32_t>(_mm_extract_epi32(values, 3)));
        left = decoded;
    }

    DecodePaethPixelValue(destination1, destination0, pixels - 1,
                          LoadFilteredPixel(source1, row_bytes, tail1,
                                            preserved1, pixels - 1));
    for (std::size_t pixel = pixels - 2; pixel < pixels; ++pixel) {
        DecodePaethPixelValue(destination2, destination1, pixel,
                              LoadFilteredPixel(source2, row_bytes, tail2,
                                                preserved2, pixel));
    }
    for (std::size_t pixel = pixels - 3; pixel < pixels; ++pixel) {
        DecodePaethPixel(destination3, source3, destination2, pixel);
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

bool Unfilter(std::byte* const bytes, const std::size_t row_bytes,
              const std::uint32_t height, PngDecodeTimings* const timings) noexcept {
    constexpr std::uint32_t max_wavefront_height = 16384;
    std::array<std::uint8_t, max_wavefront_height * 3> scratch;
    auto* const base = reinterpret_cast<std::uint8_t*>(bytes);
    for (std::uint32_t row = 0; row < height;) {
        const std::size_t source_offset = static_cast<std::size_t>(row) * (row_bytes + 1);
        const std::uint8_t filter = base[source_offset];
        if (height <= max_wavefront_height && row != 0 && row + 3 < height &&
            filter == 4 &&
            base[source_offset + row_bytes + 1] == 4 &&
            base[source_offset + 2 * (row_bytes + 1)] == 4 &&
            base[source_offset + 3 * (row_bytes + 1)] == 4) {
            AddPaethRows4(base + static_cast<std::size_t>(row) * row_bytes,
                          base + source_offset + 1, row_bytes, scratch.data());
            if (timings) timings->filter_rows[4] += 4;
            row += 4;
            continue;
        }
        if (timings && filter < timings->filter_rows.size()) {
            ++timings->filter_rows[filter];
        }
        const std::uint8_t* const source = base + source_offset + 1;
        std::uint8_t* const destination = base + static_cast<std::size_t>(row) * row_bytes;
        const std::uint8_t* const previous = row == 0 ? nullptr : destination - row_bytes;
        switch (filter) {
            case 0:
                std::memmove(destination, source, row_bytes);
                break;
            case 1:
                AddSub(destination, source, row_bytes);
                break;
            case 2:
                if (previous) AddUp(destination, source, previous, row_bytes);
                else std::memmove(destination, source, row_bytes);
                break;
            case 3:
                AddAverage(destination, source, previous, row_bytes);
                break;
            case 4:
                AddPaeth(destination, source, previous, row_bytes);
                break;
            default:
                return false;
        }
        ++row;
    }
    return true;
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

struct IdatData {
    std::size_t begin = 0;
    std::size_t bytes = 0;
};

HRESULT CompactIdat(const std::span<std::byte> compressed, IdatData& idat,
                    PngDecodeTimings* const timings) noexcept {
    const auto begin = timings ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    std::size_t write_offset = 0;
    bool found = false;
    bool found_end = false;
    for (std::size_t offset = 8; offset + 12 <= compressed.size();) {
        const std::uint32_t length = ReadBigEndian(compressed.data() + offset);
        if (length > compressed.size() - offset - 12) return WINCODEC_ERR_BADIMAGE;
        const std::byte* const type = compressed.data() + offset + 4;
        if (IsType(type, "IDAT")) {
            if (idat.bytes > std::numeric_limits<std::size_t>::max() - length) {
                return E_OUTOFMEMORY;
            }
            const std::size_t payload_offset = offset + 8;
            if (!found) {
                found = true;
                idat.begin = payload_offset;
                write_offset = payload_offset;
            }
            if (write_offset != payload_offset) {
                CopyForward(compressed.data() + write_offset,
                            compressed.data() + payload_offset, length);
            }
            write_offset += length;
            idat.bytes += length;
        } else if (IsType(type, "IEND")) {
            found_end = true;
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    if (!found || !found_end || idat.bytes < 6) return WINCODEC_ERR_BADIMAGE;
    if (timings) {
        timings->chunk_scan_nanoseconds = 0;
        timings->idat_compaction_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    }
    return S_OK;
}

HRESULT DecodeRgba8Fast(const std::span<std::byte> compressed,
                        DecodeSurface& surface,
                        const InputConsumedCallback input_consumed,
                        void* const callback_context,
                        PngDecodeTimings* const timings) noexcept {
    IdatData idat;
    const HRESULT compacted = CompactIdat(compressed, idat, timings);
    if (FAILED(compacted)) return compacted;

    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * 4;
    const std::size_t filtered_bytes = (row_bytes + 1) * surface.height;
    if (surface.allocation_bytes < filtered_bytes) {
        return E_OUTOFMEMORY;
    }

    const auto* const zlib = reinterpret_cast<const std::uint8_t*>(
        compressed.data() + idat.begin);
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
    std::size_t actual = 0;
    const auto deflate_begin = timings ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    const libdeflate_result result = libdeflate_deflate_decompress(
        decompressor.get(), zlib + 2, idat.bytes - 6, surface.pixels,
        filtered_bytes, &actual);
    if (timings) {
        timings->deflate_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - deflate_begin).count());
    }
    if (result != LIBDEFLATE_SUCCESS || actual != filtered_bytes) {
        return WINCODEC_ERR_BADIMAGE;
    }

    if (input_consumed) input_consumed(callback_context);
    const auto unfilter_begin = timings ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    const bool unfiltered = Unfilter(surface.pixels, row_bytes, surface.height,
                                     timings);
    if (timings) {
        timings->unfilter_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - unfilter_begin).count());
    }
    return unfiltered && ExpandRowsToPitch(surface)
               ? S_OK
               : WINCODEC_ERR_BADIMAGE;
}

}  // namespace

HRESULT DecodePngSpng(const std::span<std::byte> compressed,
                      DecodeSurface& surface,
                      const InputConsumedCallback input_consumed,
                      void* const callback_context,
                      PngDecodeTimings* const timings) noexcept {
    if (compressed.empty() || !surface.pixels || surface.width == 0 ||
        surface.height == 0 || surface.stride < surface.width * 4U) {
        return E_INVALIDARG;
    }

    const auto header_begin = timings ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    std::unique_ptr<spng_ctx, SpngContextDeleter> context(
        spng_ctx_new(SPNG_CTX_IGNORE_ADLER32));
    if (!context) return E_OUTOFMEMORY;

    int result = spng_set_crc_action(context.get(), SPNG_CRC_USE, SPNG_CRC_USE);
    if (result == SPNG_OK) {
        result = spng_set_png_buffer(context.get(), compressed.data(), compressed.size());
    }

    spng_ihdr header{};
    if (result == SPNG_OK) result = spng_get_ihdr(context.get(), &header);
    if (result == SPNG_OK &&
        (header.width != surface.width || header.height != surface.height)) {
        return E_INVALIDARG;
    }
    if (timings) {
        timings->header_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - header_begin).count());
    }

    if (result == SPNG_OK && header.bit_depth == 8 &&
        header.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA &&
        header.compression_method == 0 && header.filter_method == 0 &&
        header.interlace_method == SPNG_INTERLACE_NONE) {
        context.reset();
        return DecodeRgba8Fast(compressed, surface, input_consumed,
                               callback_context, timings);
    }

    const int output_format =
        header.bit_depth == 8 &&
                header.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA
            ? SPNG_FMT_PNG
            : SPNG_FMT_RGBA8;
    std::size_t decoded_bytes = 0;
    if (result == SPNG_OK) {
        result = spng_decoded_image_size(context.get(), output_format,
                                         &decoded_bytes);
    }
    if (result == SPNG_OK && decoded_bytes != surface.ByteSize()) {
        return E_INVALIDARG;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * 4;
    if (result == SPNG_OK && surface.stride != row_bytes) {
        result = spng_decode_image(context.get(), nullptr, 0, output_format,
                                   SPNG_DECODE_PROGRESSIVE);
        while (result == SPNG_OK) {
            spng_row_info row{};
            result = spng_get_row_info(context.get(), &row);
            if (result != SPNG_OK) break;
            if (row.row_num >= surface.height) {
                result = SPNG_EOVERFLOW;
                break;
            }
            result = spng_decode_row(
                context.get(),
                surface.pixels + static_cast<std::size_t>(row.row_num) *
                                     surface.stride,
                row_bytes);
        }
        if (result == SPNG_EOI) result = SPNG_OK;
    } else if (result == SPNG_OK) {
        result = spng_decode_image(context.get(), surface.pixels,
                                   surface.ByteSize(), output_format, 0);
    }
    context.reset();
    if (result == SPNG_OK && input_consumed) input_consumed(callback_context);
    return SpngErrorToHresult(result);
}

}  // namespace pv
