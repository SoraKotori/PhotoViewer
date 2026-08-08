#pragma once

#include "common.h"

#include <array>

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
    void ReleaseAllocation() noexcept {
        texture.Reset();
        texture_width = 0;
        texture_height = 0;
        committed_bytes = 0;
        mapped = false;
        surface = {};
    }

    void ResetView() noexcept {
        surface = {};
        mapped = false;
    }

    ComPtr<ID3D11Texture2D> texture;
    UINT texture_width = 0;
    UINT texture_height = 0;
    std::size_t committed_bytes = 0;
    bool mapped = false;
    DecodeSurface surface;
};

struct alignas(64) WorkToken {
    std::atomic<WorkClaim> claim{WorkClaim::Queued};
    std::array<std::byte, 64 - sizeof(claim)> cache_line_padding{};
};
static_assert(sizeof(WorkToken) == 64);

struct DecodeWork {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    WorkToken* token = nullptr;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

struct DecodeResult {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    bool success = false;
    bool cancelled = false;
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
