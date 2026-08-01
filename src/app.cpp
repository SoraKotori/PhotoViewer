#include "app.h"

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr wchar_t kWindowClassName[] = L"PhotoViewer.Window";
constexpr wchar_t kWindowTitle[] = L"PhotoViewer";

[[nodiscard]] bool pathsEqualCaseInsensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const std::wstring leftText = std::filesystem::absolute(left).lexically_normal().wstring();
    const std::wstring rightText = std::filesystem::absolute(right).lexically_normal().wstring();
    return _wcsicmp(leftText.c_str(), rightText.c_str()) == 0;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) != required) {
        return {};
    }
    return result;
}
} // namespace

App::App(Telemetry& telemetry)
    : telemetry_(telemetry), decoder_(telemetry)
{
}

bool App::initialize(
    const HINSTANCE instance,
    const int showCommand,
    const AppOptions& options)
{
    if (!buildCatalog(options.initialImage, options.catalogManifest)) {
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    if (RegisterClassExW(&windowClass) == 0) {
        return false;
    }

    window_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        720,
        nullptr,
        nullptr,
        instance,
        this);
    if (window_ == nullptr) {
        return false;
    }

    if (!renderer_.initialize(window_)) {
        return false;
    }
    rendererReady_ = true;
    telemetry_.emit("direct2d_high_quality_cubic");
    emitSurfaceMetrics();

    navigation_.initialize(initialIndex_, catalog_.size());
    decoder_.start(window_, catalog_, initialIndex_, options.decodeWorkers);
    synchronizeDecodeDemand();

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    updateWindowTitle();
    telemetry_.emit("ready", initialIndex_, catalog_.size(), options.decodeWorkers, catalog_.at(initialIndex_));
    return true;
}

int App::run()
{
    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) {
            decoder_.stop();
            telemetry_.emit("message_loop_failed", navigation_.displayedIndex());
            return 1;
        }
        if (result == 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    decoder_.stop();
    telemetry_.emit("exit", navigation_.displayedIndex(), static_cast<std::uint64_t>(message.wParam));
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app != nullptr) {
        return app->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    switch (message) {
    case WM_KEYDOWN:
        {
            const bool repeated =
                (static_cast<ULONG_PTR>(lParam) & (static_cast<ULONG_PTR>(1) << 30U)) != 0;
            if (wParam == VK_F11 && !repeated) {
                toggleFullscreen();
                return 0;
            }
            if (wParam == VK_RIGHT || wParam == VK_LEFT) {
                const int direction = wParam == VK_RIGHT ? 1 : -1;
                static_cast<void>(navigation_.onArrowKeyDown(direction, repeated));
                synchronizeDecodeDemand();
                return 0;
            }
        }
        break;

    case WM_KEYUP:
        if (wParam == VK_RIGHT || wParam == VK_LEFT) {
            const int direction = wParam == VK_RIGHT ? 1 : -1;
            const std::optional<std::size_t> cancelledIndex = navigation_.foregroundTarget();
            if (navigation_.onArrowKeyUp(direction)) {
                telemetry_.emit(
                    "navigation_stopped",
                    navigation_.displayedIndex(),
                    navigation_.targetSequence(),
                    cancelledIndex.value_or(navigation_.displayedIndex()),
                    catalog_.at(navigation_.displayedIndex()));
                updateWindowTitle();
            }
            synchronizeDecodeDemand();
            return 0;
        }
        break;

    case WM_SIZE:
        if (rendererReady_) {
            const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
            const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
            if (!renderer_.resize(width, height)) {
                telemetry_.emit("resize_failed", navigation_.displayedIndex(), width, height);
            } else {
                emitSurfaceMetrics();
            }
        }
        return 0;

    case WM_DPICHANGED:
        {
            RECT target = *reinterpret_cast<const RECT*>(lParam);
            if (fullscreen_) {
                MONITORINFO monitorInfo{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitorInfo) != 0) {
                    target = monitorInfo.rcMonitor;
                }
            }
            SetWindowPos(
                window_,
                nullptr,
                target.left,
                target.top,
                target.right - target.left,
                target.bottom - target.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            if (rendererReady_ && !renderer_.draw()) {
                telemetry_.emit("present_failed", navigation_.displayedIndex());
            }
            EndPaint(window_, &paint);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case kDecodeReadyMessage:
        handleImageReady(static_cast<std::size_t>(wParam));
        return 0;

    case kDecodeFailedMessage:
        handleDecodeFailed(static_cast<std::size_t>(wParam));
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

bool App::buildCatalog(
    const std::filesystem::path& initialImage,
    const std::filesystem::path& catalogManifest)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(initialImage, error) || error) {
        return false;
    }

    if (!catalogManifest.empty()) {
        std::ifstream manifest(catalogManifest, std::ios::binary);
        std::string line;
        while (std::getline(manifest, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::wstring widePath = utf8ToWide(line);
            if (widePath.empty()) {
                return false;
            }
            const std::filesystem::path candidate =
                std::filesystem::absolute(std::filesystem::path(widePath)).lexically_normal();
            const std::wstring extension = candidate.extension().wstring();
            if (!std::filesystem::is_regular_file(candidate, error) || error ||
                _wcsicmp(extension.c_str(), L".png") != 0) {
                return false;
            }
            catalog_.push_back(candidate);
        }
        if (!manifest.eof()) {
            return false;
        }
    } else {
        const std::filesystem::path directory = std::filesystem::absolute(initialImage).parent_path();
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             iterator != end && !error;
             iterator.increment(error)) {
            if (!iterator->is_regular_file(error) || error) {
                continue;
            }
            const std::wstring extension = iterator->path().extension().wstring();
            if (_wcsicmp(extension.c_str(), L".png") == 0) {
                catalog_.push_back(std::filesystem::absolute(iterator->path()).lexically_normal());
            }
        }
    }
    if (error || catalog_.empty()) {
        return false;
    }

    if (catalogManifest.empty()) {
        std::sort(catalog_.begin(), catalog_.end(), [](const auto& left, const auto& right) {
            return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
        });
    }

    const auto selected = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& candidate) {
        return pathsEqualCaseInsensitive(candidate, initialImage);
    });
    if (selected == catalog_.end()) {
        return false;
    }
    initialIndex_ = static_cast<std::size_t>(std::distance(catalog_.begin(), selected));
    return true;
}

void App::synchronizeDecodeDemand()
{
    const std::optional<std::size_t> target = navigation_.foregroundTarget();
    const std::uint64_t sequence = navigation_.targetSequence();
    const bool newTarget = target && sequence != announcedTargetSequence_;
    if (newTarget) {
        announcedTargetSequence_ = sequence;
        currentRequestTimeMicroseconds_ = Telemetry::nowMicroseconds();
        telemetry_.emit(
            "request",
            *target,
            sequence,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(navigation_.prefetchDirection())),
            catalog_.at(*target));
    }
    decoder_.updateDemand(
        navigation_.displayedIndex(),
        target,
        navigation_.prefetchDirection(),
        navigation_.isHolding());
    if (newTarget) {
        const DecodeInventorySnapshot inventory = decoder_.inventory(
            navigation_.displayedIndex(),
            navigation_.prefetchDirection());
        telemetry_.emit("request_inventory", *target, inventory.readyAhead, inventory.cachedImages, catalog_.at(*target));
        telemetry_.emit("decode_activity", *target, inventory.inFlightImages, inventory.desiredImages, catalog_.at(*target));
        telemetry_.emit("decode_cache_bytes", *target, inventory.cacheBytes, 0, catalog_.at(*target));
    }
}

void App::handleImageReady(const std::size_t index)
{
    const std::optional<std::size_t> target = navigation_.foregroundTarget();
    if (!target || *target != index) {
        telemetry_.emit(
            "stale_ready",
            index,
            navigation_.targetSequence(),
            target.value_or(navigation_.displayedIndex()),
            decoder_.pathAt(index));
        return;
    }

    const std::shared_ptr<const DecodedImage> image = decoder_.tryGet(index);
    if (!image) {
        return;
    }

    const std::uint64_t presentedSequence = navigation_.targetSequence();
    std::uint64_t uploadMicroseconds = 0;
    if (!renderer_.setImage(index, *image, uploadMicroseconds) || !renderer_.draw()) {
        telemetry_.emit("upload_or_present_failed", index, uploadMicroseconds, 0, image->path);
        return;
    }

    const std::uint64_t now = Telemetry::nowMicroseconds();
    const std::uint64_t requestToPresent = now >= currentRequestTimeMicroseconds_
        ? now - currentRequestTimeMicroseconds_
        : 0;
    telemetry_.emit("upload", index, uploadMicroseconds, renderer_.bitmapCacheBytes(), image->path);
    telemetry_.emit(
        "texture_dimensions",
        index,
        renderer_.imageWidth(),
        renderer_.imageHeight(),
        image->path);
    telemetry_.emit("present", index, requestToPresent, presentedSequence, image->path);

    if (!navigation_.onPresented(index)) {
        telemetry_.emit("presentation_commit_failed", index, presentedSequence, 0, image->path);
        return;
    }
    updateWindowTitle();
    synchronizeDecodeDemand();
}

void App::handleDecodeFailed(const std::size_t index)
{
    const std::optional<std::size_t> target = navigation_.foregroundTarget();
    if (target && *target == index) {
        telemetry_.emit(
            "foreground_decode_failed",
            index,
            navigation_.targetSequence(),
            0,
            catalog_.at(index));
    }
}

void App::toggleFullscreen()
{
    if (window_ == nullptr) {
        return;
    }

    if (!fullscreen_) {
        MONITORINFO monitorInfo{sizeof(MONITORINFO)};
        windowedPlacement_.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(window_, &windowedPlacement_) == 0 ||
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitorInfo) == 0) {
            return;
        }

        windowedStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
        SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        if (SetWindowPos(
                window_,
                HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED) == 0) {
            SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle_);
            return;
        }
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle_);
        windowedPlacement_.length = sizeof(WINDOWPLACEMENT);
        if (SetWindowPlacement(window_, &windowedPlacement_) == 0) {
            return;
        }
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }

    telemetry_.emit(
        "fullscreen",
        navigation_.displayedIndex(),
        GetDpiForWindow(window_),
        fullscreen_ ? 1U : 0U);
}

void App::emitSurfaceMetrics()
{
    if (window_ == nullptr || !rendererReady_) {
        return;
    }

    RECT client{};
    if (GetClientRect(window_, &client) != 0) {
        telemetry_.emit(
            "client_dimensions",
            GetDpiForWindow(window_),
            static_cast<std::uint64_t>(client.right - client.left),
            static_cast<std::uint64_t>(client.bottom - client.top));
    }
    telemetry_.emit(
        "backbuffer_dimensions",
        GetDpiForWindow(window_),
        renderer_.surfaceWidth(),
        renderer_.surfaceHeight());
}

void App::updateWindowTitle()
{
    if (catalog_.empty() || window_ == nullptr) {
        return;
    }
    const std::size_t index = navigation_.displayedIndex();
    const std::wstring title = L"PhotoViewer — " + catalog_.at(index).filename().wstring() +
        L"  [" + std::to_wstring(index + 1) + L"/" + std::to_wstring(catalog_.size()) + L"]";
    SetWindowTextW(window_, title.c_str());
}
