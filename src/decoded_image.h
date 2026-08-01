#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct DecodedImage
{
    std::filesystem::path path;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
    std::uint64_t decodeMicroseconds{};
    std::uint64_t setupMicroseconds{};
    std::uint64_t copyMicroseconds{};
};
