#pragma once

#include "spng_decoder.h"

namespace pv {

HRESULT DecodeRgba8Scanlines(
    std::span<const std::byte> zlib_stream, DecodeSurface& surface,
    InputConsumedCallback input_consumed, void* callback_context,
    PngDecodeTimings* timings) noexcept;

}  // namespace pv
