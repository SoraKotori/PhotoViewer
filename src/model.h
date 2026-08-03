#pragma once

#include "common.h"

namespace pv {

using SlotId = std::uint32_t;
constexpr SlotId kInvalidSlot = std::numeric_limits<SlotId>::max();

enum class PipelineStage {
    Outside,
    WaitingIo,
    IoInFlight,
    CompressedReady,
    DecodeQueued,
    DecodedPixelSurfaceAvailable,
    Uploading,
    PresentationTextureAvailable,
    CancelPending,
    Failed,
};

enum class WorkClaim : std::uint8_t { Queued, Claimed, Cancelled };

struct CompressedBuffer {
    CompressedBuffer() = default;
    ~CompressedBuffer() {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
    }

    CompressedBuffer(const CompressedBuffer&) = delete;
    CompressedBuffer& operator=(const CompressedBuffer&) = delete;

    [[nodiscard]] bool Allocate(const std::size_t requested) noexcept {
        constexpr std::size_t alignment = 4096;
        if (requested > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            return false;
        }
        const std::size_t required = (requested + (alignment - 1)) & ~(alignment - 1);
        if (data && allocation_size >= required) {
            size = requested;
            return true;
        }
        if (data) {
            VirtualFree(data, 0, MEM_RELEASE);
            data = nullptr;
            allocation_size = 0;
            size = 0;
        }
        data = static_cast<std::byte*>(VirtualAlloc(
            nullptr, required, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!data) return false;
        allocation_size = required;
        size = requested;
        return true;
    }

    void ReleaseAllocation() noexcept {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
        data = nullptr;
        size = 0;
        allocation_size = 0;
    }

    std::byte* data = nullptr;
    std::size_t size = 0;
    std::size_t allocation_size = 0;
};

struct CpuSurface {
    CpuSurface() = default;
    ~CpuSurface() {
        if (virtual_allocation) VirtualFree(virtual_allocation, 0, MEM_RELEASE);
    }

    CpuSurface(const CpuSurface&) = delete;
    CpuSurface& operator=(const CpuSurface&) = delete;

    [[nodiscard]] bool Allocate(const std::size_t bytes) noexcept {
        if (virtual_allocation && allocation_bytes >= bytes) {
            pixels = virtual_allocation;
            byte_size = bytes;
            return true;
        }
        if (virtual_allocation) {
            VirtualFree(virtual_allocation, 0, MEM_RELEASE);
            virtual_allocation = nullptr;
            pixels = nullptr;
            allocation_bytes = 0;
            byte_size = 0;
        }
        virtual_allocation = static_cast<std::byte*>(
            VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!virtual_allocation) return false;
        pixels = virtual_allocation;
        allocation_bytes = bytes;
        byte_size = bytes;
        return true;
    }

    void ReleaseAllocation() noexcept {
        if (virtual_allocation) VirtualFree(virtual_allocation, 0, MEM_RELEASE);
        virtual_allocation = nullptr;
        pixels = nullptr;
        allocation_bytes = 0;
        byte_size = 0;
        width = 0;
        height = 0;
        stride = 0;
    }

    std::byte* virtual_allocation = nullptr;
    std::byte* pixels = nullptr;
    std::size_t allocation_bytes = 0;
    std::size_t byte_size = 0;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;

    [[nodiscard]] std::size_t ByteSize() const noexcept { return byte_size; }
};

struct WorkToken {
    std::atomic<WorkClaim> claim{WorkClaim::Queued};
};

struct DecodeWork {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<WorkToken> token;
    SlotId compressed_slot = kInvalidSlot;
    SlotId cpu_surface_slot = kInvalidSlot;
};

struct DecodeResult {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    bool success = false;
    bool cancelled = false;
    HRESULT error = S_OK;
    SlotId cpu_surface_slot = kInvalidSlot;
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
    SlotId cpu_surface_slot = kInvalidSlot;
    SlotId gpu_texture_slot = kInvalidSlot;
    UINT width = 0;
    UINT height = 0;
    std::size_t bytes = 0;
};

}  // namespace pv
