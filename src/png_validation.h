#pragma once

#include <cstdint>

namespace pv {

enum class PngChunkCrcMode : std::uint8_t {
    All,
    Critical,
    NonIdat,
    None,
};

struct PngValidationOptions {
    PngChunkCrcMode chunk_crc = PngChunkCrcMode::All;
    bool adler32 = true;
};

}  // namespace pv
