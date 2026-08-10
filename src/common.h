#pragma once

#include <windows.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pv {

using Microsoft::WRL::ComPtr;

inline void CheckHr(const HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

[[noreturn]] inline void ThrowLastError(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (Win32 " +
                             std::to_string(GetLastError()) + ")");
}

constexpr std::size_t MiB(const std::size_t value) noexcept {
    return value * 1024ULL * 1024ULL;
}

constexpr UINT kMessageValidationStep = WM_APP + 3;
constexpr std::size_t kMaxDecoderWorkers = 256;

}  // namespace pv
