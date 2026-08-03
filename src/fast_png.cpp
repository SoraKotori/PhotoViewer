#include "fast_png.h"

#include "../third_party/libdeflate/libdeflate.h"

#include <cstdlib>
#include <cstring>
#include <immintrin.h>

namespace pv {
namespace {

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
    __m128i decoded = _mm_setzero_si128();
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        decoded = _mm_add_epi8(LoadPixel(source + offset), decoded);
        StorePixel(destination + offset, decoded);
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
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const __m128i up = _mm_unpacklo_epi8(LoadPixel(previous + offset), zero);
        const __m128i left = decoded;
        __m128i filtered = _mm_unpacklo_epi8(LoadPixel(source + offset), zero);
        __m128i distance_left = _mm_abs_epi16(_mm_sub_epi16(up, up_left));
        __m128i distance_up = _mm_abs_epi16(_mm_sub_epi16(left, up_left));
        __m128i distance_upper_left = _mm_abs_epi16(
            _mm_add_epi16(_mm_sub_epi16(up, up_left),
                          _mm_sub_epi16(left, up_left)));
        const __m128i smallest = _mm_min_epi16(
            distance_upper_left, _mm_min_epi16(distance_left, distance_up));
        const __m128i choose_left = _mm_cmpeq_epi16(smallest, distance_left);
        const __m128i choose_up = _mm_cmpeq_epi16(smallest, distance_up);
        const __m128i up_or_upper_left = _mm_blendv_epi8(up_left, up, choose_up);
        const __m128i prediction = _mm_blendv_epi8(
            up_or_upper_left, left, choose_left);
        filtered = _mm_add_epi8(filtered, prediction);
        decoded = filtered;
        StorePixel(destination + offset, _mm_packus_epi16(filtered, filtered));
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

bool Unfilter(std::byte* const bytes, const std::size_t row_bytes,
              const std::uint32_t height) noexcept {
    auto* const base = reinterpret_cast<std::uint8_t*>(bytes);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::size_t source_offset = static_cast<std::size_t>(row) * (row_bytes + 1);
        const std::uint8_t filter = base[source_offset];
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
    }
    return true;
}

}  // namespace

HRESULT DecodePngFast(const std::span<std::byte> compressed,
                      CpuSurface& surface,
                      const InputConsumedCallback input_consumed,
                      void* const callback_context) noexcept {
    constexpr std::byte signature[] = {
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    if (compressed.size() < 33 ||
        !std::equal(std::begin(signature), std::end(signature), compressed.begin()) ||
        ReadBigEndian(compressed.data() + 8) != 13 ||
        !IsType(compressed.data() + 12, "IHDR") || compressed[24] != std::byte{8} ||
        compressed[25] != std::byte{6} || compressed[26] != std::byte{0} ||
        compressed[27] != std::byte{0} || compressed[28] != std::byte{0}) {
        return WINCODEC_ERR_COMPONENTNOTFOUND;
    }
    const std::uint32_t width = ReadBigEndian(compressed.data() + 16);
    const std::uint32_t height = ReadBigEndian(compressed.data() + 20);
    if (width != surface.width || height != surface.height || !surface.pixels) {
        return E_INVALIDARG;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    const std::size_t filtered_bytes = (row_bytes + 1) * height;
    if (surface.allocation_bytes < filtered_bytes) return WINCODEC_ERR_COMPONENTNOTFOUND;

    std::size_t idat_bytes = 0;
    bool found_idat = false;
    bool found_end = false;
    for (std::size_t offset = 8; offset + 12 <= compressed.size();) {
        const std::uint32_t length = ReadBigEndian(compressed.data() + offset);
        if (length > compressed.size() - offset - 12) return WINCODEC_ERR_BADIMAGE;
        const std::byte* const type = compressed.data() + offset + 4;
        if (IsType(type, "IDAT")) {
            found_idat = true;
            if (idat_bytes > std::numeric_limits<std::size_t>::max() - length) {
                return E_OUTOFMEMORY;
            }
            idat_bytes += length;
        } else if (IsType(type, "IEND")) {
            found_end = true;
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    if (!found_idat || !found_end || idat_bytes < 6) return WINCODEC_ERR_BADIMAGE;

    std::size_t write_offset = 0;
    for (std::size_t offset = 8; offset + 12 <= compressed.size();) {
        const std::uint32_t length = ReadBigEndian(compressed.data() + offset);
        const std::byte* const type = compressed.data() + offset + 4;
        if (IsType(type, "IDAT")) {
            std::memmove(compressed.data() + write_offset,
                         compressed.data() + offset + 8, length);
            write_offset += length;
        } else if (IsType(type, "IEND")) {
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }

    const auto* const zlib = reinterpret_cast<const std::uint8_t*>(compressed.data());
    const std::uint16_t header = static_cast<std::uint16_t>((zlib[0] << 8U) | zlib[1]);
    if ((header % 31U) != 0 || (header & 0x0F00U) != 0x0800U ||
        (header & 0x0020U) != 0) {
        return WINCODEC_ERR_BADIMAGE;
    }
    struct DecompressorDeleter {
        void operator()(libdeflate_decompressor* value) const noexcept {
            libdeflate_free_decompressor(value);
        }
    };
    thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter>
        decompressor(libdeflate_alloc_decompressor());
    if (!decompressor) return E_OUTOFMEMORY;
    std::size_t actual = 0;
    const libdeflate_result result = libdeflate_deflate_decompress(
        decompressor.get(), zlib + 2, idat_bytes - 6, surface.pixels,
        filtered_bytes, &actual);
    if (result != LIBDEFLATE_SUCCESS || actual != filtered_bytes) {
        return WINCODEC_ERR_BADIMAGE;
    }
    if (input_consumed) input_consumed(callback_context);
    return Unfilter(surface.pixels, row_bytes, height) ? S_OK : WINCODEC_ERR_BADIMAGE;
}

}  // namespace pv
