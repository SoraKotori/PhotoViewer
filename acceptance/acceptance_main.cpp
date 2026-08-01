#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr wchar_t kViewerWindowClass[] = L"PhotoViewer.Window";

struct Options
{
    std::filesystem::path app;
    std::filesystem::path source;
    std::filesystem::path initial;
    std::filesystem::path resultsRoot;
    std::wstring scenario{L"development"};
    std::uint32_t durationSeconds{10};
    std::uint32_t intervalMilliseconds{100};
    std::uint32_t heldSamples{60};
    std::uint32_t heldInitialDelayMilliseconds{250};
    std::uint32_t heldRepeatIntervalMilliseconds{33};
    std::uint32_t decodeWorkers{18};
    bool homogeneousCatalog{};
    bool requireInventoryExhaustion{};
    bool durationSpecified{};
    bool verifySha256{};
};

struct PngDescriptor
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint8_t bitDepth{};
    std::uint8_t colorType{};
    std::uint8_t compressionMethod{};
    std::uint8_t filterMethod{};
    std::uint8_t interlaceMethod{};
};

struct FileRecord
{
    std::filesystem::path path;
    std::uint64_t size{};
    std::int64_t lastWriteTicks{};
    std::string sha256;
};

struct Event
{
    std::uint64_t timestampMicroseconds{};
    std::string name;
    std::size_t index{};
    std::uint64_t value1{};
    std::uint64_t value2{};
    std::string path;
};

class UniqueHandle
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle)
        : handle_(handle)
    {
    }
    ~UniqueHandle()
    {
        reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.release())
    {
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() noexcept
    {
        const HANDLE result = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }
    void reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept
    {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

void waitHighResolutionMilliseconds(const std::uint32_t milliseconds)
{
    if (milliseconds == 0) {
        return;
    }
    UniqueHandle timer(CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE));
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10'000LL;
    if (timer.valid() && SetWaitableTimerEx(
            timer.get(),
            &due,
            0,
            nullptr,
            nullptr,
            nullptr,
            0) != 0 &&
        WaitForSingleObject(timer.get(), INFINITE) == WAIT_OBJECT_0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

class Sha256Provider
{
public:
    Sha256Provider()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }
        DWORD resultSize = 0;
        if (BCryptGetProperty(
                algorithm_,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength_),
                sizeof(objectLength_),
                &resultSize,
                0) < 0 ||
            BCryptGetProperty(
                algorithm_,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashLength_),
                sizeof(hashLength_),
                &resultSize,
                0) < 0) {
            throw std::runtime_error("BCryptGetProperty failed");
        }
    }

    ~Sha256Provider()
    {
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    [[nodiscard]] std::string hashFile(const std::filesystem::path& path) const
    {
        UniqueHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!file.valid()) {
            throw std::runtime_error("Unable to open source file for hashing");
        }

        std::vector<UCHAR> hashObject(objectLength_);
        std::vector<UCHAR> hash(hashLength_);
        BCRYPT_HASH_HANDLE hashHandle = nullptr;
        if (BCryptCreateHash(
                algorithm_,
                &hashHandle,
                hashObject.data(),
                static_cast<ULONG>(hashObject.size()),
                nullptr,
                0,
                0) < 0) {
            throw std::runtime_error("BCryptCreateHash failed");
        }

        try {
            std::vector<UCHAR> buffer(1024 * 1024);
            while (true) {
                DWORD bytesRead = 0;
                if (ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) == 0) {
                    throw std::runtime_error("ReadFile failed during hashing");
                }
                if (bytesRead == 0) {
                    break;
                }
                if (BCryptHashData(hashHandle, buffer.data(), bytesRead, 0) < 0) {
                    throw std::runtime_error("BCryptHashData failed");
                }
            }
            if (BCryptFinishHash(hashHandle, hash.data(), static_cast<ULONG>(hash.size()), 0) < 0) {
                throw std::runtime_error("BCryptFinishHash failed");
            }
        } catch (...) {
            BCryptDestroyHash(hashHandle);
            throw;
        }
        BCryptDestroyHash(hashHandle);

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const UCHAR value : hash) {
            output << std::setw(2) << static_cast<unsigned int>(value);
        }
        return std::move(output).str();
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    DWORD objectLength_{};
    DWORD hashLength_{};
};

class EventCollector
{
public:
    EventCollector(const HANDLE pipe, std::filesystem::path outputPath)
        : pipe_(pipe), outputPath_(std::move(outputPath)), reader_(&EventCollector::readerLoop, this)
    {
    }

    ~EventCollector()
    {
        join();
    }

    EventCollector(const EventCollector&) = delete;
    EventCollector& operator=(const EventCollector&) = delete;

    void join()
    {
        if (reader_.joinable()) {
            reader_.join();
        }
        pipe_.reset();
    }

    [[nodiscard]] std::optional<Event> waitFor(
        const std::string_view name,
        const std::uint64_t value2,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto predicate = [&] {
            return finished_ || std::any_of(events_.begin(), events_.end(), [&](const Event& event) {
                return event.name == name && event.value2 == value2;
            });
        };
        if (!condition_.wait_for(lock, timeout, predicate)) {
            return std::nullopt;
        }
        const auto found = std::find_if(events_.begin(), events_.end(), [&](const Event& event) {
            return event.name == name && event.value2 == value2;
        });
        if (found == events_.end()) {
            return std::nullopt;
        }
        return *found;
    }

    [[nodiscard]] std::optional<Event> waitForName(
        const std::string_view name,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto predicate = [&] {
            return finished_ || std::any_of(events_.begin(), events_.end(), [&](const Event& event) {
                return event.name == name;
            });
        };
        if (!condition_.wait_for(lock, timeout, predicate)) {
            return std::nullopt;
        }
        const auto found = std::find_if(events_.begin(), events_.end(), [&](const Event& event) {
            return event.name == name;
        });
        return found == events_.end() ? std::nullopt : std::optional<Event>(*found);
    }

    [[nodiscard]] std::vector<Event> snapshot() const
    {
        std::scoped_lock lock(mutex_);
        return events_;
    }

private:
    [[nodiscard]] static std::optional<Event> parse(const std::string& line)
    {
        std::array<std::string, 6> fields{};
        std::size_t start = 0;
        for (std::size_t index = 0; index < fields.size() - 1; ++index) {
            const std::size_t separator = line.find('\t', start);
            if (separator == std::string::npos) {
                return std::nullopt;
            }
            fields[index] = line.substr(start, separator - start);
            start = separator + 1;
        }
        fields.back() = line.substr(start);

        try {
            Event event{};
            event.timestampMicroseconds = std::stoull(fields[0]);
            event.name = fields[1];
            event.index = static_cast<std::size_t>(std::stoull(fields[2]));
            event.value1 = std::stoull(fields[3]);
            event.value2 = std::stoull(fields[4]);
            event.path = fields[5];
            return event;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    void readerLoop()
    {
        std::ofstream output(outputPath_, std::ios::binary | std::ios::trunc);
        std::string pending;
        std::array<char, 4096> buffer{};
        while (true) {
            DWORD bytesRead = 0;
            if (ReadFile(pipe_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) == 0 || bytesRead == 0) {
                break;
            }
            pending.append(buffer.data(), bytesRead);
            while (true) {
                const std::size_t newline = pending.find('\n');
                if (newline == std::string::npos) {
                    break;
                }
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                output << line << '\n';
                if (const auto event = parse(line)) {
                    {
                        std::scoped_lock lock(mutex_);
                        events_.push_back(*event);
                    }
                    condition_.notify_all();
                }
            }
        }
        output.flush();
        {
            std::scoped_lock lock(mutex_);
            finished_ = true;
        }
        condition_.notify_all();
    }

    UniqueHandle pipe_;
    std::filesystem::path outputPath_;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Event> events_;
    bool finished_{};
};

class ChildProcessGuard
{
public:
    explicit ChildProcessGuard(const HANDLE process)
        : process_(process)
    {
    }

    ~ChildProcessGuard()
    {
        if (active_ && process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 50);
            WaitForSingleObject(process_, 5000);
        }
    }

    ChildProcessGuard(const ChildProcessGuard&) = delete;
    ChildProcessGuard& operator=(const ChildProcessGuard&) = delete;

    void disarm() noexcept { active_ = false; }

private:
    HANDLE process_{};
    bool active_{true};
};

[[nodiscard]] std::wstring quoteArgument(const std::filesystem::path& value)
{
    return L"\"" + value.wstring() + L"\"";
}

[[nodiscard]] std::wstring quoteArgument(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

[[nodiscard]] std::wstring lowerCase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] bool isWithin(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    const auto normalizedChild = std::filesystem::absolute(child).lexically_normal();
    const auto normalizedParent = std::filesystem::absolute(parent).lexically_normal();
    auto childIterator = normalizedChild.begin();
    for (auto parentIterator = normalizedParent.begin(); parentIterator != normalizedParent.end(); ++parentIterator, ++childIterator) {
        if (childIterator == normalizedChild.end() ||
            lowerCase(childIterator->wstring()) != lowerCase(parentIterator->wstring())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string narrowUtf8(const std::wstring& value)
{
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr));
    return result;
}

[[nodiscard]] std::string jsonEscape(const std::string_view value)
{
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(character); break;
        }
    }
    return output;
}

void writeText(const std::filesystem::path& path, const std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to write acceptance output");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("Unable to finish acceptance output");
    }
}

[[nodiscard]] std::filesystem::path createRunDirectory(const std::filesystem::path& root)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream name;
    name << L"run-" << std::setfill(L'0')
         << std::setw(4) << time.wYear
         << std::setw(2) << time.wMonth
         << std::setw(2) << time.wDay << L'-'
         << std::setw(2) << time.wHour
         << std::setw(2) << time.wMinute
         << std::setw(2) << time.wSecond << L'-'
         << GetCurrentProcessId();
    const std::filesystem::path result = root / name.str();
    if (!std::filesystem::create_directories(result)) {
        throw std::runtime_error("Unable to create a unique result directory");
    }
    return result;
}

[[nodiscard]] std::vector<FileRecord> collectManifest(
    const std::filesystem::path& source,
    const bool includeHashes)
{
    std::vector<FileRecord> files;
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::wstring extension = entry.path().extension().wstring();
        if (_wcsicmp(extension.c_str(), L".png") != 0) {
            continue;
        }
        FileRecord record{};
        record.path = std::filesystem::absolute(entry.path()).lexically_normal();
        record.size = entry.file_size();
        record.lastWriteTicks = entry.last_write_time().time_since_epoch().count();
        files.push_back(std::move(record));
    }
    std::sort(files.begin(), files.end(), [](const FileRecord& left, const FileRecord& right) {
        const std::wstring leftName = left.path.filename().wstring();
        const std::wstring rightName = right.path.filename().wstring();
        return _wcsicmp(leftName.c_str(), rightName.c_str()) < 0;
    });

    if (includeHashes) {
        Sha256Provider provider;
        std::size_t index = 0;
        for (FileRecord& file : files) {
            file.sha256 = provider.hashFile(file.path);
            ++index;
            if (index % 25 == 0 || index == files.size()) {
                std::wcout << L"Hashed " << index << L"/" << files.size() << L" source files\n";
                std::wcout.flush();
            }
        }
    }
    return files;
}

void writeManifest(const std::filesystem::path& path, const std::vector<FileRecord>& records)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "name,size,last_write_ticks,sha256\n";
    for (const FileRecord& record : records) {
        const std::string name = narrowUtf8(record.path.filename().wstring());
        output << '"' << jsonEscape(name) << "\"," << record.size << ',' << record.lastWriteTicks << ',' << record.sha256 << '\n';
    }
}

[[nodiscard]] bool manifestsEqual(
    const std::vector<FileRecord>& before,
    const std::vector<FileRecord>& after,
    const bool compareHashes)
{
    if (before.size() != after.size()) {
        return false;
    }
    for (std::size_t index = 0; index < before.size(); ++index) {
        if (lowerCase(before[index].path.filename().wstring()) != lowerCase(after[index].path.filename().wstring()) ||
            before[index].size != after[index].size ||
            before[index].lastWriteTicks != after[index].lastWriteTicks ||
            (compareHashes && before[index].sha256 != after[index].sha256)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Options parseOptions(const int argumentCount, wchar_t** arguments)
{
    Options options{};
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument = arguments[index];
        const auto requireValue = [&]() -> const wchar_t* {
            if (index + 1 >= argumentCount) {
                throw std::runtime_error("Missing option value");
            }
            return arguments[++index];
        };
        if (argument == L"--app") {
            options.app = requireValue();
        } else if (argument == L"--source") {
            options.source = requireValue();
        } else if (argument == L"--initial") {
            options.initial = requireValue();
        } else if (argument == L"--results") {
            options.resultsRoot = requireValue();
        } else if (argument == L"--scenario") {
            options.scenario = requireValue();
        } else if (argument == L"--duration") {
            options.durationSeconds = static_cast<std::uint32_t>(std::stoul(requireValue()));
            options.durationSpecified = true;
        } else if (argument == L"--interval") {
            options.intervalMilliseconds = static_cast<std::uint32_t>(std::stoul(requireValue()));
        } else if (argument == L"--held-samples") {
            options.heldSamples = static_cast<std::uint32_t>(std::stoul(requireValue()));
        } else if (argument == L"--held-initial-delay") {
            options.heldInitialDelayMilliseconds =
                static_cast<std::uint32_t>(std::stoul(requireValue()));
        } else if (argument == L"--held-repeat-interval") {
            options.heldRepeatIntervalMilliseconds =
                static_cast<std::uint32_t>(std::stoul(requireValue()));
        } else if (argument == L"--decode-workers") {
            options.decodeWorkers = static_cast<std::uint32_t>(std::stoul(requireValue()));
        } else if (argument == L"--homogeneous-catalog") {
            options.homogeneousCatalog = true;
        } else if (argument == L"--require-inventory-exhaustion") {
            options.requireInventoryExhaustion = true;
        } else if (argument == L"--verify-source-sha256") {
            options.verifySha256 = true;
        } else {
            throw std::runtime_error("Unknown command-line option");
        }
    }

    if (options.scenario == L"final" && !options.durationSpecified) {
        options.durationSeconds = 300;
        options.verifySha256 = true;
    }
    if (options.app.empty() || options.source.empty() || options.initial.empty() || options.resultsRoot.empty() ||
        options.durationSeconds == 0 || options.intervalMilliseconds == 0 || options.heldSamples < 2 ||
        options.heldInitialDelayMilliseconds > 5000 ||
        options.heldRepeatIntervalMilliseconds == 0 || options.heldRepeatIntervalMilliseconds > 1000 ||
        options.decodeWorkers == 0 || options.decodeWorkers > 32) {
        throw std::runtime_error("Required option missing or invalid");
    }
    if (options.requireInventoryExhaustion && !options.homogeneousCatalog) {
        throw std::runtime_error("--require-inventory-exhaustion requires --homogeneous-catalog");
    }
    return options;
}

struct WindowSearch
{
    DWORD processId{};
    HWND window{};
};

BOOL CALLBACK enumerateWindow(const HWND window, const LPARAM parameter)
{
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId) {
        return TRUE;
    }
    std::array<wchar_t, 128> className{};
    if (GetClassNameW(window, className.data(), static_cast<int>(className.size())) > 0 &&
        std::wstring_view(className.data()) == kViewerWindowClass) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

[[nodiscard]] HWND waitForViewerWindow(const DWORD processId, const HANDLE process)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        WindowSearch search{processId, nullptr};
        EnumWindows(enumerateWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window != nullptr) {
            return search.window;
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

[[nodiscard]] bool postKey(const HWND window, const WPARAM key)
{
    return PostMessageW(window, WM_KEYDOWN, key, 0) != 0 &&
        PostMessageW(window, WM_KEYUP, key, 0) != 0;
}

[[nodiscard]] bool postHeldKeyDown(const HWND window, const WPARAM key, const bool repeated)
{
    const LPARAM state = repeated ? static_cast<LPARAM>(1ULL << 30U) : 0;
    return PostMessageW(window, WM_KEYDOWN, key, state) != 0;
}

[[nodiscard]] bool postHeldKeyUp(const HWND window, const WPARAM key)
{
    constexpr LPARAM releasedState = static_cast<LPARAM>((1ULL << 30U) | (1ULL << 31U));
    return PostMessageW(window, WM_KEYUP, key, releasedState) != 0;
}

[[nodiscard]] bool postArrow(const HWND window, const int direction)
{
    return postKey(window, direction > 0 ? VK_RIGHT : VK_LEFT);
}

[[nodiscard]] PngDescriptor readPngDescriptor(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 29> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        !std::equal(signature.begin(), signature.end(), header.begin()) ||
        header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') {
        throw std::runtime_error("Initial file does not contain a valid PNG IHDR");
    }

    const auto bigEndian = [&](const std::size_t offset) {
        return (static_cast<std::uint32_t>(header[offset]) << 24U) |
            (static_cast<std::uint32_t>(header[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(header[offset + 2]) << 8U) |
            static_cast<std::uint32_t>(header[offset + 3]);
    };
    const std::uint32_t width = bigEndian(16);
    const std::uint32_t height = bigEndian(20);
    if (width == 0 || height == 0) {
        throw std::runtime_error("Initial PNG has invalid zero dimensions");
    }
    return PngDescriptor{
        width,
        height,
        header[24],
        header[25],
        header[26],
        header[27],
        header[28]};
}

[[nodiscard]] bool samePngEncoding(const PngDescriptor& left, const PngDescriptor& right)
{
    return left.width == right.width &&
        left.height == right.height &&
        left.bitDepth == right.bitDepth &&
        left.colorType == right.colorType &&
        left.compressionMethod == right.compressionMethod &&
        left.filterMethod == right.filterMethod &&
        left.interlaceMethod == right.interlaceMethod;
}

void writeHeldSampleManifest(
    const std::filesystem::path& path,
    const std::vector<FileRecord>& files,
    const std::vector<std::size_t>& indexes,
    const PngDescriptor& expected)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "ordinal,catalog_index,name,compressed_bytes,last_write_ticks,width,height,bit_depth,color_type,compression,filter,interlace\n";
    for (std::size_t ordinal = 0; ordinal < indexes.size(); ++ordinal) {
        const std::size_t index = indexes[ordinal];
        const FileRecord& file = files.at(index);
        const PngDescriptor descriptor = readPngDescriptor(file.path);
        if (!samePngEncoding(descriptor, expected)) {
            throw std::runtime_error("Held sample is not homogeneous in PNG dimensions and encoding");
        }
        output << ordinal << ','
               << index << ','
               << '"' << jsonEscape(narrowUtf8(file.path.filename().wstring())) << "\","
               << file.size << ','
               << file.lastWriteTicks << ','
               << descriptor.width << ','
               << descriptor.height << ','
               << static_cast<unsigned int>(descriptor.bitDepth) << ','
               << static_cast<unsigned int>(descriptor.colorType) << ','
               << static_cast<unsigned int>(descriptor.compressionMethod) << ','
               << static_cast<unsigned int>(descriptor.filterMethod) << ','
               << static_cast<unsigned int>(descriptor.interlaceMethod) << '\n';
    }
}

[[nodiscard]] std::uint64_t percentile(std::vector<std::uint64_t> values, const double fraction)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(fraction * static_cast<double>(values.size()));
    const std::size_t index = rank <= 1.0
        ? 0
        : std::min(values.size() - 1, static_cast<std::size_t>(rank - 1.0));
    return values[index];
}

[[nodiscard]] std::uint64_t privateBytes(const HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) == 0) {
        return 0;
    }
    return counters.PrivateUsage;
}

void writeMemorySamples(
    const std::filesystem::path& path,
    const std::vector<std::uint64_t>& samples)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "sample,private_bytes\n";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        output << index << ',' << samples[index] << '\n';
    }
}

[[nodiscard]] std::size_t writeHeldIntervals(
    const std::filesystem::path& path,
    const std::vector<Event>& presents,
    const std::vector<Event>& events)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "transition,from_index,to_index,from_timestamp_us,to_timestamp_us,interval_us,request_to_present_us,target_sequence,ready_ahead,cached_images,in_flight,desired_images\n";
    std::size_t inventoryEmptySamples = 0;
    for (std::size_t index = 1; index < presents.size(); ++index) {
        const Event& previous = presents[index - 1];
        const Event& current = presents[index];
        const auto inventory = std::find_if(events.rbegin(), events.rend(), [&](const Event& event) {
            return event.name == "request_inventory" && event.index == current.index &&
                event.timestampMicroseconds <= current.timestampMicroseconds;
        });
        const auto activity = std::find_if(events.rbegin(), events.rend(), [&](const Event& event) {
            return event.name == "decode_activity" && event.index == current.index &&
                event.timestampMicroseconds <= current.timestampMicroseconds;
        });
        if (inventory != events.rend() && inventory->value1 == 0) {
            ++inventoryEmptySamples;
        }
        output << index << ','
               << previous.index << ','
               << current.index << ','
               << previous.timestampMicroseconds << ','
               << current.timestampMicroseconds << ','
               << current.timestampMicroseconds - previous.timestampMicroseconds << ','
               << current.value1 << ','
               << current.value2 << ','
               << (inventory == events.rend() ? 0 : inventory->value1) << ','
               << (inventory == events.rend() ? 0 : inventory->value2) << ','
               << (activity == events.rend() ? 0 : activity->value1) << ','
               << (activity == events.rend() ? 0 : activity->value2) << '\n';
    }
    return inventoryEmptySamples;
}

[[nodiscard]] std::string buildSummary(
    const bool passed,
    const Options& options,
    const std::size_t sourceFiles,
    const std::size_t sent,
    const std::size_t requests,
    const std::size_t presents,
    const std::size_t decodedUnique,
    const std::size_t presentedUnique,
    const std::size_t decodeFailures,
    const std::size_t wrongRequests,
    const std::uint64_t p50,
    const std::uint64_t p95,
    const std::uint64_t p99,
    const std::uint64_t maximum,
    const std::uint64_t initialLatency,
    const double achievedRate,
    const std::uint64_t peakPrivate,
    const std::uint64_t plateauGrowth,
    const std::uint64_t maxGpuCache,
    const std::size_t heldRepeatPosts,
    const std::uint64_t heldInitialGap,
    const std::size_t heldSteadyIntervalCount,
    const double heldSteadyIntervalMean,
    const std::uint64_t heldSteadyIntervalP50,
    const std::uint64_t heldSteadyIntervalP95,
    const std::uint64_t heldSteadyIntervalMaximum,
    const std::size_t heldInventoryEmptySamples,
    const bool sourceUnchanged,
    const std::string_view reason)
{
    std::ostringstream output;
    output << std::boolalpha << "{\n"
           << "  \"pass\": " << passed << ",\n"
           << "  \"scenario\": \"" << jsonEscape(narrowUtf8(options.scenario)) << "\",\n"
           << "  \"reason\": \"" << jsonEscape(reason) << "\",\n"
           << "  \"source_files\": " << sourceFiles << ",\n"
           << "  \"sent_keys\": " << sent << ",\n"
           << "  \"accepted_requests\": " << requests << ",\n"
           << "  \"presented_requests\": " << presents << ",\n"
           << "  \"decoded_unique\": " << decodedUnique << ",\n"
           << "  \"presented_unique\": " << presentedUnique << ",\n"
           << "  \"decode_failures\": " << decodeFailures << ",\n"
           << "  \"wrong_requests\": " << wrongRequests << ",\n"
           << "  \"p50_ms\": " << static_cast<double>(p50) / 1000.0 << ",\n"
           << "  \"p95_ms\": " << static_cast<double>(p95) / 1000.0 << ",\n"
           << "  \"p99_ms\": " << static_cast<double>(p99) / 1000.0 << ",\n"
           << "  \"max_ms\": " << static_cast<double>(maximum) / 1000.0 << ",\n"
           << "  \"initial_present_ms\": " << static_cast<double>(initialLatency) / 1000.0 << ",\n"
           << "  \"achieved_images_per_second\": " << achievedRate << ",\n"
           << "  \"peak_private_mib\": " << static_cast<double>(peakPrivate) / (1024.0 * 1024.0) << ",\n"
           << "  \"plateau_growth_mib\": " << static_cast<double>(plateauGrowth) / (1024.0 * 1024.0) << ",\n"
           << "  \"max_gpu_cache_mib\": " << static_cast<double>(maxGpuCache) / (1024.0 * 1024.0) << ",\n"
           << "  \"held_initial_delay_ms\": " << options.heldInitialDelayMilliseconds << ",\n"
           << "  \"held_repeat_interval_ms\": " << options.heldRepeatIntervalMilliseconds << ",\n"
           << "  \"held_repeat_posts\": " << heldRepeatPosts << ",\n"
           << "  \"held_initial_gap_ms\": " << static_cast<double>(heldInitialGap) / 1000.0 << ",\n"
           << "  \"held_steady_interval_samples\": " << heldSteadyIntervalCount << ",\n"
           << "  \"held_steady_interval_mean_ms\": " << heldSteadyIntervalMean / 1000.0 << ",\n"
           << "  \"held_steady_interval_p50_ms\": " << static_cast<double>(heldSteadyIntervalP50) / 1000.0 << ",\n"
           << "  \"held_steady_interval_p95_ms\": " << static_cast<double>(heldSteadyIntervalP95) / 1000.0 << ",\n"
           << "  \"held_steady_interval_max_ms\": " << static_cast<double>(heldSteadyIntervalMaximum) / 1000.0 << ",\n"
           << "  \"held_inventory_empty_samples\": " << heldInventoryEmptySamples << ",\n"
           << "  \"source_unchanged\": " << sourceUnchanged << "\n"
           << "}\n";
    return std::move(output).str();
}
} // namespace

int wmain(const int argumentCount, wchar_t** arguments)
{
    std::filesystem::path runDirectory;
    try {
        if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0 &&
            GetLastError() != ERROR_ACCESS_DENIED) {
            throw std::runtime_error("Unable to make the acceptance runner Per-Monitor V2 DPI aware");
        }

        const Options options = parseOptions(argumentCount, arguments);
        if (!std::filesystem::is_regular_file(options.app) ||
            !std::filesystem::is_directory(options.source)) {
            throw std::runtime_error("Application or source directory is unavailable");
        }
        const std::filesystem::path initialPath = options.initial.is_absolute()
            ? options.initial
            : options.source / options.initial;
        if (!std::filesystem::is_regular_file(initialPath)) {
            throw std::runtime_error("Initial image is unavailable");
        }
        const PngDescriptor initialDescriptor = readPngDescriptor(initialPath);
        const std::uint32_t pngWidth = initialDescriptor.width;
        const std::uint32_t pngHeight = initialDescriptor.height;
        if (isWithin(options.resultsRoot, options.source) || isWithin(options.source, options.resultsRoot)) {
            throw std::runtime_error("Result and source paths must not overlap");
        }

        runDirectory = createRunDirectory(options.resultsRoot);
        writeText(runDirectory / L"RUNNING.json", "{\"status\":\"running\"}\n");
        std::wcout << L"Results: " << runDirectory << std::endl;

        std::wcout << L"Collecting source manifest before the run...\n";
        const std::vector<FileRecord> before = collectManifest(options.source, options.verifySha256);
        writeManifest(runDirectory / L"source-before.csv", before);
        if (before.empty()) {
            throw std::runtime_error("No PNG files found");
        }

        const auto initialRecord = std::find_if(before.begin(), before.end(), [&](const FileRecord& file) {
            return lowerCase(file.path.wstring()) == lowerCase(std::filesystem::absolute(initialPath).lexically_normal().wstring());
        });
        if (initialRecord == before.end()) {
            throw std::runtime_error("Initial image is not part of the source catalog");
        }
        std::vector<FileRecord> viewerCatalog;
        std::size_t initialViewerIndex = 0;
        std::filesystem::path viewerCatalogManifest;
        if (options.homogeneousCatalog) {
            auto runBegin = initialRecord;
            while (runBegin != before.begin()) {
                const auto previous = std::prev(runBegin);
                if (!samePngEncoding(readPngDescriptor(previous->path), initialDescriptor)) {
                    break;
                }
                runBegin = previous;
            }
            auto runEnd = std::next(initialRecord);
            while (runEnd != before.end() &&
                samePngEncoding(readPngDescriptor(runEnd->path), initialDescriptor)) {
                ++runEnd;
            }
            viewerCatalog.assign(runBegin, runEnd);
            initialViewerIndex = static_cast<std::size_t>(std::distance(runBegin, initialRecord));
            if (viewerCatalog.size() <= static_cast<std::size_t>(options.heldSamples) + 1) {
                throw std::runtime_error("Homogeneous catalog is too short for the requested held sample");
            }
            viewerCatalogManifest = std::filesystem::absolute(
                runDirectory / L"viewer-catalog.txt").lexically_normal();
            std::ostringstream catalogText;
            for (const FileRecord& file : viewerCatalog) {
                catalogText << narrowUtf8(file.path.wstring()) << '\n';
            }
            writeText(viewerCatalogManifest, std::move(catalogText).str());
        } else {
            viewerCatalog = before;
            initialViewerIndex = static_cast<std::size_t>(std::distance(before.begin(), initialRecord));
        }
        std::size_t currentIndex = initialViewerIndex;

        const std::wstring pipeName = L"PhotoViewerAcceptance_" + std::to_wstring(GetCurrentProcessId());
        UniqueHandle pipe(CreateNamedPipeW(
            (L"\\\\.\\pipe\\" + pipeName).c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            64 * 1024,
            0,
            nullptr));
        if (!pipe.valid()) {
            throw std::runtime_error("Unable to create telemetry pipe");
        }

        std::atomic_bool connectFinished{false};
        std::atomic_bool connectSucceeded{false};
        std::thread connector([&] {
            const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr);
            connectSucceeded = connected != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
            connectFinished = true;
        });

        const std::wstring commandLineText = quoteArgument(options.app) + L" " + quoteArgument(initialPath) +
            L" --decode-workers " + std::to_wstring(options.decodeWorkers) +
            (viewerCatalogManifest.empty()
                ? std::wstring{}
                : L" --catalog-manifest " + quoteArgument(viewerCatalogManifest)) +
            L" --telemetry-pipe " + quoteArgument(pipeName) + L" --no-persistent-state";
        std::vector<wchar_t> commandLine(commandLineText.begin(), commandLineText.end());
        commandLine.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInformation{};
        const std::wstring workingDirectory = options.app.parent_path().wstring();
        if (CreateProcessW(
                options.app.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                workingDirectory.c_str(),
                &startup,
                &processInformation) == 0) {
            CancelSynchronousIo(connector.native_handle());
            connector.join();
            throw std::runtime_error("Unable to launch PhotoViewer");
        }
        UniqueHandle process(processInformation.hProcess);
        UniqueHandle processThread(processInformation.hThread);

        const auto connectDeadline = Clock::now() + std::chrono::seconds(10);
        while (!connectFinished && Clock::now() < connectDeadline && WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connectFinished) {
            CancelSynchronousIo(connector.native_handle());
        }
        connector.join();
        if (!connectSucceeded) {
            TerminateProcess(process.get(), 40);
            WaitForSingleObject(process.get(), 5000);
            throw std::runtime_error("PhotoViewer did not connect to telemetry");
        }

        EventCollector collector(pipe.release(), runDirectory / L"events.tsv");
        ChildProcessGuard processGuard(process.get());
        const HWND window = waitForViewerWindow(processInformation.dwProcessId, process.get());
        if (window == nullptr || !collector.waitForName("ready", std::chrono::seconds(10))) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            throw std::runtime_error("PhotoViewer did not become ready");
        }
        if (!collector.waitForName("direct2d_high_quality_cubic", std::chrono::seconds(5))) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            throw std::runtime_error("Viewer did not activate the Direct2D high-quality cubic renderer");
        }

        const auto initialPresent = collector.waitFor("present", 1, std::chrono::seconds(30));
        if (!initialPresent || initialPresent->index != currentIndex) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            throw std::runtime_error("Initial image was not presented correctly");
        }

        if (!AreDpiAwarenessContextsEqual(
                GetWindowDpiAwarenessContext(window),
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            throw std::runtime_error("Viewer window is not Per-Monitor V2 DPI aware");
        }

        const auto textureDimensions = collector.waitForName("texture_dimensions", std::chrono::seconds(5));
        if (!textureDimensions || textureDimensions->index != currentIndex ||
            textureDimensions->value1 != pngWidth || textureDimensions->value2 != pngHeight) {
            throw std::runtime_error("D3D texture dimensions do not match the source PNG IHDR");
        }

        const auto clientDimensions = collector.waitForName("client_dimensions", std::chrono::seconds(5));
        const auto backBufferDimensions = collector.waitForName("backbuffer_dimensions", std::chrono::seconds(5));
        if (!clientDimensions || !backBufferDimensions || clientDimensions->index < 96 ||
            clientDimensions->value1 == 0 || clientDimensions->value2 == 0 ||
            clientDimensions->value1 != backBufferDimensions->value1 ||
            clientDimensions->value2 != backBufferDimensions->value2) {
            throw std::runtime_error("Swap-chain back buffer does not match the physical client pixel dimensions");
        }

        RECT windowedRectangle{};
        if (GetWindowRect(window, &windowedRectangle) == 0) {
            throw std::runtime_error("Unable to read the initial window rectangle");
        }
        const LONG_PTR windowedStyle = GetWindowLongPtrW(window, GWL_STYLE);
        MONITORINFO monitorInfo{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitorInfo) == 0 ||
            !postKey(window, VK_F11) ||
            !collector.waitFor("fullscreen", 1, std::chrono::seconds(5))) {
            throw std::runtime_error("F11 did not enter fullscreen");
        }

        RECT fullscreenRectangle{};
        const LONG_PTR fullscreenStyle = GetWindowLongPtrW(window, GWL_STYLE);
        if (GetWindowRect(window, &fullscreenRectangle) == 0 ||
            (fullscreenStyle & static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) != 0 ||
            fullscreenRectangle.left != monitorInfo.rcMonitor.left ||
            fullscreenRectangle.top != monitorInfo.rcMonitor.top ||
            fullscreenRectangle.right != monitorInfo.rcMonitor.right ||
            fullscreenRectangle.bottom != monitorInfo.rcMonitor.bottom) {
            throw std::runtime_error("F11 fullscreen is not borderless at the monitor's physical bounds");
        }

        if (!postKey(window, VK_F11) ||
            !collector.waitFor("fullscreen", 0, std::chrono::seconds(5))) {
            throw std::runtime_error("F11 did not leave fullscreen");
        }
        RECT restoredRectangle{};
        if (GetWindowRect(window, &restoredRectangle) == 0 ||
            GetWindowLongPtrW(window, GWL_STYLE) != windowedStyle ||
            restoredRectangle.left != windowedRectangle.left ||
            restoredRectangle.top != windowedRectangle.top ||
            restoredRectangle.right != windowedRectangle.right ||
            restoredRectangle.bottom != windowedRectangle.bottom) {
            throw std::runtime_error("F11 did not restore the original window state");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::uint64_t generation = 1;
        const int firstDirection = currentIndex + 1 < viewerCatalog.size() ? 1 : -1;
        const std::size_t adjacentIndex = static_cast<std::size_t>(static_cast<std::int64_t>(currentIndex) + firstDirection);
        if (!postArrow(window, firstDirection)) {
            throw std::runtime_error("Unable to post the first functional arrow key");
        }
        ++generation;
        const auto adjacentPresent = collector.waitFor("present", generation, std::chrono::seconds(30));
        if (!adjacentPresent || adjacentPresent->index != adjacentIndex) {
            throw std::runtime_error("Forward functional navigation failed");
        }
        currentIndex = adjacentIndex;

        if (!postArrow(window, -firstDirection)) {
            throw std::runtime_error("Unable to post the reverse functional arrow key");
        }
        ++generation;
        const auto returnPresent = collector.waitFor("present", generation, std::chrono::seconds(30));
        if (!returnPresent || returnPresent->index != initialViewerIndex) {
            throw std::runtime_error("Reverse functional navigation failed");
        }
        currentIndex = initialViewerIndex;

        const std::size_t backwardCapacity = currentIndex;
        const std::size_t forwardCapacity = viewerCatalog.size() - currentIndex - 1;
        const int heldDirection = backwardCapacity >= forwardCapacity ? -1 : 1;
        const std::size_t heldCapacity = std::max(backwardCapacity, forwardCapacity);
        const std::size_t heldPresentCount = options.heldSamples;
        if (heldCapacity <= heldPresentCount) {
            throw std::runtime_error("Catalog is too small for held-key navigation");
        }

        const WPARAM heldKey = heldDirection > 0 ? VK_RIGHT : VK_LEFT;
        const std::uint64_t firstHeldGeneration = generation + 1;
        std::vector<std::size_t> heldExpectedIndexes;
        heldExpectedIndexes.reserve(heldPresentCount);
        for (std::size_t offset = 0; offset < heldPresentCount; ++offset) {
            currentIndex = static_cast<std::size_t>(static_cast<std::int64_t>(currentIndex) + heldDirection);
            heldExpectedIndexes.push_back(currentIndex);
        }
        std::vector<std::size_t> heldSampleIndexes;
        heldSampleIndexes.reserve(heldExpectedIndexes.size() + 1);
        heldSampleIndexes.push_back(static_cast<std::size_t>(
            static_cast<std::int64_t>(heldExpectedIndexes.front()) - heldDirection));
        heldSampleIndexes.insert(
            heldSampleIndexes.end(),
            heldExpectedIndexes.begin(),
            heldExpectedIndexes.end());
        writeHeldSampleManifest(
            runDirectory / L"held-sample-manifest.csv",
            viewerCatalog,
            heldSampleIndexes,
            initialDescriptor);
        if (!postHeldKeyDown(window, heldKey, false)) {
            throw std::runtime_error("Unable to begin held arrow-key navigation");
        }
        waitHighResolutionMilliseconds(options.heldInitialDelayMilliseconds);
        if (!postHeldKeyDown(window, heldKey, true)) {
            throw std::runtime_error("Unable to post the first held arrow-key repeat");
        }

        std::atomic_bool repeatPostFailed{false};
        std::atomic_uint64_t heldRepeatPosts{1};
        std::jthread repeatPoster([&](const std::stop_token stopToken) {
            UniqueHandle timer(CreateWaitableTimerExW(
                nullptr,
                nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE));
            LARGE_INTEGER firstDue{};
            firstDue.QuadPart = -static_cast<LONGLONG>(options.heldRepeatIntervalMilliseconds) * 10'000LL;
            if (timer.valid() && SetWaitableTimerEx(
                    timer.get(),
                    &firstDue,
                    static_cast<LONG>(options.heldRepeatIntervalMilliseconds),
                    nullptr,
                    nullptr,
                    nullptr,
                    0) != 0) {
                while (!stopToken.stop_requested()) {
                    if (WaitForSingleObject(timer.get(), INFINITE) != WAIT_OBJECT_0) {
                        repeatPostFailed = true;
                        return;
                    }
                    if (!stopToken.stop_requested()) {
                        if (!postHeldKeyDown(window, heldKey, true)) {
                            repeatPostFailed = true;
                            return;
                        }
                        ++heldRepeatPosts;
                    }
                }
                CancelWaitableTimer(timer.get());
                return;
            }

            auto nextRepeat = Clock::now();
            while (!stopToken.stop_requested()) {
                nextRepeat += std::chrono::milliseconds(options.heldRepeatIntervalMilliseconds);
                std::this_thread::sleep_until(nextRepeat);
                if (!stopToken.stop_requested()) {
                    if (!postHeldKeyDown(window, heldKey, true)) {
                        repeatPostFailed = true;
                        return;
                    }
                    ++heldRepeatPosts;
                }
            }
        });
        std::vector<Event> heldPresents;
        heldPresents.reserve(heldExpectedIndexes.size());
        for (std::size_t offset = 0; offset < heldExpectedIndexes.size(); ++offset) {
            const std::uint64_t expectedGeneration = firstHeldGeneration + offset;
            const auto present = collector.waitFor("present", expectedGeneration, std::chrono::seconds(30));
            if (!present || present->index != heldExpectedIndexes[offset]) {
                throw std::runtime_error("Held-key navigation skipped an intermediate image");
            }
            heldPresents.push_back(*present);
            generation = expectedGeneration;
        }
        repeatPoster.request_stop();
        repeatPoster.join();
        if (repeatPostFailed) {
            throw std::runtime_error("Unable to post a held arrow-key repeat");
        }
        const std::size_t heldRepeatPostCount = static_cast<std::size_t>(heldRepeatPosts.load());
        if (heldPresents.size() > heldRepeatPostCount + 1) {
            throw std::runtime_error("Viewer presented more held steps than posted key-repeat inputs");
        }
        const std::size_t heldInventoryEmptySamples = writeHeldIntervals(
            runDirectory / L"held-switch-intervals.csv",
            heldPresents,
            collector.snapshot());

        const std::uint64_t heldInitialGap = heldPresents.size() >= 2
            ? heldPresents[1].timestampMicroseconds - heldPresents[0].timestampMicroseconds
            : 0;
        std::vector<std::uint64_t> heldSteadyIntervals;
        if (heldPresents.size() > 2) {
            heldSteadyIntervals.reserve(heldPresents.size() - 2);
        }
        double heldSteadyIntervalTotal = 0.0;
        for (std::size_t index = 2; index < heldPresents.size(); ++index) {
            const std::uint64_t interval =
                heldPresents[index].timestampMicroseconds - heldPresents[index - 1].timestampMicroseconds;
            heldSteadyIntervals.push_back(interval);
            heldSteadyIntervalTotal += static_cast<double>(interval);
        }
        const double heldSteadyIntervalMean = heldSteadyIntervals.empty()
            ? 0.0
            : heldSteadyIntervalTotal / static_cast<double>(heldSteadyIntervals.size());
        const std::uint64_t heldSteadyIntervalP50 = percentile(heldSteadyIntervals, 0.50);
        const std::uint64_t heldSteadyIntervalP95 = percentile(heldSteadyIntervals, 0.95);
        const std::uint64_t heldSteadyIntervalMaximum = heldSteadyIntervals.empty()
            ? 0
            : *std::max_element(heldSteadyIntervals.begin(), heldSteadyIntervals.end());

        if (!postHeldKeyUp(window, heldKey)) {
            throw std::runtime_error("Unable to release the held arrow key");
        }

        const auto stopped = collector.waitForName("navigation_stopped", std::chrono::seconds(5));
        if (!stopped || stopped->value1 <= generation) {
            throw std::runtime_error("Arrow-key release did not stop at the last displayed image");
        }
        const std::vector<Event> eventsAtStop = collector.snapshot();
        const auto lastDisplayed = std::find_if(
            eventsAtStop.rbegin(),
            eventsAtStop.rend(),
            [&](const Event& event) {
                return event.name == "present" &&
                    event.timestampMicroseconds <= stopped->timestampMicroseconds;
            });
        if (lastDisplayed == eventsAtStop.rend() || lastDisplayed->index != stopped->index) {
            throw std::runtime_error("Arrow-key release did not retain the image visible at release time");
        }
        currentIndex = stopped->index;
        generation = stopped->value1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const std::vector<Event> afterReleaseEvents = collector.snapshot();
        const auto unexpectedPresent = std::find_if(
            afterReleaseEvents.begin(),
            afterReleaseEvents.end(),
            [&](const Event& event) {
                return event.name == "present" && event.timestampMicroseconds > stopped->timestampMicroseconds;
            });
        if (unexpectedPresent != afterReleaseEvents.end()) {
            throw std::runtime_error("Viewer continued presenting images after the arrow key was released");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        const std::size_t transitionCount = static_cast<std::size_t>(options.durationSeconds) * 1000ULL /
            options.intervalMilliseconds;
        std::vector<std::size_t> expectedIndexes;
        expectedIndexes.reserve(transitionCount);
        std::vector<std::uint64_t> memorySamples;
        int direction = currentIndex + 1 < viewerCatalog.size() ? 1 : -1;
        const std::uint64_t firstPerformanceGeneration = generation + 1;
        const auto scheduleStart = Clock::now();
        for (std::size_t transition = 0; transition < transitionCount; ++transition) {
            if ((direction > 0 && currentIndex + 1 >= viewerCatalog.size()) || (direction < 0 && currentIndex == 0)) {
                direction = -direction;
            }
            currentIndex = static_cast<std::size_t>(static_cast<std::int64_t>(currentIndex) + direction);
            expectedIndexes.push_back(currentIndex);
            if (!postArrow(window, direction)) {
                throw std::runtime_error("Unable to post a performance arrow key");
            }
            ++generation;
            if (transition % 10 == 0) {
                memorySamples.push_back(privateBytes(process.get()));
            }
            std::this_thread::sleep_until(
                scheduleStart + std::chrono::milliseconds(
                    static_cast<std::int64_t>((transition + 1) * options.intervalMilliseconds)));
            if ((transition + 1) % 100 == 0 || transition + 1 == transitionCount) {
                std::wcout << L"Navigation " << transition + 1 << L"/" << transitionCount << L'\n';
            }
        }

        const auto lastPresent = collector.waitFor("present", generation, std::chrono::seconds(30));
        if (!lastPresent || lastPresent->index != currentIndex) {
            throw std::runtime_error("The final requested image was not presented");
        }

        PostMessageW(window, WM_CLOSE, 0, 0);
        const DWORD processWait = WaitForSingleObject(process.get(), 30000);
        bool normalExit = processWait == WAIT_OBJECT_0;
        if (!normalExit) {
            TerminateProcess(process.get(), 40);
            WaitForSingleObject(process.get(), 5000);
        }
        processGuard.disarm();
        collector.join();

        DWORD exitCode = 0;
        GetExitCodeProcess(process.get(), &exitCode);
        const std::vector<Event> events = collector.snapshot();

        std::map<std::uint64_t, Event> requests;
        std::map<std::uint64_t, Event> presents;
        std::set<std::size_t> decodedIndexes;
        std::set<std::size_t> presentedIndexes;
        std::size_t decodeFailures = 0;
        std::uint64_t maxGpuCache = 0;
        for (const Event& event : events) {
            if (event.name == "present") {
                presentedIndexes.insert(event.index);
            }
            if (event.name == "request" && event.value1 >= firstPerformanceGeneration && event.value1 <= generation) {
                requests[event.value1] = event;
            } else if (event.name == "present" && event.value2 >= firstPerformanceGeneration && event.value2 <= generation) {
                presents[event.value2] = event;
            } else if (event.name == "decode_done") {
                decodedIndexes.insert(event.index);
            } else if (event.name == "decode_failed") {
                ++decodeFailures;
            } else if (event.name == "upload") {
                maxGpuCache = std::max(maxGpuCache, event.value2);
            }
        }

        std::size_t wrongRequests = 0;
        std::vector<std::uint64_t> latencies;
        latencies.reserve(presents.size());
        for (std::size_t offset = 0; offset < expectedIndexes.size(); ++offset) {
            const std::uint64_t expectedGeneration = firstPerformanceGeneration + offset;
            const auto request = requests.find(expectedGeneration);
            if (request == requests.end() || request->second.index != expectedIndexes[offset]) {
                ++wrongRequests;
            }
            const auto present = presents.find(expectedGeneration);
            if (present != presents.end()) {
                latencies.push_back(present->second.value1);
            }
        }

        std::wcout << L"Collecting source manifest after the run...\n";
        const std::vector<FileRecord> after = collectManifest(options.source, options.verifySha256);
        writeManifest(runDirectory / L"source-after.csv", after);
        const bool sourceUnchanged = manifestsEqual(before, after, options.verifySha256);
        const std::uint64_t peakPrivate = memorySamples.empty()
            ? 0
            : *std::max_element(memorySamples.begin(), memorySamples.end());
        writeMemorySamples(runDirectory / L"memory-samples.csv", memorySamples);
        std::uint64_t plateauGrowth = 0;
        if (memorySamples.size() >= 9) {
            const std::size_t third = memorySamples.size() / 3;
            const std::vector<std::uint64_t> middleThird(
                memorySamples.begin() + static_cast<std::ptrdiff_t>(third),
                memorySamples.begin() + static_cast<std::ptrdiff_t>(third * 2));
            const std::vector<std::uint64_t> finalThird(
                memorySamples.begin() + static_cast<std::ptrdiff_t>(third * 2),
                memorySamples.end());
            const std::uint64_t middleMedian = percentile(middleThird, 0.50);
            const std::uint64_t finalMedian = percentile(finalThird, 0.50);
            plateauGrowth = finalMedian > middleMedian ? finalMedian - middleMedian : 0;
        }
        const std::uint64_t p50 = percentile(latencies, 0.50);
        const std::uint64_t p95 = percentile(latencies, 0.95);
        const std::uint64_t p99 = percentile(latencies, 0.99);
        const std::uint64_t maximum = latencies.empty() ? 0 : *std::max_element(latencies.begin(), latencies.end());
        const double scheduledSeconds = static_cast<double>(transitionCount * options.intervalMilliseconds) / 1000.0;
        const double achievedRate = scheduledSeconds > 0.0
            ? static_cast<double>(presents.size()) / scheduledSeconds
            : 0.0;

        const bool finalScenario = options.scenario == L"final";
        bool passed = normalExit && exitCode == 0 && sourceUnchanged && decodeFailures == 0 &&
            wrongRequests == 0 && requests.size() == transitionCount && presents.size() == transitionCount;
        std::string reason = "all required checks passed";
        if (options.requireInventoryExhaustion && heldInventoryEmptySamples == 0) {
            passed = false;
            reason = "homogeneous inventory-exhaustion scenario did not exhaust decoded inventory";
        }
        if (finalScenario) {
            passed = passed &&
                p95 <= 100'000ULL &&
                p99 <= 150'000ULL &&
                maximum <= 500'000ULL &&
                heldInitialGap <=
                    static_cast<std::uint64_t>(options.heldInitialDelayMilliseconds) * 1'000ULL + 250'000ULL &&
                heldSteadyIntervalP95 <= 100'000ULL &&
                heldSteadyIntervalMaximum <= 250'000ULL &&
                achievedRate >= 9.9 &&
                decodedIndexes.size() == viewerCatalog.size() &&
                presentedIndexes.size() == viewerCatalog.size() &&
                initialPresent->value1 <= 3'000'000ULL &&
                peakPrivate <= 5ULL * 1024ULL * 1024ULL * 1024ULL &&
                plateauGrowth <= 256ULL * 1024ULL * 1024ULL &&
                maxGpuCache <= 800ULL * 1024ULL * 1024ULL;
        } else {
            passed = passed && maximum <= 1'000'000ULL;
        }
        if (!passed && reason == "all required checks passed") {
            reason = "one or more functional, performance, integrity, or shutdown gates failed";
        }

        const std::string summary = buildSummary(
            passed,
            options,
            before.size(),
            transitionCount,
            requests.size(),
            presents.size(),
            decodedIndexes.size(),
            presentedIndexes.size(),
            decodeFailures,
            wrongRequests,
            p50,
            p95,
            p99,
            maximum,
            initialPresent->value1,
            achievedRate,
            peakPrivate,
            plateauGrowth,
            maxGpuCache,
            heldRepeatPostCount,
            heldInitialGap,
            heldSteadyIntervals.size(),
            heldSteadyIntervalMean,
            heldSteadyIntervalP50,
            heldSteadyIntervalP95,
            heldSteadyIntervalMaximum,
            heldInventoryEmptySamples,
            sourceUnchanged,
            reason);
        writeText(runDirectory / L"summary.json", summary);
        const std::filesystem::path temporaryMarker = runDirectory / L"completion.tmp";
        writeText(temporaryMarker, summary);
        const std::filesystem::path finalMarker = runDirectory / (passed ? L"PASS.json" : L"FAIL.json");
        if (MoveFileExW(temporaryMarker.c_str(), finalMarker.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
            throw std::runtime_error("Unable to atomically publish the completion marker");
        }
        std::error_code removeError;
        std::filesystem::remove(runDirectory / L"RUNNING.json", removeError);

        std::cout << summary;
        return passed ? 0 : 20;
    } catch (const std::exception& error) {
        const std::string message = std::string("acceptance incomplete: ") + error.what();
        std::cerr << message << '\n';
        if (!runDirectory.empty()) {
            try {
                const std::string failure = "{\"pass\":false,\"reason\":\"" + jsonEscape(message) + "\"}\n";
                writeText(runDirectory / L"FAIL.json", failure);
            } catch (const std::exception&) {
            }
        }
        return 50;
    }
}
