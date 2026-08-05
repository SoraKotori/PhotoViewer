#include "processor_topology.h"

#include <windows.h>

#include <cstddef>
#include <vector>

namespace pv {
namespace {

constexpr std::size_t kFallbackPhysicalCoreCount = 1;

}  // namespace

std::size_t DetectPhysicalCoreCount() noexcept {
    DWORD required_bytes = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr,
                                         &required_bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || required_bytes == 0) {
        return kFallbackPhysicalCoreCount;
    }

    std::vector<std::max_align_t> storage;
    try {
        const std::size_t element_count =
            (required_bytes + sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t);
        storage.resize(element_count);
    } catch (...) {
        return kFallbackPhysicalCoreCount;
    }
    auto* const buffer = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
        storage.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, buffer,
                                          &required_bytes)) {
        return kFallbackPhysicalCoreCount;
    }

    const auto* const bytes = reinterpret_cast<const std::byte*>(storage.data());
    std::size_t core_count = 0;
    for (std::size_t offset = 0; offset < required_bytes;) {
        constexpr std::size_t header_bytes =
            sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD);
        if (required_bytes - offset < header_bytes) {
            return kFallbackPhysicalCoreCount;
        }

        const auto* const information =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                bytes + offset);
        if (information->Size < header_bytes ||
            information->Size > required_bytes - offset) {
            return kFallbackPhysicalCoreCount;
        }
        if (information->Relationship == RelationProcessorCore) ++core_count;
        offset += information->Size;
    }
    return core_count == 0 ? kFallbackPhysicalCoreCount : core_count;
}

std::size_t DefaultWorkerCount() noexcept {
    return DefaultWorkerCountForPhysicalCores(DetectPhysicalCoreCount());
}

}  // namespace pv
