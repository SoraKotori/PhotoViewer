#pragma once

#include "decode_scheduler.h"
#include "direct2d_renderer.h"
#include "navigation_controller.h"
#include "telemetry.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <windows.h>

struct AppOptions
{
    std::filesystem::path initialImage;
    std::filesystem::path catalogManifest;
    std::wstring telemetryPipe;
    std::size_t decodeWorkers{18};
    bool noPersistentState{};
};

class App final
{
public:
    explicit App(Telemetry& telemetry);

    [[nodiscard]] bool initialize(HINSTANCE instance, int showCommand, const AppOptions& options);
    [[nodiscard]] int run();

private:
    [[nodiscard]] static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] bool buildCatalog(
        const std::filesystem::path& initialImage,
        const std::filesystem::path& catalogManifest);
    void synchronizeDecodeDemand();
    void handleImageReady(std::size_t index);
    void handleDecodeFailed(std::size_t index);
    void toggleFullscreen();
    void emitSurfaceMetrics();
    void updateWindowTitle();

    Telemetry& telemetry_;
    Direct2DRenderer renderer_;
    DecodeScheduler decoder_;
    NavigationController navigation_;
    HWND window_{};
    std::vector<std::filesystem::path> catalog_;
    std::size_t initialIndex_{};
    std::uint64_t announcedTargetSequence_{};
    std::uint64_t currentRequestTimeMicroseconds_{};
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowedStyle_{};
    bool rendererReady_{};
    bool fullscreen_{};
};
