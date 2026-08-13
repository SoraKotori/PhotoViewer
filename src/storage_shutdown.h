#pragma once

#include "iocp_transport.h"

#include <cstddef>
#include <cstdint>

namespace pv {

enum class StorageShutdownResult {
    Drained,
    AbandonBacking,
};

// Drains only operations already submitted to the kernel. Request lookup and
// time are injected so every shutdown branch can be tested synchronously.
template <typename Transport, typename RequestAt, typename Clock>
StorageShutdownResult DrainStorageForShutdown(
    const std::size_t request_count, RequestAt&& request_at,
    Transport& transport, Clock&& now_ms,
    const std::uint64_t timeout_ms) noexcept {
    std::size_t pending_operations = 0;
    for (std::size_t index = 0; index < request_count; ++index) {
        IoRequest* const request = request_at(index);
        if (!request) continue;
        try {
            (void)transport.RequestCancellation(*request);
        } catch (...) {
            return StorageShutdownResult::AbandonBacking;
        }
        if (request->header_submitted && !request->header_completed) {
            ++pending_operations;
        }
        if (request->content_submitted && !request->content_completed) {
            ++pending_operations;
        }
    }

    const std::uint64_t deadline = now_ms() + timeout_ms;
    while (pending_operations != 0) {
        const std::uint64_t now = now_ms();
        if (now >= deadline) return StorageShutdownResult::AbandonBacking;
        const auto completion = transport.WaitForShutdownCompletion(
            static_cast<DWORD>(deadline - now));
        if (!completion || !completion->request) {
            return StorageShutdownResult::AbandonBacking;
        }

        bool known_request = false;
        for (std::size_t index = 0; index < request_count; ++index) {
            if (request_at(index) == completion->request) {
                known_request = true;
                break;
            }
        }
        if (!known_request) return StorageShutdownResult::AbandonBacking;

        IoRequest& request = *completion->request;
        if (completion->overlapped == &request.header_overlapped &&
            request.header_submitted && !request.header_completed) {
            request.header_completed = true;
        } else if (completion->overlapped == &request.content_overlapped &&
                   request.content_submitted &&
                   !request.content_completed) {
            request.content_completed = true;
        } else {
            return StorageShutdownResult::AbandonBacking;
        }
        --pending_operations;
    }
    return StorageShutdownResult::Drained;
}

}  // namespace pv
