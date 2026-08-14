#include "png_rgba8_decoder.h"

#include "png_unfilter.h"

#include <wincodec.h>

#include "../third_party/libdeflate/libdeflate.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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
                png_internal::UnfilterPaethRows8(
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
                png_internal::UnfilterPaethRows4(
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
            bool unfiltered = false;
            if constexpr (TrackTimings) {
                unfiltered = png_internal::UnfilterRgba8RowWithTimings(
                    destination, base_ + source_offset + 1, previous,
                    row_bytes_, filter, *timings_);
            } else {
                unfiltered = png_internal::UnfilterRgba8Row(
                    destination, base_ + source_offset + 1, previous,
                    row_bytes_, filter);
            }
            if (!unfiltered) {
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
