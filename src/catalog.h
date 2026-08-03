#pragma once

#include "png.h"

#include <filesystem>
#include <vector>

namespace pv {

struct CatalogItem {
    std::filesystem::path path;
    PngInfo png;
    std::uint64_t file_bytes = 0;
    bool header_valid = false;
};

struct Catalog {
    std::vector<CatalogItem> items;
    std::size_t initial_index = 0;
};

Catalog BuildCatalog(const std::filesystem::path& initial_image);
Catalog BuildCatalogFromList(const std::filesystem::path& list_file,
                             const std::filesystem::path& initial_image);

}  // namespace pv
