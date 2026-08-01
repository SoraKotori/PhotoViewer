#pragma once

#include "decoded_image.h"

#include <cstddef>
#include <cstdint>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <unordered_map>
#include <windows.h>
#include <wrl/client.h>

class Direct2DRenderer final
{
public:
    [[nodiscard]] bool initialize(HWND window);
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool setImage(
        std::size_t index,
        const DecodedImage& image,
        std::uint64_t& uploadMicroseconds);
    [[nodiscard]] bool draw();

    [[nodiscard]] std::size_t bitmapCacheBytes() const noexcept { return bitmapCacheBytes_; }
    [[nodiscard]] std::uint32_t imageWidth() const noexcept { return imageWidth_; }
    [[nodiscard]] std::uint32_t imageHeight() const noexcept { return imageHeight_; }
    [[nodiscard]] std::uint32_t surfaceWidth() const noexcept { return surfaceWidth_; }
    [[nodiscard]] std::uint32_t surfaceHeight() const noexcept { return surfaceHeight_; }

private:
    struct BitmapEntry
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t bytes{};
        std::uint64_t lastUse{};
    };

    [[nodiscard]] bool createDeviceResources();
    [[nodiscard]] bool createSwapChain();
    [[nodiscard]] bool createTargetBitmap();
    void trimBitmapCache();

    HWND window_{};
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    std::unordered_map<std::size_t, BitmapEntry> bitmapCache_;
    ID2D1Bitmap1* currentBitmap_{};
    std::size_t currentIndex_{static_cast<std::size_t>(-1)};
    std::size_t bitmapCacheBytes_{};
    std::size_t bitmapCacheBudgetBytes_{768ULL * 1024ULL * 1024ULL};
    std::uint64_t useClock_{};
    std::uint32_t imageWidth_{};
    std::uint32_t imageHeight_{};
    std::uint32_t surfaceWidth_{};
    std::uint32_t surfaceHeight_{};
};
