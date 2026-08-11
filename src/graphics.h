#pragma once

#include "model.h"
#include "win32_handle.h"

namespace pv {

class Graphics {
public:
    Graphics() = default;
    ~Graphics();

    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    void InitializeDirect3D(HWND window);
    void InitializeDirect2D();
    void InitializeSwapChain();
    void InitializeBackBufferTarget();
    void Resize(UINT width, UINT height);

    void PrepareDecodeStaging(DecodeStaging& staging, UINT width, UINT height);
    void MapDecodeStaging(DecodeStaging& staging, UINT width, UINT height,
                          std::size_t decoded_bytes);
    void UnmapDecodeStaging(DecodeStaging& staging) noexcept;
    void CopyDecodedToStaging(DecodeStaging& staging);
    UploadTicket SubmitUpload(std::size_t index, std::uint64_t generation,
                              SlotId staging_slot, const DecodeStaging& source,
                              GpuImage& destination);
    void FinishUpload(GpuImage& image);
    [[nodiscard]] UINT64 Draw(const GpuImage& image);
    void ResetMetrics() noexcept;
    [[nodiscard]] std::uint64_t UploadCount() const noexcept;
    [[nodiscard]] std::uint64_t UploadNanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t DrawCount() const noexcept;
    [[nodiscard]] std::uint64_t DrawNanoseconds() const noexcept;

    [[nodiscard]] HANDLE FrameWaitableObject() const noexcept {
        return frame_waitable_.Get();
    }
    [[nodiscard]] HANDLE FenceEvent() const noexcept { return fence_event_.Get(); }
    [[nodiscard]] UINT64 CompletedFenceValue() const noexcept;
    void ArmFence(UINT64 value);

private:
    void CreateDirect3DResources();
    void CreateDirect2DResources();
    void CreateSwapChain();
    void CreateBackBufferTarget();

    HWND window_ = nullptr;
    UINT surface_width_ = 0;
    UINT surface_height_ = 0;
    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;

    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> context_;
    ComPtr<ID3D11Fence> fence_;
    UINT64 next_fence_value_ = 1;
    UniqueHandle fence_event_;

    ComPtr<IDXGIFactory2> dxgi_factory_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    UniqueHandle frame_waitable_;

    ComPtr<ID2D1Factory3> d2d_factory_;
    ComPtr<ID2D1Device2> d2d_device_;
    ComPtr<ID2D1DeviceContext2> d2d_context_;
    ComPtr<ID2D1Bitmap1> back_buffer_target_;
    bool metrics_enabled_ = false;
    std::uint64_t upload_count_ = 0;
    std::uint64_t upload_nanoseconds_ = 0;
    std::uint64_t draw_count_ = 0;
    std::uint64_t draw_nanoseconds_ = 0;
};

}  // namespace pv
