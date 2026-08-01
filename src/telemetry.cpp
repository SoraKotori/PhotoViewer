#include "telemetry.h"

#include <array>
#include <sstream>
#include <utility>

Telemetry::~Telemetry()
{
    std::scoped_lock lock(mutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool Telemetry::connect(const std::wstring& pipeName)
{
    if (pipeName.empty()) {
        return true;
    }

    const std::wstring fullName = L"\\\\.\\pipe\\" + pipeName;
    if (WaitNamedPipeW(fullName.c_str(), 5000) == 0) {
        return false;
    }

    const HANDLE pipe = CreateFileW(
        fullName.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    pipe_ = pipe;
    return true;
}

void Telemetry::emit(
    const std::string_view event,
    const std::size_t index,
    const std::uint64_t value1,
    const std::uint64_t value2,
    const std::filesystem::path& path)
{
    std::ostringstream output;
    output << nowMicroseconds() << '\t'
           << event << '\t'
           << index << '\t'
           << value1 << '\t'
           << value2 << '\t'
           << sanitize(pathToUtf8(path)) << '\n';
    const std::string line = std::move(output).str();

    std::scoped_lock lock(mutex_);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD bytesWritten = 0;
    if (WriteFile(
            pipe_,
            line.data(),
            static_cast<DWORD>(line.size()),
            &bytesWritten,
            nullptr) == 0 ||
        bytesWritten != line.size()) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

std::uint64_t Telemetry::nowMicroseconds()
{
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<std::uint64_t>(value.QuadPart);
    }();

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    return (ticks / frequency) * 1'000'000ULL + ((ticks % frequency) * 1'000'000ULL) / frequency;
}

std::string Telemetry::pathToUtf8(const std::filesystem::path& path)
{
    if (path.empty()) {
        return {};
    }

    const std::wstring wide = path.wstring();
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        required,
        nullptr,
        nullptr));
    return utf8;
}

std::string Telemetry::sanitize(std::string value)
{
    for (char& character : value) {
        if (character == '\t' || character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    return value;
}
