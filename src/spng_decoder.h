#pragma once

#include "decode_surface.h"

#include <windows.h>

#include <array>
#include <span>

namespace pv {

using InputConsumedCallback = void (*)(void*) noexcept;

struct PngDecodeTimings {
    std::uint64_t header_nanoseconds = 0;
    std::uint64_t chunk_scan_nanoseconds = 0;
    std::uint64_t idat_compaction_nanoseconds = 0;
    // Includes time spent in cache-local unfilter callbacks.
    std::uint64_t deflate_nanoseconds = 0;
    // Subset of deflate_nanoseconds spent unfiltering callback-safe rows.
    std::uint64_t unfilter_nanoseconds = 0;
    // Rows/bytes handled before the final callback; deferred_rows is the tail.
    std::uint64_t fused_output_bytes = 0;
    std::uint32_t fused_rows = 0;
    std::uint32_t deferred_rows = 0;
    std::array<std::uint32_t, 5> filter_rows{};
};

HRESULT DecodePngSpng(std::span<std::byte> compressed,
                      DecodeSurface& surface,
                      InputConsumedCallback input_consumed = nullptr,
                      void* callback_context = nullptr,
                      PngDecodeTimings* timings = nullptr) noexcept;

}  // namespace pv
