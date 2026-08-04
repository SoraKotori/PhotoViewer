#pragma once

#include "model.h"

#include <array>

namespace pv {

using InputConsumedCallback = void (*)(void*) noexcept;

struct PngDecodeTimings {
    std::uint64_t header_nanoseconds = 0;
    std::uint64_t chunk_scan_nanoseconds = 0;
    std::uint64_t idat_compaction_nanoseconds = 0;
    std::uint64_t deflate_nanoseconds = 0;
    std::uint64_t unfilter_nanoseconds = 0;
    std::array<std::uint32_t, 5> filter_rows{};
};

HRESULT DecodePngSpng(std::span<std::byte> compressed,
                      CpuSurface& surface,
                      InputConsumedCallback input_consumed = nullptr,
                      void* callback_context = nullptr,
                      PngDecodeTimings* timings = nullptr) noexcept;

}  // namespace pv
