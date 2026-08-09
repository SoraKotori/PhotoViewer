#pragma once

#include "common.h"
#include "png.h"

#include <filesystem>
#include <vector>

namespace pv {

struct CatalogItem {
    std::filesystem::path path;
    PngInfo png;
    std::uint64_t file_bytes = 0;
    bool file_size_known = false;
    bool header_valid = false;
};

struct Catalog {
    std::vector<CatalogItem> items;
    std::size_t initial_index = 0;
};

Catalog BuildInitialCatalog(const std::filesystem::path& initial_image);
Catalog BuildCatalogFromList(const std::filesystem::path& list_file,
                             const std::filesystem::path& initial_image);

class AsyncCatalog {
public:
    explicit AsyncCatalog(const std::filesystem::path& initial_image);
    ~AsyncCatalog();

    AsyncCatalog(const AsyncCatalog&) = delete;
    AsyncCatalog& operator=(const AsyncCatalog&) = delete;

    [[nodiscard]] bool Advance();
    [[nodiscard]] HANDLE CompletionEvent() const noexcept;
    [[nodiscard]] Catalog TakeCatalog();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pv
