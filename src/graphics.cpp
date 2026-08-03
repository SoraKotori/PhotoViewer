#include "graphics.h"

#include "common.h"

#include <cstring>

namespace pv {

Graphics::~Graphics() {
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    if (frame_waitable_) CloseHandle(frame_waitable_);
    if (fence_event_) CloseHandle(fence_event_);
}

void Graphics::Initialize(const HWND window) {
    window_ = window;
    RECT client{};
    if (!GetClientRect(window_, &client)) ThrowLastError("GetClientRect");
    surface_width_ = std::max<LONG>(1, client.right - client.left);
    surface_height_ = std::max<LONG>(1, client.bottom - client.top);
    CreateDeviceResources();
    CreateSwapChain();
    CreateBackBufferTarget();
}

void Graphics::CreateDeviceResources() {
    constexpr D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> base_device;
    ComPtr<ID3D11DeviceContext> base_context;
    CheckHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
                              static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
                              &base_device, &feature_level_, &base_context),
            "D3D11CreateDevice");
    if (feature_level_ < D3D_FEATURE_LEVEL_11_1) {
        throw std::runtime_error("Direct3D Feature Level 11_1 is required");
    }
    CheckHr(base_device.As(&device_), "Query ID3D11Device5");
    CheckHr(base_context.As(&context_), "Query ID3D11DeviceContext4");
    CheckHr(device_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
            "ID3D11Device5::CreateFence");
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) ThrowLastError("CreateEvent fence");

    ComPtr<IDXGIDevice> dxgi_device;
    CheckHr(device_.As(&dxgi_device), "Query IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(dxgi_device->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");
    CheckHr(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory_)), "Get DXGI factory");

    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    CheckHr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                              __uuidof(ID2D1Factory3), &options,
                              reinterpret_cast<void**>(d2d_factory_.GetAddressOf())),
            "D2D1CreateFactory");
    ComPtr<ID2D1Device> base_d2d_device;
    CheckHr(d2d_factory_->CreateDevice(dxgi_device.Get(), &base_d2d_device),
            "ID2D1Factory3::CreateDevice");
    CheckHr(base_d2d_device.As(&d2d_device_), "Query ID2D1Device2");
    ComPtr<ID2D1DeviceContext> base_d2d_context;
    CheckHr(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                             &base_d2d_context),
            "Create Direct2D context");
    CheckHr(base_d2d_context.As(&d2d_context_), "Query ID2D1DeviceContext2");
    d2d_context_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    d2d_context_->SetDpi(96.0f, 96.0f);
}

void Graphics::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = surface_width_;
    description.Height = surface_height_;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> base_swap_chain;
    CheckHr(dxgi_factory_->CreateSwapChainForHwnd(device_.Get(), window_, &description,
                                                   nullptr, nullptr, &base_swap_chain),
            "CreateSwapChainForHwnd");
    CheckHr(base_swap_chain.As(&swap_chain_), "Query IDXGISwapChain2");
    CheckHr(swap_chain_->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
    frame_waitable_ = swap_chain_->GetFrameLatencyWaitableObject();
    if (!frame_waitable_) ThrowLastError("GetFrameLatencyWaitableObject");
    dxgi_factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
}

void Graphics::CreateBackBufferTarget() {
    ComPtr<IDXGISurface> surface;
    CheckHr(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "Get swap-chain buffer");
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    CheckHr(d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                      &back_buffer_target_),
            "Create Direct2D back-buffer target");
    d2d_context_->SetTarget(back_buffer_target_.Get());
}

void Graphics::Resize(const UINT width, const UINT height) {
    if (!swap_chain_ || width == 0 || height == 0 ||
        (width == surface_width_ && height == surface_height_)) {
        return;
    }
    d2d_context_->SetTarget(nullptr);
    back_buffer_target_.Reset();
    CheckHr(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
            "ResizeBuffers");
    surface_width_ = width;
    surface_height_ = height;
    CreateBackBufferTarget();
}

UploadTicket Graphics::SubmitUpload(const std::size_t index,
                                    const std::uint64_t generation,
                                    std::unique_ptr<CpuSurface> source) {
    if (!source || !source->pixels || source->ByteSize() == 0) {
        throw std::invalid_argument("empty CPU surface");
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = source->width;
    description.Height = source->height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    UploadTicket ticket;
    ticket.index = index;
    ticket.generation = generation;
    ticket.width = source->width;
    ticket.height = source->height;
    ticket.bytes = source->ByteSize();
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = source->pixels;
    initial.SysMemPitch = source->stride;
    initial.SysMemSlicePitch = static_cast<UINT>(source->ByteSize());
    CheckHr(device_->CreateTexture2D(&description, &initial, &ticket.texture),
            "Create initialized GPU image texture");
    ticket.fence_value = next_fence_value_++;
    CheckHr(context_->Signal(fence_.Get(), ticket.fence_value),
            "ID3D11DeviceContext4::Signal");
    ticket.source = std::move(source);
    return ticket;
}

GpuImage Graphics::FinishUpload(UploadTicket& ticket) {
    GpuImage image;
    image.texture = std::move(ticket.texture);
    image.width = ticket.width;
    image.height = ticket.height;
    image.bytes = ticket.bytes;
    ComPtr<IDXGISurface> surface;
    CheckHr(image.texture.As(&surface), "Query uploaded IDXGISurface");
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    CheckHr(d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                      &image.bitmap),
            "Create Direct2D source bitmap");
    ticket.staging.Reset();
    return image;
}

void Graphics::Draw(const GpuImage& image) {
    if (!image.bitmap || surface_width_ == 0 || surface_height_ == 0) return;
    const float sx = static_cast<float>(surface_width_) / static_cast<float>(image.width);
    const float sy = static_cast<float>(surface_height_) / static_cast<float>(image.height);
    const float scale = std::min(sx, sy);
    const float width = static_cast<float>(image.width) * scale;
    const float height = static_cast<float>(image.height) * scale;
    const float left = (static_cast<float>(surface_width_) - width) * 0.5f;
    const float top = (static_cast<float>(surface_height_) - height) * 0.5f;
    const D2D1_RECT_F destination = D2D1::RectF(left, top, left + width, top + height);
    const D2D1_RECT_F source = D2D1::RectF(0.0f, 0.0f,
                                           static_cast<float>(image.width),
                                           static_cast<float>(image.height));
    d2d_context_->BeginDraw();
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d_context_->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    d2d_context_->DrawBitmap(image.bitmap.Get(), destination, 1.0f,
                             D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, source);
    CheckHr(d2d_context_->EndDraw(), "Direct2D EndDraw");
    CheckHr(swap_chain_->Present(1, 0), "DXGI Present");
}

UINT64 Graphics::CompletedFenceValue() const noexcept {
    return fence_ ? fence_->GetCompletedValue() : 0;
}

void Graphics::ArmFence(const UINT64 value) {
    CheckHr(fence_->SetEventOnCompletion(value, fence_event_),
            "ID3D11Fence::SetEventOnCompletion");
}

}  // namespace pv
