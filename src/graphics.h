#pragma once

#include "model.h"

namespace pv {

class Graphics {
public:
    Graphics() = default;
    ~Graphics();

    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    void Initialize(HWND window);
    void Resize(UINT width, UINT height);

    UploadTicket SubmitUpload(std::size_t index, std::uint64_t generation,
                              std::unique_ptr<CpuSurface> source);
    GpuImage FinishUpload(UploadTicket& ticket);
    void Draw(const GpuImage& image);

    [[nodiscard]] HANDLE FrameWaitableObject() const noexcept { return frame_waitable_; }
    [[nodiscard]] HANDLE FenceEvent() const noexcept { return fence_event_; }
    [[nodiscard]] UINT64 CompletedFenceValue() const noexcept;
    void ArmFence(UINT64 value);

private:
    void CreateDeviceResources();
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
    HANDLE fence_event_ = nullptr;

    ComPtr<IDXGIFactory2> dxgi_factory_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    HANDLE frame_waitable_ = nullptr;

    ComPtr<ID2D1Factory3> d2d_factory_;
    ComPtr<ID2D1Device2> d2d_device_;
    ComPtr<ID2D1DeviceContext2> d2d_context_;
    ComPtr<ID2D1Bitmap1> back_buffer_target_;
};

}  // namespace pv
