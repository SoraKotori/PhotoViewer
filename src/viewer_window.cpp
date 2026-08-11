#include "viewer_window.h"

namespace pv {
namespace {

constexpr wchar_t kWindowClass[] = L"PhotoViewer.Window";

void SetWindowStyleChecked(const HWND window, const LONG_PTR style) {
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(window, GWL_STYLE, style) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        ThrowLastError("SetWindowLongPtrW(GWL_STYLE)");
    }
}

}  // namespace

void ViewerWindow::Create(const HINSTANCE instance, const WNDPROC procedure,
                          void* const owner) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ThrowLastError("RegisterClassExW");
    }

    handle_ = CreateWindowExW(
        0, kWindowClass, L"PhotoViewer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, instance, owner);
    if (!handle_) ThrowLastError("CreateWindowExW");
}

void ViewerWindow::SetImageTitle(
    const std::filesystem::path& image_path) const {
    if (handle_) SetWindowTextW(handle_, image_path.filename().c_str());
}

void ViewerWindow::ToggleFullscreen() {
    if (!handle_) return;
    if (!fullscreen_) {
        windowed_style_ = GetWindowLongPtrW(handle_, GWL_STYLE);
        windowed_placement_.length = sizeof(windowed_placement_);
        if (!GetWindowPlacement(handle_, &windowed_placement_)) {
            ThrowLastError("GetWindowPlacement");
        }
        const HMONITOR monitor = MonitorFromWindow(
            handle_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) {
            ThrowLastError("GetMonitorInfoW");
        }
        SetWindowStyleChecked(
            handle_, windowed_style_ &
                         ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        fullscreen_ = true;
        if (!SetWindowPos(handle_, HWND_TOP, info.rcMonitor.left,
                          info.rcMonitor.top,
                          info.rcMonitor.right - info.rcMonitor.left,
                          info.rcMonitor.bottom - info.rcMonitor.top,
                          SWP_FRAMECHANGED | SWP_NOOWNERZORDER)) {
            ThrowLastError("SetWindowPos fullscreen");
        }
        return;
    }

    fullscreen_ = false;
    SetWindowStyleChecked(handle_, windowed_style_);
    if (!SetWindowPlacement(handle_, &windowed_placement_)) {
        ThrowLastError("SetWindowPlacement");
    }
    if (!SetWindowPos(handle_, nullptr, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                          SWP_NOOWNERZORDER | SWP_FRAMECHANGED)) {
        ThrowLastError("SetWindowPos restore");
    }
}

}  // namespace pv
