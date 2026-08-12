#pragma once

#include "win32_support.h"

#include <filesystem>

namespace pv {

class ViewerWindow {
public:
    ViewerWindow() = default;
    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    void Create(HINSTANCE instance, WNDPROC procedure, void* owner);
    void Attach(HWND handle) noexcept { handle_ = handle; }
    void Detach() noexcept { handle_ = nullptr; }

    [[nodiscard]] HWND Handle() const noexcept { return handle_; }
    [[nodiscard]] bool IsFullscreen() const noexcept { return fullscreen_; }

    void SetImageTitle(const std::filesystem::path& image_path) const;
    void ToggleFullscreen();

private:
    HWND handle_ = nullptr;
    bool fullscreen_ = false;
    WINDOWPLACEMENT windowed_placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowed_style_ = 0;
};

}  // namespace pv
