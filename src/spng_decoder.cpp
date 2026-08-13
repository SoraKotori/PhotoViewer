#include "spng_decoder.h"
#include "png_rgba8_decoder.h"

#include <wincodec.h>

#include "../third_party/libdeflate/libdeflate.h"

#define SPNG_STATIC
#include "../third_party/libspng/spng.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <memory>
#include <span>

namespace pv {
namespace {

constexpr std::array<std::byte, 8> kPngSignature{
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
constexpr std::uint32_t kIdatTypeCrc = 0x35AF061EU;

struct SpngContextDeleter {
    void operator()(spng_ctx* const context) const noexcept {
        spng_ctx_free(context);
    }
};

struct IdatData {
    std::size_t begin = 0;
    std::size_t bytes = 0;
};

HRESULT SpngErrorToHresult(const int error) noexcept {
    if (error == SPNG_OK) return S_OK;
    if (error == SPNG_EMEM || error == SPNG_EOVERFLOW) return E_OUTOFMEMORY;
    if (error == SPNG_EINVAL || error == SPNG_EBUFSIZ) return E_INVALIDARG;
    return WINCODEC_ERR_BADIMAGE;
}

std::uint32_t ReadBigEndian(const std::byte* const data) noexcept {
    std::uint32_t native = 0;
    std::memcpy(&native, data, sizeof(native));
    return _byteswap_ulong(native);
}

constexpr std::uint32_t NativeChunkType(const char first, const char second,
                                        const char third,
                                        const char fourth) noexcept {
    return static_cast<std::uint8_t>(first) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(second)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(third)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(fourth)) << 24U);
}

constexpr std::uint32_t kIhdrChunk = NativeChunkType('I', 'H', 'D', 'R');
constexpr std::uint32_t kPlteChunk = NativeChunkType('P', 'L', 'T', 'E');
constexpr std::uint32_t kIdatChunk = NativeChunkType('I', 'D', 'A', 'T');
constexpr std::uint32_t kIendChunk = NativeChunkType('I', 'E', 'N', 'D');

enum class ChunkKind : std::uint8_t {
    Unknown,
    Ihdr,
    Plte,
    Idat,
    Iend,
};

ChunkKind ClassifyChunk(const std::byte* const type) noexcept {
    std::uint32_t native = 0;
    std::memcpy(&native, type, sizeof(native));
    if (native == kIdatChunk) return ChunkKind::Idat;
    switch (native) {
        case kIhdrChunk:
            return ChunkKind::Ihdr;
        case kPlteChunk:
            return ChunkKind::Plte;
        case kIendChunk:
            return ChunkKind::Iend;
        default:
            return ChunkKind::Unknown;
    }
}

bool IsType(const std::byte* const type, const char* const expected) noexcept {
    return type[0] == std::byte{static_cast<unsigned char>(expected[0])} &&
           type[1] == std::byte{static_cast<unsigned char>(expected[1])} &&
           type[2] == std::byte{static_cast<unsigned char>(expected[2])} &&
           type[3] == std::byte{static_cast<unsigned char>(expected[3])};
}

bool IsCritical(const std::byte* const type) noexcept {
    return (std::to_integer<std::uint8_t>(type[0]) & 0x20U) == 0;
}

bool IsValidChunkType(const std::byte* const type) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(type[index]);
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z'))) {
            return false;
        }
    }
    return (std::to_integer<std::uint8_t>(type[2]) & 0x20U) == 0;
}

template <PngChunkCrcMode Mode>
bool ValidateChunkCrc(const ChunkKind kind,
                      const std::byte* const type) noexcept {
    if constexpr (Mode == PngChunkCrcMode::All) {
        return true;
    } else if constexpr (Mode == PngChunkCrcMode::Critical) {
        return IsCritical(type);
    } else if constexpr (Mode == PngChunkCrcMode::NonIdat) {
        return kind != ChunkKind::Idat;
    } else {
        return false;
    }
}

bool HeaderMatchesPlan(const std::span<const std::byte> compressed,
                       const PngResourcePlan& expected) noexcept {
    return compressed.size() >= kPngHeaderBytes &&
           std::equal(kPngSignature.begin(), kPngSignature.end(),
                      compressed.begin()) &&
           ReadBigEndian(compressed.data() + 8) == 13 &&
           IsType(compressed.data() + 12, "IHDR") &&
           ReadBigEndian(compressed.data() + 16) == expected.width &&
           ReadBigEndian(compressed.data() + 20) == expected.height &&
           std::to_integer<std::uint8_t>(compressed[24]) ==
               expected.bit_depth &&
           std::to_integer<std::uint8_t>(compressed[25]) ==
               expected.color_type &&
           compressed[26] == std::byte{0} && compressed[27] == std::byte{0} &&
           std::to_integer<std::uint8_t>(compressed[28]) ==
               expected.interlace_method;
}

template <PngChunkCrcMode CrcMode, bool TrackTimings, bool CompactIdat>
HRESULT ScanChunksImpl(const std::span<std::byte> compressed,
                       const PngResourcePlan& expected,
                       IdatData* const compacted,
                       PngDecodeTimings* const timings) noexcept {
    const auto scan_begin = TrackTimings
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    std::uint64_t compaction_nanoseconds = 0;
    std::size_t write_offset = 0;
    bool found_idat = false;
    bool found_plte = false;
    bool idat_ended = false;
    bool found_iend = false;
    const bool requires_palette =
        expected.color_type == SPNG_COLOR_TYPE_INDEXED;

    if (compressed.size() < kPngHeaderBytes ||
        libdeflate_crc32(0, compressed.data() + 12, 17) !=
            ReadBigEndian(compressed.data() + 29)) {
        return WINCODEC_ERR_BADIMAGE;
    }

    for (std::size_t offset = kPngHeaderBytes;
         offset + 12 <= compressed.size();) {
        const std::uint32_t length = ReadBigEndian(compressed.data() + offset);
        if (length > compressed.size() - offset - 12) {
            return WINCODEC_ERR_BADIMAGE;
        }
        const std::byte* const type = compressed.data() + offset + 4;
        const ChunkKind kind = ClassifyChunk(type);
        const std::size_t payload_offset = offset + 8;
        if (kind == ChunkKind::Idat) {
            if constexpr (CrcMode == PngChunkCrcMode::All ||
                          CrcMode == PngChunkCrcMode::Critical) {
                if (libdeflate_crc32(kIdatTypeCrc, type + 4, length) !=
                    ReadBigEndian(compressed.data() + offset + 8 + length)) {
                    return WINCODEC_ERR_BADIMAGE;
                }
            }
            if (idat_ended ||
                (!CompactIdat && requires_palette && !found_plte)) {
                return WINCODEC_ERR_BADIMAGE;
            }
            if (!found_idat) {
                found_idat = true;
                if constexpr (CompactIdat) {
                    compacted->begin = payload_offset;
                    write_offset = payload_offset;
                }
            }
            if constexpr (CompactIdat) {
                const auto move_begin = TrackTimings
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                if (write_offset != payload_offset) {
                    std::memmove(compressed.data() + write_offset,
                                 compressed.data() + payload_offset,
                                 length);
                }
                if constexpr (TrackTimings) {
                    compaction_nanoseconds += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - move_begin)
                            .count());
                }
                write_offset += length;
                compacted->bytes += length;
            }
            offset += static_cast<std::size_t>(length) + 12;
            continue;
        }

        if (kind == ChunkKind::Unknown && !IsValidChunkType(type)) {
            return WINCODEC_ERR_BADIMAGE;
        }
        if (kind == ChunkKind::Ihdr) return WINCODEC_ERR_BADIMAGE;
        if (ValidateChunkCrc<CrcMode>(kind, type) &&
            libdeflate_crc32(0, type,
                             static_cast<std::size_t>(length) + 4) !=
                ReadBigEndian(compressed.data() + offset + 8 + length)) {
            return WINCODEC_ERR_BADIMAGE;
        }
        if (found_idat && kind != ChunkKind::Iend) idat_ended = true;
        if (kind == ChunkKind::Plte) {
            if (found_plte || found_idat || length == 0 ||
                length > 256 * 3 || (length % 3) != 0 ||
                expected.color_type == SPNG_COLOR_TYPE_GRAYSCALE ||
                expected.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA) {
                return WINCODEC_ERR_BADIMAGE;
            }
            found_plte = true;
        }
        if (kind == ChunkKind::Unknown && IsCritical(type)) {
            return WINCODEC_ERR_BADIMAGE;
        }
        if (kind == ChunkKind::Iend) {
            if (length != 0 || !found_idat ||
                offset + 12 != compressed.size()) {
                return WINCODEC_ERR_BADIMAGE;
            }
            found_iend = true;
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }

    if (!found_idat || !found_iend ||
        (CompactIdat && compacted->bytes < 6)) {
        return WINCODEC_ERR_BADIMAGE;
    }
    if constexpr (TrackTimings) {
        const std::uint64_t elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - scan_begin)
                .count());
        timings->idat_compaction_nanoseconds = compaction_nanoseconds;
        timings->chunk_scan_nanoseconds = elapsed - compaction_nanoseconds;
    }
    return S_OK;
}

template <bool TrackTimings, bool CompactIdat>
HRESULT ScanChunksWithModeAndCompaction(
    const std::span<std::byte> compressed, const PngResourcePlan& expected,
    const PngChunkCrcMode crc_mode, IdatData* const compacted,
    PngDecodeTimings* const timings) noexcept {
    switch (crc_mode) {
        case PngChunkCrcMode::All:
            return ScanChunksImpl<PngChunkCrcMode::All, TrackTimings,
                                  CompactIdat>(
                compressed, expected, compacted, timings);
        case PngChunkCrcMode::Critical:
            return ScanChunksImpl<PngChunkCrcMode::Critical, TrackTimings,
                                  CompactIdat>(
                compressed, expected, compacted, timings);
        case PngChunkCrcMode::NonIdat:
            return ScanChunksImpl<PngChunkCrcMode::NonIdat, TrackTimings,
                                  CompactIdat>(
                compressed, expected, compacted, timings);
        case PngChunkCrcMode::None:
            return ScanChunksImpl<PngChunkCrcMode::None, TrackTimings,
                                  CompactIdat>(
                compressed, expected, compacted, timings);
    }
    return E_INVALIDARG;
}

template <bool TrackTimings>
HRESULT ScanChunksWithMode(const std::span<std::byte> compressed,
                           const PngResourcePlan& expected,
                           const PngChunkCrcMode crc_mode,
                           IdatData* const compacted,
                           PngDecodeTimings* const timings) noexcept {
    return compacted
        ? ScanChunksWithModeAndCompaction<TrackTimings, true>(
              compressed, expected, crc_mode, compacted, timings)
        : ScanChunksWithModeAndCompaction<TrackTimings, false>(
              compressed, expected, crc_mode, nullptr, timings);
}

HRESULT ScanChunks(const std::span<std::byte> compressed,
                   const PngResourcePlan& expected,
                   const PngChunkCrcMode crc_mode, IdatData* const compacted,
                   PngDecodeTimings* const timings) noexcept {
    return timings
        ? ScanChunksWithMode<true>(compressed, expected, crc_mode, compacted,
                                   timings)
        : ScanChunksWithMode<false>(compressed, expected, crc_mode, compacted,
                                    nullptr);
}

}  // namespace

HRESULT DecodePngSpng(const std::span<std::byte> compressed,
                      DecodeSurface& surface,
                      const PngResourcePlan& expected,
                      const PngValidationOptions validation,
                      const InputConsumedCallback input_consumed,
                      void* const callback_context,
                      PngDecodeTimings* const timings) noexcept {
    if (compressed.empty() || !surface.pixels || expected.width == 0 ||
        expected.height == 0 || surface.width != expected.width ||
        surface.height != expected.height ||
        surface.stride < expected.row_bytes ||
        surface.ByteSize() != expected.decoded_bytes ||
        !HeaderMatchesPlan(compressed, expected)) {
        return E_INVALIDARG;
    }

    const auto header_begin = timings ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const bool fast_rgba8 = expected.bit_depth == 8 &&
                            expected.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA &&
                            expected.interlace_method == SPNG_INTERLACE_NONE;
    if (timings) {
        timings->header_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - header_begin)
                .count());
    }

    IdatData idat;
    const HRESULT scanned = ScanChunks(
        compressed, expected, validation.chunk_crc,
        fast_rgba8 ? &idat : nullptr, timings);
    if (FAILED(scanned)) return scanned;

    if (fast_rgba8) {
        return DecodeRgba8Scanlines(
            std::span<const std::byte>(compressed.data() + idat.begin,
                                      idat.bytes),
            surface, validation.adler32, input_consumed, callback_context,
            timings);
    }

    int flags = SPNG_CTX_SKIP_CHUNK_CRC;
    if (!validation.adler32) flags |= SPNG_CTX_IGNORE_ADLER32;
    std::unique_ptr<spng_ctx, SpngContextDeleter> context(spng_ctx_new(flags));
    if (!context) return E_OUTOFMEMORY;

    int result = spng_set_png_buffer(context.get(), compressed.data(),
                                     compressed.size());
    spng_ihdr header{};
    if (result == SPNG_OK) result = spng_get_ihdr(context.get(), &header);
    if (result == SPNG_OK &&
        (header.width != expected.width || header.height != expected.height ||
         header.bit_depth != expected.bit_depth ||
         header.color_type != expected.color_type ||
         header.compression_method != 0 || header.filter_method != 0 ||
         header.interlace_method != expected.interlace_method)) {
        return E_INVALIDARG;
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
    if (result == SPNG_OK && decoded_bytes != expected.decoded_bytes) {
        return E_INVALIDARG;
    }
    if (result == SPNG_OK && surface.stride != expected.row_bytes) {
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
                expected.row_bytes);
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
