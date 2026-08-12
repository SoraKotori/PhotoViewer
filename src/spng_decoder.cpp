#include "spng_decoder.h"
#include "png_rgba8_decoder.h"

#include <wincodec.h>

#define SPNG_STATIC
#include "../third_party/libspng/spng.h"
#include "../third_party/zlib-ng/zlib.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

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

bool HasFastRgba8Header(const std::span<const std::byte> compressed,
                        std::uint32_t& width,
                        std::uint32_t& height) noexcept {
    constexpr std::array<std::byte, 8> signature{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    if (compressed.size() < 33 ||
        !std::equal(signature.begin(), signature.end(), compressed.begin()) ||
        ReadBigEndian(compressed.data() + 8) != 13 ||
        !IsType(compressed.data() + 12, "IHDR")) {
        return false;
    }
    const auto* const ihdr = reinterpret_cast<const Bytef*>(
        compressed.data() + 12);
    const uLong expected_crc = ReadBigEndian(compressed.data() + 29);
    if (crc32(0, ihdr, 17) != expected_crc) return false;

    width = ReadBigEndian(compressed.data() + 16);
    height = ReadBigEndian(compressed.data() + 20);
    return width != 0 && height != 0 &&
           compressed[24] == std::byte{8} &&
           compressed[25] == std::byte{SPNG_COLOR_TYPE_TRUECOLOR_ALPHA} &&
           compressed[26] == std::byte{0} &&
           compressed[27] == std::byte{0} &&
           compressed[28] == std::byte{SPNG_INTERLACE_NONE};
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
                std::memmove(compressed.data() + write_offset,
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
    return DecodeRgba8Scanlines(
        std::span<const std::byte>(compressed.data() + idat.begin, idat.bytes),
        surface, input_consumed, callback_context, timings);
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
    std::uint32_t fast_width = 0;
    std::uint32_t fast_height = 0;
    if (HasFastRgba8Header(compressed, fast_width, fast_height)) {
        if (fast_width != surface.width || fast_height != surface.height) {
            return E_INVALIDARG;
        }
        if (timings) {
            timings->header_nanoseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - header_begin).count());
        }
        return DecodeRgba8Fast(compressed, surface, input_consumed,
                               callback_context, timings);
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
