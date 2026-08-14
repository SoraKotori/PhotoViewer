#pragma once

#include "pipeline_types.h"
#include "win32_handle.h"

namespace pv {

struct IoRequest {
    void Reset() noexcept {
        header_overlapped = {};
        content_overlapped = {};
        file.Reset();
        index = 0;
        generation = 0;
        compressed_slot = kInvalidSlot;
        destination = nullptr;
        byte_count = 0;
        transfer_count = 0;
        prefix_bytes = 0;
        split_header = false;
        header_submitted = false;
        header_completed = false;
        content_submitted = false;
        content_completed = false;
        header_result = ERROR_IO_PENDING;
        header_transferred = 0;
        result = ERROR_IO_PENDING;
        transferred = 0;
    }

    OVERLAPPED header_overlapped{};
    OVERLAPPED content_overlapped{};
    UniqueHandle file;
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    std::byte* destination = nullptr;
    DWORD byte_count = 0;
    DWORD transfer_count = 0;
    DWORD prefix_bytes = 0;
    bool split_header = false;
    bool header_submitted = false;
    bool header_completed = false;
    bool content_submitted = false;
    bool content_completed = false;
    DWORD header_result = ERROR_IO_PENDING;
    ULONG_PTR header_transferred = 0;
    DWORD result = ERROR_IO_PENDING;
    ULONG_PTR transferred = 0;
};

}  // namespace pv
