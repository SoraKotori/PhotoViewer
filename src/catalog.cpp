#include "catalog.h"

#include "common.h"

#include <array>
#include <cstring>
#include <fstream>

namespace pv {
namespace {

bool IsPngExtension(const std::filesystem::path& path) {
    const std::wstring extension = path.extension().wstring();
    return CompareStringOrdinal(extension.c_str(), -1, L".png", -1, TRUE) == CSTR_EQUAL;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring a = left.lexically_normal().wstring();
    const std::wstring b = right.lexically_normal().wstring();
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

CatalogItem Describe(const std::filesystem::path& path,
                     const std::uintmax_t size) {
    CatalogItem item;
    item.path = path;
    if (size <= std::numeric_limits<std::uint64_t>::max()) {
        item.file_bytes = static_cast<std::uint64_t>(size);
        item.file_size_known = true;
    }
    return item;
}

void FinalizeCatalog(Catalog& catalog,
                     const std::filesystem::path& initial_image) {
    std::sort(catalog.items.begin(), catalog.items.end(), [](const CatalogItem& left,
                                                             const CatalogItem& right) {
        const std::wstring& a = left.path.native();
        const std::wstring& b = right.path.native();
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    const auto found = std::find_if(catalog.items.begin(), catalog.items.end(),
                                    [&](const CatalogItem& item) {
                                        return SamePath(item.path, initial_image);
                                    });
    if (found == catalog.items.end()) throw std::runtime_error("initial PNG was not catalogued");
    catalog.initial_index = static_cast<std::size_t>(
        std::distance(catalog.items.begin(), found));
}

struct NativeIoStatusBlock {
    union {
        LONG status;
        void* pointer;
    } value{};
    ULONG_PTR information = 0;
};

struct NativeFileDirectoryInformation {
    ULONG next_entry_offset;
    ULONG file_index;
    LARGE_INTEGER creation_time;
    LARGE_INTEGER last_access_time;
    LARGE_INTEGER last_write_time;
    LARGE_INTEGER change_time;
    LARGE_INTEGER end_of_file;
    LARGE_INTEGER allocation_size;
    ULONG file_attributes;
    ULONG file_name_length;
    WCHAR file_name[1];
};
static_assert(offsetof(NativeFileDirectoryInformation, file_name) == 64);

using NtQueryDirectoryFileExFn = LONG(NTAPI*)(
    HANDLE, HANDLE, void*, void*, NativeIoStatusBlock*, void*, ULONG, int,
    ULONG, void*);

constexpr LONG kStatusPending = 0x00000103L;
constexpr LONG kStatusBufferOverflow = static_cast<LONG>(0x80000005UL);
constexpr LONG kStatusNoMoreFiles = static_cast<LONG>(0x80000006UL);
constexpr ULONG kRestartScan = 0x01;
constexpr int kFileDirectoryInformation = 1;

}  // namespace

struct AsyncCatalog::Impl {
    explicit Impl(const std::filesystem::path& initial_image,
                  const HANDLE completion_port)
        : absolute(std::filesystem::absolute(initial_image)),
          directory_path(absolute.parent_path()) {
        if (!IsPngExtension(absolute)) {
            throw std::invalid_argument("initial image is not a PNG file");
        }
        HMODULE module = GetModuleHandleW(L"ntdll.dll");
        if (!module) ThrowLastError("GetModuleHandleW(ntdll.dll)");
        const FARPROC address = GetProcAddress(module, "NtQueryDirectoryFileEx");
        if (!address) ThrowLastError("GetProcAddress(NtQueryDirectoryFileEx)");
        static_assert(sizeof(query) == sizeof(address));
        std::memcpy(&query, &address, sizeof(query));

        directory = CreateFileW(
            directory_path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (directory == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(directory)");
        if (CreateIoCompletionPort(directory, completion_port,
                                   kCatalogIoCompletionKey, 0) !=
            completion_port) {
            const DWORD error = GetLastError();
            CloseHandle(directory);
            directory = INVALID_HANDLE_VALUE;
            SetLastError(error);
            ThrowLastError("CreateIoCompletionPort(directory)");
        }
        try {
            if (!Submit()) {
                FinalizeCatalog(catalog, absolute);
                completed = true;
            }
        } catch (...) {
            CloseHandle(directory);
            directory = INVALID_HANDLE_VALUE;
            throw;
        }
    }

    ~Impl() {
        if (in_flight) {
            CancelIoEx(directory, nullptr);
            WaitForSingleObject(directory, INFINITE);
        }
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    }

    [[nodiscard]] bool Submit() {
        io_status = {};
        in_flight = true;
        const LONG status = query(
            directory, nullptr, nullptr, &io_status, &io_status,
            buffer.data(), static_cast<ULONG>(buffer.size()),
            kFileDirectoryInformation, restart_scan ? kRestartScan : 0,
            nullptr);
        restart_scan = false;
        if (status == kStatusNoMoreFiles) {
            in_flight = false;
            return false;
        }
        if (status < 0 && status != kStatusPending) {
            in_flight = false;
            throw std::runtime_error("NtQueryDirectoryFileEx failed (NTSTATUS " +
                                     std::to_string(static_cast<unsigned long>(status)) +
                                     ")");
        }
        return true;
    }

    void Parse(const std::size_t bytes) {
        std::size_t offset = 0;
        while (offset + offsetof(NativeFileDirectoryInformation, file_name) <= bytes) {
            const auto* entry = reinterpret_cast<const NativeFileDirectoryInformation*>(
                buffer.data() + offset);
            const std::size_t name_offset =
                offset + offsetof(NativeFileDirectoryInformation, file_name);
            if ((entry->file_name_length % sizeof(WCHAR)) != 0 ||
                entry->file_name_length > bytes - name_offset) {
                throw std::runtime_error("invalid asynchronous directory entry");
            }
            if ((entry->file_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                entry->end_of_file.QuadPart >= 0) {
                const std::wstring name(entry->file_name,
                                        entry->file_name_length / sizeof(WCHAR));
                const std::filesystem::path path = directory_path / name;
                if (IsPngExtension(path)) {
                    catalog.items.push_back(Describe(
                        path, static_cast<std::uint64_t>(entry->end_of_file.QuadPart)));
                }
            }
            if (entry->next_entry_offset == 0) break;
            if (entry->next_entry_offset > bytes - offset) {
                throw std::runtime_error("invalid asynchronous directory offset");
            }
            offset += entry->next_entry_offset;
        }
    }

    [[nodiscard]] bool Advance() {
        if (completed) return true;
        if (!in_flight) throw std::logic_error("catalog query is not active");
        in_flight = false;
        const LONG result = io_status.value.status;
        const std::size_t bytes = io_status.information;
        if (result == 0 || result == kStatusBufferOverflow) {
            Parse(bytes);
            if (Submit()) return false;
        } else if (result != kStatusNoMoreFiles) {
            throw std::runtime_error("asynchronous directory query failed (NTSTATUS " +
                                     std::to_string(static_cast<unsigned long>(result)) +
                                     ")");
        }
        FinalizeCatalog(catalog, absolute);
        completed = true;
        return true;
    }

    std::filesystem::path absolute;
    std::filesystem::path directory_path;
    HANDLE directory = INVALID_HANDLE_VALUE;
    NtQueryDirectoryFileExFn query = nullptr;
    NativeIoStatusBlock io_status{};
    alignas(8) std::array<std::byte, 64 * 1024> buffer{};
    Catalog catalog;
    bool restart_scan = true;
    bool in_flight = false;
    bool completed = false;
};

AsyncCatalog::AsyncCatalog(const std::filesystem::path& initial_image,
                           const HANDLE completion_port)
    : impl_(std::make_unique<Impl>(initial_image, completion_port)) {}

AsyncCatalog::~AsyncCatalog() = default;

bool AsyncCatalog::Advance() { return impl_->Advance(); }

Catalog AsyncCatalog::TakeCatalog() {
    if (!impl_->completed) throw std::logic_error("catalog is incomplete");
    return std::move(impl_->catalog);
}

Catalog BuildInitialCatalog(const std::filesystem::path& initial_image) {
    if (initial_image.empty()) throw std::invalid_argument("an initial PNG path is required");
    const std::filesystem::path absolute = std::filesystem::absolute(initial_image);
    if (!IsPngExtension(absolute)) throw std::invalid_argument("initial image is not a PNG file");
    Catalog catalog;
    CatalogItem item;
    item.path = absolute;
    catalog.items.push_back(std::move(item));
    return catalog;
}

Catalog BuildCatalogFromList(const std::filesystem::path& list_file,
                             const std::filesystem::path& initial_image) {
    std::ifstream input(list_file, std::ios::binary);
    if (!input) throw std::invalid_argument("validation file list does not exist");

    Catalog catalog;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::u8string utf8(reinterpret_cast<const char8_t*>(line.data()),
                                 line.size());
        const std::filesystem::path path(utf8);
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || !IsPngExtension(path)) {
            throw std::invalid_argument("validation file list contains an invalid PNG path");
        }
        catalog.items.push_back(Describe(path, size));
    }
    if (catalog.items.empty()) throw std::invalid_argument("validation file list is empty");
    const std::filesystem::path absolute = std::filesystem::absolute(initial_image);
    const auto found = std::find_if(catalog.items.begin(), catalog.items.end(),
                                    [&](const CatalogItem& item) {
                                        return SamePath(item.path, absolute);
                                    });
    if (found == catalog.items.end()) {
        throw std::invalid_argument("initial PNG is absent from validation file list");
    }
    catalog.initial_index = static_cast<std::size_t>(
        std::distance(catalog.items.begin(), found));
    return catalog;
}

}  // namespace pv
