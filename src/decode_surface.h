#pragma once

#include <cstddef>
#include <cstdint>

namespace pv {

// Non-owning pixel destination used by codecs. Allocation and GPU ownership
// stay in the caller; decoders only write within this bounded view.
struct DecodeSurface {
    std::byte* pixels = nullptr;
    std::size_t allocation_bytes = 0;
    std::size_t byte_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;

    [[nodiscard]] std::size_t ByteSize() const noexcept { return byte_size; }
};

}  // namespace pv
