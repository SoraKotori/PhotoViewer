#pragma once

#include "common.h"

namespace pv {

using SlotId = std::uint32_t;
constexpr SlotId kInvalidSlot = std::numeric_limits<SlotId>::max();
constexpr std::size_t kInvalidFrame = std::numeric_limits<std::size_t>::max();

enum class PipelineStage {
    Outside,
    WaitingIo,
    IoInFlight,
    CompressedReady,
    DecodeQueued,
    DecodedStagingAvailable,
    Uploading,
    PresentationTextureAvailable,
    CancelPending,
    Failed,
};

struct CompressedBuffer {
    CompressedBuffer() = default;
    ~CompressedBuffer() {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
    }

    CompressedBuffer(const CompressedBuffer&) = delete;
    CompressedBuffer& operator=(const CompressedBuffer&) = delete;

    [[nodiscard]] bool Allocate(const std::size_t requested) {
        constexpr std::size_t alignment = 4096;
        if (requested > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            return false;
        }
        const std::size_t required = (requested + (alignment - 1)) & ~(alignment - 1);
        if (data && allocation_size >= required) {
            size = requested;
            return true;
        }
        std::byte* const replacement = static_cast<std::byte*>(VirtualAlloc(
            nullptr, required, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!replacement) return false;
        ReleaseAllocation();
        data = replacement;
        allocation_size = required;
        size = requested;
        return true;
    }

    void ReleaseAllocation() {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
        data = nullptr;
        size = 0;
        allocation_size = 0;
    }

    std::byte* data = nullptr;
    std::size_t size = 0;
    std::size_t allocation_size = 0;
};

struct DecodeSurface {
    std::byte* pixels = nullptr;
    std::size_t allocation_bytes = 0;
    std::size_t byte_size = 0;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;

    [[nodiscard]] std::size_t ByteSize() const noexcept { return byte_size; }
};

struct DecodeStaging {
    ~DecodeStaging() { ReleaseAllocation(); }

    DecodeStaging() = default;
    DecodeStaging(const DecodeStaging&) = delete;
    DecodeStaging& operator=(const DecodeStaging&) = delete;

    [[nodiscard]] bool AllocateCpu(const std::size_t bytes) noexcept {
        if (bytes == 0) return false;
        if (cpu_data && cpu_capacity >= bytes) return true;
        std::byte* const replacement = static_cast<std::byte*>(VirtualAlloc(
            nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!replacement) return false;
        if (cpu_data) VirtualFree(cpu_data, 0, MEM_RELEASE);
        cpu_data = replacement;
        cpu_capacity = bytes;
        return true;
    }

    [[nodiscard]] bool PrepareCpuSurface(const UINT width, const UINT height,
                                         const std::size_t decoded_bytes) noexcept {
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
        if (decoded_bytes > committed_bytes ||
            height > committed_bytes - decoded_bytes ||
            !AllocateCpu(committed_bytes)) {
            return false;
        }
        surface = DecodeSurface{cpu_data, cpu_capacity, decoded_bytes,
                                width, height, static_cast<UINT>(row_bytes)};
        cpu_surface = true;
        return true;
    }

    void ReleaseCpuAllocation() noexcept {
        if (cpu_data) VirtualFree(cpu_data, 0, MEM_RELEASE);
        cpu_data = nullptr;
        cpu_capacity = 0;
        if (cpu_surface) surface = {};
        cpu_surface = false;
    }

    void ReleaseAllocation() noexcept {
        ReleaseCpuAllocation();
        texture.Reset();
        texture_width = 0;
        texture_height = 0;
        planned_texture_width = 0;
        planned_texture_height = 0;
        committed_bytes = 0;
        mapped = false;
        surface = {};
    }

    void ResetView() noexcept {
        ReleaseCpuAllocation();
        planned_texture_width = 0;
        planned_texture_height = 0;
        surface = {};
        mapped = false;
    }

    std::byte* cpu_data = nullptr;
    std::size_t cpu_capacity = 0;
    ComPtr<ID3D11Texture2D> texture;
    UINT texture_width = 0;
    UINT texture_height = 0;
    UINT planned_texture_width = 0;
    UINT planned_texture_height = 0;
    std::size_t committed_bytes = 0;
    bool cpu_surface = false;
    bool mapped = false;
    DecodeSurface surface;
};

struct DecodeWork {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

struct DecodeResult {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    bool success = false;
    HRESULT error = S_OK;
    SlotId staging_slot = kInvalidSlot;
};

struct ReleasedInput {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
};

struct GpuImage {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID2D1Bitmap1> bitmap;
    UINT width = 0;
    UINT height = 0;
    std::size_t bytes = 0;
};

struct UploadTicket {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    UINT64 fence_value = 0;
    SlotId staging_slot = kInvalidSlot;
    SlotId gpu_texture_slot = kInvalidSlot;
    std::size_t bytes = 0;
};

}  // namespace pv
