#include "catalog.h"

#include "common.h"

#include <array>
#include <fstream>

namespace pv {
namespace {

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

bool IsPngExtension(const std::filesystem::path& path) {
    const std::wstring extension = path.extension().wstring();
    return CompareStringOrdinal(extension.c_str(), -1, L".png", -1, TRUE) == CSTR_EQUAL;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring a = left.lexically_normal().wstring();
    const std::wstring b = right.lexically_normal().wstring();
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

CatalogItem Probe(const std::filesystem::path& path) {
    CatalogItem item;
    item.path = path;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return item;
    const std::unique_ptr<void, HandleCloser> close(file);

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) return item;
    item.file_bytes = static_cast<std::uint64_t>(size.QuadPart);
    std::array<std::byte, 33> header{};
    DWORD read = 0;
    if (!ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr)) return item;
    const auto parsed = ParsePngHeader(std::span(header.data(), read));
    if (!parsed) return item;
    item.png = *parsed;
    item.header_valid = true;
    return item;
}

}  // namespace

Catalog BuildCatalog(const std::filesystem::path& initial_image) {
    if (initial_image.empty()) throw std::invalid_argument("an initial PNG path is required");
    const std::filesystem::path absolute = std::filesystem::absolute(initial_image);
    if (!std::filesystem::is_regular_file(absolute)) {
        throw std::invalid_argument("initial image does not exist");
    }
    if (!IsPngExtension(absolute)) throw std::invalid_argument("initial image is not a PNG file");

    Catalog catalog;
    for (const auto& entry : std::filesystem::directory_iterator(absolute.parent_path())) {
        if (entry.is_regular_file() && IsPngExtension(entry.path())) {
            catalog.items.push_back(Probe(entry.path()));
        }
    }
    std::sort(catalog.items.begin(), catalog.items.end(), [](const CatalogItem& left,
                                                             const CatalogItem& right) {
        const std::wstring a = left.path.filename().wstring();
        const std::wstring b = right.path.filename().wstring();
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    const auto found = std::find_if(catalog.items.begin(), catalog.items.end(),
                                    [&](const CatalogItem& item) {
                                        return SamePath(item.path, absolute);
                                    });
    if (found == catalog.items.end()) throw std::runtime_error("initial PNG was not catalogued");
    catalog.initial_index = static_cast<std::size_t>(std::distance(catalog.items.begin(), found));
    return catalog;
}

Catalog BuildCatalogFromList(const std::filesystem::path& list_file) {
    std::ifstream input(list_file, std::ios::binary);
    if (!input) throw std::invalid_argument("validation file list does not exist");

    Catalog catalog;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::u8string utf8(reinterpret_cast<const char8_t*>(line.data()), line.size());
        const std::filesystem::path path(utf8);
        if (!std::filesystem::is_regular_file(path) || !IsPngExtension(path)) {
            throw std::invalid_argument("validation file list contains an invalid PNG path");
        }
        CatalogItem item = Probe(path);
        if (!item.header_valid) {
            throw std::invalid_argument("validation file list contains an invalid PNG file");
        }
        catalog.items.push_back(std::move(item));
    }
    if (catalog.items.empty()) throw std::invalid_argument("validation file list is empty");
    catalog.initial_index = 0;
    return catalog;
}

}  // namespace pv
