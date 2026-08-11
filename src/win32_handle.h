#pragma once

#include <windows.h>

#include <utility>

namespace pv {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.Release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }

    void Reset(const HANDLE replacement = nullptr) noexcept {
        if (handle_ == replacement) return;
        if (IsValid()) CloseHandle(handle_);
        handle_ = replacement;
    }

    [[nodiscard]] HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return IsValid(); }

private:
    HANDLE handle_ = nullptr;
};

}  // namespace pv
