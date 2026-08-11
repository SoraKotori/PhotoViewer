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
    InitializeDevice(window);
    InitializeSurface();
}

void Graphics::InitializeDevice(const HWND window) {
    InitializeDirect3D(window);
    InitializeDirect2D();
}

void Graphics::InitializeDirect3D(const HWND window) {
    window_ = window;
    RECT client{};
    if (!GetClientRect(window_, &client)) ThrowLastError("GetClientRect");
    surface_width_ = std::max<LONG>(1, client.right - client.left);
    surface_height_ = std::max<LONG>(1, client.bottom - client.top);
    CreateDirect3DResources();
}

void Graphics::InitializeDirect2D() {
    if (!device_ || !dxgi_factory_ || d2d_context_) {
        throw std::logic_error("invalid Direct2D initialization");
    }
    CreateDirect2DResources();
}

void Graphics::InitializeSurface() {
    InitializeSwapChain();
    InitializeBackBufferTarget();
}

void Graphics::InitializeSwapChain() {
    if (!window_ || !device_ || !d2d_context_ || swap_chain_) {
        throw std::logic_error("invalid swap-chain initialization");
    }
    CreateSwapChain();
}

void Graphics::InitializeBackBufferTarget() {
    if (!swap_chain_ || back_buffer_target_) {
        throw std::logic_error("invalid back-buffer initialization");
    }
    CreateBackBufferTarget();
}

void Graphics::CreateDirect3DResources() {
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
}

void Graphics::CreateDirect2DResources() {
    ComPtr<IDXGIDevice> dxgi_device;
    CheckHr(device_.As(&dxgi_device), "Query IDXGIDevice for Direct2D");
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

void Graphics::PrepareDecodeStaging(DecodeStaging& staging, const UINT width,
                                    const UINT height) {
    if (!device_) {
        throw std::logic_error("Direct3D device is not initialized");
    }
    if (staging.mapped) {
        throw std::invalid_argument("invalid decode staging preparation");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    const std::size_t extra_rows =
        (static_cast<std::size_t>(height) + row_bytes - 1) / row_bytes;
    UINT texture_width = width;
    UINT texture_height = height;
    if (extra_rows <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION - height) {
        texture_height += static_cast<UINT>(extra_rows);
    } else if (width < D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        ++texture_width;
    } else {
        throw std::invalid_argument("PNG dimensions leave no filter workspace");
    }
    if (!staging.texture || staging.texture_width < texture_width ||
        staging.texture_height < texture_height) {
        staging.texture.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = texture_width;
        description.Height = texture_height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ |
                                     D3D11_CPU_ACCESS_WRITE;
        CheckHr(device_->CreateTexture2D(&description, nullptr, &staging.texture),
                "Create decode staging texture");
        staging.texture_width = texture_width;
        staging.texture_height = texture_height;
    }
}

void Graphics::MapDecodeStaging(DecodeStaging& staging, const UINT width,
                                const UINT height,
                                const std::size_t decoded_bytes) {
    if (staging.mapped || decoded_bytes == 0) {
        throw std::invalid_argument("invalid decode staging map");
    }
    staging.ReleaseCpuAllocation();
    PrepareDecodeStaging(staging, width, height);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHr(context_->Map(staging.texture.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped),
            "Map decode staging texture");
    const std::size_t mapped_bytes = static_cast<std::size_t>(mapped.RowPitch) *
                                     staging.texture_height;
    const std::size_t filtered_bytes = decoded_bytes + height;
    if (!mapped.pData || mapped.RowPitch < row_bytes ||
        mapped_bytes < filtered_bytes) {
        context_->Unmap(staging.texture.Get(), 0);
        throw std::runtime_error("decode staging mapping is too small");
    }
    staging.mapped = true;
    staging.surface.pixels = static_cast<std::byte*>(mapped.pData);
    staging.surface.allocation_bytes = mapped_bytes;
    staging.surface.byte_size = decoded_bytes;
    staging.surface.width = width;
    staging.surface.height = height;
    staging.surface.stride = mapped.RowPitch;
}

void Graphics::UnmapDecodeStaging(DecodeStaging& staging) noexcept {
    if (!staging.mapped || !staging.texture) return;
    context_->Unmap(staging.texture.Get(), 0);
    staging.mapped = false;
    staging.surface.pixels = nullptr;
    staging.surface.allocation_bytes = 0;
}

void Graphics::CopyDecodedToStaging(DecodeStaging& staging) {
    const DecodeSurface source = staging.surface;
    if (!staging.cpu_surface || !source.pixels || source.ByteSize() == 0) {
        throw std::invalid_argument("invalid decoded CPU source");
    }
    PrepareDecodeStaging(staging, source.width, source.height);
    const std::size_t row_bytes = static_cast<std::size_t>(source.width) * 4;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHr(context_->Map(staging.texture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped),
            "Map staging texture for decoded copy");
    if (!mapped.pData || mapped.RowPitch < row_bytes) {
        context_->Unmap(staging.texture.Get(), 0);
        throw std::runtime_error("decode staging row pitch is too small");
    }
    auto* const destination = static_cast<std::byte*>(mapped.pData);
    if (mapped.RowPitch == source.stride) {
        std::memcpy(destination, source.pixels, source.ByteSize());
    } else {
        for (UINT row = 0; row < source.height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                        source.pixels + static_cast<std::size_t>(row) * source.stride,
                        row_bytes);
        }
    }
    context_->Unmap(staging.texture.Get(), 0);
    staging.ReleaseCpuAllocation();
    staging.surface = source;
    staging.surface.pixels = nullptr;
    staging.surface.allocation_bytes = 0;
}

UploadTicket Graphics::SubmitUpload(const std::size_t index,
                                    const std::uint64_t generation,
                                    const SlotId staging_slot,
                                    const DecodeStaging& source,
                                    GpuImage& destination) {
    const auto begin = metrics_enabled_ ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    if (!source.texture || source.surface.ByteSize() == 0) {
        throw std::invalid_argument("invalid decoded staging source");
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = source.surface.width;
    description.Height = source.surface.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    UploadTicket ticket;
    ticket.index = index;
    ticket.generation = generation;
    ticket.staging_slot = staging_slot;
    ticket.bytes = source.surface.ByteSize();
    const bool reusable = destination.texture &&
                          destination.width == source.surface.width &&
                          destination.height == source.surface.height;
    if (!reusable) {
        destination = {};
        CheckHr(device_->CreateTexture2D(&description, nullptr, &destination.texture),
                "Create GPU image texture");
    }

    const D3D11_BOX source_box{0, 0, 0, source.surface.width,
                              source.surface.height, 1};
    context_->CopySubresourceRegion(destination.texture.Get(), 0, 0, 0, 0,
                                    source.texture.Get(), 0, &source_box);
    destination.width = source.surface.width;
    destination.height = source.surface.height;
    destination.bytes = source.surface.ByteSize();
    ticket.fence_value = next_fence_value_++;
    CheckHr(context_->Signal(fence_.Get(), ticket.fence_value),
            "ID3D11DeviceContext4::Signal");
    if (metrics_enabled_) {
        upload_nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        ++upload_count_;
    }
    return ticket;
}

void Graphics::FinishUpload(GpuImage& image) {
    if (image.bitmap) return;
    ComPtr<IDXGISurface> surface;
    CheckHr(image.texture.As(&surface), "Query uploaded IDXGISurface");
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,
                          D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    CheckHr(d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                      &image.bitmap),
            "Create Direct2D GPU Texture bitmap");
}

UINT64 Graphics::Draw(const GpuImage& image) {
    if (!image.bitmap || surface_width_ == 0 || surface_height_ == 0) return 0;
    const auto begin = metrics_enabled_ ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
    const float sx = static_cast<float>(surface_width_) / static_cast<float>(image.width);
    const float sy = static_cast<float>(surface_height_) / static_cast<float>(image.height);
    const float scale = std::min(sx, sy);
    const float width = static_cast<float>(image.width) * scale;
    const float height = static_cast<float>(image.height) * scale;
    const float left = (static_cast<float>(surface_width_) - width) * 0.5f;
    const float top = (static_cast<float>(surface_height_) - height) * 0.5f;
    const D2D1_RECT_F destination = D2D1::RectF(left, top, left + width, top + height);
    const D2D1_RECT_F gpu_texture_rect = D2D1::RectF(
        0.0f, 0.0f, static_cast<float>(image.width),
        static_cast<float>(image.height));
    d2d_context_->BeginDraw();
    d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2d_context_->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    d2d_context_->DrawBitmap(image.bitmap.Get(), destination, 1.0f,
                             D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                             gpu_texture_rect);
    CheckHr(d2d_context_->EndDraw(), "Direct2D EndDraw");
    CheckHr(swap_chain_->Present(1, 0), "DXGI Present");
    const UINT64 completion = next_fence_value_++;
    CheckHr(context_->Signal(fence_.Get(), completion),
            "Signal presented GPU Texture completion");
    if (metrics_enabled_) {
        draw_nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        ++draw_count_;
    }
    return completion;
}

void Graphics::ResetMetrics() noexcept {
    upload_count_ = 0;
    upload_nanoseconds_ = 0;
    draw_count_ = 0;
    draw_nanoseconds_ = 0;
    metrics_enabled_ = true;
}

std::uint64_t Graphics::UploadCount() const noexcept { return upload_count_; }
std::uint64_t Graphics::UploadNanoseconds() const noexcept { return upload_nanoseconds_; }
std::uint64_t Graphics::DrawCount() const noexcept { return draw_count_; }
std::uint64_t Graphics::DrawNanoseconds() const noexcept { return draw_nanoseconds_; }

UINT64 Graphics::CompletedFenceValue() const noexcept {
    return fence_ ? fence_->GetCompletedValue() : 0;
}

void Graphics::ArmFence(const UINT64 value) {
    CheckHr(fence_->SetEventOnCompletion(value, fence_event_),
            "ID3D11Fence::SetEventOnCompletion");
}

}  // namespace pv
