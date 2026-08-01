#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>

class Telemetry
{
public:
    Telemetry() = default;
    ~Telemetry();

    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    [[nodiscard]] bool connect(const std::wstring& pipeName);
    void emit(
        std::string_view event,
        std::size_t index = 0,
        std::uint64_t value1 = 0,
        std::uint64_t value2 = 0,
        const std::filesystem::path& path = {});

    [[nodiscard]] static std::uint64_t nowMicroseconds();

private:
    [[nodiscard]] static std::string pathToUtf8(const std::filesystem::path& path);
    [[nodiscard]] static std::string sanitize(std::string value);

    HANDLE pipe_{INVALID_HANDLE_VALUE};
    std::mutex mutex_;
};
