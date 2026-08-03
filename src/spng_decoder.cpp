#include "spng_decoder.h"

#define SPNG_STATIC
#include "../third_party/libspng/spng.h"
#include "../third_party/libdeflate/libdeflate.h"

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
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const __m128i up = _mm_unpacklo_epi8(LoadPixel(previous + offset), zero);
        const __m128i left = decoded;
        __m128i filtered = _mm_unpacklo_epi8(LoadPixel(source + offset), zero);
        const __m128i distance_left = _mm_abs_epi16(_mm_sub_epi16(up, up_left));
        const __m128i distance_up = _mm_abs_epi16(_mm_sub_epi16(left, up_left));
        const __m128i distance_upper_left = _mm_abs_epi16(
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

HRESULT DecodeRgba8Fast(const std::span<std::byte> compressed,
                        CpuSurface& surface,
                        const InputConsumedCallback input_consumed,
                        void* const callback_context) noexcept {
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
    if (!found_idat || !found_end || idat_bytes < 6) {
        return WINCODEC_ERR_BADIMAGE;
    }

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

    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * 4;
    const std::size_t filtered_bytes = (row_bytes + 1) * surface.height;
    if (surface.allocation_bytes < filtered_bytes) {
        return E_OUTOFMEMORY;
    }

    const auto* const zlib = reinterpret_cast<const std::uint8_t*>(compressed.data());
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
    const libdeflate_result result = libdeflate_deflate_decompress(
        decompressor.get(), zlib + 2, idat_bytes - 6, surface.pixels,
        filtered_bytes, &actual);
    if (result != LIBDEFLATE_SUCCESS || actual != filtered_bytes) {
        return WINCODEC_ERR_BADIMAGE;
    }

    if (input_consumed) input_consumed(callback_context);
    return Unfilter(surface.pixels, row_bytes, surface.height)
               ? S_OK
               : WINCODEC_ERR_BADIMAGE;
}

}  // namespace

HRESULT DecodePngSpng(const std::span<std::byte> compressed,
                      CpuSurface& surface,
                      const InputConsumedCallback input_consumed,
                      void* const callback_context) noexcept {
    if (compressed.empty() || !surface.pixels || surface.width == 0 ||
        surface.height == 0 || surface.stride != surface.width * 4U) {
        return E_INVALIDARG;
    }

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

    if (result == SPNG_OK && header.bit_depth == 8 &&
        header.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA &&
        header.compression_method == 0 && header.filter_method == 0 &&
        header.interlace_method == SPNG_INTERLACE_NONE) {
        context.reset();
        return DecodeRgba8Fast(compressed, surface, input_consumed,
                               callback_context);
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
    if (result == SPNG_OK) {
        result = spng_decode_image(context.get(), surface.pixels,
                                   surface.ByteSize(), output_format, 0);
    }
    context.reset();
    if (result == SPNG_OK && input_consumed) input_consumed(callback_context);
    return SpngErrorToHresult(result);
}

}  // namespace pv
