#include "direct2d_renderer.h"

#include <chrono>
#include <limits>

namespace
{
using Microsoft::WRL::ComPtr;

[[nodiscard]] bool getClientSize(
    const HWND window,
    std::uint32_t& width,
    std::uint32_t& height) noexcept
{
    RECT client{};
    if (GetClientRect(window, &client) == 0) {
        return false;
    }
    width = static_cast<std::uint32_t>(client.right - client.left);
    height = static_cast<std::uint32_t>(client.bottom - client.top);
    return width != 0 && height != 0;
}
} // namespace

bool Direct2DRenderer::initialize(const HWND window)
{
    window_ = window;
    if (!getClientSize(window_, surfaceWidth_, surfaceHeight_)) {
        return false;
    }
    return createDeviceResources() && createSwapChain() && createTargetBitmap();
}

bool Direct2DRenderer::createDeviceResources()
{
    constexpr UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        deviceFlags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3dDevice_,
        nullptr,
        nullptr);
    if (FAILED(result)) {
        return false;
    }

    result = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        d2dFactory_.GetAddressOf());
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    result = d3dDevice_.As(&dxgiDevice);
    if (FAILED(result)) {
        return false;
    }
    result = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(result)) {
        return false;
    }
    result = d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &d2dContext_);
    if (FAILED(result)) {
        return false;
    }

    d2dContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    return true;
}

bool Direct2DRenderer::createSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT result = d3dDevice_.As(&dxgiDevice);
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    result = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    result = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = surfaceWidth_;
    description.Height = surfaceHeight_;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    result = factory->CreateSwapChainForHwnd(
        d3dDevice_.Get(),
        window_,
        &description,
        nullptr,
        nullptr,
        &swapChain_);
    if (FAILED(result)) {
        return false;
    }
    static_cast<void>(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
    return true;
}

bool Direct2DRenderer::createTargetBitmap()
{
    ComPtr<IDXGISurface> surface;
    HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(result)) {
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0F,
        96.0F);
    result = d2dContext_->CreateBitmapFromDxgiSurface(
        surface.Get(),
        &properties,
        &targetBitmap_);
    if (FAILED(result)) {
        return false;
    }
    d2dContext_->SetTarget(targetBitmap_.Get());
    d2dContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    return true;
}

bool Direct2DRenderer::resize(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0 || swapChain_ == nullptr) {
        return true;
    }
    if (width == surfaceWidth_ && height == surfaceHeight_) {
        return true;
    }

    d2dContext_->SetTarget(nullptr);
    targetBitmap_.Reset();
    const HRESULT result = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        return false;
    }
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    return createTargetBitmap();
}

bool Direct2DRenderer::setImage(
    const std::size_t index,
    const DecodedImage& image,
    std::uint64_t& uploadMicroseconds)
{
    const auto start = std::chrono::steady_clock::now();
    auto existing = bitmapCache_.find(index);
    if (existing == bitmapCache_.end()) {
        if (image.width == 0 || image.height == 0 || image.pixels.empty()) {
            return false;
        }
        const std::uint64_t expectedBytes = static_cast<std::uint64_t>(image.width) * image.height * 4ULL;
        if (expectedBytes != image.pixels.size() || image.width > std::numeric_limits<UINT32>::max() / 4U) {
            return false;
        }

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F,
            96.0F);
        BitmapEntry entry{};
        const HRESULT result = d2dContext_->CreateBitmap(
            D2D1::SizeU(image.width, image.height),
            image.pixels.data(),
            image.width * 4U,
            &properties,
            &entry.bitmap);
        if (FAILED(result)) {
            return false;
        }
        entry.width = image.width;
        entry.height = image.height;
        entry.bytes = static_cast<std::size_t>(expectedBytes);
        entry.lastUse = ++useClock_;
        bitmapCacheBytes_ += entry.bytes;
        existing = bitmapCache_.emplace(index, std::move(entry)).first;
    } else {
        existing->second.lastUse = ++useClock_;
    }

    currentIndex_ = index;
    currentBitmap_ = existing->second.bitmap.Get();
    imageWidth_ = existing->second.width;
    imageHeight_ = existing->second.height;
    trimBitmapCache();
    uploadMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    return true;
}

bool Direct2DRenderer::draw()
{
    if (targetBitmap_ == nullptr || surfaceWidth_ == 0 || surfaceHeight_ == 0) {
        return true;
    }

    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0.02F, 0.025F, 0.035F, 1.0F));
    if (currentBitmap_ != nullptr && imageWidth_ != 0 && imageHeight_ != 0) {
        const float widthScale = static_cast<float>(surfaceWidth_) / static_cast<float>(imageWidth_);
        const float heightScale = static_cast<float>(surfaceHeight_) / static_cast<float>(imageHeight_);
        const float scale = widthScale < heightScale ? widthScale : heightScale;
        const float drawWidth = static_cast<float>(imageWidth_) * scale;
        const float drawHeight = static_cast<float>(imageHeight_) * scale;
        const float left = (static_cast<float>(surfaceWidth_) - drawWidth) * 0.5F;
        const float top = (static_cast<float>(surfaceHeight_) - drawHeight) * 0.5F;
        const D2D1_RECT_F destination = D2D1::RectF(
            left,
            top,
            left + drawWidth,
            top + drawHeight);

        d2dContext_->DrawBitmap(
            currentBitmap_,
            &destination,
            1.0F,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
            nullptr,
            nullptr);
    }

    const HRESULT drawResult = d2dContext_->EndDraw();
    if (FAILED(drawResult)) {
        return false;
    }
    return SUCCEEDED(swapChain_->Present(1, 0));
}

void Direct2DRenderer::trimBitmapCache()
{
    while (bitmapCacheBytes_ > bitmapCacheBudgetBytes_ && bitmapCache_.size() > 1) {
        auto victim = bitmapCache_.end();
        for (auto candidate = bitmapCache_.begin(); candidate != bitmapCache_.end(); ++candidate) {
            if (candidate->first == currentIndex_) {
                continue;
            }
            if (victim == bitmapCache_.end() || candidate->second.lastUse < victim->second.lastUse) {
                victim = candidate;
            }
        }
        if (victim == bitmapCache_.end()) {
            break;
        }
        bitmapCacheBytes_ -= victim->second.bytes;
        bitmapCache_.erase(victim);
    }
}
