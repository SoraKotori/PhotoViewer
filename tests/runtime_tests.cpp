#include "win32_support.h"
#include "decode_stage.h"
#include "graphics.h"
#include "pipeline_runtime.h"
#include "png_unfilter.h"
#include "spng_decoder.h"
#define SPNG_STATIC
#include "../third_party/libdeflate/libdeflate.h"
#include "../third_party/libspng/spng.h"
#include "../third_party/zlib-ng/zlib.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_nothrow_destructible_v<pv::AsyncCatalog>);
static_assert(std::is_nothrow_destructible_v<pv::StoragePipeline>);
static_assert(std::is_nothrow_destructible_v<pv::PipelineRuntime>);

void Check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pv::PngResourcePlan ParsePlan(const std::span<const std::byte> png) {
    const auto plan = pv::ParsePngResourcePlan(png);
    Check(plan.has_value(), "test PNG must have a valid resource header");
    return *plan;
}

pv::PngResourcePlan TestRgba8Plan(const std::uint32_t width,
                                  const std::uint32_t height) {
    const std::uint32_t row_bytes = width * 4;
    const std::uint32_t decoded_bytes = row_bytes * height;
    const std::uint32_t workspace = row_bytes + height;
    const std::uint32_t extra_rows =
        (height + row_bytes - 1) / row_bytes;
    return pv::PngResourcePlan{
        width, height, row_bytes, decoded_bytes, workspace,
        decoded_bytes + workspace, width, height + extra_rows,
        decoded_bytes, 8, 6, 0};
}

std::uint32_t ReadBigEndian(const std::byte* data);

std::uint8_t ReferencePaeth(const std::uint8_t left, const std::uint8_t up,
                            const std::uint8_t upper_left) {
    const int p = static_cast<int>(left) + up - upper_left;
    const int pa = std::abs(p - left);
    const int pb = std::abs(p - up);
    const int pc = std::abs(p - upper_left);
    if (pa <= pb && pa <= pc) return left;
    return pb <= pc ? up : upper_left;
}

void AppendBigEndian(std::vector<std::byte>& output, const std::uint32_t value) {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 24)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 16)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

void WriteBigEndian(std::vector<std::byte>& output, const std::size_t offset,
                    const std::uint32_t value) {
    Check(output.size() >= 4 && offset <= output.size() - 4,
          "write synthetic big-endian value");
    output[offset] = std::byte{static_cast<std::uint8_t>(value >> 24)};
    output[offset + 1] = std::byte{static_cast<std::uint8_t>(value >> 16)};
    output[offset + 2] = std::byte{static_cast<std::uint8_t>(value >> 8)};
    output[offset + 3] = std::byte{static_cast<std::uint8_t>(value)};
}

void RefreshChunkCrc(std::vector<std::byte>& png,
                     const std::size_t chunk_offset) {
    Check(png.size() >= 12 && chunk_offset <= png.size() - 12,
          "synthetic chunk offset");
    const std::uint32_t length = ReadBigEndian(png.data() + chunk_offset);
    Check(length <= png.size() - chunk_offset - 12,
          "synthetic chunk length");
    uLong crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc,
                reinterpret_cast<const Bytef*>(png.data() + chunk_offset + 4),
                static_cast<uInt>(length + 4));
    WriteBigEndian(png, chunk_offset + 8 + length,
                   static_cast<std::uint32_t>(crc));
}

void AppendChunk(std::vector<std::byte>& png, const std::string_view type,
                 const std::span<const std::byte> payload) {
    Check(type.size() == 4 && payload.size() <= UINT32_MAX,
          "valid synthetic PNG chunk");
    AppendBigEndian(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_begin = png.size();
    for (const char byte : type) {
        png.push_back(std::byte{static_cast<std::uint8_t>(byte)});
    }
    png.insert(png.end(), payload.begin(), payload.end());
    uLong crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc,
                reinterpret_cast<const Bytef*>(png.data() + crc_begin),
                static_cast<uInt>(png.size() - crc_begin));
    AppendBigEndian(png, static_cast<std::uint32_t>(crc));
}

struct SyntheticPng {
    std::vector<std::byte> encoded;
    std::vector<std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

SyntheticPng BuildFilterPng(const std::uint32_t width,
                            const std::span<const std::uint8_t> filters,
                            const std::size_t deflate_block_bytes = 0) {
    Check(width != 0 && !filters.empty() && filters.size() <= UINT32_MAX,
          "valid synthetic PNG dimensions");
    const std::uint32_t height = static_cast<std::uint32_t>(filters.size());
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    std::vector<std::byte> pixels(row_bytes * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const std::size_t offset = static_cast<std::size_t>(row) * row_bytes +
                                           column * 4 + channel;
                pixels[offset] = std::byte{static_cast<std::uint8_t>(
                    row * 37U + column * 19U + channel * 73U +
                    ((row ^ column) * 11U))};
            }
        }
    }

    std::vector<std::byte> filtered((row_bytes + 1) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t filter = filters[row];
        const std::size_t filtered_row = static_cast<std::size_t>(row) *
                                         (row_bytes + 1);
        filtered[filtered_row] = std::byte{filter};
        for (std::size_t column = 0; column < row_bytes; ++column) {
            const std::size_t raw_offset = static_cast<std::size_t>(row) *
                                           row_bytes + column;
            const std::uint8_t current = static_cast<std::uint8_t>(pixels[raw_offset]);
            const std::uint8_t left = column >= 4
                ? static_cast<std::uint8_t>(pixels[raw_offset - 4])
                : 0;
            const std::uint8_t up = row != 0
                ? static_cast<std::uint8_t>(pixels[raw_offset - row_bytes])
                : 0;
            const std::uint8_t upper_left = row != 0 && column >= 4
                ? static_cast<std::uint8_t>(
                      pixels[raw_offset - row_bytes - 4])
                : 0;
            std::uint8_t prediction = 0;
            switch (filter) {
                case 0: break;
                case 1: prediction = left; break;
                case 2: prediction = up; break;
                case 3: prediction = static_cast<std::uint8_t>(
                            (static_cast<unsigned int>(left) + up) / 2U);
                        break;
                case 4: prediction = ReferencePaeth(left, up, upper_left); break;
                default: break;
            }
            filtered[filtered_row + 1 + column] = std::byte{
                static_cast<std::uint8_t>(current - prediction)};
        }
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(filtered.size()));
    if (deflate_block_bytes != 0) {
        compressed_size += static_cast<uLongf>(
            (filtered.size() / deflate_block_bytes + 1) * 16);
    }
    std::vector<std::byte> compressed(compressed_size);
    if (deflate_block_bytes == 0) {
        const int compressed_result = compress2(
            reinterpret_cast<Bytef*>(compressed.data()), &compressed_size,
            reinterpret_cast<const Bytef*>(filtered.data()),
            static_cast<uLong>(filtered.size()), Z_BEST_SPEED);
        Check(compressed_result == Z_OK, "compress synthetic PNG scanlines");
    } else {
        z_stream stream{};
        Check(deflateInit(&stream, Z_BEST_SPEED) == Z_OK,
              "initialize blocked synthetic PNG compression");
        for (std::size_t offset = 0; offset < filtered.size();) {
            const std::size_t bytes = std::min(
                deflate_block_bytes, filtered.size() - offset);
            stream.next_in = reinterpret_cast<Bytef*>(filtered.data() + offset);
            stream.avail_in = static_cast<uInt>(bytes);
            stream.next_out = reinterpret_cast<Bytef*>(compressed.data()) +
                              stream.total_out;
            stream.avail_out = static_cast<uInt>(compressed.size() -
                                                 stream.total_out);
            offset += bytes;
            const int flush = offset == filtered.size() ? Z_FINISH : Z_FULL_FLUSH;
            const int result = deflate(&stream, flush);
            Check(result == (flush == Z_FINISH ? Z_STREAM_END : Z_OK) &&
                      stream.avail_in == 0,
                  "compress blocked synthetic PNG scanlines");
        }
        compressed_size = stream.total_out;
        Check(deflateEnd(&stream) == Z_OK,
              "finish blocked synthetic PNG compression");
    }
    compressed.resize(compressed_size);

    std::vector<std::byte> png{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    std::array<std::byte, 13> ihdr{};
    ihdr[0] = std::byte{static_cast<std::uint8_t>(width >> 24)};
    ihdr[1] = std::byte{static_cast<std::uint8_t>(width >> 16)};
    ihdr[2] = std::byte{static_cast<std::uint8_t>(width >> 8)};
    ihdr[3] = std::byte{static_cast<std::uint8_t>(width)};
    ihdr[4] = std::byte{static_cast<std::uint8_t>(height >> 24)};
    ihdr[5] = std::byte{static_cast<std::uint8_t>(height >> 16)};
    ihdr[6] = std::byte{static_cast<std::uint8_t>(height >> 8)};
    ihdr[7] = std::byte{static_cast<std::uint8_t>(height)};
    ihdr[8] = std::byte{8};
    ihdr[9] = std::byte{6};
    AppendChunk(png, "IHDR", ihdr);
    const std::size_t split0 = compressed.size() / 3;
    const std::size_t split1 = compressed.size() * 2 / 3;
    AppendChunk(png, "IDAT", std::span(compressed).subspan(0, split0));
    AppendChunk(png, "IDAT",
                std::span(compressed).subspan(split0, split1 - split0));
    AppendChunk(png, "IDAT", std::span(compressed).subspan(split1));
    AppendChunk(png, "IEND", {});
    return {std::move(png), std::move(pixels), width, height};
}

SyntheticPng BuildMixedFilterPng(const std::uint32_t width) {
    constexpr std::array<std::uint8_t, 24> filters{
        0, 4, 4, 4, 4, 4, 4, 4, 4, 1, 2, 3,
        4, 4, 4, 4, 4, 4, 4, 4, 2, 4, 4, 4};
    return BuildFilterPng(width, filters);
}

void CheckGuardBytes(const std::span<const std::byte> storage,
                     const std::size_t guard_bytes, const std::byte canary,
                     const char* const leading_message,
                     const char* const trailing_message) {
    Check(std::all_of(storage.begin(), storage.begin() + guard_bytes,
                      [canary](const std::byte value) { return value == canary; }),
          leading_message);
    Check(std::all_of(storage.end() - guard_bytes, storage.end(),
                      [canary](const std::byte value) { return value == canary; }),
          trailing_message);
}

void CheckSyntheticDecode(const SyntheticPng& sample,
                          const std::uint32_t stride = 0,
                          pv::PngDecodeTimings* const timings = nullptr,
                          const pv::PngValidationOptions validation = {}) {
    constexpr std::size_t guard_bytes = 64;
    const std::size_t decoded_bytes = sample.pixels.size();
    const std::uint32_t actual_stride = stride == 0 ? sample.width * 4 : stride;
    const std::size_t allocation_bytes = std::max(
        decoded_bytes + sample.height,
        static_cast<std::size_t>(actual_stride) * sample.height);
    std::vector<std::byte> storage(guard_bytes + allocation_bytes + guard_bytes,
                                   std::byte{0xA5});
    pv::DecodeSurface surface{storage.data() + guard_bytes, allocation_bytes,
                              decoded_bytes, sample.width, sample.height,
                              actual_stride};
    std::vector<std::byte> encoded = sample.encoded;
    const pv::PngResourcePlan plan = ParsePlan(encoded);
    pv::CheckHr(pv::DecodePngSpng(encoded, surface, plan, validation,
                                  nullptr, nullptr, timings),
                "decode mixed-filter synthetic PNG");
    const std::size_t row_bytes = static_cast<std::size_t>(sample.width) * 4;
    for (std::uint32_t row = 0; row < sample.height; ++row) {
        Check(std::equal(sample.pixels.begin() + static_cast<std::size_t>(row) * row_bytes,
                         sample.pixels.begin() + static_cast<std::size_t>(row + 1) * row_bytes,
                         surface.pixels + static_cast<std::size_t>(row) * actual_stride),
              "mixed-filter synthetic PNG full pixel equality");
    }
    CheckGuardBytes(storage, guard_bytes, std::byte{0xA5},
                    "synthetic PNG leading canary",
                    "synthetic PNG trailing canary");
}

void TestMixedFiltersAndBounds() {
    constexpr std::array<std::uint8_t, 10> all_filters{
        0, 1, 2, 3, 4, 4, 3, 2, 1, 0};
    for (const std::uint32_t width : {1U, 2U, 3U, 17U, 257U}) {
        const SyntheticPng sample = BuildFilterPng(width, all_filters);
        pv::PngDecodeTimings timings;
        CheckSyntheticDecode(sample, 0, &timings);
        for (std::size_t filter = 0; filter < timings.filter_rows.size(); ++filter) {
            Check(timings.filter_rows[filter] == 2,
                  "all PNG filters accounted for exactly once per row");
        }
    }

    const SyntheticPng mixed = BuildMixedFilterPng(17);
    CheckSyntheticDecode(mixed, mixed.width * 4 + 28);
}

std::vector<std::uint8_t> ReferencePaethRows(
    const std::span<const std::uint8_t> filtered,
    const std::size_t row_bytes, const std::size_t height) {
    std::vector<std::uint8_t> decoded(row_bytes * height);
    for (std::size_t row = 0; row < height; ++row) {
        Check(filtered[row * (row_bytes + 1)] == 4,
              "Paeth differential input uses filter 4");
        for (std::size_t column = 0; column < row_bytes; ++column) {
            const std::uint8_t left = column >= 4
                ? decoded[row * row_bytes + column - 4]
                : 0;
            const std::uint8_t up = row != 0
                ? decoded[(row - 1) * row_bytes + column]
                : 0;
            const std::uint8_t upper_left = row != 0 && column >= 4
                ? decoded[(row - 1) * row_bytes + column - 4]
                : 0;
            decoded[row * row_bytes + column] = static_cast<std::uint8_t>(
                filtered[row * (row_bytes + 1) + column + 1] +
                ReferencePaeth(left, up, upper_left));
        }
    }
    return decoded;
}

void CheckPaethWavefront(const std::size_t row_bytes,
                         const std::size_t prefix_rows,
                         const std::size_t batch_rows) {
    const std::size_t height = prefix_rows + batch_rows;
    std::vector<std::uint8_t> filtered((row_bytes + 1) * height);
    for (std::size_t row = 0; row < height; ++row) {
        filtered[row * (row_bytes + 1)] = 4;
        for (std::size_t column = 0; column < row_bytes; ++column) {
            filtered[row * (row_bytes + 1) + column + 1] =
                static_cast<std::uint8_t>(
                    row * 97U + column * 53U + (row ^ column) * 29U);
        }
    }
    const std::vector<std::uint8_t> expected =
        ReferencePaethRows(filtered, row_bytes, height);
    std::vector<std::uint8_t> actual = filtered;
    for (std::size_t row = 0; row < prefix_rows; ++row) {
        std::uint8_t* const destination =
            actual.data() + row * row_bytes;
        const std::uint8_t* const previous =
            row == 0 ? nullptr : destination - row_bytes;
        Check(pv::png_internal::UnfilterRgba8Row(
                  destination,
                  actual.data() + row * (row_bytes + 1) + 1,
                  previous, row_bytes, 4),
              "decode Paeth prefix row");
    }
    std::vector<std::uint8_t> scratch(height * 7);
    std::uint8_t* const destination =
        actual.data() + prefix_rows * row_bytes;
    const std::uint8_t* const source =
        actual.data() + prefix_rows * (row_bytes + 1) + 1;
    if (batch_rows == 4) {
        pv::png_internal::UnfilterPaethRows4(
            destination, source, row_bytes, scratch.data());
    } else {
        Check(batch_rows == 8, "supported Paeth wavefront batch");
        pv::png_internal::UnfilterPaethRows8(
            destination, source, row_bytes, scratch.data());
    }
    Check(std::equal(expected.begin(), expected.end(), actual.begin()),
          "SIMD Paeth wavefront must match scalar reference");
}

void TestPaethWavefrontDifferential() {
    for (const std::size_t row_bytes :
         {4U, 12U, 16U, 20U, 28U, 64U, 68U, 252U, 1028U}) {
        CheckPaethWavefront(row_bytes, 1, 4);
        CheckPaethWavefront(row_bytes, 5, 4);
        CheckPaethWavefront(row_bytes, 1, 8);
        CheckPaethWavefront(row_bytes, 5, 8);
    }
    std::array<std::uint8_t, 4> source{};
    std::array<std::uint8_t, 4> destination{};
    Check(!pv::png_internal::UnfilterRgba8Row(
              destination.data(), source.data(), nullptr,
              destination.size(), 5),
          "reject invalid PNG row filter");
}

void CheckSyntheticDecodeFails(const SyntheticPng& sample,
                               const std::size_t allocation_bytes,
                               const char* const message,
                               const pv::PngValidationOptions validation = {}) {
    constexpr std::size_t guard_bytes = 64;
    std::vector<std::byte> storage(guard_bytes + allocation_bytes + guard_bytes,
                                   std::byte{0xC7});
    pv::DecodeSurface surface{
        storage.data() + guard_bytes, allocation_bytes, sample.pixels.size(),
        sample.width, sample.height, sample.width * 4};
    std::vector<std::byte> encoded = sample.encoded;
    const pv::PngResourcePlan plan = ParsePlan(encoded);
    Check(FAILED(pv::DecodePngSpng(encoded, surface, plan, validation)),
          message);
    CheckGuardBytes(storage, guard_bytes, std::byte{0xC7},
                    "failed decode leading canary",
                    "failed decode trailing canary");
}

void TestMalformedDeflateAndOutputBounds() {
    constexpr std::array<std::uint8_t, 3> valid_filters{0, 2, 4};
    const SyntheticPng valid = BuildFilterPng(31, valid_filters);
    const std::size_t required_bytes = valid.pixels.size() + valid.height;
    CheckSyntheticDecodeFails(valid, required_bytes - 1,
                              "reject undersized filtered/output allocation");

    constexpr std::array<std::uint8_t, 1> invalid_filter{5};
    const SyntheticPng bad_filter = BuildFilterPng(31, invalid_filter);
    CheckSyntheticDecodeFails(bad_filter,
                              bad_filter.pixels.size() + bad_filter.height,
                              "reject unknown PNG row filter");

    SyntheticPng truncated = valid;
    truncated.encoded.resize(truncated.encoded.size() - 7);
    CheckSyntheticDecodeFails(truncated, required_bytes,
                              "reject truncated PNG chunk stream");

    SyntheticPng corrupt_deflate = valid;
    bool corrupted = false;
    for (std::size_t offset = 8; offset + 12 <= corrupt_deflate.encoded.size();) {
        const std::uint32_t length = ReadBigEndian(
            corrupt_deflate.encoded.data() + offset);
        Check(length <= corrupt_deflate.encoded.size() - offset - 12,
              "synthetic PNG chunks remain in bounds");
        if (std::memcmp(corrupt_deflate.encoded.data() + offset + 4,
                        "IDAT", 4) == 0 && length >= 2) {
            corrupt_deflate.encoded[offset + 8] = std::byte{0};
            RefreshChunkCrc(corrupt_deflate.encoded, offset);
            corrupted = true;
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    Check(corrupted, "find synthetic IDAT to corrupt");
    CheckSyntheticDecodeFails(corrupt_deflate, required_bytes,
                              "reject corrupt zlib/DEFLATE stream");

    SyntheticPng corrupt_idat_crc = valid;
    bool crc_corrupted = false;
    for (std::size_t offset = 8;
         offset + 12 <= corrupt_idat_crc.encoded.size();) {
        const std::uint32_t length = ReadBigEndian(
            corrupt_idat_crc.encoded.data() + offset);
        Check(length <= corrupt_idat_crc.encoded.size() - offset - 12,
              "CRC test chunk remains in bounds");
        if (std::memcmp(corrupt_idat_crc.encoded.data() + offset + 4,
                        "IDAT", 4) == 0) {
            corrupt_idat_crc.encoded[offset + 8 + length] ^=
                std::byte{0x01};
            crc_corrupted = true;
            break;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    Check(crc_corrupted, "find synthetic IDAT CRC to corrupt");
    CheckSyntheticDecodeFails(corrupt_idat_crc, required_bytes,
                              "reject IDAT payload with invalid CRC");
    CheckSyntheticDecodeFails(
        corrupt_idat_crc, required_bytes,
        "critical CRC mode must validate IDAT",
        {pv::PngChunkCrcMode::Critical, true});
    CheckSyntheticDecode(
        corrupt_idat_crc, 0, nullptr,
        {pv::PngChunkCrcMode::NonIdat, true});
    CheckSyntheticDecode(corrupt_idat_crc, 0, nullptr,
                         {pv::PngChunkCrcMode::None, true});

    SyntheticPng corrupt_ancillary_crc = valid;
    std::array<std::byte, 13> ancillary{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{'t'}, std::byte{'E'}, std::byte{'X'}, std::byte{'t'},
        std::byte{'x'}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}};
    const std::uint32_t ancillary_crc = libdeflate_crc32(
        0, ancillary.data() + 4, 5);
    ancillary[9] = std::byte{
        static_cast<std::uint8_t>((ancillary_crc >> 24U) ^ 1U)};
    ancillary[10] = std::byte{static_cast<std::uint8_t>(ancillary_crc >> 16U)};
    ancillary[11] = std::byte{static_cast<std::uint8_t>(ancillary_crc >> 8U)};
    ancillary[12] = std::byte{static_cast<std::uint8_t>(ancillary_crc)};
    corrupt_ancillary_crc.encoded.insert(
        corrupt_ancillary_crc.encoded.begin() + 33, ancillary.begin(),
        ancillary.end());
    CheckSyntheticDecodeFails(
        corrupt_ancillary_crc, required_bytes,
        "all CRC mode must validate ancillary chunks");
    CheckSyntheticDecodeFails(
        corrupt_ancillary_crc, required_bytes,
        "non-IDAT CRC mode must validate ancillary chunks",
        {pv::PngChunkCrcMode::NonIdat, true});
    CheckSyntheticDecode(
        corrupt_ancillary_crc, 0, nullptr,
        {pv::PngChunkCrcMode::Critical, true});
    CheckSyntheticDecode(corrupt_ancillary_crc, 0, nullptr,
                         {pv::PngChunkCrcMode::None, true});

    SyntheticPng corrupt_adler = valid;
    std::size_t last_idat = std::numeric_limits<std::size_t>::max();
    for (std::size_t offset = 8; offset + 12 <= corrupt_adler.encoded.size();) {
        const std::uint32_t length = ReadBigEndian(
            corrupt_adler.encoded.data() + offset);
        Check(length <= corrupt_adler.encoded.size() - offset - 12,
              "Adler test chunk remains in bounds");
        if (std::memcmp(corrupt_adler.encoded.data() + offset + 4,
                        "IDAT", 4) == 0) {
            last_idat = offset;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    Check(last_idat != std::numeric_limits<std::size_t>::max(),
          "find final synthetic IDAT");
    const std::uint32_t last_length = ReadBigEndian(
        corrupt_adler.encoded.data() + last_idat);
    Check(last_length >= 4, "final IDAT contains zlib Adler trailer");
    corrupt_adler.encoded[last_idat + 8 + last_length - 1] ^=
        std::byte{0x01};
    RefreshChunkCrc(corrupt_adler.encoded, last_idat);
    CheckSyntheticDecodeFails(corrupt_adler, required_bytes,
                              "reject valid-DEFLATE stream with invalid Adler-32");
    CheckSyntheticDecode(corrupt_adler, 0, nullptr,
                         {pv::PngChunkCrcMode::All, false});

    SyntheticPng trailing_deflate_data = valid;
    last_idat = std::numeric_limits<std::size_t>::max();
    for (std::size_t offset = 8;
         offset + 12 <= trailing_deflate_data.encoded.size();) {
        const std::uint32_t length = ReadBigEndian(
            trailing_deflate_data.encoded.data() + offset);
        Check(length <= trailing_deflate_data.encoded.size() - offset - 12,
              "trailing-data test chunk remains in bounds");
        if (std::memcmp(trailing_deflate_data.encoded.data() + offset + 4,
                        "IDAT", 4) == 0) {
            last_idat = offset;
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    Check(last_idat != std::numeric_limits<std::size_t>::max(),
          "find final IDAT for trailing-data test");
    const std::uint32_t trailing_length = ReadBigEndian(
        trailing_deflate_data.encoded.data() + last_idat);
    Check(trailing_length >= 4 && trailing_length != UINT32_MAX,
          "final IDAT can accept one trailing DEFLATE byte");
    trailing_deflate_data.encoded.insert(
        trailing_deflate_data.encoded.begin() +
            static_cast<std::ptrdiff_t>(last_idat + 8 + trailing_length - 4),
        std::byte{0});
    WriteBigEndian(trailing_deflate_data.encoded, last_idat,
                   trailing_length + 1);
    RefreshChunkCrc(trailing_deflate_data.encoded, last_idat);
    CheckSyntheticDecodeFails(
        trailing_deflate_data, required_bytes,
        "reject bytes between the DEFLATE end and zlib Adler-32");
    CheckSyntheticDecodeFails(
        trailing_deflate_data, required_bytes,
        "fast mode must still require exact DEFLATE input consumption",
        {pv::PngChunkCrcMode::None, false});

    SyntheticPng corrupt_ihdr_crc = valid;
    corrupt_ihdr_crc.encoded[32] ^= std::byte{1};
    std::vector<std::byte> bad_header_storage(required_bytes);
    pv::DecodeSurface bad_header_surface{
        bad_header_storage.data(), bad_header_storage.size(),
        valid.pixels.size(), valid.width, valid.height, valid.width * 4};
    Check(FAILED(pv::DecodePngSpng(
              corrupt_ihdr_crc.encoded, bad_header_surface,
              ParsePlan(valid.encoded),
              {pv::PngChunkCrcMode::None, false})),
          "IHDR CRC remains mandatory in fast mode");

    constexpr std::size_t guard_bytes = 64;
    std::vector<std::byte> storage(
        guard_bytes + required_bytes + guard_bytes, std::byte{0xD3});
    pv::DecodeSurface bad_stride{
        storage.data() + guard_bytes, required_bytes, valid.pixels.size(),
        valid.width, valid.height, valid.width * 4 - 1};
    std::vector<std::byte> encoded = valid.encoded;
    Check(pv::DecodePngSpng(encoded, bad_stride, ParsePlan(encoded), {}) ==
              E_INVALIDARG,
          "reject output stride narrower than a decoded row");
    CheckGuardBytes(storage, guard_bytes, std::byte{0xD3},
                    "bad stride leading canary",
                    "bad stride trailing canary");
}

void TestFusedDeflateUnfilterObservability() {
    constexpr std::array<std::uint8_t, 24> filter_pattern{
        0, 1, 2, 3, 4, 4, 4, 4, 4, 2, 3, 1,
        4, 4, 4, 4, 4, 4, 4, 4, 2, 3, 1, 0};
    std::vector<std::uint8_t> filters(1100);
    for (std::size_t row = 0; row < filters.size(); ++row) {
        filters[row] = filter_pattern[row % filter_pattern.size()];
    }
    constexpr std::size_t workers = 4;
    constexpr std::size_t guard_bytes = 64;
    std::array<SyntheticPng, workers> samples{
        BuildFilterPng(2048, filters, 1024 * 1024),
        BuildFilterPng(2051, filters, 1024 * 1024),
        BuildFilterPng(2053, filters, 1024 * 1024),
        BuildFilterPng(2057, filters, 1024 * 1024)};
    std::array<std::vector<std::byte>, workers> encoded_images;
    std::array<std::vector<std::byte>, workers> storage;
    std::array<pv::DecodeSurface, workers> surfaces;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        encoded_images[worker] = samples[worker].encoded;
        const std::size_t allocation_bytes =
            samples[worker].pixels.size() + samples[worker].height;
        storage[worker].assign(guard_bytes + allocation_bytes + guard_bytes,
                               std::byte{0x9B});
        surfaces[worker] = {
            storage[worker].data() + guard_bytes, allocation_bytes,
            samples[worker].pixels.size(), samples[worker].width,
            samples[worker].height, samples[worker].width * 4};
    }

    std::latch ready(workers);
    std::latch start(1);
    std::array<HRESULT, workers> results;
    results.fill(E_PENDING);
    std::array<pv::PngDecodeTimings, workers> timings;
    std::array<pv::PngResourcePlan, workers> plans;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        plans[worker] = ParsePlan(encoded_images[worker]);
    }
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            ready.count_down();
            start.wait();
            results[worker] = pv::DecodePngSpng(
                encoded_images[worker], surfaces[worker], plans[worker], {},
                nullptr, nullptr, &timings[worker]);
        });
    }
    ready.wait();
    start.count_down();
    threads.clear();

    for (std::size_t worker = 0; worker < workers; ++worker) {
        Check(SUCCEEDED(results[worker]),
              "four-worker fused PNG decode succeeds");
        Check(std::equal(samples[worker].pixels.begin(),
                         samples[worker].pixels.end(),
                         surfaces[worker].pixels),
              "four-worker fused PNG pixels are exact");
        CheckGuardBytes(storage[worker], guard_bytes, std::byte{0x9B},
                        "four-worker fused leading canary",
                        "four-worker fused trailing canary");
        const std::size_t row_bytes =
            static_cast<std::size_t>(samples[worker].width) * 4;
        Check(timings[worker].fused_rows > 0 &&
                  timings[worker].fused_rows <= samples[worker].height,
              "large PNG must exercise fused DEFLATE/unfilter callbacks");
        Check(timings[worker].fused_output_bytes ==
                  static_cast<std::uint64_t>(timings[worker].fused_rows) *
                      row_bytes,
              "fused output byte counter matches completed RGBA rows");
        Check(timings[worker].unfilter_nanoseconds > 0 &&
                  timings[worker].unfilter_nanoseconds <=
                      timings[worker].deflate_nanoseconds,
              "fused unfilter time is measured inside DEFLATE time");
        Check(std::accumulate(timings[worker].filter_rows.begin(),
                              timings[worker].filter_rows.end(),
                              std::uint32_t{0}) == samples[worker].height,
              "fused and deferred rows are each unfiltered exactly once");
    }
    const std::uint64_t fused_rows = std::accumulate(
        timings.begin(), timings.end(), std::uint64_t{0},
        [](const std::uint64_t total, const pv::PngDecodeTimings& timing) {
            return total + timing.fused_rows;
        });
    const std::uint64_t fused_output_bytes = std::accumulate(
        timings.begin(), timings.end(), std::uint64_t{0},
        [](const std::uint64_t total, const pv::PngDecodeTimings& timing) {
            return total + timing.fused_output_bytes;
        });
    std::cout << "FusedAcceptance workers=" << workers
              << " fused_rows=" << fused_rows
              << " fused_output_bytes=" << fused_output_bytes
              << " result=exact canaries=intact\n";
}

void TestSpng() {
    const std::array<unsigned char, 70> raw{
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
        0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
        0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,
        0x08,0xD7,0x63,0xF8,0xCF,0xC0,0xF0,0x1F,0x00,
        0x05,0x00,0x01,0xFF,0x72,0x9C,0x52,0x67,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,
        0xAE,0x42,0x60,0x82};
    std::array<std::byte, raw.size()> png{};
    for (std::size_t index = 0; index < raw.size(); ++index) png[index] = std::byte{raw[index]};

    std::array<std::byte, 8> decoded{};
    pv::DecodeSurface surface{decoded.data(), decoded.size(), 4, 1, 1, 4};
    pv::CheckHr(pv::DecodePngSpng(png, surface, ParsePlan(png), {}),
                "Decode embedded PNG with libspng");
    Check(surface.pixels[3] == std::byte{0xFF}, "decoded alpha channel");

    const std::array<unsigned char, 68> fallback_raw{
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
        0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x04,0x00,0x00,0x00,0xB5,0x1C,0x0C,0x02,
        0x00,0x00,0x00,0x0B,0x49,0x44,0x41,0x54,
        0x78,0xDA,0x63,0x64,0xF8,0x0F,0x00,0x01,
        0x05,0x01,0x01,0x27,0x18,0xE3,0x66,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,
        0xAE,0x42,0x60,0x82};
    std::array<std::byte, fallback_raw.size()> fallback_png{};
    for (std::size_t index = 0; index < fallback_raw.size(); ++index) {
        fallback_png[index] = std::byte{fallback_raw[index]};
    }
    pv::CheckHr(pv::DecodePngSpng(fallback_png, surface,
                                  ParsePlan(fallback_png), {}),
                "Decode grayscale-alpha PNG with libspng/zlib-ng fallback");
}

void TestRejectedWorkReleasesInput() {
    pv::ResourceSlots slots(1, 1, 1, pv::MiB(1), pv::MiB(1));
    const pv::SlotId compressed_slot = slots.AcquireCompressed(4096, 0, 1);
    const pv::SlotId staging_slot = slots.AcquireStaging(4096, 0, 1);
    Check(compressed_slot != pv::kInvalidSlot &&
              staging_slot != pv::kInvalidSlot,
          "allocate cancellation test slots");
    slots.CompleteFileRead(compressed_slot);
    slots.BeginDecodeInput(compressed_slot);
    slots.BeginDecodeOutput(staging_slot);

    pv::DecodeStage decode_stage(1, pv::DecodeSlotView(slots), {});
    {
        decode_stage.Start(1);
        pv::DecodeWork work{0, 1, compressed_slot, staging_slot};
        Check(decode_stage.Submit(work), "decode stage rejected test work");

        const DWORD wait = WaitForSingleObject(
            decode_stage.CompletionEvent(), 5000);
        Check(wait == WAIT_OBJECT_0,
              "worker completion notification timed out");
        const pv::CompletionQueue::Batch& batch = decode_stage.Drain();
        Check(batch.results.size() == 1 && !batch.results.front().success &&
                  batch.results.front().error == E_INVALIDARG,
              "worker must report invalid decode resources");

        Check(batch.released_inputs.size() == 1 &&
                  batch.released_inputs.front().compressed_slot == compressed_slot,
              "rejected work must release its compressed input exactly once");
        decode_stage.Stop();
    }

    slots.ReleaseCompressed(compressed_slot);
    slots.ReleaseStaging(staging_slot);
    Check(slots.FreeCompressedCount() == 1 &&
              slots.FreeStagingCount() == 1,
          "rejected work slots must return to their free indexes");
}

class RecordingPipelineObserver final : public pv::PipelineObserver {
public:
    void OnFrameReady(const std::size_t index) override {
        ready.push_back(index);
    }
    void OnFramePresented(const std::size_t index) override {
        presented.push_back(index);
    }

    std::vector<std::size_t> ready;
    std::vector<std::size_t> presented;
};

void TestPresentationControllerLifecycle() {
    pv::PresentationController presentation;
    Check(presentation.NeedsFrameCreditEvent() && !presentation.CanDraw(),
          "presentation must begin without frame credit");
    presentation.GrantFrameCredit();
    Check(presentation.CanDraw(), "frame credit must authorize one draw");
    presentation.StartDraw(3, 7);
    Check(!presentation.CanDraw() && presentation.DrawFence() == 7,
          "draw handoff must retain its slot until the fence completes");
    Check(!presentation.CompleteDraw(6),
          "an earlier fence value must not release a draw slot");
    const auto completed = presentation.CompleteDraw(7);
    Check(completed && *completed == 3 && presentation.DrawFence() == 0,
          "the matching fence must release the draw slot exactly once");
    Check(!presentation.CompleteDraw(8),
          "a completed draw slot must not be released twice");

    bool rejected_invalid_draw = false;
    presentation.RequestRedraw(true);
    try {
        presentation.StartDraw(pv::kInvalidSlot, 9);
    } catch (const std::logic_error&) {
        rejected_invalid_draw = true;
    }
    Check(rejected_invalid_draw,
          "presentation must reject a draw without a valid texture slot");
}

void TestPipelineInitialFailureState() {
    pv::Config config;
    config.compressed_slot_count = 1;
    config.staging_slot_count = 1;
    config.gpu_forward_slot_count = 1;
    config.gpu_reverse_slot_count = 0;
    pv::ViewerWindow window;
    RecordingPipelineObserver observer;
    pv::PipelineRuntime pipeline(observer, config, window);

    pv::Catalog catalog;
    pv::CatalogItem item;
    item.path = L"known-empty.png";
    item.file_size_known = true;
    item.file_bytes = 0;
    catalog.items.push_back(std::move(item));
    pipeline.LoadInitialCatalog(std::move(catalog), true);

    Check(pipeline.InitialContentFailed(),
          "known-invalid initial content must surface a startup failure");
    Check(!pipeline.InitialContentPending(),
          "failed initial content must not leave startup waiting forever");
    Check(pipeline.PendingFrameStage() == pv::PipelineStage::Failed,
          "pipeline diagnostics must identify the failed initial stage");
    Check(observer.ready.empty() && observer.presented.empty(),
          "failed initial content must never publish frame callbacks");
}

void TestManagedStagingUpload() {
    pv::ComPtr<ID3D11Device> device;
    pv::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    pv::CheckHr(D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                    feature_levels.data(), static_cast<UINT>(feature_levels.size()),
                    D3D11_SDK_VERSION, &device, &feature_level, &context),
                "Create D3D11 device for managed staging test");

    constexpr UINT width = 16;
    constexpr UINT height = 16;
    constexpr UINT stride = width * 4;
    std::array<std::byte, stride * height> source{};
    for (std::size_t offset = 0; offset < source.size(); offset += 4) {
        source[offset + 0] = std::byte{0x12};
        source[offset + 1] = std::byte{0x34};
        source[offset + 2] = std::byte{0x56};
        source[offset + 3] = std::byte{0xFF};
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pv::ComPtr<ID3D11Texture2D> texture;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &texture),
                "Create destination texture for managed staging test");

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    pv::ComPtr<ID3D11Texture2D> upload;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &upload),
                "Create managed upload staging texture");
    D3D11_MAPPED_SUBRESOURCE upload_mapping{};
    pv::CheckHr(context->Map(upload.Get(), 0, D3D11_MAP_WRITE, 0,
                            &upload_mapping),
                "Map managed upload staging texture");
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(static_cast<std::byte*>(upload_mapping.pData) +
                        static_cast<std::size_t>(row) * upload_mapping.RowPitch,
                    source.data() + static_cast<std::size_t>(row) * stride,
                    stride);
    }
    context->Unmap(upload.Get(), 0);

    std::memset(source.data(), 0xA5, source.size());
    context->CopyResource(texture.Get(), upload.Get());

    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    pv::ComPtr<ID3D11Texture2D> readback;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &readback),
                "Create readback staging texture for managed staging test");
    context->CopyResource(readback.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    pv::CheckHr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                "Read texture uploaded through managed staging");
    const auto* const pixel = static_cast<const std::byte*>(mapped.pData);
    const bool retained = pixel[0] == std::byte{0x12} &&
                          pixel[1] == std::byte{0x34} &&
                          pixel[2] == std::byte{0x56} &&
                          pixel[3] == std::byte{0xFF};
    context->Unmap(readback.Get(), 0);
    Check(retained, "managed staging upload must preserve source pixels");
}

void TestGraphics(const HINSTANCE instance) {
    constexpr wchar_t class_name[] = L"PhotoViewer.RuntimeTest";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    Check(RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "Register runtime test window");
    HWND window = CreateWindowExW(0, class_name, L"runtime-test", WS_OVERLAPPEDWINDOW,
                                  0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "Create runtime test window");

    {
        pv::Graphics graphics;
        graphics.InitializeDirect3D(window);
        graphics.InitializeDirect2D();
        graphics.InitializeSwapChain();
        graphics.InitializeBackBufferTarget();
        pv::DecodeStaging staging;
        staging.Configure(TestRgba8Plan(64, 64));
        staging.committed_bytes = staging.resource_plan.staging_committed_bytes;
        Check(staging.AllocateCpu(staging.committed_bytes),
              "allocate CPU decode surface");
        Check(staging.PrepareCpuSurface(),
              "prepare CPU decode surface");
        for (UINT row = 0; row < staging.surface.height; ++row) {
            std::byte* const pixels = staging.surface.pixels +
                static_cast<std::size_t>(row) * staging.surface.stride;
            for (UINT column = 0; column < staging.surface.width; ++column) {
                pixels[column * 4 + 0] = std::byte{0x20};
                pixels[column * 4 + 1] = std::byte{0x80};
                pixels[column * 4 + 2] = std::byte{0xE0};
                pixels[column * 4 + 3] = std::byte{0xFF};
            }
        }
        graphics.CopyDecodedToStaging(staging);
        pv::GpuImage image;
        pv::UploadTicket ticket = graphics.SubmitUpload(0, 1, 0, staging, image);
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "D3D11 fence completion");
        graphics.FinishUpload(image);
        ID3D11Texture2D* const first_texture = image.texture.Get();
        ID2D1Bitmap1* const first_bitmap = image.bitmap.Get();
        ticket = graphics.SubmitUpload(1, 1, 0, staging, image);
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "reused D3D11 fence completion");
        graphics.FinishUpload(image);
        Check(image.texture.Get() == first_texture && image.bitmap.Get() == first_bitmap,
              "same-sized GPU Texture and Direct2D bitmap must be reused");
        Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
              "initial frame credit");
        (void)graphics.Draw(image);
        graphics.Resize(400, 300);
        Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
              "resized frame credit");
        (void)graphics.Draw(image);
    }
    if (window) DestroyWindow(window);
}

std::uint32_t ReadBigEndian(const std::byte* const data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Check(input.good(), "open PNG for full pixel verification");
    const std::streamsize length = input.tellg();
    Check(length >= 29, "PNG verification input is too small");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    Check(input.good(), "read PNG for full pixel verification");
    return bytes;
}

std::vector<std::byte> DecodeReferenceSpng(
    const std::span<const std::byte> compressed,
    const std::uint32_t width, const std::uint32_t height) {
    std::unique_ptr<spng_ctx, decltype(&spng_ctx_free)> context(
        spng_ctx_new(0), &spng_ctx_free);
    Check(context != nullptr, "create reference libspng context");
    Check(spng_set_crc_action(context.get(), SPNG_CRC_USE, SPNG_CRC_USE) ==
              SPNG_OK,
          "configure reference libspng CRC validation");
    Check(spng_set_png_buffer(context.get(), compressed.data(), compressed.size()) ==
              SPNG_OK,
          "set reference libspng input");
    spng_ihdr header{};
    Check(spng_get_ihdr(context.get(), &header) == SPNG_OK &&
              header.width == width && header.height == height &&
              header.bit_depth == 8 &&
              header.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA &&
              header.interlace_method == SPNG_INTERLACE_NONE,
          "reference PNG must be non-interlaced RGBA8");
    std::size_t decoded_size = 0;
    Check(spng_decoded_image_size(context.get(), SPNG_FMT_PNG, &decoded_size) ==
              SPNG_OK &&
              decoded_size == static_cast<std::size_t>(width) * height * 4,
          "reference libspng decoded size");
    std::vector<std::byte> decoded(decoded_size);
    Check(spng_decode_image(context.get(), decoded.data(), decoded.size(),
                            SPNG_FMT_PNG, 0) == SPNG_OK,
          "reference libspng full decode");
    return decoded;
}

int VerifyFullPixels(const std::filesystem::path& start_path,
                     const std::size_t requested) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::directory_iterator(start_path.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == L".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    const auto first = std::find(files.begin(), files.end(), start_path);
    Check(first != files.end(), "find starting PNG for full pixel verification");
    std::size_t verified = 0;
    for (auto file = first; file != files.end() && verified < requested; ++file) {
        const std::vector<std::byte> original = ReadFileBytes(*file);
        const std::uint32_t width = ReadBigEndian(original.data() + 16);
        const std::uint32_t height = ReadBigEndian(original.data() + 20);
        if (original[24] != std::byte{8} || original[25] != std::byte{6} ||
            original[28] != std::byte{0}) {
            continue;
        }
        const std::vector<std::byte> reference = DecodeReferenceSpng(
            original, width, height);
        std::vector<std::byte> mutable_png = original;
        constexpr std::size_t guard_bytes = 64;
        const std::size_t decoded_bytes = reference.size();
        const std::size_t allocation_bytes = decoded_bytes + height;
        std::vector<std::byte> storage(
            guard_bytes + allocation_bytes + guard_bytes, std::byte{0x5A});
        pv::DecodeSurface surface{storage.data() + guard_bytes, allocation_bytes,
                                  decoded_bytes, width, height, width * 4};
        pv::CheckHr(pv::DecodePngSpng(mutable_png, surface,
                                      ParsePlan(mutable_png), {}),
                    "fast-path PNG decode for full pixel verification");
        const auto mismatch = std::mismatch(reference.begin(), reference.end(),
                                            surface.pixels);
        if (mismatch.first != reference.end()) {
            const std::size_t offset = static_cast<std::size_t>(
                mismatch.first - reference.begin());
            std::cerr << "PixelMismatch file=" << file->filename()
                      << " offset=" << offset
                      << " row=" << (offset / (width * 4ULL))
                      << " byte_in_row=" << (offset % (width * 4ULL))
                      << " expected="
                      << static_cast<unsigned int>(
                             static_cast<std::uint8_t>(*mismatch.first))
                      << " actual="
                      << static_cast<unsigned int>(
                             static_cast<std::uint8_t>(*mismatch.second))
                      << '\n';
        }
        Check(mismatch.first == reference.end(),
              "fast-path pixels must exactly match reference libspng");
        Check(std::all_of(storage.begin(), storage.begin() + guard_bytes,
                          [](const std::byte value) {
                              return value == std::byte{0x5A};
                          }),
              "real PNG leading canary");
        Check(std::all_of(storage.end() - guard_bytes, storage.end(),
                          [](const std::byte value) {
                              return value == std::byte{0x5A};
                          }),
              "real PNG trailing canary");
        ++verified;
    }
    Check(verified == requested, "not enough compatible PNGs for verification");
    std::cout << "FullPixelVerify images=" << verified
              << " reference=libspng result=exact canaries=intact\n";
    return 0;
}

int BenchmarkDecode(const std::filesystem::path& path, const std::size_t workers,
                    const pv::PngValidationOptions validation = {}) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == L".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    const auto first = std::find(files.begin(), files.end(), path);
    Check(first != files.end() && static_cast<std::size_t>(files.end() - first) >= workers,
          "not enough benchmark PNGs after starting image");

    std::vector<std::vector<std::byte>> compressed_images;
    compressed_images.reserve(workers);
    const auto load_begin = std::chrono::steady_clock::now();
    std::size_t compressed_bytes = 0;

    std::vector<std::vector<std::byte>> pixel_storage;
    std::vector<pv::DecodeSurface> surfaces;
    std::vector<pv::PngResourcePlan> plans;
    pixel_storage.reserve(workers);
    surfaces.reserve(workers);
    plans.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        std::ifstream input(*(first + index), std::ios::binary | std::ios::ate);
        Check(input.good(), "open benchmark PNG");
        const auto length = input.tellg();
        Check(length >= 24, "benchmark PNG is too small");
        std::vector<std::byte> compressed(static_cast<std::size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(compressed.data()), length);
        Check(input.good(), "read benchmark PNG");
        compressed_bytes += compressed.size();
        const std::uint32_t width = ReadBigEndian(compressed.data() + 16);
        const std::uint32_t height = ReadBigEndian(compressed.data() + 20);
        const std::size_t decoded_bytes = static_cast<std::size_t>(width) * height * 4;
        pixel_storage.emplace_back(decoded_bytes + height);
        pv::DecodeSurface surface{pixel_storage.back().data(),
                                  pixel_storage.back().size(), decoded_bytes,
                                  width, height, width * 4};
        plans.push_back(ParsePlan(compressed));
        compressed_images.push_back(std::move(compressed));
        surfaces.push_back(surface);
    }
    const auto load_elapsed = std::chrono::steady_clock::now() - load_begin;

    std::latch ready(static_cast<std::ptrdiff_t>(workers));
    std::latch start(1);
    std::vector<HRESULT> results(workers, E_PENDING);
    std::vector<double> worker_milliseconds(workers, 0.0);
    std::vector<pv::PngDecodeTimings> decode_timings(workers);
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        threads.emplace_back([&, index] {
            ready.count_down();
            start.wait();
            const auto worker_begin = std::chrono::steady_clock::now();
            results[index] = pv::DecodePngSpng(
                compressed_images[index], surfaces[index], plans[index], validation,
                nullptr, nullptr, &decode_timings[index]);
            worker_milliseconds[index] = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - worker_begin).count();
        });
    }
    ready.wait();
    const auto begin = std::chrono::steady_clock::now();
    start.count_down();
    threads.clear();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    Check(std::all_of(results.begin(), results.end(),
                      [](const HRESULT result) { return SUCCEEDED(result); }),
          "PNG benchmark decode");
    std::uint64_t sampled_digest = 1469598103934665603ULL;
    for (const auto& surface : surfaces) {
        for (std::size_t offset = 0; offset < surface.ByteSize(); offset += 4096) {
            sampled_digest ^= static_cast<std::uint8_t>(surface.pixels[offset]);
            sampled_digest *= 1099511628211ULL;
        }
        sampled_digest ^= static_cast<std::uint8_t>(
            surface.pixels[surface.ByteSize() - 1]);
        sampled_digest *= 1099511628211ULL;
    }
    std::sort(worker_milliseconds.begin(), worker_milliseconds.end());
    const auto percentile = [&](const double value) {
        const std::size_t rank = static_cast<std::size_t>(
            std::ceil(value * static_cast<double>(worker_milliseconds.size())));
        return worker_milliseconds[std::min(worker_milliseconds.size() - 1,
                                            std::max<std::size_t>(1, rank) - 1)];
    };
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double load_seconds = std::chrono::duration<double>(load_elapsed).count();
    pv::PngDecodeTimings total_timings;
    for (const auto& timing : decode_timings) {
        total_timings.header_nanoseconds += timing.header_nanoseconds;
        total_timings.chunk_scan_nanoseconds += timing.chunk_scan_nanoseconds;
        total_timings.idat_compaction_nanoseconds += timing.idat_compaction_nanoseconds;
        total_timings.deflate_nanoseconds += timing.deflate_nanoseconds;
        total_timings.unfilter_nanoseconds += timing.unfilter_nanoseconds;
        total_timings.fused_output_bytes += timing.fused_output_bytes;
        total_timings.fused_rows += timing.fused_rows;
        total_timings.deferred_rows += timing.deferred_rows;
        for (std::size_t filter = 0; filter < total_timings.filter_rows.size(); ++filter) {
            total_timings.filter_rows[filter] += timing.filter_rows[filter];
        }
    }
    const auto average_ms = [workers](const std::uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / static_cast<double>(workers) / 1.0e6;
    };
    std::cout << "FileRead images=" << workers
              << " elapsed_ms=" << (load_seconds * 1000.0)
              << " mib_per_second=" << ((compressed_bytes / 1048576.0) / load_seconds)
              << '\n';
    std::cout << "LibdeflateDecode"
              << " workers=" << workers << " images=" << workers
              << " batch_elapsed_ms=" << (seconds * 1000.0)
              << " images_per_second=" << (workers / seconds)
              << " p50_worker_ms=" << percentile(0.50)
              << " p95_worker_ms=" << percentile(0.95)
              << " max_worker_ms=" << worker_milliseconds.back() << '\n';
    std::cout << "DecodeStages"
              << " header_ms=" << average_ms(total_timings.header_nanoseconds)
              << " chunk_scan_ms=" << average_ms(total_timings.chunk_scan_nanoseconds)
              << " idat_compaction_ms="
              << average_ms(total_timings.idat_compaction_nanoseconds)
              << " deflate_ms=" << average_ms(total_timings.deflate_nanoseconds)
              << " unfilter_ms=" << average_ms(total_timings.unfilter_nanoseconds)
              << " unfilter_inside_deflate=1"
              << " fused_rows=" << total_timings.fused_rows
              << " deferred_rows=" << total_timings.deferred_rows
              << " fused_output_mib="
              << (static_cast<double>(total_timings.fused_output_bytes) /
                  1048576.0)
              << " filter_none=" << total_timings.filter_rows[0]
              << " filter_sub=" << total_timings.filter_rows[1]
              << " filter_up=" << total_timings.filter_rows[2]
              << " filter_average=" << total_timings.filter_rows[3]
              << " filter_paeth=" << total_timings.filter_rows[4]
              << " sampled_digest=" << sampled_digest << '\n';
    return 0;
}

int BenchmarkGraphics(const HINSTANCE instance) {
    constexpr wchar_t class_name[] = L"PhotoViewer.GraphicsBenchmark";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    Check(RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "register graphics benchmark window");
    HWND window = CreateWindowExW(0, class_name, L"graphics-benchmark",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 1920, 1080,
                                  nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "create graphics benchmark window");

    pv::Graphics graphics;
    graphics.InitializeDirect3D(window);
    graphics.InitializeDirect2D();
    graphics.InitializeSwapChain();
    graphics.InitializeBackBufferTarget();
    pv::DecodeStaging staging;
    constexpr UINT width = 7680;
    constexpr UINT height = 4320;
    staging.Configure(TestRgba8Plan(width, height));
    graphics.MapDecodeStaging(staging);
    for (UINT row = 0; row < height; ++row) {
        std::memset(staging.surface.pixels +
                        static_cast<std::size_t>(row) * staging.surface.stride,
                    0x80, static_cast<std::size_t>(width) * 4);
    }
    graphics.UnmapDecodeStaging(staging);
    Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
          "initial graphics benchmark frame credit");

    constexpr std::size_t frames = 30;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < frames; ++index) {
        pv::GpuImage image;
        pv::UploadTicket ticket = graphics.SubmitUpload(index, 1, 0, staging, image);
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "graphics benchmark upload fence");
        graphics.FinishUpload(image);
        (void)graphics.Draw(image);
        if (index + 1 < frames) {
            Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
                  "graphics benchmark frame credit");
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "Graphics8K frames=" << frames
              << " elapsed_ms=" << (seconds * 1000.0)
              << " frames_per_second=" << (frames / seconds) << '\n';
    if (window) DestroyWindow(window);
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--graphics-benchmark") {
            return BenchmarkGraphics(GetModuleHandleW(nullptr));
        }
        if (argc == 4 &&
            (std::wstring_view(argv[1]) == L"--decode-benchmark" ||
             std::wstring_view(argv[1]) == L"--decode-benchmark-fast")) {
            const std::size_t workers = std::stoull(argv[3]);
            Check(workers > 0 && workers <= 64, "invalid benchmark worker count");
            const pv::PngValidationOptions validation =
                std::wstring_view(argv[1]) == L"--decode-benchmark-fast"
                    ? pv::PngValidationOptions{pv::PngChunkCrcMode::None, false}
                    : pv::PngValidationOptions{};
            return BenchmarkDecode(argv[2], workers, validation);
        }
        if (argc == 4 && std::wstring_view(argv[1]) == L"--verify-full-pixels") {
            const std::size_t images = std::stoull(argv[3]);
            Check(images > 0 && images <= 256, "invalid full pixel verification count");
            return VerifyFullPixels(argv[2], images);
        }
        const auto run = [](const char* const name, auto&& test) {
            std::cerr << "RUN: " << name << '\n';
            test();
        };
        run("spng", TestSpng);
        run("mixed filters and bounds", TestMixedFiltersAndBounds);
        run("Paeth wavefront differential", TestPaethWavefrontDifferential);
        run("malformed deflate and output bounds",
            TestMalformedDeflateAndOutputBounds);
        run("fused deflate unfilter observability",
            TestFusedDeflateUnfilterObservability);
        run("rejected work releases input", TestRejectedWorkReleasesInput);
        run("presentation lifecycle", TestPresentationControllerLifecycle);
        run("pipeline initial failure", TestPipelineInitialFailureState);
        run("managed staging upload", TestManagedStagingUpload);
        run("graphics", [] { TestGraphics(GetModuleHandleW(nullptr)); });
        std::cout << "PASS: four-worker fused DEFLATE/unfilter, mixed PNG filters, malformed input and guarded output bounds, rejected-work input release, libspng fallback, managed D3D11 staging upload/fence, Direct2D draw, DXGI present\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
